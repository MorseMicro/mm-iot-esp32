#
# Copyright 2025 Morse Micro
#
# SPDX-License-Identifier: Apache-2.0
#

# Configure the toolchain
TOOLCHAIN_DIR := /opt/morse/riscv32-esp-elf
TOOLCHAIN_BASE := $(TOOLCHAIN_DIR)/bin/riscv32-esp-elf-

CC := $(TOOLCHAIN_BASE)gcc
CXX := "$(TOOLCHAIN_BASE)g++"
OBJCOPY := $(TOOLCHAIN_BASE)objcopy
AR := $(TOOLCHAIN_BASE)ar

CFLAGS += -march=rv32imc_zicsr_zifencei
CFLAGS += -mabi=ilp32

# Include directories
TOOLCHAIN_INCLUDES := "$(TOOLCHAIN_DIR)/include"
INCLUDES += $(TOOLCHAIN_INCLUDES)

ARCH := riscv
BFDNAME := elf32-littleriscv

# Compiler flags
CFLAGS += -Wall \
		  -Wextra \
		  -Werror \
		  -Os \
		  -Wno-cast-qual \
		  -Wno-unused-parameter \
		  -Wno-sign-compare \
		  -Wno-incompatible-pointer-types \
		  -Wno-error=deprecated-declarations \
		  -Wno-implicit-function-declaration \
		  -Wno-return-type \
		  -Wno-enum-conversion \
		  -Wno-int-conversion \
		  -Wno-unused-variable \
		  -Wno-implicit-fallthrough \
		  -Wno-old-style-declaration

# Linker flags
LINKFLAGS += -nostdlib -Wl,--gc-sections
