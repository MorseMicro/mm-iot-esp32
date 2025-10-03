#
# Copyright 2022-2023 Morse Micro
#
# SPDX-License-Identifier: Apache-2.0
#

# By default the Morse firmware is converted to MBIN and then linked into the
# application.  On platforms where the firmware is stored externally,
# MM_FIRMWARE_EXTERNAL_START_ADDR is set to the external storage start address
# and so the firmware is not linked into the application.  However, the MBIN
# file should still be generated for manually writing to the external storage.
FW_MBIN  ?= $(MMIOT_ROOT)/morsefirmware/mm6108.mbin
ifeq ($(MM_FIRMWARE_EXTERNAL_START_ADDR),)
FW_OBJ  := $(patsubst %.mbin,$(BUILD_DIR)/%.mbin.o,$(notdir $(FW_MBIN)))
OBJS += $(FW_OBJ)
endif

#
# If a BCF file is specified (see BCF_MBIN_REL or BCF_MBIN below), then it will be linked into
# the MCU application, otherwise the the HAL will attempt to retrieve it from the
# config store (see mmhal.c).
#
# BCF_MBIN_REL: Path to BCF file relative to the morsefirmware directory
# BCF_MBIN:     Path to BCF file relative to Makefile directory (takes precedence over BCF_MBIN_REL)
#
ifneq ($(BCF_MBIN_REL),)
BCF_MBIN ?= $(MMIOT_ROOT)/morsefirmware/$(BCF_MBIN_REL)
endif

ifneq ($(BCF_MBIN),)
$(info Use BCF: $(BCF_MBIN))
BCF_OBJ := $(patsubst %.mbin,$(BUILD_DIR)/%.mbin.o,$(notdir $(BCF_MBIN)))
OBJS += $(BCF_OBJ)
BUILD_DEFINES += INCLUDE_BCF_FILE_IN_APPLICATION
endif

#
# Given a filename, get the name of the _start symbol
# (e.g.,  _binary____morsespi_bcf_default_bin_start)
#
# Warning: filename must not contain a character other than alphanumeric, underscore, dash, or dot.
#
start_symbol = _binary_$(subst -,_,$(subst /,_,$(subst .,_,$(1))))_start

# Given a filename, get the name of the _end symbol
# (See start_symbol above for more info)
end_symbol = _binary_$(subst -,_,$(subst /,_,$(subst .,_,$(1))))_end

$(FW_OBJ): $(FW_MBIN)
	@echo "Copying $<"
	@mkdir -p $(dir $@)
	$(QUIET)$(OBJCOPY) -I binary -O $(BFDNAME) -B $(ARCH) $< $@                 \
		--redefine-sym $(call start_symbol,$<)=firmware_binary_start              \
		--redefine-sym $(call end_symbol,$<)=firmware_binary_end                  \
		--rename-section .data=.rodata._fw_mbin,contents,alloc,load,readonly,data \
		--set-section-alignment .data=4

$(BCF_OBJ): $(BCF_MBIN)
	@echo "Copying $<"
	@mkdir -p $(dir $@)
	$(QUIET)$(OBJCOPY) -I binary -O $(BFDNAME) -B $(ARCH) $< $@        \
		--redefine-sym $(call start_symbol,$<)=bcf_binary_start          \
		--redefine-sym $(call end_symbol,$<)=bcf_binary_end              \
		--rename-section .data=.rodata,contents,alloc,load,readonly,data \
		--set-section-alignment .data=4
