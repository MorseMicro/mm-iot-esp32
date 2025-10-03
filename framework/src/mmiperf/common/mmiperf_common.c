/*
 * Copyright 2024 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <endian.h>

#include "mmiperf_private.h"

void iperf_finalize_report_and_invoke_callback(struct mmiperf_state *base_state,
                                               uint32_t duration_ms,
                                               enum mmiperf_report_type report_type)
{
    base_state->report.report_type = report_type;
    base_state->report.duration_ms = duration_ms;
    /* This shouldn't be possible in practice but, just in case, we clamp the duration
     * to be greater than or equal to zero. */
    if ((int32_t)duration_ms <= 0)
    {
        base_state->report.duration_ms = 0;
        base_state->report.bandwidth_kbitpsec = 0;
    }
    else
    {
        base_state->report.bandwidth_kbitpsec =
            base_state->report.bytes_transferred * 8 / duration_ms;
    }

    if (base_state->report_fn != NULL)
    {
        base_state->report_fn(&base_state->report, base_state->report_arg, base_state);
    }
}

bool mmiperf_get_interim_report(mmiperf_handle_t handle, struct mmiperf_report *report)
{
    struct mmiperf_state *base_state = iperf_list_get(handle);
    if (base_state == NULL)
    {
        return false;
    }

    /* There is a potential race condition here if the report is getting written at the same
     * time we read it, but it is relatively unlikely and should be minor in its impact. */
    memcpy(report, &base_state->report, sizeof(*report));

    /* Adjust duration and bandwidth values if the iperf session is still running, in case
     * it has been a while since the last time the report as updated. */
    if (report->report_type == MMIPERF_INTERRIM_REPORT)
    {
        report->duration_ms = mmosal_get_time_ms() - base_state->time_started_ms;
        /* This shouldn't be possible in practice but, just in case, we clamp the duration
         * to be greater than or equal to zero. */
        if ((int32_t)report->duration_ms <= 0)
        {
            report->duration_ms = 0;
            report->bandwidth_kbitpsec = 0;
        }
        else
        {
            report->bandwidth_kbitpsec = report->bytes_transferred * 8 / report->duration_ms;
        }
    }

    return true;
}

void iperf_populate_udp_server_report(struct mmiperf_state *base_state,
                                      struct iperf_udp_server_report *report)
{
    uint32_t duration_ms = base_state->last_rx_time_ms - base_state->time_started_ms;
    memset(report, 0, sizeof(*report));
    report->flags = htobe32(IPERF_HEADER_VERSION1);
    report->total_len1 = htobe32(base_state->report.bytes_transferred >> 32);
    report->total_len2 = htobe32(base_state->report.bytes_transferred & 0xFFFFFFFF);
    report->stop_sec = htobe32(duration_ms / 1000);
    report->stop_usec = htobe32((duration_ms % 1000) * 1000);
    report->error_cnt = htobe32(base_state->report.error_count);
    report->outorder_cnt = htobe32(base_state->report.out_of_sequence_frames);
    report->datagrams = htobe32(base_state->report.rx_frames);
    report->IPGcnt = htobe32(base_state->report.ipg_count);
    report->IPGsum = htobe32(base_state->report.ipg_sum_ms);
}

bool iperf_parse_udp_server_report(struct mmiperf_state *base_state,
                                   const struct iperf_udp_header *hdr,
                                   const struct iperf_udp_server_report *report,
                                   enum iperf_version version)
{
    MMOSAL_ASSERT(hdr != NULL);

    /* If the packet_id is not less than or equal to zero then this is not a report and
     * something went wrong. */
    if (version == IPERF_VERSION_2_0_9)
    {
        if ((int32_t)be32toh(hdr->id_lo) > 0)
        {
            return false;
        }
    }
    else
    {
        if ((int32_t)be32toh(hdr->id_hi) > 0)
        {
            return false;
        }
        if (hdr->id_hi == 0 && hdr->id_lo != 0)
        {
            return false;
        }
    }

    base_state->report.bytes_transferred = (uint64_t)be32toh(report->total_len1) << 32;
    base_state->report.bytes_transferred |= be32toh(report->total_len2);
    base_state->report.error_count = be32toh(report->error_cnt);
    base_state->report.out_of_sequence_frames = be32toh(report->outorder_cnt);
    base_state->report.rx_frames = be32toh(report->datagrams);
    base_state->report.duration_ms =
        be32toh(report->stop_sec) * 1000 + be32toh(report->stop_usec) / 1000;
    /* This will be calculated later. */
    base_state->report.bandwidth_kbitpsec = 0;
    return true;
}
