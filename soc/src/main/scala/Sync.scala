package voyager

import chisel3._
import chisel3.util._

// A queued wait: holds once counters(counter) >= threshold.
class WaitEntry(idBits: Int) extends Bundle {
  val counter   = UInt(idBits.W)
  val threshold = UInt(32.W)
}

// The counter a pass adds 1 to at its done, if any.
class Post(idBits: Int) extends Bundle {
  val valid   = Bool()
  val counter = UInt(idBits.W)
}

// A dispatched pass: how many of the queued waits are its, and its post.
class Pass(idBits: Int, waitBits: Int) extends Bundle {
  val waits = UInt(waitBits.W)
  val post  = new Post(idBits)
}

// One unit's share of the wrapper's synchronization: the waits and the post
// of every pass dispatched on it, and the passes in flight.
//
// The firmware writes a pass's waits (wait_req) and its post (post_req),
// dispatches it (dispatch_req), then sends its params. The unit raises
// start_vld once the params are in; start_rdy is granted when every wait of
// the oldest dispatched pass holds. Dones arrive in start order; each names
// the counter its pass adds 1 to (posted).
//
// Encodings: wait_req [idBits-1:0] counter, [63:32] threshold, the wait holds
// once counter >= threshold (unsigned); post_req [idBits-1:0] counter;
// dispatch_req's bits are unused. A write to a full queue stalls until an
// entry leaves; the depths are chosen so that the unit's params path fills
// first, so the queues bound nothing (Voyager.scala).
class UnitSync(numCounters: Int, waitDepth: Int, passDepth: Int, startedDepth: Int) extends Module {
  val idBits   = log2Ceil(numCounters)
  val waitBits = log2Ceil(waitDepth + 1)

  val io = IO(new Bundle {
    val wait_req     = Flipped(Decoupled(UInt(64.W)))
    val post_req     = Flipped(Decoupled(UInt(8.W)))
    val dispatch_req = Flipped(Decoupled(UInt(8.W)))
    val counters     = Input(Vec(numCounters, UInt(32.W)))
    val start_vld    = Input(Bool())
    val start_rdy    = Output(Bool())
    val done         = Input(Bool())
    // The counter the pass completing this cycle adds 1 to.
    val posted       = Output(Valid(UInt(idBits.W)))
    // Passes dispatched and not done (queued behind a wait included), and
    // whether one has started.
    val inflight     = Output(UInt(8.W))
    val running      = Output(Bool())
  })

  val waits   = Module(new Queue(new WaitEntry(idBits), waitDepth))
  val passes  = Module(new Queue(new Pass(idBits, waitBits), passDepth))
  val started = Module(new Queue(new Post(idBits), startedDepth))

  // The pass being assembled: its waits and post since the last dispatch.
  val open_waits = RegInit(0.U(waitBits.W))
  val open_post  = RegInit(0.U.asTypeOf(new Post(idBits)))

  val wait_room = open_waits =/= waitDepth.U
  waits.io.enq.valid          := io.wait_req.valid && wait_room
  waits.io.enq.bits.counter   := io.wait_req.bits(idBits - 1, 0)
  waits.io.enq.bits.threshold := io.wait_req.bits(63, 32)
  io.wait_req.ready           := waits.io.enq.ready && wait_room
  when(waits.io.enq.fire) { open_waits := open_waits + 1.U }

  io.post_req.ready := true.B
  when(io.post_req.fire) {
    open_post.valid   := true.B
    open_post.counter := io.post_req.bits(idBits - 1, 0)
  }

  passes.io.enq.valid      := io.dispatch_req.valid
  passes.io.enq.bits.waits := open_waits
  passes.io.enq.bits.post  := open_post
  io.dispatch_req.ready    := passes.io.enq.ready
  when(passes.io.enq.fire) {
    open_waits      := 0.U
    open_post.valid := false.B
  }

  // The oldest dispatched pass's waits are the oldest entries of the wait
  // queue; they retire one per cycle as they hold.
  val head     = passes.io.deq.bits
  val resolved = RegInit(0.U(waitBits.W))
  val pending  = passes.io.deq.valid && resolved < head.waits
  val oldest   = waits.io.deq.bits
  val holds    = waits.io.deq.valid && io.counters(oldest.counter) >= oldest.threshold
  waits.io.deq.ready := pending && holds
  when(waits.io.deq.fire) { resolved := resolved + 1.U }

  io.start_rdy := passes.io.deq.valid && !pending && started.io.enq.ready
  val start_fire = io.start_rdy && io.start_vld
  passes.io.deq.ready  := start_fire
  started.io.enq.valid := start_fire
  started.io.enq.bits  := head.post
  when(start_fire) { resolved := 0.U }

  started.io.deq.ready := io.done
  assert(!io.done || started.io.deq.valid, "done with no pass started")
  io.posted.valid := started.io.deq.fire && started.io.deq.bits.valid
  io.posted.bits  := started.io.deq.bits.counter

  io.inflight := passes.io.count +& started.io.count
  io.running  := started.io.deq.valid
}

// The DMA engine's completion port: add_vld for one cycle adds 1 to counter
// add_id. Sphinx has no DMA engine; the SoC testbench drives the port's two
// variables by hierarchical reference (VerificationCollateral.v), and in any
// other use they hold their initial values.
class CounterAddPort extends BlackBox with HasBlackBoxPath {
  val io = IO(new Bundle {
    val add_vld = Output(Bool())
    val add_id  = Output(UInt(5.W))
  })

  val chipyardDir = System.getProperty("user.dir")
  addPath(s"$chipyardDir/generators/voyager/soc/src/main/resources/vsrc/CounterAddPort.v")
}
