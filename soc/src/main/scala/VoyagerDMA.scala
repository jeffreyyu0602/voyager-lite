package voyager

import chisel3._
import chisel3.util._
import freechips.rocketchip.diplomacy._
import freechips.rocketchip.tilelink._
import freechips.rocketchip.util._
import org.chipsalliance.cde.config.{Config, Field, Parameters}
import org.chipsalliance.diplomacy.lazymodule.{LazyModule, LazyModuleImp}

// The wrapper's DMA engine, a simulation model: two in-order descriptor
// queues (loads into the scratchpad, stores out of it), each served by its
// own engine that masters the system bus. readsInFlight Gets per engine cover
// DRAM bandwidth x latency, and four 64-row blocks of an 8-bit transposed copy
// (two 128-row blocks of a 4-bit one). writesInFlight bounds an engine's
// unacknowledged Puts.
case class VoyagerDMAParams(
  readsInFlight:  Int = 256,
  writesInFlight: Int = 64,
  queueDepth:     Int = 8)

case object VoyagerDMAKey extends Field[Option[VoyagerDMAParams]](None)

class WithVoyagerDMA(params: VoyagerDMAParams = VoyagerDMAParams())
    extends Config((site, here, up) => { case VoyagerDMAKey => Some(params) })

// The simulated DRAM's hashed channel map: `channels` channels of
// `blockBytes` blocks over [base, base + size). The DMA engines rewrite their
// DRAM addresses with DRAMChannelHash, so the system bus routes them by the
// hashed channel, and the harness's SimDRAM applies it again to store at the
// plain address. None: the plain blockBytes interleave.
case class DRAMHashParams(base: BigInt, size: BigInt, channels: Int, blockBytes: Int = 64)

case object VoyagerDRAMHashKey extends Field[Option[DRAMHashParams]](None)

object DRAMChannelHash {
  // `addr` with its channel bits XORed with every higher group of as many bits
  // of the DRAM offset, as DDR controllers hash banks: power-of-two strides
  // spread over the channels, and a contiguous stream still visits each in
  // turn. The map is its own inverse; addresses outside DRAM pass unchanged.
  def apply(addr: UInt, h: DRAMHashParams): UInt = {
    require(isPow2(h.channels) && isPow2(h.size) && h.base % h.size == 0)
    val lgChan = log2Ceil(h.channels)
    val lo     = log2Ceil(h.blockBytes)
    val hi     = lo + lgChan
    val lgSize = log2Ceil(h.size)
    if (lgChan == 0) {
      addr
    } else {
      val groups = (hi until lgSize by lgChan).map(k => addr(math.min(k + lgChan, lgSize) - 1, k))
      val chan   = groups.foldLeft(addr(hi - 1, lo))(_ ^ _)(lgChan - 1, 0)
      val hashed = Cat(addr(addr.getWidth - 1, hi), chan, addr(lo - 1, 0))
      Mux((addr >> lgSize) === (h.base >> lgSize).U, hashed, addr)
    }
  }
}

// One copy (register map and semantics: soc/firmware/common/dma.h). The
// innermost run of `inner` bytes repeats over up to three outer levels, level
// 0 fastest; level k moves the source by srcStride(k) and the destination by
// dstStride(k) bytes. A transposed copy reads `count(0)` source rows of
// `inner` bytes and writes each element column as a destination row,
// dstStride(0) bytes apart; levels 1 and 2 repeat the whole transpose.
class DMADescriptor extends Bundle {
  val src       = UInt(64.W)
  val dst       = UInt(64.W)
  val inner     = UInt(32.W)
  val ctrl      = UInt(16.W)
  val count     = Vec(3, UInt(32.W))
  val srcStride = Vec(3, UInt(64.W))
  val dstStride = Vec(3, UInt(64.W))

  // CTRL: [4:0] counter, [5] post, [6] fill (write zeros, read nothing),
  // [7] transposed, [9:8] log2 of the transposed element's bytes, [10] the
  // transposed elements are 4-bit (two to a byte, the even one low).
  def counter    = ctrl(4, 0)
  def post       = ctrl(5)
  def fill       = ctrl(6)
  def transposed = ctrl(7)
  def elemLg2    = ctrl(9, 8)
  def nibbles    = ctrl(10)
  // log2 of a transposed element's bits: 2, 3 or 4.
  def elemBitsLg2 = Mux(nibbles, 2.U(3.W), elemLg2 +& 3.U)
}

// A contiguous byte range a copy reads or writes.
class DMARun extends Bundle {
  val addr = UInt(64.W)
  val len  = UInt(32.W)
}

// A destination run; `last` marks the run that ends its copy, which carries
// the copy's CTRL[5:0] (post, counter) in `tag`.
class DMAWriteRun extends Bundle {
  val run  = new DMARun
  val last = Bool()
  val tag  = UInt(6.W)
}

// The bytes [lo, hi) of a 64 B lane-addressed beat, in stream order.
class DMAChunk(beatBytes: Int) extends Bundle {
  val data = UInt((8 * beatBytes).W)
  val lo   = UInt(log2Ceil(beatBytes + 1).W)
  val hi   = UInt(log2Ceil(beatBytes + 1).W)
}

// One write: a Put of 2^lg bytes at addr with byte lanes `mask`, or, when
// !write, a marker that carries only descEnd (a copy that moves nothing).
class DMABeat(beatBytes: Int) extends Bundle {
  val addr    = UInt(64.W)
  val data    = UInt((8 * beatBytes).W)
  val mask    = UInt(beatBytes.W)
  val lg      = UInt(3.W)
  val write   = Bool()
  val descEnd = Bool()
  val tag     = UInt(6.W)
}

// Writes one line to the simulation's stderr each cycle `valid` is high. A
// Chisel printf would print only under +verbose, with the core's commit trace.
// `format` is a $fwrite format of `fields` 64-bit values; `name` names the
// module, one per format.
class SimLog(name: String, format: String, fields: Int) extends BlackBox with HasBlackBoxInline {
  val io = IO(new Bundle {
    val clock  = Input(Clock())
    val valid  = Input(Bool())
    val values = Input(Vec(fields, UInt(64.W)))
  })
  override def desiredName = name

  val ports  = (0 until fields).map(i => s"input [63:0] values_$i").mkString(", ")
  val values = (0 until fields).map(i => s"values_$i").mkString(", ")
  setInline(
    s"$name.v",
    s"""module $name(input clock, input valid, $ports);
       |  always @(posedge clock) if (valid) $$fwrite(32'h80000002, "$format\\n", $values);
       |endmodule
       |""".stripMargin,
  )
}

object SimLog {
  def apply(name: String, format: String, valid: Bool, values: Seq[UInt]): Unit = {
    val log = Module(new SimLog(name, format, values.size))
    log.io.clock  := Module.clock
    log.io.valid  := valid && !Module.reset.asBool
    log.io.values := VecInit(values.map(value => 0.U(64.W) | value))
  }
}

object DMAUtil {
  // A mask of the low n of `width` bytes, n <= width.
  def byteMask(n: UInt, width: Int): UInt =
    (((BigInt(1) << width) - 1).U((2 * width).W) >> (width.U - n))(width - 1, 0)

  // log2 of the smallest naturally aligned power of two that holds the bytes
  // [first, last] of one beat.
  def coverLg(first: UInt, last: UInt, maxLg: Int): UInt = {
    val diff = (first ^ last)(maxLg - 1, 0)
    Mux(diff === 0.U, 0.U, Log2(diff) +& 1.U)
  }
}

// Packs a stream of byte chunks into the destination beats of a stream of
// runs: run k takes the next run(k).len bytes of the chunk stream, written
// from run(k).addr on, any alignment on either side. `last` marks the run that
// ends its copy; a zero-length run emits one marker beat.
class DMAPacker(beatBytes: Int) extends Module {
  val lgBeat = log2Ceil(beatBytes)
  val io     = IO(new Bundle {
    val chunk = Flipped(Decoupled(new DMAChunk(beatBytes)))
    val run   = Flipped(Decoupled(new DMAWriteRun))
    val beat  = Decoupled(new DMABeat(beatBytes))
  })

  // Up to two beats of stream bytes, oldest in the low lanes.
  val buf = RegInit(0.U((16 * beatBytes).W))
  val cnt = RegInit(0.U(log2Ceil(2 * beatBytes + 1).W))
  val off = RegInit(0.U(32.W))

  val run      = io.run.bits.run
  val addr     = run.addr + off
  val lane     = addr(lgBeat - 1, 0)
  val left     = run.len - off
  val room     = beatBytes.U - lane
  val need     = Mux(left < room, left, room)(lgBeat, 0)
  val empty    = run.len === 0.U
  val runEnd   = left === need
  val canWrite = io.run.valid && (empty || cnt >= need)

  io.beat.valid        := canWrite
  io.beat.bits.write   := !empty
  io.beat.bits.descEnd := io.run.bits.last && (empty || runEnd)
  io.beat.bits.tag     := io.run.bits.tag
  io.beat.bits.lg      := DMAUtil.coverLg(lane, lane + need - 1.U, lgBeat)
  io.beat.bits.addr    := addr & ~((1.U(64.W) << io.beat.bits.lg) - 1.U)
  io.beat.bits.mask    := DMAUtil.byteMask(need, beatBytes) << lane
  io.beat.bits.data    :=
    ((buf(8 * beatBytes - 1, 0) & FillInterleaved(8, DMAUtil.byteMask(need, beatBytes))) << (lane << 3))(
      8 * beatBytes - 1,
      0,
    )

  val emitted = Mux(io.beat.fire && !empty, need, 0.U)
  io.run.ready := io.beat.fire && (empty || runEnd)
  when(io.beat.fire) {
    off := Mux(empty || runEnd, 0.U, off + need)
  }

  val kept  = cnt - emitted
  val bytes = io.chunk.bits.hi - io.chunk.bits.lo
  io.chunk.ready := kept <= beatBytes.U
  val incoming = Mux(
    io.chunk.fire,
    ((io.chunk.bits.data >> (io.chunk.bits.lo << 3)) & FillInterleaved(8, DMAUtil.byteMask(bytes, beatBytes))),
    0.U,
  )
  buf := ((buf >> (emitted << 3)) | (incoming << (kept << 3)))(16 * beatBytes - 1, 0)
  cnt := kept + Mux(io.chunk.fire, bytes, 0.U)
}

// The shape of one block of a transposed copy (DMATransposeBuffer): its
// element size, valid columns and rows, and whether it is the copy's last.
class DMABlock extends Bundle {
  val lgBits = UInt(3.W)
  val cols   = UInt(8.W)
  val rows   = UInt(8.W)
  val last   = Bool()
}

// One run of DMARunGen: `fill` reads nothing; `transposed` runs go through the
// transpose buffer; `first` and `last` mark a copy's first and last run, and
// a transposed run carries its block, `blockStart` on the block's first run.
class DMAGenRun extends Bundle {
  val run        = new DMARun
  val fill       = Bool()
  val transposed = Bool()
  val first      = Bool()
  val last       = Bool()
  val block      = new DMABlock
  val blockStart = Bool()
  val tag        = UInt(6.W)
}

// Walks a copy's runs on one side, the source (reads) or the destination
// (writes). A normal copy has one run per (i2, i1, i0). A transposed copy (R =
// count(0) rows of C elements of `inner` bytes, W elements to 64 B) moves 64 B
// x W-row blocks (rb, cb): the source side reads the block's W row segments,
// rows past R as zero fills; the destination side writes the block's valid
// columns, each a segment of one destination row. With 4-bit elements R must
// be even, so that a column segment is whole bytes.
class DMARunGen(source: Boolean) extends Module {
  val io = IO(new Bundle {
    val desc = Flipped(Decoupled(new DMADescriptor))
    val run  = Decoupled(new DMAGenRun)
  })

  val d      = Reg(new DMADescriptor)
  val active = RegInit(false.B)
  val first  = Reg(Bool())
  // i2, i1, i0 (rb when transposed), cb, j: j fastest.
  val idx    = Reg(Vec(5, UInt(32.W)))
  io.desc.ready := !active
  when(io.desc.fire) {
    d      := io.desc.bits
    active := true.B
    first  := true.B
    idx.foreach(_ := 0.U)
  }

  val lgBits     = d.elemBitsLg2
  val shift      = 9.U - lgBits // log2 of W
  val transposed = d.transposed && !d.fill
  val rows       = d.count(0)
  val cols       = (d.inner << 3) >> lgBits
  val width      = (512.U >> lgBits)(7, 0)
  val nothing    = d.inner === 0.U || d.count.map(_ === 0.U).reduce(_ || _) || (transposed && cols === 0.U)

  val rb        = idx(2)
  val cb        = idx(3)
  val j         = idx(4)
  val colsLeft  = cols - (cb << shift)
  val blockCols = Mux(colsLeft < width, colsLeft, width)(7, 0)
  val rowsLeft  = rows - (rb << shift)
  val blockRows = Mux(rowsLeft < width, rowsLeft, width)(7, 0)
  val counts    = Mux(
    transposed,
    VecInit(d.count(2), d.count(1), ((rows +& width - 1.U) >> shift)(31, 0),
      ((cols + width - 1.U) >> shift)(31, 0), (if (source) width else blockCols)),
    VecInit(d.count(2), d.count(1), d.count(0), 1.U, 1.U),
  )
  val wraps     = (0 until 5).map(k => idx(k) === counts(k) - 1.U)
  val lastRun   = nothing || wraps.reduce(_ && _)

  val srcBase = d.src + idx(0) * d.srcStride(2) + idx(1) * d.srcStride(1)
  val dstBase = d.dst + idx(0) * d.dstStride(2) + idx(1) * d.dstStride(1)
  val row     = (rb << shift) + j
  val segLeft = d.inner - (cb << 6)

  val out = io.run.bits
  out.fill       := d.fill
  out.transposed := transposed && !nothing
  out.first      := first
  out.last       := lastRun
  out.block.lgBits := lgBits
  out.block.cols := blockCols
  out.block.rows := blockRows
  out.block.last := wraps.take(4).reduce(_ && _)
  out.blockStart := j === 0.U
  out.tag        := d.ctrl(5, 0)
  out.run.addr   := Mux(source.B, srcBase + rb * d.srcStride(0), dstBase + rb * d.dstStride(0))
  out.run.len    := Mux(nothing, 0.U, d.inner)
  when(transposed && !nothing) {
    if (source) {
      // Row `row` of the block: its 64 B segment, or zeros past the last row.
      out.fill     := row >= rows
      out.run.addr := srcBase + row * d.srcStride(0) + (cb << 6)
      out.run.len  := Mux(row >= rows || segLeft >= 64.U, 64.U, segLeft)
    } else {
      // Column j: the block's rows of destination row cb * W + j.
      out.run.addr := dstBase + ((cb << shift) + j) * d.dstStride(0) + (rb << 6)
      out.run.len  := (blockRows << lgBits) >> 3
    }
  }

  // The source side has nothing to read for a copy that moves nothing.
  io.run.valid := active && !(source.B && nothing)
  when(io.run.fire || (active && source.B && nothing)) {
    first := false.B
    when(lastRun) {
      active := false.B
    }.otherwise {
      // An odometer over the five indices, j fastest.
      val carry = Wire(Vec(6, Bool()))
      carry(5) := true.B
      for (k <- 4 to 0 by -1) {
        carry(k) := carry(k + 1) && wraps(k)
        when(carry(k + 1)) { idx(k) := Mux(wraps(k), 0.U, idx(k) + 1.U) }
      }
    }
  }
}

// The transpose buffer: Gemmini's AlwaysOutTransposer at 64 B per row, one each
// for 4-bit, 1-byte and 2-byte elements. A block's W rows go in one per cycle,
// and its columns come out while the next block's rows go in; W filler rows
// push out a copy's last block. Columns past the block's valid count are
// dropped; each kept one leaves as a chunk of the block's valid rows.
class DMATransposeBuffer extends Module {
  val io = IO(new Bundle {
    // Rows packed from lane 0, and the block they belong to.
    val row   = Flipped(Decoupled(UInt(512.W)))
    val block = Flipped(Decoupled(new DMABlock))
    val col   = Decoupled(new DMAChunk(64))
  })

  val nibbles = Module(new AlwaysOutTransposer(128, UInt(4.W)))
  val bytes   = Module(new AlwaysOutTransposer(64, UInt(8.W)))
  val halves  = Module(new AlwaysOutTransposer(32, UInt(16.W)))
  // Element bits' log2 (2, 3, 4) and each instance.
  val transposers = Seq(2 -> nibbles, 3 -> bytes, 4 -> halves)

  // The group of W pushes under way: a block's rows (pushing) or fillers
  // (flushing), and the block whose columns come out during it (pending).
  val pushing  = RegInit(false.B)
  val flushing = RegInit(false.B)
  val cur      = Reg(new DMABlock)
  val pending  = RegInit(false.B)
  val pend     = Reg(new DMABlock)
  val i        = RegInit(0.U(8.W))
  val width    = (512.U >> cur.lgBits)(7, 0)

  io.block.ready := !pushing && !flushing
  when(io.block.fire) {
    pushing := true.B
    cur     := io.block.bits
  }

  val emits  = pending && i < pend.cols
  val canIn  = flushing || (pushing && io.row.valid)
  val push   = canIn && (!emits || io.col.ready)
  val input  = Mux(flushing, 0.U, io.row.bits)
  io.row.ready := pushing && push

  io.col.valid     := canIn && emits
  io.col.bits.data := MuxLookup(pend.lgBits, 0.U)(transposers.map { case (lg, t) => lg.U -> t.io.outCol.bits.asUInt })
  io.col.bits.lo   := 0.U
  io.col.bits.hi   := (pend.rows << pend.lgBits) >> 3

  for ((lg, t) <- transposers) {
    t.io.inRow.valid  := push && cur.lgBits === lg.U
    t.io.inRow.bits   := input.asTypeOf(chiselTypeOf(t.io.inRow.bits))
    t.io.outCol.ready := true.B
  }

  when(push) {
    i := i + 1.U
    when(i === width - 1.U) {
      i        := 0.U
      pending  := pushing
      pend     := cur
      flushing := pushing && cur.last
      pushing  := false.B
    }
  }
}

// `queue` numbers the engine in its log lines: 0 loads, 1 stores.
class VoyagerDMAEngine(name: String, queue: Int, params: VoyagerDMAParams)(implicit p: Parameters)
    extends LazyModule {
  val numReads  = params.readsInFlight
  val numWrites = params.writesInFlight
  // Reads and writes have a system-bus port each, so neither waits for the
  // other's requests.
  def client(port: String, ids: Int) = TLClientNode(
    Seq(TLMasterPortParameters.v1(Seq(TLMasterParameters.v1(name = s"$name $port", sourceId = IdRange(0, ids)))))
  )
  val readNode  = client("reads", numReads)
  val writeNode = client("writes", numWrites)

  lazy val module = new Impl

  class Impl extends LazyModuleImp(this) {
    val io = IO(new Bundle {
      val desc   = Flipped(Decoupled(new DMADescriptor))
      // The counter a copy adds 1 to, the cycle its last write is acknowledged.
      val posted = Valid(UInt(5.W))
      // Copies taken and not yet complete.
      val busy   = Output(Bool())
    })

    val (readPort, readEdge)   = readNode.out(0)
    val (writePort, writeEdge) = writeNode.out(0)
    // A request's address on the bus: hashed when the DRAM is.
    val hash                   = p(VoyagerDRAMHashKey)
    def busAddress(addr: UInt) = hash.fold(addr)(DRAMChannelHash(addr, _))
    val beatBytes              = readEdge.manager.beatBytes
    val lgBeat     = log2Ceil(beatBytes)
    require(beatBytes == 64, "the DMA engine moves 64 B beats")

    val cycle = RegInit(0.U(64.W))
    cycle := cycle + 1.U

    // ---------------------------------------------------------------------
    // Runs: the source and the destination side each walk every copy
    // ---------------------------------------------------------------------
    val srcGen = Module(new DMARunGen(source = true))
    val dstGen = Module(new DMARunGen(source = false))
    srcGen.io.desc.valid := io.desc.valid && dstGen.io.desc.ready
    dstGen.io.desc.valid := io.desc.valid && srcGen.io.desc.ready
    srcGen.io.desc.bits  := io.desc.bits
    dstGen.io.desc.bits  := io.desc.bits
    io.desc.ready        := srcGen.io.desc.ready && dstGen.io.desc.ready

    // The reads, and a transposed row's packing from lane 0.
    class ReadRun extends Bundle {
      val run        = new DMARun
      val fill       = Bool()
      val transposed = Bool()
      val first      = Bool()
    }
    val readRuns  = Module(new Queue(new ReadRun, 2))
    val alignRuns = Module(new Queue(new DMAWriteRun, numReads))
    val sr        = srcGen.io.run.bits
    val srcRoom   = readRuns.io.enq.ready && (!sr.transposed || alignRuns.io.enq.ready)
    srcGen.io.run.ready             := srcRoom
    readRuns.io.enq.valid           := srcGen.io.run.valid && srcRoom
    readRuns.io.enq.bits.run        := sr.run
    readRuns.io.enq.bits.fill       := sr.fill
    readRuns.io.enq.bits.transposed := sr.transposed
    readRuns.io.enq.bits.first      := sr.first
    alignRuns.io.enq.valid          := srcGen.io.run.valid && srcRoom && sr.transposed
    alignRuns.io.enq.bits.run.addr  := 0.U
    alignRuns.io.enq.bits.run.len   := sr.run.len
    alignRuns.io.enq.bits.last      := false.B
    alignRuns.io.enq.bits.tag       := 0.U

    // The writes: a normal copy's runs, a transposed copy's columns and its
    // blocks' shapes, and each copy's path for the writer (true: transposed).
    // A block's columns are written while the next block's rows go in, so
    // the column queue holds a block (up to 2 x beatBytes columns, 4-bit) and
    // a run of the next.
    val normalRuns = Module(new Queue(new DMAWriteRun, numReads))
    val columnRuns = Module(new Queue(new DMAWriteRun, 2 * beatBytes + 8))
    val blocks     = Module(new Queue(new DMABlock, 4))
    val paths      = Module(new Queue(Bool(), 4))
    val dr         = dstGen.io.run.bits
    val newBlock   = dr.transposed && dr.blockStart
    val dstRoom    = Mux(dr.transposed, columnRuns.io.enq.ready, normalRuns.io.enq.ready) &&
      (!newBlock || blocks.io.enq.ready) && (!dr.first || paths.io.enq.ready)
    dstGen.io.run.ready := dstRoom
    val dstFire = dstGen.io.run.valid && dstRoom
    for ((queue, selected) <- Seq((normalRuns, !dr.transposed), (columnRuns, dr.transposed))) {
      queue.io.enq.valid     := dstFire && selected
      queue.io.enq.bits.run  := dr.run
      queue.io.enq.bits.last := dr.last
      queue.io.enq.bits.tag  := dr.tag
    }
    blocks.io.enq.valid := dstFire && newBlock
    blocks.io.enq.bits  := dr.block
    paths.io.enq.valid  := dstFire && dr.first
    paths.io.enq.bits   := dr.transposed

    // ---------------------------------------------------------------------
    // Reader: Gets in run order, a reorder buffer indexed by source id
    // ---------------------------------------------------------------------
    val robData  = Mem(numReads, UInt((8 * beatBytes).W))
    val robValid = RegInit(VecInit(Seq.fill(numReads)(false.B)))
    val robLo    = Reg(Vec(numReads, UInt((lgBeat + 1).W)))
    val robHi    = Reg(Vec(numReads, UInt((lgBeat + 1).W)))
    val robRow   = Reg(Vec(numReads, Bool()))
    val robHead  = RegInit(0.U(log2Ceil(numReads).W))
    val robTail  = RegInit(0.U(log2Ceil(numReads).W))
    val robCount = RegInit(0.U(log2Ceil(numReads + 1).W))

    val rd      = readRuns.io.deq.bits
    val rOff    = RegInit(0.U(32.W))
    val rAddr   = rd.run.addr + rOff
    val rLane   = rAddr(lgBeat - 1, 0)
    val rLeft   = rd.run.len - rOff
    val rRoom   = beatBytes.U - rLane
    val rBytes  = Mux(rLeft < rRoom, rLeft, rRoom)(lgBeat, 0)
    val rEmpty  = rd.run.len === 0.U
    val rRunEnd = rLeft === rBytes
    val rLg     = DMAUtil.coverLg(rLane, rLane + rBytes - 1.U, lgBeat)
    val robRoom = robCount =/= numReads.U

    val get = readPort.a
    get.valid := readRuns.io.deq.valid && !rEmpty && !rd.fill && robRoom
    get.bits  := readEdge
      .Get(robTail, busAddress(rAddr & ~((1.U(64.W) << rLg) - 1.U)), rLg)
      ._2

    // A fill chunk is a reorder-buffer entry that is complete when allocated.
    val fillChunk = readRuns.io.deq.valid && !rEmpty && rd.fill && robRoom
    val allocate  = get.fire || fillChunk
    readRuns.io.deq.ready := (readRuns.io.deq.valid && rEmpty) || (allocate && rRunEnd)
    when(allocate) {
      robLo(robTail)  := rLane
      robHi(robTail)  := rLane + rBytes
      robRow(robTail) := rd.transposed
      rOff            := Mux(rRunEnd, 0.U, rOff + rBytes)
      robTail         := robTail + 1.U
    }
    when(fillChunk) {
      robData(robTail)  := 0.U
      robValid(robTail) := true.B
    }

    // The reorder buffer has a slot for every read in flight.
    readPort.d.ready := true.B
    // Each copy's first Get logs its round trip, bus and DRAM together.
    val probe      = RegInit(false.B)
    val probeId    = Reg(UInt(log2Ceil(numReads).W))
    val probeIssue = Reg(UInt(64.W))
    when(get.fire && rd.first && rOff === 0.U && !probe) {
      probe      := true.B
      probeId    := robTail
      probeIssue := cycle
    }
    val probeBack = probe && readPort.d.fire && readPort.d.bits.source === probeId
    when(probeBack) { probe := false.B }
    SimLog(
      "VoyagerDMAGetLog",
      "[DMA-GET] queue %0d issue %0d latency %0d",
      probeBack,
      Seq(queue.U, probeIssue, cycle - probeIssue),
    )

    when(readPort.d.fire) {
      robData(readPort.d.bits.source)  := readPort.d.bits.data
      robValid(readPort.d.bits.source) := true.B
    }

    // The head chunk goes to the normal packer or, for a transposed row, to
    // the row packer.
    val normalPacker = Module(new DMAPacker(beatBytes))
    val rowPacker    = Module(new DMAPacker(beatBytes))
    val columnPacker = Module(new DMAPacker(beatBytes))
    for ((packer, selected) <- Seq((normalPacker, !robRow(robHead)), (rowPacker, robRow(robHead)))) {
      packer.io.chunk.valid     := robValid(robHead) && selected
      packer.io.chunk.bits.data := robData(robHead)
      packer.io.chunk.bits.lo   := robLo(robHead)
      packer.io.chunk.bits.hi   := robHi(robHead)
    }
    val chunkTaken = normalPacker.io.chunk.fire || rowPacker.io.chunk.fire
    when(chunkTaken) {
      robValid(robHead) := false.B
      robHead           := robHead + 1.U
    }
    robCount := robCount + allocate.asUInt - chunkTaken.asUInt

    normalPacker.io.run <> normalRuns.io.deq
    rowPacker.io.run <> alignRuns.io.deq
    columnPacker.io.run <> columnRuns.io.deq

    val transpose = Module(new DMATransposeBuffer)
    transpose.io.row.valid       := rowPacker.io.beat.valid
    transpose.io.row.bits        := rowPacker.io.beat.bits.data
    rowPacker.io.beat.ready      := transpose.io.row.ready
    transpose.io.block <> blocks.io.deq
    columnPacker.io.chunk <> transpose.io.col

    // ---------------------------------------------------------------------
    // Writer: Puts in copy order, and each copy's completion once its writes
    // are acknowledged
    // ---------------------------------------------------------------------
    // Copies whose last write has issued, oldest first, in a ring of
    // `ringDepth` slots; the slot at ringTail collects the writes of the copy
    // being written.
    val ringDepth   = 4
    val outstanding = RegInit(VecInit(Seq.fill(ringDepth)(0.U(log2Ceil(numWrites + 1).W))))
    val ringCtr     = Reg(Vec(ringDepth, UInt(5.W)))
    val ringPost    = Reg(Vec(ringDepth, Bool()))
    val ringHead    = RegInit(0.U(log2Ceil(ringDepth).W))
    val ringTail    = RegInit(0.U(log2Ceil(ringDepth).W))
    val ringCount   = RegInit(0.U(log2Ceil(ringDepth + 1).W))
    val slotOf      = Reg(Vec(numWrites, UInt(log2Ceil(ringDepth).W)))
    val idBusy      = RegInit(0.U(numWrites.W))
    val freeId      = PriorityEncoder(~idBusy)
    val haveId      = !idBusy.andR

    val columns  = paths.io.deq.bits
    val beat     = Mux(columns, columnPacker.io.beat.bits, normalPacker.io.beat.bits)
    val beatOk   = paths.io.deq.valid && Mux(columns, columnPacker.io.beat.valid, normalPacker.io.beat.valid)
    val ringRoom = ringCount =/= ringDepth.U
    val put      = writePort.a
    put.valid := beatOk && beat.write && haveId && ringRoom
    put.bits  := writeEdge
      .Put(
        freeId,
        busAddress(beat.addr),
        beat.lg,
        beat.data,
        beat.mask,
      )
      ._2
    val beatTaken = beatOk && ringRoom && Mux(beat.write, put.ready && haveId, true.B)
    normalPacker.io.beat.ready := beatTaken && !columns
    columnPacker.io.beat.ready := beatTaken && columns

    writePort.d.ready := true.B
    val ack      = writePort.d.fire
    val ackId    = writePort.d.bits.source
    val ackSlot  = slotOf(ackId)
    val issuedId = UIntToOH(freeId, numWrites)
    idBusy := (idBusy | Mux(put.fire, issuedId, 0.U)) & ~Mux(ack, UIntToOH(ackId, numWrites), 0.U)
    when(put.fire) { slotOf(freeId) := ringTail }
    for (s <- 0 until ringDepth) {
      val inc = put.fire && ringTail === s.U
      val dec = ack && ackSlot === s.U
      outstanding(s) := outstanding(s) + inc.asUInt - dec.asUInt
    }

    val copyEnd = beatTaken && beat.descEnd
    paths.io.deq.ready := copyEnd
    when(copyEnd) {
      ringCtr(ringTail)  := beat.tag(4, 0)
      ringPost(ringTail) := beat.tag(5)
      ringTail           := ringTail + 1.U
    }

    val complete = ringCount =/= 0.U && outstanding(ringHead) === 0.U
    when(complete) { ringHead := ringHead + 1.U }
    ringCount := ringCount + copyEnd.asUInt - complete.asUInt

    io.posted.valid := complete && ringPost(ringHead)
    io.posted.bits  := ringCtr(ringHead)

    val taken = RegInit(0.U(32.W))
    val done  = RegInit(0.U(32.W))
    when(io.desc.fire) { taken := taken + 1.U }
    when(complete) { done := done + 1.U }
    SimLog(
      "VoyagerDMALog",
      "[DMA] queue %0d copy %0d ctr %0d post %0d cycle %0d",
      complete,
      Seq(queue.U, done, ringCtr(ringHead), ringPost(ringHead), cycle),
    )
    io.busy := taken =/= done
  }
}

// A unit's posts in a DMA config: each waits until every write the unit issued
// before its done has been acknowledged. Per write interface, `issued` counts
// the words the unit has handed it, this cycle's included, and `counts` its
// progress. A post waits until those words have all issued as Puts, then
// until the Puts issued by then are acknowledged; one whose writes have all
// landed fires in the cycle of its done. Logs each post's latency from the
// done.
class AckedPost(idBits: Int, depth: Int, unit: Int, writers: Int) extends Module {
  val io = IO(new Bundle {
    val posted  = Input(Valid(UInt(idBits.W)))
    val issued  = Input(Vec(writers, UInt(32.W)))
    val counts  = Input(Vec(writers, new WriteCounts))
    val out     = Output(Valid(UInt(idBits.W)))
    val pending = Output(Bool())
  })

  class Entry extends Bundle {
    val counter = UInt(idBits.W)
    val targets = Vec(writers, UInt(32.W))
    val done    = UInt(64.W)
  }

  val cycle = RegInit(0.U(64.W))
  cycle := cycle + 1.U

  val posts = Module(new Queue(new Entry, depth, flow = true))
  posts.io.enq.valid        := io.posted.valid
  posts.io.enq.bits.counter := io.posted.bits
  posts.io.enq.bits.targets := io.issued
  posts.io.enq.bits.done    := cycle
  assert(!io.posted.valid || posts.io.enq.ready, "unit post queue overflow")

  def all(conditions: Seq[Bool]): Bool = conditions.foldLeft(true.B)(_ && _)

  // The head post's Puts issued at the cycle its words had all issued.
  val head     = posts.io.deq.bits
  val captured = RegInit(false.B)
  val puts     = Reg(Vec(writers, UInt(32.W)))
  val drained  = all((0 until writers).map(i => io.counts(i).words >= head.targets(i)))
  val landed   = all(io.counts.map(c => c.acks >= c.puts))
  val caught   = all((0 until writers).map(i => io.counts(i).acks >= puts(i)))
  val fire     = posts.io.deq.valid && ((captured && caught) || (drained && landed))
  posts.io.deq.ready := fire
  when(posts.io.deq.valid && drained && !captured && !fire) {
    captured := true.B
    puts.zip(io.counts).foreach { case (held, count) => held := count.puts }
  }
  when(fire) { captured := false.B }

  io.out.valid := fire
  io.out.bits  := head.counter
  io.pending   := posts.io.deq.valid

  SimLog(
    "VoyagerPostLog",
    "[POST] unit %0d ctr %0d done %0d latency %0d",
    fire,
    Seq(unit.U, head.counter, head.done, cycle - head.done),
  )
  // A post held for every 2^14 cycles logs what it waits for.
  val stalled = posts.io.deq.valid && !fire && (cycle - head.done)(13, 0).andR
  for (i <- 0 until writers) {
    SimLog(
      "VoyagerPostWaitLog",
      "[POST-WAIT] unit %0d writer %0d target %0d words %0d puts %0d acks %0d captured %0d",
      stalled,
      Seq(unit.U, i.U, head.targets(i), io.counts(i).words, io.counts(i).puts, io.counts(i).acks, puts(i)),
    )
  }
}
