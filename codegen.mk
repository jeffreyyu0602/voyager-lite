# Makefile targets for different codegen models/datatypes
# Each target has the format: $(CODEGEN_DIR)/networks/$(NETWORK)/$(DATATYPE)/model.txt

# Set default values if not already defined in the environment.
# SCRATCHPAD_SIZE is the whole physical L2 SRAM; COMMON_FLAGS always passes
# --double_buffered_l2, so a ping-ponged buffer gets half of it per slot.
SCRATCHPAD_SIZE ?= 2097152
SCRATCHPAD_OFFSET ?= 0
NUM_BANKS ?= 16

# One input beat in bytes: the PE array's column count times the activation
# element size. The store bus writes that word whole, so the memory planner
# aligns allocations to it and reserves the tail a sub-word beat overshoots by.
BYTE_BANK_WIDTH := $(OC_DIMENSION)
NIBBLE_BANK_WIDTH := $(shell expr $(OC_DIMENSION) / 2)

E4M3_FLAGS := --activation fp8_e4m3 --weight fp8_e4m3 --bf16 --scratchpad_word_bytes $(BYTE_BANK_WIDTH)
P8_1_FLAGS := --activation posit8_1 --weight posit8_1 --bf16 --scratchpad_word_bytes $(BYTE_BANK_WIDTH)
# Samples the per-tensor INT8 scales are calibrated on; a handful leaves the
# scales noisy.
INT8_CALIBRATION_STEPS := 32
INT8_FLAGS := --activation int8,qs=per_tensor_symmetric --weight int8,qs=per_tensor_symmetric --bias int24 --bf16 --calibration_steps $(INT8_CALIBRATION_STEPS) --scratchpad_word_bytes $(BYTE_BANK_WIDTH)
INT8_32_FLAGS := --activation int8,qs=per_tensor_symmetric --weight int8,qs=per_tensor_symmetric --bias int32 --bf16 --calibration_steps $(INT8_CALIBRATION_STEPS) --scratchpad_word_bytes $(BYTE_BANK_WIDTH)
BLOCK_SIZE := $(shell [ $(IC_DIMENSION) -gt $(OC_DIMENSION) ] && echo $(IC_DIMENSION) || echo $(OC_DIMENSION))
MXINT8_FLAGS := --activation int8,qs=microscaling,bs=$(BLOCK_SIZE),pot=1 --weight int8,qs=microscaling,bs=$(BLOCK_SIZE),pot=1 --bf16 --scratchpad_word_bytes $(BYTE_BANK_WIDTH)
MXNF4_FLAGS := --activation lut4_to_int6,qs=microscaling,bs=$(BLOCK_SIZE),scale=fp8_e5m3 --weight lut4_to_int6,qs=microscaling,bs=$(BLOCK_SIZE),scale=fp8_e5m3 --bf16 --residual fp8_e4m3 --quantize_fc --scratchpad_word_bytes $(NIBBLE_BANK_WIDTH)
COMMON_FLAGS := --remove_fp32_casts --tensor_layout nhwc --gemm_layout "xc,ck->xk" --pe_array_size $(IC_DIMENSION),$(OC_DIMENSION) --dump_tensors --double_buffered_l2 --scratchpad_size $(SCRATCHPAD_SIZE) --scratchpad_offset $(SCRATCHPAD_OFFSET) --scratchpad_banks $(NUM_BANKS) --input_buffer_size $(INPUT_BUFFER_SIZE) --weight_buffer_size $(WEIGHT_BUFFER_SIZE) --accum_buffer_size $(ACCUM_BUFFER_SIZE)
EXTRA_COMPILER_FLAGS ?=

CONTEXT ?= 1024
LLM_FLAGS := --context_length $(CONTEXT) --num_hidden_layers 1
# Only MXNF4's vector unit reads an int1 mask; MXINT8 keeps it bf16.
LLM_MXNF4_FLAGS := --quantize_attention_mask --bool_mask

ifneq ($(filter true 1,$(DOUBLE_BUFFERED_ACCUM_BUFFER)),)
COMMON_FLAGS += --double_buffered_accum_buffer
endif

# FREQUENCY (GHz) is the clock the compiler's cost model assumes. It defaults
# to 1 / CLOCK_PERIOD (ns); set it when the chip runs at a different clock
# than the one the RTL is synthesized for.
ifdef CLOCK_PERIOD
FREQUENCY ?= $(shell awk 'BEGIN { print 1.0 / $(CLOCK_PERIOD) }')
endif
ifdef FREQUENCY
COMMON_FLAGS += --frequency $(FREQUENCY)
endif

ifeq ($(CONV2D_IM2COL),1)
COMMON_FLAGS += --conv2d_im2col
endif

################################################################################
# Networks
#
# One rule compiles any network. NETWORK is the name a compiled program is
# filed and tested under, networks/<NETWORK>/<DATATYPE>/; these pick what is
# compiled into it:
#   MODE         compile_model.py's model, e.g. clm (default: NETWORK)
#   STAGE        the one stage of a causal LM (MODE=clm) to compile:
#                prefill or decode
#   CHECKPOINT   --model_name_or_path (default: the compiler's own)
#   QCONFIG      --qconfig (default: none)
#   KV_CACHE     kivi: a decode's KV cache in KIVI's 2-bit format
#   DECODE_TOKENS  positions a decode step writes at once; above one, a
#                speculative-decoding verification step (default: one)
#   WEIGHT_LAYOUT  how every program stores the linear weights, kc [out, in]
#                or ck [in, out] (--weight_layout; default: the compiler's)
# A network's settings live in ci/networks/<network>.env, KEY=VALUE per line,
# and the drivers pass them on make's command line. For example:
#   make network-proto NETWORK=llama2_7b_decode_kivi MODE=clm STAGE=decode \
#     KV_CACHE=kivi CHECKPOINT=meta-llama/Llama-2-7b-hf
################################################################################
MODE ?= $(NETWORK)
STAGE ?=
CHECKPOINT ?=
QCONFIG ?=
KV_CACHE ?=
DECODE_TOKENS ?=
# A KIVI cache's full-precision residual chunk (the compiler requires it).
RESIDUAL_LENGTH ?= 128
WEIGHT_LAYOUT ?=

ifeq ($(MODE),clm)
ifeq ($(STAGE),)
$(error MODE=clm compiles one STAGE (prefill or decode): set STAGE)
endif
endif

NETWORK_FLAGS := \
	$(if $(filter clm,$(MODE)),--stage $(STAGE) $(LLM_FLAGS)) \
	$(if $(and $(filter clm,$(MODE)),$(filter MXNF4,$(DATATYPE))),$(LLM_MXNF4_FLAGS)) \
	$(if $(CHECKPOINT),--model_name_or_path $(CHECKPOINT)) \
	$(if $(QCONFIG),--qconfig $(QCONFIG)) \
	$(if $(KV_CACHE),--kv_cache $(KV_CACHE) --residual_length $(RESIDUAL_LENGTH)) \
	$(if $(DECODE_TOKENS),--decode_tokens $(DECODE_TOKENS)) \
	$(if $(WEIGHT_LAYOUT),--weight_layout $(WEIGHT_LAYOUT))

# A folder remembers what it was compiled as, so a NETWORK name reused with
# other settings stops here instead of silently reusing the old compile.
NETWORK_ARGS := $(strip $(MODE) $(NETWORK_FLAGS))
NETWORK_RECORD := $(CODEGEN_DIR)/networks/$(NETWORK)/$(DATATYPE)/network_args.txt
ifeq ($(filter print-compile-args,$(MAKECMDGOALS)),)
ifneq ($(wildcard $(NETWORK_RECORD)),)
ifneq ($(strip $(shell cat $(NETWORK_RECORD))),$(NETWORK_ARGS))
$(error $(NETWORK_RECORD) was compiled as "$(strip $(shell cat $(NETWORK_RECORD)))", not "$(NETWORK_ARGS)": use another NETWORK name or move that folder aside)
endif
endif
endif

# The compile_model.py arguments the rule below passes, less the output
# folder, for a driver that runs the compile itself (the accuracy flow).
.PHONY: print-compile-args
print-compile-args:
	@echo '$(MODE) $($(DATATYPE)_FLAGS) $(EXTRA_COMPILER_FLAGS) $(COMMON_FLAGS) $(NETWORK_FLAGS)'

$(CODEGEN_DIR)/networks/%/model.txt:
	mkdir -p $(dir $@)
	python voyager-compiler/scripts/compile_model.py $(MODE) $($(notdir $(patsubst %/,%,$(dir $@)))_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) $(NETWORK_FLAGS) > $(dir $@)codegen.log 2>&1
	echo '$(NETWORK_ARGS)' > $(dir $@)network_args.txt

################################################################################
# Layer Tests
################################################################################
build/codegen/networks/layertest/CFLOAT/model.txt:
	mkdir -p $(dir $@)
	python voyager-compiler/scripts/compile_model.py layertest --model_output_dir $(dir $@) > $(dir $@)/codegen.log 2>&1
