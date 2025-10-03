/*
 * Copyright 2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "porting_assistant.h"
#include "sdio_spi.h"
#include "mmhal.h"
#include "mmlog.h"
#include "mmutils.h"
#include "chip_cfg.h"

/** Address used when measuring raw throughput */
#define BENCHMARK_ADDR_START      0x80100000

/** Packet length used for bulk read/write operations. Chosen because it is the approximate size of
 * a max data frame. */
#define BULK_RW_PACKET_LEN_BYTES (1496)

/** Duration to perform the benchmark over. This was arbitrarily chosen. */
#define BENCHMARK_WAIT_MS (2500)


/** The chip configuration as detected by probing the chip ID register. */
const struct chip_cfg *probed_chip_cfg;

/**
 * Function to validate if a given chip id is valid.
 *
 * @param chip_id Chip ID to validate
 *
 * @return @c true if the given id is in the list of valid ids, else @c false
 */
static bool validate_chip_id(uint32_t chip_id, const struct chip_cfg *chip_cfg)
{
    for (size_t ii = 0; ii < chip_cfg->n_valid_chip_ids; ii++)
    {
        if (chip_id == chip_cfg->valid_chip_ids[ii])
        {
            return true;
        }
    }

    return false;
}

/** This variable is going to be increased with each test interrupt. */
static volatile uint8_t irq_counter = 0;

/**
 * This Function is passed into wlan HAL to be called when busy pin is asserted.
 *
 */
static void test_hal_irq_handle(void)
{
    irq_counter++;
}

TEST_STEP(test_step_mmhal_wlan_init, "WLAN HAL initialisation")
{
    MM_UNUSED(log_buf);
    MM_UNUSED(log_buf_len);

    mmhal_wlan_init();

    /* We don't get indication of success or failure from mmhal_wlan_init() so we return
     * "no result". */
    return TEST_NO_RESULT;
}

TEST_STEP(test_step_mmhal_wlan_hard_reset, "Hard reset device")
{
    MM_UNUSED(log_buf);
    MM_UNUSED(log_buf_len);

    mmhal_wlan_hard_reset();

    /*
     * Since mmhal_wlan_hard_reset() does not return a status code we cannot verify here whether
     * the MM chip was reset successfully at this stage, so we return "no result". To validate
     * reset behavior an external logic analyzer can be used to probe the reset line.
     */
    return TEST_NO_RESULT;
}

TEST_STEP(test_step_mmhal_wlan_sdio_startup, "SDIO/SPI Startup")
{
    int ret;

    MM_UNUSED(log_buf);
    MM_UNUSED(log_buf_len);

    ret = mmhal_wlan_sdio_startup();

    switch (ret)
    {
    case 0:
        return TEST_PASSED;

    case MMHAL_SDIO_HW_ERROR:
        TEST_LOG_APPEND(
            "SDIO/SPI controller hardware error. Possible causes:\n"
            " - SDIO/SPI controller not configured correctly\n"
            " - SDIO/SPI controller clock not enabled\n\n");
        return TEST_FAILED;

    case MMHAL_SDIO_CMD_TIMEOUT:
        TEST_LOG_APPEND(
            "Timeout while executing an SDIO command. Possible causes:\n"
            " - SDIO/SPI pins not set to correct function (e.g., output instead of alternative)\n"
            " - SPI chip select not being asserted\n"
            " - MM chip not powered on\n\n");
        return TEST_FAILED;

    case MMHAL_SDIO_CMD_CRC_ERROR:
        TEST_LOG_APPEND(
            "CRC error in command or response while executing an SDIO command. Possible causes:\n"
            " - Noise on SPI/SDIO lines\n"
            " - Wrong SPI device selected\n\n");
        return TEST_FAILED;

    case MMHAL_SDIO_DATA_TIMEOUT:
        TEST_LOG_APPEND(
            "Timeout while transferring data. Possible causes:\n"
            " - Communication errors due to noise on SPI/SDIO lines\n"
            " - SPI/SDIO clock rate too low\n"
            " - SPI/SDIO data timeout to aggressive\n\n");
        return TEST_FAILED;

    case MMHAL_SDIO_DATA_UNDERFLOW:
        TEST_LOG_APPEND(
            "Data underflow. Possible causes:\n"
            " - DMA incorrectly configured\n"
            " - Data being fed into the SPI/SDIO controller FIFO too slowly\n\n");
        return TEST_FAILED;

    case MMHAL_SDIO_DATA_OVERRUN:
        TEST_LOG_APPEND(
            "Data overrun. Possible causes:\n"
            " - DMA incorrectly configured\n"
            " - Data being read from the SPI/SDIO controller FIFO too slowly\n\n");
        return TEST_FAILED;

    default:
        TEST_LOG_APPEND(
            "An unspecified error occurred. This may be due to communications or other isuses.\n"
            "Possible causes may include:\n"
            " - SDIO/SPI controller not configured correctly\n"
            " - Communication errors due to noise on SPI/SDIO lines\n\n");
        return TEST_FAILED;
    }
}

TEST_STEP(test_step_read_chip_id, "Read chip id from the MM chip")
{
    /*
     * This step is deceptively complicated. Reading the chip id using the SDIO over SPI protocol
     * requires a decent amount of setup. At a high-level the following gets executed:
     *
     * 1. The upper 16bits of the address is set into keyhole registers (relevant for CMD52/53)
     *    - This requires three (3) CMD52 writes to be achieved
     * 2. We execute a CMD53 read
     *    - We first write a CMD53 to the chip indicating that we want to read data and how much.
     *    - We then read out the amount of data requested plus a CRC which is used to validate
     *      the data integrity.
     *
     * This is glossing over the details but what we want to convey here is that sdio_spi_read_le32
     * is not just a read but a series of reads and writes.
     */

    int ret = MMHAL_SDIO_OTHER_ERROR;
    uint32_t data;
    int ii;
    size_t chip_cfg_idx;

    for (chip_cfg_idx = 0; chip_cfg_idx < n_chip_cfgs; chip_cfg_idx++)
    {
        /* MM chip requires few bytes to be written after CMD63 to get it to active state. We just
         * attempt to read the chip id a few times. */
        for (ii = 0; ii < 3; ii++)
        {
            ret = sdio_spi_read_le32(chip_cfgs[chip_cfg_idx].reg_chip_id, &data);
            if (ret == 0)
            {
                break;
            }
        }

        if (ret == 0)
        {
            if (validate_chip_id(data, &chip_cfgs[chip_cfg_idx]))
            {
                probed_chip_cfg = &chip_cfgs[chip_cfg_idx];
                return TEST_PASSED;
            }
            ret = MMHAL_SDIO_OTHER_ERROR;
        }
        else
        {
            break;
        }
    }

    switch (ret)
    {
    case MMHAL_SDIO_CMD_TIMEOUT:
        /* We shouldn't get this error code in this test, since it should have caused the
         * previous test to fail. */
        TEST_LOG_APPEND(
            "Failed to read chip id due to a command timeout. Possible causes:\n"
            " - SPI/SDIO controller not configured correctly\n"
            " - SPI/SDIO pins not set to correct function (e.g., output instead of alternative)\n"
            " - SPI Chip Select not being asserted (should be low during the transfer)\n"
            " - MM chip not powered on\n\n");
        break;

    case MMHAL_SDIO_CMD_CRC_ERROR:
        TEST_LOG_APPEND("Failed to validate CRC for recieved data. Possible causes:\n"
                        " - Error in reading data from SPI/SDIO peripheral\n"
                        " - Possible noise on the SPI/SDIO lines causing corruption\n\n");
        break;

    case MMHAL_SDIO_OTHER_ERROR:
        TEST_LOG_APPEND("Failed to match a valid chip ID\n");
        break;

    default:
        TEST_LOG_APPEND("Failed to read chip ID due to an unknown error\n\n");
        break;
    }

    return TEST_FAILED;
}

/**
 * Function to process return codes from @ref sdio_spi_write_multi_byte() and @ref
 * sdio_spi_read_multi_byte(). This attempts to give some hints as to what might be the cause of the
 * error.
 *
 * @param ret           Return code to process.
 * @param log_buf       Reference to the log buffer to append any messages.
 * @param log_buf_len   Length of the log buffer.
 *
 * @return @c true if it was a successful return code, else @c false for all other codes.
 */
bool process_sdio_spi_multi_byte_return(int ret, char *log_buf, size_t log_buf_len)
{
    switch (ret)
    {
    case 0:
        return true;
        break;

    case MMHAL_SDIO_CMD_TIMEOUT:
        /* We shouldn't get this error code in this test, since it should have caused the
         * previous test to fail. */
        TEST_LOG_APPEND(
            "Failed to read chip ID due to the timeout. Possible causes:\n"
            " - SPI/SDIO controller not configured correctly\n"
            " - SPI/SDIO pins not set to correct function (e.g., output instead of alternative)\n"
            " - SPI Chip Select not being asserted (should be low during the transfer)\n"
            " - MM chip not powered on\n\n");
        break;

    case MMHAL_SDIO_CMD_CRC_ERROR:
        TEST_LOG_APPEND("Failed to validate CRC for recieved data. Possible causes:\n"
                        " - Error in reading data from SPI peripheral\n"
                        " - Possible noise on the SPI lines causing corruption\n\n");
        break;

    case MMHAL_SDIO_INVALID_ARGUMENT:
        /* We should not reach this. */
        TEST_LOG_APPEND("Invalid input was given to sdio_spi_read_le32().\n"
                        "Likely a NULL pointer for the data variable\n\n");
        break;

    default:
        TEST_LOG_APPEND("Failed multi byte operation due to an unknown error\n\n");
    }

    return TEST_FAILED;
}

/**
 * Function to populate a buffer with a specific pattern.
 *
 * @param data      Reference to the buffer.
 * @param length    Length of the given buffer.
 */
void populate_buffer(uint8_t *data, uint32_t length)
{
    uint32_t cnt;
    for (cnt = 0; cnt < length; cnt++)
    {
        data[cnt] = cnt;
    }
}

/**
 * Function to validate the contents of a buffer matches what would be expected if @ref
 * populate_buffer() was called on it.
 *
 * @param data      Reference to the buffer.
 * @param length    Length of the given buffer.
 *
 * @return @c true if the contents matches the expected patter, else @c false
 */
bool valid_buffer(uint8_t *data, uint32_t length, uint32_t offset)
{
    uint8_t value = offset;
    uint32_t cnt;
    for (cnt = 0; cnt < length; cnt++)
    {
        if (data[cnt] != value++)
        {
            printf("\nInvalid data at %lu (offset=%lu, expect %02x got %02x)\n",
                   cnt, offset, value, data[cnt]);
            return false;
        }
    }
    return true;
}

TEST_STEP(test_step_bulk_write_read, "Bulk write/read into the MM chip")
{
    int ret = MMHAL_SDIO_OTHER_ERROR;
    bool ok;
    enum test_result result = TEST_PASSED;

    uint8_t *tx_data = (uint8_t *)mmosal_malloc(BULK_RW_PACKET_LEN_BYTES);
    uint8_t *rx_data = (uint8_t *)mmosal_malloc(BULK_RW_PACKET_LEN_BYTES);
    if ((tx_data == NULL) || (rx_data == NULL))
    {
        TEST_LOG_APPEND("Failed to allocate write/read buffers. Is there enough heap allocated?");
        result = TEST_FAILED;
        goto exit;
    }

    populate_buffer(tx_data, BULK_RW_PACKET_LEN_BYTES);

    ret = sdio_spi_write_multi_byte(BENCHMARK_ADDR_START, tx_data, BULK_RW_PACKET_LEN_BYTES);
    ok = process_sdio_spi_multi_byte_return(ret, log_buf, log_buf_len);
    if (!ok)
    {
        TEST_LOG_APPEND("Failure during sdio_spi_write_multi_byte\n");
        result = TEST_FAILED;
        goto exit;
    }

    ret = sdio_spi_read_multi_byte(BENCHMARK_ADDR_START, rx_data, BULK_RW_PACKET_LEN_BYTES);
    ok = process_sdio_spi_multi_byte_return(ret, log_buf, log_buf_len);
    if (!ok)
    {
        TEST_LOG_APPEND("Failure during sdio_spi_read_multi_byte\n");
        result = TEST_FAILED;
        goto exit;
    }

    if (!valid_buffer(rx_data, BULK_RW_PACKET_LEN_BYTES, 0))
    {
        TEST_LOG_APPEND("Data read from the MM chip does not match the data written.\n");
        result = TEST_FAILED;
        goto exit;
    }

exit:
    if (tx_data != NULL)
    {
        mmosal_free(tx_data);
    }

    if (rx_data != NULL)
    {
        mmosal_free(rx_data);
    }

    return result;
}


TEST_STEP(test_step_raw_tput, "Raw throughput test")
{
    /*
     * Please note that this test is intended to give some indication of the raw throughput that can
     * be achieved when transferring data across the bus to/from the MM chip. It serves more as an
     * upper limit for the WLAN throughput that can be achieved. The actual throughput that can be
     * achieved when transmitting will be lower than this. This is because there are additional
     * overheads that are not captured as part of this test.
     */
    int ret = MMHAL_SDIO_OTHER_ERROR;
    bool ok;
    enum test_result result = TEST_PASSED;
    unsigned offset = 0;
    uint32_t start_time;
    uint32_t end_time;
    uint32_t time_taken_ms;
    uint32_t benchmark_end_time;
    uint32_t transaction_count = 0;

    uint8_t *tx_data = (uint8_t *)mmosal_malloc(BULK_RW_PACKET_LEN_BYTES + 16);
    uint8_t *rx_data = (uint8_t *)mmosal_malloc(BULK_RW_PACKET_LEN_BYTES);
    if ((tx_data == NULL) || (rx_data == NULL))
    {
        TEST_LOG_APPEND("Failed to allocate write/read buffers. Is there enough heap allocated?");
        result = TEST_FAILED;
        goto exit;
    }

    populate_buffer(tx_data, BULK_RW_PACKET_LEN_BYTES + 16);

    start_time = mmosal_get_time_ms();
    benchmark_end_time = start_time + BENCHMARK_WAIT_MS;
    while (mmosal_time_le(mmosal_get_time_ms(), benchmark_end_time))
    {
        offset += 4;
        ret = sdio_spi_write_multi_byte(BENCHMARK_ADDR_START, tx_data + (offset & 15),
                                        BULK_RW_PACKET_LEN_BYTES);
        ok = process_sdio_spi_multi_byte_return(ret, log_buf, log_buf_len);
        if (!ok)
        {
            TEST_LOG_APPEND("Failure during sdio_spi_write_multi_byte\n");
            result = TEST_FAILED;
            goto exit;
        }

        ret = sdio_spi_read_multi_byte(BENCHMARK_ADDR_START, rx_data,
                                       BULK_RW_PACKET_LEN_BYTES);
        ok = process_sdio_spi_multi_byte_return(ret, log_buf, log_buf_len);
        if (!ok)
        {
            TEST_LOG_APPEND("Failure during sdio_spi_read_multi_byte\n");
            result = TEST_FAILED;
            goto exit;
        }

        transaction_count++;
    }
    end_time = mmosal_get_time_ms();

    /* We are only validating the contents of the buffer once because there are already checks in
     * place at the transport layer to validate the contents. This is in the form of CRCs. We just
     * perform this check for sanity's sake. */
    if (!valid_buffer(rx_data, BULK_RW_PACKET_LEN_BYTES, offset & 15))
    {
        TEST_LOG_APPEND("Data read from the MM chip does not match the data written.\n");
        result = TEST_FAILED;
        goto exit;
    }

    time_taken_ms = end_time - start_time;
    TEST_LOG_APPEND("Note: This will not be the final WLAN TPUT. See test step for more info.\n");
    TEST_LOG_APPEND("\tTime spent (ms):   %lu\n", time_taken_ms);
    TEST_LOG_APPEND("\tTransaction count: %lu\n", transaction_count);
    TEST_LOG_APPEND("\tBytes xferred:     %lu\n", transaction_count * 2 * BULK_RW_PACKET_LEN_BYTES);
    TEST_LOG_APPEND("\tRaw TPUT (kbit/s): %lu\n\n",
                    (transaction_count * 2 * BULK_RW_PACKET_LEN_BYTES * 8) / time_taken_ms);

exit:
    if (tx_data != NULL)
    {
        mmosal_free(tx_data);
    }

    if (rx_data != NULL)
    {
        mmosal_free(rx_data);
    }

    return result;
}

TEST_STEP(test_step_verify_busy_pin, "Verify BUSY pin")
{
    /* In this We toggle the BUSY pin on the chip and expect that we can see the GPIO input
     * on the host change and that the busy irq handler gets called. */
    enum test_result result = TEST_PASSED;
    uint8_t i = 0;
    probed_chip_cfg->gpio_set_oe(probed_chip_cfg->busy_gpio_num, true);
    probed_chip_cfg->gpio_set_value(probed_chip_cfg->busy_gpio_num, false);
    mmhal_wlan_register_busy_irq_handler(test_hal_irq_handle);

    mmhal_wlan_set_busy_irq_enabled(true);
    /* Clear counter after irq_enabled to ignore potential stale interrupt. */
    mmosal_task_sleep(1);
    irq_counter = 0;
    /* First toggle pin with IRQ enabled to verify the input value and irq handle call. */
    for (i = 0; i < 2; i++)
    {
        probed_chip_cfg->gpio_set_value(probed_chip_cfg->busy_gpio_num, true);
        mmosal_task_sleep(2);
        if (!mmhal_wlan_busy_is_asserted())
        {
            TEST_LOG_APPEND("BUSY pin set HIGH but mmhal_wlan_busy_is_asserted() returned false "
                            "(expected true)\n\n");
            result = TEST_FAILED;
            goto exit;
        }
        probed_chip_cfg->gpio_set_value(probed_chip_cfg->busy_gpio_num, false);
        mmosal_task_sleep(2);
        if (mmhal_wlan_busy_is_asserted())
        {
            TEST_LOG_APPEND("BUSY pin is set LOW but mmhal_wlan_busy_is_asserted() returned true "
                            "(expected false)\n\n");
            result = TEST_FAILED;
            goto exit;
        }
    }
    if (irq_counter != 2)
    {
        TEST_LOG_APPEND("BUSY pin IRQ hander was not called as expected. Expected 2 invocations, "
                        "but was invoked %u times\n\n",
                        irq_counter);
        result = TEST_FAILED;
        goto exit;
    }
    /* Now toggle the pin with IRQ disabled and make sure handler isn't called. */
    mmhal_wlan_set_busy_irq_enabled(false);
    for (i = 0; i < 2; i++)
    {
        probed_chip_cfg->gpio_set_value(probed_chip_cfg->busy_gpio_num, true);
        mmosal_task_sleep(2);
        probed_chip_cfg->gpio_set_value(probed_chip_cfg->busy_gpio_num, false);
        mmosal_task_sleep(2);
    }
    if (irq_counter > 2)
    {
      TEST_LOG_APPEND("Busy IRQ is still enabled.\n\n");
      result = TEST_FAILED;
      goto exit;
    }

exit:
    mmhal_wlan_set_busy_irq_enabled(false);
    mmhal_wlan_register_busy_irq_handler(NULL);
    irq_counter = 0;
    return result;
}
