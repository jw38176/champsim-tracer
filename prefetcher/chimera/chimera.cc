#include "cache.h"
#include <bits/stdc++.h>

using namespace std;

/* Fill level constants compatible with modern ChampSim */
constexpr int FILL_L1 = 1;
constexpr int FILL_L2 = 2;

// Constant definitions
const int TIME_MASK = 0xfff;
const int IP_TAG_BITS = 8;        // Number of PC-tag bits
const int REGION_TAG_BITS = 8;    // Number of region-tag bits
const int LINE_ADDR_BITS = 12;    // Number of line address bits
const int OFFSET_RANGE = 7;       // Offset range (-63 ~ +63)
const int MAX_OFFSET = 63;        // Maximum offset
const int OFFSET_COUNT = 2*MAX_OFFSET + 1; // Total number of offsets
const int CONFIDENCE_BITS = 4;    // Number of confidence bits
const int REFRESH_THRESHOLD = 512; // Refresh threshold for Epoch-based prefetcher

// Statistics
uint64_t pp_prefetches = 0;      // PC-based prefetch count
uint64_t sp_prefetches = 0;      // Space-based prefetch count
uint64_t ep_prefetches = 0;      // Epoch-based prefetch count
uint64_t prefetch_degree_count[9] = {0}; // Count of different prefetch degrees
uint64_t latency = 0;
uint64_t prefetch_issued = 0;
uint64_t cross_page_prefetch = 0;
ofstream outfile("HOP_log.txt");

// Hash functions
uint32_t hash_ip(uint64_t ip) {
    return (ip >> 2) & ((1 << IP_TAG_BITS) - 1);
}

uint32_t hash_region(uint64_t addr) {
    return (addr >> (LOG2_BLOCK_SIZE + 6)) & ((1 << REGION_TAG_BITS) - 1);
}

// History table entry
struct HistoryEntry {
    uint32_t ip_tag;          // PC-tag
    uint32_t region_tag;      // region-tag
    uint64_t line_addr;       // line address
    
    HistoryEntry() : ip_tag(0), region_tag(0), line_addr(0) {}
    HistoryEntry(uint32_t ip, uint32_t region, uint64_t addr) : 
        ip_tag(ip), region_tag(region), line_addr(addr) {}
};

// PC-based prefetcher table entry
struct PCEntry {
    uint32_t ip_tag;
    std::vector<uint32_t> offset_confidence;  // Confidence for each offset
    int best_offset1, best_offset2;           // Best offsets
    uint32_t best_confidence1, best_confidence2; // Best confidences
    
    PCEntry() : ip_tag(0), offset_confidence(OFFSET_COUNT, 0), 
               best_offset1(-1), best_offset2(-1), 
               best_confidence1(0), best_confidence2(0) {}
               
    PCEntry(uint32_t ip) : ip_tag(ip), offset_confidence(OFFSET_COUNT, 0), 
                         best_offset1(-1), best_offset2(-1), 
                         best_confidence1(0), best_confidence2(0) {}
};

// Space-based prefetcher table entry
struct SpaceEntry {
    uint32_t region_tag;
    std::vector<uint32_t> offset_confidence;  // Confidence for each offset
    int best_offset1, best_offset2;           // Best offsets
    uint32_t best_confidence1, best_confidence2; // Best confidences
    
    SpaceEntry() : region_tag(0), offset_confidence(OFFSET_COUNT, 0), 
                 best_offset1(-1), best_offset2(-1), 
                 best_confidence1(0), best_confidence2(0) {}
                 
    SpaceEntry(uint32_t region) : region_tag(region), offset_confidence(OFFSET_COUNT, 0), 
                                best_offset1(-1), best_offset2(-1), 
                                best_confidence1(0), best_confidence2(0) {}
};

// Epoch-based prefetcher
struct EpochEntry {
    std::vector<uint32_t> offset_confidence;  // Confidence for each offset
    int best_offset1, best_offset2, best_offset3; // Three best offsets
    uint32_t refresh_count;                    // Refresh counter
    
    EpochEntry() : offset_confidence(OFFSET_COUNT, 0), 
                 best_offset1(-1), best_offset2(-1), best_offset3(-1),
                 refresh_count(0) {}
                 
    void update_best_offsets() {
        // Find the three best offsets
        best_offset1 = best_offset2 = best_offset3 = -1;
        uint32_t max1 = 0, max2 = 0, max3 = 0;
        
        for (int i = 0; i < OFFSET_COUNT; i++) {
            if (offset_confidence[i] > max1) {
                max3 = max2;
                max2 = max1;
                max1 = offset_confidence[i];
                best_offset3 = best_offset2;
                best_offset2 = best_offset1;
                best_offset1 = i - MAX_OFFSET;
            } else if (offset_confidence[i] > max2) {
                max3 = max2;
                max2 = offset_confidence[i];
                best_offset3 = best_offset2;
                best_offset2 = i - MAX_OFFSET;
            } else if (offset_confidence[i] > max3) {
                max3 = offset_confidence[i];
                best_offset3 = i - MAX_OFFSET;
            }
        }
    }
    
    void refresh() {
        // Refresh all confidence values
        std::fill(offset_confidence.begin(), offset_confidence.end(), 0);
        best_offset1 = best_offset2 = best_offset3 = -1;
        refresh_count = 0;
    }
};

// Prefetch allocator entry
struct PrefetchEntry {
    int offset;
    int level; // 0 for L2C, 1 for L1D
    
    PrefetchEntry(int off, int lvl) : offset(off), level(lvl) {}
};

// HOP prefetcher implementation
class HOPPrefetcher {
private:
    const int cache_set = 64;
    const int cache_way = 12;

    // History table (128 entries)
    std::deque<HistoryEntry> history_table;
    const size_t HISTORY_SIZE = 128;
    
    // PC-based prefetcher (16 entries)
    std::vector<PCEntry> pc_prefetcher;
    const size_t PC_TABLE_SIZE = 16;
    
    // Space-based prefetcher (64 entries)
    std::vector<SpaceEntry> space_prefetcher;
    const size_t SPACE_TABLE_SIZE = 64;
    
    // Epoch-based prefetcher (1 entry)
    EpochEntry epoch_prefetcher;
    
    // Prefetch allocator
    std::vector<PrefetchEntry> prefetch_allocator;

    struct corres_cache {
        uint64_t addr = 0;
        uint8_t pf = 0;     // Is this prefetched
    };

    vector<vector<corres_cache>> ccache{
      vector<vector<corres_cache>>(cache_set, vector<corres_cache>(cache_way))};

    
    
    // Helper function - Update best offsets for PC-based prefetcher
    void update_pc_best_offsets(PCEntry &entry) {
        int max1 = 0, max2 = 0;
        int idx1 = -1, idx2 = -1;
        
        for (int i = 0; i < OFFSET_COUNT; i++) {
            if (entry.offset_confidence[i] > max1) {
                max2 = max1;
                max1 = entry.offset_confidence[i];
                idx2 = idx1;
                idx1 = i - MAX_OFFSET;
            } else if (entry.offset_confidence[i] > max2) {
                max2 = entry.offset_confidence[i];
                idx2 = i - MAX_OFFSET;
            }
        }
        
        entry.best_offset1 = idx1;
        entry.best_offset2 = idx2;
        entry.best_confidence1 = max1;
        entry.best_confidence2 = max2;
    }
    
    // Helper function - Update best offsets for Space-based prefetcher
    void update_space_best_offsets(SpaceEntry &entry) {
        int max1 = 0, max2 = 0;
        int idx1 = -1, idx2 = -1;
        
        for (int i = 0; i < OFFSET_COUNT; i++) {
            if (entry.offset_confidence[i] > max1) {
                max2 = max1;
                max1 = entry.offset_confidence[i];
                idx2 = idx1;
                idx1 = i - MAX_OFFSET;
            } else if (entry.offset_confidence[i] > max2) {
                max2 = entry.offset_confidence[i];
                idx2 = i - MAX_OFFSET;
            }
        }
        
        entry.best_offset1 = idx1;
        entry.best_offset2 = idx2;
        entry.best_confidence1 = max1;
        entry.best_confidence2 = max2;
    }
    
public:
    HOPPrefetcher() {
        // Initialize components
        history_table.clear();
        pc_prefetcher.resize(PC_TABLE_SIZE);
        space_prefetcher.resize(SPACE_TABLE_SIZE);
        prefetch_allocator.clear();
    }

    uint8_t corres_cache_add(uint32_t set, uint32_t way, uint64_t line_addr, uint8_t pf){
        ccache[set][way].addr = line_addr;
        ccache[set][way].pf = pf;
        return ccache[set][way].pf;
    }

    uint8_t corres_cache_is_pf(uint64_t line_addr)
    {
    for (uint32_t i = 0; i < cache_set; i++) {
        for (uint32_t ii = 0; ii < cache_way; ii++) {
        if (ccache[i][ii].addr == line_addr)
            return ccache[i][ii].pf;
        }
    }
    return 0;
    }
    
    // Record memory access
    void record_access(uint64_t addr, uint64_t ip, uint8_t cache_hit, uint8_t prefetch_hit, uint64_t cycle) {
        uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
        uint32_t ip_tag = hash_ip(ip);
        uint32_t region_tag = hash_region(addr);
        
        // 1. Update History Table
        HistoryEntry entry(ip_tag, region_tag, line_addr);
        
        // If history table is full, remove oldest entry
        if (history_table.size() >= HISTORY_SIZE) {
            history_table.pop_front();
        }
        
        // Add new entry to end of history table
        history_table.push_back(entry);
        
        // 2. Search for offsets between current access and historical accesses in History Table
        for (auto it = history_table.begin(); it != history_table.end(); ++it) {
            if (it->line_addr == line_addr) continue; // Skip same address
            
            // Calculate offset
            int offset = static_cast<int>(line_addr - it->line_addr);
            if (abs(offset) > MAX_OFFSET) continue; // Out of offset range
            
            int offset_idx = offset + MAX_OFFSET;
            
            // 3. Update PC-based Prefetcher (when PC tag matches)
            if (it->ip_tag == ip_tag) {
                // Find or create PC table entry
                bool found = false;
                for (auto &pc_entry : pc_prefetcher) {
                    if (pc_entry.ip_tag == ip_tag) {
                        // Increase offset confidence
                        pc_entry.offset_confidence[offset_idx]++;
                        update_pc_best_offsets(pc_entry);
                        found = true;
                        break;
                    }
                }
                
                if (!found) {
                    // Replace least recently used PC table entry
                    int replace_idx = rand() % PC_TABLE_SIZE;
                    pc_prefetcher[replace_idx] = PCEntry(ip_tag);
                    pc_prefetcher[replace_idx].offset_confidence[offset_idx] = 1;
                    update_pc_best_offsets(pc_prefetcher[replace_idx]);
                }
            }
            
            // 4. Update Space-based Prefetcher (when region tag matches)
            if (it->region_tag == region_tag) {
                // Find or create Space table entry
                bool found = false;
                for (auto &space_entry : space_prefetcher) {
                    if (space_entry.region_tag == region_tag) {
                        // Increase offset confidence
                        space_entry.offset_confidence[offset_idx]++;
                        update_space_best_offsets(space_entry);
                        found = true;
                        break;
                    }
                }
                
                if (!found) {
                    // Replace table entry (pseudo-LRU)
                    int replace_idx = rand() % SPACE_TABLE_SIZE;
                    space_prefetcher[replace_idx] = SpaceEntry(region_tag);
                    space_prefetcher[replace_idx].offset_confidence[offset_idx] = 1;
                    update_space_best_offsets(space_prefetcher[replace_idx]);
                }
            }
            
            // 5. Update Epoch-based Prefetcher (if cache miss or prefetch hit)
            if (!cache_hit || prefetch_hit) {
                epoch_prefetcher.offset_confidence[offset_idx]++;
                epoch_prefetcher.refresh_count++;
                
                // If refresh threshold reached, refresh confidence
                if (epoch_prefetcher.refresh_count >= REFRESH_THRESHOLD) {
                    epoch_prefetcher.refresh();
                } else {
                    epoch_prefetcher.update_best_offsets();
                }
            }
        }
    }
    
    // Perform prefetching
    void do_prefetch(CACHE* cache, uint64_t addr, uint64_t ip) {
        uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
        uint32_t ip_tag = hash_ip(ip);
        uint32_t region_tag = hash_region(addr);
        
        prefetch_allocator.clear();
        
        // 1. Collect predictions from PC-based prefetcher
        for (auto &pc_entry : pc_prefetcher) {
            if (pc_entry.ip_tag == ip_tag) {
                if (pc_entry.best_confidence1 >= 2) {
                    prefetch_allocator.push_back(PrefetchEntry(pc_entry.best_offset1, FILL_L1));
                    pp_prefetches++;
                }
                if (pc_entry.best_confidence2 >= 2) {
                    prefetch_allocator.push_back(PrefetchEntry(pc_entry.best_offset2, FILL_L2));
                    pp_prefetches++;
                }
                break;
            }
        }
        
        // 2. Collect predictions from Space-based prefetcher
        for (auto &space_entry : space_prefetcher) {
            if (space_entry.region_tag == region_tag) {
                if (space_entry.best_confidence1 >= 2) {
                    // Check if already in prefetch allocator
                    bool found = false;
                    for (auto &pf : prefetch_allocator) {
                        if (pf.offset == space_entry.best_offset1) {
                            pf.level = FILL_L1; // Upgrade to L1 prefetch
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        prefetch_allocator.push_back(PrefetchEntry(space_entry.best_offset1, FILL_L1));
                    }
                    sp_prefetches++;
                }
                if (space_entry.best_confidence2 >= 2) {
                    // Check if already in prefetch allocator
                    bool found = false;
                    for (auto &pf : prefetch_allocator) {
                        if (pf.offset == space_entry.best_offset2) {
                            pf.level = FILL_L1; // Upgrade to L1 prefetch
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        prefetch_allocator.push_back(PrefetchEntry(space_entry.best_offset2, FILL_L2));
                    }
                    sp_prefetches++;
                }
                break;
            }
        }
        
        // 3. Collect predictions from Epoch-based prefetcher
        if (epoch_prefetcher.best_offset1 != -1 && epoch_prefetcher.offset_confidence[epoch_prefetcher.best_offset1 + MAX_OFFSET] >= 32) {
            // Check if already in prefetch allocator
            bool found = false;
            for (auto &pf : prefetch_allocator) {
                if (pf.offset == epoch_prefetcher.best_offset1) {
                    pf.level = FILL_L1; // Upgrade to L1 prefetch
                    found = true;
                    break;
                }
            }
            if (!found) {
                prefetch_allocator.push_back(PrefetchEntry(epoch_prefetcher.best_offset1, FILL_L1));
            }
            ep_prefetches++;
        }
        
        if (epoch_prefetcher.best_offset2 != -1 && epoch_prefetcher.offset_confidence[epoch_prefetcher.best_offset2 + MAX_OFFSET] >= 32) {
            // Check if already in prefetch allocator
            bool found = false;
            for (auto &pf : prefetch_allocator) {
                if (pf.offset == epoch_prefetcher.best_offset2) {
                    pf.level = FILL_L1; // Upgrade to L1 prefetch
                    found = true;
                    break;
                }
            }
            if (!found) {
                prefetch_allocator.push_back(PrefetchEntry(epoch_prefetcher.best_offset2, FILL_L2));
            }
            ep_prefetches++;
        }
        
        if (epoch_prefetcher.best_offset3 != -1 && epoch_prefetcher.offset_confidence[epoch_prefetcher.best_offset3 + MAX_OFFSET] >= 32) {
            // Check if already in prefetch allocator
            bool found = false;
            for (auto &pf : prefetch_allocator) {
                if (pf.offset == epoch_prefetcher.best_offset3) {
                    pf.level = FILL_L1; // Upgrade to L1 prefetch
                    found = true;
                    break;
                }
            }
            if (!found) {
                prefetch_allocator.push_back(PrefetchEntry(epoch_prefetcher.best_offset3, FILL_L2));
            }
            ep_prefetches++;
        }
        
        // Count prefetch degree
        prefetch_degree_count[prefetch_allocator.size()]++;
        
        // 4. Perform actual prefetching
        for (auto &pf : prefetch_allocator) {
            uint64_t pf_line_addr = line_addr + pf.offset;
            uint64_t pf_addr = pf_line_addr << LOG2_BLOCK_SIZE;
            
            // Check if prefetch queue and MSHR have space (modern ChampSim API)
            if (cache->get_pq_occupancy().back() < cache->get_pq_size().back() &&
                cache->get_pq_occupancy().back() + cache->get_mshr_occupancy() < cache->get_mshr_size()) {
                
                bool fill_this_level = (pf.level == FILL_L1);
                int success = cache->prefetch_line(pf_addr, fill_this_level, 0);
                if (success) {
                    prefetch_issued++;
                    
                    // Check if cross-page prefetch
                    uint64_t curr_page = line_addr >> 6; // 6 is log of blocks per page, corresponds to 64 blocks
                    uint64_t pf_page = pf_line_addr >> 6;
                    if (curr_page != pf_page) {
                        cross_page_prefetch++;
                    }
                }
            }
        }
    }
};

// Map each cache instance to its Chimera prefetcher
#if (USER_CODES == ENABLE)
#include <map>
#include <memory>
namespace {
std::map<CACHE*, std::unique_ptr<HOPPrefetcher>> chimera_prefetchers;
}

static uint64_t global_cycle = 0;

void CACHE::prefetcher_initialize()
{
    chimera_prefetchers[this] = std::make_unique<HOPPrefetcher>();
    std::cout << "Chimera Prefetcher Initialised" << std::endl;
}

void CACHE::prefetcher_cycle_operate()
{
    ++global_cycle;
}

uint32_t CACHE::prefetcher_cache_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit,
                                                                 bool useful_prefetch, uint8_t type, uint32_t metadata_in)
{
    if ((type != champsim::to_underlying(access_type::LOAD)) &&
        (type != champsim::to_underlying(access_type::PREFETCH))) {
        return metadata_in;
    }

    auto& pf = chimera_prefetchers.at(this);

    uint8_t prefetch_hit = pf->corres_cache_is_pf(addr >> LOG2_BLOCK_SIZE);
    pf->record_access(addr, ip, cache_hit, prefetch_hit, global_cycle);
    pf->do_prefetch(this, addr, ip);

    return metadata_in;
}

uint32_t CACHE::prefetcher_cache_fill(uint64_t addr, uint32_t set, uint32_t way,
                                                              uint8_t prefetch, uint64_t /*evicted_addr*/, uint32_t metadata_in)
{
    auto& pf = chimera_prefetchers.at(this);
    uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
    pf->corres_cache_add(set, way, line_addr, prefetch);
    return metadata_in;
}

void CACHE::prefetcher_final_stats()
{
    std::cout << "PC-based Prefetches: " << pp_prefetches << std::endl;
    std::cout << "Space-based Prefetches: " << sp_prefetches << std::endl;
    std::cout << "Epoch-based Prefetches: " << ep_prefetches << std::endl;
    std::cout << "Total Prefetches Issued: " << prefetch_issued << std::endl;
    std::cout << "Cross-Page Prefetches: " << cross_page_prefetch << std::endl;
    std::cout << "Prefetch degree distribution: ";
    for (int i = 0; i < 9; i++)
        std::cout << prefetch_degree_count[i] << " ";
    std::cout << std::endl;
}

#endif // USER_CODES