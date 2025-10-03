/*
 * Copyright 2021-2024 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef MMIPERF_LWIP_H__
#define MMIPERF_LWIP_H__

#include "../common/mmiperf_private.h"
#include "lwip/pbuf.h"

/**
 * Get a pbuf containing an iperf payload.
 *
 * @param offset The numerical offset that the payload should start at.
 * @param len    Length of data to put into the pbuf.
 *
 * @returns a pbuf containing the data on success or @c NULL on failure.
 */
static inline struct pbuf *iperf_get_data_pbuf(size_t offset, size_t len)
{
    struct pbuf *pbuf = pbuf_alloc(PBUF_RAW, len, PBUF_ROM);
    if (pbuf == NULL)
    {
        LWIP_DEBUGF(LWIP_DBG_LEVEL_WARNING, ("iperf UDP tx failed to alloc payload\n"));
        return NULL;
    }

    ((struct pbuf_rom *)pbuf)->payload = iperf_get_data(offset);
    return pbuf;
}

#endif
