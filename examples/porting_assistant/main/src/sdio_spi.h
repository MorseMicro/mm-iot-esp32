/*
 * Copyright 2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>

/**
 * Send an SDIO command to the transceiver and validate the response.
 *
 * @param cmd_idx   Command Index (e.g., 52 for CMD52).
 * @param arg       Argument for the command (i.e., the 32 bits following the command index).
 * @param rsp       If not NULL, then will be set to the first octet of the received response,
 *                  if applicable.
 *
 * @return Result of operation (0 or an error code from @ref mmhal_sdio_error_codes).
 */
int sdio_spi_send_cmd(uint8_t cmd_idx, uint32_t arg, uint32_t *rsp);

/**
 * Carry out the steps to read a le32 value from the transceiver at the specified address.
 *
 * @param address   Address to read from.
 * @param data      Reference to the location to store the data. This must not be NULL.
 *
 * @return Result of operation (0 or an error code from @ref mmhal_sdio_error_codes).
 */
int sdio_spi_read_le32(uint32_t address, uint32_t *data);

/**
 * Carry out the steps to write a large buffer to a specified address in the transceiver.
 *
 * @param address   Address to write to.
 * @param data      Data to write to the specified address.
 * @param len       Length of the data to write.
 *
 * @return Result of operation (0 or an error code from @ref mmhal_sdio_error_codes).
 */
int sdio_spi_write_multi_byte(uint32_t address, const uint8_t *data, uint32_t len);

/**
 * Carry out the steps to read a le32 value from the transceiver at the specified address.
 *
 * @param address   Address to read from.
 * @param value     The value to be written at specified address.
 *
 * @return Result of operation (0 or an error code from @ref mmhal_sdio_error_codes).
 */
int sdio_spi_write_le32(uint32_t address, uint32_t value);

/**
 * Function to modify specified bits in a register.
 *
 * @param address Address of the register to modify
 * @param mask Mask for bits to change (0 means not changing the bit)
 * @param value Value of the bits that are specified by Mask
 *
 * @return Result of operation (0 or an error code from @ref mmhal_sdio_error_codes).
 */
int sdio_spi_update_le32(uint32_t address, uint32_t mask, uint32_t value);

/**
 * Function to set specified bits in a specified address.
 *
 * @param address Address of the register to modify
 * @param mask Mask for bits to be set (0 means not changing the bit)
 *
 * @return Result of operation (0 or an error code from @ref mmhal_sdio_error_codes).
 */
static inline int sdio_spi_set_bits_le32(uint32_t address, uint32_t mask)
{
    return sdio_spi_update_le32(address, mask, mask);
}

/**
 * Function to clear specified bits in a specified address.
 *
 * @param address Address of the register to modify
 * @param mask Mask for bits to be cleared (0 means not changing the bit)
 *
 * @return Result of operation (0 or an error code from @ref mmhal_sdio_error_codes).
 */
static inline int sdio_spi_clear_bits_le32(uint32_t address, uint32_t mask)
{
    return sdio_spi_update_le32(address, mask, 0);
}


/**
 * Carry out the steps to read a large buffer from a specified address in the transceiver.
 *
 * @param address   Address to read from.
 * @param data      Buffer to write the data into.
 * @param len       Length of the data to read.
 *
 * @return Result of operation (0 or an error code from @ref mmhal_sdio_error_codes).
 */
int sdio_spi_read_multi_byte(uint32_t address, uint8_t *data, uint32_t len);
