/*
 * Copyright 2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "porting_assistant.h"
#include "mmhal.h"

#ifndef PACKED
#define PACKED __attribute__((packed))
#endif

/** Enumeration of TLV field types */
enum fw_tlv_types
{
    FIELD_TYPE_MAGIC = 0x8000,
    FIELD_TYPE_EOF   = 0x8f00,
};

/** Expected value of the magic field for a firmware image. */
#define MBIN_FW_MAGIC_NUMBER (0x57464d4d)
/** Expected value of the magic field for a BCF. */
#define MBIN_BCF_MAGIC_NUMBER (0x43424d4d)

/** TLV (type-length-value) header */
struct PACKED tlv_header
{
    /** Type code. */
    uint16_t type;
    /** Value length. */
    uint16_t len;
};

/** Max number of TLVs to iterate through in this test. We would not expect to find more than this
 *  number in a firmware or BCF file. */
#define MAX_TLVS    (50)

/** Clean up an robuf when it is no longer needed. */
static void robuf_cleanup(struct mmhal_robuf *robuf)
{
    if (robuf != NULL)
    {
        /* Free the robuf buffer */
        if (robuf->free_cb != NULL)
        {
            robuf->free_cb(robuf->free_arg);
        }
        memset(robuf, 0, sizeof(*robuf));
    }
}

typedef void (*file_read_fn_t)(uint32_t offset, uint32_t requested_len, struct mmhal_robuf *robuf);

static enum test_result execute_fw_bcf_test(file_read_fn_t file_read_fn,
                                            const char *file_read_fn_name, const char *type,
                                            uint32_t expected_magic_number,
                                            char *log_buf, size_t log_buf_len)
{
    uint32_t offset = 0;

    unsigned num_tlvs;
    for (num_tlvs = 0; num_tlvs < MAX_TLVS; num_tlvs++)
    {
        struct mmhal_robuf robuf = {0};
        struct tlv_header hdr;
        file_read_fn(offset, sizeof(hdr), &robuf);

        if (robuf.len == 0)
        {
            TEST_LOG_APPEND(
                    "%s invalid or ended too soon (EOF marker not found)\n"
                    "Check that you have provided a valid %s file and review your implementation\n"
                    "of %s().\n", type, type, file_read_fn_name);
            return TEST_FAILED_NON_CRITICAL;
        }

        if (robuf.buf == NULL)
        {
            TEST_LOG_APPEND("%s returned NULL buffer but non-zero length\n", file_read_fn_name);
            TEST_LOG_APPEND("Review your implementation of %s().\n", file_read_fn_name);
            return TEST_FAILED_NON_CRITICAL;
        }

        if (robuf.len < sizeof(hdr))
        {
            TEST_LOG_APPEND(
                "The length of data returned by %s() was too short\n"
                "%s() is required to return a minimum of "
                "MMWLAN_FW_BCF_MIN_READ_LENGTH (%u) bytes.\n",
                file_read_fn_name, file_read_fn_name, MMHAL_WLAN_FW_BCF_MIN_READ_LENGTH);
            return TEST_FAILED_NON_CRITICAL;
        }

        memcpy(&hdr, (struct tlv_header *)robuf.buf, sizeof(hdr));

        robuf_cleanup(&robuf);

        if (num_tlvs == 0 && hdr.type != FIELD_TYPE_MAGIC)
        {
            TEST_LOG_APPEND(
                "The firware was corrupt (did not start with a magic number). \n"
                "Possible causes include using invalid (e.g., outdated) firmware, or a bug in\n"
                "%s()\n", file_read_fn_name);
            return TEST_FAILED_NON_CRITICAL;
        }

        if (hdr.type == FIELD_TYPE_EOF)
        {
            return TEST_PASSED;
        }

        offset += sizeof(hdr);

        /* Read TLV data */
        uint32_t remaining_len = hdr.len;
        while (remaining_len > 0)
        {
            file_read_fn(offset, remaining_len, &robuf);
            if (robuf.len == 0)
            {
                TEST_LOG_APPEND(
                    "%s ended too soon.\n"
                    "Check that you have provided a valid firmware file and review "
                    "your implementation of\n%s().\n", type, file_read_fn_name);
                return TEST_FAILED_NON_CRITICAL;
            }

            if (robuf.len > remaining_len)
            {
                TEST_LOG_APPEND(
                    "The length of data returned by %s() was too great\n"
                    "%s() should not return more than `requested_len` bytes.\n",
                    file_read_fn_name, file_read_fn_name);
                return TEST_FAILED_NON_CRITICAL;
            }

            if (num_tlvs == 0)
            {
                if (robuf.len < sizeof(uint32_t))
                {
                    TEST_LOG_APPEND(
                        "The length of data returned by %s() was too short\n"
                        "%s() is required to return a minimum of "
                        "MMWLAN_FW_BCF_MIN_READ_LENGTH %u) bytes\n",
                        file_read_fn_name, file_read_fn_name, MMHAL_WLAN_FW_BCF_MIN_READ_LENGTH);
                    return TEST_FAILED_NON_CRITICAL;
                }

                uint32_t magic = (robuf.buf[0]) |
                                 (robuf.buf[1] << 8) |
                                 (robuf.buf[2] << 16) |
                                 (robuf.buf[3] << 24);
                if (magic != expected_magic_number)
                {
                    TEST_LOG_APPEND(
                        "The %s was corrupt (did not contain the correct magic number -- "
                        "expect 0x%08lx, got 0x%08lx).\n"
                        "This is likey caused by using an invalid (e.g., outdated) version.\n",
                        type, expected_magic_number, magic);
                    return TEST_FAILED_NON_CRITICAL;
                }
            }

            offset += robuf.len;
            remaining_len -= robuf.len;
            robuf_cleanup(&robuf);
        }
    }

    TEST_LOG_APPEND(
        "%s invalid or ended too soon (EOF marker not found after %u TLVs).\n"
        "Check that you have provided a valid %s file and review your implementation\n"
        "of %s().\n", type, num_tlvs, type, file_read_fn_name);
    return TEST_FAILED_NON_CRITICAL;
}

TEST_STEP(test_step_mmhal_wlan_validate_fw, "Validate MM firmware")
{
    return execute_fw_bcf_test(mmhal_wlan_read_fw_file, "mmhal_wlan_read_fw_file", "Firmware",
                               MBIN_FW_MAGIC_NUMBER, log_buf, log_buf_len);
}

TEST_STEP(test_step_mmhal_wlan_validate_bcf, "Validate BCF")
{
    return execute_fw_bcf_test(mmhal_wlan_read_bcf_file, "mmhal_wlan_read_bcf_file", "BCF",
                               MBIN_BCF_MAGIC_NUMBER, log_buf, log_buf_len);
}
