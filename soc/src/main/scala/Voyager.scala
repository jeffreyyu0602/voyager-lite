package voyager

import sys.process._

import chisel3._
import chisel3.util._
import chisel3.experimental.{BaseModule, IntParam}
import freechips.rocketchip.amba.axi4._
import freechips.rocketchip.subsystem.{BaseSubsystem, PBUS, SBUS}
import freechips.rocketchip.diplomacy._
import freechips.rocketchip.regmapper.{HasRegMap, RegField, RegFieldDesc, RegWriteFn}
import freechips.rocketchip.resources.SimpleDevice
import freechips.rocketchip.subsystem.SystemBusKey
import freechips.rocketchip.tilelink._
import freechips.rocketchip.util._
import freechips.rocketchip.prci.{ClockSinkDomain, ClockSinkParameters}
import freechips.rocketchip.interrupts.{IntSourceNode, IntSourcePortSimple}
import org.chipsalliance.cde.config.{Config, Field, Parameters}
import org.chipsalliance.diplomacy.lazymodule.LazyModule
import scala.collection.immutable.ListMap
import scala.collection.mutable.ArrayBuffer

import testchipip.serdes._
case class VoyagerParams(
  mmioAddress:               BigInt  = 0x20000000L,
  datatype:                  String  = "E4M3",
  icDimension:               Int     = 16,
  ocDimension:               Int     = 16,
  inputBufferSize:           Int     = 1024,
  weightBufferSize:          Int     = 1024,
  accumBufferSize:           Int     = 1024,
  clockPeriod:               Double  = 5,
  technology:                String  = "generic",
  scaleDatatype:             String  = "",
  doubleBufferedAccumBuffer: Boolean = false,
  supportMVM:                Boolean = false,
  supportSpmm:               Boolean = false,
  // Lanes of the MVM unit; None is the array width, the accelerator's own
  // default for every datatype but MXNF4 (which the Sphinx configs set to 128).
  mvUnitWidth:               Option[Int] = None)

case object VoyagerKey extends Field[Option[VoyagerParams]](None)

class BaseVoyagerIO(
  bitWidth:      Int,
  icDimension:   Int,
  ocDimension:   Int,
  scaleBitWidth: Int)
    extends Bundle {
  val clk  = Input(Clock())
  val rstn = Input(Bool())

  val matrix_unit_params_in_vld = Input(Bool())
  val matrix_unit_params_in_rdy = Output(Bool())
  val matrix_unit_params_in_dat = Input(UInt(64.W))

  val matrix_unit_input_req_vld  = Output(Bool())
  val matrix_unit_input_req_rdy  = Input(Bool())
  val matrix_unit_input_req_dat  = Output(UInt(96.W))
  val matrix_unit_input_resp_vld = Input(Bool())
  val matrix_unit_input_resp_rdy = Output(Bool())
  val matrix_unit_input_resp_dat = Input(UInt((bitWidth * icDimension).W))

  val matrix_unit_weight_req_vld  = Output(Bool())
  val matrix_unit_weight_req_rdy  = Input(Bool())
  val matrix_unit_weight_req_dat  = Output(UInt(96.W))
  val matrix_unit_weight_resp_vld = Input(Bool())
  val matrix_unit_weight_resp_rdy = Output(Bool())
  val matrix_unit_weight_resp_dat = Input(UInt((bitWidth * ocDimension).W))

  val matrix_unit_bias_req_vld  = Output(Bool())
  val matrix_unit_bias_req_rdy  = Input(Bool())
  val matrix_unit_bias_req_dat  = Output(UInt(96.W))
  val matrix_unit_bias_resp_vld = Input(Bool())
  val matrix_unit_bias_resp_rdy = Output(Bool())
  val matrix_unit_bias_resp_dat = Input(UInt((bitWidth * ocDimension).W))

  val matrix_unit_output_data_vld = Output(Bool())
  val matrix_unit_output_data_rdy = Input(Bool())
  val matrix_unit_output_data_dat = Output(UInt((bitWidth * ocDimension).W))
  val matrix_unit_output_addr_vld = Output(Bool())
  val matrix_unit_output_addr_rdy = Input(Bool())
  val matrix_unit_output_addr_dat = Output(UInt(32.W))

  val matrix_unit_start_vld = Output(Bool())
  val matrix_unit_start_rdy = Input(Bool())
  val matrix_unit_done_vld  = Output(Bool())
  val matrix_unit_done_rdy  = Input(Bool())

  val vector_unit_params_in_vld = Input(Bool());
  val vector_unit_params_in_rdy = Output(Bool());
  val vector_unit_params_in_dat = Input(UInt(64.W));

  val vector_fetch_0_req_rdy  = Input(Bool())
  val vector_fetch_0_req_vld  = Output(Bool())
  val vector_fetch_0_req_dat  = Output(UInt(96.W))
  val vector_fetch_0_resp_rdy = Output(Bool())
  val vector_fetch_0_resp_vld = Input(Bool())
  val vector_fetch_0_resp_dat = Input(UInt((bitWidth * ocDimension).W))

  val vector_fetch_1_req_rdy  = Input(Bool())
  val vector_fetch_1_req_vld  = Output(Bool())
  val vector_fetch_1_req_dat  = Output(UInt(96.W))
  val vector_fetch_1_resp_rdy = Output(Bool())
  val vector_fetch_1_resp_vld = Input(Bool())
  val vector_fetch_1_resp_dat = Input(UInt((bitWidth * ocDimension).W))

  val vector_fetch_2_req_rdy  = Input(Bool())
  val vector_fetch_2_req_vld  = Output(Bool())
  val vector_fetch_2_req_dat  = Output(UInt(96.W))
  val vector_fetch_2_resp_rdy = Output(Bool())
  val vector_fetch_2_resp_vld = Input(Bool())
  val vector_fetch_2_resp_dat = Input(UInt((bitWidth * ocDimension).W))

  val vector_output_data_vld = Output(Bool())
  val vector_output_data_rdy = Input(Bool())
  val vector_output_data_dat = Output(UInt((bitWidth * ocDimension).W))
  val vector_output_addr_vld = Output(Bool())
  val vector_output_addr_rdy = Input(Bool())
  val vector_output_addr_dat = Output(UInt(32.W))

  val mx_scale_output_data_vld = Output(Bool())
  val mx_scale_output_data_rdy = Input(Bool())
  val mx_scale_output_data_dat = Output(UInt(scaleBitWidth.W))
  val mx_scale_output_addr_vld = Output(Bool())
  val mx_scale_output_addr_rdy = Input(Bool())
  val mx_scale_output_addr_dat = Output(UInt(32.W))

  val sparse_tensor_output_data_vld = Output(Bool())
  val sparse_tensor_output_data_rdy = Input(Bool())
  val sparse_tensor_output_data_dat = Output(UInt((bitWidth * ocDimension).W))
  val sparse_tensor_output_addr_vld = Output(Bool())
  val sparse_tensor_output_addr_rdy = Input(Bool())
  val sparse_tensor_output_addr_dat = Output(UInt(32.W))

  val vector_unit_start_vld = Output(Bool())
  val vector_unit_start_rdy = Input(Bool())
  val vector_unit_done_vld  = Output(Bool())
  val vector_unit_done_rdy  = Input(Bool())
}

class MatrixUnitMxIO(bitWidth: Int, scaleBitWidth: Int, oc: Int) extends Bundle {
  val matrix_unit_input_scale_req_rdy  = Input(Bool())
  val matrix_unit_input_scale_req_vld  = Output(Bool())
  val matrix_unit_input_scale_req_dat  = Output(UInt(96.W))
  val matrix_unit_input_scale_resp_rdy = Output(Bool())
  val matrix_unit_input_scale_resp_vld = Input(Bool())
  val matrix_unit_input_scale_resp_dat = Input(UInt(scaleBitWidth.W))

  val matrix_unit_weight_scale_req_rdy  = Input(Bool())
  val matrix_unit_weight_scale_req_vld  = Output(Bool())
  val matrix_unit_weight_scale_req_dat  = Output(UInt(96.W))
  val matrix_unit_weight_scale_resp_rdy = Output(Bool())
  val matrix_unit_weight_scale_resp_vld = Input(Bool())
  val matrix_unit_weight_scale_resp_dat = Input(UInt((bitWidth * oc).W))
}

class MatrixVectorUnitIO(bitWidth: Int, oc: Int) extends Bundle {
  val matrix_vector_unit_params_in_rdy = Output(Bool())
  val matrix_vector_unit_params_in_vld = Input(Bool())
  val matrix_vector_unit_params_in_dat = Input(UInt(64.W))

  val matrix_vector_unit_input_req_rdy  = Input(Bool())
  val matrix_vector_unit_input_req_vld  = Output(Bool())
  val matrix_vector_unit_input_req_dat  = Output(UInt(96.W))
  val matrix_vector_unit_input_resp_rdy = Output(Bool())
  val matrix_vector_unit_input_resp_vld = Input(Bool())
  val matrix_vector_unit_input_resp_dat = Input(UInt((bitWidth * oc).W))

  val matrix_vector_unit_weight_req_rdy  = Input(Bool())
  val matrix_vector_unit_weight_req_vld  = Output(Bool())
  val matrix_vector_unit_weight_req_dat  = Output(UInt(96.W))
  val matrix_vector_unit_weight_resp_rdy = Output(Bool())
  val matrix_vector_unit_weight_resp_vld = Input(Bool())
  val matrix_vector_unit_weight_resp_dat = Input(UInt((bitWidth * oc).W))

  val matrix_vector_unit_bias_req_rdy  = Input(Bool())
  val matrix_vector_unit_bias_req_vld  = Output(Bool())
  val matrix_vector_unit_bias_req_dat  = Output(UInt(96.W))
  val matrix_vector_unit_bias_resp_rdy = Output(Bool())
  val matrix_vector_unit_bias_resp_vld = Input(Bool())
  val matrix_vector_unit_bias_resp_dat = Input(UInt((bitWidth * oc).W))

  val matrix_vector_unit_weight_dq_scale_req_rdy  = Input(Bool())
  val matrix_vector_unit_weight_dq_scale_req_vld  = Output(Bool())
  val matrix_vector_unit_weight_dq_scale_req_dat  = Output(UInt(96.W))
  val matrix_vector_unit_weight_dq_scale_resp_rdy = Output(Bool())
  val matrix_vector_unit_weight_dq_scale_resp_vld = Input(Bool())
  val matrix_vector_unit_weight_dq_scale_resp_dat = Input(UInt((bitWidth * oc).W))

  val matrix_vector_unit_weight_dq_zp_req_rdy  = Input(Bool())
  val matrix_vector_unit_weight_dq_zp_req_vld  = Output(Bool())
  val matrix_vector_unit_weight_dq_zp_req_dat  = Output(UInt(96.W))
  val matrix_vector_unit_weight_dq_zp_resp_rdy = Output(Bool())
  val matrix_vector_unit_weight_dq_zp_resp_vld = Input(Bool())
  val matrix_vector_unit_weight_dq_zp_resp_dat = Input(UInt((bitWidth * oc).W))

  val matrix_vector_unit_start_vld = Output(Bool())
  val matrix_vector_unit_start_rdy = Input(Bool())
  val matrix_vector_unit_done_vld  = Output(Bool())
  val matrix_vector_unit_done_rdy  = Input(Bool())
}

// scalePortWidth is the accelerator's MVU_SCALE_PORT_WIDTH: one block scale
// per array-width group of the MVM unit's lanes.
class MatrixVectorUnitMxIO(bitWidth: Int, scalePortWidth: Int, oc: Int) extends Bundle {
  val matrix_vector_unit_input_scale_req_rdy  = Input(Bool())
  val matrix_vector_unit_input_scale_req_vld  = Output(Bool())
  val matrix_vector_unit_input_scale_req_dat  = Output(UInt(96.W))
  val matrix_vector_unit_input_scale_resp_rdy = Output(Bool())
  val matrix_vector_unit_input_scale_resp_vld = Input(Bool())
  val matrix_vector_unit_input_scale_resp_dat = Input(UInt(scalePortWidth.W))

  val matrix_vector_unit_weight_scale_req_rdy  = Input(Bool())
  val matrix_vector_unit_weight_scale_req_vld  = Output(Bool())
  val matrix_vector_unit_weight_scale_req_dat  = Output(UInt(96.W))
  val matrix_vector_unit_weight_scale_resp_rdy = Output(Bool())
  val matrix_vector_unit_weight_scale_resp_vld = Input(Bool())
  val matrix_vector_unit_weight_scale_resp_dat = Input(UInt(scalePortWidth.W))
}

class SpMMUnitIO(bitWidth: Int, scaleBitWidth: Int, oc: Int) extends Bundle {
  val spmm_unit_params_in_rdy = Output(Bool())
  val spmm_unit_params_in_vld = Input(Bool())
  val spmm_unit_params_in_dat = Input(UInt(64.W))

  val spmm_unit_input_indptr_req_rdy  = Input(Bool())
  val spmm_unit_input_indptr_req_vld  = Output(Bool())
  val spmm_unit_input_indptr_req_dat  = Output(UInt(96.W))
  val spmm_unit_input_indptr_resp_rdy = Output(Bool())
  val spmm_unit_input_indptr_resp_vld = Input(Bool())
  val spmm_unit_input_indptr_resp_dat = Input(UInt((bitWidth * oc).W))

  val spmm_unit_input_indices_req_rdy  = Input(Bool())
  val spmm_unit_input_indices_req_vld  = Output(Bool())
  val spmm_unit_input_indices_req_dat  = Output(UInt(96.W))
  val spmm_unit_input_indices_resp_rdy = Output(Bool())
  val spmm_unit_input_indices_resp_vld = Input(Bool())
  val spmm_unit_input_indices_resp_dat = Input(UInt((bitWidth * oc).W))

  val spmm_unit_input_data_req_rdy  = Input(Bool())
  val spmm_unit_input_data_req_vld  = Output(Bool())
  val spmm_unit_input_data_req_dat  = Output(UInt(96.W))
  val spmm_unit_input_data_resp_rdy = Output(Bool())
  val spmm_unit_input_data_resp_vld = Input(Bool())
  val spmm_unit_input_data_resp_dat = Input(UInt((bitWidth * oc).W))

  val spmm_unit_weight_req_rdy  = Input(Bool())
  val spmm_unit_weight_req_vld  = Output(Bool())
  val spmm_unit_weight_req_dat  = Output(UInt(96.W))
  val spmm_unit_weight_resp_rdy = Output(Bool())
  val spmm_unit_weight_resp_vld = Input(Bool())
  val spmm_unit_weight_resp_dat = Input(UInt((bitWidth * oc).W))

  val spmm_unit_weight_scale_req_rdy  = Input(Bool())
  val spmm_unit_weight_scale_req_vld  = Output(Bool())
  val spmm_unit_weight_scale_req_dat  = Output(UInt(96.W))
  val spmm_unit_weight_scale_resp_rdy = Output(Bool())
  val spmm_unit_weight_scale_resp_vld = Input(Bool())
  val spmm_unit_weight_scale_resp_dat = Input(UInt((bitWidth * oc).W))

  val spmm_unit_start_vld = Output(Bool())
  val spmm_unit_start_rdy = Input(Bool())
  val spmm_unit_done_vld  = Output(Bool())
  val spmm_unit_done_rdy  = Input(Bool())
}

class VoyagerIO(
  bitWidth:      Int,
  icDimension:   Int,
  ocDimension:   Int,
  scaleBitWidth: Int,
  supportMX:     Boolean,
  supportMVM:    Boolean,
  supportSpmm:   Boolean,
  mvmScaleWidth: Int)
    extends Record {

  private val baseBundle = new BaseVoyagerIO(bitWidth, icDimension, ocDimension, scaleBitWidth)
  private val mxBundle   = if (supportMX) Some(new MatrixUnitMxIO(bitWidth, scaleBitWidth, ocDimension)) else None
  private val mvmBundle  = if (supportMVM) Some(new MatrixVectorUnitIO(bitWidth, ocDimension)) else None
  private val mmBundle   =
    if (supportMX && supportMVM) Some(new MatrixVectorUnitMxIO(bitWidth, mvmScaleWidth, ocDimension))
    else None
  private val spmmBundle = if (supportSpmm) Some(new SpMMUnitIO(bitWidth, scaleBitWidth, ocDimension)) else None

  override val elements: ListMap[String, Data] = {
    ListMap.from(baseBundle.elements) ++
      mxBundle.map(b => ListMap.from(b.elements)).getOrElse(ListMap.empty) ++
      mvmBundle.map(b => ListMap.from(b.elements)).getOrElse(ListMap.empty) ++
      mmBundle.map(b => ListMap.from(b.elements)).getOrElse(ListMap.empty) ++
      spmmBundle.map(b => ListMap.from(b.elements)).getOrElse(ListMap.empty)
  }
}

class Accelerator(
  datatype:                  String,
  icDimension:               Int,
  ocDimension:               Int,
  inputBufferSize:           Int,
  weightBufferSize:          Int,
  accumBufferSize:           Int,
  clockPeriod:               Double,
  technology:                String,
  doubleBufferedAccumBuffer: Boolean = false,
  supportMVM:                Boolean = false,
  supportSpmm:               Boolean = false,
  bitWidth:                  Int,
  scaleBitWidth:             Int,
  supportMX:                 Boolean,
  mvUnitWidth:               Int,
  hwDMA:                     Boolean)
    extends BlackBox
    with HasBlackBoxPath {

  val io = IO(
    new VoyagerIO(
      bitWidth,
      icDimension,
      ocDimension,
      scaleBitWidth,
      supportMX,
      supportMVM,
      supportSpmm,
      scaleBitWidth * mvUnitWidth / ocDimension,
    )
  )

  val chipyardDir = System.getProperty("user.dir")
  val voyagerDir  = s"$chipyardDir/generators/voyager"

  // The HLS build directory, relative to generators/voyager (the Catapult
  // scripts join it to that root): the parameter set's own unless
  // VOYAGER_BUILD_DIR names another, so a test build never touches a cache
  // that cannot be regenerated.
  val paramsDir = s"""${datatype}_${icDimension}x${ocDimension}_
                     |${inputBufferSize}x${weightBufferSize}x${accumBufferSize}_
                     |${doubleBufferedAccumBuffer}_${supportMVM}_${supportSpmm}
                     |""".stripMargin.replaceAll("\n", "")
  val buildDir  = sys.env.getOrElse("VOYAGER_BUILD_DIR", s"build/$paramsDir")
  require(!buildDir.startsWith("/"), "VOYAGER_BUILD_DIR must be relative to generators/voyager")
  val buildPath = s"$voyagerDir/$buildDir"

  // Call Voyager to generate RTL
  val makeStr = s"""make -C $voyagerDir -j rtl
                   | BUILD_DIR=$buildDir
                   | DATATYPE=$datatype
                   | IC_DIMENSION=$icDimension
                   | OC_DIMENSION=$ocDimension
                   | CLOCK_PERIOD=$clockPeriod
                   | TECHNOLOGY=$technology
                   | INPUT_BUFFER_SIZE=$inputBufferSize
                   | WEIGHT_BUFFER_SIZE=$weightBufferSize
                   | ACCUM_BUFFER_SIZE=$accumBufferSize
                   | DOUBLE_BUFFERED_ACCUM_BUFFER=$doubleBufferedAccumBuffer
                   | SUPPORT_MVM=$supportMVM
                   | SUPPORT_SPMM=$supportSpmm
                   | MV_UNIT_WIDTH=$mvUnitWidth
                   |""".stripMargin.replace("\n", " ")

  require(makeStr.! == 0, "Failed to run RTL generation for Voyager")

  // Add the path to the generated Verilog
  val genVerilogPath =
    s"$buildPath/Catapult/$technology/clock_$clockPeriod/Accelerator/Accelerator.v1/concat_rtl.v"
  addPath(genVerilogPath)

  // Get value of environment variable CONFIG
  val config = System.getenv("CONFIG")

  val shellPath = s"$voyagerDir/build/configs/${config}/config.sh"
  val shellStr  = s"""export DATATYPE=${datatype}
                    |export IC_DIMENSION=${icDimension}
                    |export OC_DIMENSION=${ocDimension}
                    |export INPUT_BUFFER_SIZE=${inputBufferSize}
                    |export WEIGHT_BUFFER_SIZE=${weightBufferSize}
                    |export ACCUM_BUFFER_SIZE=${accumBufferSize}
                    |export DOUBLE_BUFFERED_ACCUM_BUFFER=${doubleBufferedAccumBuffer}
                    |export SUPPORT_MVM=${supportMVM}
                    |export SUPPORT_SPMM=${supportSpmm}
                    |export MV_UNIT_WIDTH=${mvUnitWidth}
                    |""".stripMargin + (if (hwDMA) "export HW_DMA=true\n" else "")
  val file      = new java.io.File(shellPath);
  if (!file.getParentFile().exists()) {
    file.getParentFile().mkdirs();
  }
  val shellFile = new java.io.PrintWriter(shellPath)
  shellFile.write(shellStr)
  shellFile.close()
}

class TLVoyager(
  address:       BigInt,
  pbusBeatBytes: Int,
  sbusBeatBytes: Int,
)(
  implicit p:    Parameters)
    extends ClockSinkDomain(ClockSinkParameters())(p) {
  override lazy val desiredName = "TLVoyager"

  val config             = p(VoyagerKey).get
  val datatypeWidth      = config.datatype match {
    case "E4M3"   => 8
    case "P8"     => 8
    case "INT8"   => 8
    case "MXINT8" => 8
    case "BF16"   => 16
    case "FP32"   => 32
    case "MXNF4"  => 4
    case _        => throw new Exception("Unknown width for datatype. Please update the datatypeWidth mapping")
  }
  val icPortWidth        = datatypeWidth * config.icDimension
  val ocPortWidth        = datatypeWidth * config.ocDimension
  val scaleDatatypeWidth = config.scaleDatatype match {
    case "E8M0" => 8
    case "E5M3" => 8
    case ""     => 0
    case _      => throw new Exception("Unknown width for scale datatype. Please update the scaleDatatypeWidth mapping")
  }
  val supportMX          = config.datatype.startsWith("MX")
  val supportMVM         = config.supportMVM
  // The DMA engine and the write-acknowledged posts, in a config that has them.
  val dmaParams          = p(VoyagerDMAKey)
  // The MVM unit's lanes, and its scale ports' width: one block scale per
  // array-width group of lanes (the accelerator's MVU_SCALE_PORT_WIDTH).
  val mvUnitWidth        = config.mvUnitWidth.getOrElse(config.ocDimension)
  val mvmScaleWidth      = scaleDatatypeWidth * mvUnitWidth / config.ocDimension

  val device       = new SimpleDevice("voyager", Seq("stanford,voyager"))
  val voyager_node = TLRegisterNode(
    address     = Seq(AddressSet(address, 0xfff)),
    device      = device,
    beatBytes   = pbusBeatBytes,
    concurrency = 1,
  )

  // Matrix Unit
  val matrix_unit_input_tl_node        = TLIdentityNode()
  val matrix_unit_weight_tl_node       = TLIdentityNode()
  val matrix_unit_bias_tl_node         = TLIdentityNode()
  val matrix_unit_input_scale_tl_node  = TLIdentityNode()
  val matrix_unit_weight_scale_tl_node = TLIdentityNode()
  val matrix_unit_output_tl_node       = TLIdentityNode()
  // Vector Unit
  val vector_fetch_0_tl_node           = TLIdentityNode()
  val vector_fetch_1_tl_node           = TLIdentityNode()
  val vector_fetch_2_tl_node           = TLIdentityNode()
  val vector_output_tl_node            = TLIdentityNode()
  val mx_scale_output_tl_node          = TLIdentityNode()
  val sparse_tensor_output_tl_node     = TLIdentityNode()
  // Matrix Vector Unit
  val mvm_unit_input_tl_node           = TLIdentityNode()
  val mvm_unit_weight_tl_node          = TLIdentityNode()
  val mvm_unit_bias_tl_node            = TLIdentityNode()
  val mvm_unit_input_scale_tl_node     = TLIdentityNode()
  val mvm_unit_weight_scale_tl_node    = TLIdentityNode()
  val mvm_unit_weight_dq_scale_tl_node = TLIdentityNode()
  val mvm_unit_weight_dq_zp_tl_node    = TLIdentityNode()
  // SpMM Unit
  val spmm_unit_input_indptr_tl_node   = TLIdentityNode()
  val spmm_unit_input_indices_tl_node  = TLIdentityNode()
  val spmm_unit_input_data_tl_node     = TLIdentityNode()
  val spmm_unit_weight_tl_node         = TLIdentityNode()
  val spmm_unit_weight_scale_tl_node   = TLIdentityNode()

  def connectInterface(sourceNode: TLNode, destNode: TLNode, portWidth: Int): Unit = {
    if (portWidth != sbusBeatBytes * 8) {
      destNode := TLFIFOFixer(TLFIFOFixer.all) := TLWidthWidget(portWidth / 8) := sourceNode
    } else {
      destNode := TLFIFOFixer(TLFIFOFixer.all) := sourceNode
    }
  }

  def makeRead(name: String, width: Int, destNode: TLNode, enabled: Boolean = true) = {
    if (enabled) {
      val interface = LazyModule(new TLBurstReadInterface(name, width, 16)(p))
      connectInterface(interface.node, destNode, width)
      interface
    } else null
  }

  def makeWrite(name: String, width: Int, destNode: TLNode, enabled: Boolean = true) = {
    if (enabled) {
      val interface = LazyModule(new TLFullWriteInterface(name, width, 16, 16, dmaParams.isDefined)(p))
      connectInterface(interface.node, destNode, width)
      interface
    } else null
  }

  // --- Matrix Unit ---
  val matrix_unit_input_interface  = makeRead("Matrix Unit Input", icPortWidth, matrix_unit_input_tl_node)
  val matrix_unit_weight_interface = makeRead("Matrix Unit Weight", ocPortWidth, matrix_unit_weight_tl_node)
  val matrix_unit_bias_interface   = makeRead("Matrix Unit Bias", ocPortWidth, matrix_unit_bias_tl_node)

  val matrix_unit_input_scale_interface  =
    makeRead("Matrix Unit Input Scale", scaleDatatypeWidth, matrix_unit_input_scale_tl_node, supportMX)
  val matrix_unit_weight_scale_interface =
    makeRead("Matrix Unit Weight Scale", ocPortWidth, matrix_unit_weight_scale_tl_node, supportMX)

  val matrix_unit_output_interface = makeWrite("Matrix Unit Output", ocPortWidth, matrix_unit_output_tl_node)

  // --- Vector Unit ---
  val vector_fetch_0_interface       = makeRead("Vector Fetch 0", ocPortWidth, vector_fetch_0_tl_node)
  val vector_fetch_1_interface       = makeRead("Vector Fetch 1", ocPortWidth, vector_fetch_1_tl_node)
  val vector_fetch_2_interface       = makeRead("Vector Fetch 2", ocPortWidth, vector_fetch_2_tl_node)
  val vector_output_interface        = makeWrite("Vector Output", ocPortWidth, vector_output_tl_node)
  val mx_scale_output_interface      = makeWrite("MX Scale Output", 8, mx_scale_output_tl_node)
  val sparse_tensor_output_interface = makeWrite("Sparse Tensor Output", ocPortWidth, sparse_tensor_output_tl_node)

  // --- Matrix Vector Unit ---
  val mvm_unit_input_interface  = makeRead("Matrix Vector Unit Input", ocPortWidth, mvm_unit_input_tl_node, supportMVM)
  val mvm_unit_weight_interface =
    makeRead("Matrix Vector Unit Weight", ocPortWidth, mvm_unit_weight_tl_node, supportMVM)
  val mvm_unit_bias_interface   = makeRead("Matrix Vector Unit Bias", ocPortWidth, mvm_unit_bias_tl_node, supportMVM)

  val mvm_unit_input_scale_interface     = makeRead(
    "Matrix Vector Unit Input Scale",
    mvmScaleWidth,
    mvm_unit_input_scale_tl_node,
    supportMVM && supportMX,
  )
  val mvm_unit_weight_scale_interface    = makeRead(
    "Matrix Vector Unit Weight Scale",
    mvmScaleWidth,
    mvm_unit_weight_scale_tl_node,
    supportMVM && supportMX,
  )
  val mvm_unit_weight_dq_scale_interface = makeRead(
    "Matrix Vector Unit Weight DQ Scale",
    ocPortWidth,
    mvm_unit_weight_dq_scale_tl_node,
    supportMVM && supportMX,
  )
  val mvm_unit_weight_dq_zp_interface    = makeRead(
    "Matrix Vector Unit Weight DQ Zero Point",
    ocPortWidth,
    mvm_unit_weight_dq_zp_tl_node,
    supportMVM && supportMX,
  )

  // SpMM Unit
  val spmm_unit_input_indptr_interface  =
    makeRead("SpMM Unit Input Indptr", ocPortWidth, spmm_unit_input_indptr_tl_node, config.supportSpmm)
  val spmm_unit_input_indices_interface =
    makeRead("SpMM Unit Input Indices", ocPortWidth, spmm_unit_input_indices_tl_node, config.supportSpmm)
  val spmm_unit_input_data_interface    =
    makeRead("SpMM Unit Input Data", ocPortWidth, spmm_unit_input_data_tl_node, config.supportSpmm)
  val spmm_unit_weight_interface        =
    makeRead("SpMM Unit Weight", ocPortWidth, spmm_unit_weight_tl_node, config.supportSpmm)
  val spmm_unit_weight_scale_interface  =
    makeRead("SpMM Unit Weight Scale", ocPortWidth, spmm_unit_weight_scale_tl_node, config.supportSpmm)

  // The DMA engine's two queues, loads and stores, each an engine of its own
  // on the system bus (VoyagerDMA.scala).
  val dma_engines  = dmaParams.toSeq.flatMap { params =>
    Seq("Voyager DMA Load", "Voyager DMA Store").zipWithIndex.map { case (name, queue) =>
      LazyModule(new VoyagerDMAEngine(name, queue, params)(p))
    }
  }
  val dma_tl_nodes = dma_engines.flatMap(engine => Seq(engine.readNode, engine.writeNode)).map { client =>
    val node = TLIdentityNode()
    node := client
    node
  }

  val intNode = IntSourceNode(IntSourcePortSimple())

  override lazy val module = new VoyagerImpl

  class VoyagerImpl extends Impl {
    withClockAndReset(clock, reset) {
      val config      = p(VoyagerKey).get
      val accelerator = Module(
        new Accelerator(
          config.datatype,
          config.icDimension,
          config.ocDimension,
          config.inputBufferSize,
          config.weightBufferSize,
          config.accumBufferSize,
          config.clockPeriod,
          config.technology,
          config.doubleBufferedAccumBuffer,
          config.supportMVM,
          config.supportSpmm,
          datatypeWidth,
          scaleDatatypeWidth,
          supportMX,
          mvUnitWidth,
          dmaParams.isDefined,
        )
      )

      val voyager_base_addr = Reg(UInt(64.W))
      val voyager_reg_map   = ArrayBuffer[(Int, Seq[RegField])]()

      // -------------------------------
      // Synchronization counters
      // -------------------------------
      // 32 count-up counters shared by the units and the DMA engine. Each
      // unit's UnitSync (Sync.scala) holds the waits and the post of every
      // pass dispatched on it. Register map: soc/firmware/common/voyager_address.h.
      val numCounters = 32
      val counters    = RegInit(VecInit(Seq.fill(numCounters)(0.U(32.W))))

      // Unit order: matrix, vector, mvm, spmm (the firmware's unit codes). How
      // far the CPU runs ahead is bounded by a unit's params path alone: the
      // one-word queue below plus the records its deserializer and dispatch
      // thread hold, at most two complete passes and a third's DISPATCH
      // before a params write stalls. Three dispatched passes carry at most
      // 15 waits; passes started and not done are bounded by the unit's
      // own pipeline, fewer than the started queue holds.
      val syncs = Seq.fill(4) {
        Module(new UnitSync(numCounters, waitDepth = 16, passDepth = 4, startedDepth = 8))
      }
      syncs.foreach { sync =>
        sync.io.counters           := counters
        sync.io.start_vld          := false.B
        sync.io.done               := false.B
        sync.io.wait_req.valid     := false.B
        sync.io.wait_req.bits      := 0.U
        sync.io.post_req.valid     := false.B
        sync.io.post_req.bits      := 0.U
        sync.io.dispatch_req.valid := false.B
        sync.io.dispatch_req.bits  := 0.U
      }

      // The DMA engine's completion port: +1 to a counter when a copy lands.
      val counter_add_port = Module(new CounterAddPort)

      // In a DMA config a unit's post waits until every write the unit issued
      // before its done is acknowledged: each unit's write interfaces, by the
      // accelerator's port prefix, and its AckedPost; unitWrites connects the
      // counts once the ports are mapped. An accelerator without MX or SpMM
      // leaves that output's valid undriven, so it is not counted.
      val ackedPosts = dmaParams.map { _ =>
        val vectorOutputs = Seq(
          Some("vector_output" -> vector_output_interface),
          Option.when(supportMX)("mx_scale_output" -> mx_scale_output_interface),
          Option.when(config.supportSpmm)("sparse_tensor_output" -> sparse_tensor_output_interface),
        ).flatten
        val unitOutputs = Seq(
          Seq("matrix_unit_output" -> matrix_unit_output_interface),
          vectorOutputs,
          Nil,
          Nil,
        )
        syncs.zip(unitOutputs).zipWithIndex.map { case ((sync, outputs), unit) =>
          val acked = Module(
            new AckedPost(idBits = log2Ceil(numCounters), depth = 8, unit = unit, writers = outputs.size)
          )
          acked.io.posted := sync.io.posted
          (acked, outputs)
        }
      }
      val posted     = ackedPosts.map(_.map(_._1.io.out)).getOrElse(syncs.map(_.io.posted))

      // CPU-side reset and add of one counter: the add stands in for a copy's
      // completion when the data is preloaded (full JTAG mode, the chip).
      val ctr_reset = Wire(Decoupled(UInt(8.W)))
      ctr_reset.ready := true.B
      val ctr_add = Wire(Decoupled(UInt(8.W)))
      ctr_add.ready := true.B

      for (i <- 0 until numCounters) {
        val posts = posted.map(post => post.valid && post.bits === i.U)
        val added = Seq(
          counter_add_port.io.add_vld && counter_add_port.io.add_id === i.U,
          ctr_add.valid && ctr_add.bits === i.U,
        ) ++ dma_engines.map(engine => engine.module.io.posted.valid && engine.module.io.posted.bits === i.U)
        val incs  = (posts ++ added).map(_.asUInt).reduce(_ +& _)
        when(ctr_reset.valid && ctr_reset.bits === i.U) {
          counters(i) := 0.U
        }.elsewhen(incs =/= 0.U) {
          counters(i) := counters(i) + incs
        }
      }

      // The mailbox doorbell: a count the firmware advances by writing the
      // register; the SoC testbench (VerificationCollateral.v) services the
      // mailbox when it changes.
      val doorbell = RegInit(0.U(32.W))
      dontTouch(doorbell)
      val ring_doorbell = RegWriteFn((valid: Bool, data: UInt) => {
        when(valid) { doorbell := doorbell + 1.U }
        true.B
      })

      voyager_reg_map ++= Seq(
        0x500 -> Seq(RegField(32, doorbell, ring_doorbell)),
        0x508 -> Seq(RegField.w(8, ctr_reset)),
        0x510 -> Seq(RegField.w(8, ctr_add)),
      )
      voyager_reg_map ++= (0 until numCounters).map { i =>
        (0x600 + 8 * i) -> Seq(RegField.r(32, counters(i)))
      }

      val bank_0_loaded = RegInit(false.B)
      val bank_1_loaded = RegInit(false.B)

      val io_map = accelerator.io.elements

      def bool(name: String) = io_map(name).asInstanceOf[Bool]

      def connectInput(
        prefix:    String,
        dataWidth: Int,
        interface: TLBurstReadInterface,
        fifoDepth: Int = 4,
      ): (Queue[UInt], Queue[UInt]) = {
        val req_q  = Module(new Queue(UInt((64 + 32).W), fifoDepth))
        val resp_q = Module(new Queue(UInt(dataWidth.W), fifoDepth))

        interface.module.io.baseAddress := voyager_base_addr

        req_q.io.enq.valid           := io_map(s"${prefix}_req_vld")
        req_q.io.enq.bits            := io_map(s"${prefix}_req_dat")
        io_map(s"${prefix}_req_rdy") := req_q.io.enq.ready
        interface.module.io.request  <> req_q.io.deq

        resp_q.io.enq                 <> interface.module.io.response
        io_map(s"${prefix}_resp_dat") := resp_q.io.deq.bits
        io_map(s"${prefix}_resp_vld") := resp_q.io.deq.valid
        resp_q.io.deq.ready           := io_map(s"${prefix}_resp_rdy")

        (req_q, resp_q)
      }

      def connectOutput(
        prefix:    String,
        dataWidth: Int,
        interface: TLFullWriteInterface,
        fifoDepth: Int = 4,
      ): (Queue[UInt], Queue[UInt]) = {
        val addr_q = Module(new Queue(UInt(32.W), fifoDepth))
        val data_q = Module(new Queue(UInt(dataWidth.W), fifoDepth))

        interface.module.io.baseAddress := voyager_base_addr

        // Address channel
        addr_q.io.enq.valid           := io_map(s"${prefix}_addr_vld")
        addr_q.io.enq.bits            := io_map(s"${prefix}_addr_dat")
        io_map(s"${prefix}_addr_rdy") := addr_q.io.enq.ready
        interface.module.io.addr      <> addr_q.io.deq

        // Data channel
        data_q.io.enq.valid           := io_map(s"${prefix}_data_vld")
        data_q.io.enq.bits            := io_map(s"${prefix}_data_dat")
        io_map(s"${prefix}_data_rdy") := data_q.io.enq.ready
        interface.module.io.data      <> data_q.io.deq

        (addr_q, data_q)
      }

      // A unit's start and done handshakes through its UnitSync, and its
      // register block at `base` (layout in voyager_address.h).
      def connectUnit(unit: Int, prefix: String, params_q: Queue[UInt], base: Int): Unit = {
        val sync = syncs(unit)
        io_map(s"${prefix}_start_rdy") := sync.io.start_rdy
        sync.io.start_vld              := bool(s"${prefix}_start_vld")
        io_map(s"${prefix}_done_rdy")  := true.B
        sync.io.done                   := bool(s"${prefix}_done_vld")

        val cycle_count = WideCounter(64, sync.io.running)

        voyager_reg_map ++= Seq(
          base + 0x00 -> Seq(RegField.w(64, params_q.io.enq)),
          base + 0x08 -> Seq(RegField.r(64, cycle_count.value)),
          base + 0x10 -> Seq(RegField.r(8, sync.io.inflight)),
          base + 0x18 -> Seq(RegField.w(64, sync.io.wait_req)),
          base + 0x20 -> Seq(RegField.w(8, sync.io.post_req)),
          base + 0x28 -> Seq(RegField.w(8, sync.io.dispatch_req)),
        )
      }

      // A DMA config's per-unit write counts: per write interface, the words
      // the unit has handed it (this cycle's included), and its progress.
      def unitWrites(unit: Int): Unit = ackedPosts.foreach { posts =>
        val (acked, outputs) = posts(unit)
        outputs.zipWithIndex.foreach { case ((prefix, interface), i) =>
          val fire  = bool(s"${prefix}_data_vld") && bool(s"${prefix}_data_rdy")
          val count = RegInit(0.U(32.W))
          when(fire) { count := count + 1.U }
          acked.io.issued(i) := count + fire.asUInt
          acked.io.counts(i) := interface.module.writeCounts.get
        }
      }

      io_map("clk")  := clock
      io_map("rstn") := ~(reset.asBool)

      // -------------------------------
      // Matrix Unit
      // -------------------------------

      val matrix_unit_params_in_q = Module(new Queue(UInt(64.W), 1))
      io_map("matrix_unit_params_in_vld")  := matrix_unit_params_in_q.io.deq.valid
      io_map("matrix_unit_params_in_dat")  := matrix_unit_params_in_q.io.deq.bits
      matrix_unit_params_in_q.io.deq.ready := io_map("matrix_unit_params_in_rdy")

      val (matrix_unit_input_req_q, matrix_unit_input_resp_q)   =
        connectInput("matrix_unit_input", icPortWidth, matrix_unit_input_interface)
      val (matrix_unit_weight_req_q, matrix_unit_weight_resp_q) =
        connectInput("matrix_unit_weight", ocPortWidth, matrix_unit_weight_interface)
      val (matrix_unit_bias_req_q, matrix_unit_bias_resp_q)     =
        connectInput("matrix_unit_bias", ocPortWidth, matrix_unit_bias_interface)
      val matrix_unit_output_addr_q, matrix_unit_output_data_q  =
        connectOutput("matrix_unit_output", ocPortWidth, matrix_unit_output_interface)

      if (supportMX) {
        val (matrix_unit_input_scale_req_q, matrix_unit_input_scale_resp_q)   =
          connectInput("matrix_unit_input_scale", scaleDatatypeWidth, matrix_unit_input_scale_interface)
        val (matrix_unit_weight_scale_req_q, matrix_unit_weight_scale_resp_q) =
          connectInput("matrix_unit_weight_scale", ocPortWidth, matrix_unit_weight_scale_interface)
      }

      connectUnit(0, "matrix_unit", matrix_unit_params_in_q, 0x100)
      unitWrites(0)

      // -------------------------------
      // Vector Unit
      // -------------------------------

      val vector_unit_params_in_q = Module(new Queue(UInt(64.W), 1))
      io_map("vector_unit_params_in_vld")  := vector_unit_params_in_q.io.deq.valid
      io_map("vector_unit_params_in_dat")  := vector_unit_params_in_q.io.deq.bits
      vector_unit_params_in_q.io.deq.ready := io_map("vector_unit_params_in_rdy")

      val (vector_fetch_0_req_q, vector_fetch_0_resp_q) =
        connectInput("vector_fetch_0", ocPortWidth, vector_fetch_0_interface)
      val (vector_fetch_1_req_q, vector_fetch_1_resp_q) =
        connectInput("vector_fetch_1", ocPortWidth, vector_fetch_1_interface)
      val (vector_fetch_2_req_q, vector_fetch_2_resp_q) =
        connectInput("vector_fetch_2", ocPortWidth, vector_fetch_2_interface)

      val vector_output_addr_q, vector_output_data_q     =
        connectOutput("vector_output", ocPortWidth, vector_output_interface)
      val mx_scale_output_addr_q, mx_scale_output_data_q =
        connectOutput("mx_scale_output", 8, mx_scale_output_interface)
      val sparse_output_addr_q, sparse_output_data_q     =
        connectOutput("sparse_tensor_output", ocPortWidth, sparse_tensor_output_interface)

      connectUnit(1, "vector_unit", vector_unit_params_in_q, 0x200)
      unitWrites(1)

      // -------------------------------
      // Matrix Vector Unit
      // -------------------------------

      if (supportMVM) {
        val mvm_unit_params_in_q = Module(new Queue(UInt(64.W), 1))
        io_map("matrix_vector_unit_params_in_vld") := mvm_unit_params_in_q.io.deq.valid
        io_map("matrix_vector_unit_params_in_dat") := mvm_unit_params_in_q.io.deq.bits
        mvm_unit_params_in_q.io.deq.ready          := io_map("matrix_vector_unit_params_in_rdy")

        val (mvm_unit_input_req_q, mvm_unit_input_resp_q)                     =
          connectInput("matrix_vector_unit_input", ocPortWidth, mvm_unit_input_interface)
        val (mvm_unit_weight_req_q, mvm_unit_weight_resp_q)                   =
          connectInput("matrix_vector_unit_weight", ocPortWidth, mvm_unit_weight_interface)
        val (mvm_unit_bias_req_q, mvm_unit_bias_resp_q)                       =
          connectInput("matrix_vector_unit_bias", ocPortWidth, mvm_unit_bias_interface)
        val (mvm_unit_weight_dq_scale_req_q, mvm_unit_weight_dq_scale_resp_q) =
          connectInput("matrix_vector_unit_weight_dq_scale", ocPortWidth, mvm_unit_weight_dq_scale_interface)
        val (mvm_unit_weight_dq_zp_req_q, mvm_unit_weight_dq_zp_resp_q)       =
          connectInput("matrix_vector_unit_weight_dq_zp", ocPortWidth, mvm_unit_weight_dq_zp_interface)

        if (supportMX) {
          val (mvm_unit_input_scale_req_q, mvm_unit_input_scale_resp_q)   =
            connectInput("matrix_vector_unit_input_scale", mvmScaleWidth, mvm_unit_input_scale_interface)
          val (mvm_unit_weight_scale_req_q, mvm_unit_weight_scale_resp_q) =
            connectInput("matrix_vector_unit_weight_scale", mvmScaleWidth, mvm_unit_weight_scale_interface)
        }

        connectUnit(2, "matrix_vector_unit", mvm_unit_params_in_q, 0x300)
      }

      // -------------------------------
      // SpMM Unit
      // -------------------------------

      if (config.supportSpmm) {
        val spmm_unit_params_in_q = Module(new Queue(UInt(64.W), 1))
        io_map("spmm_unit_params_in_vld")  := spmm_unit_params_in_q.io.deq.valid
        io_map("spmm_unit_params_in_dat")  := spmm_unit_params_in_q.io.deq.bits
        spmm_unit_params_in_q.io.deq.ready := io_map("spmm_unit_params_in_rdy")

        val (spmm_unit_input_indptr_req_q, spmm_unit_input_indptr_resp_q)   =
          connectInput("spmm_unit_input_indptr", ocPortWidth, spmm_unit_input_indptr_interface)
        val (spmm_unit_input_indices_req_q, spmm_unit_input_indices_resp_q) =
          connectInput("spmm_unit_input_indices", ocPortWidth, spmm_unit_input_indices_interface)
        val (spmm_unit_input_data_req_q, spmm_unit_input_data_resp_q)       =
          connectInput("spmm_unit_input_data", ocPortWidth, spmm_unit_input_data_interface)
        val (spmm_unit_weight_req_q, spmm_unit_weight_resp_q)               =
          connectInput("spmm_unit_weight", ocPortWidth, spmm_unit_weight_interface)

        if (supportMX) {
          val (spmm_unit_weight_scale_req_q, spmm_unit_weight_scale_resp_q) =
            connectInput("spmm_unit_weight_scale", ocPortWidth, spmm_unit_weight_scale_interface)
        }

        connectUnit(3, "spmm_unit", spmm_unit_params_in_q, 0x400)
      }

      // The accelerator is busy while any unit has a pass dispatched, queued
      // behind a wait included (what ctr_drain polls); its cycle count runs
      // while a pass is running.
      val accel_busy              =
        (syncs.map(_.io.inflight =/= 0.U) ++ ackedPosts.toSeq.flatten.map(_._1.io.pending)).reduce(_ || _)
      val accel_running           = syncs.map(_.io.running).reduce(_ || _)
      val accelerator_cycle_count = WideCounter(64, accel_running)

      // -------------------------------
      // Interrupts
      // -------------------------------

      val int_status = RegInit(0.U(16.W))
      val int_enable = RegInit(0.U(16.W))

      val bank_0_loaded_prev = RegNext(bank_0_loaded)
      val bank_1_loaded_prev = RegNext(bank_1_loaded)
      val accel_busy_prev    = RegNext(accel_busy)

      val int_set_signals = Cat(
        0.U(13.W),                            // bits 15-3 (unused)
        !accel_busy && accel_busy_prev,       // bit 2
        bank_1_loaded && !bank_1_loaded_prev, // bit 1
        bank_0_loaded && !bank_0_loaded_prev, // bit 0
      )

      val (interrupts, _) = intNode.out(0)
      interrupts(0) := (int_status & int_enable).orR

      voyager_reg_map ++= Seq(
        0x000 -> Seq(RegField(64, voyager_base_addr)),
        0x008 -> Seq(RegField.r(64, accelerator_cycle_count.value)),
        0x010 -> Seq(RegField.r(1, accel_busy)),
        0x01a -> Seq(RegField(8, bank_0_loaded)),
        0x01b -> Seq(RegField(8, bank_1_loaded)),
        0x020 -> Seq(RegField.w1ToClear(16, int_status, int_set_signals)),
        0x028 -> Seq(RegField(16, int_enable)),
      )
      // The DMA engine's registers (layout in soc/firmware/common/dma.h): a
      // staged descriptor, pushed onto queue PUSH's data names (0 loads, 1
      // stores); BUSY bit q while queue q holds or runs a copy; each queue's
      // busy cycles, and each unit's counter-wait stall cycles.
      dmaParams.foreach { params =>
        val initial = Wire(new DMADescriptor)
        initial := 0.U.asTypeOf(initial)
        initial.count.foreach(_ := 1.U)
        val staged  = RegInit(initial)

        val queues = dma_engines.map { engine =>
          val queue = Module(new Queue(new DMADescriptor, params.queueDepth))
          engine.module.io.desc <> queue.io.deq
          queue
        }
        val push   = Wire(Decoupled(UInt(8.W)))
        queues.zipWithIndex.foreach { case (queue, q) =>
          queue.io.enq.valid := push.valid && push.bits === q.U
          queue.io.enq.bits  := staged
        }
        push.ready := VecInit(queues.map(_.io.enq.ready))(push.bits(0))

        val busy       = dma_engines.zip(queues).map { case (engine, queue) =>
          engine.module.io.busy || queue.io.deq.valid
        }
        val busyCycles = busy.map(b => WideCounter(64, b))
        // Per unit: cycles it sat idle while its next dispatched pass waited
        // on its counters.
        val waitStalls = syncs.map(sync =>
          WideCounter(64, sync.io.inflight =/= 0.U && !sync.io.running && !sync.io.start_rdy)
        )

        voyager_reg_map ++= Seq(
          0x700 -> Seq(RegField(64, staged.src)),
          0x708 -> Seq(RegField(64, staged.dst)),
          0x710 -> Seq(RegField(32, staged.inner)),
          0x718 -> Seq(RegField(16, staged.ctrl)),
        )
        voyager_reg_map ++= (0 until 3).flatMap { k =>
          Seq(
            (0x720 + 0x18 * k) -> Seq(RegField(32, staged.count(k))),
            (0x728 + 0x18 * k) -> Seq(RegField(64, staged.srcStride(k))),
            (0x730 + 0x18 * k) -> Seq(RegField(64, staged.dstStride(k))),
          )
        }
        voyager_reg_map ++= Seq(
          0x768 -> Seq(RegField.w(8, push)),
          0x770 -> Seq(RegField.r(8, Cat(busy.reverse).pad(8))),
          0x778 -> Seq(RegField.r(64, busyCycles(0).value)),
          0x780 -> Seq(RegField.r(64, busyCycles(1).value)),
        )
        voyager_reg_map ++= waitStalls.zipWithIndex.map { case (stall, unit) =>
          (0x788 + 8 * unit) -> Seq(RegField.r(64, stall.value))
        }
      }
      voyager_node.regmap(voyager_reg_map.toSeq: _*)
    }
  }
}

trait CanHavePeripheryVoyager { this: BaseSubsystem =>
  private val pbus = locateTLBusWrapper(PBUS)
  private val sbus = locateTLBusWrapper(SBUS)

  p(VoyagerKey).map { k =>
    val voyager = LazyModule(new TLVoyager(k.mmioAddress, pbus.beatBytes, sbus.beatBytes)(p))
    voyager.clockNode := sbus.fixedClockNode

    sbus.coupleFrom("voyager_matrix_unit_input") { _ := sbus { TLBuffer() } := voyager.matrix_unit_input_tl_node }
    sbus.coupleFrom("voyager_matrix_unit_weight") { _ := sbus { TLBuffer() } := voyager.matrix_unit_weight_tl_node }
    sbus.coupleFrom("voyager_matrix_unit_bias") { _ := sbus { TLBuffer() } := voyager.matrix_unit_bias_tl_node }

    if (k.datatype.startsWith("MX")) {
      sbus.coupleFrom("voyager_matrix_unit_input_scale") {
        _ := sbus { TLBuffer() } := voyager.matrix_unit_input_scale_tl_node
      }
      sbus.coupleFrom("voyager_matrix_unit_weight_scale") {
        _ := sbus { TLBuffer() } := voyager.matrix_unit_weight_scale_tl_node
      }
    }
    sbus.coupleFrom("voyager_matrix_unit_output") { _ := sbus { TLBuffer() } := voyager.matrix_unit_output_tl_node }

    sbus.coupleFrom("voyager_vector_fetch_0") { _ := sbus { TLBuffer() } := voyager.vector_fetch_0_tl_node }
    sbus.coupleFrom("voyager_vector_fetch_1") { _ := sbus { TLBuffer() } := voyager.vector_fetch_1_tl_node }
    sbus.coupleFrom("voyager_vector_fetch_2") { _ := sbus { TLBuffer() } := voyager.vector_fetch_2_tl_node }
    sbus.coupleFrom("voyager_vector_output") { _ := sbus { TLBuffer() } := voyager.vector_output_tl_node }
    sbus.coupleFrom("voyager_mx_scale_output") { _ := sbus { TLBuffer() } := voyager.mx_scale_output_tl_node }
    sbus.coupleFrom("voyager_sparse_tensor_output") { _ := sbus { TLBuffer() } := voyager.sparse_tensor_output_tl_node }

    if (k.supportMVM) {
      sbus.coupleFrom("voyager_matrix_vector_unit_input") {
        _ := sbus { TLBuffer() } := voyager.mvm_unit_input_tl_node
      }
      sbus.coupleFrom("voyager_matrix_vector_unit_weight") {
        _ := sbus { TLBuffer() } := voyager.mvm_unit_weight_tl_node
      }
      sbus.coupleFrom("voyager_matrix_vector_unit_bias") {
        _ := sbus { TLBuffer() } := voyager.mvm_unit_bias_tl_node
      }
      sbus.coupleFrom("voyager_matrix_vector_unit_weight_dq_scale") {
        _ := sbus { TLBuffer() } := voyager.mvm_unit_weight_dq_scale_tl_node
      }
      sbus.coupleFrom("voyager_matrix_vector_unit_weight_dq_zp") {
        _ := sbus { TLBuffer() } := voyager.mvm_unit_weight_dq_zp_tl_node
      }

      if (k.datatype.startsWith("MX")) {
        sbus.coupleFrom("voyager_matrix_vector_unit_input_scale") {
          _ := sbus { TLBuffer() } := voyager.mvm_unit_input_scale_tl_node
        }
        sbus.coupleFrom("voyager_matrix_vector_unit_weight_scale") {
          _ := sbus { TLBuffer() } := voyager.mvm_unit_weight_scale_tl_node
        }
      }
    }

    if (k.supportSpmm) {
      sbus.coupleFrom("voyager_spmm_unit_input_indptr") {
        _ := sbus { TLBuffer() } := voyager.spmm_unit_input_indptr_tl_node
      }
      sbus.coupleFrom("voyager_spmm_unit_input_indices") {
        _ := sbus { TLBuffer() } := voyager.spmm_unit_input_indices_tl_node
      }
      sbus.coupleFrom("voyager_spmm_unit_input_data") {
        _ := sbus { TLBuffer() } := voyager.spmm_unit_input_data_tl_node
      }
      sbus.coupleFrom("voyager_spmm_unit_weight") {
        _ := sbus { TLBuffer() } := voyager.spmm_unit_weight_tl_node
      }
      sbus.coupleFrom("voyager_spmm_unit_weight_scale") {
        _ := sbus { TLBuffer() } := voyager.spmm_unit_weight_scale_tl_node
      }
    }

    voyager.dma_tl_nodes.zipWithIndex.foreach { case (node, q) =>
      sbus.coupleFrom(s"voyager_dma_$q") { _ := sbus { TLBuffer() } := node }
    }

    pbus.coupleTo("voyager") {
      voyager.voyager_node := TLFragmenter(pbus.beatBytes, pbus.blockBytes) := _
    }

    // connect voyager interrupt node to ibus
    ibus.fromSync := voyager.intNode
  }
}

class WithVoyager(
  datatype:                  String,
  icDimension:               Int,
  ocDimension:               Int,
  inputBufferSize:           Int,
  weightBufferSize:          Int,
  accumBufferSize:           Int,
  clockPeriod:               Double,
  technology:                String,
  scaleDatatype:             String  = "",
  doubleBufferedAccumBuffer: Boolean = false,
  supportMVM:                Boolean = false,
  supportSpmm:               Boolean = false,
  mvUnitWidth:               Option[Int] = None)
    extends Config((site, here, up) => {
      case VoyagerKey => Some(
          VoyagerParams(
            datatype                  = datatype,
            icDimension               = icDimension,
            ocDimension               = ocDimension,
            inputBufferSize           = inputBufferSize,
            weightBufferSize          = weightBufferSize,
            accumBufferSize           = accumBufferSize,
            clockPeriod               = clockPeriod,
            technology                = technology,
            scaleDatatype             = scaleDatatype,
            doubleBufferedAccumBuffer = doubleBufferedAccumBuffer,
            supportMVM                = supportMVM,
            supportSpmm               = supportSpmm,
            mvUnitWidth               = mvUnitWidth,
          )
        )
    })
