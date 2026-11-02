#include "Harness.h"

#include <mc_connections.h>
#include <systemc.h>

#include <cassert>
#include <cstdlib>
#include <memory>

#include "AccelTypes.h"
#include "sysc/kernel/sc_time.h"

#ifndef CFLOAT
#include "test/gold/GoldModel.h"
#include "toolchain/MapOperation.h"

Harness::Harness(sc_module_name name, const Model& model,
                 const Model::Selection& selection, MemoryInterface* memory)
    : sc_module(name),
      clk("clk", std::stod(getenv("CLOCK_PERIOD", "1")), SC_NS, 0.5, 0, SC_NS,
          true),
      model(model),
      selection(selection),
      memory(memory) {
  accelerator.clk(clk);
  accelerator.rstn(rstn);
  accelerator.matrix_unit_params_in(matrix_unit_params_in);
  accelerator.matrix_unit_input_req(matrix_unit_input_req);
  accelerator.matrix_unit_input_resp(matrix_unit_input_resp);
  accelerator.matrix_unit_weight_req(matrix_unit_weight_req);
  accelerator.matrix_unit_weight_resp(matrix_unit_weight_resp);
  accelerator.matrix_unit_bias_req(matrix_unit_bias_req);
  accelerator.matrix_unit_bias_resp(matrix_unit_bias_resp);
#if SUPPORT_MX
  accelerator.matrix_unit_input_scale_req(matrix_unit_input_scale_req);
  accelerator.matrix_unit_input_scale_resp(matrix_unit_input_scale_resp);
  accelerator.matrix_unit_weight_scale_req(matrix_unit_weight_scale_req);
  accelerator.matrix_unit_weight_scale_resp(matrix_unit_weight_scale_resp);
#endif
  accelerator.matrix_unit_output_data(matrix_unit_output_data);
  accelerator.matrix_unit_output_addr(matrix_unit_output_addr);
  accelerator.matrix_unit_start(matrix_unit_start);
  accelerator.matrix_unit_done(matrix_unit_done);
#if SUPPORT_MVM
  accelerator.matrix_vector_unit_params_in(matrix_vector_unit_params_in);
  accelerator.matrix_vector_unit_input_req(matrix_vector_unit_input_req);
  accelerator.matrix_vector_unit_input_resp(matrix_vector_unit_input_resp);
  accelerator.matrix_vector_unit_weight_req(matrix_vector_unit_weight_req);
  accelerator.matrix_vector_unit_weight_resp(matrix_vector_unit_weight_resp);
  accelerator.matrix_vector_unit_bias_req(matrix_vector_unit_bias_req);
  accelerator.matrix_vector_unit_bias_resp(matrix_vector_unit_bias_resp);
#if SUPPORT_MX
  accelerator.matrix_vector_unit_input_scale_req(
      matrix_vector_unit_input_scale_req);
  accelerator.matrix_vector_unit_input_scale_resp(
      matrix_vector_unit_input_scale_resp);
  accelerator.matrix_vector_unit_weight_scale_req(
      matrix_vector_unit_weight_scale_req);
  accelerator.matrix_vector_unit_weight_scale_resp(
      matrix_vector_unit_weight_scale_resp);
#endif
  accelerator.matrix_vector_unit_weight_dq_scale_req(
      matrix_vector_unit_weight_dq_scale_req);
  accelerator.matrix_vector_unit_weight_dq_scale_resp(
      matrix_vector_unit_weight_dq_scale_resp);
  accelerator.matrix_vector_unit_weight_dq_zp_req(
      matrix_vector_unit_weight_dq_zp_req);
  accelerator.matrix_vector_unit_weight_dq_zp_resp(
      matrix_vector_unit_weight_dq_zp_resp);
  accelerator.matrix_vector_unit_start(matrix_vector_unit_start);
  accelerator.matrix_vector_unit_done(matrix_vector_unit_done);
#endif
#if SUPPORT_SPMM
  accelerator.spmm_unit_params_in(spmm_unit_params_in);
  accelerator.spmm_unit_input_indptr_req(spmm_unit_input_indptr_req);
  accelerator.spmm_unit_input_indptr_resp(spmm_unit_input_indptr_resp);
  accelerator.spmm_unit_input_indices_req(spmm_unit_input_indices_req);
  accelerator.spmm_unit_input_indices_resp(spmm_unit_input_indices_resp);
  accelerator.spmm_unit_input_data_req(spmm_unit_input_data_req);
  accelerator.spmm_unit_input_data_resp(spmm_unit_input_data_resp);
  accelerator.spmm_unit_weight_req(spmm_unit_weight_req);
  accelerator.spmm_unit_weight_resp(spmm_unit_weight_resp);
#if SUPPORT_MX
  accelerator.spmm_unit_weight_scale_req(spmm_unit_weight_scale_req);
  accelerator.spmm_unit_weight_scale_resp(spmm_unit_weight_scale_resp);
#endif
  accelerator.spmm_unit_start(spmm_unit_start);
  accelerator.spmm_unit_done(spmm_unit_done);
#endif
#if SUPPORT_DWC
  accelerator.dwc_unit_params_in(dwc_unit_params_in);
  accelerator.dwc_unit_input_req(dwc_unit_input_req);
  accelerator.dwc_unit_input_resp(dwc_unit_input_resp);
  accelerator.dwc_unit_weight_req(dwc_unit_weight_req);
  accelerator.dwc_unit_weight_resp(dwc_unit_weight_resp);
  accelerator.dwc_unit_bias_req(dwc_unit_bias_req);
  accelerator.dwc_unit_bias_resp(dwc_unit_bias_resp);
#if SUPPORT_MX
  accelerator.dwc_unit_input_scale_req(dwc_unit_input_scale_req);
  accelerator.dwc_unit_input_scale_resp(dwc_unit_input_scale_resp);
  accelerator.dwc_unit_weight_scale_req(dwc_unit_weight_scale_req);
  accelerator.dwc_unit_weight_scale_resp(dwc_unit_weight_scale_resp);
#endif
  accelerator.dwc_unit_start(dwc_unit_start);
  accelerator.dwc_unit_done(dwc_unit_done);
#endif
  accelerator.vector_unit_params_in(vector_unit_params_in);
  accelerator.vector_fetch_0_req(vector_fetch_0_req);
  accelerator.vector_fetch_0_resp(vector_fetch_0_resp);
  accelerator.vector_fetch_1_req(vector_fetch_1_req);
  accelerator.vector_fetch_1_resp(vector_fetch_1_resp);
  accelerator.vector_fetch_2_req(vector_fetch_2_req);
  accelerator.vector_fetch_2_resp(vector_fetch_2_resp);
  accelerator.vector_output_data(vector_output_data);
  accelerator.vector_output_addr(vector_output_addr);
  accelerator.mx_scale_output_data(mx_scale_output_data);
  accelerator.mx_scale_output_addr(mx_scale_output_addr);
  accelerator.sparse_tensor_output_data(sparse_tensor_output_data);
  accelerator.sparse_tensor_output_addr(sparse_tensor_output_addr);
  accelerator.vector_unit_start(vector_unit_start);
  accelerator.vector_unit_done(vector_unit_done);

  SC_CTHREAD(reset, clk);

#define REGISTER_FN(NAME)           \
  SC_THREAD(NAME);                  \
  sensitive << clk.posedge_event(); \
  async_reset_signal_is(rstn, false);

#define REGISTER_IO_FN(NAME)          \
  REGISTER_FN(read_##NAME##_request); \
  REGISTER_FN(send_##NAME##_response);

  REGISTER_IO_FN(matrix_unit_input)
  REGISTER_IO_FN(matrix_unit_weight)
  REGISTER_IO_FN(matrix_unit_bias)
#if SUPPORT_MX
  REGISTER_IO_FN(matrix_unit_input_scale)
  REGISTER_IO_FN(matrix_unit_weight_scale)
#endif

#if SUPPORT_MVM
  REGISTER_IO_FN(matrix_vector_unit_input)
  REGISTER_IO_FN(matrix_vector_unit_weight)
  REGISTER_IO_FN(matrix_vector_unit_bias)
#if SUPPORT_MX
  REGISTER_IO_FN(matrix_vector_unit_input_scale)
  REGISTER_IO_FN(matrix_vector_unit_weight_scale)
#endif
  REGISTER_IO_FN(matrix_vector_unit_weight_dq_scale)
  REGISTER_IO_FN(matrix_vector_unit_weight_dq_zp)
#endif

#if SUPPORT_SPMM
  REGISTER_IO_FN(spmm_unit_input_indptr)
  REGISTER_IO_FN(spmm_unit_input_indices)
  REGISTER_IO_FN(spmm_unit_input_data)
  REGISTER_IO_FN(spmm_unit_weight)
#if SUPPORT_MX
  REGISTER_IO_FN(spmm_unit_weight_scale)
#endif
#endif

#if SUPPORT_DWC
  REGISTER_IO_FN(dwc_unit_input)
  REGISTER_IO_FN(dwc_unit_weight)
  REGISTER_IO_FN(dwc_unit_bias)
#if SUPPORT_MX
  REGISTER_IO_FN(dwc_unit_input_scale)
  REGISTER_IO_FN(dwc_unit_weight_scale)
#endif
#endif

  REGISTER_IO_FN(vector_fetch_0)
  REGISTER_IO_FN(vector_fetch_1)
  REGISTER_IO_FN(vector_fetch_2)

  REGISTER_FN(store_matrix_unit_output)
  REGISTER_FN(store_vector_output)
  REGISTER_FN(store_mx_scale_output)
  REGISTER_FN(store_sparse_tensor_output)

#define REGISTER_UNIT_FN(NAME)   \
  REGISTER_FN(NAME##_starts);    \
  REGISTER_FN(NAME##_dones);

  REGISTER_FN(run_walker)
  REGISTER_UNIT_FN(matrix_unit)
  REGISTER_UNIT_FN(vector_unit)
#if SUPPORT_MVM
  REGISTER_UNIT_FN(matrix_vector_unit)
#endif
#if SUPPORT_SPMM
  REGISTER_UNIT_FN(spmm_unit)
#endif
#if SUPPORT_DWC
  REGISTER_UNIT_FN(dwc_unit)
#endif

  access_counter = new AccessCounter();
// do not set access counters for an RTL simulation
#ifndef CCS_DUT_RTL
  accelerator.matrix_unit.input_buffer.access_counter = access_counter;
  accelerator.matrix_unit.weight_buffer.access_counter = access_counter;
#endif
}

template <int width>
void Harness::process_read_request(
    Connections::Combinational<MemoryRequest>* request_out,
    sc_fifo<ac_int<width, false>>* data_fifo) {
  request_out->ResetRead();

  constexpr int num_bytes = width / 8;

  // Every datapath operand lives on chip: the graph's async_copies stage it
  // there before the operation runs, and stage the results back out.
  const auto array_memory = (ArrayMemory*)(this->memory);
  char* memory = array_memory->memories[SRAM_PARTITION];

  wait();

  while (true) {
    MemoryRequest request = request_out->Pop();

    uint64_t base_address = request.address;
    int total_bytes = request.burst_size;
    int num_words = (total_bytes + num_bytes - 1) / num_bytes;

    access_counter->increment(std::string(name()), total_bytes);

    ac_int<width, false> bits;

    for (int i = 0; i < num_words; i++) {
      for (int j = 0; j < num_bytes; j++) {
        uint64_t address = base_address + i * num_bytes + j;
        bits.set_slc(j * 8, static_cast<ac_int<8, false>>(memory[address]));
      }

      data_fifo->write(bits);
    }
  }
}

template <int width>
void Harness::send_data_response(
    sc_fifo<ac_int<width, false>>* data_fifo,
    Connections::Combinational<ac_int<width, false>>* response) {
  response->ResetWrite();

  wait();

  while (true) {
    response->Push(data_fifo->read());
  }
}

template <int width>
void Harness::process_write_request(
    Connections::Combinational<ac_int<width, false>>* data_out,
    Connections::Combinational<ac_int<ADDRESS_WIDTH, false>>* address_out) {
  data_out->ResetRead();
  address_out->ResetRead();

  constexpr int num_bytes = width / 8;

  wait();

  while (true) {
    uint64_t address = address_out->Pop();
    auto data = data_out->Pop();

    access_counter->increment(std::string(name()) + "_" + "outputs", num_bytes);

    char bytes[num_bytes];
    for (int i = 0; i < num_bytes; i++) {
      bytes[i] = data.template slc<8>(i * 8);
    }
    this->memory->write_bytes_to_memory(address, SRAM_PARTITION, num_bytes,
                                        bytes);
  }
}

#define DEFINE_IO_FN(NAME)                                \
  void Harness::read_##NAME##_request() {                 \
    process_read_request(&NAME##_req, &NAME##_resp_fifo); \
  }                                                       \
                                                          \
  void Harness::send_##NAME##_response() {                \
    send_data_response(&NAME##_resp_fifo, &NAME##_resp);  \
  }

DEFINE_IO_FN(matrix_unit_input)
DEFINE_IO_FN(matrix_unit_weight)
DEFINE_IO_FN(matrix_unit_bias)
#if SUPPORT_MX
DEFINE_IO_FN(matrix_unit_input_scale)
DEFINE_IO_FN(matrix_unit_weight_scale)
#endif

#if SUPPORT_MVM
DEFINE_IO_FN(matrix_vector_unit_input)
DEFINE_IO_FN(matrix_vector_unit_weight)
DEFINE_IO_FN(matrix_vector_unit_bias)
#if SUPPORT_MX
DEFINE_IO_FN(matrix_vector_unit_input_scale)
DEFINE_IO_FN(matrix_vector_unit_weight_scale)
#endif
DEFINE_IO_FN(matrix_vector_unit_weight_dq_scale)
DEFINE_IO_FN(matrix_vector_unit_weight_dq_zp)
#endif

#if SUPPORT_SPMM
DEFINE_IO_FN(spmm_unit_input_indptr)
DEFINE_IO_FN(spmm_unit_input_indices)
DEFINE_IO_FN(spmm_unit_input_data)
DEFINE_IO_FN(spmm_unit_weight)
#if SUPPORT_MX
DEFINE_IO_FN(spmm_unit_weight_scale)
#endif
#endif

#if SUPPORT_DWC
DEFINE_IO_FN(dwc_unit_input)
DEFINE_IO_FN(dwc_unit_weight)
DEFINE_IO_FN(dwc_unit_bias)
#if SUPPORT_MX
DEFINE_IO_FN(dwc_unit_input_scale)
DEFINE_IO_FN(dwc_unit_weight_scale)
#endif
#endif

DEFINE_IO_FN(vector_fetch_0)
DEFINE_IO_FN(vector_fetch_1)
DEFINE_IO_FN(vector_fetch_2)

void Harness::store_matrix_unit_output() {
  process_write_request(&matrix_unit_output_data, &matrix_unit_output_addr);
}

void Harness::store_vector_output() {
  process_write_request(&vector_output_data, &vector_output_addr);
}

void Harness::store_mx_scale_output() {
  process_write_request(&mx_scale_output_data, &mx_scale_output_addr);
}

void Harness::store_sparse_tensor_output() {
  process_write_request(&sparse_tensor_output_data, &sparse_tensor_output_addr);
}

void Harness::reset() {
  rstn.write(0);
  wait(5);
  rstn.write(1);
  wait();
}

template <typename T, unsigned int width>
void send_serialized_params(
    T params,
    Connections::Combinational<ac_int<width, false>>* serial_params_in) {
  ac_int<T::width, false> serialized_params;
  vector_to_type(TypeToBits<T>(params), false, &serialized_params);

  // round up to the nearest multiple of width
  ac_int<((T::width + width - 1) / width) * width, false> padded_params =
      serialized_params;

  for (int i = 0; i < padded_params.width / width; i++) {
    serial_params_in->Push(padded_params.template slc<width>(i * width));
  }
}

template <typename T>
T* get_param(const std::deque<BaseParams*>& params, int& index) {
  if (index < params.size()) {
    if (auto* casted = dynamic_cast<T*>(params[index])) {
      index++;  // Advance index only if the cast succeeds
      return casted;
    }
  }
  return nullptr;
}

// Serialize one operation's params and queue a pass on each unit it drives.
//
// The walk only pushes params -- the units' deserializers hand them to the
// double-buffered controllers, so the next tile's input and weight fetches
// begin while the current tile computes -- and the unit threads pace the
// handshakes. Groups within one operation are the passes of a multi-pass op
// (softmax and layer_norm expand into three vector passes, a strided
// quantize_mx into a scale pass and a main pass); each unit runs its passes in
// queue order. A unit can start a pass before the previous one has finished
// writing, so a pass also waits for the previous pass of the same operation
// on its unit to retire: the ordering the SoC emitter gives those passes
// with counter 31 (the IR's waits order passes of different operations).
//
// Inside a commit, the commit's waits on a unit go to the first group that
// starts that unit -- "matrix" being the matrix-class units the params select:
// the systolic array or the MVM, and the SpMM unit, each of which gets every
// "matrix" wait -- and its post fires when its last group retires. A commit
// around a DwC dispatch may carry neither.
void Harness::dispatch_params(const voyager::Operation& op,
                              const std::deque<BaseParams*>& params) {
  auto timing = std::make_shared<OpTiming>();
  timing->op = &op;
  // The sequence number of this operation's latest pass on each unit.
  std::map<Unit*, long> previous_pass;

  int idx = 0;
  while (idx < params.size()) {
    std::vector<Unit*> matrix_class;
    std::vector<Unit*> unsynchronized;
    bool vector = false;

    if (auto* matrix_params = get_param<MatrixParams>(params, idx)) {
#if SUPPORT_MVM
      if (matrix_params->is_fc) {
        send_serialized_params<MatrixParams, 64>(*matrix_params,
                                                 &matrix_vector_unit_params_in);
        matrix_class.push_back(&matrix_vector_unit_queue);
      } else
#endif
#if SUPPORT_SPMM
          if (matrix_params->is_spmm) {
        // SpMM operation could have a fused dense matrix op
        if (auto* dense_params = get_param<MatrixParams>(params, idx)) {
          send_serialized_params<MatrixParams, 64>(*dense_params,
                                                   &matrix_unit_params_in);
          matrix_class.push_back(&matrix_unit_queue);
        }
        send_serialized_params<MatrixParams, 64>(*matrix_params,
                                                 &spmm_unit_params_in);
        matrix_class.push_back(&spmm_unit_queue);
      } else
#endif
      {
        send_serialized_params<MatrixParams, 64>(*matrix_params,
                                                 &matrix_unit_params_in);
        matrix_class.push_back(&matrix_unit_queue);
      }
    } else if (auto* dwc_params = get_param<DwCParams>(params, idx)) {
#if SUPPORT_DWC
      send_serialized_params<DwCParams, 64>(*dwc_params, &dwc_unit_params_in);
      unsynchronized.push_back(&dwc_unit_queue);
#else
      throw std::runtime_error("DWC support is not enabled.");
#endif
    }

    if (auto* vector_params = get_param<VectorParams>(params, idx)) {
      send_serialized_params<VectorParams, 64>(*vector_params,
                                               &vector_unit_params_in);

      auto* vector_config = get_param<VectorInstructionConfig>(params, idx);
      send_serialized_params<VectorInstructionConfig, 64>(
          *vector_config, &vector_unit_params_in);
      vector = true;
    }

    auto group = std::make_shared<Group>();
    group->timing = timing;
    group->commit = open_commit;
    group->last = idx >= params.size();

    std::vector<CounterWait> matrix_waits;
    std::vector<CounterWait> vector_waits;
    if (open_commit) {
      Commit& commit = *open_commit;
      commit.groups++;
      commit.groups_left++;
      const bool synchronized = commit.sync.post || !commit.sync.waits.empty();
      if (synchronized && !unsynchronized.empty()) {
        throw std::runtime_error(commit.sync.op +
                                 ": a wait or a post on a DwC dispatch, "
                                 "which takes neither.");
      }
      for (const auto& wait : commit.sync.waits) {
        if (wait.unit == "matrix") {
          if (!matrix_class.empty() && !commit.matrix_waits_taken) {
            matrix_waits.push_back(wait);
          }
        } else if (wait.unit == "vector") {
          if (vector && !commit.vector_waits_taken) {
            vector_waits.push_back(wait);
          }
        } else {
          throw std::runtime_error(commit.sync.op + ": wait on counter " +
                                   wait.node + " names the unit \"" +
                                   wait.unit + "\", not matrix or vector.");
        }
      }
      if (!matrix_class.empty()) commit.matrix_waits_taken = true;
      if (vector) commit.vector_waits_taken = true;
    }

    std::vector<std::pair<Unit*, std::vector<CounterWait>>> passes;
    for (Unit* unit : matrix_class) passes.emplace_back(unit, matrix_waits);
    for (Unit* unit : unsynchronized) {
      passes.emplace_back(unit, std::vector<CounterWait>{});
    }
    if (vector) passes.emplace_back(&vector_unit_queue, vector_waits);
    if (passes.empty()) {
      throw std::runtime_error(op.name() + ": params entry " +
                               std::to_string(idx) + " drives no unit.");
    }

    group->passes_left = static_cast<int>(passes.size());
    for (auto& [unit, waits] : passes) {
      const auto earlier = previous_pass.find(unit);
      const long after = earlier == previous_pass.end() ? -1 : earlier->second;
      previous_pass[unit] = unit->dispatched++;
      unit->to_start.push_back({std::move(waits), group, after});
      unit->pushed.notify(SC_ZERO_TIME);
    }
  }
}

// Start a unit's queued passes in order. A pass starts once every wait it
// carries holds; SyncPop then completes when the unit offers its start
// handshake -- the matrix unit raises it from the processor at the front of
// its pipeline, so a start can land while the previous pass still drains
// through the accumulation buffer and output controller behind it.
void Harness::drive_starts(Unit& unit) {
  unit.start->ResetRead();

  wait();

  while (true) {
    while (unit.to_start.empty()) wait(unit.pushed);
    Pass pass = std::move(unit.to_start.front());
    unit.to_start.pop_front();

    for (const auto& wait_on : pass.waits) {
      while (counters_[wait_on.counter] < wait_on.threshold) {
        wait(counter_changed[wait_on.counter]);
      }
    }
    while (unit.retired <= pass.after) wait(unit.pass_retired);
    unit.start->SyncPop();

    OpTiming& timing = *pass.group->timing;
    if (!timing.started) {
      timing.started = true;
      timing.start = sc_time_stamp();
      CCS_LOG("----- Accelerator Layer '" << timing.op->name()
                                          << "' Started. -----");
    }

    unit.to_retire.push_back(std::move(pass));
    unit.started.notify(SC_ZERO_TIME);
  }
}

// Retire a unit's passes in the order they started: pop the done handshake,
// and when the pass is the last of its group to finish, fire what the group
// carried -- its commit's post once the walk has closed the commit, and the
// per-op logging.
void Harness::drive_dones(Unit& unit) {
  unit.done->ResetRead();

  wait();

  while (true) {
    while (unit.to_retire.empty()) wait(unit.started);
    Pass pass = std::move(unit.to_retire.front());
    unit.to_retire.pop_front();

    unit.done->SyncPop();
    unit.retired++;
    unit.pass_retired.notify(SC_ZERO_TIME);

    Group& group = *pass.group;
    if (--group.passes_left > 0) continue;

    if (group.commit) {
      Commit& commit = *group.commit;
      if (--commit.groups_left == 0 && commit.closed) post_commit(commit);
    }

    if (group.last) {
      const sc_time end = sc_time_stamp();
      const OpTiming& timing = *group.timing;
      CCS_LOG("----- Accelerator Layer '" << timing.op->name()
                                          << "' Finished. -----");
      // to_default_time_units returns a double; print whole nanoseconds so
      // large values do not switch to scientific notation.
      std::cout << "Runtime: "
                << static_cast<long long>(end.to_default_time_units() -
                                          timing.start.to_default_time_units())
                << " ns" << std::endl;
      if (timing.op->has_tiling()) {
        access_counter->print_summary(timing.op->tiling(), true);
      }
    }
  }
}

#define DEFINE_UNIT_FN(NAME)                                    \
  void Harness::NAME##_starts() { drive_starts(NAME##_queue); } \
  void Harness::NAME##_dones() { drive_dones(NAME##_queue); }

DEFINE_UNIT_FN(matrix_unit)
DEFINE_UNIT_FN(vector_unit)
#if SUPPORT_MVM
DEFINE_UNIT_FN(matrix_vector_unit)
#endif
#if SUPPORT_SPMM
DEFINE_UNIT_FN(spmm_unit)
#endif
#if SUPPORT_DWC
DEFINE_UNIT_FN(dwc_unit)
#endif

void Harness::post_commit(Commit& commit) {
  if (!commit.sync.post) return;
  posts_pending[*commit.sync.post]--;
  add_counter(*commit.sync.post);
}

void Harness::begin_commit(const CommitSync& sync) {
  if (open_commit) {
    throw std::runtime_error(sync.op + ": a commit inside commit " +
                             open_commit->sync.op + ".");
  }
  open_commit = std::make_shared<Commit>();
  open_commit->sync = sync;
  if (sync.post) posts_pending[*sync.post]++;
}

// The walk reaches end_commit as soon as it has pushed the body's params.
// Every wait must have gone to a pass -- a wait on a unit the body never
// dispatches would gate some later dispatch instead -- and the post needs a
// pass to fire from.
void Harness::end_commit() {
  Commit& commit = *open_commit;
  for (const auto& wait : commit.sync.waits) {
    const bool taken = wait.unit == "matrix" ? commit.matrix_waits_taken
                                             : commit.vector_waits_taken;
    if (!taken) {
      throw std::runtime_error(commit.sync.op + ": " + wait.unit +
                               " wait on counter " + wait.node +
                               ", but the commit dispatches no " + wait.unit +
                               " pass.");
    }
  }
  if (commit.sync.post && commit.groups == 0) {
    throw std::runtime_error(commit.sync.op + ": posts counter " +
                             commit.sync.post_node +
                             " but dispatches nothing.");
  }
  commit.closed = true;
  if (commit.groups_left == 0) post_commit(commit);
  open_commit.reset();
}

void Harness::reset_counter(int64_t id, const std::string& node) {
  if (posts_pending[id] > 0) {
    throw std::runtime_error(
        "counter " + node + " (id " + std::to_string(id) + ") reset while " +
        std::to_string(posts_pending[id]) +
        " dispatched pass(es) have yet to add to it.");
  }
  Backend::reset_counter(id, node);
}

void Harness::add_counter(int64_t id) {
  Backend::add_counter(id);
  counter_changed[id].notify(SC_ZERO_TIME);
}

// Called from the walk; blocks until the unit threads (or the walk's own
// copies) have counted up to the threshold.
void Harness::wait_counter(int64_t id, int64_t threshold,
                           const std::string& op) {
  while (counters_[id] < threshold) wait(counter_changed[id]);
}

// Drives the DUT. The bufferized graph already states the tile loops, the
// reduction conditionals and every DMA, so there is nothing left for the
// harness to synthesize: it walks the graph exactly as the gold model does and,
// when the walk reaches an accelerator operation, hands it to the hardware
// instead of to a kernel.
void Harness::run_walker() {
  matrix_unit_params_in.ResetWrite();
  vector_unit_params_in.ResetWrite();
#if SUPPORT_MVM
  matrix_vector_unit_params_in.ResetWrite();
#endif
#if SUPPORT_SPMM
  spmm_unit_params_in.ResetWrite();
#endif
#if SUPPORT_DWC
  dwc_unit_params_in.ResetWrite();
#endif

  wait();

  const sc_time start = sc_time_stamp();

  Interpreter interpreter(model, memory, this);
  interpreter.run(selection);

  // Every kernel's epilogue waits each counter at its final count, so the
  // walk returns with every pass retired.
  interpreter.log_counters();

  const sc_time end = sc_time_stamp();
  std::cout << "Total Runtime: "
            << static_cast<long long>(end.to_default_time_units() -
                                      start.to_default_time_units())
            << " ns" << std::endl;

  sc_stop();
}

void Harness::execute(const voyager::Operation& op, const ScalarEnv& env) {
  const std::vector<Tensor> outputs = resolve_outputs(op, env);

  // Anything the compiler tagged "cpu" runs on the control processor, not the
  // datapath -- a slice, a host-side pad. Those still need a kernel, so let the
  // gold model do them in place, exactly as it does in the gold flow. The
  // IR's CPU waits before one ensure it reads nothing a pass is still writing.
  if (!is_datapath(op)) {
    run_host_operation(op, env, memory);
    return;
  }

  std::deque<BaseParams*> params;
  map_operation(op, env, params);

#ifndef __SYNTHESIS__
  if (std::getenv("PRINT_PARAMS")) {
    std::cout << "===== params for " << op.name() << " (" << params.size()
              << ") =====" << std::endl;
    for (auto* p : params) {
      if (auto* mp = dynamic_cast<MatrixParams*>(p)) std::cout << *mp;
      if (auto* vp = dynamic_cast<VectorParams*>(p)) std::cout << *vp;
    }
  }
#endif

  // Synchronous and committed dispatches alike only queue their passes: the
  // IR's counter waits order them against everything else.
  dispatch_params(op, params);

  for (auto* param : params) delete param;
}

#endif  // CFLOAT: the harness instantiates the SystemC Accelerator, which the
        // CFloat datatype cannot compile. run_accelerator below just aborts.

void run_accelerator(const Model& model, const Model::Selection& selection,
                     MemoryInterface* memory) {
#ifdef CFLOAT
  spdlog::error(
      "The SystemC model does not support the CFloat datatype. Only the gold "
      "model should be used for CFloat.\n");
  std::abort();
#else
  // CONNECTIONS_FAST_SIM makes Harness larger than the default process stack
  // (about 12 MiB for a 64x64 MX build). Construct it on the heap so entering
  // run_accelerator cannot overflow the stack before the constructor runs.
  auto harness = std::make_unique<Harness>("harness", model, selection, memory);
  sc_start();
#endif
}
