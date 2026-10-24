#pragma once

#include <mc_connections.h>
#include <systemc.h>

// Converts Wide -> Narrow (Serializer)
// Assumption: in_width is a multiple of out_width
template <typename T, int in_width, int out_width>
SC_MODULE(Serializer) {
  sc_in<bool> clk;
  sc_in<bool> rstn;

  Connections::In<Pack1D<T, in_width>> in;
  Connections::Out<Pack1D<T, out_width>> out;

  SC_CTOR(Serializer) {
    SC_THREAD(run);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
  }

  void run() {
    in.Reset();
    out.Reset();
    wait();

    constexpr int ratio = in_width / out_width;

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
    while (true) {
      Pack1D<T, in_width> wide_data = in.Pop();

      for (int i = 0; i < ratio; i++) {
        Pack1D<T, out_width> narrow_data;
#pragma hls_unroll yes
        for (int j = 0; j < out_width; j++) {
          narrow_data[j] = wide_data[i * out_width + j];
        }
        out.Push(narrow_data);
      }
    }
  }
};

// Converts Narrow -> Wide (Deserializer)
// Assumption: out_width is a multiple of in_width
template <typename T, int in_width, int out_width>
SC_MODULE(Deserializer) {
  sc_in<bool> clk;
  sc_in<bool> rstn;

  Connections::In<Pack1D<T, in_width>> in;
  Connections::Out<Pack1D<T, out_width>> out;

  SC_CTOR(Deserializer) {
    SC_THREAD(run);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
  }

  void run() {
    in.Reset();
    out.Reset();
    wait();

    constexpr int ratio = out_width / in_width;

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
    while (true) {
      Pack1D<T, out_width> wide_data;

      for (int i = 0; i < ratio; i++) {
        Pack1D<T, in_width> narrow_data = in.Pop();
#pragma hls_unroll yes
        for (int j = 0; j < in_width; j++) {
          wide_data[i * in_width + j] = narrow_data[j];
        }
      }
      out.Push(wide_data);
    }
  }
};

// WIDE -> NARROW (Truncate/Slice)
// Drops the upper (WIDE_W - NARROW_W) elements.
template <typename T, int in_width, int out_width>
SC_MODULE(Slicer) {
  sc_in<bool> clk;
  sc_in<bool> rstn;

  Connections::In<Pack1D<T, in_width>> in;
  Connections::Out<Pack1D<T, out_width>> out;

  SC_CTOR(Slicer) {
    SC_THREAD(run);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
  }

  void run() {
    in.Reset();
    out.Reset();
    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
    while (true) {
      auto wide_data = in.Pop();
      Pack1D<T, out_width> narrow_data;

      // Only copy the lower out_width elements
#pragma hls_unroll yes
      for (int i = 0; i < out_width; i++) {
        narrow_data[i] = wide_data[i];
      }
      out.Push(narrow_data);
    }
  }
};

// NARROW -> WIDE (Zero Pad/Extend)
// Fills the upper elements with Zero.
template <typename T, int in_width, int out_width>
SC_MODULE(ZeroPadder) {
  sc_in<bool> clk;
  sc_in<bool> rstn;

  Connections::In<Pack1D<T, in_width>> in;
  Connections::Out<Pack1D<T, out_width>> out;

  SC_CTOR(ZeroPadder) {
    SC_THREAD(run);
    sensitive << clk.pos();
    async_reset_signal_is(rstn, false);
  }

  void run() {
    in.Reset();
    out.Reset();
    wait();

#pragma hls_pipeline_init_interval 1
#pragma hls_pipeline_stall_mode flush
    while (true) {
      auto narrow_data = in.Pop();
      Pack1D<T, out_width> wide_data;

#pragma hls_unroll yes
      for (int i = 0; i < in_width; i++) {
        wide_data[i] = narrow_data[i];
      }

      // Zero out the rest
#pragma hls_unroll yes
      for (int i = in_width; i < out_width; i++) {
        wide_data[i] = T::zero();
      }
      out.Push(wide_data);
    }
  }
};
