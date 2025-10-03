/*
 * Copyright 2025 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "chip_cfg.h"
#include "sdio_spi.h"

#include "mmutils.h"


const uint32_t mm6108_valid_chip_ids[] = {
    0x206,
    0x306,
    0x406,
};


static int mm6108_gpio_set_oe(uint8_t gpio_num, bool oe)
{
    if (oe)
    {
        return sdio_spi_set_bits_le32(0x10012008, 1ul << gpio_num);
    }
    else
    {
        return sdio_spi_clear_bits_le32(0x10012008, 1ul << gpio_num);
    }
}

static int mm6108_gpio_set_value(uint8_t gpio_num, bool value)
{
    if (value)
    {
        return sdio_spi_set_bits_le32(0x1001200c, 1ul << gpio_num);
    }
    else
    {
        return sdio_spi_clear_bits_le32(0x1001200c, 1ul << gpio_num);
    }
}

const uint32_t mm8108_valid_chip_ids[] = {
    0x609,
    0x709,
    0x809,
};

static int mm8108_gpio_set_oe(uint8_t gpio_num, bool oe)
{
    if (oe)
    {
        return sdio_spi_write_le32(0x1360, 1ul << gpio_num);
    }
    else
    {
        return sdio_spi_write_le32(0x1364, 1ul << gpio_num);
    }
}

static int mm8108_gpio_set_value(uint8_t gpio_num, bool value)
{
    if (value)
    {
        return sdio_spi_write_le32(0x1368, 1ul << gpio_num);
    }
    else
    {
        return sdio_spi_write_le32(0x136c, 1ul << gpio_num);
    }
}

const struct chip_cfg chip_cfgs[] = {
    {
        .name = "mm6108",
        .reg_chip_id = 0x10054d20,
        .gpio_set_oe = mm6108_gpio_set_oe,
        .gpio_set_value = mm6108_gpio_set_value,
        .busy_gpio_num = 0,
        .valid_chip_ids = mm6108_valid_chip_ids,
        .n_valid_chip_ids = MM_ARRAY_COUNT(mm6108_valid_chip_ids),
    },
    {
        .name = "mm8108",
        .reg_chip_id = 0x00002d20,
        .gpio_set_oe = mm8108_gpio_set_oe,
        .gpio_set_value = mm8108_gpio_set_value,
        .busy_gpio_num = 2,
        .valid_chip_ids = mm8108_valid_chip_ids,
        .n_valid_chip_ids = MM_ARRAY_COUNT(mm8108_valid_chip_ids),
    },
};

const size_t n_chip_cfgs = MM_ARRAY_COUNT(chip_cfgs);
