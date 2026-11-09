#ifndef RUN_VOYAGER_OPERATION_H
#define RUN_VOYAGER_OPERATION_H

#include <stdint.h>

typedef enum {
  MATRIX_UNIT,
  VECTOR_UNIT,
  MATRIX_VECTOR_UNIT,
  SPMM_UNIT,
} voyager_params_t;

// Unit codes: the hardware's register blocks, the testbench's done events.
#define UNIT_MATRIX 0
#define UNIT_VECTOR 1
#define UNIT_MVM 2
#define UNIT_SPMM 3

/* Hardware synchronization: 32 count-up counters (voyager_address.h). A
 * pass's waits and its post are written to its unit before its params; the
 * hardware attaches them to the next pass dispatched on that unit. A
 * threshold of 0 or less always holds. */

/* The next pass dispatched on unit starts only once ctr >= threshold. */
void sync_wait(int unit, int ctr, int64_t threshold);
/* The next pass dispatched on unit adds 1 to ctr when it completes. */
void sync_post(int unit, int ctr);
/* CPU spins until ctr >= threshold. */
void ctr_wait(int ctr, int64_t threshold);
/* ctr = 0. */
void ctr_reset(int ctr);

void wait_for_accelerator_done();
void send_voyager_params(const void** params, voyager_params_t* params_type,
                         int count);

// Per-unit sends the generated firmware calls directly. Each compute send
// and send_vector_params dispatch one pass.
void send_matrix_unit_params(const void* matrix_params);
void send_matrix_vector_unit_params(const void* matrix_params);
void send_spmm_unit_params(const void* matrix_params);
void send_vector_params(const void* vector_params);
void send_vector_instructions(const void* vector_instructions);

// Suppress printf in JTAG simulation where no frontend server is present
#ifdef SUPPRESS_PRINTF
#include <stdio.h>
#ifdef printf
#undef printf
#endif
// clang-format off
#define printf(...) do {} while (0)
// clang-format on
#endif

#endif
