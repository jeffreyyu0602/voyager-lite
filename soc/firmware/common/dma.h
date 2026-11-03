/* The wrapper's DMA engine (a DRAM config's VoyagerDMA.scala).
 *
 * Two in-order queues: DMA_LOAD for copies into the scratchpad, DMA_STORE for
 * copies out of it. A copy is staged in the descriptor registers, then pushed
 * onto a queue; the staged registers keep their values, so a push writes only
 * what changes. A copy whose CTRL has DMA_POST adds 1 to its counter once every
 * write it made has been acknowledged.
 *
 * A copy moves runs of `inner` bytes, any alignment on either side, over up to
 * three outer levels (level 0 fastest): level k repeats count(k) times,
 * advancing the source by src_stride(k) and the destination by dst_stride(k)
 * bytes. A count of 0 moves nothing (the counter still advances). With
 * DMA_FILL the copy writes zeros and reads nothing. A transposed copy
 * (DMA_TRANSPOSED, elements of 1 << DMA_ELEM_LG2 bytes, or 4-bit with
 * DMA_NIBBLES) reads count(0) source rows of `inner` bytes, src_stride(0)
 * apart, and writes element column c as the destination row at dst + c *
 * dst_stride(0), element r of it from source row r; levels 1 and 2 repeat the
 * whole transpose. 4-bit elements sit two to a byte, the even one in the low
 * nibble, and count(0) must be even. */
#ifndef DMA_H
#define DMA_H

#include <stdint.h>

#include "voyager_address.h"

// clang-format off
#define DMA_SRC               (VOYAGER_MMIO_BASE_ADDRESS + 0x700) /* W64 */
#define DMA_DST               (VOYAGER_MMIO_BASE_ADDRESS + 0x708) /* W64 */
#define DMA_INNER             (VOYAGER_MMIO_BASE_ADDRESS + 0x710) /* W32 */
#define DMA_CTRL              (VOYAGER_MMIO_BASE_ADDRESS + 0x718) /* W16 */
#define DMA_COUNT(k)          (VOYAGER_MMIO_BASE_ADDRESS + 0x720 + 0x18 * (k))
#define DMA_SRC_STRIDE(k)     (VOYAGER_MMIO_BASE_ADDRESS + 0x728 + 0x18 * (k))
#define DMA_DST_STRIDE(k)     (VOYAGER_MMIO_BASE_ADDRESS + 0x730 + 0x18 * (k))
/* W8: pushes the staged copy onto queue DMA_LOAD or DMA_STORE; stalls while
 * the queue is full. */
#define DMA_PUSH              (VOYAGER_MMIO_BASE_ADDRESS + 0x768)
/* R8: bit q is set while queue q holds or runs a copy. */
#define DMA_BUSY              (VOYAGER_MMIO_BASE_ADDRESS + 0x770)
/* R64: cycles each queue has been busy. */
#define DMA_LOAD_BUSY_CYCLES  (VOYAGER_MMIO_BASE_ADDRESS + 0x778)
#define DMA_STORE_BUSY_CYCLES (VOYAGER_MMIO_BASE_ADDRESS + 0x780)
/* R64: cycles unit u sat idle with its next pass held on its counters. */
#define WAIT_STALL_CYCLES(u)  (VOYAGER_MMIO_BASE_ADDRESS + 0x788 + 8 * (u))
// clang-format on

#define DMA_LOAD 0
#define DMA_STORE 1
#define DMA_LEVELS 3

/* CTRL bits: [4:0] counter, [5] post, [6] fill, [7] transposed, [9:8] log2 of
 * a transposed element's bytes, [10] 4-bit transposed elements. */
#define DMA_POST (1u << 5)
#define DMA_FILL (1u << 6)
#define DMA_TRANSPOSED (1u << 7)
#define DMA_ELEM_LG2(lg) ((uint32_t)(lg) << 8)
#define DMA_NIBBLES (1u << 10)
#define DMA_COUNTER(ctr) ((uint32_t)(ctr) & 0x1f)

/* One outer level of a copy. */
typedef struct {
  uint32_t count;
  int64_t src_stride;
  int64_t dst_stride;
} dma_level_t;

/* --- firmware side (the testbench shares the definitions above) ---------- */
#ifndef __cplusplus
#include "mmio.h"

/* Stages a copy with every field written and pushes it onto `queue`. */
void dma_push(int queue, uint64_t src, uint64_t dst, uint32_t inner,
              uint32_t ctrl, const dma_level_t levels[DMA_LEVELS]);

/* The pushes so far, and the core cycles they took. */
extern uint64_t dma_pushes;
extern uint64_t dma_push_cycles;

/* Spins until the queues in `mask` (bit q for queue q) are empty and idle. */
static inline void dma_wait_idle(uint32_t mask) {
  while (reg_read8(DMA_BUSY) & mask) {
  }
}
#endif /* !__cplusplus */

#endif /* DMA_H */
