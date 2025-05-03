#include "kairos.hh"

#include <algorithm>

using namespace kairos_space;


PrefetchTable::PrefetchTable(std::size_t table_max_size)
  : max_size(table_max_size) {}

void PrefetchTable::insert(const Entry& entry) {
  if (table.size() >= max_size) {
    table.pop_front();  // Remove oldest
  }
  table.push_back(entry);  // Insert newest
}

std::optional<PrefetchTable::Entry> PrefetchTable::lookup(uint64_t addr) const {
  for (const auto& entry : table) {
    if (entry.addr == addr) {
      return entry;
    }
  }
  return std::nullopt;
}

void PrefetchTable::remove(uint64_t addr) {
  auto it = std::find_if(table.begin(), table.end(),
                         [addr](const Entry& entry) {
                           return entry.addr == addr;
                         });
  if (it != table.end()) {
    table.erase(it);
  }
}

KAIROS::KAIROS()
    : scoreMax(SCORE_MAX), roundMax(ROUND_MAX), badScore(BAD_SCORE), rrEntries(RR_SIZE), tagMask((1 << TAG_BITS) - 1),
      issuePrefetchRequests(true), prefetch_table(PREFETCH_TABLE_SIZE), phaseBestOffset(0), bestScore(0), round(0)
{
  if (!champsim::msl::isPowerOf2(rrEntries)) {
    throw std::invalid_argument{"Number of RR entries is not power of 2\n"};
  }
  if (!champsim::msl::isPowerOf2(BLOCK_SIZE)) {
    throw std::invalid_argument{"Cache line size is not power of 2\n"};
  }
  if (NEGATIVE_OFFSETS_ENABLE && (OFFSET_LIST_SIZE % 2 != 0)) {
    throw std::invalid_argument{"Negative offsets enabled with odd offset list size\n"};
  }

  rrTable.resize(rrEntries);

  /*
   * Following the paper implementation, a list with the specified number
   * of offsets which are of the form 2^i * 3^j * 5^k with i,j,k >= 0
   */
  const int factors[] = {2, 3, 5};
  unsigned int i = 0;
  int64_t offset_i = 1;

  while (i < OFFSET_LIST_SIZE) {
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
  if constexpr (champsim::kairos_dbug) {
    std::cout << "Offsets List:\n";
    for (const auto& entry : offsetsList) {
        std::cout << "Offset: " << entry.first << ", Metadata: " << static_cast<int>(entry.second) << '\n';
    }
  }
}

unsigned int KAIROS::index(uint64_t addr) const
{
  /*
   * For indexing the RR Table, the cache line
   * address is XORed with itself after right shifting it by the log base
   * 2 of the number of entries in the RR Table.
  */
  uint64_t log_rr_entries = champsim::lg2(rrEntries);
  uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
  uint64_t hash = line_addr ^ (line_addr >> log_rr_entries);
  hash &= ((1ULL << log_rr_entries) - 1);
  return hash % rrEntries;
}

void KAIROS::insertIntoRR(uint64_t addr, uint64_t tag)
{
  rrTable[index(addr)] = tag;
}

void KAIROS::resetScores()
{
  for (auto& it : offsetsList) {
    it.second = 0;
  }
}

inline uint64_t KAIROS::tag(uint64_t addr) const { return (addr >> LOG2_BLOCK_SIZE) & tagMask; }

bool KAIROS::testRR(uint64_t addr_tag) const
{
  for (auto& it : rrTable) {
    if (it == addr_tag) {
      return true;
    }
  }

  return false;
}

void KAIROS::bestOffsetLearning(uint64_t addr)
{
  /*
   * This can be changed to store offset in cache/shadowcache and checking this way
  */


  // Skip learning if any of the already-learned offsets covered this addr
  for (std::size_t i = 0; i < learned_offsets.size(); ++i) {
    if (i == current_learning_offset_idx) continue; // skip the offset we're learning
    uint64_t off = learned_offsets[i];
    if (off == 0) continue; // unused slot
    uint64_t prev_pf_addr = addr - (off << LOG2_BLOCK_SIZE);
    if (testRR(tag(prev_pf_addr))) {
        return; // Already covered by another learned offset
    }
  }

  uint64_t offset = (*offsetsListIterator).first;

  // Subtract offset from full addr to avoid underflow before tagging
  uint64_t lookup_tag = tag(addr - (offset << LOG2_BLOCK_SIZE));

  // Score offset if demand addr was prefetched
  if (testRR(lookup_tag)) {
    if constexpr (champsim::kairos_dbug) {
      std::cout << "Address " << lookup_tag << " found in RR table" << std::endl;
    }
    (*offsetsListIterator).second++;

    if ((*offsetsListIterator).second > bestScore) {
      bestScore = (*offsetsListIterator).second;
      phaseBestOffset = offset;
      if constexpr (champsim::kairos_dbug) {
        std::cout << "New best score is " << bestScore << " for offset " << offset << std::endl;
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
    learned_offsets[current_learning_offset_idx] = phaseBestOffset;
    if constexpr (champsim::kairos_dbug) {
      std::cout << "Learned new offset #" << current_learning_offset_idx << ": " << phaseBestOffset << std::endl;
    }

    // Move to next learning slot (wrap 0–3)
    current_learning_offset_idx = (current_learning_offset_idx + 1) % learned_offsets.size();

    // Reset learning phase
    round = 0;
    bestScore = 0;
    phaseBestOffset = 0;
    resetScores();
  }
}

std::vector<uint64_t> KAIROS::calculatePrefetchAddrs(uint64_t addr)
{
  std::vector<uint64_t> pf_addrs;

  for (auto offset : learned_offsets) {
    if (offset == 0) continue; // Skip unused slots

    uint64_t pf_addr = addr + (offset << LOG2_BLOCK_SIZE);

    // prefetch_table.remove(PrefetchTableEntry{addr, matched_offset}); 

    if ((addr >> LOG2_PAGE_SIZE) != (pf_addr >> LOG2_PAGE_SIZE))
    {
      if constexpr (champsim::kairos_dbug) 
      {
        std::cout << "Prefetch not issued - Page crossed" << std::endl;
      }
      continue;
    } 

    PrefetchTable::Entry entry{pf_addr, offset};
    prefetch_table.insert(entry);

    pf_addrs.push_back(pf_addr);

    if constexpr (champsim::kairos_dbug) {
      std::cout << "Generated prefetch: " << pf_addr << " with offset " << offset << std::endl;
    }
  }

  return pf_addrs;
}

void KAIROS::insertFill(uint64_t addr)
{
  auto result = prefetch_table.lookup(addr);
  if (result.has_value()) {
    uint64_t matched_offset = result->offset;

    uint64_t base_address = addr - (matched_offset << LOG2_BLOCK_SIZE);
    // prefetch_table.invalidate(PrefetchTableEntry{addr, matched_offset}); 

    if ((base_address >> LOG2_PAGE_SIZE) != (addr >> LOG2_PAGE_SIZE))
    {
      if constexpr (champsim::kairos_dbug) 
      {
        std::cout << "Filled address crossed page" << std::endl;
      }
      return;
    } 

    uint64_t tag_y = tag(base_address);

    if (issuePrefetchRequests) {
      insertIntoRR(addr, tag_y);
      if constexpr (champsim::kairos_dbug) 
      {
        std::cout << "Filled RR" << std::endl;
      }
    }
  }
  else {
    if constexpr (champsim::kairos_dbug) 
    {
      std::cout << "Filled addr not found in recent prefetches" << std::endl;
    }
  }
}

void CACHE::prefetcher_initialize() 
{ 
  kairos = new KAIROS();
  std::cout << "KAIROS Prefetcher Initialise" << std::endl; 
}

// addr: address of cache block
// ip: PC of load/store cache_hit: true if load/store hits in cache.
// type : 0 for load
uint32_t CACHE::prefetcher_cache_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, bool useful_prefetch, uint8_t type, uint32_t metadata_in)
{
  if (type != 0) {
    return metadata_in; // Not a load
  }

  if ((cache_hit && useful_prefetch) || !cache_hit) {
    // increment useful prefetch counter, not quite the same as cache stat since we don't remove the prefetch tag, could change to increment sim stats here
    // also missing mshr hit
    if(cache_hit && useful_prefetch) { kairos->pf_useful_kairos++; }
    kairos->bestOffsetLearning(addr);

    if (kairos->issuePrefetchRequests) {
      auto pf_addrs = kairos->calculatePrefetchAddrs(addr);
      
      for (auto pf_addr : pf_addrs) {
        std::vector<std::size_t> pq_occupancy = get_pq_occupancy();
        // std::cout << "pq_occupany: " << pq_occupancy[2] << std::endl;
        // Compare l2C pq size to check if pf will be issued
        // if (pq_occupancy[2] < PQ_SIZE){ 
        //   ++(kairos->pf_issued_kairos);
        // }
        bool issued = prefetch_line(pf_addr, true, metadata_in);
        if (issued) {++(kairos->pf_issued_kairos);} else {std::cout << "pq_occupany: " << pq_occupancy[2] << "PQ FULL" << std::endl;}
      }
    }
  }

  return metadata_in;
}

// Invoked when cache is refilled (prefetch or not)
// addr: address of cache line that was prefetched.
// set and way of allocated entry
uint32_t CACHE::prefetcher_cache_fill(uint64_t addr, uint32_t set, uint32_t way, uint8_t prefetch, uint64_t evicted_addr, uint32_t metadata_in)
{
  // Only insert into the RR Table if fill is a hardware prefetch
  if (prefetch){
    kairos->insertFill(addr);
  }

  return metadata_in;
}

void CACHE::prefetcher_cycle_operate() {}

void CACHE::prefetcher_final_stats() {
  std::cout << "KAIROS ISSUED: " << kairos->pf_issued_kairos << std::endl;
  std::cout << "KAIROS USEFUL: " << kairos->pf_useful_kairos << std::endl;
}
