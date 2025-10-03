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
#include <string.h>

#include "mmosal.h"
#include "../common/mmiperf_private.h"

#include "lwip/tcpip.h"
#include "lwip/arch.h"
#include "lwip/debug.h"
#include "lwip/ip_addr.h"
#include "lwip/tcp.h"

/* Currently, only TCP is implemented */
#if LWIP_TCP && LWIP_CALLBACK_API

/** Connection handle for a TCP iperf session */
struct iperf_state_tcp
{
    struct mmiperf_state base;
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *conn_pcb;
    uint8_t poll_count;
    uint32_t mss;
    /* 1=start server when client is closed */
    uint8_t client_tradeoff_mode;
    struct iperf_settings settings;
    uint8_t have_settings_buf;
    ip_addr_t remote_addr;
    /* block parameter */
    bool bw_limit;
    uint32_t block_end_time;
    uint32_t block_txlen;
    int32_t block_remaining_txlen;
};

static err_t iperf_start_tcp_server_impl(const struct mmiperf_server_args *args,
                                         struct iperf_state_tcp **state);

static err_t iperf_tcp_poll(void *arg, struct tcp_pcb *tpcb);

static void iperf_tcp_err(void *arg, err_t err);

/** Close an iperf tcp session */
static void
iperf_tcp_close(struct iperf_state_tcp *conn, enum mmiperf_report_type report_type)
{
    err_t err;

    MMOSAL_ASSERT(conn != NULL);

    iperf_finalize_report_and_invoke_callback(&conn->base,
                                              mmosal_get_time_ms() - conn->base.time_started_ms,
                                              report_type);
    if (conn->conn_pcb != NULL)
    {
        tcp_arg(conn->conn_pcb, NULL);
        tcp_poll(conn->conn_pcb, NULL, 0);
        tcp_sent(conn->conn_pcb, NULL);
        tcp_recv(conn->conn_pcb, NULL);
        tcp_err(conn->conn_pcb, NULL);
        err = tcp_close(conn->conn_pcb);
        if (err != ERR_OK)
        {
            /* don't want to wait for free memory here... */
            tcp_abort(conn->conn_pcb);
        }
        conn->conn_pcb = NULL;
    }
    else if (conn->server_pcb != NULL)
    {
        /* no conn pcb, this is the listener pcb */
        err = tcp_close(conn->server_pcb);
        LWIP_ASSERT("error", err == ERR_OK);
        conn->server_pcb = NULL;
    }

    if (conn->conn_pcb == NULL && conn->server_pcb == NULL)
    {
        iperf_list_remove(&conn->base);
        IPERF_FREE(struct iperf_state_tcp, conn);
    }
}

/** Try to send more data on an iperf tcp session */
static err_t
iperf_tcp_client_send_more(struct iperf_state_tcp *conn)
{
    int send_more;
    err_t err;
    u16_t txlen;
    u16_t txlen_max;
    void *txptr;
    uint8_t apiflags;

    LWIP_ASSERT("conn invalid", (conn != NULL) && conn->base.tcp && (conn->base.server == 0));

    do
    {
        send_more = 0;
        if (conn->settings.amount & PP_HTONL(0x80000000))
        {
            /* this session is time-limited */
            uint32_t now = sys_now();
            uint32_t diff_ms = now - conn->base.time_started_ms;
            uint32_t time = (uint32_t)-(int32_t)lwip_htonl(conn->settings.amount);
            uint32_t time_ms = time * 10;

            if (diff_ms >= time_ms)
            {
                /* time specified by the client is over -> close the connection */
                iperf_tcp_close(conn, MMIPERF_TCP_DONE_CLIENT);
                return ERR_OK;
            }
        }
        else
        {
            /* this session is byte-limited */
            uint32_t amount_bytes = lwip_htonl(conn->settings.amount);
            /* @todo: this can send up to 1*MSS more than requested... */
            if (amount_bytes <= conn->base.report.bytes_transferred)
            {
                /* all requested bytes transferred -> close the connection */
                iperf_tcp_close(conn, MMIPERF_TCP_DONE_CLIENT);
                return ERR_OK;
            }
        }
        /* update block parameter after each block duration */
        if ((conn->bw_limit) && (conn->block_end_time < sys_now()))
        {
            conn->block_end_time += BLOCK_DURATION_MS;
            conn->block_remaining_txlen += conn->block_txlen;
        }

        if (conn->base.report.bytes_transferred < 24)
        {
            /* transmit the settings a first time */
            txptr = &((uint8_t *)&conn->settings)[conn->base.report.bytes_transferred];
            txlen_max = (u16_t)(24 - conn->base.report.bytes_transferred);
            apiflags = TCP_WRITE_FLAG_COPY;
        }
        else if (conn->base.report.bytes_transferred < 48)
        {
            /* transmit the settings a second time */
            txptr = &((uint8_t *)&conn->settings)[conn->base.report.bytes_transferred - 24];
            txlen_max = (u16_t)(48 - conn->base.report.bytes_transferred);
            apiflags = TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE;
            send_more = 1;
        }
        else
        {
            /* transmit data */
            /* @todo: every x bytes, transmit the settings again */
            txptr = LWIP_CONST_CAST(void *, iperf_get_data(conn->base.report.bytes_transferred));
            txlen_max = conn->mss;
            if (conn->base.report.bytes_transferred == 48)
            { /* @todo: fix this for intermediate settings, too */
                txlen_max = conn->mss - 24;
            }
            apiflags = 0; /* no copying needed */
            send_more = 1;
        }
        txlen = txlen_max;

        if (conn->conn_pcb->snd_buf >= (conn->mss / 2) &&
            conn->conn_pcb->snd_queuelen < LWIP_MIN(TCP_SND_QUEUELEN, TCP_SNDQUEUELEN_OVERFLOW))
        {
            if (txlen > conn->conn_pcb->snd_buf)
            {
                txlen = conn->conn_pcb->snd_buf;
            }
            err = tcp_write(conn->conn_pcb, txptr, txlen, apiflags);
        }
        else
        {
            err = ERR_MEM;
        }

        if (err == ERR_OK)
        {
            conn->base.report.bytes_transferred += txlen;
            conn->block_remaining_txlen -= txlen;
        }
        else
        {
            send_more = 0;
        }

        if ((conn->bw_limit) && (conn->block_remaining_txlen <= 0))
        {
            send_more = 0;
        }
    } while (send_more);

    tcp_output(conn->conn_pcb);
    return ERR_OK;
}

/** TCP sent callback, try to send more data */
static err_t
iperf_tcp_client_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
    struct iperf_state_tcp *conn = (struct iperf_state_tcp *)arg;
    /* @todo: check 'len' (e.g. to time ACK of all data)? for now, we just send more... */
    LWIP_ASSERT("invalid conn", conn->conn_pcb == tpcb);
    LWIP_UNUSED_ARG(tpcb);
    LWIP_UNUSED_ARG(len);

    conn->poll_count = 0;
    /* if block txlen is exceeded, sleep until block end time before sending more */
    while (conn->bw_limit && conn->block_remaining_txlen <= 0 && sys_now() < conn->block_end_time)
    {
        sys_msleep(1);
    }

    return iperf_tcp_client_send_more(conn);
}

static void init_report(struct iperf_state_tcp *conn, struct tcp_pcb *pcb)
{
    char *result;
    struct mmiperf_report *report = &conn->base.report;

    report->report_type = MMIPERF_INTERRIM_REPORT;

    if (pcb != NULL)
    {
        result = ipaddr_ntoa_r(&pcb->local_ip,
                               report->local_addr, sizeof(report->local_addr));
        LWIP_ASSERT("IP buf too short", result != NULL);
        report->local_port = pcb->local_port;
    }

    if (pcb != NULL)
    {
        result = ipaddr_ntoa_r(&pcb->remote_ip,
                               report->remote_addr, sizeof(report->remote_addr));
        LWIP_ASSERT("IP buf too short", result != NULL);
        report->remote_port = pcb->remote_port;
    }
}

/** TCP connected callback (active connection), send data now */
static err_t
iperf_tcp_client_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
    struct iperf_state_tcp *conn = (struct iperf_state_tcp *)arg;
    LWIP_ASSERT("invalid conn", conn->conn_pcb == tpcb);
    LWIP_UNUSED_ARG(tpcb);
    if (err != ERR_OK)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("Remote side aborted iperf test (%d)\n",
                                             MMIPERF_TCP_ABORTED_REMOTE));
        iperf_tcp_close(conn, MMIPERF_TCP_ABORTED_REMOTE);
        return ERR_OK;
    }
    conn->poll_count = 0;
    conn->base.time_started_ms = sys_now();
    conn->block_end_time = sys_now() + BLOCK_DURATION_MS;

    init_report(conn, tpcb);

    return iperf_tcp_client_send_more(conn);
}

/** Start TCP connection back to the client (either parallel or after the
 * receive test has finished.
 */
static err_t
iperf_tx_start_impl(const struct mmiperf_client_args *args, struct iperf_settings *settings,
                    struct iperf_state_tcp **new_conn)
{
    int result;
    err_t err;
    struct iperf_state_tcp *client_conn;
    struct tcp_pcb *newpcb;
    ip_addr_t remote_addr;
    uint16_t server_port;

    LWIP_ASSERT("remote_ip != NULL", settings != NULL);
    LWIP_ASSERT("new_conn != NULL", new_conn != NULL);
    *new_conn = NULL;

    if (args->server_port == 0)
    {
        server_port = MMIPERF_DEFAULT_PORT;
    }
    else
    {
        server_port = args->server_port;
    }

    LWIP_DEBUGF(LWIP_DBG_LEVEL_ALL, ("Starting TCP iperf client to %s:%u, amount %ld\n",
                                     args->server_addr, server_port,
                                     (int32_t)ntohl(settings->amount)));

    client_conn = (struct iperf_state_tcp *)IPERF_ALLOC(struct iperf_state_tcp);
    if (client_conn == NULL)
    {
        return ERR_MEM;
    }

    result = ipaddr_aton(args->server_addr, &remote_addr);
    if (!result)
    {
        IPERF_FREE(struct iperf_state_tcp, client_conn);
        return ERR_ARG;
    }

    newpcb = tcp_new_ip_type(IP_GET_TYPE(&remote_addr));
    if (newpcb == NULL)
    {
        IPERF_FREE(struct iperf_state_tcp, client_conn);
        return ERR_MEM;
    }
    memset(client_conn, 0, sizeof(*client_conn));
    client_conn->base.tcp = 1;
    client_conn->conn_pcb = newpcb;
    client_conn->base.time_started_ms = sys_now();
    client_conn->base.report_fn = args->report_fn;
    client_conn->base.report_arg = args->report_arg;
    memcpy(&client_conn->settings, settings, sizeof(*settings));
    client_conn->have_settings_buf = 1;
    client_conn->mss = TCP_MSS;

#if LWIP_IPV6
    if (IP_IS_V6(&remote_addr))
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
            LWIP_DEBUGF(LWIP_DBG_LEVEL_SERIOUS, ("bandwidth limit too low\n"));
            iperf_tcp_close(client_conn, MMIPERF_TCP_ABORTED_LOCAL);
            return ERR_ARG;
        }
    }

    tcp_arg(newpcb, client_conn);
    tcp_sent(newpcb, iperf_tcp_client_sent);
    tcp_poll(newpcb, iperf_tcp_poll, 2U);
    tcp_err(newpcb, iperf_tcp_err);

    err = tcp_connect(newpcb, &remote_addr, server_port, iperf_tcp_client_connected);
    if (err != ERR_OK)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING,
                    ("Test aborted due to local error (%d)\n", MMIPERF_TCP_ABORTED_LOCAL));
        iperf_tcp_close(client_conn, MMIPERF_TCP_ABORTED_LOCAL);
        return err;
    }
    iperf_list_add(&client_conn->base);
    *new_conn = client_conn;
    return ERR_OK;
}

/** Receive data on an iperf tcp session */
static err_t
iperf_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
    uint8_t tmp;
    u16_t tot_len;
    uint32_t packet_idx;
    struct pbuf *q;
    struct iperf_state_tcp *conn = (struct iperf_state_tcp *)arg;

    LWIP_ASSERT("pcb mismatch", conn->conn_pcb == tpcb);
    LWIP_UNUSED_ARG(tpcb);

    if (err != ERR_OK)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("Remote side aborted iperf test (%d)\n",
                                             MMIPERF_TCP_ABORTED_REMOTE));
        iperf_tcp_close(conn, MMIPERF_TCP_ABORTED_REMOTE);
        return ERR_OK;
    }
    if (p == NULL)
    {
        iperf_tcp_close(conn, MMIPERF_TCP_DONE_SERVER);
        return ERR_OK;
    }
    tot_len = p->tot_len;

    conn->poll_count = 0;

    if ((!conn->have_settings_buf) ||
        ((conn->base.report.bytes_transferred - 24) % (1024 * 128) == 0))
    {
        /* wait for 24-byte header */
        if (p->tot_len < sizeof(conn->settings))
        {
            LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("test aborted due to data check error  (%d)\n",
                                                 MMIPERF_TCP_ABORTED_LOCAL_DATAERROR));
            iperf_tcp_close(conn, MMIPERF_TCP_ABORTED_LOCAL_DATAERROR);
            pbuf_free(p);
            return ERR_OK;
        }
        if (!conn->have_settings_buf)
        {
            if (pbuf_copy_partial(p, &conn->settings, sizeof(conn->settings), 0) !=
                sizeof(conn->settings))
            {
                LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING,
                            ("Test aborted due to local error (%d)\n", MMIPERF_TCP_ABORTED_LOCAL));
                iperf_tcp_close(conn, MMIPERF_TCP_ABORTED_LOCAL);
                pbuf_free(p);
                return ERR_OK;
            }
            conn->have_settings_buf = 1;
        }
        conn->base.report.bytes_transferred += sizeof(conn->settings);
        if (conn->base.report.bytes_transferred <= 24)
        {
            conn->base.time_started_ms = sys_now();
            tcp_recved(tpcb, p->tot_len);
            pbuf_free(p);
            return ERR_OK;
        }
        tmp = pbuf_remove_header(p, 24);
        LWIP_ASSERT("pbuf_remove_header failed", tmp == 0);
        LWIP_UNUSED_ARG(tmp); /* for LWIP_NOASSERT */
    }

    packet_idx = 0;
    for (q = p; q != NULL; q = q->next)
    {
        packet_idx += q->len;
    }
    LWIP_ASSERT("count mismatch", packet_idx == p->tot_len);
    conn->base.report.bytes_transferred += packet_idx;
    tcp_recved(tpcb, tot_len);
    pbuf_free(p);
    return ERR_OK;
}

/** Error callback, iperf tcp session aborted */
static void
iperf_tcp_err(void *arg, err_t err)
{
    struct iperf_state_tcp *conn = (struct iperf_state_tcp *)arg;
    LWIP_UNUSED_ARG(err);

    /* pcb is already deallocated, prevent double-free */
    conn->conn_pcb = NULL;
    conn->server_pcb = NULL;

    LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING,
                ("Remote side aborted iperf test (%d)\n", MMIPERF_TCP_ABORTED_REMOTE));
    iperf_tcp_close(conn, MMIPERF_TCP_ABORTED_REMOTE);
}

/** TCP poll callback, try to send more data */
static err_t
iperf_tcp_poll(void *arg, struct tcp_pcb *tpcb)
{
    struct iperf_state_tcp *conn = (struct iperf_state_tcp *)arg;
    LWIP_ASSERT("pcb mismatch", conn->conn_pcb == tpcb);
    LWIP_UNUSED_ARG(tpcb);
    if (++conn->poll_count >= IPERF_TCP_MAX_IDLE_S)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING,
                    ("Test aborted due to local error (%d)\n", MMIPERF_TCP_ABORTED_LOCAL));
        iperf_tcp_close(conn, MMIPERF_TCP_ABORTED_LOCAL);
        return ERR_OK; /* iperf_tcp_close frees conn */
    }

    if (!conn->base.server)
    {
        iperf_tcp_client_send_more(conn);
    }

    return ERR_OK;
}

/** This is called when a new client connects for an iperf tcp session */
static err_t
iperf_tcp_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    struct iperf_state_tcp *conn;
    if ((err != ERR_OK) || (newpcb == NULL) || (arg == NULL))
    {
        return ERR_VAL;
    }

    conn = (struct iperf_state_tcp *)arg;
    LWIP_ASSERT("invalid session", conn->base.server);
    LWIP_ASSERT("invalid listen pcb", conn->server_pcb != NULL);

    if (conn->conn_pcb != NULL)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("TCP session already in progress\n"));
        return ERR_ALREADY;
    }

    memset(&conn->base.report, 0, sizeof(conn->base.report));
    memset(&conn->settings, 0, sizeof(conn->settings));
    conn->have_settings_buf = false;

    /* setup the tcp rx connection */
    conn->conn_pcb = newpcb;
    tcp_arg(newpcb, conn);
    tcp_recv(newpcb, iperf_tcp_recv);
    tcp_poll(newpcb, iperf_tcp_poll, 2U);
    tcp_err(conn->conn_pcb, iperf_tcp_err);

    init_report(conn, newpcb);

    return ERR_OK;
}

mmiperf_handle_t mmiperf_start_tcp_server(const struct mmiperf_server_args *args)
{
    err_t err;
    struct iperf_state_tcp *state = NULL;

    LOCK_TCPIP_CORE();
    err = iperf_start_tcp_server_impl(args, &state);
    UNLOCK_TCPIP_CORE();
    if (err == ERR_OK)
    {
        return &(state->base);
    }
    return NULL;
}

static err_t iperf_start_tcp_server_impl(const struct mmiperf_server_args *args,
                                         struct iperf_state_tcp **state)
{
    err_t err = ERR_MEM;
    struct tcp_pcb *pcb = NULL;
    struct iperf_state_tcp *s = NULL;
    uint16_t local_port;
    ip_addr_t local_addr = *(IP_ADDR_ANY);

    if (args->local_addr[0] != '\0')
    {
        int result = ipaddr_aton(args->local_addr, &local_addr);
        if (!result)
        {
            LWIP_DEBUGF(LWIP_DBG_LEVEL_SERIOUS,
                        ("Unable to parse local_addr as IP address (%s)\n", args->local_addr));
            err = ERR_ARG;
            goto exit;
        }
    }

    LWIP_ASSERT_CORE_LOCKED();

    LWIP_ASSERT("state != NULL", state != NULL);

    s = (struct iperf_state_tcp *)IPERF_ALLOC(struct iperf_state_tcp);
    if (s == NULL)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_SERIOUS, ("Failed to allocate state data\n"));
        err = ERR_MEM;
        goto exit;
    }
    memset(s, 0, sizeof(struct iperf_state_tcp));
    s->base.tcp = 1;
    s->base.server = 1;
    s->base.report_fn = args->report_fn;
    s->base.report_arg = args->report_arg;

    pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == NULL)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_SERIOUS, ("Failed to create pcb for iperf server\n"));
        err = ERR_MEM;
        goto exit;
    }

    local_port = args->local_port ? args->local_port : MMIPERF_DEFAULT_PORT;

    err = tcp_bind(pcb, &local_addr, local_port);
    if (err != ERR_OK)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_SERIOUS, ("Failed to bind to TCP port %d\n", local_port));
        goto exit;
    }
    pcb = tcp_listen_with_backlog_and_err(pcb, 1, &err);
    if (err != ERR_OK)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_SERIOUS, ("Failed to listen on TCP port\n"));
        goto exit;
    }
    s->server_pcb = pcb;
    tcp_arg(s->server_pcb, s);
    tcp_accept(s->server_pcb, iperf_tcp_accept);

    iperf_list_add(&s->base);
    *state = s;
    s = NULL;
    return ERR_OK;

exit:
    if (pcb != NULL)
    {
        tcp_close(pcb);
        pcb = NULL;
    }
    if (s != NULL)
    {
        IPERF_FREE(struct iperf_state_tcp, s);
        s = NULL;
    }
    return err;
}

/** Control */
enum lwiperf_client_type
{
    /** Unidirectional tx only test */
    LWIPERF_CLIENT,
    /** Do a bidirectional test simultaneously */
    LWIPERF_DUAL,
    /** Do a bidirectional test individually */
    LWIPERF_TRADEOFF
};

mmiperf_handle_t mmiperf_start_tcp_client(const struct mmiperf_client_args *args)
{
    err_t ret;
    struct iperf_settings settings;
    struct iperf_state_tcp *state = NULL;
    mmiperf_handle_t result = NULL;

    /* Bidirectional/trade-off disabled until better tested and also until supported in UDP. */

    memset(&settings, 0, sizeof(settings));

    settings.amount = htonl(args->amount);
    settings.num_threads = htonl(1);
    settings.remote_port = htonl(MMIPERF_DEFAULT_PORT);

    LOCK_TCPIP_CORE();
    ret = iperf_tx_start_impl(args, &settings, &state);
    if (ret == ERR_OK)
    {
        LWIP_ASSERT("state != NULL", state != NULL);
        result = &(state->base);
    }
    UNLOCK_TCPIP_CORE();
    return result;
}

#endif /* LWIP_TCP && LWIP_CALLBACK_API */
