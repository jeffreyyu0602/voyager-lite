// clang-format off
#ifndef VOYAGER_ADDRESS_H
#define VOYAGER_ADDRESS_H

#define VOYAGER_MMIO_BASE_ADDRESS 0x20000000L

// --- Global / Accelerator Registers ---
#define VOYAGER_BASE_ADDR       (VOYAGER_MMIO_BASE_ADDRESS + 0x000)
#define ACCELERATOR_CYCLE_COUNT (VOYAGER_MMIO_BASE_ADDRESS + 0x008)
// 1 while any unit has a pass dispatched and not done, queued behind a wait
// included.
#define ACCELERATOR_RUNNING     (VOYAGER_MMIO_BASE_ADDRESS + 0x010)
#define BANK_0_LOADED           (VOYAGER_MMIO_BASE_ADDRESS + 0x01A)
#define BANK_1_LOADED           (VOYAGER_MMIO_BASE_ADDRESS + 0x01B)
#define VOYAGER_INT_STATUS      (VOYAGER_MMIO_BASE_ADDRESS + 0x020)
#define VOYAGER_INT_ENABLE      (VOYAGER_MMIO_BASE_ADDRESS + 0x028)

// --- Per-unit registers, at the unit's base (0x100 matrix, 0x200 vector,
// 0x300 MVM, 0x400 SpMM) ---
// +0x00 PARAMS_IN   W64  one word of a pass's params record
// +0x08 CYCLE_COUNT R64  cycles with a pass running
// +0x10 OP_INFLIGHT R8   passes dispatched and not done (queued included)
// +0x18 SYNC_WAIT   W64  [4:0] counter, [63:32] threshold: the next pass
//                        dispatched starts once counter >= threshold
// +0x20 SYNC_POST   W8   the counter the next pass's done adds 1 to
// +0x28 DISPATCH    W8   closes the next pass: the waits and the post written
//                        since the previous DISPATCH are its; params follow
#define UNIT_PARAMS_IN   0x00
#define UNIT_CYCLE_COUNT 0x08
#define UNIT_OP_INFLIGHT 0x10
#define UNIT_SYNC_WAIT   0x18
#define UNIT_SYNC_POST   0x20
#define UNIT_DISPATCH    0x28

#define MATRIX_UNIT_BASE        (VOYAGER_MMIO_BASE_ADDRESS + 0x100)
#define VECTOR_UNIT_BASE        (VOYAGER_MMIO_BASE_ADDRESS + 0x200)
#define MVM_UNIT_BASE           (VOYAGER_MMIO_BASE_ADDRESS + 0x300)
#define SPMM_UNIT_BASE          (VOYAGER_MMIO_BASE_ADDRESS + 0x400)

#define MATRIX_UNIT_PARAMS_IN   (MATRIX_UNIT_BASE + UNIT_PARAMS_IN)
#define MATRIX_UNIT_CYCLE_COUNT (MATRIX_UNIT_BASE + UNIT_CYCLE_COUNT)
#define MATRIX_UNIT_OP_INFLIGHT (MATRIX_UNIT_BASE + UNIT_OP_INFLIGHT)
#define VECTOR_UNIT_PARAMS_IN   (VECTOR_UNIT_BASE + UNIT_PARAMS_IN)
#define VECTOR_UNIT_CYCLE_COUNT (VECTOR_UNIT_BASE + UNIT_CYCLE_COUNT)
#define VECTOR_UNIT_OP_INFLIGHT (VECTOR_UNIT_BASE + UNIT_OP_INFLIGHT)
#define MVM_UNIT_PARAMS_IN      (MVM_UNIT_BASE + UNIT_PARAMS_IN)
#define MVM_UNIT_CYCLE_COUNT    (MVM_UNIT_BASE + UNIT_CYCLE_COUNT)
#define MVM_UNIT_OP_INFLIGHT    (MVM_UNIT_BASE + UNIT_OP_INFLIGHT)
#define SPMM_UNIT_PARAMS_IN     (SPMM_UNIT_BASE + UNIT_PARAMS_IN)
#define SPMM_UNIT_CYCLE_COUNT   (SPMM_UNIT_BASE + UNIT_CYCLE_COUNT)
#define SPMM_UNIT_OP_INFLIGHT   (SPMM_UNIT_BASE + UNIT_OP_INFLIGHT)

// --- Synchronization counters ---
// 32 count-up 32-bit counters. A pass's done adds 1 to the counter its
// SYNC_POST named; the DMA engine (the SoC testbench) adds 1 when a copy has
// landed; the CPU resets one and reads them.
#define NUM_COUNTERS 32
// W32: any write adds 1; the SoC testbench services the mailbox on a change.
#define HOST_DOORBELL           (VOYAGER_MMIO_BASE_ADDRESS + 0x500)
// W8: the counter id to zero.
#define CTR_RESET               (VOYAGER_MMIO_BASE_ADDRESS + 0x508)
// W8: the counter id to add 1 to (a copy's completion when the data is
// preloaded: full JTAG mode, the chip).
#define CTR_ADD                 (VOYAGER_MMIO_BASE_ADDRESS + 0x510)
// R32: the counter's value.
#define CTR_VALUE(id)           (VOYAGER_MMIO_BASE_ADDRESS + 0x600 + 8 * (id))

// --- Memory ---
#define SRAM_BASE (0x40000000L)

// Bit definitions
#define INT_BANK0 (1 << 0)
#define INT_BANK1 (1 << 1)
#define INT_DONE  (1 << 2)

#endif // VOYAGER_ADDRESS_H
// clang-format on
