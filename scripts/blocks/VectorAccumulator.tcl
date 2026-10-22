set block "VectorAccumulator"
set full_block_name "VectorAccumulator<$VECTOR_DATATYPE, $ACCUMULATOR_WIDTH>"

proc pre_architect {} {
  global full_block_name ACCUMULATOR_WIDTH
  set vector_accumulator_stripped [string map {" " ""} $full_block_name]

  # Wide feedback state crosses Catapult's automatic SRAM-mapping threshold.
  # Preserve the parallel accesses required by the accumulation pipeline.
  if {$ACCUMULATOR_WIDTH >= 256} {
    directive set \
      /$vector_accumulator_stripped/run_accumulation/while:if:for:acc_old.value.float_val.d:rsc \
      -MAP_TO_MODULE {[Register]}
  }
}
