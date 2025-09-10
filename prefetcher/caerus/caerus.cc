#include "caerus.hh"

#include <algorithm>
#include <map>
#include <memory>
#include <bit>

#if (USER_CODES == ENABLE)

namespace
{
std::map<CACHE*, std::unique_ptr<caerus_space::CAERUS>> caerus_prefetchers;

template <typename T>
constexpr bool is_power_of_2(T n)
{
    return (n > 0) && ((n & (n - 1)) == 0);
}
}

using namespace caerus_space;

static inline std::size_t hash_index(uint64_t value, std::size_t log_size) {
    uint64_t hash = value ^ (value >> log_size);
    hash &= ((1ULL << log_size) - 1);
    return hash;
}

RRTable::RRTable(std::size_t size) : log_size(champsim::lg2(size))
{
  table.resize(size);
}

std::size_t RRTable::index(uint64_t addr) const
{
  uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
  return hash_index(line_addr, log_size);
}

void RRTable::insert(uint64_t addr, uint64_t pc)
{
  std::size_t idx = index(addr);
  uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
  table[idx] = {line_addr, pc};
}

RRTable::Entry RRTable::lookup(uint64_t addr) const
{
  std::size_t idx = index(addr);
  const Entry& e = table[idx];
  return e;
}

bool RRTable::test(uint64_t addr) const
{
  auto idx = index(addr);
  return (table[idx].line_addr) == (addr >> LOG2_BLOCK_SIZE);
}

HoldingTable::HoldingTable(std::size_t size) : log_size(champsim::lg2(size)) { entries.resize(size); }

void HoldingTable::insert(uint64_t addr, uint64_t base_addr, uint64_t pc)
{
  auto idx = index(addr);
  entries[idx] = {base_addr, pc};
}

std::optional<HoldingTable::Entry> HoldingTable::lookup(uint64_t addr)
{
  auto idx = index(addr);
  Entry& e = entries[idx];
  if (e.base_addr != 0) {
    Entry result = e;          
    e = Entry{};       
    return result;
  }
  return std::nullopt;
}

uint64_t HoldingTable::index(uint64_t addr) const
{
  return hash_index(addr, log_size);
}

RecentPrefetchesTable::RecentPrefetchesTable(std::size_t size) : log_size(champsim::lg2(size)) { entries.resize(size); }

void RecentPrefetchesTable::insert(uint64_t pf_addr, uint64_t offset, int offset_idx)
{
  auto idx = index(pf_addr);
  entries[idx] = {pf_addr, offset, offset_idx};
}

RecentPrefetchesTable::Entry RecentPrefetchesTable::lookup(uint64_t pf_addr)
{
  auto idx = index(pf_addr);
  Entry& e = entries[idx];
  if (e.pf_addr != 0) {
    Entry result = e;          
    e = Entry{};       
    return result;
  }
  return Entry{};
}

bool RecentPrefetchesTable::test(uint64_t pf_addr) const
{
  auto idx = index(pf_addr);
  return entries[idx].pf_addr == pf_addr;
}

uint64_t RecentPrefetchesTable::index(uint64_t pf_addr) const
{
  return hash_index(pf_addr, log_size);
}


AccuracyTable::AccuracyTable(std::size_t size)
    : table_size(size),
      table(size, std::vector<uint16_t>(NUM_OFFSETS, 8)) 
{}

int AccuracyTable::getIndex(uint64_t pc) const { return (pc ^ (pc / table_size)) % table_size; }

int16_t AccuracyTable::lookup(uint64_t pc, int offset_idx) const
{
  int idx = getIndex(pc);
  if (offset_idx >= 0 && offset_idx < NUM_OFFSETS) {
    return table[idx][offset_idx];
  }
  return 0; 
}

void AccuracyTable::increment(uint64_t pc, int offset_idx)
{
  int idx = getIndex(pc);
  if (offset_idx >= 0 && offset_idx < NUM_OFFSETS) {
    if (table[idx][offset_idx] < ACC_MAX)
      table[idx][offset_idx]+= ACCURACY_INCREMENT; // More optimistic for accuracy 
  }
}

void AccuracyTable::decrement(uint64_t pc, int offset_idx)
{
  int idx = getIndex(pc);
  if (offset_idx >= 0 && offset_idx < NUM_OFFSETS) {
    if (table[idx][offset_idx] > ACC_MIN)
      table[idx][offset_idx]-= ACCURACY_DECREMENT;
  }
}

void AccuracyTable::resetOffsetStats(int offset_idx)
{
  if (offset_idx < 0 || offset_idx >= NUM_OFFSETS)
    return;

  for (std::size_t i = 0; i < table_size; ++i) {
    table[i][offset_idx] = 8;
  }
}

EvictionTable::EvictionTable(std::size_t size)
    : log_size(champsim::lg2(size))
{
  table.resize(size);
}

std::size_t EvictionTable::index(uint64_t line_addr) const
{
  return hash_index(line_addr, log_size);
}

void EvictionTable::insert(uint64_t line_addr)
{ 
  std::size_t idx = index(line_addr);
  table[idx] = line_addr;
}

bool EvictionTable::test(uint64_t addr) const
{
  uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
  auto idx = index(line_addr);
  return table[idx] == line_addr;
}

CAERUS::CAERUS()
    : scoreMax(SCORE_MAX), roundMax(ROUND_MAX), phaseBestOffset(0), bestScore(0), round(0), rr_table(RR_SIZE), holding_table(HOLDING_TABLE_SIZE),
      accuracy_table(ACCURACY_TABLE_SIZE), eviction_table(EVICTION_TABLE_SIZE), recent_prefetches_table(RECENT_PREFETCHES_SIZE)

{
  if (!is_power_of_2(RR_SIZE)) {
    throw std::invalid_argument{"Number of RR entries is not power of 2\n"};
  }
  if (!is_power_of_2(BLOCK_SIZE)) {
    throw std::invalid_argument{"Cache line size is not power of 2\n"};
  }

  if (!is_power_of_2(HOLDING_TABLE_SIZE)) {
    throw std::invalid_argument{"Holding table size is not power of 2\n"};
  }

  /*
   * Following the paper implementation, a list with the specified number
   * of offsets which are of the form 2^i * 3^j * 5^k with i,j,k >= 0
   */
    
  const int factors[] = {2, 3, 5};
  unsigned int i = 0;
  int64_t offset_i = 1;
    

  while (i < OFFSET_LIST_SIZE) 
  {
    int64_t offset = offset_i;

    for (int n : factors) {
      while ((offset % n) == 0) {
        offset /= n;
      }
    }

    if (offset == 1) {
      offsetsList.push_back(OffsetListEntry(offset_i, 0));
      i++;
      /*
       * If we want to use negative offsets, add also the negative value
       * of the offset just calculated
       */
      if (NEGATIVE_OFFSETS_ENABLE) {
        offsetsList.push_back(OffsetListEntry(-offset_i, 0));
        i++;
      }
    }

    offset_i++;
  }

  offsetsListIterator = offsetsList.begin();
}

void CAERUS::resetScores()
{
  for (auto& it : offsetsList) {
    it.second = 0;
  }
}

void CAERUS::bestOffsetLearning(uint64_t addr, uint8_t cache_hit)
{
  if (cache_hit) {

    // Skip learning if any of the already-learned offsets covered this addr
    for (std::size_t i = 0; i < learned_offsets.size(); ++i) {
      if (i == current_learning_offset_idx) continue; // skip the offset we're learning

      uint64_t off = learned_offsets[i];
      if (off == 0) continue; // unused slot

      // Overlap Prevention MOD 
      uint64_t prev_pf_addr = addr - (off << LOG2_BLOCK_SIZE);
      if (rr_table.test(prev_pf_addr) and accuracy_table.lookup(rr_table.lookup(prev_pf_addr).pc, i) >= ACCURACY_THRESHOLD) {
        if (OVERLAP_LEAKAGE) {
          overlap_leakage_counter++;
          if (overlap_leakage_counter % LEAKAGE_PERIOD != 0) {
            return; 
          }
        } else {
          return; // Already covered by another learned offset
        }
      }
    }

    // Check if the addr is in the recent prefetches table (pf hit)
    if (recent_prefetches_table.test(addr)) {

      auto entry = recent_prefetches_table.lookup(addr);

      assert(entry.offset != 0); // Should not happen

      bool learn = entry.offset_idx == current_learning_offset_idx && entry.offset == learned_offsets[current_learning_offset_idx]; 

      if (learn) {
        rp_hit_counter++; // Only learn on pf hits that are from the current learning offset
      } else {
        rp_miss_counter++;
        return; // This access was prefetched by another offset, do not learn 
      }
    } else {
      // Stats 
      rp_miss_counter++; // The RP Table is not tracking this access 
    }
  }

  for (std::size_t i = 0; i < TRAIN_SPEED; ++i) 
  {

    uint64_t offset = (*offsetsListIterator).first;

    bool is_learned_offset = std::find(learned_offsets.begin(), learned_offsets.end(), offset) != learned_offsets.end();
    // Still include the current learning offset 
    bool is_current_learning_offset = offset == learned_offsets[current_learning_offset_idx];

    // TEST MOD no offset learning
    uint64_t test_addr = addr - (offset << LOG2_BLOCK_SIZE);

    // Skip learning if offset is in learned_offsets
    if (!is_learned_offset || is_current_learning_offset) {

      // Score offset if demand addr in RR
      if (rr_table.test(test_addr)) {
        (*offsetsListIterator).second ++;

        if ((*offsetsListIterator).second > bestScore) {
          bestScore = (*offsetsListIterator).second;
          phaseBestOffset = offset;
        }
      }

    }

    // Advance learning offset candidate
    ++offsetsListIterator;
    if (offsetsListIterator == offsetsList.end()) {
      offsetsListIterator = offsetsList.begin();
      round++;
    }

    // Learning phase end
    if ((bestScore >= scoreMax) || (round >= roundMax)) {

      // Record statistics
      if (bestScore >= scoreMax) score_max_counter++;
      if (round >= roundMax) round_max_counter++;

      // This avoids training bad offsets
      if (bestScore > BAD_SCORE) {
        learned_offsets[current_learning_offset_idx] = phaseBestOffset;
        // Reset statistics for this offset
        accuracy_table.resetOffsetStats(current_learning_offset_idx);
        // Move to next learning slot 
        current_learning_offset_idx = (current_learning_offset_idx + 1) % learned_offsets.size();
      } else {
        // Retrain the previous accurate offset 
        current_learning_offset_idx = (current_learning_offset_idx - 1) % learned_offsets.size();
        bad_score_counter++;
      }

      

      // Reset learning state
      round = 0;
      bestScore = 0;
      phaseBestOffset = 0;
      resetScores();

    }

  }
}

std::vector<uint64_t> CAERUS::calculateAccuratePrefetchAddrs(uint64_t addr, uint64_t pc, CACHE* cache)
{
  std::vector<uint64_t> pf_addrs;
  for (int i = 0; i < NUM_OFFSETS; i++) {
    uint64_t offset = learned_offsets[i];
    if (offset == 0)
      continue;

    int16_t score = accuracy_table.lookup(pc, i);
    // TEST MOD no accuracy filtering 
    // score = 10; 

    if (score >= ACCURACY_THRESHOLD) {
      uint64_t pf_addr = addr + (offset << LOG2_BLOCK_SIZE);

      if (!ALLOW_CROSS_PAGE) {
        if ((addr >> LOG2_PAGE_SIZE) != (pf_addr >> LOG2_PAGE_SIZE)) {
          continue;
        }
      }

      if (cache->get_pq_occupancy().back() >= cache->get_pq_size().back()) {
        continue;
      }

      pf_addrs.push_back(pf_addr);
    }
  }
  return pf_addrs;
}

std::vector<uint64_t> CAERUS::calculateAccuratePrefetchOffsets(uint64_t addr, uint64_t pc, CACHE* cache)
{
  std::vector<uint64_t> pf_offsets;
  for (int i = 0; i < NUM_OFFSETS; i++) {
    uint64_t offset = learned_offsets[i];
    if (offset == 0)
      continue;

    int16_t score = accuracy_table.lookup(pc, i);
    // TEST MOD no accuracy filtering 
    // score = 10; 

    if (score >= ACCURACY_THRESHOLD) {

      if (!ALLOW_CROSS_PAGE) {
        if ((addr >> LOG2_PAGE_SIZE) != ((addr + (offset << LOG2_BLOCK_SIZE)) >> LOG2_PAGE_SIZE)) {
          continue;
        }
      }

      if (cache->get_pq_occupancy().back() >= cache->get_pq_size().back()) {
        continue;
      }

      pf_offsets.push_back(offset);
    }
  }
  return pf_offsets;
}

std::vector<uint64_t> CAERUS::calculateAllPrefetchAddrs(uint64_t addr)
{
  std::vector<uint64_t> pf_addrs;

  for (auto offset : learned_offsets) {
    if (offset == 0)
      continue;

    uint64_t pf_addr = (addr + offset) << LOG2_BLOCK_SIZE; // shift to get full addr bits

    if(!ALLOW_CROSS_PAGE){
      if (((addr << LOG2_BLOCK_SIZE) >> LOG2_PAGE_SIZE) != (pf_addr >> LOG2_PAGE_SIZE)) {
        pf_addrs.push_back(0); // represent page crosses by 0
        continue;
      }
    }

    pf_addrs.push_back(pf_addr);
  }

  return pf_addrs;
}

int CAERUS::getOffsetIdx(uint64_t offset)
{
  return std::find(learned_offsets.begin(), learned_offsets.end(), offset) - learned_offsets.begin();
}

void CAERUS::accuracy_train(uint64_t line_addr, uint64_t pc)
{
  if (pc == 0)
    return;

  std::vector<uint64_t> pf_addrs = calculateAllPrefetchAddrs(line_addr);

  for (int i = 0; i < NUM_OFFSETS; ++i) {
    if (i >= static_cast<int>(pf_addrs.size())){
      break; // for safety
    }
    
    if(!ALLOW_CROSS_PAGE){
      if(pf_addrs[i] == 0) {
        continue; // skip prefetches that cross page boundary
      }
    }
  
    if (rr_table.test(pf_addrs[i]) || eviction_table.test(pf_addrs[i])) {
      accuracy_table.increment(pc, i);
    } else {
      accuracy_table.decrement(pc, i);
    }
  }

  eviction_table.insert(line_addr);
}

void CAERUS::insertFill(uint64_t addr, uint64_t current_cycle)
{
  auto result = holding_table.lookup(addr);
  if (result.has_value()) {
    RRTable::Entry evicted_entry = rr_table.lookup(result->base_addr);
    accuracy_train(evicted_entry.line_addr, evicted_entry.pc);
    rr_table.insert(result->base_addr, result->pc);
  }
}

// ==============================
// ==== CACHE FUNCTIONS =========
// ==============================

void CACHE::prefetcher_initialize()
{
  caerus_prefetchers[this] = std::make_unique<CAERUS>();
  std::cout << "CAERUS Prefetcher Initialised" << std::endl;
}

void CACHE::prefetcher_cycle_operate() 
{
}

uint32_t CACHE::prefetcher_cache_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, bool useful_prefetch, uint8_t type,
                                                               uint32_t metadata_in)
{
  if ((type != champsim::to_underlying(access_type::LOAD)) && (type != champsim::to_underlying(access_type::PREFETCH))) {
    return metadata_in;
  }

  if ((cache_hit && useful_prefetch) || !cache_hit) {
    auto pf_addrs = caerus_prefetchers.at(this)->calculateAccuratePrefetchAddrs(addr, ip, this);

    if (!pf_addrs.empty()) {

      // PRE-RECENTPREFETCHES IMPLEMENTATION
      // bool added_to_holding = false;
      // for (auto pf_addr : pf_addrs) {
        
      //   // Basic MSHR Occupancy Based Filtering
      //   if (this->get_pq_occupancy().back() < this->get_pq_size().back() &&
      //       this->get_pq_occupancy().back() + this->get_mshr_occupancy() < this->get_mshr_size() - 1) {

      //     bool issued = this->prefetch_line(pf_addr, true, 0x1);
          
          
      //     if (issued) {
      //       // Record total prefetches issued
      //       caerus_prefetchers.at(this)->pf_counter++;
            
      //       // Sample one prefetch into the holding table 
      //       if (!added_to_holding) {

      //         caerus_prefetchers.at(this)->holding_table.insert(pf_addr, addr, ip);

      //         // Record total prefetch opportunities 
      //         caerus_prefetchers.at(this)->trigger_pf_counter++;
              
      //         added_to_holding = true;
      //       }

      //       // Put prefetches into the recent prefetches table
      //       caerus_prefetchers.at(this)->recent_prefetches_table.insert(pf_addr, offset, i);
      //     }
      //   }
      // }

      bool added_to_holding = false;

      auto pf_offsets = caerus_prefetchers.at(this)->calculateAccuratePrefetchOffsets(addr, ip, this);

      
      for (auto pf_offset : pf_offsets) {
        
        // Basic MSHR Occupancy Based Filtering
        if (this->get_pq_occupancy().back() < this->get_pq_size().back() &&
            this->get_pq_occupancy().back() + this->get_mshr_occupancy() < this->get_mshr_size() - 1) {

          uint64_t pf_addr = addr + (pf_offset << LOG2_BLOCK_SIZE);

          bool issued = this->prefetch_line(pf_addr, true, 0x1);
          
          
          if (issued) {
            // Record total prefetches issued
            caerus_prefetchers.at(this)->pf_counter++;
            
            // Sample one prefetch into the holding table 
            if (!added_to_holding) {

              caerus_prefetchers.at(this)->holding_table.insert(pf_addr, addr, ip);

              // Record total prefetch opportunities 
              caerus_prefetchers.at(this)->trigger_pf_counter++;
              
              added_to_holding = true;
            }

            // Put prefetches into the recent prefetches table
            auto offset_idx = caerus_prefetchers.at(this)->getOffsetIdx(pf_offset);
            caerus_prefetchers.at(this)->recent_prefetches_table.insert(pf_addr, pf_offset, offset_idx);
          }
        }
      }

      

    } else if (cache_hit && useful_prefetch) { // Prefetch hit where no prefetches issued
      RRTable::Entry evicted_entry = caerus_prefetchers.at(this)->rr_table.lookup(addr);
      // Preventing duplicate items in the RR table 
      if (evicted_entry.pc != ip) {
        caerus_prefetchers.at(this)->accuracy_train(evicted_entry.line_addr, evicted_entry.pc);
        caerus_prefetchers.at(this)->rr_table.insert(addr, ip);
      }
      
    } else {  // X is a cache miss with no PFs generated
      caerus_prefetchers.at(this)->holding_table.insert(addr, addr, ip);
    }

    caerus_prefetchers.at(this)->bestOffsetLearning(addr, cache_hit);
  }

  return metadata_in;
}

uint32_t CACHE::prefetcher_cache_fill(uint64_t addr, uint32_t set, uint32_t way, uint8_t prefetch, uint64_t evicted_addr,
                                                            uint32_t metadata_in)
{
  caerus_prefetchers.at(this)->insertFill(addr, current_cycle);
  return metadata_in;
}

void CACHE::prefetcher_final_stats()
{

  std::cout << "CAERUS Prefetcher Statistics:" << std::endl;
  std::cout << "Round Max Counter: " << caerus_prefetchers.at(this)->round_max_counter << std::endl;
  std::cout << "Score Max Counter: " << caerus_prefetchers.at(this)->score_max_counter << std::endl;
  std::cout << "Average Prefetches: " << caerus_prefetchers.at(this)->pf_counter * 1.0 / caerus_prefetchers.at(this)->trigger_pf_counter << std::endl;
  std::cout << "RP Miss Counter: " << caerus_prefetchers.at(this)->rp_miss_counter << std::endl;
  std::cout << "RP Hit Counter: " << caerus_prefetchers.at(this)->rp_hit_counter << std::endl;
}

#endif
