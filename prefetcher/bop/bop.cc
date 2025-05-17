#include "bop.hh"

#include <algorithm>

using namespace bop_space;

BOP::BOP()
    : scoreMax(SCORE_MAX), roundMax(ROUND_MAX), badScore(BAD_SCORE), rrEntries(RR_SIZE), tagMask((1 << TAG_BITS) - 1),
      bestOffset(0), phaseBestOffset(0), bestScore(0), round(0), issuePrefetchRequests(false)
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
}

unsigned int BOP::index(uint64_t addr) const
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

void BOP::insertIntoRR(uint64_t addr, uint64_t tag)
{
  rrTable[index(addr)] = tag;
}

void BOP::resetScores()
{
  for (auto& it : offsetsList) {
    it.second = 0;
  }
}

inline uint64_t BOP::tag(uint64_t addr) const {
  uint64_t log_rr_entries = champsim::lg2(rrEntries);
  uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
  return (line_addr >> log_rr_entries) & tagMask; 
}

bool BOP::testRR(uint64_t addr_tag) const
{
  for (auto& it : rrTable) {
    if (it == addr_tag) {
      return true;
    }
  }

  return false;
}

void BOP::bestOffsetLearning(uint64_t addr)
{
  int64_t offset = (*offsetsListIterator).first;

  /*
   * Compute the lookup tag for the RR table. As tags are generated using
   * lower 12 bits we subtract offset from the full address rather than the
   * tag to avoid integer underflow.
  */
  uint64_t lookup_tag = tag((addr) - (offset << LOG2_BLOCK_SIZE));

  // There was a hit in the RR table, increment the score for this offset
  if (testRR(lookup_tag)) {
    if constexpr (champsim::bop_debug) 
    {
      std::cout << "Tag " << lookup_tag << " found in the RR table" << std::endl;
    }
    (*offsetsListIterator).second++;
    if ((*offsetsListIterator).second > bestScore) {
      bestScore = (*offsetsListIterator).second;
      phaseBestOffset = (*offsetsListIterator).first;
      if constexpr (champsim::bop_debug) 
      {
        std::cout << "New best score is " << bestScore << std::endl;
      }
    }
  }

  // Move the offset iterator forward to prepare for the next time
  offsetsListIterator++;

  /*
   * All the offsets in the list were visited meaning that a learning
   * phase finished. Check if
   */
  if (offsetsListIterator == offsetsList.end()) {
    offsetsListIterator = offsetsList.begin();
    round++;
  }

  // Check if its time to re-calculate the best offset
  if ((bestScore >= scoreMax) || (round >= roundMax)) {
    round = 0;

    /*
     * If the current best score (bestScore) has exceed the threshold to
     * enable prefetching (badScore), reset the learning structures and
     * enable prefetch generation
     */
    if (bestScore > badScore) {
      bestOffset = phaseBestOffset;
      if constexpr (champsim::bop_debug) 
      {
        std::cout << "New best offset is " << bestOffset << std::endl;
      }
      round = 0;
      issuePrefetchRequests = true;
    } else {
      issuePrefetchRequests = false;
    }
    resetScores();
    bestScore = 0;
    phaseBestOffset = 0;
  }
}

std::optional<uint64_t> BOP::calculatePrefetchAddr(uint64_t addr)
{
  uint64_t pf_addr = addr + (bestOffset << LOG2_BLOCK_SIZE);

  if ((addr >> LOG2_PAGE_SIZE) != (pf_addr >> LOG2_PAGE_SIZE))
  {
    if constexpr (champsim::bop_debug) 
    {
      std::cout << "Prefetch not issued - Page crossed" << std::endl;
    }
    return std::nullopt;
  } 

  if constexpr (champsim::bop_debug) {
    std::cout << "Generated prefetch: " << pf_addr << std::endl;
  }
  return pf_addr;
}

void BOP::insertFill(uint64_t addr, uint8_t prefetch, uint32_t metadata_in)
{
  if (issuePrefetchRequests && prefetch && metadata_in == 2) {
    uint64_t base_address = addr - (bestOffset << LOG2_BLOCK_SIZE);

    if ((base_address >> LOG2_PAGE_SIZE) != (addr >> LOG2_PAGE_SIZE))
    {
      if constexpr (champsim::bop_debug) 
      {
        std::cout << "Filled address not inserted in RR - Crossed Page" << std::endl;
      }
      return;
    }
    uint64_t tag_base = tag(base_address);
    insertIntoRR(addr, tag_base);
  } else if (!prefetch && !issuePrefetchRequests) {
    /*
     * We insert the fetched line into the RR table when prefetch is off, 
     * (i.e. D = 0)
    */
    insertIntoRR(addr, tag(addr));
  }
  return;
}

void CACHE::prefetcher_initialize() 
{ 
  bop = new BOP();
  std::cout << "BOP Prefetcher Initialise" << std::endl; 
}

// addr: address of cache block
// ip: PC of load/store 
// cache_hit: true if load/store hits in cache.
uint32_t CACHE::prefetcher_cache_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, bool useful_prefetch, uint8_t type, uint32_t metadata_in)
{
  if (type != champsim::to_underlying(access_type::LOAD)) {
    return metadata_in; // Not a load
  }

  if ((cache_hit && useful_prefetch) || !cache_hit) {
    /*
    * Go through the nth offset and update the score, the best score and the
    * current best offset if a better one is found
   */
    bop->bestOffsetLearning(addr);

    if (bop->issuePrefetchRequests) {
      auto pf_addr = bop->calculatePrefetchAddr(addr);
      if (pf_addr.has_value()) {
        prefetch_line(*pf_addr, true, 2);
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
  bop->insertFill(addr, prefetch, metadata_in);

  return metadata_in;
}

void CACHE::prefetcher_cycle_operate() {}

void CACHE::prefetcher_final_stats() {}
