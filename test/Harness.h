#pragma once

#include <mc_connections.h>
#include <mc_scverify.h>
#include <systemc.h>

#include <deque>
#include <memory>
#include <vector>

#include "AccelTypes.h"
#include "ArchitectureParams.h"
#include "test/AccessCounter.h"
#include "runtime/ArrayMemory.h"
#include "runtime/Backend.h"
#include "runtime/Interpreter.h"
#include "runtime/Model.h"
#include "runtime/Utils.h"

#ifndef CFLOAT
#include "Accelerator.h"

struct Harness : public sc_module, public Backend {
  sc_clock CCS_INIT_S1(clk);
  sc_signal<bool> CCS_INIT_S1(rstn);

  //----------------------------------------------------------
  // MATRIX UNIT CONNECTIONS
  //----------------------------------------------------------

  Connections::Combinational<ac_int<64, false>> CCS_INIT_S1(
      matrix_unit_params_in);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(matrix_unit_input_req);
  sc_fifo<IC_PORT_TYPE> matrix_unit_input_resp_fifo;
  Connections::Combinational<IC_PORT_TYPE> CCS_INIT_S1(matrix_unit_input_resp);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(matrix_unit_weight_req);
  sc_fifo<ac_int<OC_PORT_WIDTH, false>> matrix_unit_weight_resp_fifo;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      matrix_unit_weight_resp);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(matrix_unit_bias_req);
  sc_fifo<ac_int<OC_PORT_WIDTH, false>> matrix_unit_bias_resp_fifo;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      matrix_unit_bias_resp);

#if SUPPORT_MX
  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      matrix_unit_input_scale_req);
  sc_fifo<ac_int<SCALE_DATATYPE::width, false>>
      matrix_unit_input_scale_resp_fifo;
  Connections::Combinational<ac_int<SCALE_DATATYPE::width, false>> CCS_INIT_S1(
      matrix_unit_input_scale_resp);
  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      matrix_unit_weight_scale_req);
  sc_fifo<ac_int<OC_PORT_WIDTH, false>> matrix_unit_weight_scale_resp_fifo;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      matrix_unit_weight_scale_resp);
#endif

  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      matrix_unit_output_data);
  Connections::Combinational<ac_int<ADDRESS_WIDTH, false>> CCS_INIT_S1(
      matrix_unit_output_addr);

  Connections::SyncChannel CCS_INIT_S1(matrix_unit_start);
  Connections::SyncChannel CCS_INIT_S1(matrix_unit_done);

  //----------------------------------------------------------
  // MATRIX VECTOR UNIT CONNECTIONS
  //----------------------------------------------------------

#if SUPPORT_MVM
  Connections::Combinational<ac_int<64, false>> CCS_INIT_S1(
      matrix_vector_unit_params_in);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      matrix_vector_unit_input_req);
  sc_fifo<OC_PORT_TYPE> matrix_vector_unit_input_resp_fifo;
  Connections::Combinational<OC_PORT_TYPE> CCS_INIT_S1(
      matrix_vector_unit_input_resp);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      matrix_vector_unit_weight_req);
  sc_fifo<OC_PORT_TYPE> matrix_vector_unit_weight_resp_fifo;
  Connections::Combinational<OC_PORT_TYPE> CCS_INIT_S1(
      matrix_vector_unit_weight_resp);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      matrix_vector_unit_bias_req);
  sc_fifo<OC_PORT_TYPE> matrix_vector_unit_bias_resp_fifo;
  Connections::Combinational<OC_PORT_TYPE> CCS_INIT_S1(
      matrix_vector_unit_bias_resp);

#if SUPPORT_MX
  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      matrix_vector_unit_input_scale_req);
  sc_fifo<ac_int<MVU_SCALE_PORT_WIDTH, false>>
      matrix_vector_unit_input_scale_resp_fifo;
  Connections::Combinational<ac_int<MVU_SCALE_PORT_WIDTH, false>> CCS_INIT_S1(
      matrix_vector_unit_input_scale_resp);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      matrix_vector_unit_weight_scale_req);
  sc_fifo<ac_int<MVU_SCALE_PORT_WIDTH, false>>
      matrix_vector_unit_weight_scale_resp_fifo;
  Connections::Combinational<ac_int<MVU_SCALE_PORT_WIDTH, false>> CCS_INIT_S1(
      matrix_vector_unit_weight_scale_resp);
#endif

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      matrix_vector_unit_weight_dq_scale_req);
  sc_fifo<OC_PORT_TYPE> matrix_vector_unit_weight_dq_scale_resp_fifo;
  Connections::Combinational<OC_PORT_TYPE> CCS_INIT_S1(
      matrix_vector_unit_weight_dq_scale_resp);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      matrix_vector_unit_weight_dq_zp_req);
  sc_fifo<OC_PORT_TYPE> matrix_vector_unit_weight_dq_zp_resp_fifo;
  Connections::Combinational<OC_PORT_TYPE> CCS_INIT_S1(
      matrix_vector_unit_weight_dq_zp_resp);

  Connections::SyncChannel CCS_INIT_S1(matrix_vector_unit_start);
  Connections::SyncChannel CCS_INIT_S1(matrix_vector_unit_done);
#endif

  //----------------------------------------------------------
  // SPMM UNIT CONNECTIONS
  //----------------------------------------------------------

#if SUPPORT_SPMM
  Connections::Combinational<ac_int<64, false>> CCS_INIT_S1(
      spmm_unit_params_in);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      spmm_unit_input_indptr_req);
  sc_fifo<ac_int<OC_PORT_WIDTH, false>> spmm_unit_input_indptr_resp_fifo;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      spmm_unit_input_indptr_resp);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      spmm_unit_input_indices_req);
  sc_fifo<ac_int<OC_PORT_WIDTH, false>> spmm_unit_input_indices_resp_fifo;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      spmm_unit_input_indices_resp);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      spmm_unit_input_data_req);
  sc_fifo<ac_int<OC_PORT_WIDTH, false>> spmm_unit_input_data_resp_fifo;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      spmm_unit_input_data_resp);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(spmm_unit_weight_req);
  sc_fifo<ac_int<OC_PORT_WIDTH, false>> spmm_unit_weight_resp_fifo;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      spmm_unit_weight_resp);

#if SUPPORT_MX
  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      spmm_unit_weight_scale_req);
  sc_fifo<ac_int<OC_PORT_WIDTH, false>> spmm_unit_weight_scale_resp_fifo;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      spmm_unit_weight_scale_resp);
#endif

  Connections::SyncChannel CCS_INIT_S1(spmm_unit_start);
  Connections::SyncChannel CCS_INIT_S1(spmm_unit_done);
#endif

  //----------------------------------------------------------
  // DWC UNIT CONNECTIONS
  //----------------------------------------------------------

#if SUPPORT_DWC
  Connections::Combinational<ac_int<64, false>> CCS_INIT_S1(dwc_unit_params_in);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(dwc_unit_input_req);
  sc_fifo<ac_int<UNROLLFACTOR * DWC_DATATYPE::width, false>>
      dwc_unit_input_resp_fifo;
  Connections::Combinational<ac_int<UNROLLFACTOR * DWC_DATATYPE::width, false>>
      CCS_INIT_S1(dwc_unit_input_resp);
  Connections::Combinational<MemoryRequest> CCS_INIT_S1(dwc_unit_weight_req);
  sc_fifo<ac_int<DWC_KERNEL_SIZE * DWC_DATATYPE::width, false>>
      dwc_unit_weight_resp_fifo;
  Connections::Combinational<
      ac_int<DWC_KERNEL_SIZE * DWC_DATATYPE::width, false>>
      CCS_INIT_S1(dwc_unit_weight_resp);
  Connections::Combinational<MemoryRequest> CCS_INIT_S1(dwc_unit_bias_req);
  sc_fifo<ac_int<ACCUM_BUFFER_DATATYPE::width, false>> dwc_unit_bias_resp_fifo;
  Connections::Combinational<ac_int<ACCUM_BUFFER_DATATYPE::width, false>>
      CCS_INIT_S1(dwc_unit_bias_resp);

#if SUPPORT_MX
  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      dwc_unit_input_scale_req);
  sc_fifo<ac_int<SCALE_DATATYPE::width, false>> dwc_unit_input_scale_resp_fifo;
  Connections::Combinational<ac_int<SCALE_DATATYPE::width, false>> CCS_INIT_S1(
      dwc_unit_input_scale_resp);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(
      dwc_unit_weight_scale_req);
  sc_fifo<ac_int<DWC_KERNEL_SIZE * SCALE_DATATYPE::width, false>>
      dwc_unit_weight_scale_resp_fifo;
  Connections::Combinational<
      ac_int<DWC_KERNEL_SIZE * SCALE_DATATYPE::width, false>>
      CCS_INIT_S1(dwc_unit_weight_scale_resp);
#endif

  Connections::SyncChannel CCS_INIT_S1(dwc_unit_start);
  Connections::SyncChannel CCS_INIT_S1(dwc_unit_done);
#endif

  //----------------------------------------------------------
  // VECTOR UNIT CONNECTIONS
  //----------------------------------------------------------

  Connections::Combinational<ac_int<64, false>> CCS_INIT_S1(
      vector_unit_params_in);

  Connections::Combinational<MemoryRequest> CCS_INIT_S1(vector_fetch_0_req);
  sc_fifo<ac_int<OC_PORT_WIDTH, false>> vector_fetch_0_resp_fifo;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      vector_fetch_0_resp);
  Connections::Combinational<MemoryRequest> CCS_INIT_S1(vector_fetch_1_req);
  sc_fifo<ac_int<OC_PORT_WIDTH, false>> vector_fetch_1_resp_fifo;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      vector_fetch_1_resp);
  Connections::Combinational<MemoryRequest> CCS_INIT_S1(vector_fetch_2_req);
  sc_fifo<ac_int<OC_PORT_WIDTH, false>> vector_fetch_2_resp_fifo;
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      vector_fetch_2_resp);

  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      vector_output_data);
  Connections::Combinational<ac_int<ADDRESS_WIDTH, false>> CCS_INIT_S1(
      vector_output_addr);
  Connections::Combinational<ac_int<SCALE_DATATYPE::width, false>> CCS_INIT_S1(
      mx_scale_output_data);
  Connections::Combinational<ac_int<ADDRESS_WIDTH, false>> CCS_INIT_S1(
      mx_scale_output_addr);
  Connections::Combinational<ac_int<OC_PORT_WIDTH, false>> CCS_INIT_S1(
      sparse_tensor_output_data);
  Connections::Combinational<ac_int<ADDRESS_WIDTH, false>> CCS_INIT_S1(
      sparse_tensor_output_addr);

  Connections::SyncChannel CCS_INIT_S1(vector_unit_start);
  Connections::SyncChannel CCS_INIT_S1(vector_unit_done);

  Harness(sc_module_name name, const Model& model,
          const Model::Selection& selection, MemoryInterface* memory);
  SC_HAS_PROCESS(Harness);

  // Backend: run one accelerator operation on the DUT.
  void execute(const voyager::Operation& op, const ScalarEnv& env) override;

  // Backend: the counters as the hardware keeps them. The walk pushes a
  // commit's params and moves on -- the units' double-buffered controllers
  // start fetching the next tile's operands while the current tile computes
  // -- so a commit's waits go to the passes it dispatches and its post fires
  // when the last of them retires; a CPU wait blocks the walk, which is the
  // back-pressure that keeps it in step with the hardware.
  void begin_commit(const CommitSync& sync) override;
  void end_commit() override;
  void reset_counter(int64_t id, const std::string& node) override;
  void add_counter(int64_t id) override;
  void wait_counter(int64_t id, int64_t threshold,
                    const std::string& op) override;

 private:
  const Model& model;
  Model::Selection selection;
  MemoryInterface* memory;
  AccessCounter* access_counter;

#ifdef SIM_Accelerator
  CCS_DESIGN(Accelerator) CCS_INIT_S1(accelerator);
#else
  Accelerator CCS_INIT_S1(accelerator);
#endif

  template <int width>
  void process_read_request(
      Connections::Combinational<MemoryRequest>* request_out,
      sc_fifo<ac_int<width, false>>* data_fifo);

  template <int width>
  void send_data_response(
      sc_fifo<ac_int<width, false>>* data_fifo,
      Connections::Combinational<ac_int<width, false>>* response);

  template <int width>
  void process_write_request(
      Connections::Combinational<ac_int<width, false>>* data_out,
      Connections::Combinational<ac_int<ADDRESS_WIDTH, false>>* address_out);

  void read_matrix_unit_input_request();
  void send_matrix_unit_input_response();
  void read_matrix_unit_weight_request();
  void send_matrix_unit_weight_response();
  void read_matrix_unit_bias_request();
  void send_matrix_unit_bias_response();
#if SUPPORT_MX
  void read_matrix_unit_input_scale_request();
  void send_matrix_unit_input_scale_response();
  void read_matrix_unit_weight_scale_request();
  void send_matrix_unit_weight_scale_response();
#endif

  void read_matrix_vector_unit_input_request();
  void send_matrix_vector_unit_input_response();
  void read_matrix_vector_unit_weight_request();
  void send_matrix_vector_unit_weight_response();
  void read_matrix_vector_unit_bias_request();
  void send_matrix_vector_unit_bias_response();
#if SUPPORT_MX
  void read_matrix_vector_unit_input_scale_request();
  void send_matrix_vector_unit_input_scale_response();
  void read_matrix_vector_unit_weight_scale_request();
  void send_matrix_vector_unit_weight_scale_response();
#endif
  void read_matrix_vector_unit_weight_dq_scale_request();
  void send_matrix_vector_unit_weight_dq_scale_response();
  void read_matrix_vector_unit_weight_dq_zp_request();
  void send_matrix_vector_unit_weight_dq_zp_response();

#if SUPPORT_SPMM
  void read_spmm_unit_input_indptr_request();
  void send_spmm_unit_input_indptr_response();
  void read_spmm_unit_input_indices_request();
  void send_spmm_unit_input_indices_response();
  void read_spmm_unit_input_data_request();
  void send_spmm_unit_input_data_response();
  void read_spmm_unit_weight_request();
  void send_spmm_unit_weight_response();
#if SUPPORT_MX
  void read_spmm_unit_weight_scale_request();
  void send_spmm_unit_weight_scale_response();
#endif
#endif

#if SUPPORT_DWC
  void read_dwc_unit_input_request();
  void send_dwc_unit_input_response();
  void read_dwc_unit_weight_request();
  void send_dwc_unit_weight_response();
  void read_dwc_unit_bias_request();
  void send_dwc_unit_bias_response();
#if SUPPORT_MX
  void read_dwc_unit_input_scale_request();
  void send_dwc_unit_input_scale_response();
  void read_dwc_unit_weight_scale_request();
  void send_dwc_unit_weight_scale_response();
#endif
#endif

  void read_vector_fetch_0_request();
  void send_vector_fetch_0_response();
  void read_vector_fetch_1_request();
  void send_vector_fetch_1_response();
  void read_vector_fetch_2_request();
  void send_vector_fetch_2_response();

  void store_matrix_unit_output();
  void store_vector_output();
  void store_mx_scale_output();
  void store_sparse_tensor_output();

  void reset();

  // A commit the walk has opened: its waits and post, which of its wait
  // groups a dispatched pass has taken, and how many of its params groups are
  // still in flight. The post fires once the walk has closed the commit and
  // its last group has retired.
  struct Commit {
    CommitSync sync;
    bool matrix_waits_taken = false;
    bool vector_waits_taken = false;
    long groups = 0;
    long groups_left = 0;
    bool closed = false;
  };

  // One operation's start, stamped by the first of its passes to start and
  // read when its last group retires.
  struct OpTiming {
    const voyager::Operation* op = nullptr;
    bool started = false;
    sc_time start;
  };

  // One params group in flight: how many of its passes have yet to retire,
  // the commit it belongs to, and whether it closes its operation.
  struct Group {
    int passes_left = 0;
    std::shared_ptr<Commit> commit;
    std::shared_ptr<OpTiming> timing;
    bool last = false;
  };

  // One pass queued on a unit: the waits it starts behind, its group, and the
  // unit sequence number of the previous pass of the same operation on this
  // unit, which must retire first (-1 for none).
  struct Pass {
    std::vector<CounterWait> waits;
    std::shared_ptr<Group> group;
    long after = -1;
  };

  // A unit's in-order queues and handshakes. Passes start in queue order,
  // each once its waits hold, and retire in the same order. Passes are
  // numbered on their unit in dispatch order; `retired` counts those done.
  struct Unit {
    const char* name;
    Connections::SyncChannel* start;
    Connections::SyncChannel* done;
    std::deque<Pass> to_start;
    std::deque<Pass> to_retire;
    sc_event pushed;
    sc_event started;
    long dispatched = 0;
    long retired = 0;
    sc_event pass_retired;
  };

  Unit matrix_unit_queue{"matrix unit", &matrix_unit_start, &matrix_unit_done};
  Unit vector_unit_queue{"vector unit", &vector_unit_start, &vector_unit_done};
#if SUPPORT_MVM
  Unit matrix_vector_unit_queue{"matrix-vector unit",
                                &matrix_vector_unit_start,
                                &matrix_vector_unit_done};
#endif
#if SUPPORT_SPMM
  Unit spmm_unit_queue{"spmm unit", &spmm_unit_start, &spmm_unit_done};
#endif
#if SUPPORT_DWC
  Unit dwc_unit_queue{"dwc unit", &dwc_unit_start, &dwc_unit_done};
#endif

  sc_event counter_changed[NUM_COUNTERS];
  // Dispatched passes that have yet to add to each counter: a reset while one
  // is outstanding would lose its post.
  long posts_pending[NUM_COUNTERS] = {};
  std::shared_ptr<Commit> open_commit;

  // Serialize one operation's params to the units and queue a pass on each
  // unit it drives; does not wait for them to execute.
  void dispatch_params(const voyager::Operation& op,
                       const std::deque<BaseParams*>& params);

  // Adds 1 to the commit's post counter, if it has one.
  void post_commit(Commit& commit);

  // The thread that drives the DUT: it walks the graph exactly as the gold
  // model does -- same loops, same conditionals, same DMAs -- and dispatches
  // each accelerator operation through execute() above. Two threads per unit
  // pace its start and done handshakes so consecutive tiles overlap in the
  // datapath.
  void run_walker();
  void drive_starts(Unit& unit);
  void drive_dones(Unit& unit);
  void matrix_unit_starts();
  void matrix_unit_dones();
  void vector_unit_starts();
  void vector_unit_dones();
#if SUPPORT_MVM
  void matrix_vector_unit_starts();
  void matrix_vector_unit_dones();
#endif
#if SUPPORT_SPMM
  void spmm_unit_starts();
  void spmm_unit_dones();
#endif
#if SUPPORT_DWC
  void dwc_unit_starts();
  void dwc_unit_dones();
#endif
};

#endif

// Declared outside the CFLOAT guard to mirror the definition: for CFloat it
// compiles (the harness itself cannot) and aborts if actually called.
void run_accelerator(const Model& model, const Model::Selection& selection,
                     MemoryInterface* memory);
