/*
 * Copyright 2024 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MMIPERF_FREERTOSPLUSTCP_H__
#define MMIPERF_FREERTOSPLUSTCP_H__

#include "../common/mmiperf_private.h"

#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "FreeRTOS_IPv4_Sockets.h"
#include "FreeRTOS_IPv6_Sockets.h"

/**
 * Initialise the given iperf state structure at the start of an iperf session.
 *
 * @param base        The data sturcture to initialize.
 * @param local_addr  The local socket address that is used for the session (includes port).
 * @param remote_addr The remote socket address that is used for the session (includes port).
 */
void iperf_freertosplustcp_session_start_common(struct mmiperf_state *base,
                                                const struct freertos_sockaddr *local_addr,
                                                const struct freertos_sockaddr *remote_addr);

#endif
