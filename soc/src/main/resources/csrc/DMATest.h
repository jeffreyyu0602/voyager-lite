#pragma once

#include <cstdint>
#include <vector>

#include "SoCMemory.h"

// The testbench of the DMA engine's directed test (soc/firmware/tests), run
// with DMA_TEST set and no network: fills both test regions with patterns,
// models every copy of dma_test_cases.h in issue order, and on the firmware's
// FINISH request compares every byte of both regions with the model, printing
// "Error count: <n>". Built for a DRAM config only (HW_DMA).
class DMATest {
 public:
  DMATest();

  void start();
  void doorbell();

 private:
  // The model's byte at absolute address `address`, in either region.
  uint8_t& model(uint64_t address);
  void apply(int index);
  void finish(uint64_t cell);

  SoCMemory memory_;
  std::vector<uint8_t> dram_;
  std::vector<uint8_t> sram_;
  uint64_t mailbox_ = 0;
  uint64_t tail_ = 0;
};
