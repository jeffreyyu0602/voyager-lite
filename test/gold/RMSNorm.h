#pragma once

#include <limits>

#include "test/gold/Common.h"

// The vector unit's RMSNorm (toolchain/RMSNorm.h): pass 1 feeds the reducer
// eps / D + (x / D) * x, the approximation unit's polynomial, and takes
// sqrt then reciprocal of the row sum; pass 2 scales x by it, then by weight.
template <typename T>
inline std::shared_ptr<T[]> rms_norm(std::any input_ptr, std::any weight_ptr,
                                     const std::vector<int> input_shape,
                                     const std::vector<int> normalized_shape,
                                     const float eps) {
  T* inputs = std::any_cast<std::shared_ptr<T[]>&>(input_ptr).get();
  T* weights = std::any_cast<std::shared_ptr<T[]>&>(weight_ptr).get();

  std::shared_ptr<T[]> output(new T[get_size(input_shape)]);

  const int outer_dim = get_size(normalized_shape);
  const int inner_dim = get_size(input_shape) / outer_dim;

  // The polynomial coefficients c0 + (c1 + c2 * x) * x the mapper loads
  const T c0 = eps / outer_dim;
  const T c1 = 0.0;
  const T c2 = 1.0 / outer_dim;

  for (int i = 0; i < inner_dim; i++) {
    T terms[outer_dim];
    for (int j = 0; j < outer_dim; j++) {
      const T input = inputs[i * outer_dim + j];
      const T linear = c1 + input * c2;
      terms[j] = c0 + linear * input;
    }

    // The reducer alternates between two accumulators, joined at the end
    T sums[2] = {0, 0};
    int index = 0;

    for (int j = 0; j < outer_dim; j += REDUCER_WIDTH) {
      T buffer[REDUCER_WIDTH];
      for (int k = 0; k < REDUCER_WIDTH; k++) {
        buffer[k] = terms[j + k];
      }
      sums[index++ % 2] += fused_tree_reduce<REDUCER_WIDTH>(buffer);
    }

    const T root = fused_tree_reduce<2>(sums).sqrt();
    const T rms_inv = root == T::zero() ? T::zero() : root.reciprocal();

    for (int j = 0; j < outer_dim; j++) {
      T input = inputs[i * outer_dim + j];
      input *= rms_inv;
      if (weights) input *= weights[j];
      output[i * outer_dim + j] = input;
    }
  }

  return output;
}

template <typename T>
inline std::shared_ptr<T[]> rms_norm(std::map<std::string, std::any>& kwargs,
                                     const voyager::PrimOp& op,
                                     const ScalarEnv& env) {
  assert(strip_namespace(op.target()) == "rms_norm");

  const auto input = resolve(op, "input", env);
  std::any input_ptr = kwargs[input.node];

  std::any weight_ptr = std::shared_ptr<T[]>();

  if (has_tensor(op, "weight")) {
    const auto weight = resolve(op, "weight", env);
    weight_ptr = kwargs[weight.node];
  }

  // aten::rms_norm defaults eps to the epsilon of its float32 compute type
  const float eps = has_arg(op, "eps") ? arg_float(op, "eps", env)
                                       : std::numeric_limits<float>::epsilon();

  const auto input_shape = get_shape(input);
  const auto norm_shape = arg_ints(op, "normalized_shape", env);

  return rms_norm<T>(input_ptr, weight_ptr, input_shape, norm_shape, eps);
}
