#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "runtime/GraphUtils.h"
#include "runtime/Model.h"
#include "soc/emitter/DMALowering.h"
#include "soc/emitter/HostRequests.h"

// Emits one layer's bufferized program as C for the control processor.
//
// The generated firmware executes the whole program op for op: loops, scalar
// arithmetic, conditionals, dispatches, the index-cell bookkeeping, and the
// program's synchronization on the accelerator's hardware counters
// (run_voyager_operation.h): a commit's waits and post are attached to its
// passes with sync_wait / sync_post, a CPU wait is ctr_wait, a counter
// declaration is ctr_reset. The DRAM<->scratchpad
// transfers the firmware cannot perform itself (Sphinx has no DMA engine)
// become requests to the SoC testbench (host_request.h): a voyager::async_copy
// or zero fill names its prim by ordinal in the host-request table and ships
// the run-time values of the scalars the prim references; the testbench adds
// 1 to the copy's counter once the transfer has landed. Built with
// NO_TESTBENCH (full JTAG mode, the chip) the image is preloaded and
// host_copy adds the 1 itself; the emitted C is the same.
//
// For a DRAM config (HW_DMA set at generation) every copy and zero fill
// that lowers to a DMA descriptor (DMALowering.h) is also emitted as a push
// onto the wrapper's DMA engine, under #ifdef HW_DMA; the mailbox request
// stays in the #else arm, so the same C builds either way. A copy that does
// not lower waits for the DMA queues to drain, then takes the mailbox, so
// the copies on its counter still land in order.
//
// A dispatch's params are serialized once at generation time with the
// iteration-0 scalar environment and embedded as static byte arrays. Fields
// that depend on runtime scalars (software-pipeline slot addresses) are
// located by probing -- re-serializing with one scalar perturbed and diffing
// the bit stream -- and the generated C patches them with patch_bits() before
// each send.
class CEmitter {
 public:
  explicit CEmitter(const Model& model) : model_(model) {}

  // Emits one layer. Returns the complete C translation unit; `layer` names
  // it in diagnostics.
  std::string emit_layer(const Model::Selection& selection,
                         const std::string& layer);

 private:
  // --- symbol table: SSA name -> C identifier, scoped like ScalarEnv ---
  std::string declare(const std::string& ssa_name, int indent,
                      bool emit_decl = true);
  std::string bind(const std::string& ssa_name, const std::string& c_name);
  std::string ref(const std::string& ssa_name) const;

  std::string scalar_expr(const voyager::ScalarValue& value) const;
  // A wait's threshold, lowered by the stores a mid-tile cut dropped from
  // its counter (Model::Selection::lowered).
  std::string threshold_expr(const voyager::ScalarValue& value) const;

  // --- emission over the operation tree ---
  void emit_ops(
      const google::protobuf::RepeatedPtrField<voyager::Operation>& ops,
      int indent);
  void emit_operation(const voyager::Operation& op, int indent);
  void emit_scalar_prim(const voyager::Operation& op,
                        const voyager::PrimOp& prim, int indent);
  void emit_delinearize(const voyager::Operation& op,
                        const voyager::PrimOp& prim, int indent);
  // aten::_local_scalar_dense: a volatile load from the scratchpad.
  void emit_scalar_load(const voyager::Operation& op,
                        const voyager::PrimOp& prim, int indent);

  // A window of byte-aligned integer cells in the scratchpad, as C: the
  // element type, its width, the constant part of the byte address, the
  // run-time offset terms, and the element count.
  struct CellWindow {
    std::string c_type;
    int64_t width = 0;
    int64_t base = 0;
    std::string runtime_terms;
    int64_t count = 0;
  };
  CellWindow cell_window(const voyager::TensorBoxRef& ref,
                         const std::string& who) const;

  // aten::clone of an index cell, or the integer aten::add that maintains a
  // CSR running base: bookkeeping the control processor runs in place, after
  // the dispatch whose results it reads has retired.
  void emit_host_bookkeeping(const voyager::Operation& op,
                             const voyager::PrimOp& prim, int indent);
  void emit_for(const voyager::Operation& op, const voyager::ForLoop& loop,
                int indent);
  void emit_while(const voyager::Operation& op, const voyager::WhileLoop& loop,
                  int indent);
  void emit_cond(const voyager::Operation& op, const voyager::CondOp& cond,
                 int indent);
  void emit_dispatch(const voyager::Operation& op, int indent);

  // --- the program's counters and its requests to the testbench ---
  // GraphUtils' counter_id, with `who` naming the op in the error.
  int64_t counter_id(const voyager::TensorBox& box,
                     const std::string& who) const;
  // voyager::zeros of a counter: ctr_reset(id).
  void emit_counter_reset(const voyager::Operation& op, int indent);
  // voyager::async_wait: the CPU spins until counter >= threshold.
  void emit_async_wait(const voyager::Operation& op,
                       const voyager::PrimOp& prim, int indent);
  // A commit: its body's one dispatch carries the waits and the post.
  void emit_async(const voyager::Operation& op, int indent);
  // A prim in the host-request table: a copy, a zero fill or a host tensor
  // op, issued to the testbench by ordinal.
  void emit_host_request(const voyager::Operation& op,
                         const voyager::PrimOp& prim, size_t ordinal,
                         int indent);
  // The mailbox request of a host-table prim (emit_host_request's #else arm
  // in a DMA layer).
  void emit_mailbox_request(const voyager::Operation& op,
                            const voyager::PrimOp& prim, size_t ordinal,
                            int indent);
  // A copy's or zero fill's DMA push, or the drain before its mailbox
  // fallback; logs which. Returns whether it lowered.
  bool emit_dma_request(const voyager::Operation& op,
                        const voyager::PrimOp& prim, size_t ordinal,
                        int indent);

  // With `requests`, a copy or zero fill counts as a dispatch: its DMA
  // lowering, like a dispatch's params, needs an environment the program
  // reaches.
  bool contains_dispatch(const voyager::Operation& op,
                         bool requests = false) const;
  // Anything the firmware emits code for: a dispatch, a request, a counter
  // operation, bookkeeping. A loop with none is not emitted.
  bool contains_work(const voyager::Operation& op) const;
  bool cond_guards_dispatch(const voyager::Operation& op, bool requests) const;

  // Concrete iteration-0 evaluation, mirroring the Interpreter's scalar
  // semantics; keeps env_ valid so dispatch sites can be probed.
  Scalar eval_scalar_prim(const voyager::PrimOp& prim) const;

  // Per-iteration advance of a scalar, followed back through constant
  // factors to a loop counter; 0 when it is not a scaled counter.
  int64_t derived_step(const std::string& ssa_name, int depth = 0) const;

  // Every scalar SSA name a dispatch's operand references (window offsets,
  // scalar kwargs) -- the probe set.
  std::set<std::string> collect_ref_scalars(const voyager::Operation& op) const;

  void line(int indent, const std::string& text);

  const Model& model_;
  std::string layer_;
  ScalarEnv env_;
  std::vector<std::map<std::string, std::string>> scopes_;
  std::map<std::string, int> name_counts_;
  std::ostringstream decls_;  // file-scope statics: the params arrays
  std::ostringstream body_;   // statements inside main()

  // The layer's host-request table (HostRequests.h) and each prim's ordinal.
  std::vector<HostOp> host_table_;
  std::map<const voyager::PrimOp*, size_t> host_ordinals_;
  const std::set<const voyager::Operation*>* bounded_ = nullptr;
  const std::map<const voyager::ScalarValue*, int64_t>* lowered_ = nullptr;
  int loop_depth_ = 0;
  int max_tiles_ = 0;

  // Defining prim per scalar SSA value, recorded as the walk passes it.
  std::map<std::string, const voyager::PrimOp*> scalar_def_;

  // Counters whose advance is known: a for loop's induction variable, and a
  // carried value the body yields as add(itself, constant). Anything else is
  // not a counter -- assuming a step for it would let a rejected probe read
  // as invariance.
  std::map<std::string, int64_t> loop_counter_steps_;

  // Scalars sampled at their own step, so run-time values lie on the sampled
  // grid. Only these may carry a fractional coefficient.
  std::set<std::string> derived_scalars_;

  // True while concretely evaluating a cond arm the iteration-0 predicate
  // does not take: guarded arithmetic there (a divisor that is zero only on
  // the untaken path) yields a placeholder instead of aborting generation.
  bool speculative_ = false;

  // A first walk that emits nothing and maps nothing, run only to learn the
  // environments below. Both walks use the same bounds, so a dispatch absent
  // from concrete_env_ after it is one the firmware's own loop never reaches.
  bool surveying_ = false;

  // The first environment in which each dispatch was reached with its
  // guarding predicates actually true. A dispatch inside a cond arm the
  // iteration-0 predicate does not take carries only placeholder scalars at
  // its emission point, and params serialized from those describe nothing the
  // hardware will run -- so it is mapped and probed under this instead.
  std::map<const voyager::Operation*, ScalarEnv> concrete_env_;

  // The commit whose waits and post the next dispatch emitted must carry;
  // null outside a commit body and once the dispatch has taken them.
  const voyager::Operation* pending_commit_ = nullptr;

  // HW_DMA at generation: copies and zero fills become DMA pushes.
  bool hw_dma_ = false;
  // The queue each copy counter's copies go to: all on one queue, so they
  // land in issue order.
  std::map<int64_t, int> counter_queues_;
  // This layer's lowering tally, for the report.
  int dma_lowered_ = 0;
  int dma_transposed_ = 0;
  int dma_fallbacks_ = 0;
};
