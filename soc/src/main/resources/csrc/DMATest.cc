#include "DMATest.h"

#ifdef HW_DMA

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>

#include "runtime/GraphUtils.h"
#include "soc/emitter/DMAModel.h"
#include "soc/firmware/common/host_request.h"
#include "soc/firmware/tests/dma_test_cases.h"

namespace {

// The initial pattern of each region: distinct per byte and per region.
uint8_t pattern(uint64_t address, uint32_t seed) {
  uint64_t x = (address + seed) * 0x9E3779B97F4A7C15ULL;
  return static_cast<uint8_t>(x >> 56);
}

constexpr uint32_t kDRAMSeed = 0x1234;
constexpr uint32_t kSRAMSeed = 0xABCD;

// The model regions as a MemoryInterface at absolute addresses (the partition
// is ignored), for the element path of the testbench's host copies.
class ModelMemory : public MemoryInterface {
 public:
  explicit ModelMemory(std::function<uint8_t&(uint64_t)> byte)
      : byte_(std::move(byte)) {}

  void read_bytes_from_memory(const long long address, const int partition,
                              const int num_bytes, char* bytes) override {
    for (int i = 0; i < num_bytes; i++) bytes[i] = byte_(address + i);
  }

  void write_bytes_to_memory(const long long address, const int partition,
                             const int num_bytes, const char* bytes) override {
    for (int i = 0; i < num_bytes; i++) byte_(address + i) = bytes[i];
  }

 private:
  std::function<uint8_t&(uint64_t)> byte_;
};

}  // namespace

DMATest::DMATest()
    : memory_(
          {4096,
           static_cast<uint64_t>(std::atoll(std::getenv("SCRATCHPAD_SIZE")
                                                ? std::getenv("SCRATCHPAD_SIZE")
                                                : "8388608")),
           4096}),
      dram_(DT_DRAM_BYTES),
      sram_(DT_SRAM_BYTES) {}

uint8_t& DMATest::model(uint64_t address) {
  if (address >= DT_DRAM && address < DT_DRAM + DT_DRAM_BYTES) {
    return dram_[address - DT_DRAM];
  }
  if (address >= DT_SRAM && address < DT_SRAM + DT_SRAM_BYTES) {
    return sram_[address - DT_SRAM];
  }
  throw std::runtime_error("DMA test address outside the test regions");
}

void DMATest::apply(int index) {
  const dma_test_case_t& c = dma_test_cases[index];
  const dma_level_t* level = c.levels;
  if ((c.ctrl & DMA_TRANSPOSED) != 0 && (c.ctrl & DMA_NIBBLES) != 0) {
    // As the testbench's transposed host copy moves int4 (copy_run's element
    // path): element (row r, column j) of the source to (row j, column r).
    ModelMemory memory(
        [this](uint64_t address) -> uint8_t& { return model(address); });
    for (uint32_t i2 = 0; i2 < level[2].count; i2++) {
      for (uint32_t i1 = 0; i1 < level[1].count; i1++) {
        const uint64_t src =
            c.src + i2 * level[2].src_stride + i1 * level[1].src_stride;
        const uint64_t dst =
            c.dst + i2 * level[2].dst_stride + i1 * level[1].dst_stride;
        for (uint32_t r = 0; r < level[0].count; r++) {
          for (uint32_t j = 0; j < 2 * c.inner; j++) {
            const float value =
                memory.read_value(0, src + r * level[0].src_stride, j, "int4");
            memory.write_value(0, dst + j * level[0].dst_stride, r, "int4",
                               value);
          }
        }
      }
    }
    return;
  }
  DMADescriptor copy;
  copy.src = c.src;
  copy.dst = c.dst;
  copy.inner = c.inner;
  copy.ctrl = c.ctrl;
  for (int k = 0; k < DMA_LEVELS; k++) copy.levels[k] = c.levels[k];
  // Moves in nibbles (DMAModel.h); every other copy moves whole bytes.
  for_each_dma_move(copy, [&](uint64_t src, uint64_t dst, uint32_t nibbles) {
    const bool fill = src == kDMAFill;
    if ((!fill && src % 2 != 0) || dst % 2 != 0 || nibbles % 2 != 0) {
      throw std::runtime_error("a byte copy moves half bytes");
    }
    for (uint32_t i = 0; i < nibbles / 2; i++) {
      model(dst / 2 + i) = fill ? 0 : model(src / 2 + i);
    }
  });
}

void DMATest::start() {
  MemoryInterface* memory = &memory_;
  for (uint64_t i = 0; i < DT_DRAM_BYTES; i++) {
    dram_[i] = pattern(DT_DRAM + i, kDRAMSeed);
  }
  for (uint64_t i = 0; i < DT_SRAM_BYTES; i++) {
    sram_[i] = pattern(DT_SRAM + i, kSRAMSeed);
  }
  memory->write_bytes_to_memory(DT_DRAM - DRAM_BASE, DRAM_PARTITION,
                                DT_DRAM_BYTES,
                                reinterpret_cast<const char*>(dram_.data()));
  memory->write_bytes_to_memory(DT_SRAM - SRAM_BASE, SRAM_PARTITION,
                                DT_SRAM_BYTES,
                                reinterpret_cast<const char*>(sram_.data()));
  for (int i = 0; i < DMA_TEST_CASES; i++) apply(i);

  const char* mailbox = std::getenv("HOST_MAILBOX");
  if (mailbox == nullptr) {
    throw std::runtime_error("HOST_MAILBOX is not set");
  }
  mailbox_ = std::strtoull(mailbox, nullptr, 0);
  std::cerr << "[DMATEST] " << DMA_TEST_CASES << " copies; regions loaded"
            << std::endl;
}

void DMATest::doorbell() {
  MemoryInterface* memory = &memory_;
  auto read_u64 = [&](uint64_t address) {
    uint64_t value = 0;
    memory->read_bytes_from_memory(address - SRAM_BASE, SRAM_PARTITION,
                                   sizeof(value),
                                   reinterpret_cast<char*>(&value));
    return value;
  };
  const uint64_t head = read_u64(mailbox_ + offsetof(host_mailbox_t, head));
  while (tail_ < head) {
    const uint64_t slot = mailbox_ + offsetof(host_mailbox_t, slots) +
                          (tail_ % HOST_MAILBOX_SLOTS) * sizeof(host_request_t);
    const uint64_t kind = read_u64(slot + offsetof(host_request_t, kind));
    const uint64_t cell = read_u64(slot + offsetof(host_request_t, cell));
    tail_++;
    const uint64_t tail = tail_;
    memory->write_bytes_to_memory(
        mailbox_ + offsetof(host_mailbox_t, tail) - SRAM_BASE, SRAM_PARTITION,
        sizeof(tail), reinterpret_cast<const char*>(&tail));
    if (kind != HOST_REQ_FINISH) {
      throw std::runtime_error("DMA test: unexpected request kind " +
                               std::to_string(kind));
    }
    finish(cell);
  }
}

void DMATest::finish(uint64_t cell) {
  MemoryInterface* memory = &memory_;
  std::vector<uint8_t> dram(DT_DRAM_BYTES);
  std::vector<uint8_t> sram(DT_SRAM_BYTES);
  memory->read_bytes_from_memory(DT_DRAM - DRAM_BASE, DRAM_PARTITION,
                                 DT_DRAM_BYTES,
                                 reinterpret_cast<char*>(dram.data()));
  memory->read_bytes_from_memory(DT_SRAM - SRAM_BASE, SRAM_PARTITION,
                                 DT_SRAM_BYTES,
                                 reinterpret_cast<char*>(sram.data()));

  // Mismatching bytes, reported as maximal runs.
  uint64_t errors = 0;
  auto compare = [&](const char* region, uint64_t base,
                     const std::vector<uint8_t>& got,
                     const std::vector<uint8_t>& want) {
    for (uint64_t i = 0; i < got.size();) {
      if (got[i] == want[i]) {
        i++;
        continue;
      }
      uint64_t end = i;
      while (end < got.size() && got[end] != want[end]) end++;
      std::cerr << "[DMATEST] " << region << " mismatch [0x" << std::hex
                << base + i << ", 0x" << base + end << ")" << std::dec
                << std::endl;
      errors += end - i;
      i = end;
    }
  };
  compare("DRAM", DT_DRAM, dram, dram_);
  compare("SRAM", DT_SRAM, sram, sram_);
  std::cout << "Error count: " << errors << std::endl;

  if (cell != 0) {
    uint64_t value = 0;
    memory->read_bytes_from_memory(cell - SRAM_BASE, SRAM_PARTITION,
                                   sizeof(value),
                                   reinterpret_cast<char*>(&value));
    value++;
    memory->write_bytes_to_memory(cell - SRAM_BASE, SRAM_PARTITION,
                                  sizeof(value),
                                  reinterpret_cast<const char*>(&value));
  }
}

#endif  // HW_DMA
