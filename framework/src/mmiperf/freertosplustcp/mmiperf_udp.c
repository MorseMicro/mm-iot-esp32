/*
 * Copyright 2023-2024 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdatomic.h>

#include "mmiperf.h"
#include "mmipal.h"
#include "mmosal.h"
#include "mmutils.h"
#include "mmiperf_freertosplustcp.h"

#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "FreeRTOS_IPv4_Sockets.h"
#include "FreeRTOS_IPv6_Sockets.h"

#ifndef min
#define min(a, b) ((b) < (a) ? (b) : (a))
#endif

/* State data fo a speicific iperf server session */
struct iperf_server_session_udp
{
    /* A negative value indicates there is no active session (but there may still be state
     * from a previous session) */
    int32_t next_packet_id;

    int32_t error_cnt;
    struct timeval ipg_start;
    struct freertos_sockaddr client_sa;
};

/** Connection handle for a UDP iperf server */
struct iperf_server_state_udp
{
    struct mmiperf_state base;
    struct
    {
        IPv46_Address_t local_addr;
        uint16_t local_port;
        enum iperf_version version;
    } args;
    Socket_t udp_socket;
    struct freertos_sockaddr udp_server_sa;
    struct iperf_server_session_udp session;
    struct mmosal_task *task;
};

struct iperf_client_state_udp
{
    struct mmiperf_state base;

    /* Given configuration */
    struct mmiperf_client_args args;
    IPv46_Address_t server_addr;

    /* State */
    Socket_t udp_socket;
    struct freertos_sockaddr udp_client_sa;
    struct freertos_sockaddr udp_server_sa;
    uint16_t local_port;
    uint32_t check_interval;
    struct mmosal_task *task;
    bool awaiting_report;
    uint8_t *report;
    uint32_t report_len;
    int32_t next_packet_id;

    /* block parameter for bandwdith limit */
    uint32_t block_tx_amount;
};

static bool is_multicast_ip_addr(IPv46_Address_t ip_addr)
{
    if (!ip_addr.xIs_IPv6 && ((ip_addr.xIPAddress.ulIP_IPv4 >> 28) == 14))
    {
        return true;
    }
#if ipconfigUSE_IPv6
    else if (xIPv6_GetIPType(&ip_addr.xIPAddress.xIP_IPv6) == eIPv6_Multicast)
    {
        return true;
    }
#endif
    return false;
}

static bool session_has_timed_out(struct iperf_server_state_udp *server_state)
{
    return mmosal_time_has_passed(
        server_state->base.last_rx_time_ms + IPERF_UDP_SERVER_SESSION_TIMEOUT_MS);
}

static struct iperf_server_session_udp *get_free_session_slot(
    struct iperf_server_state_udp *server_state)
{
    /* For now we only support a single session. */
    struct iperf_server_session_udp *session = &(server_state->session);
    if (session->next_packet_id < 0 || session_has_timed_out(server_state))
    {
        return session;
    }

    return NULL;
}

static struct iperf_server_session_udp *start_session(
    struct iperf_server_state_udp *server_state, const struct freertos_sockaddr *client_sa)
{
    /* For now we only support a single session. */
    struct iperf_server_session_udp *session = get_free_session_slot(server_state);
    if (session == NULL)
    {
        return NULL;
    }

    memset(session, 0, sizeof(*session));
    memcpy(&session->client_sa, client_sa, sizeof(session->client_sa));
    iperf_freertosplustcp_session_start_common(&server_state->base,
                                               &server_state->udp_server_sa,
                                               client_sa);
    return session;
}

static bool sockaddr_match(const struct freertos_sockaddr *a, const struct freertos_sockaddr *b)
{
    if (a->sin_port != b->sin_port || a->sin_len != b->sin_len || a->sin_family != b->sin_family)
    {
        return false;
    }
    if (a->sin_family == FREERTOS_AF_INET)
    {
        return a->sin_address.ulIP_IPv4 == b->sin_address.ulIP_IPv4;
    }
    else
    {
        return !memcmp(&a->sin_address.xIP_IPv6, &b->sin_address.xIP_IPv6,
                       sizeof(a->sin_address.xIP_IPv6));
    }
}

static struct iperf_server_session_udp *get_session(struct iperf_server_state_udp *server_state,
                                                    const struct freertos_sockaddr *rx_client_sa)
{
    /* For now we only support a single session. */
    struct iperf_server_session_udp *session = &(server_state->session);
    if (sockaddr_match(rx_client_sa, &session->client_sa) && !session_has_timed_out(server_state))
    {
        return session;
    }
    else
    {
        return start_session(server_state, rx_client_sa);
    }
}

/**
 * Get the difference in microseconds between to timevals.
 *
 * \note Delta must not exceed the range of an int32.
 */
static int32_t time_delta(const struct timeval *a, const struct timeval *b)
{
    int32_t delta = (a->tv_sec - b->tv_sec) * 1000000;
    delta += (a->tv_usec - b->tv_usec);
    return delta;
}

static void iperf_udp_recv_task(void *arg)
{
    struct iperf_server_state_udp *server_state = (struct iperf_server_state_udp *)arg;

    struct iperf_udp_header *hdr;
    struct iperf_settings *settings;
    struct timeval packet_time;
    int64_t packet_id = 0;
    bool final_packet = false;
    struct iperf_server_session_udp *session = NULL;

    int udp_recv_len = sizeof(*hdr) + 1500;
    int len = 0;

    struct freertos_sockaddr remote_sa;
    uint32_t remote_sa_len = sizeof(remote_sa);

    uint8_t *recv_buff = (uint8_t *)mmosal_malloc(udp_recv_len);
    if (recv_buff == NULL)
    {
        FreeRTOS_debug_printf(("iperf UDP rx task failed to alloc recv_buff\n"));
        return;
    }

    while (1)
    {
        while (!final_packet)
        {
            len = FreeRTOS_recvfrom(server_state->udp_socket, recv_buff, udp_recv_len, 0,
                                    &remote_sa, &remote_sa_len);

            if (len >= (int)(sizeof(*hdr) + sizeof(*settings)))
            {
                hdr = (struct iperf_udp_header *)recv_buff;
                settings = (struct iperf_settings *)(hdr + 1);
                packet_time.tv_sec = FreeRTOS_ntohl(hdr->tv_sec);
                packet_time.tv_usec = FreeRTOS_ntohl(hdr->tv_usec);

                if (server_state->args.version == IPERF_VERSION_2_0_9)
                {
                    packet_id = (int64_t)((int32_t)FreeRTOS_ntohl(hdr->id_lo));
                }
                else
                {
                    packet_id = (int64_t)(((uint64_t)FreeRTOS_ntohl(hdr->id_hi) << 32) |
                                          (uint64_t)FreeRTOS_ntohl(hdr->id_lo));
                }

                /* A negative packet ID indicates that this is the final packet. */
                if (packet_id < 0)
                {
                    final_packet = true;
                    packet_id = -packet_id;
                }

                session = get_session(server_state, &remote_sa);
                if (session == NULL)
                {
                    FreeRTOS_debug_printf(("Another UDP server session already in progress\n"));
                }
                else if (session->next_packet_id >= 0)
                {
                    server_state->base.last_rx_time_ms = mmosal_get_time_ms();
                    server_state->base.report.bytes_transferred += len;
                    server_state->base.report.rx_frames++;
                    server_state->base.report.ipg_count++;
                    server_state->base.report.ipg_sum_ms +=
                        time_delta(&packet_time, &(session->ipg_start));
                    session->ipg_start = packet_time;

                    if (packet_id < session->next_packet_id)
                    {
                        server_state->base.report.out_of_sequence_frames++;
                    }
                    else if (packet_id > session->next_packet_id)
                    {
                        server_state->base.report.error_count +=
                            packet_id - session->next_packet_id;
                    }

                    if (packet_id >= session->next_packet_id)
                    {
                        session->next_packet_id = packet_id + 1;
                    }
                }
            }
        }

        if (final_packet)
        {
            /* Handle the local report if this is the first time receiving the final packet. */
            if (session->next_packet_id >= 0)
            {
                uint32_t duration_ms =
                    server_state->base.last_rx_time_ms - server_state->base.time_started_ms;
                iperf_finalize_report_and_invoke_callback(&server_state->base, duration_ms,
                                                          MMIPERF_UDP_DONE_SERVER);
                session->next_packet_id = -1;
            }

            final_packet = false;

            /* Send server report if not a multicast address */
            if (!is_multicast_ip_addr(server_state->args.local_addr))
            {
                /* We use the UDP header and flags from the pbuf we received. */
                struct iperf_udp_header *report_hdr = (struct iperf_udp_header *)recv_buff;
                struct iperf_udp_server_report *report =
                    (struct iperf_udp_server_report *)(report_hdr + 1);
                uint32_t tx_report_len = (sizeof(*report_hdr) + sizeof(*report));

                iperf_populate_udp_server_report(&server_state->base, report);

                len = FreeRTOS_sendto(server_state->udp_socket, recv_buff, tx_report_len, 0,
                                      &session->client_sa,
                                      sizeof(session->client_sa));
                if (len <= 0)
                {
                    FreeRTOS_debug_printf(("Failed to tx udp server report\n"));
                }
            }
        }
    }

    mmosal_free(recv_buff);
    recv_buff = NULL;
}

mmiperf_handle_t mmiperf_start_udp_server(const struct mmiperf_server_args *args)
{
    int ok = -1;
    struct iperf_server_state_udp *s;
    mmiperf_handle_t result = NULL;
    struct freertos_sockaddr *sa = NULL;
    BaseType_t ret = pdFAIL;

    s = (struct iperf_server_state_udp *)IPERF_ALLOC(struct iperf_server_state_udp);
    if (s == NULL)
    {
        goto exit;
    }
    memset(s, 0, sizeof(*s));
    s->base.tcp = 0;
    s->base.server = 1;
    s->base.report_fn = args->report_fn;
    s->base.report_arg = args->report_arg;
    s->base.report.report_type = MMIPERF_INTERRIM_REPORT;
    memcpy(&(s->args.local_addr), &args->local_addr, sizeof(s->args.local_addr));
    s->args.local_port = args->local_port;
    s->args.version = args->version;
    /* Set next_packet_id to -1 to show that there is no session active. We will start a new
     * session with the first packet we receive from a client. */
    s->session.next_packet_id = -1;

    if (args->local_addr[0] != '\0')
    {
#if ipconfigUSE_IPv4
        s->args.local_addr.xIs_IPv6 = pdFALSE;
        ret = FreeRTOS_inet_pton4(args->local_addr, &s->args.local_addr.xIPAddress.ulIP_IPv4);
#endif
#if ipconfigUSE_IPv6
        if (ret != pdPASS)
        {
            ret = FreeRTOS_inet_pton6(args->local_addr,
                                      &s->args.local_addr.xIPAddress.xIP_IPv6.ucBytes);
            if (ret == pdPASS)
            {
                s->args.local_addr.xIs_IPv6 = pdTRUE;
            }
        }
#else
        MM_UNUSED(ret);
#endif
    }

    s->udp_socket =
        FreeRTOS_socket((s->args.local_addr.xIs_IPv6 ? FREERTOS_AF_INET6 : FREERTOS_AF_INET),
                        FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP);
    if (s->udp_socket == NULL)
    {
        goto exit;
    }

    TickType_t xTimeoutTime = pdMS_TO_TICKS(IPERF_UDP_CLIENT_REPORT_TIMEOUT_MS);
    ok = FreeRTOS_setsockopt(s->udp_socket, 0, FREERTOS_SO_RCVTIMEO, &xTimeoutTime,
                             sizeof(TickType_t));
    if (ok != 0)
    {
        FreeRTOS_debug_printf(("Setting FreeRTOS socket option FREERTOS_SO_RCVTIMEO failed\n"));
    }

    ok = FreeRTOS_setsockopt(s->udp_socket, 0, FREERTOS_SO_SNDTIMEO, &xTimeoutTime,
                             sizeof(TickType_t));
    if (ok != 0)
    {
        FreeRTOS_debug_printf(("Setting FreeRTOS socket option FREERTOS_SO_SNDTIMEO failed\n"));
    }

    memset(&s->udp_server_sa, 0, sizeof(s->udp_server_sa));
    sa = (struct freertos_sockaddr *)&s->udp_server_sa;
    sa->sin_family = (s->args.local_addr.xIs_IPv6 ? FREERTOS_AF_INET6 : FREERTOS_AF_INET);
    sa->sin_port = FreeRTOS_htons(args->local_port);
    if (s->args.local_addr.xIs_IPv6)
    {
        memcpy(sa->sin_address.xIP_IPv6.ucBytes, s->args.local_addr.xIPAddress.xIP_IPv6.ucBytes,
               sizeof(sa->sin_address.xIP_IPv6.ucBytes));
    }
    else
    {
        sa->sin_address.ulIP_IPv4 = s->args.local_addr.xIPAddress.ulIP_IPv4;
    }

    ok = FreeRTOS_bind(s->udp_socket, &s->udp_server_sa, sizeof(s->udp_server_sa));
    if (ok != 0)
    {
        goto exit;
    }

    /* Note: Multicast is not yet supported. */

    iperf_list_add(&s->base);
    s->task = mmosal_task_create(iperf_udp_recv_task, s, MMOSAL_TASK_PRI_LOW,
                                 MMIPERF_STACK_SIZE, "iperf_udp_recv");
    MMOSAL_ASSERT(s->task != NULL);
    result = &(s->base);
    s = NULL;

exit:
    if (s != NULL)
    {
        IPERF_FREE(struct iperf_server_state_udp, s);
    }
    return result;
}

static int iperf_udp_client_send_packet(struct iperf_client_state_udp *client_state,
                                        uint32_t tx_amount, bool final)
{
    struct iperf_udp_header *udp_hdr;
    struct iperf_settings *settings;
    uint32_t hdrs_len = sizeof(*udp_hdr) + sizeof(*settings);
    uint32_t payload_len = 0;
    uint32_t udp_payload_len = 0;
    int ret = 0;
    struct freertos_sockaddr sockaddr_to;

    if (client_state->args.version == IPERF_VERSION_2_0_9)
    {
        hdrs_len = (hdrs_len - sizeof(uint32_t));
    }

    if (tx_amount > hdrs_len)
    {
        payload_len = tx_amount - hdrs_len;
    }

    udp_payload_len = (hdrs_len + payload_len);

    uint8_t *udp_payload = (uint8_t *)mmosal_malloc(udp_payload_len);
    if (udp_payload == NULL)
    {
        FreeRTOS_debug_printf(("iperf UDP tx failed to alloc udp_payload\n"));
        return -1;
    }

    int64_t datagrams_cnt = (int32_t)client_state->base.report.tx_frames;
    if (final)
    {
        datagrams_cnt = -datagrams_cnt;
    }

    udp_hdr = (struct iperf_udp_header *)udp_payload;
    if (client_state->args.version == IPERF_VERSION_2_0_9)
    {
        udp_hdr->id_lo = FreeRTOS_htonl((uint32_t)datagrams_cnt);
    }
    else
    {
        udp_hdr->id_lo = FreeRTOS_htonl((uint32_t)((uint64_t)datagrams_cnt));
        udp_hdr->id_hi = FreeRTOS_htonl((uint32_t)(((uint64_t)datagrams_cnt) >> 32));
    }
    uint32_t now = mmosal_get_time_ms();
    udp_hdr->tv_usec = FreeRTOS_htonl((now % 1000) * 1000);
    udp_hdr->tv_sec = FreeRTOS_htonl(now / 1000);

    settings = (struct iperf_settings *)(udp_hdr + 1);
    memset(settings, 0, sizeof(*settings));

    const uint8_t *payload = iperf_get_data(0);
    if (payload == NULL)
    {
        FreeRTOS_debug_printf(("iperf get payload failed\n"));
        mmosal_free(udp_payload);
        return -1;
    }

    memcpy((udp_payload + hdrs_len), payload, payload_len);

    memset(&sockaddr_to, 0, sizeof(sockaddr_to));
    struct freertos_sockaddr *sa = (struct freertos_sockaddr *)&sockaddr_to;
    sa->sin_port = FreeRTOS_htons(client_state->args.server_port);
    if (client_state->server_addr.xIs_IPv6)
    {
        sa->sin_family = FREERTOS_AF_INET6;
        memcpy(sa->sin_address.xIP_IPv6.ucBytes,
               client_state->server_addr.xIPAddress.xIP_IPv6.ucBytes,
               sizeof(sa->sin_address.xIP_IPv6.ucBytes));
    }
    else
    {
        sa->sin_family = FREERTOS_AF_INET;
        sa->sin_address.ulIP_IPv4 = client_state->server_addr.xIPAddress.ulIP_IPv4;
    }

    ret = FreeRTOS_sendto(client_state->udp_socket, udp_payload, udp_payload_len, 0,
                          &sockaddr_to, sizeof(sockaddr_to));

    mmosal_free(udp_payload);
    if (ret < 0)
    {
        FreeRTOS_debug_printf(("iperf UDP tx failed to send\n"));
        return -1;
    }

    return 0;
}

static void iperf_udp_client_recv(struct iperf_client_state_udp *session)
{
    struct iperf_udp_header *hdr;
    struct iperf_udp_server_report *report;
    int udp_report_len = sizeof(*hdr) + sizeof(*report);
    int len = 0;
    uint32_t server_sa_len = sizeof(session->udp_server_sa);

    uint8_t *recv_buff = (uint8_t *)mmosal_malloc(udp_report_len);
    if (recv_buff == NULL)
    {
        FreeRTOS_debug_printf(("iperf UDP rx failed to alloc recv_buff\n"));
        return;
    }

    len = FreeRTOS_recvfrom(session->udp_socket, recv_buff, udp_report_len, 0,
                            &session->udp_server_sa, &server_sa_len);
    if (len < 0)
    {
        FreeRTOS_debug_printf(("iperf UDP rx failed to recv\n"));
        mmosal_free(recv_buff);
        recv_buff = NULL;
        return;
    }

    session->report = recv_buff;
    session->report_len = len;
    recv_buff = NULL;
}

static void iperf_udp_client_task(void *arg)
{
    struct iperf_client_state_udp *client_state = (struct iperf_client_state_udp *)arg;

    uint32_t end_time = UINT32_MAX;
    uint64_t remaining_amount = UINT64_MAX;

    iperf_freertosplustcp_session_start_common(&client_state->base,
                                               &client_state->udp_client_sa,
                                               &client_state->udp_server_sa);

    /* A negative amount means it is a time (in hundredths of seconds), a postive amount is
     * number of bytes. */
    if (client_state->args.amount < 0)
    {
        end_time = client_state->base.time_started_ms + (-(client_state->args.amount) * 10);
    }
    else
    {
        remaining_amount = client_state->args.amount;
    }

    uint32_t tx_amount = 0;
    bool final = false;
    unsigned failure_cnt = 0;

    /* if no input for bandwidth limit, set bw_limit flag to false */
    bool bw_limit = true;
    if (client_state->args.target_bw == 0)
    {
        bw_limit = false;
    }
    uint32_t block_end_time = mmosal_get_time_ms() + BLOCK_DURATION_MS;
    uint32_t block_remaining_tx_amount = client_state->block_tx_amount;

    while (!final && failure_cnt < IPERF_UDP_CLIENT_MAX_CONSEC_FAILURES)
    {
        /* If this is the last packet then set the counter to negative to inform the other side. */
        if (mmosal_get_time_ms() > end_time ||
            remaining_amount <= (uint64_t)client_state->args.packet_size)
        {
            final = true;
            client_state->awaiting_report = true;
        }
        tx_amount = min(remaining_amount, client_state->args.packet_size);

        /* when bw_limit is set to false, always send packets without check block parameter */
        if (bw_limit && block_end_time < mmosal_get_time_ms())
        {
            block_end_time = mmosal_get_time_ms() + BLOCK_DURATION_MS;
            block_remaining_tx_amount += client_state->block_tx_amount;
        }
        if (!bw_limit || block_remaining_tx_amount >= tx_amount || mmosal_get_time_ms() > end_time)
        {
            int err = iperf_udp_client_send_packet(client_state, tx_amount, final);

            if (err == 0)
            {
                client_state->base.report.bytes_transferred += tx_amount;
                client_state->base.report.tx_frames++;
                remaining_amount -= tx_amount;
                block_remaining_tx_amount -= tx_amount;
                failure_cnt = 0;
            }
            else
            {
                failure_cnt++;
                mmosal_task_sleep(IPERF_UDP_CLIENT_RETRY_WAIT_TIME_MS);
            }
        }
        else
        {
            mmosal_task_sleep(1);
        }
    }
    iperf_udp_client_recv(client_state);

    if (!is_multicast_ip_addr(client_state->server_addr))
    {
        unsigned ii;
        for (ii = 0; ii < IPERF_UDP_CLIENT_REPORT_RETRIES && client_state->report == NULL; ii++)
        {
            iperf_udp_client_send_packet(client_state, tx_amount, true);
            iperf_udp_client_recv(client_state);
        }
    }

    uint32_t final_duration_ms = 0;
    if (client_state->report != NULL)
    {
        struct iperf_udp_header *hdr = (struct iperf_udp_header *)client_state->report;
        struct iperf_udp_server_report *report = (struct iperf_udp_server_report *)(hdr + 1);

        if (client_state->report_len >= sizeof(*hdr) + sizeof(*report))
        {
            iperf_parse_udp_server_report(&client_state->base, hdr, report,
                                          client_state->args.version);
            final_duration_ms = client_state->base.report.duration_ms;
        }

        mmosal_free(client_state->report);
        client_state->report = NULL;
        client_state->report_len = 0;
    }
    else
    {
        final_duration_ms = mmosal_get_time_ms() - client_state->base.time_started_ms;
        if (!is_multicast_ip_addr(client_state->server_addr))
        {
            /* If we receive no response from the server in unicast mode then clear the
             * results in the report so that it is obvious. */
            client_state->base.report.bytes_transferred = 0;
            client_state->base.report.bandwidth_kbitpsec = 0;
        }
    }

    iperf_finalize_report_and_invoke_callback(&client_state->base, final_duration_ms,
                                              MMIPERF_UDP_DONE_CLIENT);

    /* Clean up state and free allocated memory. */
    int ret = FreeRTOS_closesocket(client_state->udp_socket);
    if (ret < 0)
    {
        FreeRTOS_debug_printf(("Socket close failed\n"));
    }
    client_state->udp_socket = 0;
}

mmiperf_handle_t mmiperf_start_udp_client(const struct mmiperf_client_args *args)
{
    struct iperf_client_state_udp *s;
    mmiperf_handle_t result = NULL;
    uint32_t pkt_size = 0;
    int ok = 0;
    struct freertos_sockaddr *sa = NULL;
    static atomic_uint session_counter = 0;
    BaseType_t ret = pdFAIL;

    s = (struct iperf_client_state_udp *)IPERF_ALLOC(struct iperf_client_state_udp);
    if (s == NULL)
    {
        goto exit;
    }

    memset(s, 0, sizeof(*s));
    s->base.tcp = 0;
    s->base.server = 0;
    s->base.report_fn = args->report_fn;
    s->base.report_arg = args->report_arg;
    s->next_packet_id = 0;

    memcpy(&(s->args), args, sizeof(s->args));
#if ipconfigUSE_IPv4
    s->server_addr.xIs_IPv6 = pdFALSE;
    ret = FreeRTOS_inet_pton4(args->server_addr, &s->server_addr.xIPAddress.ulIP_IPv4);
#endif
#if ipconfigUSE_IPv6
    if (ret != pdPASS)
    {
        ret = FreeRTOS_inet_pton6(args->server_addr, &s->server_addr.xIPAddress.xIP_IPv6.ucBytes);
        if (ret == pdPASS)
        {
            s->server_addr.xIs_IPv6 = pdTRUE;
        }
    }
#else
    MM_UNUSED(ret);
#endif

    if (s->args.server_port == 0)
    {
        s->args.server_port = MMIPERF_DEFAULT_PORT;
    }

    if (s->args.packet_size == 0)
    {
        if (s->server_addr.xIs_IPv6)
        {
            s->args.packet_size = MMIPERF_DEFAULT_UDP_PACKET_SIZE_V6;
        }
        else
        {
            s->args.packet_size = MMIPERF_DEFAULT_UDP_PACKET_SIZE_V4;
        }
    }

    if (s->args.amount == 0)
    {
        s->args.amount = MMIPERF_DEFAULT_AMOUNT;
    }

    FreeRTOS_debug_printf(("Starting UDP iperf client to %s:%u, amount %ld\n",
                           s->args.server_addr, s->args.server_port, args->amount));

    /* target_bw (kbps) convert to block_tx_amount (bytes) = bw * (BLOCK_DURATION_MS / 8) */
    s->block_tx_amount = s->args.target_bw * BLOCK_DURATION_MS / 8;
    /* check packet size. If the target_bw is too low, error message is printed. */
    pkt_size = s->args.target_bw * 1000 / 8;
    if (s->args.target_bw != 0 && s->args.packet_size > pkt_size)
    {
        FreeRTOS_debug_printf(("bandwidth limit too low.\n"));
        goto exit;
    }

    /* We use a counter across a range of local ports so we don't use the same port for subsequent
     * iterations. */
    (void)atomic_fetch_add(&session_counter, 1);
    s->local_port = IPERF_UDP_CLIENT_LOCAL_PORT_RANGE_BASE +
        (session_counter & (IPERF_UDP_CLIENT_LOCAL_PORT_RANGE_SIZE - 1));

    s->udp_socket =
        FreeRTOS_socket((s->server_addr.xIs_IPv6 ? FREERTOS_AF_INET6 : FREERTOS_AF_INET),
                        FREERTOS_SOCK_DGRAM, FREERTOS_IPPROTO_UDP);
    if (s->udp_socket == NULL)
    {
        goto exit;
    }

    TickType_t xTimeoutTime = pdMS_TO_TICKS(IPERF_UDP_CLIENT_REPORT_TIMEOUT_MS);
    ok = FreeRTOS_setsockopt(s->udp_socket, 0, FREERTOS_SO_RCVTIMEO, &xTimeoutTime,
                             sizeof(TickType_t));
    if (ok != 0)
    {
        FreeRTOS_debug_printf(("Setting FreeRTOS socket option FREERTOS_SO_RCVTIMEO failed\n"));
    }

    ok = FreeRTOS_setsockopt(s->udp_socket, 0, FREERTOS_SO_SNDTIMEO, &xTimeoutTime,
                             sizeof(TickType_t));
    if (ok != 0)
    {
        FreeRTOS_debug_printf(("Setting FreeRTOS socket option FREERTOS_SO_SNDTIMEO failed\n"));
    }

    memset(&s->udp_client_sa, 0, sizeof(s->udp_client_sa));
    sa = (struct freertos_sockaddr *)&s->udp_client_sa;
    sa->sin_family = (s->server_addr.xIs_IPv6 ? FREERTOS_AF_INET6 : FREERTOS_AF_INET);
    sa->sin_port = FreeRTOS_htons(s->local_port);

    memset(&s->udp_server_sa, 0, sizeof(s->udp_server_sa));
    sa = (struct freertos_sockaddr *)&s->udp_server_sa;
    sa->sin_family = (s->server_addr.xIs_IPv6 ? FREERTOS_AF_INET6 : FREERTOS_AF_INET);
    sa->sin_port = FreeRTOS_htons(s->args.server_port);
    if (s->server_addr.xIs_IPv6)
    {
        memcpy(sa->sin_address.xIP_IPv6.ucBytes, s->server_addr.xIPAddress.xIP_IPv6.ucBytes,
               sizeof(sa->sin_address.xIP_IPv6.ucBytes));
    }
    else
    {
        sa->sin_address.ulIP_IPv4 = s->server_addr.xIPAddress.ulIP_IPv4;
    }

    ok = FreeRTOS_bind(s->udp_socket, &s->udp_client_sa, sizeof(s->udp_client_sa));
    if (ok != 0)
    {
        goto exit;
    }

    iperf_list_add(&s->base);

    s->task = mmosal_task_create(iperf_udp_client_task, s, MMOSAL_TASK_PRI_LOW,
                                 MMIPERF_STACK_SIZE, "iperf_udp");
    MMOSAL_ASSERT(s->task != NULL);
    result = &(s->base);
    s = NULL;

exit:
    if (s != NULL)
    {
        IPERF_FREE(struct iperf_session_udp_client, s);
    }
    return result;
}
