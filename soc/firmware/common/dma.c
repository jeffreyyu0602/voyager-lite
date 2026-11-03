#include "dma.h"

#include "encoding.h"

uint64_t dma_pushes;
uint64_t dma_push_cycles;

void dma_push(int queue, uint64_t src, uint64_t dst, uint32_t inner,
              uint32_t ctrl, const dma_level_t levels[DMA_LEVELS]) {
  const uint64_t start = read_csr(mcycle);
  /* A register field takes a write only when every byte of it is written. */
  reg_write64(DMA_SRC, src);
  reg_write64(DMA_DST, dst);
  reg_write32(DMA_INNER, inner);
  reg_write16(DMA_CTRL, (uint16_t)ctrl);
  for (int k = 0; k < DMA_LEVELS; k++) {
    reg_write32(DMA_COUNT(k), levels[k].count);
    reg_write64(DMA_SRC_STRIDE(k), (uint64_t)levels[k].src_stride);
    reg_write64(DMA_DST_STRIDE(k), (uint64_t)levels[k].dst_stride);
  }
  reg_write8(DMA_PUSH, (uint8_t)queue);
  dma_pushes++;
  dma_push_cycles += read_csr(mcycle) - start;
}
