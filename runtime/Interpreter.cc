#include "runtime/Interpreter.h"

#include <algorithm>
#include <stdexcept>

#include "spdlog/spdlog.h"
#include "runtime/GraphUtils.h"
#include "runtime/Utils.h"

namespace {

// The host-side arithmetic that drives tiling: loop counters, tile indices,
// bank selection, and every branch predicate. All of it runs on the control
// processor, never on the accelerator.
Scalar apply_scalar_op(const std::string& target, const Scalar& a,
                       const Scalar& b) {
  const bool is_float =
      std::holds_alternative<double>(a) || std::holds_alternative<double>(b);

  if (target == "add") {
    if (is_float) return to_double(a) + to_double(b);
    return to_int(a) + to_int(b);
  }
  if (target == "sub") {
    if (is_float) return to_double(a) - to_double(b);
    return to_int(a) - to_int(b);
  }
  if (target == "mul") {
    if (is_float) return to_double(a) * to_double(b);
    return to_int(a) * to_int(b);
  }
  if (target == "mod") {
    const int64_t divisor = to_int(b);
    if (divisor == 0) throw std::runtime_error("Scalar mod by zero");
    // Python/sympy floor semantics, which is what the compiler emitted against.
    const int64_t remainder = to_int(a) % divisor;
    return (remainder != 0 && ((remainder < 0) != (divisor < 0)))
               ? remainder + divisor
               : remainder;
  }
  if (target == "floordiv") {
    const int64_t divisor = to_int(b);
    if (divisor == 0) throw std::runtime_error("Scalar floordiv by zero");
    const int64_t quotient = to_int(a) / divisor;
    const int64_t remainder = to_int(a) % divisor;
    return (remainder != 0 && ((remainder < 0) != (divisor < 0))) ? quotient - 1
                                                                  : quotient;
  }

  if (target == "eq")
    return is_float ? to_double(a) == to_double(b) : to_int(a) == to_int(b);
  if (target == "ne")
    return is_float ? to_double(a) != to_double(b) : to_int(a) != to_int(b);
  if (target == "lt")
    return is_float ? to_double(a) < to_double(b) : to_int(a) < to_int(b);
  if (target == "le")
    return is_float ? to_double(a) <= to_double(b) : to_int(a) <= to_int(b);
  if (target == "gt")
    return is_float ? to_double(a) > to_double(b) : to_int(a) > to_int(b);
  if (target == "ge")
    return is_float ? to_double(a) >= to_double(b) : to_int(a) >= to_int(b);

  if (target == "and_") return to_bool(a) && to_bool(b);
  if (target == "or_") return to_bool(a) || to_bool(b);

  throw std::runtime_error("Unsupported scalar operation: " + target);
}

// Cycles one execution of an accelerator operation would take with no stalls.
//
// The operands are one tile, so this is the cost of one tile. The walk runs the
// operation once per tile, which is where the old num_tiles multiplier went.
long ideal_cycles(const voyager::Operation& operation, const ScalarEnv& env,
                  bool& is_matrix) {
  const auto& first_op = get_anchor_op(operation);
  const auto outputs = resolve_outputs(operation, env);
  const auto& output = outputs.back();
  const std::string& target = strip_namespace(first_op.target());

  is_matrix = is_gemm_op(target);

  if (is_matrix) {
    const bool is_matmul = target.find("matmul") != std::string::npos;
    const auto& weight = resolve(first_op, is_matmul ? "other" : "weight", env);
    const auto& weight_shape = weight.shape;

    // The total number of MACs is X * Y * C * FX * FY * K.
    const long num_macs = get_size(output) * get_size(weight);

    if (is_fc_layer(first_op)) {
      if (weight_shape.size() < 2) {
        throw std::runtime_error(
            "MVM weight must have at least two dimensions.");
      }
      const int K = weight_shape[weight_shape.size() - 2];
      const auto& input = resolve(first_op, "input", env);
      const int width =
          input.dtype == "bfloat16" ? VECTOR_UNIT_WIDTH : MV_UNIT_WIDTH;
      return num_macs / K / width;
    }

    const int K = weight_shape.back();
    return num_macs / K / (IC_DIMENSION * OC_DIMENSION);
  }

  long cycles = get_size(output) / VECTOR_UNIT_WIDTH;

  if (target == "softmax") {
    cycles *= 3;
  } else if (target == "layer_norm") {
    cycles *= 4;
  } else if (target == "calculate_mx_qparam") {
    cycles *= arg_int(first_op, "block_size", env);
  } else if (target == "max_pool2d") {
    for (const auto& k : arg_ints(first_op, "kernel_size", env)) cycles *= k;
  } else if (target == "adaptive_avg_pool2d") {
    const auto& input_shape = resolve(first_op, "input", env).shape;
    const auto& output_shape = output.shape;
    cycles *=
        (input_shape[1] / output_shape[1]) * (input_shape[2] / output_shape[2]);
  }

  return cycles;
}

}  // namespace

Interpreter::Interpreter(const Model& model, MemoryInterface* memory,
                         Backend* backend)
    : model_(model), memory_(memory), backend_(backend) {}

void Interpreter::log_counters() const {
  for (const auto& [id, node] : counters_) {
    spdlog::info("counter {} ({}) final count: {}\n", id, node,
                 backend_->counter(id));
  }
}

void Interpreter::run(const Model::Selection& selection) {
  for (const auto* op : selection.ops) {
    // Which top-level ops MAX_TILES bounds is the selection's decision -- it
    // stopped the walk at the loop it bounds -- so read it rather than work
    // it out again here. Re-deriving it from TESTS would only agree when a
    // layer's display name happens to also be an op name, and layers.txt
    // names a layer by its extent, not by any one of its ops.
    bound_current_top_ = selection.bounded.count(op) != 0;
    execute_operation(*op);
  }
  bound_current_top_ = true;
}

void Interpreter::execute_region(const voyager::Region& region) {
  for (const auto& op : region.ops()) execute_operation(op);
}

void Interpreter::execute_operation(const voyager::Operation& op) {
  switch (op.op_type_case()) {
    case voyager::Operation::kPrim:
      execute_prim(op, op.prim());
      break;
    case voyager::Operation::kFused:
      // A fusion is always datapath work: its intermediates never leave the
      // pipeline, so there is nothing for the interpreter to do but dispatch
      // it.
      dispatch(op);
      break;
    case voyager::Operation::kLoop:
      if (op.loop().has_for_loop()) {
        execute_for(op, op.loop().for_loop());
      } else if (op.loop().has_while_loop()) {
        execute_while(op, op.loop().while_loop());
      } else {
        throw std::runtime_error("Loop with no body: " + op.name());
      }
      return;
    case voyager::Operation::kCond:
      execute_cond(op, op.cond());
      return;
    case voyager::Operation::kAsync:
      execute_async(op, op.async());
      return;
    default:
      throw std::runtime_error("Operation with no op_type: " + op.name());
  }
}

void Interpreter::execute_prim(const voyager::Operation& op,
                               const voyager::PrimOp& prim) {
  const std::string& target = prim.target();

  if (target == "voyager::alloc") {
    // Address assignment is the compiler's; at run time an allocation only
    // establishes its contents. The executable spec zero-fills integer
    // allocations (torch.zeros) -- the sparse flow's running CSR base is
    // read before any write and counts on it -- while float allocations are
    // randn junk that nothing may read before writing. A register-level
    // allocation declares a counter, which the kernel resets with zeros.
    if (op.outputs_size() == 1 && op.outputs(0).has_tensor_box()) {
      const auto& box = op.outputs(0).tensor_box();
      if (is_counter(box)) {
        counters_[counter_id(box)] = box.node();
        return;
      }
      const bool is_integer =
          box.dtype().rfind("int", 0) == 0 || box.dtype().rfind("uint", 0) == 0;
      if (is_integer && box.has_memory()) {
        // Each slot's own elements, and nothing else. The span between two
        // slots is the pipelining stride, not this buffer's storage: the
        // planner hands that padding to a buffer whose lifetime is disjoint
        // from this one, and zeroing it would clobber that buffer -- and
        // mark its bytes written, which grades a region neither side owns.
        if (backend_->intercepts_data_ops()) {
          backend_->data_op(op, prim, env_);
        } else {
          for (uint32_t bank = 0; bank < banks_of(box); bank++) {
            zero_buffer(to_tensor(box, bank), 1, 0, memory_);
          }
        }
      }
    }
    return;
  }
  if (target == "voyager::zeros") {
    execute_zeros(op, prim);
    return;
  }
  if (target == "voyager::async_copy") {
    if (backend_->intercepts_data_ops()) {
      backend_->data_op(op, prim, env_);
    } else {
      run_async_copy(prim, env_, memory_);
    }
    // The landed transfer adds 1 to the copy stream's counter.
    if (has_tensor(prim, "counter")) {
      backend_->add_counter(counter_id(arg(prim, "counter").tensor_box().box()));
    }
    return;
  }
  if (target == "voyager::async_wait") {
    // The control processor blocks until counter >= threshold. For a
    // sequential backend the wait must already hold; the Harness blocks the
    // walk until its completion threads have counted up to it.
    const auto& box = arg(prim, "counter").tensor_box().box();
    const int64_t threshold = arg_int(prim, "threshold", env_);
    backend_->wait_counter(counter_id(box), threshold, prim.name());
    return;
  }
  if (target == "voyager::delinearize_index") {
    const int64_t linear = eval_int(prim.kwargs().at("linear").scalar(), env_);
    const auto basis =
        eval_int_list(prim.kwargs().at("basis").scalar_list(), env_);

    std::vector<int64_t> index(basis.size(), 0);
    int64_t remaining = linear;
    for (int d = static_cast<int>(basis.size()) - 1; d >= 0; d--) {
      index[d] = remaining % basis[d];
      remaining /= basis[d];
    }
    if (op.outputs_size() != static_cast<int>(basis.size())) {
      throw std::runtime_error("delinearize_index " + op.name() +
                               " has the wrong number of results.");
    }
    for (int d = 0; d < op.outputs_size(); d++) {
      env_.define(op.outputs(d).name(), index[d]);
    }
    return;
  }

  // Dispatch on what the operation produces, not on where it runs. An op tagged
  // "cpu" is host-side, but that covers both scalar index arithmetic and tensor
  // work the control processor does itself (a slice, a pad) -- and the latter
  // still needs a kernel, exactly like a datapath op.
  bool produces_only_scalars = op.outputs_size() > 0;
  for (const auto& output : op.outputs()) {
    if (!output.has_scalar()) produces_only_scalars = false;
  }

  if (produces_only_scalars) {
    execute_scalar(op, prim);
  } else {
    dispatch(op);
  }
}

void Interpreter::dispatch(const voyager::Operation& op) {
  // An operation the compiler tagged "cpu" runs on the control processor -- a
  // slice, a host-side pad. It costs the accelerator nothing.
  if (is_datapath(op)) {
    bool is_matrix = false;
    const long cycles = ideal_cycles(op, env_, is_matrix);
    (is_matrix ? matrix_cycles_ : vector_cycles_) += cycles;
  }

  backend_->execute(op, env_);
}

void Interpreter::execute_scalar(const voyager::Operation& op,
                                 const voyager::PrimOp& prim) {
  Scalar result;

  if (strip_namespace(prim.target()) == "_local_scalar_dense") {
    // A data-dependent scalar: the sparse flow reads CSR bookkeeping (an
    // indptr entry) out of memory to size and address the tiles that follow.
    // Read it in the box's own dtype -- these are integer indices.
    if (backend_->intercepts_scalar_reads()) {
      // A recording backend has not written that memory yet; it takes the
      // read and performs it at replay. The walk continues on a placeholder,
      // which is sound because everything downstream of this value is either
      // replayed against the value read then, or patched by the firmware from
      // live memory at run time.
      backend_->scalar_read(op, prim, env_);
      if (op.outputs_size() != 1) {
        throw std::runtime_error("Scalar operation " + op.name() +
                                 " must have exactly one result.");
      }
      env_.define(op.outputs(0).name(), int64_t{0});
      return;
    }
    const Tensor input = resolve(prim, "input", env_);
    if (get_size(input) != 1) {
      throw std::runtime_error("Scalar operation " + op.name() +
                               " reads more than one element.");
    }

    // to_int64 returns ac_int's Slong, which is long long -- a distinct type
    // from Scalar's int64_t alternative even where both are 64 bits wide.
    // Name the alternative rather than leave the variant's converting
    // assignment to pick one; which conversions it will consider is a point
    // the C++17 compilers here disagree on.
    std::any data = memory_->read_tensor(input);
    if (auto* value =
            std::any_cast<std::shared_ptr<DataTypes::int32[]>>(&data)) {
      result = static_cast<int64_t>((*value)[0].int_val.to_int64());
    } else if (auto* value =
                   std::any_cast<std::shared_ptr<DataTypes::int64[]>>(&data)) {
      result = static_cast<int64_t>((*value)[0].int_val.to_int64());
    } else {
      throw std::runtime_error("Scalar operation " + op.name() +
                               " reads a tensor of non-index dtype " +
                               input.dtype + ".");
    }
  } else {
    result = eval_scalar_prim(prim, env_);
    // A recording backend needs the ops downstream of a deferred read as well
    // as the read itself: their values are derived from a placeholder here and
    // have to be recomputed when the real one is available.
    if (backend_->intercepts_scalar_reads()) {
      backend_->scalar_op(op, prim, env_);
    }
  }

  if (op.outputs_size() != 1) {
    throw std::runtime_error("Scalar operation " + op.name() +
                             " must have exactly one result.");
  }
  env_.define(op.outputs(0).name(), result);
}

Scalar eval_scalar_prim(const voyager::PrimOp& prim, const ScalarEnv& env) {
  const std::string target = strip_namespace(prim.target());
  if (target == "sym_ite") {
    const bool predicate = to_bool(eval(prim.kwargs().at("b").scalar(), env));
    return eval(prim.kwargs().at(predicate ? "t" : "f").scalar(), env);
  }
  const auto input = prim.kwargs().find("input");
  const auto other = prim.kwargs().find("other");
  if (input == prim.kwargs().end() || other == prim.kwargs().end()) {
    throw std::runtime_error("Scalar operation " + prim.name() + " (" + target +
                             ") is not binary.");
  }
  return apply_scalar_op(target, eval(input->second.scalar(), env),
                         eval(other->second.scalar(), env));
}

// voyager::zeros writes an existing buffer in place: its `input` operand. A
// counter is reset to 0; a memory buffer is zero-filled, every slot of it
// when the operand is the whole allocation.
void Interpreter::execute_zeros(const voyager::Operation& op,
                                const voyager::PrimOp& prim) {
  const voyager::TensorBoxRef* ref = &arg(prim, "input").tensor_box();
  const voyager::TensorBox& box = ref->box();
  if (is_counter(box)) {
    const int64_t id = counter_id(box);
    counters_[id] = box.node();
    backend_->reset_counter(id, box.node());
    return;
  }
  if (backend_->intercepts_data_ops()) {
    backend_->data_op(op, prim, env_);
    return;
  }
  if (ref->offsets_size() == 0) {
    zero_buffer(to_tensor(box), banks_of(box), bank_stride_of(box), memory_);
  } else {
    zero_buffer(resolve(*ref, env_, op.name()), 1, 0, memory_);
  }
}

// A commit: dispatch a region asynchronously. Each wait gates one unit's pass
// on a counter reaching its threshold; the post is the counter the last pass
// adds 1 to. The walk of the body is identical for every backend -- scalars
// and conditionals resolve here, and the datapath/host ops flow through
// backend->execute(). What differs is the backend: a sequential one checks
// the waits, runs the body in place and adds the post at once; the Harness
// attaches the waits and the post to the passes the body dispatches.
void Interpreter::execute_async(const voyager::Operation& op,
                                const voyager::AsyncOp& async) {
  CommitSync sync;
  sync.op = op.name();
  for (const auto& wait : async.waits()) {
    const auto& box = wait.counter().box();
    sync.waits.push_back({wait.unit(), counter_id(box),
                          eval_int(wait.threshold(), env_), box.node()});
  }
  if (async.has_post()) {
    const auto& box = async.post().box();
    sync.post = counter_id(box);
    sync.post_node = box.node();
  }

  backend_->begin_commit(sync);
  execute_region(async.body());
  backend_->end_commit();
}

void Interpreter::execute_for(const voyager::Operation& op,
                              const voyager::ForLoop& loop) {
  const int64_t start = eval_int(loop.start(), env_);
  int64_t end = eval_int(loop.end(), env_);
  const int64_t step = eval_int(loop.step(), env_);
  if (step == 0) throw std::runtime_error("Zero-step loop: " + op.name());

  // MAX_TILES bounds outer tile iterations, which is how regression tests keep
  // very large layers workable. Never apply it to a nested reduction/index
  // loop: truncating the reduction produces an invalid partial output and can
  // leave the software-pipeline drain waiting for a bank that was never posted.
  const int64_t max_tiles = getenv_int("MAX_TILES", 0);
  const bool outermost_loop = loop_depth_ == 0;
  if (max_tiles > 0 && bound_current_top_ && outermost_loop && step > 0) {
    end = std::min(end, start + max_tiles * step);
  }

  std::vector<Scalar> carried;
  for (const auto& arg : loop.iter_args()) {
    carried.push_back(eval(arg.initial(), env_));
  }

  loop_depth_++;
  for (int64_t iv = start; step > 0 ? iv < end : iv > end; iv += step) {
    env_.push();
    env_.define(loop.iv(), iv);
    for (int i = 0; i < loop.iter_args_size(); i++) {
      env_.define(loop.iter_args(i).name(), carried[i]);
    }

    execute_region(loop.body());

    // The body yields the loop-carried state for the next trip. Read it before
    // the scope closes, since the yielded names are defined inside it.
    if (loop.body().yields_size() != loop.iter_args_size()) {
      throw std::runtime_error("Loop " + op.name() +
                               " yields a different number of values than it "
                               "carries.");
    }
    for (int i = 0; i < loop.body().yields_size(); i++) {
      carried[i] = eval(loop.body().yields(i), env_);
    }
    env_.pop();
  }
  loop_depth_--;

  // The final loop-carried values are the loop's results.
  for (int i = 0; i < op.outputs_size() && i < static_cast<int>(carried.size());
       i++) {
    env_.define(op.outputs(i).name(), carried[i]);
  }
}

void Interpreter::execute_while(const voyager::Operation& op,
                                const voyager::WhileLoop& loop) {
  std::vector<Scalar> carried;
  for (const auto& arg : loop.iter_args()) {
    carried.push_back(eval(arg.initial(), env_));
  }

  const int64_t max_tiles = getenv_int("MAX_TILES", 0);
  const bool outermost_loop = loop_depth_ == 0;
  int64_t trips = 0;
  loop_depth_++;
  while (true) {
    if (max_tiles > 0 && bound_current_top_ && outermost_loop &&
        trips >= max_tiles) {
      break;
    }

    env_.push();
    for (int i = 0; i < loop.iter_args_size(); i++) {
      env_.define(loop.iter_args(i).name(), carried[i]);
    }

    execute_region(loop.condition());
    if (loop.condition().yields_size() != 1) {
      throw std::runtime_error("While loop " + op.name() +
                               " must yield exactly one predicate.");
    }
    if (!to_bool(eval(loop.condition().yields(0), env_))) {
      env_.pop();
      break;
    }

    execute_region(loop.body());
    for (int i = 0; i < loop.body().yields_size(); i++) {
      carried[i] = eval(loop.body().yields(i), env_);
    }
    env_.pop();
    trips++;
  }
  loop_depth_--;

  for (int i = 0; i < op.outputs_size() && i < static_cast<int>(carried.size());
       i++) {
    env_.define(op.outputs(i).name(), carried[i]);
  }
}

void Interpreter::execute_cond(const voyager::Operation& op,
                               const voyager::CondOp& cond) {
  const bool predicate = to_bool(eval(cond.predicate(), env_));
  const voyager::Region& region =
      predicate ? cond.true_region() : cond.false_region();

  env_.push();
  execute_region(region);

  // A conditional that produces scalars binds them in the enclosing scope.
  std::vector<Scalar> yielded;
  for (const auto& yield : region.yields())
    yielded.push_back(eval(yield, env_));
  env_.pop();

  for (int i = 0; i < op.outputs_size() && i < static_cast<int>(yielded.size());
       i++) {
    env_.define(op.outputs(i).name(), yielded[i]);
  }
}
