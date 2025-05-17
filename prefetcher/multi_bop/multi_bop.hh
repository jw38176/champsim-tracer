#ifndef __MEM_CACHE_PREFETCH_MULTI_BOP_HH__
#define __MEM_CACHE_PREFETCH_MULTI_BOP_HH__

#include <algorithm>
#include <iostream>
#include <queue>
#include <stdlib.h>
#include <vector>
#include <deque>
#include <optional>
#include <cstdint>
#include <unordered_set>

#include "multi_bop_parameters.h"
#include "cache.h"
#include "msl/bits.h"
#include "msl/lru_table.h"

namespace multi_bop_space
{ 
class PrefetchTable {
public:
  struct Entry {
    uint64_t addr;
    uint64_t offset;
  };

  PrefetchTable(std::size_t table_max_size);

  void insert(const Entry& entry);
  std::optional<Entry> lookup(uint64_t addr) const;
  void remove(uint64_t addr);

private:
  std::deque<Entry> table;
  std::size_t max_size;
};

class MULTI_BOP
{
private:
  /** Learning phase parameters */
  const unsigned int scoreMax;
  const unsigned int roundMax;
  /** Recent requests table parameteres */
  const unsigned int rrEntries;
  const unsigned int tagMask;

  std::vector<uint64_t> rrTable;

  /** Structure to save the offset and the score */
  typedef std::pair<int16_t, uint8_t> OffsetListEntry;
  std::vector<OffsetListEntry> offsetsList;

  std::array<uint64_t, NUM_OFFSETS> learned_offsets = {1};
  unsigned int current_learning_offset_idx = 0;

  std::unordered_set<uint64_t> suppressed_offsets;

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
  /** Total number of pf issued */
  unsigned int pf_issued_multi_bop = 0;

  /** Total number of useful pf */
  unsigned int pf_useful_multi_bop = 0;

  PrefetchTable prefetch_table;

  std::unordered_map<uint64_t, uint64_t> offset_issued;
  std::unordered_map<uint64_t, uint64_t> offset_useful;
  std::unordered_map<uint64_t, std::vector<double>> offset_accuracy_log;

  void recordAccuracy();

  /** Learning phase of the BOP. Update the intermediate values of the
   * round and update the best offset if found
   * @param addr: full address used to compute X-O tag to determine
   *              offset efficacy.
   */
  void bestOffsetLearning(uint64_t addr);

  uint64_t calculatePrefetchAddr(uint64_t addr);

  std::vector<std::pair<uint64_t, uint64_t>> calculatePrefetchAddrs(uint64_t addr);

  void insertFill(uint64_t addr);

  MULTI_BOP();
  ~MULTI_BOP() = default;
}; // class MULTI_BOP

MULTI_BOP* multi_bop;

} // namespace multi_bop_space

#endif /* __MEM_CACHE_PREFETCH_MULTI_BOP_HH__ */
