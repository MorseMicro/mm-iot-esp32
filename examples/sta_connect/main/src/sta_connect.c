/*
 * Copyright 2022-2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Example application to demonstrate MMWLAN API to connect to an AP.
 *
 * @note It is assumed that you have followed the steps in the @ref GETTING_STARTED guide and are
 * therefore familiar with how to build, flash, and monitor an application using the MM-IoT-SDK
 * framework.
 */

#include <endian.h>
#include <string.h>
#include "mmhal.h"
#include "mmosal.h"
#include "mmwlan.h"
#include "mm_app_regdb.h"

// #define COUNTRY_CODE "AU"
#ifndef COUNTRY_CODE
#error COUNTRY_CODE must be defined to the appropriate 2 character country code. \
       See mm_app_regdb.c for valid options.
#endif

/** SSID of the AP to connect to. */
#define SSID "MorseMicro"
/** Passphrase of the AP to connect to. Comment out for OWE. */
#define PASSPHRASE "12345678"

/**
 * Link state callback. This is typically used to signal state to the network stack.
 */
static void link_state_change_hanndler(enum mmwlan_link_state link_state, void *arg)
{
    printf("Link went %s\n", (link_state == MMWLAN_LINK_DOWN) ? "Down" : "Up");

    if (link_state == MMWLAN_LINK_UP)
    {
        struct mmosal_semb *link_up_semaphore = (struct mmosal_semb *)arg;
        bool ok = mmosal_semb_give(link_up_semaphore);
        if (!ok)
        {
            printf("Failed to give link_up_semaphore\n");
            MMOSAL_ASSERT(false);
        }
    }
}

/**
 * Receive callback. This is invoked when a packet is received, which is typically passed to
 * the network stack.
 */
static void rx_handler(uint8_t *header, unsigned header_len,
                       uint8_t *payload, unsigned payload_len,
                       void *arg)
{
    struct __attribute__((packed)) dot3_header
    {
        uint8_t dest_addr[6];
        uint8_t src_addr[6];
        uint16_t ethertype;
    };

    struct dot3_header *hdr = (struct dot3_header *)header;

    MMOSAL_ASSERT(sizeof(*hdr) == header_len);

    printf("RX from %02x:%02x:%02x:%02x:%02x:%02x type 0x%04x\n",
           hdr->src_addr[0], hdr->src_addr[1], hdr->src_addr[2],
           hdr->src_addr[3], hdr->src_addr[4], hdr->src_addr[5],
           be16toh(hdr->ethertype));
}

/**
 * STA status callback. This is typically used to signal STA state to the application (e.g.,
 * to update UI).
 */
static void sta_status_handler(enum mmwlan_sta_state sta_state)
{
    const char *sta_state_desc[] = {
        "DISABLED",
        "CONNECTING",
        "CONNECTED",
    };
    printf("STA state: %s (%u)\n", sta_state_desc[sta_state], sta_state);
}

void app_print_version_info(void)
{
    enum mmwlan_status status;
    struct mmwlan_version version = {0};
    struct mmwlan_bcf_metadata bcf_metadata = {0};

    printf("-----------------------------------\n");

    status = mmwlan_get_bcf_metadata(&bcf_metadata);
    if (status == MMWLAN_SUCCESS)
    {
        printf("  BCF API version:         %u.%u.%u\n",
               bcf_metadata.version.major, bcf_metadata.version.minor, bcf_metadata.version.patch);
        if (bcf_metadata.build_version[0] != '\0')
        {
            printf("  BCF build version:       %s\n", bcf_metadata.build_version);
        }
        if (bcf_metadata.board_desc[0] != '\0')
        {
            printf("  BCF board description:   %s\n", bcf_metadata.board_desc);
        }
    }
    else
    {
        printf("  !! BCF metadata retrival failed !!\n");
    }

    status = mmwlan_get_version(&version);
    if (status != MMWLAN_SUCCESS)
    {
        printf("  !! Error occured whilst retrieving version info !!\n");
    }
    printf("  Morselib version:        %s\n", version.morselib_version);
    printf("  Morse firmware version:  %s\n", version.morse_fw_version);
    printf("  Morse chip ID:           0x%04lx\n", version.morse_chip_id);
    printf("  Morse chip name:         %s\n", version.morse_chip_id_string);
    printf("-----------------------------------\n");

    MMOSAL_ASSERT(status == MMWLAN_SUCCESS);
}

/**
 * Main entry point to the application. This will be invoked in a thread once operating system
 * and hardware initialization has completed. It may return, but it does not have to.
 */
void app_main(void)
{
    enum mmwlan_status status;
    struct mmwlan_sta_args sta_args = MMWLAN_STA_ARGS_INIT;
    const struct mmwlan_s1g_channel_list* channel_list;
    uint8_t mac_addr[MMWLAN_MAC_ADDR_LEN];
    struct mmosal_semb *link_up_semaphore;
    bool ok;

    printf("\n\nMorse STA Demo (Built "__DATE__ " " __TIME__ ")\n\n");

    /* This semaphore is used by the link state callback to signal the link going up. We use
     * it in this main thread to block until the link goes up. */
    link_up_semaphore = mmosal_semb_create("link_up");
    MMOSAL_ASSERT(link_up_semaphore != NULL);

    /* Initialize subsystems, note that they must be called in this order. */
    mmhal_init();
    mmwlan_init();

    /* Load channel list. */
    channel_list = mmwlan_lookup_regulatory_domain(get_regulatory_db(), COUNTRY_CODE);
    if (channel_list == NULL)
    {
        printf("Could not find specified regulatory domain matching country code %s\n",
               COUNTRY_CODE);
        MMOSAL_ASSERT(false);
    }
    status = mmwlan_set_channel_list(channel_list);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to set country code %s\n", channel_list->country_code);
        MMOSAL_ASSERT(false);
    }

    /* Register callback to be invoked when the link goes up and down. We pass link_up_semaphore
     * as an opaque argument so that this can be used to signal link up. */
    status = mmwlan_register_link_state_cb(link_state_change_hanndler, link_up_semaphore);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to register %s callback\n", "link state");
        MMOSAL_ASSERT(false);
    }

    /* Register a callback to be invoked on receive. */
    status = mmwlan_register_rx_cb(rx_handler, NULL);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to register %s callback\n", "rx");
        MMOSAL_ASSERT(false);
    }

    /* Boot the WLAN interface so that we can retrieve the firmware version. */
    struct mmwlan_boot_args boot_args = MMWLAN_BOOT_ARGS_INIT;
    (void)mmwlan_boot(&boot_args);
    app_print_version_info();

    status = mmwlan_get_mac_addr(mac_addr);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to get MAC address\n");
        MMOSAL_ASSERT(false);
    }

    /* Set up STA arguments and start connection to AP. */
    sta_args.ssid_len = sizeof(SSID) - 1;
    memcpy(sta_args.ssid, SSID, sta_args.ssid_len);
#ifdef PASSPHRASE
    sta_args.passphrase_len = sizeof(PASSPHRASE) - 1;
    memcpy(sta_args.passphrase, PASSPHRASE, sta_args.passphrase_len);
    sta_args.security_type = MMWLAN_SAE;
#else
    /* We default to OWE if PASSPHRASE is not defined. Alternatively, MMWLAN_OPEN is provided
     * to completely disable security. */
    sta_args.security_type = MMWLAN_OWE;
#endif
    status = mmwlan_sta_enable(&sta_args, sta_status_handler);
    if (status != MMWLAN_SUCCESS)
    {
        printf("Failed to enable STA mode: status %d\n", status);
        MMOSAL_ASSERT(false);
    }

    /* Wait until the link comes up. */
    ok = mmosal_semb_wait(link_up_semaphore, UINT32_MAX);
    MMOSAL_ASSERT(ok);

    /* Send a packet. Note that this is just for demonstration purposes and normally this function
     * would be connected up to the IP stack (e.g., via a LWIP netif). */
    uint8_t arp_packet[] = {
        /* 802.3 header: dest_addr, source_addr, ethertype */
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5],
        0x08, 0x06,

        /* ARP payload */
        0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01,
        mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5],
        0xc0, 0xa8, 0x01, 0x02, /* Our IP address (192.168.1.2) */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xc0, 0xa8, 0x01, 0x01,  /* Target IP address (192.168.1.1) */
    };
    status = mmwlan_tx(arp_packet, sizeof(arp_packet));
    if (status != MMWLAN_SUCCESS)
    {
        printf("TX failed with status %d\n", status);
        MMOSAL_ASSERT(false);
    }
}
