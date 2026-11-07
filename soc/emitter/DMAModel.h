#pragma once

#include <cstdint>

#include "soc/firmware/common/dma.h"

// One copy of the wrapper's DMA engine, as the firmware stages it (dma.h).
struct DMADescriptor {
  uint64_t src = 0;
  uint64_t dst = 0;
  uint32_t inner = 0;
  uint32_t ctrl = 0;
  dma_level_t levels[DMA_LEVELS] = {{1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
};

// The source address of a fill copy's moves: it reads nothing.
constexpr uint64_t kDMAFill = ~uint64_t{0};

// Calls move(src, dst, nibbles) for every range `copy` moves, in the order the
// engine moves them (dma.h states the semantics), with addresses and lengths in
// nibbles (a byte address times 2, its low nibble first): one call per run, or
// per element of a transposed copy; src is kDMAFill for a fill.
template <typename Move>
void for_each_dma_move(const DMADescriptor& copy, Move move) {
  const dma_level_t* level = copy.levels;
  const bool fill = (copy.ctrl & DMA_FILL) != 0;
  const bool transposed = (copy.ctrl & DMA_TRANSPOSED) != 0;
  const uint32_t elem =
      (copy.ctrl & DMA_NIBBLES) != 0 ? 1u : 2u << ((copy.ctrl >> 8) & 3);
  for (uint32_t i2 = 0; i2 < level[2].count; i2++) {
    for (uint32_t i1 = 0; i1 < level[1].count; i1++) {
      const uint64_t src =
          copy.src + i2 * level[2].src_stride + i1 * level[1].src_stride;
      const uint64_t dst =
          copy.dst + i2 * level[2].dst_stride + i1 * level[1].dst_stride;
      for (uint32_t i0 = 0; i0 < level[0].count; i0++) {
        if (!transposed) {
          move(fill ? kDMAFill : 2 * (src + i0 * level[0].src_stride),
               2 * (dst + i0 * level[0].dst_stride), 2 * copy.inner);
          continue;
        }
        for (uint32_t c = 0; c < 2 * copy.inner / elem; c++) {
          move(2 * (src + i0 * level[0].src_stride) + c * elem,
               2 * (dst + c * level[0].dst_stride) + i0 * elem, elem);
        }
      }
    }
  }
}
