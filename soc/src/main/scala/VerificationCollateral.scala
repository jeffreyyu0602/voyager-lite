package voyager

import chisel3._
import chisel3.util._

class VoyagerVerification extends BlackBox with HasBlackBoxPath {
  val io = IO(new Bundle {
    val clock = Input(Clock())
    val reset = Input(Reset())
  })

  val chipyardDir       = System.getProperty("user.dir")
  val voyagerWrapperDir = s"$chipyardDir/generators/voyager/soc/src/main/resources"
  val voyagerDir        = s"$chipyardDir/generators/voyager"

  // SoC Verification Collateral
  addPath(s"$voyagerWrapperDir/vsrc/VerificationCollateral.v")
  addPath(s"$voyagerWrapperDir/csrc/VerificationCollateral.cc")
  addPath(s"$voyagerWrapperDir/csrc/SoCSimulation.cc")
  addPath(s"$voyagerWrapperDir/csrc/SoCSimulation.h")
  addPath(s"$voyagerWrapperDir/csrc/SoCMemory.cc")
  addPath(s"$voyagerWrapperDir/csrc/SoCMemory.h")

  // Accelerator Verification Collateral (new-IR toolchain)
  addPath(s"$voyagerDir/test/gold/GoldModel.cc")
  addPath(s"$voyagerDir/test/network/ArrayMemory.cc")
  addPath(s"$voyagerDir/test/network/Tiling.cc")
  addPath(s"$voyagerDir/test/network/Model.cc")
  addPath(s"$voyagerDir/test/network/GraphUtils.cc")
  addPath(s"$voyagerDir/test/network/Interpreter.cc")
  addPath(s"$voyagerDir/test/checker/Checker.cc")
  addPath(s"$voyagerDir/test/network/Simulation.cc")
  addPath(s"$voyagerDir/build/codegen/proto/voyager_ir.pb.cc")
  addPath(s"$voyagerDir/toolchain/MapOperation.cc")
  addPath(s"$voyagerDir/soc/emitter/HostRequests.cc")
}
