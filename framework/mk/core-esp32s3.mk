#
# Copyright 2023 Morse Micro
#
# SPDX-License-Identifier: Apache-2.0
#

# Configure the toolchain
TOOLCHAIN_VERSION ?= 12.2.0_20230208
TOOLCHAIN_DIR := /opt/morse/xtensa-esp32s3-elf
TOOLCHAIN_BASE := $(TOOLCHAIN_DIR)/bin/xtensa-esp32s3-elf-

# Compiler
CC := "$(TOOLCHAIN_BASE)gcc"
CXX := "$(TOOLCHAIN_BASE)g++"
AS := $(CC) -x assembler-with-cpp
OBJCOPY := "$(TOOLCHAIN_BASE)objcopy"
AR := "$(TOOLCHAIN_BASE)ar"

ARCH := xtensa
BFDNAME := elf32-xtensa-le

# Include directories
TOOLCHAIN_INCLUDES := "$(TOOLCHAIN_DIR)/include"
INCLUDES += $(TOOLCHAIN_INCLUDES)

# Compiler flags
CFLAGS := -Wall \
		  -Wextra \
		  -Werror \
		  -mlongcalls \
		  -Os \
		  -Wno-cast-qual \
		  -Wno-unused-parameter \
		  -Wno-sign-compare \
		  -Wno-incompatible-pointer-types \
		  -Wno-error=deprecated-declarations \
		  -Wno-implicit-function-declaration \
		  -Wno-return-type -Wno-enum-conversion \
		  -Wno-int-conversion \
		  -Wno-unused-variable \
		  -Wno-implicit-fallthrough \
		  -Wno-old-style-declaration \
		  -mtext-section-literals \

# Linker flags
LINKFLAGS += -nostdlib -Wl,--gc-sections
