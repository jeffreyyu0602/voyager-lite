#pragma once

#include <svdpi.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "SoCMemory.h"
#include "test/Simulation.h"
#include "soc/emitter/HostRequests.h"
#include "soc/firmware/common/host_request.h"

// The SoC RTL testbench: the DMA engine Sphinx does not have, plus the gold
// reference and the grading.
//
// The firmware on the Rocket core executes the whole of the layer's
// bufferized program. What it cannot do itself -- move data between the
// host-side DRAM and the DUT's scratchpad -- it asks for through a mailbox
// in its own memory (host_request.h): a copy or zero fill names its prim by
// ordinal in the host-request table (HostRequests.h) and carries the
// run-time values of the scalars the prim references, so the testbench runs
// the very same run_async_copy / zero_buffer the interpreter does. A copy
// completes by adding 1 to the hardware synchronization counter it names,
// through the wrapper's DMA completion port; the other kinds by adding 1 to
// a cell the firmware spins on.
//
// The testbench is driven by three DPI events from the Verilog collateral:
// the doorbell (the wrapper's doorbell count changed: read the mailbox), a
// unit's done pulse, and the same done `done_settle` cycles later, once the
// unit's last writes have landed. Requests that touch the scratchpad wait
// for that settle.
class SoCSimulation : public Simulation {
 public:
  SoCSimulation();

  void start();
  void doorbell();
  void unit_started(int unit);
  void unit_done(int unit);
  void unit_retired(int unit);

 protected:
  ArrayMemory* make_memory(const std::string& sim,
                           const std::vector<uint64_t>& sizes) override;

 private:
  using Request = host_request_t;

  void drain_mailbox();
  void service();
  void execute(const Request& request);
  void complete(const Request& request);
  ScalarEnv request_env(const HostOp& host, const Request& request) const;
  std::string describe(const Request& request) const;

  // Scratchpad cells by absolute (CPU) address.
  uint64_t read_u64(uint64_t address);
  void write_u64(uint64_t address, uint64_t value);
  void read_bytes(uint64_t address, uint64_t count, void* out);

  std::vector<HostOp> table_;
  uint64_t mailbox_ = 0;  // absolute address of host_mailbox
  uint64_t tail_ = 0;     // requests taken off the ring

  std::deque<Request> queue_;  // taken, not yet serviced, in order

  int settling_ = 0;  // done pulses whose settle has not elapsed

  bool finished_ = false;
  const bool trace_;
};
