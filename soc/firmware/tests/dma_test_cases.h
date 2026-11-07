/* The DMA engine's directed test (dma_test.c), shared with the testbench
 * (DMATest.cc), which fills the two test regions with patterns, applies every
 * copy below to a model of them in issue order, and compares every byte of
 * both regions once the firmware finishes: wrong bytes and bytes written
 * outside a copy's destination both count. */
#ifndef DMA_TEST_CASES_H
#define DMA_TEST_CASES_H

#include <stdint.h>

#include "../common/dma.h"

/* The regions the test reads and writes: one in DRAM, one in the scratchpad
 * above the firmware's 256 KB. */
#define DT_DRAM (DRAM_BASE + 0x10000000ULL)
#define DT_DRAM_BYTES 0x300000ULL
#define DT_SRAM ((uint64_t)SRAM_BASE + 0x40000ULL)
#define DT_SRAM_BYTES 0x1A0000ULL

#define DT_D(offset) (DT_DRAM + (offset))
#define DT_S(offset) (DT_SRAM + (offset))
#define DT_ONE {1, 0, 0}

typedef struct {
  const char* name;
  int queue;
  uint64_t src;
  uint64_t dst;
  uint32_t inner;
  uint32_t ctrl;
  dma_level_t levels[DMA_LEVELS];
} dma_test_case_t;

#define DT_POST(ctr) (DMA_POST | DMA_COUNTER(ctr))

#define DT_TRANSPOSED(lg, ctr) \
  (DT_POST(ctr) | DMA_TRANSPOSED | DMA_ELEM_LG2(lg))
#define DT_TRANSPOSED4(ctr) (DT_POST(ctr) | DMA_TRANSPOSED | DMA_NIBBLES)

/* Timed alone before the others: a 256 KB load, a 64 B one, and the
 * transposes from DT_TRANSPOSE on: the int8 prefill K tile (1024 rows of
 * 128 B), the same bytes as int4, the MXNF4 prefill K tile (1024 rows of
 * 64 B), a q_proj weight tile read from an [out, in] weight (128 rows of
 * the 2048 B row of 4096 inputs, written as 4096 rows of 128 outputs), and
 * the same bytes as int8. */
#define DT_THROUGHPUT 0
#define DT_LATENCY 1
#define DT_TRANSPOSE 2
#define DT_TIMED 7

/* One copy per line: name, queue, source, destination, inner bytes, CTRL,
 * levels. */
// clang-format off
static const dma_test_case_t dma_test_cases[] = {
  {"throughput 256 KB load", DMA_LOAD, DT_D(0x100000), DT_S(0x40000), 0x40000,
   DT_POST(9), {DT_ONE, DT_ONE, DT_ONE}},
  {"latency 64 B load", DMA_LOAD, DT_D(0xA0000), DT_S(0x14000), 64,
   DT_POST(10), {DT_ONE, DT_ONE, DT_ONE}},
  /* The five transposed copies of the test set, then edge shapes. */
  {"transpose int8 1024 x 128", DMA_LOAD, DT_D(0x140000), DT_S(0x80000), 128,
   DT_TRANSPOSED(0, 11), {{1024, 128, 1024}, DT_ONE, DT_ONE}},
  {"transpose int4 1024 x 256", DMA_LOAD, DT_D(0x200000), DT_S(0xC0000), 128,
   DT_TRANSPOSED4(19), {{1024, 128, 512}, DT_ONE, DT_ONE}},
  {"transpose int4 1024 x 128 (MXNF4 K)", DMA_LOAD, DT_D(0x220000),
   DT_S(0xE0000), 64, DT_TRANSPOSED4(20), {{1024, 64, 512}, DT_ONE, DT_ONE}},
  {"transpose int4 128 x 4096 (q_proj weight tile)", DMA_LOAD, DT_D(0x240000),
   DT_S(0x100000), 2048, DT_TRANSPOSED4(21), {{128, 2048, 64}, DT_ONE,
   DT_ONE}},
  {"transpose int8 128 x 2048 (the q_proj tile's bytes)", DMA_LOAD,
   DT_D(0x240000), DT_S(0x160000), 2048, DT_TRANSPOSED(0, 28), {{128, 2048,
   128}, DT_ONE, DT_ONE}},
  {"transpose fp8 1024 x 2", DMA_LOAD, DT_D(0x160000), DT_S(0xA0000), 2,
   DT_TRANSPOSED(0, 12), {{1024, 2, 1024}, DT_ONE, DT_ONE}},
  {"transpose bf16 64 x 128", DMA_LOAD, DT_D(0x161000), DT_S(0xA1000), 256,
   DT_TRANSPOSED(1, 13), {{64, 256, 128}, DT_ONE, DT_ONE}},
  {"transpose int8 576 x 128", DMA_LOAD, DT_D(0x165000), DT_S(0xA5000), 128,
   DT_TRANSPOSED(0, 14), {{576, 128, 576}, DT_ONE, DT_ONE}},
  {"transpose fp8 9 x 128", DMA_LOAD, DT_D(0x177000), DT_S(0xB7000), 128,
   DT_TRANSPOSED(0, 15), {{9, 128, 9}, DT_ONE, DT_ONE}},
  {"transpose int8 37 x 100, unaligned", DMA_LOAD, DT_D(0x178003),
   DT_S(0xB8005), 100, DT_TRANSPOSED(0, 16), {{37, 112, 50}, DT_ONE, DT_ONE}},
  {"transpose bf16 40 x 45, unaligned", DMA_LOAD, DT_D(0x17A001),
   DT_S(0xBA003), 90, DT_TRANSPOSED(1, 17), {{40, 96, 82}, DT_ONE, DT_ONE}},
  {"transpose int8 70 x 70, two of them", DMA_LOAD, DT_D(0x17C000),
   DT_S(0xBC000), 70, DT_TRANSPOSED(0, 18), {{70, 80, 72}, {2, 6000, 5100},
   DT_ONE}},
  /* The q_proj tile's fp8 scales from [out, in / 64], then 4-bit edge
   * shapes: an even row count is required. */
  {"transpose fp8 128 x 64 (q_proj scale tile)", DMA_LOAD, DT_D(0x280000),
   DT_S(0x140000), 64, DT_TRANSPOSED(0, 22), {{128, 64, 128}, DT_ONE, DT_ONE}},
  {"transpose int4 130 x 128, past one block", DMA_LOAD, DT_D(0x282000),
   DT_S(0x142000), 64, DT_TRANSPOSED4(23), {{130, 64, 72}, DT_ONE, DT_ONE}},
  {"transpose int4 38 x 100, unaligned", DMA_LOAD, DT_D(0x285003),
   DT_S(0x145005), 50, DT_TRANSPOSED4(24), {{38, 56, 23}, DT_ONE, DT_ONE}},
  {"transpose int4 2 x 2", DMA_LOAD, DT_D(0x286001), DT_S(0x146003), 1,
   DT_TRANSPOSED4(25), {{2, 5, 3}, DT_ONE, DT_ONE}},
  {"transpose int4 70 x 140, two of them", DMA_LOAD, DT_D(0x287000),
   DT_S(0x148000), 70, DT_TRANSPOSED4(26), {{70, 80, 40}, {2, 6000, 6000},
   DT_ONE}},
  {"transpose int4 200 x 300", DMA_LOAD, DT_D(0x28A000), DT_S(0x14B000), 150,
   DT_TRANSPOSED4(27), {{200, 160, 104}, DT_ONE, DT_ONE}},
  {"aligned 4 KB load", DMA_LOAD, DT_D(0x0), DT_S(0x0), 4096, DT_POST(0),
   {DT_ONE, DT_ONE, DT_ONE}},
  {"unaligned both sides", DMA_LOAD, DT_D(0x10003), DT_S(0x2005), 1000,
   DT_POST(1), {DT_ONE, DT_ONE, DT_ONE}},
  {"unaligned source", DMA_LOAD, DT_D(0x20011), DT_S(0x4000), 77, DT_POST(2),
   {DT_ONE, DT_ONE, DT_ONE}},
  {"unaligned destination", DMA_LOAD, DT_D(0x30000), DT_S(0x4141), 130,
   DT_POST(2), {DT_ONE, DT_ONE, DT_ONE}},
  {"one byte", DMA_LOAD, DT_D(0x31007), DT_S(0x5003), 1, DT_POST(2),
   {DT_ONE, DT_ONE, DT_ONE}},
  {"two bytes across beats", DMA_LOAD, DT_D(0x3103F), DT_S(0x507F), 2,
   DT_POST(2), {DT_ONE, DT_ONE, DT_ONE}},
  {"2-D", DMA_LOAD, DT_D(0x40005), DT_S(0x6003), 100, DT_POST(3),
   {{37, 256, 112}, DT_ONE, DT_ONE}},
  {"3-D", DMA_LOAD, DT_D(0x50000), DT_S(0x8001), 64, DT_POST(3),
   {{5, 192, 64}, {3, 4096, 330}, DT_ONE}},
  {"3 levels, negative stride", DMA_LOAD, DT_D(0x61000), DT_S(0xA000), 20,
   DT_POST(4), {{4, -40, 24}, {3, 300, 100}, {2, 2000, 400}}},
  {"fill", DMA_LOAD, 0, DT_S(0xC003), 3000, DT_POST(5) | DMA_FILL,
   {DT_ONE, DT_ONE, DT_ONE}},
  {"2-D fill", DMA_LOAD, 0, DT_S(0xD007), 33, DT_POST(5) | DMA_FILL,
   {{10, 0, 50}, DT_ONE, DT_ONE}},
  {"2-D store", DMA_STORE, DT_S(0xE009), DT_D(0x7000D), 500, DT_POST(6),
   {{6, 512, 777}, DT_ONE, DT_ONE}},
  {"aligned 8 KB store", DMA_STORE, DT_S(0x10000), DT_D(0x80000), 8192,
   DT_POST(6), {DT_ONE, DT_ONE, DT_ONE}},
  {"empty", DMA_LOAD, DT_D(0x0), DT_S(0x0), 64, DT_POST(7),
   {{0, 0, 0}, DT_ONE, DT_ONE}},
  {"same counter 0", DMA_LOAD, DT_D(0x90000), DT_S(0x12000), 100, DT_POST(8),
   {DT_ONE, DT_ONE, DT_ONE}},
  {"same counter 1", DMA_LOAD, DT_D(0x90100), DT_S(0x12080), 100, DT_POST(8),
   {DT_ONE, DT_ONE, DT_ONE}},
  {"same counter 2", DMA_LOAD, DT_D(0x90200), DT_S(0x12100), 100, DT_POST(8),
   {DT_ONE, DT_ONE, DT_ONE}},
  {"same counter 3", DMA_LOAD, DT_D(0x90300), DT_S(0x12180), 100, DT_POST(8),
   {DT_ONE, DT_ONE, DT_ONE}},
};
// clang-format on

#define DMA_TEST_CASES \
  ((int)(sizeof(dma_test_cases) / sizeof(dma_test_cases[0])))

#endif /* DMA_TEST_CASES_H */
