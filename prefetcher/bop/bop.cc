#include "bop.hh"

#include <algorithm>

using namespace bop_space;

BOP::BOP()
    : scoreMax(SCORE_MAX), roundMax(ROUND_MAX), badScore(BAD_SCORE), rrEntries(RR_SIZE), tagMask((1 << TAG_BITS) - 1),
      // delayQueueEnabled(DELAY_QUEUE_ENABLE),
      // delayQueueSize(DELAY_QUEUE_SIZE),
      // delayTicks(cyclesToTicks(p.delay_queue_cycles)),
      // delayQueueEvent([this]{ delayQueueEventWrapper(); }, name()),
      issuePrefetchRequests(true), bestOffset(1), phaseBestOffset(0), bestScore(0), round(0), degree(1)
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

  rrLeft.resize(rrEntries);
  rrRight.resize(rrEntries);

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

// void
// BOP::delayQueueEventWrapper()
// {
//     while (!delayQueue.empty() &&
//             delayQueue.front().processTick <= curTick())
//     {
//         Addr addr_x = delayQueue.front().baseAddr;
//         Addr addr_tag = tag(addr_x);
//         insertIntoRR(addr_x, addr_tag, RRWay::Left);
//         delayQueue.pop_front();
//     }

//     // Schedule an event for the next element if there is one
//     if (!delayQueue.empty()) {
//         schedule(delayQueueEvent, delayQueue.front().processTick);
//     }
// }

unsigned int BOP::index(uint64_t addr, unsigned int way) const
{
  /*
   * The second parameter, way, is set to 0 for indexing the left side of the
   * RR Table and, it is set to 1 for indexing the right side of the RR
   * Table. This is because we always pass the enum RRWay as the way argument
   * while calling index. This enum is defined in the bop.hh file.
   *
   * The indexing function in the author's ChampSim code, which can be found
   * here: https://comparch-conf.gatech.edu/dpc2/final_program.html, computes
   * the hash as follows:
   *
   *  1. For indexing the left side of the RR Table (way = 0), the cache line
   *     address is XORed with itself after right shifting it by the log base
   *     2 of the number of entries in the RR Table.
   *  2. For indexing the right side of the RR Table (way = 1), the cache
   *     line address is XORed with itself after right shifting it by the log
   *     base 2 of the number of entries in the RR Table, multiplied by two.
   *
   * Therefore, we if just left shift the log base 2 of the number of RR
   * entries (log_rr_entries) with the parameter way, then if we are indexing
   * the left side, we'll leave log_rr_entries as it is, but if we are
   * indexing the right side, we'll multiply it with 2. Now once we have the
   * result of this operation, we can right shift the cache line address
   * (line_addr) by this value to get the first operand of the final XOR
   * operation. The second operand of the XOR operation is line_addr itself
   */
  uint64_t log_rr_entries = champsim::lg2(rrEntries);
  uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
  uint64_t hash = line_addr ^ (line_addr >> (log_rr_entries << way));
  hash &= ((1ULL << log_rr_entries) - 1);
  return hash % rrEntries;
}

void BOP::insertIntoRR(uint64_t addr, uint64_t tag, unsigned int way)
{
  switch (way) {
  case RRWay::Left:
    rrLeft[index(addr, RRWay::Left)] = tag;
    break;
  case RRWay::Right:
    rrRight[index(addr, RRWay::Right)] = tag;
    break;
  }
}

// void
// BOP::insertIntoDelayQueue(uint64_t x)
// {
//     if (delayQueue.size() == delayQueueSize) {
//         return;
//     }

//     /*
//     * Add the address to the delay queue and schedule an event to process
//     * it after the specified delay cycles
//     */
//     Tick process_tick = curTick() + delayTicks;

//     delayQueue.push_back(DelayQueueEntry(x, process_tick));

//     if (!delayQueueEvent.scheduled()) {
//         schedule(delayQueueEvent, process_tick);
//     }
// }

void BOP::resetScores()
{
  for (auto& it : offsetsList) {
    it.second = 0;
  }
}

inline uint64_t BOP::tag(uint64_t addr) const { return (addr >> LOG2_BLOCK_SIZE) & tagMask; }

bool BOP::testRR(uint64_t addr_tag) const
{
  for (auto& it : rrLeft) {
    if (it == addr_tag) {
      return true;
    }
  }

  for (auto& it : rrRight) {
    if (it == addr_tag) {
      return true;
    }
  }

  return false;
}

void BOP::bestOffsetLearning(uint64_t addr)
{
  uint64_t offset_tag = (*offsetsListIterator).first;

  /*
   * Compute the lookup tag for the RR table. As tags are generated using
   * lower 12 bits we subtract offset from the full address rather than the
   * tag to avoid integer underflow.
   */
  uint64_t lookup_tag = tag((addr) - (offset_tag << LOG2_BLOCK_SIZE));

  // There was a hit in the RR table, increment the score for this offset
  if (testRR(lookup_tag)) {
    if constexpr (champsim::bop_debug) 
    {
      std::cout << "Address " << lookup_tag << " found in the RR table" << std::endl;
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

uint64_t BOP::calculatePrefetchAddr(uint64_t addr)
{
  return addr + (bestOffset << LOG2_BLOCK_SIZE);
}

void BOP::insertFill(uint64_t addr)
{
  uint64_t tag_y = tag(addr);

  if (issuePrefetchRequests) {
    insertIntoRR(addr, tag_y - bestOffset, RRWay::Right);
    std::cout << "Filled RR" << std::endl;
  }
}

void CACHE::prefetcher_initialize() 
{ 
  bop = new BOP();
  std::cout << "BOP Prefetcher Initialise" << std::endl; 
}

// addr: address of cache block
// ip: PC of load/store cache_hit: true if load/store hits in cache.
// type : 0 for load
uint32_t CACHE::prefetcher_cache_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, bool useful_prefetch, uint8_t type, uint32_t metadata_in)
{
  if (type != 0) {
    return metadata_in; // access is not a load
  }

  if ((cache_hit && useful_prefetch) || !cache_hit) {
    /*
    * Go through the nth offset and update the score, the best score and the
    * current best offset if a better one is found
   */
    bop->bestOffsetLearning(addr);

  // if (delayQueueEnabled) {
  //   insertIntoDelayQueue(addr);
  // } else {
  //   insertIntoRR(addr, tag_x, RRWay::Left);
  // }

    if (bop->issuePrefetchRequests) {
      uint64_t pf_addr = bop->calculatePrefetchAddr(addr);
      prefetch_line(pf_addr, true, metadata_in);
      if constexpr (champsim::bop_debug) 
      {
        std::cout << "Generated Prefetch " << pf_addr << std::endl;
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
  // Only insert into the RR right way if fill is a hardware prefetch
  if (prefetch){
    bop->insertFill(addr);
  }

  return metadata_in;
}

void CACHE::prefetcher_cycle_operate() {}

void CACHE::prefetcher_final_stats() {}
