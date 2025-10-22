#ifndef TRACE_INTERFACE_H
#define TRACE_INTERFACE_H

#include <cstdint>
#include <vector>
#include <cstdio>

// Branch types encoded per user specification
enum class BR_TYPE : int8_t {
  NOT_BR          = 0,
  COND_DIRECT     = 1,
  COND_INDIRECT   = 2,
  UNCOND_DIRECT   = 3,
  UNCOND_INDIRECT = 4,
  CALL            = 5,
  RET             = 6,
};

// Packed trace element representing one branch outcome
struct HistElt {
  uint64_t pc;
  uint64_t target;
  uint8_t  direction; // 1 if taken, 0 otherwise
  BR_TYPE  type;
} __attribute__((packed));

#ifdef __cplusplus
extern "C" {
#endif

// Optional helper for reading trace (not used by tracer writer but provided for completeness)
FILE* open_trace(char* input_trace);
std::vector<HistElt> read_trace(FILE* input_trace, size_t chunk_size);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // TRACE_INTERFACE_H
