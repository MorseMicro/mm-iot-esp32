/*
 * Copyright 2021-2024 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdatomic.h>

#include "lwip/err.h"
#include "mmosal.h"
#include "../common/mmiperf_private.h"
#include "mmiperf_lwip.h"
#include "mmutils.h"

#include "lwip/debug.h"
#include "lwip/igmp.h"
#include "lwip/ip_addr.h"
#include "lwip/mld6.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"

/* Currently, only UDP is implemented */
#if LWIP_UDP && LWIP_CALLBACK_API

#define HEADER_VERSION1 0x80000000

typedef struct _udp_header
{
    uint32_t id_lo;
    uint32_t tv_sec;
    uint32_t tv_usec;
    uint32_t id_hi; /* Note: not present in Iperf 2.0.9 */
} udp_header_t;

/* State data fo a speicific iperf server session */
struct iperf_server_session_udp
{
    /* A negative value indicates there is no active session (but there may still be state
     * from a previous session) */
    int32_t next_packet_id;

    ip_addr_t client_addr;
    uint16_t client_port;
    struct timeval ipg_start;
};

/** Connection handle for a UDP iperf server */
struct iperf_server_state_udp
{
    struct mmiperf_state base;
    struct
    {
        ip_addr_t local_addr;
        uint16_t local_port;
        enum iperf_version version;
    } args;
    struct udp_pcb *pcb;
    struct iperf_server_session_udp session;
};

struct iperf_client_state_udp
{
    struct mmiperf_state base;

    /* Given configuration */
    struct mmiperf_client_args args;
    ip_addr_t server_addr;

    /* State */
    struct udp_pcb *pcb;
    uint16_t local_port;
    uint32_t check_interval;
    struct mmosal_task *task;
    struct mmosal_semb *report_semb;
    bool awaiting_report;
    struct pbuf *report;
    int32_t next_packet_id;

    /* block parameter for bandwdith limit */
    uint32_t block_tx_amount;
};

#ifndef min
#define min(a, b) ((b) < (a) ? (b) : (a))
#endif

static bool udp_server_session_has_timed_out(struct iperf_server_state_udp *server_state)
{
    return mmosal_time_has_passed(
        server_state->base.last_rx_time_ms + IPERF_UDP_SERVER_SESSION_TIMEOUT_MS);
}

static struct iperf_server_session_udp *udp_server_start_session(
    struct iperf_server_state_udp *server_state, const ip_addr_t *addr, uint16_t port)
{
    /* For now we only support a single session. */
    struct iperf_server_session_udp *session = &(server_state->session);
    if (session->next_packet_id >= 0 && !udp_server_session_has_timed_out(server_state))
    {
        return NULL;
    }

    memset(session, 0, sizeof(*session));
    session->client_addr = *addr;
    session->client_port = port;
    memset(&server_state->base.report, 0, sizeof(server_state->base.report));
    server_state->base.report.report_type = MMIPERF_INTERRIM_REPORT;
    server_state->base.time_started_ms = mmosal_get_time_ms();
    server_state->base.report.local_port = server_state->args.local_port;
    server_state->base.report.remote_port = port;

    const char *result;
    result = ipaddr_ntoa_r(&server_state->pcb->local_ip,
                           server_state->base.report.local_addr,
                           sizeof(server_state->base.report.local_addr));
    LWIP_ASSERT("IP buf too short", result != NULL);
    result = ipaddr_ntoa_r(addr,
                           server_state->base.report.remote_addr,
                           sizeof(server_state->base.report.remote_addr));
    LWIP_ASSERT("IP buf too short", result != NULL);

    return session;
}

/* This is a workaround for LWIP not providing an implementation of ip_addr_cmp_zoneless()
 * for the case where IPv4 is enabled but IPv6 is not. */
#ifndef ip_addr_cmp_zoneless
#define ip_addr_cmp_zoneless(addr1, addr2) ip_addr_eq(addr1, addr2)
#endif

static struct iperf_server_session_udp *get_session(struct iperf_server_state_udp *server_state,
                                                    const ip_addr_t *addr, uint16_t port)
{
    /* We only support a single session. */
    struct iperf_server_session_udp *session = &(server_state->session);
    if (ip_addr_cmp_zoneless(&(session->client_addr), addr) && session->client_port == port &&
        !udp_server_session_has_timed_out(server_state))
    {
        return session;
    }
    else
    {
        return udp_server_start_session(server_state, addr, port);
    }
}

/**
 * Get the difference in microseconds between two timevals.
 *
 * \note Delta must not exceed the range of an int32.
 */
static int32_t time_delta(const struct timeval *a, const struct timeval *b)
{
    int32_t delta = (a->tv_sec - b->tv_sec) * 1000000;
    delta += (a->tv_usec - b->tv_usec);
    return delta;
}

/* Construct and send server report after final packet if not a multicast address. */
static void iperf_udp_server_handle_final_packet(struct iperf_server_state_udp *server_state,
                                                 struct udp_pcb *pcb, const ip_addr_t *addr,
                                                 uint16_t port, struct pbuf *p)
{
    struct pbuf *report_pbuf = NULL;

    if (ip_addr_ismulticast(&(server_state->args.local_addr)))
    {
        return;
    }

    struct iperf_udp_server_report report;

    report_pbuf = pbuf_alloc(PBUF_TRANSPORT, sizeof(report) + sizeof(struct iperf_udp_header),
                             PBUF_RAM);
    if (report_pbuf == NULL)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("Pbuf alloc failure.\n"));
        goto report_cleanup;
    }
    err_t err = pbuf_copy_partial_pbuf(report_pbuf, p, sizeof(struct iperf_udp_header), 0);
    if (err != ERR_OK)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("Failed to retrieve header.\n"));
        goto report_cleanup;
    }

    iperf_populate_udp_server_report(&server_state->base, &report);

    err = pbuf_take_at(report_pbuf, &report, sizeof(report),
                       sizeof(struct iperf_udp_header));

    if (err != ERR_OK)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("Failed to add report to pbuf.\n"));
        goto report_cleanup;
    }

    err = udp_sendto(pcb, report_pbuf, addr, port);
    if (err != ERR_OK)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("Failed to send Iperf Server Report.\n"));
    }

report_cleanup:
    if (report_pbuf != NULL)
    {
        pbuf_free(report_pbuf);
    }
}

static void iperf_udp_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                  const ip_addr_t *addr, uint16_t port)
{
    struct iperf_server_state_udp *server_state = (struct iperf_server_state_udp *)arg;

    LWIP_ASSERT("NULL packet", p != NULL);

    udp_header_t hdr = { 0 };

    uint16_t copy_len = pbuf_copy_partial(p, &hdr, sizeof(hdr), 0);
    if (copy_len != sizeof(hdr))
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("Dropping too short packet\n"));
        goto cleanup;
    }

    struct iperf_settings settings = { 0 };
    copy_len = pbuf_copy_partial(p, &settings, sizeof(settings), sizeof(hdr));
    if (copy_len != sizeof(settings))
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("Dropping too short packet\n"));
        goto cleanup;
    }

    struct timeval packet_time = {
        .tv_sec = ntohl(hdr.tv_sec),
        .tv_usec = ntohl(hdr.tv_usec),
    };
    int64_t packet_id = 0;
    bool final_packet = false;
    struct iperf_server_session_udp *session = &server_state->session;

    if (server_state->args.version == IPERF_VERSION_2_0_9)
    {
        packet_id = (int64_t)((int32_t)ntohl(hdr.id_lo));
    }
    else
    {
        packet_id = (int64_t)(((uint64_t)ntohl(hdr.id_hi) << 32) | (uint64_t)ntohl(hdr.id_lo));
    }

    /* A negative packet ID indicates that this is the final packet. */
    if (packet_id < 0)
    {
        final_packet = true;
        packet_id = -packet_id;
    }

    session = get_session(server_state, addr, port);
    if (session == NULL)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("Another session already in progress\n"));
        goto cleanup;
    }

    /* next_packet_id < 0 indicates that we have already received the final frame from the client
     * so we should not update our session state. However, we can still send responses. */
    if (session->next_packet_id >= 0)
    {
        server_state->base.last_rx_time_ms = mmosal_get_time_ms();
        server_state->base.report.bytes_transferred += p->tot_len;
        server_state->base.report.rx_frames++;
        server_state->base.report.ipg_count++;
        server_state->base.report.ipg_sum_ms += time_delta(&packet_time, &(session->ipg_start));
        session->ipg_start = packet_time;

        if (packet_id < session->next_packet_id)
        {
            server_state->base.report.out_of_sequence_frames++;
        }
        else if (packet_id > session->next_packet_id)
        {
            server_state->base.report.error_count += packet_id - session->next_packet_id;
        }

        if (packet_id >= session->next_packet_id)
        {
            session->next_packet_id = packet_id + 1;
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

        iperf_udp_server_handle_final_packet(server_state, pcb, addr, port, p);
    }

cleanup:
    if (p != NULL)
    {
        pbuf_free(p);
    }
}

mmiperf_handle_t mmiperf_start_udp_server(const struct mmiperf_server_args *args)
{
    LOCK_TCPIP_CORE();

    err_t err;
    struct udp_pcb *pcb;
    struct iperf_server_state_udp *s;
    mmiperf_handle_t result = NULL;

    LWIP_ASSERT_CORE_LOCKED();

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
    s->args.local_port = args->local_port;
    s->args.version = args->version;
    /* Set next_packet_id to -1 to show that there is no session active. We will start a new
     * session with the first packet we receive from a client. */
    s->session.next_packet_id = -1;
    s->base.report.report_type = MMIPERF_INTERRIM_REPORT;

    s->args.local_addr = *(IP_ADDR_ANY);
    if (args->local_addr[0] != '\0')
    {
        int ok = ipaddr_aton(args->local_addr, &s->args.local_addr);
        if (!ok)
        {
            LWIP_DEBUGF(LWIP_DBG_LEVEL_SERIOUS,
                        ("Unable to parse local_addr as IP address (%s)\n", args->local_addr));
            goto exit;
        }
    }

    pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == NULL)
    {
        goto exit;
    }

    err = udp_bind(pcb, &s->args.local_addr, args->local_port);
    if (err != ERR_OK)
    {
        goto exit;
    }
#if LWIP_IPV4 && LWIP_IGMP
    if (IP_IS_V4(&s->args.local_addr) && ip_addr_ismulticast(&s->args.local_addr))
    {
        const ip_addr_t ifaddr = IPADDR4_INIT(IPADDR_ANY);
        igmp_joingroup(ip_2_ip4(&ifaddr), ip_2_ip4(&s->args.local_addr));
    }
#endif
#if LWIP_IPV6 && LWIP_IGMP
    if (IP_IS_V6(&s->args.local_addr) && ip_addr_ismulticast(&s->args.local_addr))
    {
        mld6_joingroup(IP6_ADDR_ANY6, ip_2_ip6(&s->args.local_addr));
    }
#endif

    udp_recv(pcb, iperf_udp_server_recv, s);

    s->pcb = pcb;

    iperf_list_add(&s->base);
    result = &(s->base);
    s = NULL;

exit:
    if (s != NULL)
    {
        IPERF_FREE(struct iperf_server_state_udp, s);
    }
    UNLOCK_TCPIP_CORE();
    return result;
}

static err_t iperf_udp_client_send_packet(struct iperf_client_state_udp *session,
                                          uint32_t tx_amount, bool final)
{
    udp_header_t *udp_hdr;
    struct iperf_settings *settings;
    uint32_t hdrs_len = sizeof(*udp_hdr) + sizeof(*settings);

    if (session->args.version == IPERF_VERSION_2_0_9)
    {
        hdrs_len = (hdrs_len - sizeof(uint32_t));
    }

    struct pbuf *hdrs_pbuf = pbuf_alloc(PBUF_TRANSPORT, hdrs_len, PBUF_POOL);
    if (hdrs_pbuf == NULL)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("iperf UDP tx failed to alloc hdrs\n"));
        return ERR_MEM;
    }

    /* Ensure we got allocated the right length and not chained pbufs */
    if (hdrs_pbuf->len != hdrs_len)
    {
        LWIP_PLATFORM_ASSERT("pbuf length mismatch");
    }

    int64_t datagrams_cnt = session->base.report.tx_frames;
    if (final)
    {
        datagrams_cnt = -datagrams_cnt;
    }

    udp_hdr = (udp_header_t *)hdrs_pbuf->payload;
    if (session->args.version == IPERF_VERSION_2_0_9)
    {
        udp_hdr->id_lo = htonl((uint32_t)datagrams_cnt);
    }
    else
    {
        udp_hdr->id_lo = htonl((uint32_t)((uint64_t)datagrams_cnt));
        udp_hdr->id_hi = htonl((uint32_t)(((uint64_t)datagrams_cnt) >> 32));
    }
    uint32_t now = sys_now();
    udp_hdr->tv_usec = htonl((now % 1000) * 1000);
    udp_hdr->tv_sec = htonl(now / 1000);

    settings = (struct iperf_settings *)(udp_hdr + 1);
    memset(settings, 0, sizeof(*settings));

    uint32_t payload_len = 0;
    if (tx_amount > hdrs_len)
    {
        payload_len = tx_amount - hdrs_len;
    }

    struct pbuf *payload_pbuf = iperf_get_data_pbuf(0, payload_len);
    if (payload_pbuf == NULL)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("pbuf allocation failed\n"));
        pbuf_free(hdrs_pbuf);
        return ERR_MEM;
    }

    pbuf_cat(hdrs_pbuf, payload_pbuf);
    payload_pbuf = NULL;

    LOCK_TCPIP_CORE();
    err_t err = udp_sendto(session->pcb, hdrs_pbuf,
                           &(session->server_addr), session->args.server_port);
    UNLOCK_TCPIP_CORE();
    pbuf_free(hdrs_pbuf);
    if (err != ERR_OK)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("iperf UDP tx failed to send (err=%d)\n", err));
        return err;
    }

    return ERR_OK;
}

static void iperf_udp_client_report(struct iperf_client_state_udp *session,
                                    uint32_t *final_duration_ms)
{
    struct iperf_udp_header hdr;
    struct iperf_udp_server_report report;

    uint16_t copy_len = pbuf_copy_partial(session->report, &hdr, sizeof(hdr), 0);
    if (copy_len != sizeof(hdr))
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("iperf UDP received report too short\n"));
        return;
    }

    copy_len = pbuf_copy_partial(session->report, &report, sizeof(report), sizeof(hdr));
    if (copy_len != sizeof(report))
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("iperf UDP received report too short\n"));
        return;
    }

    iperf_parse_udp_server_report(&session->base, &hdr, &report, session->args.version);
    *final_duration_ms = session->base.report.duration_ms;
}

static void iperf_udp_client_task(void *arg)
{
    struct iperf_client_state_udp *session = (struct iperf_client_state_udp *)arg;

    uint32_t end_time = UINT32_MAX;
    uint64_t remaining_amount = UINT64_MAX;

    /* A negative amount means it is a time (in hundredths of seconds), a postive amount is
     * number of bytes. */
    if (session->args.amount < 0)
    {
        end_time = session->base.time_started_ms + (-(session->args.amount) * 10);
    }
    else
    {
        remaining_amount = session->args.amount;
    }

    uint32_t tx_amount = 0;
    bool final = false;
    unsigned failure_cnt = 0;

    /* if no input for bandwidth limit, set bw_limit flag to false */
    bool bw_limit = true;
    if (session->args.target_bw == 0)
    {
        bw_limit = false;
    }
    uint32_t block_end_time = sys_now() + BLOCK_DURATION_MS;
    uint32_t block_remaining_tx_amount = session->block_tx_amount;

    const char *result;

    result = ipaddr_ntoa_r(&session->pcb->local_ip,
                           session->base.report.local_addr,
                           sizeof(session->base.report.local_addr));
    session->base.report.local_port = session->pcb->local_port;
    LWIP_ASSERT("IP buf too short", result != NULL);
    mmosal_safer_strcpy(session->base.report.remote_addr, session->args.server_addr,
                        sizeof(session->base.report.remote_addr));
    session->base.report.remote_port = session->args.server_port;

    while (!final && failure_cnt < IPERF_UDP_CLIENT_MAX_CONSEC_FAILURES)
    {
        /* If this is the last packet then set the counter to negative to inform the other side. */
        if (sys_now() > end_time || remaining_amount <= (uint64_t)session->args.packet_size ||
            session->base.report.tx_frames >= UINT32_MAX - 10)
        {
            final = true;
            session->awaiting_report = true;
        }
        tx_amount = min(remaining_amount, session->args.packet_size);

        /* when bw_limit is set to false, always send packets without check block parameter */
        if (bw_limit && block_end_time < sys_now())
        {
            block_end_time = sys_now() + BLOCK_DURATION_MS;
            block_remaining_tx_amount += session->block_tx_amount;
        }
        if (!bw_limit || block_remaining_tx_amount >= tx_amount || sys_now() > end_time)
        {
            err_t err = iperf_udp_client_send_packet(session, tx_amount, final);

            if (err == ERR_OK)
            {
                session->base.report.bytes_transferred += tx_amount;
                session->base.report.tx_frames++;
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
    /* Wait for status report from other end.  Use a binary semaphore to block us until
     * we receive report. */
    mmosal_semb_wait(session->report_semb, IPERF_UDP_CLIENT_REPORT_TIMEOUT_MS);
    if (!ip_addr_ismulticast(&(session->server_addr)))
    {
        unsigned ii;
        for (ii = 0; ii < IPERF_UDP_CLIENT_REPORT_RETRIES && session->report == NULL; ii++)
        {
            iperf_udp_client_send_packet(session, tx_amount, true);
            mmosal_semb_wait(session->report_semb, IPERF_UDP_CLIENT_REPORT_TIMEOUT_MS);
        }
    }

    uint32_t final_duration_ms = 0;
    if (session->report != NULL)
    {
        iperf_udp_client_report(session, &final_duration_ms);
        pbuf_free(session->report);
        session->report = NULL;
    }
    else
    {
        final_duration_ms = mmosal_get_time_ms() - session->base.time_started_ms;
        if (!ip_addr_ismulticast(&(session->server_addr)))
        {
            /* If we receive no response from the server in unicast mode then set
             * bytes_transferred to zero so it is obvious in the report. */
            session->base.report.bytes_transferred = 0;
        }
    }

    /* Clean up state and free allocated memory. */
    LOCK_TCPIP_CORE();
    udp_remove(session->pcb);
    UNLOCK_TCPIP_CORE();
    mmosal_semb_delete(session->report_semb);
    session->report_semb = NULL;
    session->pcb = NULL;
    iperf_list_remove(&(session->base));
    iperf_finalize_report_and_invoke_callback(&session->base, final_duration_ms,
                                              MMIPERF_UDP_DONE_CLIENT);
    IPERF_FREE(iperf_state_udp_t, session);
}

static void iperf_udp_client_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p,
                                  const ip_addr_t *addr, uint16_t port)
{
    (void)pcb;

    struct iperf_client_state_udp *session = (struct iperf_client_state_udp *)arg;

    if (!ip_addr_cmp_zoneless(addr, &(session->server_addr)))
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("UDP client rx from invalid address\n"));

        goto cleanup;
    }

    if (port != session->args.server_port)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("UDP client rx from invalid port\n"));
        goto cleanup;
    }

    if (!session->awaiting_report)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_ALL, ("UDP client rx unexpected\n"));
        goto cleanup;
    }

    if (session->report != NULL)
    {
        /* Report already received; we can drop this one. */
        goto cleanup;
    }

    session->report = p;
    p = NULL;

    mmosal_semb_give(session->report_semb);

cleanup:
    if (p != NULL)
    {
        pbuf_free(p);
    }
}

mmiperf_handle_t mmiperf_start_udp_client(const struct mmiperf_client_args *args)
{
    LOCK_TCPIP_CORE();

    struct udp_pcb *pcb;
    struct iperf_client_state_udp *s;
    mmiperf_handle_t result = NULL;
    int ok;
    uint32_t pkt_size = 0;
    err_t err = ERR_VAL;

    LWIP_ASSERT_CORE_LOCKED();

    s = (struct iperf_client_state_udp *)IPERF_ALLOC(struct iperf_client_state_udp);
    if (s == NULL)
    {
        goto exit;
    }

    memset(s, 0, sizeof(*s));
    memcpy(&(s->args), args, sizeof(s->args));

    ok = ipaddr_aton(args->server_addr, &s->server_addr);
    if (!ok)
    {
        goto exit;
    }

    if (s->args.server_port == 0)
    {
        s->args.server_port = MMIPERF_DEFAULT_PORT;
    }

    if (s->args.packet_size == 0)
    {
#if LWIP_IPV4
        if (IP_IS_V4(&s->server_addr))
        {
            s->args.packet_size = MMIPERF_DEFAULT_UDP_PACKET_SIZE_V4;
        }
#endif
#if LWIP_IPV6
        if (IP_IS_V6(&s->server_addr))
        {
            s->args.packet_size = MMIPERF_DEFAULT_UDP_PACKET_SIZE_V6;
        }
#endif
    }

    if (s->args.amount == 0)
    {
        s->args.amount = MMIPERF_DEFAULT_AMOUNT;
    }

    LWIP_DEBUGF(LWIP_DBG_LEVEL_ALL, ("Starting UDP iperf client to %s:%u, amount %ld\n",
                                     s->args.server_addr, s->args.server_port, args->amount));

    /* target_bw (kbps) convert to block_tx_amount (bytes) = bw * (BLOCK_DURATION_MS / 8) */
    s->block_tx_amount = s->args.target_bw * BLOCK_DURATION_MS / 8;
    /* check packet size. If the target_bw is too low, error message is printed. */
    pkt_size = s->args.target_bw * 1000 / 8;
    if (s->args.target_bw != 0 && s->args.packet_size > pkt_size)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_SERIOUS, ("bandwidth limit too low.\n"));
        s->pcb = NULL;
        goto exit;
    }

    /* We use a counter across a range of local ports so we don't use the same port for subsequent
     * iterations. */
    static atomic_uint session_counter = 0;
    (void)atomic_fetch_add(&session_counter, 1);
    s->local_port = IPERF_UDP_CLIENT_LOCAL_PORT_RANGE_BASE +
        (session_counter & (IPERF_UDP_CLIENT_LOCAL_PORT_RANGE_SIZE - 1));
    s->report_semb = mmosal_semb_create("iperf_udp");

    s->base.report.report_type = MMIPERF_INTERRIM_REPORT;
    s->base.time_started_ms = mmosal_get_time_ms();
    s->base.report_fn = args->report_fn;
    s->base.report_arg = args->report_arg;

    /* Create PCB to receive response from server */
    pcb = udp_new_ip_type(IPADDR_TYPE_ANY);
    if (pcb == NULL)
    {
        goto exit;
    }

#if LWIP_IPV4
    if (IP_IS_V4(&(s->server_addr)))
    {
        err = udp_bind(pcb, IP4_ADDR_ANY, s->local_port);
    }
#endif
#if LWIP_IPV6
    if (IP_IS_V6(&(s->server_addr)))
    {
        err = udp_bind(pcb, IP6_ADDR_ANY, s->local_port);
    }
#endif

    if (err != ERR_OK)
    {
        goto exit;
    }

    udp_recv(pcb, iperf_udp_client_recv, s);

    s->pcb = pcb;

    iperf_list_add(&s->base);

    s->task = mmosal_task_create(iperf_udp_client_task, s, MMOSAL_TASK_PRI_LOW,
                                 MMIPERF_STACK_SIZE, "iperf_udp");
    MMOSAL_ASSERT(s->task != NULL);
    result = &(s->base);
    s = NULL;

exit:
    if (s != NULL)
    {
        IPERF_FREE(struct iperf_client_state_udp, s);
    }
    UNLOCK_TCPIP_CORE();
    return result;
}

#endif
