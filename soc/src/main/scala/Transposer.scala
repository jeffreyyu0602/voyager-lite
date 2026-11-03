// Copyright (c) 2018-2019, The Regents of the University of California
// (Regents).  All Rights Reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Neither the name of the Regents nor the
//    names of its contributors may be used to endorse or promote products
//    derived from this software without specific prior written permission.
//
// IN NO EVENT SHALL REGENTS BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT,
// SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST PROFITS, ARISING
// OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN IF REGENTS HAS
// BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// REGENTS SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE. THE SOFTWARE AND ACCOMPANYING DOCUMENTATION, IF ANY, PROVIDED
// HEREUNDER IS PROVIDED "AS IS". REGENTS HAS NO OBLIGATION TO PROVIDE
// MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.

// Gemmini's transposer (gemmini/Transposer.scala, gemmini/Util.scala), copied
// verbatim for the DMA's transpose buffer.
package voyager

import chisel3._
import chisel3.util._
import Util._

private object Util {
  def wrappingAdd(u: UInt, n: UInt, max_plus_one: Int): UInt = {
    val max = max_plus_one - 1
    if (max == 0) {
      0.U
    } else {
      assert(n <= max.U, "cannot wrapAdd when n is larger than max")
      Mux(u >= max.U - n + 1.U && n =/= 0.U, n - (max.U - u) - 1.U, u + n)
    }
  }
}

trait Transposer[T <: Data] extends Module {
  def dim: Int
  def dataType: T
  val io = IO(new Bundle {
    val inRow = Flipped(Decoupled(Vec(dim, dataType)))
    val outCol = Decoupled(Vec(dim, dataType))
  })
}

class AlwaysOutTransposer[T <: Data](val dim: Int, val dataType: T) extends Transposer[T] {
  require(isPow2(dim))

  val LEFT_DIR = 0.U(1.W)
  val UP_DIR = 1.U(1.W)

  class PE extends Module {
    val io = IO(new Bundle {
      val inR = Input(dataType)
      val inD = Input(dataType)
      val outL = Output(dataType)
      val outU = Output(dataType)
      val dir = Input(UInt(1.W))
      val en = Input(Bool())
    })

    val reg = RegEnable(Mux(io.dir === LEFT_DIR, io.inR, io.inD), io.en)

    io.outU := reg
    io.outL := reg
  }

  val pes = Seq.fill(dim,dim)(Module(new PE))
  val counter = RegInit(0.U((log2Ceil(dim) max 1).W)) // TODO replace this with a standard Chisel counter
  val dir = RegInit(LEFT_DIR)

  // Wire up horizontal signals
  for (row <- 0 until dim; col <- 0 until dim) {
    val right_in = if (col == dim-1) io.inRow.bits(row) else pes(row)(col+1).io.outL
    pes(row)(col).io.inR := right_in
  }

  // Wire up vertical signals
  for (row <- 0 until dim; col <- 0 until dim) {
    val down_in = if (row == dim-1) io.inRow.bits(col) else pes(row+1)(col).io.outU
    pes(row)(col).io.inD := down_in
  }

  // Wire up global signals
  pes.flatten.foreach(_.io.dir := dir)
  pes.flatten.foreach(_.io.en := io.inRow.fire)

  io.outCol.valid := true.B
  io.inRow.ready := true.B

  val left_out = VecInit(pes.transpose.head.map(_.io.outL))
  val up_out = VecInit(pes.head.map(_.io.outU))

  io.outCol.bits := Mux(dir === LEFT_DIR, left_out, up_out)

  when (io.inRow.fire) {
    counter := wrappingAdd(counter, 1.U, dim)
  }

  when (counter === (dim-1).U && io.inRow.fire) {
    dir := ~dir
  }
}
