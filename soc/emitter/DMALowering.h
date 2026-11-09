#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "runtime/GraphUtils.h"
#include "soc/emitter/DMAModel.h"
#include "soc/emitter/HostRequests.h"

// The DMA lowering of a copy or a zero fill for a DRAM config's firmware
// (HW_DMA): one descriptor of the wrapper's DMA engine (dma.h) whose source
// and destination are affine in the prim's run-time scalars, and whose shape
// does not depend on them.
//
// The descriptor is fitted to the bytes run_async_copy / zero_buffer move under
// the emission environment, then each scalar is probed: the moves under a
// perturbed value must be the descriptor's with only the addresses moved, by a
// whole number of bytes per unit of the scalar. Every probed environment, and
// one with every scalar perturbed at once, is checked byte for byte against
// the executable spec. A prim that fails any of this stays on the mailbox, and
// `reason` says why.
struct DMALowering {
  bool lowered = false;
  std::string reason;
  int queue = DMA_LOAD;
  // The descriptor under the emission environment.
  DMADescriptor descriptor;
  // Bytes the source and destination move per unit of each scalar, and the
  // scalar's value in the emission environment.
  std::map<std::string, int64_t> src_coefficients;
  std::map<std::string, int64_t> dst_coefficients;
  std::map<std::string, int64_t> base_values;
};

// One range a copy or fill moves, in nibbles: absolute (CPU) byte addresses
// times 2, the low nibble first; src is kDMAFill for a fill.
struct DMAMove {
  uint64_t src;
  uint64_t dst;
  uint64_t nibbles;
};

// The moves of `host` under `env`, in the order the spec makes them. Throws
// when the prim does something a descriptor cannot (converts a type, fills a
// pad value, touches the reference partition).
std::vector<DMAMove> record_dma_moves(const HostOp& host, const ScalarEnv& env);

// Lowers `host` under the emission environment `env`. `step_of(name)` is the
// scalar's own advance when the emitter knows it (0 otherwise); probes use it
// before searching for an accepted delta.
DMALowering lower_to_dma(
    const HostOp& host, const ScalarEnv& env,
    const std::function<int64_t(const std::string&)>& step_of);
