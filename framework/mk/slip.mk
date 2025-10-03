#
# Copyright 2023 Morse Micro
#
# SPDX-License-Identifier: Apache-2.0
#

SLIP_DIR = src/slip

SLIP_SRCS_C += slip.c
SLIP_SRCS_H += slip.h

MMIOT_SRCS_C += $(addprefix $(SLIP_DIR)/,$(SLIP_SRCS_C))
MMIOT_SRCS_H += $(addprefix $(SLIP_DIR)/,$(SLIP_SRCS_H))

MMIOT_INCLUDES += $(SLIP_DIR)
