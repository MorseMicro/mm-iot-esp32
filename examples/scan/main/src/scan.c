/*
 * Copyright 2022-2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Example application to demonstrate using the MMWLAN scan subsystem.
 *
 * @note It is assumed that you have followed the steps in the @ref GETTING_STARTED guide and are
 * therefore familiar with how to build, flash, and monitor an application using the MM-IoT-SDK
 * framework.
 */

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

/*
 * If ASNI_ESCAPE_ENABLED is non-zero (the default) then ANSI escape characters will be used to
 *  format the log output.
 */
#if !(defined(ASNI_ESCAPE_ENABLED) && ASNI_ESCAPE_ENABLED == 0)
/** ANSI escape sequence for bold text. */
#define ANSI_BOLD  "\x1b[1m"
/** ANSI escape sequence to reset font. */
#define ANSI_RESET "\x1b[0m"
#else
/** ANSI escape sequence for bold text (disabled so no-op). */
#define ANSI_BOLD  ""
/** ANSI escape sequence to reset font (disabled so no-op). */
#define ANSI_RESET ""
#endif

/** Length of string representation of a MAC address (i.e., "XX:XX:XX:XX:XX:XX")
 * including null terminator. */
#define MAC_ADDR_STR_LEN    (18)

/** Number of results found. */
static int num_scan_results;

/** Enumeration of Authentication Key Management (AKM) Suite OUIs as BE32 integers. */
enum akm_suite_oui
{
    /** Open (no security) */
    AKM_SUITE_NONE = 0,
    /** Pre-shared key (WFA OUI) */
    AKM_SUITE_PSK = 0x506f9a02,
    /** Simultaneous Authentication of Equals (SAE) */
    AKM_SUITE_SAE = 0x000fac08,
    /** OWE */
    AKM_SUITE_OWE = 0x000fac12,
    /** Another suite not in this enum */
    AKM_SUITE_OTHER = 1,
};

/**
 * Get the name of the given AKM Suite as a string.
 *
 * @param akm_suite_oui     The OUI of the AKM suite as a big endian integer.
 *
 * @returns the string representation.
 */
const char *akm_suite_to_string(uint32_t akm_suite_oui)
{
    switch (akm_suite_oui)
    {
    case AKM_SUITE_NONE:
        return "None";

    case AKM_SUITE_PSK:
        return "PSK";

    case AKM_SUITE_SAE:
        return "SAE";

    case AKM_SUITE_OWE:
        return "OWE";

    default:
        return "Other";
    }
}


/** Maximum number of pairwise cipher suites our parser will process. */
#define RSN_INFORMATION_MAX_PAIRWISE_CIPHER_SUITES  (2)

/** Maximum number of AKM suites our parser will process. */
#define RSN_INFORMATION_MAX_AKM_SUITES  (2)


/**
 * Data structure to represent information extracted from an RSN information element.
 *
 * All integers in host order.
 */
struct rsn_information
{
    /** The group cipher suite OUI. */
    uint32_t group_cipher_suite;
    /** Pairwise cipher suite OUIs. Count given by @c num_pairwise_cipher_suites. */
    uint32_t pairwise_cipher_suites[RSN_INFORMATION_MAX_PAIRWISE_CIPHER_SUITES];
    /** AKM suite OUIs. Count given by @c num_akm_suites. */
    uint32_t akm_suites[RSN_INFORMATION_MAX_AKM_SUITES];
    /** Number of pairwise cipher suites in @c pairwise_cipher_suites. */
    uint16_t num_pairwise_cipher_suites;
    /** Number of AKM suites in @c akm_suites. */
    uint16_t num_akm_suites;
    /** Version number of the RSN IE. */
    uint16_t version;
    /** RSN Capabilities field of the RSN IE (in host order). */
    uint16_t rsn_capabilities;
};


/** Tag number of the RSN information element, in which we can find security details of the AP. */
#define RSN_INFORMATION_IE_TYPE (48)

/**
 * Search through the given list of information elements to find the RSN IE then parse it
 * to extract relevant information into an instance of @ref rsn_information.
 *
 * @param[in] ies       Buffer containing the information elements.
 * @param[in] ies_len   Length of @p ies
 * @param[out] output   Pointer to an instance of @ref rsn_information to receive output.
 *
 * @returns -1 on parse error, 0 if the RSN IE was not found, 1 if the RSN IE was found.
 */
static int parse_rsn_information(const uint8_t *ies, unsigned ies_len,
                                 struct rsn_information *output)
{
    size_t offset = 0;
    memset(output, 0, sizeof(*output));

    while (offset < ies_len)
    {
        uint8_t type = ies[offset++];
        uint8_t length = ies[offset++];

        if (type == RSN_INFORMATION_IE_TYPE)
        {
            uint16_t num_pairwise_cipher_suites;
            uint16_t num_akm_suites;
            uint16_t ii;


            if (offset + length > ies_len)
            {
                printf("*WRN* RSN IE extends past end of IEs\n");
                return -1;
            }

            if (length < 8)
            {
                printf("*WRN* RSN IE too short\n");
                return -1;
            }

            /* Skip version field */
            output->version = ies[offset] | ies[offset+1] << 8;
            offset += 2;
            length -= 2;

            output->group_cipher_suite =
                ies[offset] << 24 | ies[offset+1] << 16 | ies[offset+2] << 8 | ies[offset+3];
            offset += 4;
            length -= 4;

            num_pairwise_cipher_suites = ies[offset] | ies[offset+1] << 8;
            offset += 2;
            length -= 2;

            output->num_pairwise_cipher_suites = num_pairwise_cipher_suites;
            if (num_pairwise_cipher_suites > RSN_INFORMATION_MAX_PAIRWISE_CIPHER_SUITES)
            {
                output->num_pairwise_cipher_suites = RSN_INFORMATION_MAX_PAIRWISE_CIPHER_SUITES;
            }

            if (length < 4 * num_pairwise_cipher_suites + 2)
            {
                printf("*WRN* RSN IE too short\n");
                return -1;
            }

            for (ii = 0; ii < num_pairwise_cipher_suites; ii++)
            {
                if (ii < output->num_pairwise_cipher_suites)
                {
                    output->pairwise_cipher_suites[ii] =
                        ies[offset] << 24 | ies[offset+1] << 16 |
                        ies[offset+2] << 8 | ies[offset+3];
                }
                offset += 4;
                length -= 4;
            }

            num_akm_suites = ies[offset] | ies[offset+1] << 8;
            offset += 2;
            length -= 2;

            output->num_akm_suites = num_akm_suites;
            if (num_akm_suites > RSN_INFORMATION_MAX_AKM_SUITES)
            {
                output->num_akm_suites = RSN_INFORMATION_MAX_AKM_SUITES;
            }

            if (length < 4 * num_akm_suites + 2)
            {
                printf("*WRN* RSN IE too short\n");
                return -1;
            }

            for (ii = 0; ii < num_akm_suites; ii++)
            {
                if (ii < output->num_akm_suites)
                {
                    output->akm_suites[ii] =
                        ies[offset] << 24 | ies[offset+1] << 16 |
                        ies[offset+2] << 8 | ies[offset+3];
                }
                offset += 4;
                length -= 4;
            }

            output->rsn_capabilities = ies[offset] | ies[offset+1] << 8;
            return 1;
        }

        offset += length;
    }

    /* No RSE IE found; implies open security. */
    return 0;
}


/**
 * Scan rx callback.
 *
 * @param result        Pointer to the scan result.
 * @param arg           Opaque argument.
 */
static void scan_rx_callback(const struct mmwlan_scan_result *result, void *arg)
{
    (void)(arg);
    char bssid_str[MAC_ADDR_STR_LEN];
    char ssid_str[MMWLAN_SSID_MAXLEN];
    int ret;
    struct rsn_information rsn_info;

    num_scan_results++;
    snprintf(bssid_str, MAC_ADDR_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x",
             result->bssid[0], result->bssid[1], result->bssid[2], result->bssid[3],
             result->bssid[4], result->bssid[5]);
    snprintf(ssid_str, (result->ssid_len+1), "%s", result->ssid);

    printf(ANSI_BOLD "%2d. %s" ANSI_RESET "\n", num_scan_results, ssid_str);
    printf("    Operating BW: %u MHz\n",  result->op_bw_mhz);
    printf("    BSSID: %s\n", bssid_str);
    printf("    RSSI: %3d\n", result->rssi);
    printf("    Beacon Interval(TUs): %u\n", result->beacon_interval);
    printf("    Capability Info: 0x%04x\n", result->capability_info);

    ret = parse_rsn_information(result->ies, result->ies_len, &rsn_info);
    if (ret < 0)
    {
        printf("    Invalid probe response\n");
    }
    else if (rsn_info.num_akm_suites == 0)
    {
        printf("    Security: None\n");
    }
    else if (ret > 0)
    {
        unsigned ii;
        printf("    Security:");
        for (ii = 0; ii < rsn_info.num_akm_suites; ii++)
        {
            printf(" %s", akm_suite_to_string(rsn_info.akm_suites[ii]));
        }
        printf("\n");
    }
}

/**
 * Scan complete callback.
 *
 * @param state         Scan complete status.
 * @param arg           Opaque argument.
 */
static void scan_complete_callback(enum mmwlan_scan_state state, void *arg)
{
    (void)(state);
    (void)(arg);
    printf("Scanning completed.\n");
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
    const struct mmwlan_s1g_channel_list* channel_list;

    printf("\n\nMorse Scan Demo (Built "__DATE__ " " __TIME__ ")\n\n");

    /* Initialize Morse subsystems, note that they must be called in this order. */
    mmhal_init();
    mmwlan_init();

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

    /* Boot the WLAN interface so that we can retrieve the firmware version. */
    struct mmwlan_boot_args boot_args = MMWLAN_BOOT_ARGS_INIT;
    (void)mmwlan_boot(&boot_args);
    app_print_version_info();

    num_scan_results = 0;
    struct mmwlan_scan_req scan_req = MMWLAN_SCAN_REQ_INIT;
    scan_req.scan_rx_cb = scan_rx_callback;
    scan_req.scan_complete_cb = scan_complete_callback;
    status = mmwlan_scan_request(&scan_req);
    MMOSAL_ASSERT(status == MMWLAN_SUCCESS);
    printf("Scan started on %s channels, Waiting for results...\n", channel_list->country_code);
}
