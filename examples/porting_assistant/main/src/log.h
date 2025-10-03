/*
 * Copyright 2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "mmhal.h"
#include "mmosal.h"

#define ANSI_ESC_SGR(_n) "\033[" _n "m"
#define ANSI_ESC_RESET      ANSI_ESC_SGR("0")
#define ANSI_ESC_BOLD       ANSI_ESC_SGR("1")
#define ANSI_ESC_FAINT      ANSI_ESC_SGR("2")
#define ANSI_ESC_RED        ANSI_ESC_SGR("31")
#define ANSI_ESC_GREEN      ANSI_ESC_SGR("32")
#define ANSI_ESC_YELLOW     ANSI_ESC_SGR("33")
#define ANSI_ESC_BLUE       ANSI_ESC_SGR("34")
#define ANSI_ESC_GRAY       ANSI_ESC_SGR("90")

#if LOG_COLOR_ENABLED != 0

#define F_BOLD(_str) ANSI_ESC_BOLD _str ANSI_ESC_RESET
#define F_FAINT(_str) ANSI_ESC_FAINT _str ANSI_ESC_RESET
#define F_RED(_str) ANSI_ESC_RED _str ANSI_ESC_RESET
#define F_BLUE(_str) ANSI_ESC_BLUE _str ANSI_ESC_RESET
#define F_YELLOW(_str) ANSI_ESC_YELLOW _str ANSI_ESC_RESET
#define F_GRAY(_str) ANSI_ESC_GRAY _str ANSI_ESC_RESET
#define F_GREEN(_str) ANSI_ESC_GREEN _str ANSI_ESC_RESET
#else
#define F_BOLD(_str) _str
#define F_FAINT(_str) _str
#define F_RED(_str) _str
#define F_BLUE(_str) _str
#define F_YELLOW(_str)  _str
#define F_GRAY(_str) _str
#define F_GREEN(_str) _str
#endif

#ifndef VSNPRINTF
#define VSNPRINTF vsnprintf
#endif

static inline void log_printf(const char *fstr, ...)
{
    va_list arg;
    va_start(arg, fstr);
    char buf[128];
    int len = VSNPRINTF(buf, sizeof(buf), fstr, arg);
    MMOSAL_ASSERT(len >= 0);
    va_end(arg);
    mmhal_log_write((const uint8_t *)buf, len);
}

#define LOG_PRINTF(...)     log_printf(__VA_ARGS__)
#define LOG_WRITE(_str)     mmhal_log_write((const uint8_t *)_str, strlen(_str))
#define LOG_FLUSH()         mmhal_log_flush()

#ifndef STRINGIFY
#define STRINGIFY(x) #x
#endif
