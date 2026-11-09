#pragma once

#include <limits>

#include "toolchain/Common.h"

void map_rms_norm(const voyager::Operation& operation, const ScalarEnv& env,
                  std::deque<BaseParams*>& mapped_params) {
  const auto& rms_norm_op = get_anchor_op(operation);

  const Tensor& anchor_input = resolve(rms_norm_op, "input", env);
  const voyager::PrimOp* input_producer =
      get_fused_producer(operation, env, anchor_input);
  const voyager::PrimOp* dequantize_op =
      input_producer != nullptr &&
              strip_namespace(input_producer->target()) == "dequantize"
          ? input_producer
          : nullptr;
  const Tensor& input = dequantize_op == nullptr
                            ? anchor_input
                            : resolve(*dequantize_op, "input", env);
  const auto output = resolve_outputs(operation, env).back();

  VectorParams* vector_params;
  VectorInstructionConfig* vector_instruction_config;

  const auto input_shape = squeeze_shape(get_shape(input));
  const int input_size = get_size(input);
  const int reduction_dim = input_shape.back();

  std::vector<int> non_reduction_loops;
  for (int i = 0; i < input_shape.size() - 1; i++) {
    non_reduction_loops.push_back(input_shape[i]);
  }

  non_reduction_loops = split_loops(non_reduction_loops, MAX_LOOP_VALUE);
  pad_shape_to_ndim(non_reduction_loops, 2);
  const int reduced_size = get_size(non_reduction_loops);

  constexpr int vu_unit_ratio = OC_DIMENSION / VECTOR_UNIT_WIDTH;

  int input_dtype = hw_dtype_index<VU_INPUT_TYPES>(input.dtype, "read");
  int input_dtype_width = get_type_width<VU_INPUT_TYPES>(input_dtype);
  int input_fetch_width = OC_DIMENSION * input_dtype_width;

  int vector_dtype = get_type_index<VECTOR_DATATYPE, VU_INPUT_TYPES>();
  int vector_dtype_width = get_type_width<VU_INPUT_TYPES>(vector_dtype);
  int vector_fetch_width = OC_DIMENSION * vector_dtype_width;

  // The compiler reserves one OC_DIMENSION-wide row per normalized row: pass 1
  // writes the row's reciprocal RMS there and pass 2 reads it.
  const int mean_square_address =
      get_address(resolve(rms_norm_op, "mean_square", env));

  // aten::rms_norm defaults eps to the epsilon of its float32 compute type.
  const float eps = has_arg(rms_norm_op, "eps")
                        ? arg_float(rms_norm_op, "eps", env)
                        : std::numeric_limits<float>::epsilon();

  // ======================================================================
  // Pass 1: reciprocal RMS, rsqrt(mean(x^2) + eps)
  // ======================================================================

  vector_params = new VectorParams;
  vector_instruction_config = new VectorInstructionConfig;

  vector_params->vector_fetch_0_offset = get_address(input);
  vector_params->vector_fetch_0_mode = 2;
  vector_params->vector_fetch_0_dtype = input_dtype;
  vector_params->vector_fetch_0_stride = OC_DIMENSION;
  vector_params->vector_fetch_0_burst_size = input_fetch_width / 8;
  vector_params->vector_fetch_0_num_beats = input_fetch_width / OC_PORT_WIDTH;
  vector_params->vector_fetch_0_packing_factor = vu_unit_ratio;

  for (int i = 0; i < 3; i++) {
    vector_params->vector_fetch_0_loops[0][i] = 1;
  }
  vector_params->vector_fetch_0_loops[1][0] = non_reduction_loops[0];
  vector_params->vector_fetch_0_loops[1][1] = non_reduction_loops[1];
  vector_params->vector_fetch_0_loops[1][2] = reduction_dim / OC_DIMENSION;

  vector_params->vector_output_offset = mean_square_address;
  vector_params->output_mode = 2;
  vector_params->output_dtype =
      get_type_index<VECTOR_DATATYPE, OUTPUT_DATATYPES>();

  for (int i = 0; i < 3; i++) {
    vector_params->output_loops[0][i] = 1;
  }
  vector_params->output_loops[1][0] = 1;
  vector_params->output_loops[1][1] = 1;
  vector_params->output_loops[1][2] = reduced_size;

  // The row sum of x^2 / norm_size + eps / norm_size is mean(x^2) + eps
  VectorInstructions inst0;
  inst0.op_type = VectorInstructions::reduction;
  inst0.inst_loop_count = reduced_size;
  inst0.reduce_count = reduction_dim / REDUCER_WIDTH;
  inst0.reduce_op = VectorInstructions::radd;
  inst0.rsqrt = 1;
  inst0.rreciprocal = 1;
  inst0.rduplicate = 1;
  inst0.rdest = VectorInstructions::to_memory;
  vector_instruction_config->inst[0] = inst0;

  // Stages 0 and 2 evaluate the polynomial c0 + (c1 + c2 * x) * x; one
  // range set with c2 = 1 / norm_size, c1 = 0 and c0 = eps / norm_size
  // covers every input.
  auto& approx_config = vector_instruction_config->approx_config;
  for (int i = 0; i < NUM_RANGES; i++) {
    approx_config.ranges[i][0] = eps / reduction_dim;
    approx_config.ranges[i][1] = 0.0;
    approx_config.ranges[i][2] = 1.0 / reduction_dim;
  }

  VectorInstructions inst1;
  inst1.op_type = VectorInstructions::vector;
  inst1.inst_loop_count = input_size / VECTOR_UNIT_WIDTH;
  inst1.vector_op0_src0 = VectorInstructions::from_vector_fetch_0;
  inst1.vector_op0 = VectorInstructions::op0_poly;
  inst1.vector_op2 = VectorInstructions::op2_poly;
  inst1.vdest = VectorInstructions::to_reduce;
  set_dequantize_scale(dequantize_op, env, inst1);
  vector_instruction_config->inst[1] = inst1;

  vector_instruction_config->num_inst = 2;
  vector_instruction_config->config_loop_count = 1;

  mapped_params.push_back(vector_params);
  mapped_params.push_back(vector_instruction_config);

  // ======================================================================
  // Pass 2: scale by the reciprocal RMS, then by the weight
  // ======================================================================

  vector_params = new VectorParams;
  vector_instruction_config = new VectorInstructionConfig;

  vector_params->vector_fetch_0_offset = get_address(input);
  vector_params->vector_fetch_0_mode = 2;
  vector_params->vector_fetch_0_dtype = input_dtype;
  vector_params->vector_fetch_0_stride = OC_DIMENSION;
  vector_params->vector_fetch_0_burst_size = input_fetch_width / 8;
  vector_params->vector_fetch_0_num_beats = input_fetch_width / OC_PORT_WIDTH;
  vector_params->vector_fetch_0_packing_factor = vu_unit_ratio;

  for (int i = 0; i < 3; i++) {
    vector_params->vector_fetch_0_loops[0][i] = 1;
  }
  vector_params->vector_fetch_0_loops[1][0] = non_reduction_loops[0];
  vector_params->vector_fetch_0_loops[1][1] = non_reduction_loops[1];
  vector_params->vector_fetch_0_loops[1][2] = reduction_dim / OC_DIMENSION;

  // Address generator 1: reciprocal RMS, one row per normalized row
  vector_params->vector_fetch_1_offset = mean_square_address;
  vector_params->vector_fetch_1_mode = 1;
  vector_params->vector_fetch_1_dtype = vector_dtype;
  vector_params->vector_fetch_1_stride = OC_DIMENSION;
  vector_params->vector_fetch_1_burst_size = vector_fetch_width / 8;
  vector_params->vector_fetch_1_num_beats = vector_fetch_width / OC_PORT_WIDTH;
  vector_params->vector_fetch_1_packing_factor = vu_unit_ratio;

  vector_params->vector_fetch_1_broadcast = 0b100;
  for (int i = 0; i < 3; i++) {
    vector_params->vector_fetch_1_loops[0][i] = 1;
  }
  vector_params->vector_fetch_1_loops[1][0] = non_reduction_loops[0];
  vector_params->vector_fetch_1_loops[1][1] = non_reduction_loops[1];
  vector_params->vector_fetch_1_loops[1][2] = reduction_dim / OC_DIMENSION;

  // Address generator 2: weight, the same vector for every row
  const bool has_weight = has_tensor(rms_norm_op, "weight");
  if (has_weight) {
    const auto weight = resolve(rms_norm_op, "weight", env);
    int weight_dtype = hw_dtype_index<VU_INPUT_TYPES>(weight.dtype, "read");
    int weight_dtype_width = get_type_width<VU_INPUT_TYPES>(weight_dtype);
    int weight_fetch_width = OC_DIMENSION * weight_dtype_width;

    vector_params->vector_fetch_2_offset = get_address(weight);
    vector_params->vector_fetch_2_mode = 1;
    vector_params->vector_fetch_2_broadcast = 0b011;
    vector_params->vector_fetch_2_dtype = weight_dtype;
    vector_params->vector_fetch_2_stride = OC_DIMENSION;
    vector_params->vector_fetch_2_burst_size = weight_fetch_width / 8;
    vector_params->vector_fetch_2_num_beats =
        weight_fetch_width / OC_PORT_WIDTH;
    vector_params->vector_fetch_2_packing_factor = vu_unit_ratio;

    for (int i = 0; i < 3; i++) {
      vector_params->vector_fetch_2_loops[0][i] = 1;
    }
    vector_params->vector_fetch_2_loops[1][0] = non_reduction_loops[0];
    vector_params->vector_fetch_2_loops[1][1] = non_reduction_loops[1];
    vector_params->vector_fetch_2_loops[1][2] = reduction_dim / OC_DIMENSION;
  }

  vector_params->vector_output_offset = get_address(output);
  vector_params->output_mode = 2;
  vector_params->output_dtype =
      hw_dtype_index<OUTPUT_DATATYPES>(output.dtype, "write");

  for (int i = 0; i < 3; i++) {
    vector_params->output_loops[0][i] = 1;
  }
  vector_params->output_loops[1][0] = non_reduction_loops[0];
  vector_params->output_loops[1][1] = non_reduction_loops[1];
  vector_params->output_loops[1][2] = reduction_dim / OC_DIMENSION;

  // (inputs x reciprocal RMS) x weight, the order aten::rms_norm multiplies in
  VectorInstructions inst4;
  inst4.op_type = VectorInstructions::vector;
  inst4.inst_loop_count = input_size / VECTOR_UNIT_WIDTH;
  inst4.vector_op0_src0 = VectorInstructions::from_vector_fetch_0;
  inst4.vector_op0_src1 = VectorInstructions::from_vector_fetch_1;
  inst4.vector_op0 = VectorInstructions::op0_mul;
  if (has_weight) {
    inst4.vector_op2_src1 = VectorInstructions::from_vector_fetch_2;
    inst4.vector_op2 = VectorInstructions::op2_mul;
  }
  inst4.vdest = VectorInstructions::to_output;
  set_dequantize_scale(dequantize_op, env, inst4);
  set_quantize_params(operation, env, vector_params, inst4,
                      vector_instruction_config, mapped_params);
  vector_instruction_config->inst[0] = inst4;

  vector_instruction_config->num_inst = 1;
  vector_instruction_config->config_loop_count = 1;

  mapped_params.push_back(vector_params);
  mapped_params.push_back(vector_instruction_config);
}
