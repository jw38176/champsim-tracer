#ifndef __MEM_CACHE_PREFETCH_BOP_HH__
#define __MEM_CACHE_PREFETCH_BOP_HH__

#include <algorithm>
#include <iostream>
#include <queue>
#include <stdlib.h>
#include <vector>
#include <optional>

#include "bop_parameters.h"
#include "cache.h"
#include "msl/bits.h"

namespace bop_space
{
class BOP
{
private:
  /** Learning phase parameters */
  const unsigned int scoreMax;
  const unsigned int roundMax;
  const unsigned int badScore;
  /** Recent requests table parameteres */
  const unsigned int rrEntries;
  const unsigned int tagMask;

  std::vector<uint64_t> rrTable;

  /** Structure to save the offset and the score */
  typedef std::pair<int64_t, uint8_t> OffsetListEntry;
  std::vector<OffsetListEntry> offsetsList;

  /** Current best offset to issue prefetches */
  int64_t bestOffset;
  /** Current best offset found in the learning phase */
  int64_t phaseBestOffset;
  /** Current test offset index */
  std::vector<OffsetListEntry>::iterator offsetsListIterator;
  /** Max score found so far */
  unsigned int bestScore;
  /** Current round */
  unsigned int round;

  /** Generate a hash for the specified address to index the RR table
   *  @param addr: address to hash
   */
  unsigned int index(uint64_t addr) const;

  /** Insert the specified address into the RR table
   *  @param addr: address to insert
   *  @param addr_tag: The tag to insert in the RR table
   */
  void insertIntoRR(uint64_t addr, uint64_t addr_tag);

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

public:
  /** Hardware prefetcher enabled */
  bool issuePrefetchRequests;

  /** Learning phase of the BOP. Update the intermediate values of the
   * round and update the best offset if found
   * @param addr: full address used to compute X-O tag to determine
   *              offset efficacy.
   */
  void bestOffsetLearning(uint64_t addr);

  std::optional<uint64_t> calculatePrefetchAddr(uint64_t addr);

  void insertFill(uint64_t addr, uint8_t prefetch);

  BOP();
  ~BOP() = default;
}; // class bop

BOP* bop;

} // namespace bop_space

#endif /* __MEM_CACHE_PREFETCH_BOP_HH__ */
