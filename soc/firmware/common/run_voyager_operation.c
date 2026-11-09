#include "run_voyager_operation.h"

#include <stdio.h>
#include <string.h>

#include "mmio.h"
#include "voyager_address.h"
#include "voyager_params.h"

static const uintptr_t unit_base[4] = {MATRIX_UNIT_BASE, VECTOR_UNIT_BASE,
                                       MVM_UNIT_BASE, SPMM_UNIT_BASE};

void sync_wait(int unit, int ctr, int64_t threshold) {
  /* A threshold of 0 or less always holds (the IR's t - 1 on the first
   * tile); the hardware compares unsigned, so it is clamped to 0 here. */
  const uint64_t clamped = threshold > 0 ? (uint64_t)(uint32_t)threshold : 0;
  const uint64_t word = (clamped << 32) | (uint64_t)(ctr & 0x1f);
  reg_write64(unit_base[unit] + UNIT_SYNC_WAIT, word);
}

void sync_post(int unit, int ctr) {
  reg_write8(unit_base[unit] + UNIT_SYNC_POST, (uint8_t)ctr);
}

void ctr_wait(int ctr, int64_t threshold) {
  /* Signed: a threshold of 0 or less holds at once. */
  if (threshold <= 0) return;
  while ((int64_t)reg_read32(CTR_VALUE(ctr)) < threshold) {
  }
}

void ctr_reset(int ctr) { reg_write8(CTR_RESET, (uint8_t)ctr); }

/* DISPATCH closes the pass the waits and the post written so far belong to,
 * then the params record follows it on the same unit. */
static void send_pass(int unit, const void* params, int width) {
  const uint64_t* ptr = (const uint64_t*)params;
  const int words = (width + 63) / 64;
  reg_write8(unit_base[unit] + UNIT_DISPATCH, 1);
  for (int i = 0; i < words; i++) {
    reg_write64(unit_base[unit] + UNIT_PARAMS_IN, ptr[i]);
  }
}

void send_matrix_unit_params(const void* matrix_params) {
  send_pass(UNIT_MATRIX, matrix_params, matrix_params_width);
}

void send_matrix_vector_unit_params(const void* matrix_params) {
  send_pass(UNIT_MVM, matrix_params, matrix_params_width);
}

void send_spmm_unit_params(const void* matrix_params) {
  send_pass(UNIT_SPMM, matrix_params, matrix_params_width);
}

void send_vector_params(const void* vector_params) {
  send_pass(UNIT_VECTOR, vector_params, vector_params_width);
}

/* The second half of a vector pass's record; its DISPATCH went with the
 * params. */
void send_vector_instructions(const void* vector_instructions) {
  const uint64_t* ptr = (const uint64_t*)vector_instructions;
  const int words = (vector_instruction_config_width + 63) / 64;
  for (int i = 0; i < words; i++) {
    reg_write64(VECTOR_UNIT_PARAMS_IN, ptr[i]);
  }
}

void wait_for_accelerator_done() {
  while (reg_read8(ACCELERATOR_RUNNING)) {
    __asm__ volatile("wfi");
  }
}

void send_voyager_params(const void** params, voyager_params_t* params_type,
                         int count) {
  for (int i = 0; i < count; i++) {
    if (params_type[i] == MATRIX_UNIT) {
      send_matrix_unit_params(params[i]);
    } else if (params_type[i] == MATRIX_VECTOR_UNIT) {
      send_matrix_vector_unit_params(params[i]);
    } else if (params_type[i] == SPMM_UNIT) {
      send_spmm_unit_params(params[i]);
    } else if (params_type[i] == VECTOR_UNIT) {
      send_vector_params(params[i]);
      send_vector_instructions(params[++i]);
    }
  }
}
