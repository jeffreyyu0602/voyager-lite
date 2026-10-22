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

E4M3_FLAGS := --activation fp8_e4m3 --weight fp8_e4m3 --bf16 --bank_width $(BYTE_BANK_WIDTH)
P8_1_FLAGS := --activation posit8_1 --weight posit8_1 --bf16 --bank_width $(BYTE_BANK_WIDTH)
INT8_FLAGS := --activation int8,qs=per_tensor_symmetric --weight int8,qs=per_tensor_symmetric --bias int24 --bf16 --calibration_steps 3 --bank_width $(BYTE_BANK_WIDTH)
INT8_32_FLAGS := --activation int8,qs=per_tensor_symmetric --weight int8,qs=per_tensor_symmetric --bias int32 --bf16 --calibration_steps 3 --bank_width $(BYTE_BANK_WIDTH)
BLOCK_SIZE := $(shell [ $(IC_DIMENSION) -gt $(OC_DIMENSION) ] && echo $(IC_DIMENSION) || echo $(OC_DIMENSION))
MXINT8_FLAGS := --activation int8,qs=microscaling,bs=$(BLOCK_SIZE) --weight int8,qs=microscaling,bs=$(BLOCK_SIZE) --force_scale_power_of_two --bf16 --bank_width $(BYTE_BANK_WIDTH)
MXNF4_FLAGS := --activation nf4_6,qs=microscaling,bs=$(BLOCK_SIZE),scale=fp8_e5m3 --weight nf4_6,qs=microscaling,bs=$(BLOCK_SIZE),scale=fp8_e5m3 --bf16 --residual fp8_e4m3 --quantize_fc --bank_width $(NIBBLE_BANK_WIDTH)
COMMON_FLAGS := --layout_policy systolic --pe_array_size $(IC_DIMENSION),$(OC_DIMENSION) --dump_tensors --double_buffered_l2 --scratchpad_size $(SCRATCHPAD_SIZE) --scratchpad_offset $(SCRATCHPAD_OFFSET) --num_banks $(NUM_BANKS) --input_buffer_size $(INPUT_BUFFER_SIZE) --weight_buffer_size $(WEIGHT_BUFFER_SIZE) --accum_buffer_size $(ACCUM_BUFFER_SIZE)
EXTRA_COMPILER_FLAGS ?=

CONTEXT ?= 1024
LLM_FLAGS := --context_length $(CONTEXT) --num_hidden_layers 1 --quantize_attention_mask

ifneq ($(filter true 1,$(DOUBLE_BUFFERED_ACCUM_BUFFER)),)
COMMON_FLAGS += --double_buffered_accum_buffer
endif

ifdef CLOCK_PERIOD
# CLOCK_PERIOD is in ns; the compiler's cost model takes frequency in GHz.
COMMON_FLAGS += --frequency $(shell awk 'BEGIN { print 1.0 / $(CLOCK_PERIOD) }')
endif

ifeq ($(CONV2D_IM2COL),1)
COMMON_FLAGS += --conv2d_im2col
endif

################################################################################
# Networks
#
# One rule compiles any network. NETWORK names the output folder; these pick
# what is compiled into it:
#   MODE         test_codegen.py's model, e.g. llama_prefill (default: NETWORK)
#   CHECKPOINT   --model_name_or_path (default: the compiler's own)
#   QCONFIG      --qconfig (default: none)
#   SPEC_LENGTH  draft tokens of a llama_verify step (default: 14)
# For example:
#   make network-proto NETWORK=llama2_7b_decode_kivi MODE=llama_decode_kivi \
#     CHECKPOINT=meta-llama/Llama-2-7b-hf QCONFIG=mxnf4_int6
################################################################################
MODE ?= $(NETWORK)
CHECKPOINT ?=
QCONFIG ?=
SPEC_LENGTH ?= 14

NETWORK_FLAGS := \
	$(if $(filter llama_%,$(MODE)),$(LLM_FLAGS)) \
	$(if $(CHECKPOINT),--model_name_or_path $(CHECKPOINT)) \
	$(if $(QCONFIG),--qconfig $(QCONFIG)) \
	$(if $(filter llama_verify,$(MODE)),--spec_length $(SPEC_LENGTH))

# A folder remembers what it was compiled as, so a NETWORK name reused with
# other settings stops here instead of silently reusing the old compile.
NETWORK_ARGS := $(strip $(MODE) $(NETWORK_FLAGS))
NETWORK_RECORD := $(CODEGEN_DIR)/networks/$(NETWORK)/$(DATATYPE)/network_args.txt
ifneq ($(wildcard $(NETWORK_RECORD)),)
ifneq ($(strip $(shell cat $(NETWORK_RECORD))),$(NETWORK_ARGS))
$(error $(NETWORK_RECORD) was compiled as "$(strip $(shell cat $(NETWORK_RECORD)))", not "$(NETWORK_ARGS)": use another NETWORK name or move that folder aside)
endif
endif

$(CODEGEN_DIR)/networks/%/model.txt:
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py $(MODE) $($(notdir $(patsubst %/,%,$(dir $@)))_FLAGS) $(EXTRA_COMPILER_FLAGS) --model_output_dir $(dir $@) $(COMMON_FLAGS) $(NETWORK_FLAGS) > $(dir $@)codegen.log 2>&1
	echo '$(NETWORK_ARGS)' > $(dir $@)network_args.txt

################################################################################
# Gesture
################################################################################
$(CODEGEN_DIR)/networks/gesture/CFLOAT/model.txt:
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py gesture --model_name_or_path models/gesture/model.pth --model_output_dir $(dir $@) > $(dir $@)/codegen.log 2>&1

################################################################################
# Layer Tests
################################################################################
test/compiler/networks/layertest/CFLOAT/model.txt:
	mkdir -p $(dir $@)
	python voyager-compiler/test/test_codegen.py layertest --model_output_dir $(dir $@) > $(dir $@)/codegen.log 2>&1
