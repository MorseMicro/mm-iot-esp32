/*
 * Copyright 2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "porting_assistant.h"
#include "sdio_spi.h"
#include "mmhal.h"


/* Series of defines used when writing to the MM chip */
#define MORSE_REG_ADDRESS_BASE      0x10000
#define MORSE_REG_ADDRESS_WINDOW_0  MORSE_REG_ADDRESS_BASE
#define MORSE_REG_ADDRESS_WINDOW_1  (MORSE_REG_ADDRESS_BASE + 1)
#define MORSE_REG_ADDRESS_CONFIG    (MORSE_REG_ADDRESS_BASE + 2)

#define MORSE_CONFIG_ACCESS_1BYTE   0
#define MORSE_CONFIG_ACCESS_2BYTE   1
#define MORSE_CONFIG_ACCESS_4BYTE   2

/** PACK byte array to 16 bit data (little endian/LSB first) */
#define PACK_LE16(dst16_data, src8_array)                   \
    do {                                                    \
        dst16_data = *src8_array;                           \
        dst16_data |= ((uint16_t)*(src8_array+1) << 8);     \
    } while (0)

/** PACK byte array to 16 bit data (big endian/MSB first) */
#define PACK_BE16(dst16_data, src8_array)                   \
    do {                                                    \
        dst16_data = *(src8_array+1);                       \
        dst16_data |= ((uint16_t)*(src8_array) << 8);     \
    } while (0)

/** UNPACK 16 bit data to byte array (little endian/LSB first) */
#define UNPACK_LE16(dst8_array, src16_data)                 \
    do {                                                    \
        *dst8_array     = (uint8_t)(src16_data);            \
        *(dst8_array+1) = (uint8_t)(src16_data >> 8);       \
    } while (0)

/** UNPACK 16 bit data to byte array (big endian/MSB first) */
#define UNPACK_BE16(dst8_array, src16_data)                 \
    do {                                                    \
        *(dst8_array+1) = (uint8_t)(src16_data);            \
        *(dst8_array)   = (uint8_t)(src16_data >> 8);       \
    } while (0)

/** PACK byte array to 32 bit data (little endian/LSB first) */
#define PACK_LE32(dst32_data, src8_array)                   \
    do {                                                    \
        dst32_data = *src8_array;                           \
        dst32_data |= ((uint32_t)*(src8_array+1) << 8);     \
        dst32_data |= ((uint32_t)*(src8_array+2) << 16);    \
        dst32_data |= ((uint32_t)*(src8_array+3) << 24);    \
    } while (0)

/** PACK byte array to 32 bit data (big endian/MSB first) */
#define PACK_BE32(dst32_data, src8_array)                   \
    do {                                                    \
        dst32_data = *(src8_array+3);                       \
        dst32_data |= ((uint32_t)*(src8_array+2) << 8);     \
        dst32_data |= ((uint32_t)*(src8_array+1) << 16);    \
        dst32_data |= ((uint32_t)*(src8_array)   << 24);    \
    } while (0)

/** UNPACK 32 bit data to byte array (little endian/LSB first) */
#define UNPACK_LE32(dst8_array, src32_data)                 \
    do {                                                    \
        *dst8_array     = (uint8_t)(src32_data);            \
        *(dst8_array+1) = (uint8_t)(src32_data >> 8);       \
        *(dst8_array+2) = (uint8_t)(src32_data >> 16);      \
        *(dst8_array+3) = (uint8_t)(src32_data >> 24);      \
    } while (0)

/** UNPACK 32 bit data to byte array (big endian/MSB first) */
#define UNPACK_BE32(dst8_array, src32_data)                 \
    do {                                                    \
        *(dst8_array+3) = (uint8_t)(src32_data);            \
        *(dst8_array+2) = (uint8_t)(src32_data >> 8);       \
        *(dst8_array+1) = (uint8_t)(src32_data >> 16);      \
        *(dst8_array)   = (uint8_t)(src32_data >> 24);      \
    } while (0)

/** PACK byte array to 64 bit data (little endian/LSB first) */
#define PACK_LE64(dst64_data, src8_array)                   \
    do {                                                    \
        dst64_data  = ((uint64_t)*(src8_array+0) << 0);     \
        dst64_data |= ((uint64_t)*(src8_array+1) << 8);     \
        dst64_data |= ((uint64_t)*(src8_array+2) << 16);    \
        dst64_data |= ((uint64_t)*(src8_array+3) << 24);    \
        dst64_data |= ((uint64_t)*(src8_array+4) << 32);    \
        dst64_data |= ((uint64_t)*(src8_array+5) << 40);    \
        dst64_data |= ((uint64_t)*(src8_array+6) << 48);    \
        dst64_data |= ((uint64_t)*(src8_array+7) << 56);    \
    } while (0)

/** PACK byte array to 64 bit data (big endian/MSB first) */
#define PACK_BE64(dst64_data, src8_array)                   \
    do {                                                    \
        dst64_data  = ((uint64_t)*(src8_array+7) << 0);     \
        dst64_data |= ((uint64_t)*(src8_array+6) << 8);     \
        dst64_data |= ((uint64_t)*(src8_array+5) << 16);    \
        dst64_data |= ((uint64_t)*(src8_array+4) << 24);    \
        dst64_data |= ((uint64_t)*(src8_array+3) << 32);    \
        dst64_data |= ((uint64_t)*(src8_array+2) << 40);    \
        dst64_data |= ((uint64_t)*(src8_array+1) << 48);    \
        dst64_data |= ((uint64_t)*(src8_array+0) << 56);    \
    } while (0)

/** UNPACK 64 bit data to byte array (little endian/LSB first) */
#define UNPACK_LE64(dst8_array, src64_data)                 \
    do {                                                    \
        *(dst8_array+0) = (uint8_t)(src64_data >> 0);       \
        *(dst8_array+1) = (uint8_t)(src64_data >> 8);       \
        *(dst8_array+2) = (uint8_t)(src64_data >> 16);      \
        *(dst8_array+3) = (uint8_t)(src64_data >> 24);      \
        *(dst8_array+4) = (uint8_t)(src64_data >> 32);      \
        *(dst8_array+5) = (uint8_t)(src64_data >> 40);      \
        *(dst8_array+6) = (uint8_t)(src64_data >> 48);      \
        *(dst8_array+7) = (uint8_t)(src64_data >> 56);      \
    } while (0)

/** UNPACK 64 bit data to byte array (big endian/MSB first) */
#define UNPACK_BE64(dst8_array, src64_data)                 \
    do {                                                    \
        *(dst8_array+7) = (uint8_t)(src64_data >> 0);       \
        *(dst8_array+6) = (uint8_t)(src64_data >> 8);       \
        *(dst8_array+5) = (uint8_t)(src64_data >> 16);      \
        *(dst8_array+4) = (uint8_t)(src64_data >> 24);      \
        *(dst8_array+3) = (uint8_t)(src64_data >> 32);      \
        *(dst8_array+2) = (uint8_t)(src64_data >> 40);      \
        *(dst8_array+1) = (uint8_t)(src64_data >> 48);      \
        *(dst8_array+0) = (uint8_t)(src64_data >> 56);      \
    } while (0)


/* MAX blocks for single CMD53 read/write*/
#define CMD53_MAX_BLOCKS 128

enum block_size
{
    BLOCK_SIZE_FN1      = 8,
    BLOCK_SIZE_FN1_LOG2 = 3,
    BLOCK_SIZE_FN2      = 512,
    BLOCK_SIZE_FN2_LOG2 = 9,
};

enum max_block_transfer_size
{
    MAX_BLOCK_TRANSFER_SIZE_FN1 = BLOCK_SIZE_FN1 * CMD53_MAX_BLOCKS,
    MAX_BLOCK_TRANSFER_SIZE_FN2 = BLOCK_SIZE_FN2 * CMD53_MAX_BLOCKS,
};


/* MORSE set chip active for CMD62 and CMD63 */
#define CHIP_ACTIVE_SEQ  (0x00000000)
#define MAX_RETRY 3

/** Direction bit. */
enum sdio_direction
{
    SDIO_DIR_CARD_TO_HOST = 0,
    SDIO_DIR_HOST_TO_CARD = 1 << 6,
};

/**
 * Index to used to identify the command
 */
enum sdio_cmd_index
{
    /** GO_IDLE_STATE, used to change from SD to SPI mode. */
    SDIO_CMD0 = 0,
    /** IO_RW_DIRECT, used to read and write to a single register address. */
    SDIO_CMD52 = 52,
    /** IO_RW_EXTENDED, used to read and write to multiple register addresses with a
     *  single command. */
    SDIO_CMD53 = 53,
    /** Morse init with response, custom command to switch into SPI mode. */
    SDIO_CMD63 = 63,
};

/*
 * SDIO Card Common Control Register Flags, per SDIO Specification Version 4.10, Part E1,
 * Section 6.9.
 */

#define SDIO_CCCR_IEN_ADDR 0x04u
#define SDIO_CCCR_IEN_IENM (1u)
#define SDIO_CCCR_IEN_IEN1 (1u << 1)

#define SDIO_CCCR_BIC_ADDR 0x07u
#define SDIO_CCCR_BIC_ECSI (1u << 5)


static inline uint32_t min_u32(uint32_t a, uint32_t b)
{
    if (a < b)
    {
        return a;
    }
    else
    {
        return b;
    }
}

/**
 * @brief Perform a CMD52 write and validate the response.
 *
 * @param address   Destination address for the data.
 * @param data      Byte of data to write.
 * @param function  SDIO function used.
 *
 * @return Result of operation
 *
 * @note For more details see SDIO Specification Part E1, Section 5.1.
 */
static int morse_cmd52_write(uint32_t address, uint8_t data, enum mmhal_sdio_function function)
{
    uint32_t arg = mmhal_make_cmd52_arg(MMHAL_SDIO_WRITE, function, address, data);

    MMOSAL_ASSERT(address <= MMHAL_SDIO_ADDRESS_MAX);

    return sdio_spi_send_cmd(SDIO_CMD52, arg, NULL);
}

/**
 * @brief Uses SDIO CMD53 to read a given amount of data.
 *
 * @param function  SDIO function to use for the read operation. This will affect the size of
 *                  the blocks used to read the data.
 * @param address   The address to read from. The first two(2 bytes) will be discarded
 *                  @ref morse_address_base_set is used to set the upper bytes.
 * @param data      Data buffer to store the read data. Should at least have a size of len.
 * @param len       Length data to read in bytes.
 *
 * @return Result of operation
 *
 * @note See SDIO Specification Part E1, Section 5.3.
 */
static int morse_cmd53_read(enum mmhal_sdio_function function, uint32_t address,
                            uint8_t *data, uint32_t len)
{
    int result = -1;

    enum block_size block_size = BLOCK_SIZE_FN2;
    enum block_size block_size_log2 = BLOCK_SIZE_FN2_LOG2;

    if (function == MMHAL_SDIO_FUNCTION_1)
    {
        block_size = BLOCK_SIZE_FN1;
        block_size_log2 = BLOCK_SIZE_FN1_LOG2;
    }

    /* Attempt to read as many blocks as possible */
    uint16_t num_blocks = len >> block_size_log2;
    if (num_blocks > 0)
    {
        struct mmhal_wlan_sdio_cmd53_read_args args = {
            .sdio_arg = mmhal_make_cmd53_arg(MMHAL_SDIO_READ, function, MMHAL_SDIO_MODE_BLOCK,
                                             address & 0x0000ffff, num_blocks),
            .data = data,
            .transfer_length = num_blocks,
            .block_size = BLOCK_SIZE_FN2,
        };

        result = mmhal_wlan_sdio_cmd53_read(&args);
        if (result != 0)
        {
            goto exit;
        }

        uint32_t transfer_size = num_blocks * block_size;
        address += transfer_size;
        data += transfer_size;
        len -= transfer_size;
    }

    /* Now we use byte mode to read anything that was left over. */
    if (len > 0)
    {
        struct mmhal_wlan_sdio_cmd53_read_args args = {
            .sdio_arg = mmhal_make_cmd53_arg(MMHAL_SDIO_READ, function, MMHAL_SDIO_MODE_BYTE,
                                             address & 0x0000ffff, len),
            .data = data,
            .transfer_length = len,
            .block_size = 0,
        };

        result = mmhal_wlan_sdio_cmd53_read(&args);
    }

exit:
    return result;
}

/**
 * @brief Uses SDIO CMD53 to write a given amount of data.
 *
 * @param function  SDIO function to use for the write operation. This will affect the size of
 *                  the blocks used to write the data.
 * @param address   The address to write to. The first two(2 bytes) will be discarded
 *                  @ref morse_address_base_set is used to set the upper bytes.
 * @param data      Data buffer to write data from. Should at least have a size of len.
 * @param len       Length data to write in bytes.
 *
 * @return Result of operation
 *
 * @note See SDIO Specification Part E1, Section 5.3.
 */
static int morse_cmd53_write(enum mmhal_sdio_function function, uint32_t address,
                             const uint8_t *data, uint32_t len)
{
    int result = MMHAL_SDIO_OTHER_ERROR;

    enum block_size block_size = BLOCK_SIZE_FN2;
    enum block_size block_size_log2 = BLOCK_SIZE_FN2_LOG2;

    if (function == MMHAL_SDIO_FUNCTION_1)
    {
        block_size = BLOCK_SIZE_FN1;
        block_size_log2 = BLOCK_SIZE_FN1_LOG2;
    }

    /* Attempt to write as many blocks as possible */
    uint16_t num_blocks = len >> block_size_log2;
    if (num_blocks > 0)
    {
        struct mmhal_wlan_sdio_cmd53_write_args args = {
            .sdio_arg = mmhal_make_cmd53_arg(MMHAL_SDIO_WRITE, function, MMHAL_SDIO_MODE_BLOCK,
                                             address & 0x0000ffff, num_blocks),
            .data = (uint8_t *)data,
            .transfer_length = num_blocks,
            .block_size = BLOCK_SIZE_FN2,
        };

        result = mmhal_wlan_sdio_cmd53_write(&args);
        if (result != 0)
        {
            goto exit;
        }

        uint32_t transfer_size = num_blocks * block_size;
        address += transfer_size;
        data += transfer_size;
        len -= transfer_size;
    }

    /* Now we use byte mode to write anything that was left over. */
    if (len > 0)
    {
        struct mmhal_wlan_sdio_cmd53_write_args args = {
            .sdio_arg = mmhal_make_cmd53_arg(MMHAL_SDIO_WRITE, function, MMHAL_SDIO_MODE_BYTE,
                                             address & 0x0000ffff, len),
            .data = (uint8_t *)data,
            .transfer_length = len,
            .block_size = 0,
        };

        result = mmhal_wlan_sdio_cmd53_write(&args);
    }

exit:
    return result;
}

/**
 * @brief Writes to the keyhole registers that set upper 16 bits of addressed used by CMD52
 *        and CMD53 operations.
 *
 * @param address    The address value to set (the lower 16 bits will be ignored).
 * @param access     Access mode (one of @ref MORSE_CONFIG_ACCESS_1BYTE,
 *                   @ref MORSE_CONFIG_ACCESS_2BYTE, @ref MORSE_CONFIG_ACCESS_4BYTE).
 *
 * @return Result of operation
 */
static int morse_address_base_set(uint32_t address, uint8_t access,
                                  enum mmhal_sdio_function function)
{
    int result;

    address &= 0xFFFF0000;

    MMOSAL_ASSERT(access <= MORSE_CONFIG_ACCESS_4BYTE);

    result = morse_cmd52_write(MORSE_REG_ADDRESS_WINDOW_0, (uint8_t)(address >> 16),
                               function);
    if (result != 0)
    {
        goto exit;
    }

    result = morse_cmd52_write(MORSE_REG_ADDRESS_WINDOW_1, (uint8_t)(address >> 24),
                               function);
    if (result != 0)
    {
        goto exit;
    }

    result = morse_cmd52_write(MORSE_REG_ADDRESS_CONFIG, access, function);
    if (result != 0)
    {
        goto exit;
    }

exit:
    return result;
}

int sdio_spi_read_le32(uint32_t address, uint32_t *data)
{
    int result = -1;
    uint8_t receive_data[4];

    if (data == NULL)
    {
        return MMHAL_SDIO_INVALID_ARGUMENT;
    }

    enum mmhal_sdio_function function = MMHAL_SDIO_FUNCTION_1;

    result = morse_address_base_set(address, MORSE_CONFIG_ACCESS_4BYTE, function);
    if (result != 0)
    {
        goto exit;
    }

    result = morse_cmd53_read(function, address, receive_data, sizeof(receive_data));
    if (result != 0)
    {
        goto exit;
    }

    PACK_LE32(*data, receive_data);

exit:
    return result;
}

int sdio_spi_read_multi_byte(uint32_t address, uint8_t *data, uint32_t len)
{
    int result = -1;
    enum mmhal_sdio_function function = MMHAL_SDIO_FUNCTION_2;
    enum max_block_transfer_size max_transfer_size = MAX_BLOCK_TRANSFER_SIZE_FN2;

    /* Length must be a non-zero multiple of 4 */
    if (len == 0 || (len & 0x03) != 0)
    {
        printf("Invalid length %lu\n", len);
        result = MMHAL_SDIO_INVALID_ARGUMENT;
        goto exit;
    }

    /* Reads cannot cross 64K boundaries, so we may need to do several operations
     * to read the all the data. */
    while (len > 0)
    {
        result = morse_address_base_set(address, MORSE_CONFIG_ACCESS_4BYTE, function);
        if (result != 0)
        {
            goto exit;
        }

        /* We first calculate the number of bytes to transfer on this iteration of the loop. */
        uint32_t size = min_u32(len, max_transfer_size); // NOLINT(build/include_what_you_use)

        /* Read operations cannot cross the 64K boundary. We truncate the operation if this is
         * is the case. */
        uint32_t next_boundary = (address & 0xFFFF0000) + 0x10000;
        if ((address + size) > next_boundary)
        {
            size = next_boundary - address;
        }

        morse_cmd53_read(function, address, data, size);

        /*
         * Observed sometimes that SDIO read repeats the first 4-bytes word twice,
         * overwriting second word (hence, tail will be overwritten with 'sync' byte). When
         * this happens, reading will fetch the correct word.
         * NB: if repeated again, pass it anyway and upper layers will handle it
         */
        if ((size >= 8) && !memcmp(data, data+4, 4)) {
            /* Lets try one more time before passing up */
            printf("Corrupt Payload. Re-Read first 8 bytes\n");
            morse_cmd53_read(function, address, data, 8);
        }

        address += size;
        data += size;
        len -= size;
    }

exit:
    return result;
}

int sdio_spi_write_multi_byte(uint32_t address, const uint8_t *data, uint32_t len)
{
    int result = -1;
    enum mmhal_sdio_function function = MMHAL_SDIO_FUNCTION_2;
    enum max_block_transfer_size max_transfer_size = MAX_BLOCK_TRANSFER_SIZE_FN2;

    /* Length must be a non-zero multiple of 4 */
    if (len == 0 || (len & 0x03) != 0)
    {
        printf("Invalid length %lu\n", len);
        result = MMHAL_SDIO_INVALID_ARGUMENT;
        goto exit;
    }

    /* Writes cannot cross 64K boundaries, so we may need to do several operations
     * to write the all the given data. */
    while (len > 0)
    {
        result = morse_address_base_set(address, MORSE_CONFIG_ACCESS_4BYTE, function);
        if (result != 0)
        {
            goto exit;
        }

        /* We first calculate the number of bytes to transfer on this iteration of the loop. */
        uint32_t size = min_u32(len, max_transfer_size); // NOLINT(build/include_what_you_use)

        /* Write operations cannot cross the 64K boundary. We truncate the operation if this is
         * is the case. */
        uint32_t next_boundary = (address & 0xFFFF0000) + 0x10000;
        if ((address + size) > next_boundary)
        {
            size = next_boundary - address;
        }

        result = morse_cmd53_write(function, address, data, size);
        if (result != 0)
        {
            goto exit;
        }

        address += size;
        data += size;
        len -= size;
    }

exit:
    return result;
}

int sdio_spi_write_le32(uint32_t address, uint32_t data)
{
    int result = -1;
    enum mmhal_sdio_function function = MMHAL_SDIO_FUNCTION_2;

    result = morse_address_base_set(address, MORSE_CONFIG_ACCESS_4BYTE, function);
    if (result != 0)
    {
        goto exit;
    }

    result = morse_cmd53_write(function, address, (uint8_t*)&data, sizeof(data));

exit:
    return result;
}

int sdio_spi_send_cmd(uint8_t cmd_idx, uint32_t arg, uint32_t *rsp)
{
    return mmhal_wlan_sdio_cmd(cmd_idx, arg, rsp);
}

int sdio_spi_update_le32(uint32_t address, uint32_t mask, uint32_t value)
{
    int result = -1;
    uint32_t reg_value;
    result = sdio_spi_read_le32(address, &reg_value);
    if (result < 0)
    {
        return result;
    }
    reg_value |= (value & mask);
    reg_value &= (value | ~mask);
    result = sdio_spi_write_le32(address, reg_value);
    if (result < 0)
    {
        return result;
    }
    return result;
}
