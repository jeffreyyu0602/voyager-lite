#pragma once

#include <set>
#include <string>
#include <vector>

#include "build/codegen/proto/voyager_ir.pb.h"
#include "runtime/Model.h"

// The host-request table: the prims of a layer's program that the firmware
// asks the SoC testbench to perform (host_request.h), numbered by one walk
// both sides make. The emitter writes a prim's ordinal into the firmware; the
// testbench indexes this table with it and runs the prim itself, so its
// semantics (run_async_copy, zero_buffer, run_host_operation) are written
// once.
struct HostOp {
  enum Kind { kCopy, kZero, kHostOp };
  Kind kind;
  const voyager::Operation* op;
  const voyager::PrimOp* prim;
};

// A static walk of the selection in proto order -- every loop body once,
// both cond arms, commit bodies -- so the numbering does not depend on
// run-time values. Both sides must build it from the same Selection (same
// TESTS and MAX_TILES).
std::vector<HostOp> enumerate_host_ops(const Model::Selection& selection);

// The box a voyager::zeros writes: its tensor operand, or, for a zeros that
// declares its buffer, the declared box. A counter (register level) resets;
// anything else is a zero fill. Null when the op names no box.
const voyager::TensorBox* zeros_target(const voyager::Operation& op);

// Every scalar SSA name an operation's operands reference: window offsets,
// scalar kwargs, scalar-list entries, destination offsets. The firmware
// ships their run-time values in this (sorted) order; the testbench
// rebinds them under the same names before resolving the prim.
std::set<std::string> request_scalar_names(const voyager::Operation& op);
