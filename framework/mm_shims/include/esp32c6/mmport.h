/*
 * Copyright 2025 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#define MMPORT_BREAKPOINT() asm("ebreak")
#define MMPORT_GET_LR()     (__builtin_return_address(0))
#define MMPORT_GET_PC(_a)   asm volatile ("auipc %0, 0" : "=r" (_a))
#define MMPORT_MEM_SYNC()   __sync_synchronize()
