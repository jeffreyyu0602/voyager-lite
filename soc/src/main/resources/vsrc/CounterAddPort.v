// The Voyager wrapper's DMA completion port: `add_vld` for one cycle adds 1 to
// synchronization counter `add_id`. Sphinx has no DMA engine, so nothing in
// the design drives the two variables; the SoC testbench
// (VerificationCollateral.v) assigns them by hierarchical reference when a
// transfer it performed has landed, and in any other use they hold their
// initial values.
module CounterAddPort(
    output       add_vld,
    output [4:0] add_id
);
  reg       vld = 1'b0;
  reg [4:0] id  = 5'd0;

  assign add_vld = vld;
  assign add_id  = id;
endmodule
