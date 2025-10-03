/*
 * Copyright (c) 2014 Simon Goldschmidt
 * Copyright 2021-2024 Morse Micro
 * All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Authors: Simon Goldschmidt, Morse Micro
 */

#pragma once

#include "mmiperf.h"
#include "mmosal.h"

/* File internal memory allocation (struct iperf_*): this defaults to
 *  the heap */
#ifndef IPERF_ALLOC
#define IPERF_ALLOC(type) mmosal_malloc_(sizeof(type))
#define IPERF_FREE(type, item) mmosal_free(item)
#endif

/** Specify the idle timeout (in seconds) after that the test fails */
#ifndef IPERF_TCP_MAX_IDLE_S
#define IPERF_TCP_MAX_IDLE_S 140U
#endif
#if IPERF_TCP_MAX_IDLE_SEC > 255
#error IPERF_TCP_MAX_IDLE_SEC must fit into an u8_t
#endif

/** Time after which we consider a server session to have timed out. */
#ifndef IPERF_UDP_SERVER_SESSION_TIMEOUT_MS
#define IPERF_UDP_SERVER_SESSION_TIMEOUT_MS       (60000)
#endif

/** Max number of times for UDP client to transmit final packet if it does not receive a report. */
#ifndef IPERF_UDP_CLIENT_REPORT_RETRIES
#define IPERF_UDP_CLIENT_REPORT_RETRIES           (3)
#endif

/** Interval between retransmits of UDP client's final packet if it does not receive a report. */
#ifndef IPERF_UDP_CLIENT_REPORT_TIMEOUT_MS
#define IPERF_UDP_CLIENT_REPORT_TIMEOUT_MS        (1000)
#endif

/** The maximum number of conecutive transmit failurse we tolerate before giving up. */
#ifndef IPERF_UDP_CLIENT_MAX_CONSEC_FAILURES
#define IPERF_UDP_CLIENT_MAX_CONSEC_FAILURES      (60)
#endif

/** The wait time in milliseconds between retries before giving up. */
#ifndef IPERF_UDP_CLIENT_RETRY_WAIT_TIME_MS
#define IPERF_UDP_CLIENT_RETRY_WAIT_TIME_MS       (1000)
#endif

/** Beginning of the local port range for the UDP client to use. */
#ifndef IPERF_UDP_CLIENT_LOCAL_PORT_RANGE_BASE
#define IPERF_UDP_CLIENT_LOCAL_PORT_RANGE_BASE    (5010)
#endif

/** Size of the local port range for the UDP client to use (MUST be a power of 2). */
#ifndef IPERF_UDP_CLIENT_LOCAL_PORT_RANGE_SIZE
#define IPERF_UDP_CLIENT_LOCAL_PORT_RANGE_SIZE    (16)
#endif

/** This is the Iperf settings struct sent from the client */
struct iperf_settings
{
#define IPERF_FLAGS_ANSWER_TEST 0x80000000
#define IPERF_FLAGS_ANSWER_NOW 0x00000001
    uint32_t flags;
    uint32_t num_threads; /* unused for now */
    uint32_t remote_port;
    uint32_t buffer_len; /* unused for now */
    uint32_t win_band;   /* TCP window / UDP rate: unused for now */
    uint32_t amount;     /* pos. value: bytes?; neg. values: time (unit is 10ms: 1/100 second) */
};

#define IPERF_HEADER_VERSION1 0x80000000

struct iperf_udp_header
{
    uint32_t id_lo;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t id_hi; /* Note: not present in Iperf 2.0.9 */
};

struct iperf_udp_server_report
{
    int32_t flags;
    int32_t total_len1;
    int32_t total_len2;
    int32_t stop_sec;
    int32_t stop_usec;
    int32_t error_cnt;
    int32_t outorder_cnt;
    int32_t datagrams;
    int32_t jitter1;
    int32_t jitter2;
    int32_t minTransit1;
    int32_t minTransit2;
    int32_t maxTransit1;
    int32_t maxTransit2;
    int32_t sumTransit1;
    int32_t sumTransit2;
    int32_t meanTransit1;
    int32_t meanTransit2;
    int32_t m2Transit1;
    int32_t m2Transit2;
    int32_t vdTransit1;
    int32_t vdTransit2;
    int32_t cntTransit;
    int32_t IPGcnt;
    int32_t IPGsum;
};

struct mmiperf_state
{
    /* Allow these state structures to be collected as a linked list. */
    struct mmiperf_state *next;
    /* Iperf protocol: 1=tcp, 0=udp. */
    uint8_t tcp;
    /* Iperf type: 1=server, 0=client. */
    uint8_t server;
    /** The time at which this iperf session was startd. */
    uint32_t time_started_ms;
    /** The last time at which we received a packet. (UDP only) */
    uint32_t last_rx_time_ms;
    /** Data is collected within this report structure for easy access via
     *  @ref mmiperf_get_interim_report() */
    struct mmiperf_report report;
    /** Calback function to invoke on iperf completion. */
    mmiperf_report_fn report_fn;
    /** Argument to pass to callback function. */
    void *report_arg;
};

/** Add an iperf session to the 'active' list */
void iperf_list_add(struct mmiperf_state *item);

/** Remove an iperf session from the 'active' list */
void iperf_list_remove(struct mmiperf_state *item);

/** Update the report data for the given iperf session based on the given time. */
void iperf_finalize_report_and_invoke_callback(struct mmiperf_state *state, uint32_t duration_ms,
                                               enum mmiperf_report_type report_type);

/**
 * Populate an iperf UDP server report to send to a client.
 *
 * @param base_state Iperf session state data structure.
 * @param report     The report structure to populate.
 */
void iperf_populate_udp_server_report(struct mmiperf_state *base_state,
                                      struct iperf_udp_server_report *report);

/**
 * Parse an iperf UDP server report received from a server.
 *
 * @param base_state Iperf session state data structure. This will be updated based on the
 *                      received report.
 * @param hdr        The iperf UDP header in the iperf frame.
 * @param report     The iperf UDP server report in the iperf frame.
 * @param version    The iperf version.
 */
bool iperf_parse_udp_server_report(struct mmiperf_state *base_state,
                                   const struct iperf_udp_header *hdr,
                                   const struct iperf_udp_server_report *report,
                                   enum iperf_version version);

/** Find a given item in the list and return a pointer to it if found, else return NULL. */
struct mmiperf_state *iperf_list_find(struct mmiperf_state *item);

/** Look up an iperf session in the list based on a given handle. */
struct mmiperf_state *iperf_list_get(mmiperf_handle_t handle);

/**
 * Get a pointer to iperf payload data at the given offset.
 *
 * @param offset Offset into the data.
 *
 * @returns a pointer into the data.
 */
const uint8_t *iperf_get_data(uint32_t offset);
