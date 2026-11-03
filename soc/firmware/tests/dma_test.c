/* The DMA engine's directed test (dma_test_cases.h). Times the first
 * DT_TIMED copies alone, then pushes every other copy, waits for each counter
 * to reach the number of copies that post it, and asks the testbench to
 * compare the two test regions (host_finish). Prints the counter values it
 * found. */
#include "dma.h"

#include <stdio.h>

#include "dma_test_cases.h"
#include "encoding.h"
#include "host_request.h"
#include "mmio.h"

static void push(const dma_test_case_t* c) {
  dma_push(c->queue, c->src, c->dst, c->inner, c->ctrl, c->levels);
}

static int counter_of(const dma_test_case_t* c) {
  return (int)(c->ctrl & 0x1f);
}

static void wait_counter(int ctr, uint32_t value) {
  while (reg_read32(CTR_VALUE(ctr)) < value) {
  }
}

/* Cycles from the push of `c` to its counter reaching `value`. */
static uint64_t timed(const dma_test_case_t* c, uint32_t value) {
  const uint64_t start = read_csr(mcycle);
  push(c);
  wait_counter(counter_of(c), value);
  return read_csr(mcycle) - start;
}

int main(void) {
  host_init();

  const dma_test_case_t* big = &dma_test_cases[DT_THROUGHPUT];
  const uint64_t busy_before = reg_read64(DMA_LOAD_BUSY_CYCLES);
  const uint64_t cycles = timed(big, 1);
  const uint64_t busy = reg_read64(DMA_LOAD_BUSY_CYCLES) - busy_before;
  printf("DMA throughput: %u bytes, %lu cycles push to counter, %lu busy\n",
         big->inner, cycles, busy);
  printf("DMA latency: 64 B load, %lu cycles push to counter\n",
         timed(&dma_test_cases[DT_LATENCY], 1));
  for (int i = DT_TRANSPOSE; i < DT_TIMED; i++) {
    printf("DMA transpose: %s, %lu cycles push to counter\n",
           dma_test_cases[i].name, timed(&dma_test_cases[i], 1));
  }

  uint32_t expected[NUM_COUNTERS] = {0};
  for (int i = 0; i < DMA_TEST_CASES; i++) {
    if (dma_test_cases[i].ctrl & DMA_POST) {
      expected[counter_of(&dma_test_cases[i])]++;
    }
  }
  for (int i = DT_TIMED; i < DMA_TEST_CASES; i++) {
    push(&dma_test_cases[i]);
  }
  for (int ctr = 0; ctr < NUM_COUNTERS; ctr++) {
    wait_counter(ctr, expected[ctr]);
  }
  dma_wait_idle((1u << DMA_LOAD) | (1u << DMA_STORE));

  int counter_errors = 0;
  for (int ctr = 0; ctr < NUM_COUNTERS; ctr++) {
    const uint32_t value = reg_read32(CTR_VALUE(ctr));
    if (value != expected[ctr]) {
      printf("counter %d is %u, expected %u\n", ctr, value, expected[ctr]);
      counter_errors++;
    }
  }
  printf("DMA counter errors: %d\n", counter_errors);

  host_finish();
  printf("DMA test finished\n");
  return 0;
}
