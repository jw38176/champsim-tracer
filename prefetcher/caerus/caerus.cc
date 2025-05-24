#include "caerus.hh"

#include <algorithm>

using namespace caerus_space;

RRTable::RRTable(std::size_t size) : log_size(champsim::lg2(size)) { table.resize(size); }

std::size_t RRTable::index(uint64_t addr) const
{
  uint64_t line_addr = addr >> LOG2_BLOCK_SIZE;
  uint64_t hash = line_addr ^ (line_addr >> log_size);
  hash &= ((1ULL << log_size) - 1);
  return hash;
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

std::optional<HoldingTable::Entry> HoldingTable::lookup(uint64_t addr) const
{
  auto idx = index(addr);
  const Entry& e = entries[idx];
  if (e.base_addr != 0) 
    return e;
  return std::nullopt;
}

uint64_t HoldingTable::index(uint64_t addr) const
{
  uint64_t hash = addr ^ (addr >> log_size);
  hash &= ((1ULL << log_size) - 1);
  return hash;
}

AccuracyTable::AccuracyTable(std::size_t size) : table_size(size) { table.resize(table_size, std::vector<int16_t>(NUM_OFFSETS)); }

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
      table[idx][offset_idx]++;
    if constexpr (champsim::caerus_dbug) {
      std::cout << "Offset score incremented to: " << table[idx][offset_idx] << std::endl;
    }
  }
}

void AccuracyTable::decrement(uint64_t pc, int offset_idx)
{
  int idx = getIndex(pc);
  if (offset_idx >= 0 && offset_idx < NUM_OFFSETS) {
    if (table[idx][offset_idx] > ACC_MIN)
      table[idx][offset_idx]--;
    if constexpr (champsim::caerus_dbug) {
      std::cout << "Offset score decremented to: " << table[idx][offset_idx] << std::endl;
    }
  }
}

void AccuracyTable::resetOffsetStats(int offset_idx)
{
  if (offset_idx < 0 || offset_idx >= NUM_OFFSETS)
    return;

  for (std::size_t i = 0; i < table_size; ++i) {
    table[i][offset_idx] = 0;
  }
}

EvictionTable::EvictionTable(std::size_t size)
    : log_size(champsim::lg2(size))
{
  table.resize(size);
}

std::size_t EvictionTable::index(uint64_t line_addr) const
{
  uint64_t hash = line_addr ^ (line_addr >> log_size);
  hash &= ((1ULL << log_size) - 1);
  return hash;
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
      accuracy_table(ACCURACY_TABLE_SIZE), eviction_table(EVICTION_TABLE_SIZE)

{
  if (!champsim::msl::isPowerOf2(RR_SIZE)) {
    throw std::invalid_argument{"Number of RR entries is not power of 2\n"};
  }
  if (!champsim::msl::isPowerOf2(BLOCK_SIZE)) {
    throw std::invalid_argument{"Cache line size is not power of 2\n"};
  }

  if (!champsim::msl::isPowerOf2(HOLDING_TABLE_SIZE)) {
    throw std::invalid_argument{"Holding table size is not power of 2\n"};
  }

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
    }

    offset_i++;
  }

  offsetsListIterator = offsetsList.begin();
  if constexpr (champsim::caerus_dbug) {
    std::cout << "Offsets List:\n";
    for (const auto& entry : offsetsList) {
      std::cout << "Offset: " << entry.first << ", Metadata: " << static_cast<int>(entry.second) << '\n';
    }
  }
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

      uint64_t prev_pf_addr = addr - (off << LOG2_BLOCK_SIZE);
      if (rr_table.test(prev_pf_addr) and accuracy_table.lookup(rr_table.lookup(prev_pf_addr).pc, i) > ACCURACY_THRESHOLD) {
        return; // Already covered by another learned offset
      }
    }
  }

  uint64_t offset = (*offsetsListIterator).first;

  uint64_t test_addr = addr - (offset << LOG2_BLOCK_SIZE);

  // Score offset if demand addr in RR
  if (rr_table.test(test_addr)) {
    if constexpr (champsim::caerus_dbug) {
      std::cout << "Address " << test_addr << " found in RR table" << std::endl;
    }
    (*offsetsListIterator).second++;

    if ((*offsetsListIterator).second > bestScore) {
      bestScore = (*offsetsListIterator).second;
      phaseBestOffset = offset;
      if constexpr (champsim::caerus_dbug) {
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
    if constexpr (champsim::caerus_dbug) {
      std::cout << "Learned new offset #" << current_learning_offset_idx << ": " << phaseBestOffset << std::endl;
    }

    // Reset statistics for this offset
    accuracy_table.resetOffsetStats(current_learning_offset_idx);

    // Move to next learning slot (wrap 0–3)
    current_learning_offset_idx = (current_learning_offset_idx + 1) % learned_offsets.size();

    // Reset learning phase
    round = 0;
    bestScore = 0;
    phaseBestOffset = 0;
    resetScores();
  }
}

std::vector<uint64_t> CAERUS::calculateAccuratePrefetchAddrs(uint64_t addr, uint64_t pc)
{
  std::vector<uint64_t> pf_addrs;

  for (uint64_t i = 0; i < learned_offsets.size(); ++i) {
    auto offset = learned_offsets[i];

    if (offset == 0)
      continue;

    if (accuracy_table.lookup(pc, i) < ACCURACY_THRESHOLD) {
      continue;
    }

    uint64_t pf_addr = addr + (offset << LOG2_BLOCK_SIZE);

    // if ((addr >> LOG2_PAGE_SIZE) != (pf_addr >> LOG2_PAGE_SIZE)) {
    //   if constexpr (champsim::caerus_dbug) {
    //     std::cout << "Prefetch not issued - Page crossed" << std::endl;
    //   }
    //   continue;
    // }

    pf_addrs.push_back(pf_addr);

    if constexpr (champsim::caerus_dbug) {
      std::cout << "Generated accurate prefetch: " << pf_addr << " with offset " << offset << std::endl;
    }
  }

  return pf_addrs;
}

std::vector<uint64_t> CAERUS::calculateAllPrefetchAddrs(uint64_t line_addr)
{
  std::vector<uint64_t> pf_addrs;

  for (auto offset : learned_offsets) {
    if (offset == 0)
      continue;

    uint64_t pf_addr = (line_addr + offset) << LOG2_BLOCK_SIZE; // shift to get full addr bits

    // if ((addr >> LOG2_PAGE_SIZE) != (pf_addr >> LOG2_PAGE_SIZE)) {
    //   if constexpr (champsim::caerus_dbug) {
    //     std::cout << "Prefetch not issued - Page crossed" << std::endl;
    //   }
    //   continue;
    // }

    pf_addrs.push_back(pf_addr);
  }

  return pf_addrs;
}

void CAERUS::accuracy_train(uint64_t line_addr, uint64_t pc)
{
  if (pc == 0)
    return;

  std::vector<uint64_t> pf_addrs = calculateAllPrefetchAddrs(line_addr);

  if constexpr (champsim::caerus_dbug) {
        std::cout << "ALL PF ADDR SIZE" << pf_addrs.size() << std::endl;
  }

  for (int i = 0; i < NUM_OFFSETS; ++i) {
    if (i >= static_cast<int>(pf_addrs.size())){
      break; // for safety
    }
    if (rr_table.test(pf_addrs[i]) || eviction_table.test(pf_addrs[i])) {
      accuracy_table.increment(pc, i);
      if constexpr (champsim::caerus_dbug) {
        std::cout << "INCREMENT" << std::endl;
      }
    } else {
      if constexpr (champsim::caerus_dbug) {
        std::cout << "DECREMENT" << std::endl;
      }
      accuracy_table.decrement(pc, i);
    }
  }

  eviction_table.insert(line_addr);
}

void CAERUS::insertFill(uint64_t addr)
{
  auto result = holding_table.lookup(addr);
  if (result.has_value()) {
    RRTable::Entry evicted_entry = rr_table.lookup(result->base_addr);
    accuracy_train(evicted_entry.line_addr, evicted_entry.pc);
    rr_table.insert(result->base_addr, result->pc);
  }
}

// if ((base_address >> LOG2_PAGE_SIZE) != (addr >> LOG2_PAGE_SIZE)) {
//   if constexpr (champsim::caerus_dbug) {
//     std::cout << "Filled address crossed page" << std::endl;
//   }
//   return;
// }

void CACHE::prefetcher_initialize()
{
  caerus = new CAERUS();
  std::cout << "CAERUS Prefetcher Initialise" << std::endl;
}

uint32_t CACHE::prefetcher_cache_operate(uint64_t addr, uint64_t ip, uint8_t cache_hit, bool useful_prefetch, uint8_t type, uint32_t metadata_in)
{
  if (type != champsim::to_underlying(access_type::LOAD)) {
    return metadata_in; // Not a load
  }

  if ((cache_hit && useful_prefetch) || !cache_hit) {
    // increment useful prefetch counter, not quite the same as cache stat since we don't remove the prefetch tag
    if (cache_hit && useful_prefetch) {
      caerus->pf_useful_caerus++;
    }

    auto pf_addrs = caerus->calculateAccuratePrefetchAddrs(addr, ip);

    if constexpr (champsim::caerus_dbug) {
        std::cout << "ACCURATE PF ADDR SIZE" << pf_addrs.size() << std::endl;
    }

    if (pf_addrs.size() > 0) {
      bool added_to_holding = false;
      for (auto pf_addr : pf_addrs) {
        bool issued = prefetch_line(pf_addr, true, metadata_in);
        if (issued) {
          if (!added_to_holding) {
            caerus->holding_table.insert(pf_addr, addr, ip);
            added_to_holding = true;
          }
          ++(caerus->pf_issued_caerus);
        } else {
          if constexpr (champsim::caerus_dbug) {
            std::vector<std::size_t> pq_occupancy = get_pq_occupancy();
            std::cout << "PQ FULL, pq_occupany: " << pq_occupancy[2] << std::endl; // hard-coded for L2 occupancy
          }
        }
      }
    } else if (cache_hit && useful_prefetch) { // Prefetch hit where no prefetches issued
      RRTable::Entry evicted_entry = caerus->rr_table.lookup(addr);
      caerus->accuracy_train(evicted_entry.line_addr, evicted_entry.pc);
      caerus->rr_table.insert(addr, ip);
    } else { // X is a cache miss with no PFs generated
      caerus->holding_table.insert(addr, addr, ip);
    }

    caerus->bestOffsetLearning(addr, cache_hit);
  }

  return metadata_in;
}

uint32_t CACHE::prefetcher_cache_fill(uint64_t addr, uint32_t set, uint32_t way, uint8_t prefetch, uint64_t evicted_addr, uint32_t metadata_in)
{
  caerus->insertFill(addr);

  return metadata_in;
}

void CACHE::prefetcher_cycle_operate() {}

void CACHE::prefetcher_final_stats()
{
  std::cout << "CAERUS ISSUED: " << caerus->pf_issued_caerus << std::endl;
  std::cout << "CAERUS USEFUL: " << caerus->pf_useful_caerus << std::endl;
}
