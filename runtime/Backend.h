#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "runtime/GraphUtils.h"
#include "build/codegen/proto/voyager_ir.pb.h"

// One wait a dispatch's pass starts behind: on `unit`, counter >= threshold.
struct CounterWait {
  std::string unit;  // "matrix" (systolic array or MVM) or "vector"
  int64_t counter = 0;
  int64_t threshold = 0;
  std::string node;  // the counter's node name, for messages
};

// A commit's synchronization: its waits, grouped by unit, and at most one
// post, which the dispatch's last pass adds 1 to when it completes.
struct CommitSync {
  std::string op;  // the commit's name, for messages
  std::vector<CounterWait> waits;
  std::optional<int64_t> post;
  std::string post_node;
};

// Where the interpreter hands an operation to whatever is actually computing.
//
// The interpreter resolves control flow, scalars and data movement identically
// in every environment; only this last step differs. The gold model runs the
// bit-accurate C++ kernel in place; the SystemC and RTL flows will instead
// lower the operation into MatrixParams / VectorParams and push it at the
// hardware.
class Backend {
 public:
  virtual ~Backend() = default;

  // `op` is a prim or a fusion whose targets run on the accelerator.
  virtual void execute(const voyager::Operation& op, const ScalarEnv& env) = 0;

  // --- Counters -----------------------------------------------------------
  //
  // NUM_COUNTERS count-up counters, keyed by id as in the hardware. They start
  // at zero (the reset state), voyager::zeros resets one, a copy adds 1 when
  // it has landed, a commit's last pass adds 1 to its post, and a wait holds
  // once the counter has reached its threshold. Nothing ever decrements.
  //
  // The defaults are the sequential semantics, right for the gold model: the
  // walk reaches a wait only after the ops that satisfy it, so a wait that
  // does not hold is a compiler bug, and the oracle fails on it. The SystemC
  // Harness overrides the waits with blocking ones, because there a commit
  // retires on another thread and the walk can reach a wait first.
  virtual void reset_counter(int64_t id, const std::string& node) {
    counters_[id] = 0;
    counter_nodes_[id] = node;
  }
  virtual void add_counter(int64_t id) { counters_[id]++; }
  virtual void wait_counter(int64_t id, int64_t threshold,
                            const std::string& op) {
    if (counters_[id] < threshold) throw wait_failure(op, "", id, threshold);
  }
  int64_t counter(int64_t id) const { return counters_[id]; }

  // --- Committed (async) regions ------------------------------------------
  //
  // A `commit` dispatches a region asynchronously so a matrix tile's ramp-up
  // (input/weight load, array fill) can overlap the previous tile's ramp-down
  // (output drain). Each of its waits gates one unit's pass; its post is
  // added by the last pass. Sequential backends have nothing to overlap, so
  // the default is fully synchronous: begin_commit checks that every wait
  // already holds, the body's ops flow through execute() exactly as top-level
  // ops do, and end_commit adds the post.
  //
  // The Harness instead attaches the waits to the passes the body dispatches
  // and adds the post when the last of them retires.
  virtual void begin_commit(const CommitSync& sync) {
    for (const auto& wait : sync.waits) {
      if (counters_[wait.counter] < wait.threshold) {
        throw wait_failure(sync.op, wait.unit, wait.counter, wait.threshold);
      }
    }
    commit_post_ = sync.post;
  }
  virtual void end_commit() {
    if (commit_post_) add_counter(*commit_post_);
    commit_post_.reset();
  }

  // --- Data-movement interception -----------------------------------------
  //
  // The interpreter itself performs the program's data movement: async_copy
  // transfers, the materialization of voyager::zeros data buffers, and the
  // zero-fill of integer allocations. A backend that does not execute the
  // program in place -- the SoC schedule recorder, which replays these
  // effects into the DUT's scratchpad later -- overrides
  // intercepts_data_ops() and receives each such op through data_op()
  // instead of the interpreter touching memory. Counter accounting is not
  // affected: adds and waits already flow through the backend.
  virtual bool intercepts_data_ops() const { return false; }
  virtual void data_op(const voyager::Operation& op,
                       const voyager::PrimOp& prim, const ScalarEnv& env) {}

  // A scalar the program reads out of memory at run time -- the CSR cursors a
  // sparse layer appends against. A backend that records rather than executes
  // has not populated that memory yet, so it takes the read here and performs
  // it when it replays; the interpreter then continues on a placeholder.
  virtual bool intercepts_scalar_reads() const { return false; }
  virtual void scalar_read(const voyager::Operation& op,
                           const voyager::PrimOp& prim, const ScalarEnv& env) {}

  // Every other scalar op, offered so a recording backend can follow which
  // values descend from a deferred read. Those were computed from a
  // placeholder and must be recomputed when the real value is known; the rest
  // -- loop indices and the like -- are already correct and are ignored.
  virtual void scalar_op(const voyager::Operation& op,
                         const voyager::PrimOp& prim, const ScalarEnv& env) {}

 protected:
  // The oracle's verdict on a wait that does not hold: the op, the unit (empty
  // for a CPU wait), the counter, its value and the threshold.
  std::runtime_error wait_failure(const std::string& op,
                                  const std::string& unit, int64_t id,
                                  int64_t threshold) const {
    const std::string where = unit.empty() ? "" : unit + " ";
    return std::runtime_error(
        op + ": " + where + "wait on counter " + counter_nodes_[id] + " (id " +
        std::to_string(id) + ") does not hold: value " +
        std::to_string(counters_[id]) + " < threshold " +
        std::to_string(threshold) + ".");
  }

  std::array<int64_t, NUM_COUNTERS> counters_{};
  std::array<std::string, NUM_COUNTERS> counter_nodes_{};

 private:
  std::optional<int64_t> commit_post_;
};
