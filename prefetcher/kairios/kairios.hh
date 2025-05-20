#ifndef __MEM_CACHE_PREFETCH_KAIRIOS_HH__
#define __MEM_CACHE_PREFETCH_KAIRIOS_HH__

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iostream>
#include <optional>
#include <queue>
#include <stdlib.h>
#include <vector>

#include "cache.h"
#include "kairios_parameters.h"
#include "msl/bits.h"
#include "msl/lru_table.h"
#include <unordered_set>

namespace kairios_space
{

class RRTable
{
public:
  struct Entry {
    uint64_t addr = 0;
    uint64_t pc = 0;
  };

  RRTable(std::size_t size);

  void insert(uint64_t addr, uint64_t pc);
  Entry lookup(uint64_t addr) const;
  bool test(uint64_t addr) const;

private:
  std::size_t log_size;
  std::vector<Entry> table;

  std::size_t index(uint64_t addr) const;
};


class HoldingTable
{
public:
  struct Entry {
    uint64_t base_addr = 0;
    uint64_t pc = 0;
  };

  explicit HoldingTable(std::size_t size);

  void insert(uint64_t addr, uint64_t base_addr, uint64_t pc);

  std::optional<Entry> lookup(uint64_t addr) const;

private:
  std::vector<Entry> entries;
  uint64_t log_size;

  uint64_t index(uint64_t addr) const;
};


class AccuracyTable
{
public:
  explicit AccuracyTable(std::size_t size);

  int16_t lookup(uint64_t pc, int offset_idx) const;

  void increment(uint64_t pc, int offset_idx);

  void decrement(uint64_t pc, int offset_idx);

  void resetOffsetStats(int offset_idx);

private:
  std::vector<std::vector<int16_t>> table;

  std::size_t table_size;

  int getIndex(uint64_t pc) const;

  static constexpr int16_t ACC_MIN = -16;
  static constexpr int16_t ACC_MAX = 15;
};


class EvictionTable {
public:
  explicit EvictionTable(std::size_t size);

  void insert(uint64_t addr);
  bool test(uint64_t addr) const;
  void clear();

private:
  std::vector<uint64_t> table;
  std::size_t table_size;
  std::size_t next_index;
};


class KAIRIOS
{
private:
  /** Learning phase parameters */
  const unsigned int scoreMax;
  const unsigned int roundMax;

  /** Structure to save the offset and the score */
  typedef std::pair<int16_t, uint8_t> OffsetListEntry;
  std::vector<OffsetListEntry> offsetsList;

  std::array<uint64_t, NUM_OFFSETS> learned_offsets = {0};
  unsigned int current_learning_offset_idx = 0;

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

  /** Reset all the scores from the offset list */
  void resetScores();
public:
  /** Total number of pf issued */
  unsigned int pf_issued_kairios = 0;

  /** Total number of useful pf */
  unsigned int pf_useful_kairios = 0;

  RRTable rr_table;

  HoldingTable holding_table;

  AccuracyTable accuracy_table;

  EvictionTable eviction_table;

  /** Learning phase of KAIRIOS. Update the intermediate values of the
   * round and update the best offset if found
   * @param addr: full address used to compute X-O tag to determine
   *              offset efficacy.
   */
  void bestOffsetLearning(uint64_t addr, uint8_t cache_hit);

  std::vector<uint64_t> calculateAccuratePrefetchAddrs(uint64_t addr, uint64_t pc);

  std::vector<uint64_t> calculateAllPrefetchAddrs(uint64_t addr);

  void insertFill(uint64_t addr);

  void accuracy_train(uint64_t addr, uint64_t pc);

  KAIRIOS();
  ~KAIRIOS() = default;
}; // class KAIRIOS

KAIRIOS* kairios;

} // namespace kairios_space

#endif /* __MEM_CACHE_PREFETCH_KAIRIOS_HH__ */

