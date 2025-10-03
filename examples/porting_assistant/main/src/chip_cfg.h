/*
 * Copyright 2025 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


/** Chip configuration data structure. */
struct chip_cfg
{
    /** Chip name. */
    const char *name;
    /** Address of the chip ID register. */
    uint32_t reg_chip_id;
    /** Function to set GPIO output enable. */
    int (*gpio_set_oe)(uint8_t gpio_num, bool oe);
    /** Function to set GPIO value. */
    int (*gpio_set_value)(uint8_t gpio_num, bool value);
    /** Busy GPIO number. */
    uint8_t busy_gpio_num;
    /** List of valid chip IDs for this configuration. */
    const uint32_t *valid_chip_ids;
    /** Number of valid chip IDs in @c valid_chip_ids. */
    size_t n_valid_chip_ids;
};

/** Array of supported chip configurations. */
extern const struct chip_cfg chip_cfgs[];
/** Number of supported chip configurations in @c chip_cfgs. */
extern const size_t n_chip_cfgs;
