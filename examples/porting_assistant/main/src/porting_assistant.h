/*
 * Copyright 2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdlib.h>
#include <stdint.h>

#include "mmosal.h"

#include "config.h"
#include "log.h"

#ifndef PACKED
#define PACKED __attribute__((packed))
#endif

/* Enumeration of result codes that can be returned by a test. */
enum test_result
{
    TEST_NO_RESULT,
    TEST_PASSED,
    TEST_SKIPPED,
    TEST_FAILED,
    TEST_FAILED_NON_CRITICAL,
};

/** Test step descriptor. */
struct test_step
{
    /** Short, user friendly description of the test step. */
    const char *description;
    /** Test step execution function. */
    enum test_result (*exec)(char *log_buf, size_t log_buf_len);
};

#define TEST_STEP(name, description) \
    static enum test_result name##_exec(char *log_buf, size_t log_buf_len); \
    const struct test_step name = { description, name##_exec }; \
    static enum test_result name##_exec(char *log_buf, size_t log_buf_len)

#define TEST_LOG_APPEND(fstr, ...) \
    do { \
        int len = snprintf(log_buf, log_buf_len, fstr, ##__VA_ARGS__);  \
        if (len > 0 && (int)log_buf_len >= len) { \
            log_buf += len; \
            log_buf_len -= len; \
        } \
    } while (0)
