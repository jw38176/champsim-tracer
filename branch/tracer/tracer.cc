#include <map>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdlib>
#include <cstdio>
#include <fstream>

#include "msl/fwcounter.h"
#include "ooo_cpu.h"
#include "trace_interface.h"

// Based on the bimodal predictor

namespace
{
constexpr std::size_t BIMODAL_TABLE_SIZE = 16384;
constexpr std::size_t BIMODAL_PRIME = 16381;
constexpr std::size_t COUNTER_BITS = 2;

std::map<O3_CPU*, std::array<champsim::msl::fwcounter<COUNTER_BITS>, BIMODAL_TABLE_SIZE>> bimodal_table;
constexpr std::size_t TRACE_BUFFER_SIZE = 1 << 20; // flush every ~1M branches

static FILE* trace_pipe = nullptr;
static std::vector<HistElt> trace_buffer;

static uint64_t warmup_instr_limit = 0;
static uint64_t warmup_branches = 0;
static uint64_t simulation_branches = 0;
// path for counts file
static std::string count_output_path;

void flush_trace()
{
  if (!trace_pipe || trace_buffer.empty())
    return;

  std::size_t written = fwrite(trace_buffer.data(), sizeof(HistElt), trace_buffer.size(), trace_pipe);
  if (written != trace_buffer.size())
    std::perror("Failed to write branch trace");
  trace_buffer.clear();
  fflush(trace_pipe);
}

void write_counts()
{
  if (count_output_path.empty())
    return;
  std::ofstream ofs(count_output_path);
  if (!ofs) return;
  ofs << "warmup_branches " << warmup_branches << "\n";
  ofs << "simulation_branches " << simulation_branches << "\n";
}

struct AtExitFlusher {
  AtExitFlusher() { std::atexit([]() { flush_trace(); if (trace_pipe) pclose(trace_pipe); }); }
};
static AtExitFlusher _trace_at_exit;

struct CountFileFlusher {
  CountFileFlusher() { std::atexit([]() { flush_trace(); write_counts(); if (trace_pipe) pclose(trace_pipe); }); }
};
static CountFileFlusher _count_at_exit;
} // namespace

void O3_CPU::initialize_branch_predictor()
{
  if (!::trace_pipe) {
    const char* env_name = std::getenv("BRANCH_TRACE_FILE");
    std::string filename = env_name ? env_name : std::string("branch_trace.bz2");

    char cmd[4096];
    std::snprintf(cmd, sizeof(cmd), "bzip2 > %s", filename.c_str());
    ::trace_pipe = popen(cmd, "w");

    // initialize counts output
    const char* count_path_env = std::getenv("BRANCH_COUNT_FILE");
    if (count_path_env)
      ::count_output_path = count_path_env;

    const char* warmup_env = std::getenv("WARMUP_INSTR");
    if (warmup_env)
      ::warmup_instr_limit = std::strtoull(warmup_env, nullptr, 10);
  }
}

uint8_t O3_CPU::predict_branch(uint64_t ip)
{
  auto hash = ip % ::BIMODAL_PRIME;
  auto value = ::bimodal_table[this][hash];

  return value.value() >= (value.maximum / 2);
}

void O3_CPU::last_branch_result(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  auto hash = ip % ::BIMODAL_PRIME;
  ::bimodal_table[this][hash] += taken ? 1 : -1;

  HistElt elt{ip, branch_target, taken, static_cast<BR_TYPE>(branch_type)};
  ::trace_buffer.push_back(elt);
  if (::trace_buffer.size() >= ::TRACE_BUFFER_SIZE)
    ::flush_trace();

  // Count branches by phase based on retired instruction count
  if (warmup_instr_limit == 0 || this->num_retired < warmup_instr_limit)
    ++::warmup_branches;
  else
    ++::simulation_branches;
}
