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

#include "mmiperf_private.h"

/** List of active iperf sessions */
static struct mmiperf_state *iperf_all_connections;

void iperf_list_add(struct mmiperf_state *item)
{
    item->next = iperf_all_connections;
    iperf_all_connections = item;
}

void iperf_list_remove(struct mmiperf_state *item)
{
    struct mmiperf_state *prev = NULL;
    struct mmiperf_state *iter;
    for (iter = iperf_all_connections; iter != NULL; prev = iter, iter = iter->next)
    {
        if (iter == item)
        {
            if (prev == NULL)
            {
                iperf_all_connections = iter->next;
            }
            else
            {
                prev->next = iter->next;
            }
            /* ensure this item is listed only once */
            for (iter = iter->next; iter != NULL; iter = iter->next)
            {
                MMOSAL_ASSERT(iter != item);
            }
            break;
        }
    }
}

struct mmiperf_state *iperf_list_find(struct mmiperf_state *item)
{
    struct mmiperf_state *iter;
    for (iter = iperf_all_connections; iter != NULL; iter = iter->next)
    {
        if (iter == item)
        {
            return item;
        }
    }
    return NULL;
}

struct mmiperf_state *iperf_list_get(mmiperf_handle_t handle)
{
    struct mmiperf_state *iter;
    for (iter = iperf_all_connections; iter != NULL; iter = iter->next)
    {
        if (iter == handle)
        {
            return iter;
        }
    }
    return NULL;
}
