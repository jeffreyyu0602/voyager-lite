`ifdef GL_SIM
// TODO: replace this with the correct path in the GL netlist
`define VOYAGER(sig) TestDriver.testHarness.chiptop0.system_voyager``sig
`define MATRIX_UNIT_START_VLD        `VOYAGER(__accelerator_matrix_unit_start_vld)
`define MATRIX_UNIT_DONE_VLD         `VOYAGER(__accelerator_matrix_unit_done_vld)
`define VECTOR_UNIT_START_VLD        `VOYAGER(__accelerator_vector_unit_start_vld)
`define VECTOR_UNIT_DONE_VLD         `VOYAGER(__accelerator_vector_unit_done_vld)
`define MATRIX_VECTOR_UNIT_START_VLD `VOYAGER(__accelerator_matrix_vector_unit_start_vld)
`define MATRIX_VECTOR_UNIT_DONE_VLD  `VOYAGER(__accelerator_matrix_vector_unit_done_vld)
`define SPMM_UNIT_START_VLD          `VOYAGER(__accelerator_spmm_unit_start_vld)
`define SPMM_UNIT_DONE_VLD           `VOYAGER(__accelerator_spmm_unit_done_vld)
`define HOST_DOORBELL                `VOYAGER(_doorbell)
`define COUNTER_ADD_VLD              `VOYAGER(_counter_add_port.vld)
`define COUNTER_ADD_ID               `VOYAGER(_counter_add_port.id)
`else
`define VOYAGER TestDriver.testHarness.chiptop0.system.voyager
`define MATRIX_UNIT_START_VLD        `VOYAGER.accelerator.matrix_unit.start_vld
`define MATRIX_UNIT_DONE_VLD         `VOYAGER.accelerator.matrix_unit.done_vld
`define VECTOR_UNIT_START_VLD        `VOYAGER.accelerator.vector_unit.start_vld
`define VECTOR_UNIT_DONE_VLD         `VOYAGER.accelerator.vector_unit.done_vld
`define MATRIX_VECTOR_UNIT_START_VLD `VOYAGER.accelerator.matrix_vector_unit.start_vld
`define MATRIX_VECTOR_UNIT_DONE_VLD  `VOYAGER.accelerator.matrix_vector_unit.done_vld
`define SPMM_UNIT_START_VLD          `VOYAGER.accelerator.spmm_unit.start_vld
`define SPMM_UNIT_DONE_VLD           `VOYAGER.accelerator.spmm_unit.done_vld
// The wrapper's doorbell count: the firmware advances it once per request it
// writes into its mailbox (host_request.h).
`define HOST_DOORBELL                `VOYAGER.doorbell
// The wrapper's DMA completion port (CounterAddPort.v): the testbench adds 1
// to a synchronization counter through it when a copy has landed.
`define COUNTER_ADD_VLD              `VOYAGER.counter_add_port.vld
`define COUNTER_ADD_ID               `VOYAGER.counter_add_port.id
// Handshakes probed by the per-pass stamps.
`define FIRE(p)     (`VOYAGER.p``_vld && `VOYAGER.p``_rdy)
`define PUT_FIRE(p) (`VOYAGER.auto_``p``_tl_out_a_valid && \
                     `VOYAGER.auto_``p``_tl_out_a_ready)
`define ACK_FIRE(p) (`VOYAGER.auto_``p``_tl_out_d_valid && \
                     `VOYAGER.auto_``p``_tl_out_d_ready)
`endif

import "DPI-C" context function void load_memory();
// unit encoding: 0=matrix, 1=vector, 2=mvm, 3=spmm (host_request.h)
import "DPI-C" context function void unit_started(input int unit);
import "DPI-C" context function void unit_done(input int unit);
import "DPI-C" context function void unit_retired(input int unit);
import "DPI-C" context function void host_doorbell();

module VoyagerVerification (
    input clock,
    input reset
);

    wire _source_clk = TestDriver.testHarness.source.clk;

    // Source-clock cycles between a unit's done and unit_retired(): the
    // testbench reads a tile back (a store-back request) only once its last
    // writes have landed. 100 cycles (1 us) was not enough for llama's
    // mlp_up_proj on the 2026-09 params: the final 64-byte row of every tile
    // but the last was still in flight behind the next tile's fetches (Error
    // count 456 at MAX_TILES=16, 119 at 4; 0 once the copies waited).
    // Override with +done_settle=<cycles>.
    int done_settle_cycles;
    initial begin
        if (!$value$plusargs("done_settle=%d", done_settle_cycles))
            done_settle_cycles = 1000;
    end
    logic matrix_unit_start_vld_q;
    logic vector_unit_start_vld_q;
`ifdef SUPPORT_MVM
    logic matrix_vector_unit_start_vld_q;
`endif
`ifdef SUPPORT_SPMM
    logic spmm_unit_start_vld_q;
`endif
    logic [31:0] doorbell_q;

    // Counter adds the testbench has requested (counter_add, called from its
    // DPI context) and not yet driven: the port takes one per cycle.
    int counter_adds[$];

    export "DPI-C" function counter_add;
    function void counter_add(input int counter);
        counter_adds.push_back(counter);
    endfunction

`ifndef GL_SIM
    // Per-pass stamps on the matrix (0), vector (1) and mvm (2) units: when
    // a pass's waits cleared (it is the queue head and `pending` is low) and
    // its first input beat. Input ports: 0 matrix input, 1-3 vector fetch
    // 0-2, 4 mvm input; each 64-byte response beat is counted.
    localparam int NUM_STAMP_UNITS = 3;
    localparam int NUM_INPUT_PORTS = 5;
    localparam int BEAT_BYTES = 64;
    longint in_owed[NUM_INPUT_PORTS];   // beats owed to requests issued
    longint in_taken[NUM_INPUT_PORTS];  // response beats taken
    bit in_beat[NUM_INPUT_PORTS];       // a response beat this edge

    int head_pass[NUM_STAMP_UNITS];  // the head's ordinal: starts so far
    bit head_seen[NUM_STAMP_UNITS];
    bit head_cleared[NUM_STAMP_UNITS];
    time head_time[NUM_STAMP_UNITS];

    // A pass whose first input is not yet seen: per port of its unit, the
    // response beat that is its first (B + 1, B the beats owed when its
    // parameters were taken) and the beats owed when the next pass's were.
    typedef struct {
        int pass;
        longint first[NUM_INPUT_PORTS];
        longint limit[NUM_INPUT_PORTS];
    } stamp_t;
    stamp_t stamps[NUM_STAMP_UNITS][$];
    int params_taken[NUM_STAMP_UNITS];
    int unit_dones[NUM_STAMP_UNITS];

    // The matrix unit's compute end per pass: its last row into the array
    // (matrix_processor.input_channel). The processor takes a pass's
    // parameters only after the previous pass's last row, so a pass ends at
    // the last row before the next pass's parameters, or before its own done.
    time last_row_time;
    longint rows_since_end;
    int row_ends;      // passes whose last row is stamped
    int matrix_dones;

    task automatic track_rows(
        input logic row_fire,
        input logic params_fire,
        input logic done
    );
        if (params_fire && rows_since_end > 0) begin
            $display("[%0t] matrix unit last row pass %0d at %0t", $time,
                     row_ends, last_row_time);
            row_ends++;
            rows_since_end = 0;
        end
        if (done) begin
            if (row_ends == matrix_dones && rows_since_end > 0) begin
                $display("[%0t] matrix unit last row pass %0d at %0t", $time,
                         row_ends, last_row_time);
                row_ends++;
                rows_since_end = 0;
            end
            matrix_dones++;
        end
        if (row_fire) begin
            last_row_time = $time;
            rows_since_end++;
        end
    endtask

    task automatic track_input(
        input int        port,
        input logic      req_fire,
        input bit [31:0] req_bytes,
        input logic      resp_fire
    );
        if (req_fire) in_owed[port] += (req_bytes + BEAT_BYTES - 1) / BEAT_BYTES;
        in_beat[port] = resp_fire;
        if (resp_fire) in_taken[port]++;
    endtask

    task automatic track_pass(
        input string name,
        input int    unit,
        input int    first_port,
        input int    last_port,
        input logic  head_valid,
        input logic  pending,
        input logic  start_fire,
        input logic  params_fire,
        input logic  done
    );
        if (head_valid && !head_seen[unit]) begin
            head_seen[unit] = 1;
            head_time[unit] = $time;
        end
        if (head_valid && !pending && !head_cleared[unit]) begin
            head_cleared[unit] = 1;
            $display("[%0t] %s waits cleared pass %0d head %0t", $time, name,
                     head_pass[unit], head_time[unit]);
        end
        if (start_fire) begin
            head_pass[unit]++;
            head_seen[unit] = 0;
            head_cleared[unit] = 0;
        end

        // A first beat counts only before its pass's done, so a pass that
        // reads nothing yields no stamp.
        for (int i = 0; i < stamps[unit].size(); i++) begin
            bit hit = 0;
            for (int p = first_port; p <= last_port; p++)
                if (in_beat[p] && in_taken[p] == stamps[unit][i].first[p] &&
                    stamps[unit][i].first[p] <= stamps[unit][i].limit[p])
                    hit = 1;
            if (hit || stamps[unit][i].pass < unit_dones[unit]) begin
                if (hit)
                    $display("[%0t] %s first input pass %0d", $time, name,
                             stamps[unit][i].pass);
                stamps[unit].delete(i);
                i--;
            end
        end
        if (done) unit_dones[unit]++;

        if (params_fire) begin
            stamp_t s;
            if (stamps[unit].size() > 0)
                for (int p = first_port; p <= last_port; p++)
                    stamps[unit][$].limit[p] = in_owed[p];
            s.pass = params_taken[unit]++;
            for (int p = 0; p < NUM_INPUT_PORTS; p++) begin
                s.first[p] = in_owed[p] + 1;
                s.limit[p] = 64'h7fffffffffffffff;
            end
            stamps[unit].push_back(s);
        end
    endtask
`endif

    initial begin
        @(posedge clock);  // move past time 0 to avoid signal glitch
        wait (reset === 1'b1);
        wait (reset === 1'b0);
        $display("[%0t] reset deassertion", $time);
`ifndef GL_SIM
        // The stamps count from reset, whatever the simulator's register
        // initialization left in these variables.
        foreach (in_owed[p]) begin
            in_owed[p] = 0;
            in_taken[p] = 0;
            in_beat[p] = 0;
        end
        foreach (head_pass[u]) begin
            head_pass[u] = 0;
            head_seen[u] = 0;
            head_cleared[u] = 0;
            params_taken[u] = 0;
            unit_dones[u] = 0;
            stamps[u].delete();
        end
        rows_since_end = 0;
        row_ends = 0;
        matrix_dones = 0;
`endif
        load_memory();
    end

    task automatic check_unit_status(
        input string name,
        input int    unit,
        input logic  start_vld,
        input logic  done_vld,
        input logic  start_vld_q
    );
        // GL start_rdy is hard to probe, so use start_vld falling edge as the
        // vld/rdy accept event.
        if (start_vld_q && !start_vld) begin
            $display("[%0t] %s started", $time, name);

            fork
                begin
                    @(negedge _source_clk);
                    unit_started(unit);
                end
            join_none
        end

        if (done_vld) begin
            $display("[%0t] %s done", $time, name);

            fork
                begin
                    // Delivered off-edge, like every DPI event below: the
                    // testbench deposits into the SRAM macros via VPI, and a
                    // deposit in the same time slot as a posedge NBA update
                    // is scheduler-order dependent.
                    @(negedge _source_clk);
                    unit_done(unit);
                    // Wait for data to be written to memory.
                    repeat (done_settle_cycles) @(posedge _source_clk);
                    @(negedge _source_clk);
                    unit_retired(unit);
                end
            join_none
        end
    endtask

    always @(posedge _source_clk) begin
        if (reset) begin
            matrix_unit_start_vld_q <= 1'b0;
            vector_unit_start_vld_q <= 1'b0;
`ifdef SUPPORT_MVM
            matrix_vector_unit_start_vld_q <= 1'b0;
`endif
`ifdef SUPPORT_SPMM
            spmm_unit_start_vld_q <= 1'b0;
`endif
            doorbell_q <= 32'd0;
            `COUNTER_ADD_VLD <= 1'b0;
            `COUNTER_ADD_ID <= 5'd0;
        end else begin
            matrix_unit_start_vld_q <= `MATRIX_UNIT_START_VLD;
            vector_unit_start_vld_q <= `VECTOR_UNIT_START_VLD;
`ifdef SUPPORT_MVM
            matrix_vector_unit_start_vld_q <= `MATRIX_VECTOR_UNIT_START_VLD;
`endif
`ifdef SUPPORT_SPMM
            spmm_unit_start_vld_q <= `SPMM_UNIT_START_VLD;
`endif

`ifndef GL_SIM
            track_input(0, `FIRE(accelerator.matrix_unit_input_req),
                        `VOYAGER.accelerator.matrix_unit_input_req_dat[95:64],
                        `FIRE(accelerator.matrix_unit_input_resp));
            track_input(1, `FIRE(accelerator.vector_fetch_0_req),
                        `VOYAGER.accelerator.vector_fetch_0_req_dat[95:64],
                        `FIRE(accelerator.vector_fetch_0_resp));
            track_input(2, `FIRE(accelerator.vector_fetch_1_req),
                        `VOYAGER.accelerator.vector_fetch_1_req_dat[95:64],
                        `FIRE(accelerator.vector_fetch_1_resp));
            track_input(3, `FIRE(accelerator.vector_fetch_2_req),
                        `VOYAGER.accelerator.vector_fetch_2_req_dat[95:64],
                        `FIRE(accelerator.vector_fetch_2_resp));
`ifdef SUPPORT_MVM
            track_input(4, `FIRE(accelerator.matrix_vector_unit_input_req),
                        `VOYAGER.accelerator.matrix_vector_unit_input_req_dat[95:64],
                        `FIRE(accelerator.matrix_vector_unit_input_resp));
`endif
            track_pass("matrix unit", 0, 0, 0,
                       `VOYAGER.syncs_0._passes_io_deq_valid,
                       `VOYAGER.syncs_0.pending,
                       `VOYAGER.syncs_0.io_start_rdy &&
                           `VOYAGER.accelerator.matrix_unit_start_vld,
                       `FIRE(accelerator.matrix_unit.input_controller.params_in),
                       `MATRIX_UNIT_DONE_VLD);
            track_rows(`FIRE(accelerator.matrix_unit.matrix_processor.input_channel),
                       `FIRE(accelerator.matrix_unit.matrix_processor.params_in),
                       `MATRIX_UNIT_DONE_VLD);
            track_pass("vector unit", 1, 1, 3,
                       `VOYAGER.syncs_1._passes_io_deq_valid,
                       `VOYAGER.syncs_1.pending,
                       `VOYAGER.syncs_1.io_start_rdy &&
                           `VOYAGER.accelerator.vector_unit_start_vld,
                       `FIRE(accelerator.vector_unit.fetcher.params_in),
                       `VECTOR_UNIT_DONE_VLD);
`ifdef SUPPORT_MVM
            track_pass("matrix vector unit", 2, 4, 4,
                       `VOYAGER.syncs_2._passes_io_deq_valid,
                       `VOYAGER.syncs_2.pending,
                       `VOYAGER.syncs_2.io_start_rdy &&
                           `VOYAGER.accelerator.matrix_vector_unit_start_vld,
                       `FIRE(accelerator.matrix_vector_unit.fetch_input_param),
                       `MATRIX_VECTOR_UNIT_DONE_VLD);
`endif
`endif

            check_unit_status("matrix unit", 0,
                              `MATRIX_UNIT_START_VLD,
                              `MATRIX_UNIT_DONE_VLD,
                              matrix_unit_start_vld_q);

            check_unit_status("vector unit", 1,
                              `VECTOR_UNIT_START_VLD,
                              `VECTOR_UNIT_DONE_VLD,
                              vector_unit_start_vld_q);
`ifdef SUPPORT_MVM
            check_unit_status("matrix vector unit", 2,
                              `MATRIX_VECTOR_UNIT_START_VLD,
                              `MATRIX_VECTOR_UNIT_DONE_VLD,
                              matrix_vector_unit_start_vld_q);
`endif
`ifdef SUPPORT_SPMM
            check_unit_status("spmm unit", 3,
                              `SPMM_UNIT_START_VLD,
                              `SPMM_UNIT_DONE_VLD,
                              spmm_unit_start_vld_q);
`endif

            // The doorbell is a counter: any change means new requests.
            doorbell_q <= `HOST_DOORBELL;
            if (`HOST_DOORBELL != doorbell_q) begin
                fork
                    begin
                        @(negedge _source_clk);
                        host_doorbell();
                    end
                join_none
            end

            // One counter add per cycle; the wrapper samples the port on the
            // next edge.
            if (counter_adds.size() > 0) begin
                `COUNTER_ADD_VLD <= 1'b1;
                `COUNTER_ADD_ID <= counter_adds.pop_front();
            end else begin
                `COUNTER_ADD_VLD <= 1'b0;
            end
        end
    end

endmodule
