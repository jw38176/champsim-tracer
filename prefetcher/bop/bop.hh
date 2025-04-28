#ifndef __MEM_CACHE_PREFETCH_BOP_HH__
#define __MEM_CACHE_PREFETCH_BOP_HH__

#include <algorithm>
#include <iostream>
#include <queue>
#include <stdlib.h>
#include <vector>

#include "bop_parameters.h"
#include "cache.h"
#include "msl/bits.h"

namespace bop_space
{
class BOP
{
private:
  enum RRWay { Left, Right };

  /** Learning phase parameters */
  const unsigned int scoreMax;
  const unsigned int roundMax;
  const unsigned int badScore;
  /** Recent requests table parameteres */
  const unsigned int rrEntries;
  const unsigned int tagMask;
  /** Delay queue parameters */
  // const bool         delayQueueEnabled;
  // const unsigned int delayQueueSize;
  // const unsigned int delayTicks;

  std::vector<uint64_t> rrLeft;
  std::vector<uint64_t> rrRight;

  /** Structure to save the offset and the score */
  typedef std::pair<int16_t, uint8_t> OffsetListEntry;
  std::vector<OffsetListEntry> offsetsList;

  /** In a first implementation of the BO prefetcher, both banks of the
   *  RR were written simultaneously when a prefetched line is inserted
   *  into the cache. Adding the delay queue tries to avoid always
   *  striving for timeless prefetches, which has been found to not
   *  always being optimal.
   */
  // struct DelayQueueEntry
  // {
  //     uint64_t baseAddr;
  //     uint64_t processTick;

  //     DelayQueueEntry(uint64_t x, uint64_t t) : baseAddr(x), processTick(t)
  //     {}
  // };

  // std::deque<DelayQueueEntry> delayQueue;

  /** Event to handle the delay queue processing */
  // void delayQueueEventWrapper();
  // EventFunctionWrapper delayQueueEvent;

  /** Current best offset to issue prefetches */
  uint64_t bestOffset;
  /** Current best offset found in the learning phase */
  uint64_t phaseBestOffset;
  /** Current test offset index */
  std::vector<OffsetListEntry>::iterator offsetsListIterator;
  /** Max score found so far */
  unsigned int bestScore;
  /** Current round */
  unsigned int round;

  /** Generate a hash for the specified address to index the RR table
   *  @param addr: address to hash
   *  @param way:  RR table to which is addressed (left/right)
   */
  unsigned int index(uint64_t addr, unsigned int way) const;

  /** Insert the specified address into the RR table
   *  @param addr: address to insert
   *  @param addr_tag: The tag to insert in the RR table
   *  @param way: RR table to which the address will be inserted
   */
  void insertIntoRR(uint64_t addr, uint64_t addr_tag, unsigned int way);

  /** Insert the specified address into the delay queue. This will
   *  trigger an event after the delay cycles pass
   *  @param addr: address to insert into the delay queue
   */
  void insertIntoDelayQueue(uint64_t addr);

  /** Reset all the scores from the offset list */
  void resetScores();

  /** Generate the tag for the specified address based on the tag bits
   *  and the block size
   *  @param addr: address to get the tag from
   */
  uint64_t tag(uint64_t addr) const;

  /** Test if @X-O is hitting in the RR table to update the
   *  offset score
   *  @param addr_tag: tag searched for within the RR
   */
  bool testRR(uint64_t addr_tag) const;

  /** Update the RR right table after a prefetch fill */
  // void notifyFill(const CacheAccessProbeArg &arg) override;

public:
  /** The prefetch degree, i.e. the number of prefetches to generate */
  unsigned int degree;

  /** Hardware prefetcher enabled */
  bool issuePrefetchRequests;

  /** Learning phase of the BOP. Update the intermediate values of the
   * round and update the best offset if found
   * @param addr: full address used to compute X-O tag to determine
   *              offset efficacy.
   */
  void bestOffsetLearning(uint64_t addr);

  uint64_t calculatePrefetchAddr(uint64_t addr);

  void insertFill(uint64_t addr);

  BOP();
  ~BOP() = default;

}; // class bop

BOP* bop;

} // namespace bop_space

#endif /* __MEM_CACHE_PREFETCH_BOP_HH__ */
