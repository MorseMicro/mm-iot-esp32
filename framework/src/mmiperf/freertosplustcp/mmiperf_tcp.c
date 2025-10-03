/*
 * Copyright 2023-2024 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>

#include "mmiperf_freertosplustcp.h"
#include "mmipal.h"
#include "mmutils.h"

#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"
#include "FreeRTOS_IPv4_Sockets.h"
#include "FreeRTOS_IPv6_Sockets.h"

/** Connection handle for a TCP iperf session */
struct iperf_state_tcp
{
    struct mmiperf_state base;
    Socket_t server_socket;
    Socket_t conn_socket;
    struct freertos_sockaddr tcp_client_sa;
    struct freertos_sockaddr tcp_server_sa;
    uint8_t poll_count;
    uint8_t next_num;
    uint32_t mss;
    /* 1=start server when client is closed */
    uint8_t client_tradeoff_mode;
    struct iperf_settings settings;
    uint8_t have_settings_buf;
    uint8_t specific_remote;
    /* block parameter */
    bool bw_limit;
    uint32_t block_end_time;
    uint32_t block_txlen;
    uint32_t block_remaining_txlen;
    struct mmosal_task *tcp_client_task;
    struct mmosal_task *tcp_server_task;
};

static int tcp_listen_on_new_socket(struct iperf_state_tcp *s)
{
    int err = 0;
    TickType_t xTimeoutTime = pdMS_TO_TICKS(1000);

    s->server_socket = FreeRTOS_socket(FREERTOS_AF_INET, FREERTOS_SOCK_STREAM,
                                       FREERTOS_IPPROTO_TCP);
    if (s->server_socket == NULL)
    {
        FreeRTOS_debug_printf(("Failed to create TCP server socket\n"));
        return -1;
    }

    err = FreeRTOS_setsockopt(s->server_socket, 0, FREERTOS_SO_RCVTIMEO, &xTimeoutTime,
                              sizeof(TickType_t));
    if (err != 0)
    {
        FreeRTOS_debug_printf(("Setting FreeRTOS socket option FREERTOS_SO_RCVTIMEO failed\n"));
    }

    err = FreeRTOS_setsockopt(s->server_socket, 0, FREERTOS_SO_SNDTIMEO, &xTimeoutTime,
                              sizeof(TickType_t));
    if (err != 0)
    {
        FreeRTOS_debug_printf(("Setting FreeRTOS socket option FREERTOS_SO_SNDTIMEO failed\n"));
    }

    err = FreeRTOS_bind(s->server_socket, &s->tcp_server_sa, sizeof(s->tcp_server_sa));
    if (err != 0)
    {
        FreeRTOS_debug_printf(("Failed to bind TCP server socket, err = %d\n", err));
        return err;
    }

    err = FreeRTOS_listen(s->server_socket, 1);
    if (err != 0)
    {
        FreeRTOS_debug_printf(("Failed to listen on TCP socket, err = %d\n", err));
        return err;
    }

    return err;
}

static void iperf_tcp_server_task(void *arg)
{
    struct iperf_state_tcp *s;
    uint32_t client_sa_len = sizeof(s->tcp_client_sa);
    uint8_t *recv_buff = NULL;
    int16_t len = 0;
    uint32_t tcp_recv_len = ipconfigNETWORK_MTU;
    int ret = 0;

    MMOSAL_ASSERT(arg != NULL);
    s = (struct iperf_state_tcp *)arg;
    memset(&s->tcp_client_sa, 0, client_sa_len);

    recv_buff = (uint8_t *)mmosal_malloc(tcp_recv_len);
    if (recv_buff == NULL)
    {
        FreeRTOS_debug_printf(("iperf tcp server task failed to alloc recv_buff\n"));
        goto exit;
    }

    while (1)
    {
        s->conn_socket = FreeRTOS_accept(s->server_socket, &s->tcp_client_sa, &client_sa_len);
        if ((s->conn_socket != NULL) && FreeRTOS_issocketconnected(s->conn_socket))
        {
            iperf_freertosplustcp_session_start_common(&s->base,
                                                       &s->tcp_server_sa, &s->tcp_client_sa);
        }

        while (s->conn_socket != NULL)
        {
            len = FreeRTOS_recv(s->conn_socket, recv_buff, tcp_recv_len, 0);
            if (len > 0)
            {
                s->poll_count = 0;
                s->base.report.bytes_transferred += len;
            }

            if (!FreeRTOS_issocketconnected(s->conn_socket))
            {
                uint32_t duration_ms = mmosal_get_time_ms() - s->base.time_started_ms;
                iperf_finalize_report_and_invoke_callback(&s->base, duration_ms,
                                                          MMIPERF_TCP_DONE_SERVER);

                ret = FreeRTOS_closesocket(s->conn_socket);
                if (ret < 0)
                {
                    FreeRTOS_debug_printf(("Socket close failed, ret = %d\n", ret));
                }
                s->conn_socket = NULL;

                ret = FreeRTOS_closesocket(s->server_socket);
                if (ret < 0)
                {
                    FreeRTOS_debug_printf(("Socket close failed, ret = %d\n", ret));
                }
                s->server_socket = NULL;

                ret = tcp_listen_on_new_socket(s);
                if (ret != 0)
                {
                    FreeRTOS_debug_printf(("Failed to add new socket and listen on TCP port\n"));
                }

                memset(&s->tcp_client_sa, 0, client_sa_len);
                break;
            }
        }
    }

exit:
    if (recv_buff != NULL)
    {
        mmosal_free(recv_buff);
        recv_buff = NULL;
    }
    if (s != NULL)
    {
        mmosal_free(s);
        s = NULL;
    }
}

static int iperf_start_tcp_server_impl(const struct mmiperf_server_args *args,
                                       struct iperf_state_tcp **state)
{
    int err = -1;
    struct iperf_state_tcp *s = NULL;
    uint16_t local_port;
    IPv46_Address_t local_addr = { 0 };
    BaseType_t ret = pdFAIL;

    MMOSAL_ASSERT(state != NULL);

    if (args->local_addr[0] != '\0')
    {
#if ipconfigUSE_IPv4
        local_addr.xIs_IPv6 = pdFALSE;
        ret = FreeRTOS_inet_pton4(args->local_addr, &local_addr.xIPAddress.ulIP_IPv4);
#endif
#if ipconfigUSE_IPv6
        if (ret != pdPASS)
        {
            ret = FreeRTOS_inet_pton6(args->local_addr, &local_addr.xIPAddress.xIP_IPv6.ucBytes);
            if (ret == pdPASS)
            {
                local_addr.xIs_IPv6 = pdTRUE;
            }
            else
            {
                FreeRTOS_debug_printf(("Unable to parse local_addr as IP address (%s)\n",
                                       args->local_addr));
            }
        }
#else
        MM_UNUSED(ret);
#endif
    }

    s = (struct iperf_state_tcp *)mmosal_malloc(sizeof(*s));
    if (s == NULL)
    {
        FreeRTOS_debug_printf(("Failed to allocate state data in TCP server\n"));
        err = -1;
        goto exit;
    }
    memset(s, 0, sizeof(*s));
    s->base.tcp = 1;
    s->base.server = 1;
    s->base.report_fn = args->report_fn;
    s->base.report_arg = args->report_arg;

    local_port = args->local_port ? args->local_port : MMIPERF_DEFAULT_PORT;

    s->tcp_server_sa.sin_family = (local_addr.xIs_IPv6 ? FREERTOS_AF_INET6 : FREERTOS_AF_INET);
    s->tcp_server_sa.sin_port = FreeRTOS_htons(local_port);
    if (local_addr.xIs_IPv6)
    {
        memcpy(s->tcp_server_sa.sin_address.xIP_IPv6.ucBytes,
               local_addr.xIPAddress.xIP_IPv6.ucBytes,
               sizeof(s->tcp_server_sa.sin_address.xIP_IPv6.ucBytes));
    }
    else
    {
        s->tcp_server_sa.sin_address.ulIP_IPv4 = local_addr.xIPAddress.ulIP_IPv4;
    }

    err = tcp_listen_on_new_socket(s);
    if (err != 0)
    {
        FreeRTOS_debug_printf(("Failed to add new socket and listen on TCP port\n"));
        goto exit;
    }

    s->tcp_server_task = mmosal_task_create(iperf_tcp_server_task, s, MMOSAL_TASK_PRI_LOW,
                                            MMIPERF_STACK_SIZE, "iperf_tcp_server");
    MMOSAL_ASSERT(s->tcp_server_task != NULL);

    iperf_list_add(&s->base);
    *state = s;
    return 0;

exit:
    if (s != NULL)
    {
        mmosal_free(s);
        s = NULL;
    }
    return err;
}

mmiperf_handle_t mmiperf_start_tcp_server(const struct mmiperf_server_args *args)
{
    int err;
    struct iperf_state_tcp *state = NULL;

    err = iperf_start_tcp_server_impl(args, &state);
    if (err == 0)
    {
        return &(state->base);
    }
    return NULL;
}

/** Close an iperf tcp session */
static void iperf_tcp_close(struct iperf_state_tcp *conn, enum mmiperf_report_type report_type)
{
    int ok;
    uint32_t duration_ms = mmosal_get_time_ms() - conn->base.time_started_ms;

    iperf_list_remove(&conn->base);
    iperf_finalize_report_and_invoke_callback(&conn->base, duration_ms, report_type);

    if (conn->conn_socket != NULL)
    {
        ok = FreeRTOS_closesocket(conn->conn_socket);
        if (ok < 0)
        {
            FreeRTOS_debug_printf(("Socket close failed\n"));
        }
        conn->conn_socket = NULL;
    }
    else if (conn->server_socket != NULL)
    {
        /* no conn socket, this is the listener socket */
        ok = FreeRTOS_closesocket(conn->server_socket);
        if (ok < 0)
        {
            FreeRTOS_debug_printf(("Socket close failed\n"));
        }
        conn->server_socket = NULL;
    }
    mmosal_free(conn);
}

/** Try to send more data on an iperf tcp session */
static int iperf_tcp_client_send_more(struct iperf_state_tcp *conn)
{
    int send_more = 1;
    int ret;
    uint16_t txlen;
    uint16_t txlen_max;
    void *txptr;

    MMOSAL_ASSERT((conn != NULL) && conn->base.tcp && (conn->base.server == 0));

    do
    {
        if (conn->settings.amount & FreeRTOS_htonl(0x80000000))
        {
            /* this session is time-limited */
            uint32_t now = mmosal_get_time_ms();
            uint32_t diff_ms = now - conn->base.time_started_ms;
            uint32_t time = (uint32_t)-(int32_t)FreeRTOS_htonl(conn->settings.amount);
            uint32_t time_ms = time * 10;

            if (diff_ms >= time_ms)
            {
                /* time specified by the client is over -> close the connection */
                goto exit;
            }
        }
        else
        {
            /* this session is byte-limited */
            uint32_t amount_bytes = FreeRTOS_htonl(conn->settings.amount);
            /* @todo: this can send up to 1*MSS more than requested... */
            if (amount_bytes <= conn->base.report.bytes_transferred)
            {
                /* all requested bytes transferred -> close the connection */
                goto exit;
            }
        }
        /* update block parameter after each block duration */
        if ((conn->bw_limit) && (conn->block_end_time < mmosal_get_time_ms()))
        {
            conn->block_end_time += BLOCK_DURATION_MS;
            conn->block_remaining_txlen += conn->block_txlen;
        }

        if (conn->base.report.bytes_transferred < 24)
        {
            /* transmit the settings a first time */
            txptr = &((uint8_t *)&conn->settings)[conn->base.report.bytes_transferred];
            txlen_max = (uint16_t)(24 - conn->base.report.bytes_transferred);
        }
        else if (conn->base.report.bytes_transferred < 48)
        {
            /* transmit the settings a second time */
            txptr = &((uint8_t *)&conn->settings)[conn->base.report.bytes_transferred - 24];
            txlen_max = (uint16_t)(48 - conn->base.report.bytes_transferred);
        }
        else
        {
            /* transmit data */
            /* @todo: every x bytes, transmit the settings again */
            txptr = (void *)iperf_get_data(conn->base.report.bytes_transferred);
            txlen_max = conn->mss;
            if (conn->base.report.bytes_transferred == 48)
            { /* @todo: fix this for intermediate settings, too */
                txlen_max = conn->mss - 24;
            }
        }
        txlen = txlen_max;

        ret = FreeRTOS_send(conn->conn_socket, txptr, txlen, FREERTOS_MSG_DONTWAIT);
        if (ret >= 0)
        {
            conn->base.report.bytes_transferred += ret;
            conn->block_remaining_txlen -= ret;
        }
        else if (ret == -pdFREERTOS_ERRNO_ENOTCONN)
        {
            printf("Socket connection was closed!\n");
            iperf_tcp_close(conn, MMIPERF_TCP_ABORTED_REMOTE);
            return -1;
        }
        else
        {
            mmosal_task_sleep(1);
        }

        if ((conn->bw_limit) && (conn->block_remaining_txlen <= 0))
        {
            send_more = 0;
        }
    } while (send_more);

exit:
    /* Wait until all the data in the tx queue is transmited. */
    while (FreeRTOS_tx_size(conn->conn_socket) > 0)
    {
        mmosal_task_sleep(1);
    }
    ret = FreeRTOS_shutdown(conn->conn_socket, 2);
    if (ret != 0)
    {
        FreeRTOS_debug_printf(("TCP socket shutdown failed\n"));
    }
    iperf_tcp_close(conn, MMIPERF_TCP_DONE_CLIENT);
    return 0;
}

static void iperf_tcp_client_task(void *arg)
{
    struct iperf_state_tcp *conn = (struct iperf_state_tcp *)arg;
    MMOSAL_ASSERT(conn->conn_socket != NULL);

    conn->poll_count = 0;
    conn->base.time_started_ms = mmosal_get_time_ms();
    conn->block_end_time = mmosal_get_time_ms() + BLOCK_DURATION_MS;

    iperf_tcp_client_send_more(conn);
}

/**
 * The number of miliseconds to attempt a send or receive over a TCP socket before timing out.
 * This should be at least 5 seconds to allow enough time for a TCP handshake retry in the case
 * where the STA's IP address already exists in the AP's ARP table under a different MAC address.
 */
#define IPERF_TCP_SOCKET_TIMEOUT_TIME_MS  5000

MM_STATIC_ASSERT(IPERF_TCP_SOCKET_TIMEOUT_TIME_MS >= 5000,
                 "Timeout must be at least 5000ms to allow time for a TCP handshake retry.\n");

/**
 * Start TCP connection back to the client (either parallel or after the
 * receive test has finished.
 */
static int iperf_tx_start_impl(const struct mmiperf_client_args *args,
                               struct iperf_settings *settings,
                               struct iperf_state_tcp **new_conn)
{
    int ok;
    struct iperf_state_tcp *client_conn;
    IPv46_Address_t remote_addr;
    uint16_t server_port;
    BaseType_t ret = pdFAIL;

    MMOSAL_ASSERT(settings != NULL);
    MMOSAL_ASSERT(new_conn != NULL);
    *new_conn = NULL;

    if (args->server_port == 0)
    {
        server_port = MMIPERF_DEFAULT_PORT;
    }
    else
    {
        server_port = args->server_port;
    }

    FreeRTOS_debug_printf(("Starting TCP iperf client to %s:%u, amount %ld\n",
                           args->server_addr, server_port,
                           (int32_t)FreeRTOS_ntohl(settings->amount)));

    client_conn = (struct iperf_state_tcp *)mmosal_malloc(sizeof(*client_conn));
    if (client_conn == NULL)
    {
        return -1;
    }

#if ipconfigUSE_IPv4
    remote_addr.xIs_IPv6 = pdFALSE;
    ret = FreeRTOS_inet_pton4(args->server_addr, &remote_addr.xIPAddress.ulIP_IPv4);
#endif
#if ipconfigUSE_IPv6
    if (ret != pdPASS)
    {
        ret = FreeRTOS_inet_pton6(args->server_addr, &remote_addr.xIPAddress.xIP_IPv6.ucBytes);
        if (ret == pdPASS)
        {
            remote_addr.xIs_IPv6 = pdTRUE;
        }
    }
#else
    MM_UNUSED(ret);
#endif
    if (remote_addr.xIPAddress.ulIP_IPv4 == 0)
    {
        mmosal_free(client_conn);
        return -1;
    }

    memset(client_conn, 0, sizeof(*client_conn));
    client_conn->base.tcp = 1;
    client_conn->base.time_started_ms = mmosal_get_time_ms();
    client_conn->base.report_fn = args->report_fn;
    client_conn->base.report_arg = args->report_arg;
    client_conn->next_num = 4; /* initial nr is '4' since the header has 24 byte */
    memcpy(&client_conn->settings, settings, sizeof(*settings));
    client_conn->have_settings_buf = 1;
    client_conn->mss = ipconfigTCP_MSS;

    client_conn->conn_socket =
        FreeRTOS_socket((remote_addr.xIs_IPv6 ? FREERTOS_AF_INET6 : FREERTOS_AF_INET),
                        FREERTOS_SOCK_STREAM, FREERTOS_IPPROTO_TCP);
    if (client_conn->conn_socket == NULL)
    {
        mmosal_free(client_conn);
        return -1;
    }

    TickType_t xTimeoutTime = pdMS_TO_TICKS(IPERF_TCP_SOCKET_TIMEOUT_TIME_MS);
    ok = FreeRTOS_setsockopt(client_conn->conn_socket, 0, FREERTOS_SO_RCVTIMEO, &xTimeoutTime,
                             sizeof(TickType_t));
    if (ok != 0)
    {
        FreeRTOS_debug_printf(("Setting FreeRTOS socket option FREERTOS_SO_RCVTIMEO failed\n"));
    }

    ok = FreeRTOS_setsockopt(client_conn->conn_socket, 0, FREERTOS_SO_SNDTIMEO, &xTimeoutTime,
                             sizeof(TickType_t));
    if (ok != 0)
    {
        FreeRTOS_debug_printf(("Setting FreeRTOS socket option FREERTOS_SO_SNDTIMEO failed\n"));
    }

#if ipconfigUSE_IPv6
    if (remote_addr.xIs_IPv6)
    {
        client_conn->mss -= IPV6_HEADER_SIZE_DIFF;
    }
#endif

    /* set block parameter if bandwidth limit is set */
    if (args->target_bw == 0)
    {
        client_conn->bw_limit = false;
    }
    else
    {
        client_conn->bw_limit = true;
        client_conn->block_txlen = args->target_bw * BLOCK_DURATION_MS / 8;
        client_conn->block_remaining_txlen = client_conn->block_txlen;
        /* log error message if maximum segment size is too large for chosen bandwidth limit */
        if (client_conn->mss > client_conn->block_txlen)
        {
            FreeRTOS_debug_printf(("bandwidth limit too low\n"));
            iperf_tcp_close(client_conn, MMIPERF_TCP_ABORTED_LOCAL);
            return -1;
        }
    }

    memset(&client_conn->tcp_server_sa, 0, sizeof(client_conn->tcp_server_sa));
    client_conn->tcp_server_sa.sin_family =
        (remote_addr.xIs_IPv6 ? FREERTOS_AF_INET6 : FREERTOS_AF_INET);
    client_conn->tcp_server_sa.sin_port = FreeRTOS_htons(server_port);
    if (remote_addr.xIs_IPv6)
    {
        memcpy(client_conn->tcp_server_sa.sin_address.xIP_IPv6.ucBytes,
               remote_addr.xIPAddress.xIP_IPv6.ucBytes,
               sizeof(client_conn->tcp_server_sa.sin_address.xIP_IPv6.ucBytes));
    }
    else
    {
        client_conn->tcp_server_sa.sin_address.ulIP_IPv4 = remote_addr.xIPAddress.ulIP_IPv4;
    }

    ok = FreeRTOS_connect(client_conn->conn_socket, &client_conn->tcp_server_sa,
                          sizeof(client_conn->tcp_server_sa));
    if (ok != 0)
    {
        FreeRTOS_debug_printf(("TCP socket connect failed\n"));
        iperf_tcp_close(client_conn, MMIPERF_TCP_ABORTED_LOCAL);
        return -1;
    }

    FreeRTOS_GetLocalAddress(client_conn->conn_socket, &client_conn->tcp_client_sa);
    iperf_freertosplustcp_session_start_common(&client_conn->base,
                                               &client_conn->tcp_client_sa,
                                               &client_conn->tcp_server_sa);

    client_conn->tcp_client_task =
        mmosal_task_create(iperf_tcp_client_task, client_conn, MMOSAL_TASK_PRI_LOW,
                           MMIPERF_STACK_SIZE, "iperf_tcp_client");
    MMOSAL_ASSERT(client_conn->tcp_client_task != NULL);

    iperf_list_add(&client_conn->base);
    *new_conn = client_conn;
    return 0;
}

mmiperf_handle_t mmiperf_start_tcp_client(const struct mmiperf_client_args *args)
{
    int ret = 0;
    struct iperf_settings settings;
    struct iperf_state_tcp *state = NULL;
    mmiperf_handle_t result = NULL;

    /* Bidirectional/trade-off disabled until better tested and also until supported in UDP. */

    memset(&settings, 0, sizeof(settings));

    settings.amount = FreeRTOS_htonl(args->amount);
    settings.num_threads = FreeRTOS_htonl(1);
    settings.remote_port = FreeRTOS_htonl(MMIPERF_DEFAULT_PORT);

    ret = iperf_tx_start_impl(args, &settings, &state);
    if (ret == 0)
    {
        MMOSAL_ASSERT(state != NULL);
        result = &(state->base);
    }
    return result;
}
