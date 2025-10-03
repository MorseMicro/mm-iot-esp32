/**
 * Copyright 2024 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 * @file
 * This File implements platform specific shims for accessing the data-link transport.
 */

#include "mmhal_uart.h"
#include "mmutils.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#define UART_TXD (CONFIG_RF_TEST_UART_TXD)
#define UART_RXD (CONFIG_RF_TEST_UART_RXD)
#define UART_RTS (UART_PIN_NO_CHANGE)
#define UART_CTS (UART_PIN_NO_CHANGE)

#define UART_PORT_NUM        (CONFIG_RF_TEST_UART_PORT_NUM)
#define UART_BAUD_RATE       (CONFIG_RF_TEST_UART_BAUD_RATE)
#define UART_BUF_SIZE        (1024)


/** Size of the receive ring buffer. */
#define RX_RINGBUF_SIZE     (128)
/** Add the given value to a RX ringbuf index, taking into account wrapping. */
#define RX_RINGBUF_IDX_ADD(_x, _y) (((_x) + (_y)) % RX_RINGBUF_SIZE)

/** RX thread stack size in 32 bit words */
#define RX_THREAD_STACK_SIZE_WORDS  (768)
/** RX thread priority */
#define RX_THREAD_PRIORITY          (MMOSAL_TASK_PRI_NORM)

/** Data structure UART HAL global state. */
struct mmhal_uart_data
{
    mmhal_uart_rx_cb_t rx_cb;
    void *rx_cb_arg;

    struct
    {
        struct mmosal_task *handle;
        volatile bool run;
        volatile bool complete;
    } rx_thread;
    struct
    {
        uint8_t buf[RX_RINGBUF_SIZE];
        volatile size_t write_idx;
        volatile size_t read_idx;
    } rx_ringbuf;
};

static struct mmhal_uart_data mmhal_uart;

static void uart_rx_main(void *arg)
{
    uint8_t data[1];

    MM_UNUSED(arg);

    while (mmhal_uart.rx_thread.run)
    {
        int ret = uart_read_bytes(UART_PORT_NUM, data, sizeof(data), UINT32_MAX);
        if (ret > 0 && mmhal_uart.rx_cb != NULL)
        {
            mmhal_uart.rx_cb(data, ret, mmhal_uart.rx_cb_arg);
        }
    }

    mmhal_uart.rx_thread.complete = true;
}

void mmhal_uart_init(mmhal_uart_rx_cb_t rx_cb, void *rx_cb_arg)
{
    memset(&mmhal_uart, 0, sizeof(mmhal_uart));
    mmhal_uart.rx_cb = rx_cb;
    mmhal_uart.rx_cb_arg = rx_cb_arg;

    /* Configure parameters of an UART driver,
     * communication pins and install the driver */
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

#if CONFIG_UART_ISR_IN_IRAM
    intr_alloc_flags = ESP_INTR_FLAG_IRAM;
#endif

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_BUF_SIZE * 2, 0, 0, NULL,
                                        intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_TXD, UART_RXD,
                                 UART_RTS, UART_CTS));

    mmhal_uart.rx_thread.run = true;
    mmhal_uart.rx_thread.handle = mmosal_task_create(
        uart_rx_main, NULL, RX_THREAD_PRIORITY, RX_THREAD_STACK_SIZE_WORDS, "uart");
}

void mmhal_uart_deinit(void)
{
    if (mmhal_uart.rx_thread.handle != NULL)
    {
        mmhal_uart.rx_thread.run = false;
        while (!mmhal_uart.rx_thread.complete)
        {
            mmosal_task_sleep(3);
        }
        mmhal_uart.rx_thread.handle = NULL;
    }
}

void mmhal_uart_tx(const uint8_t *tx_data, size_t length)
{
    while (length > 0)
    {
        int ret = uart_write_bytes(UART_PORT_NUM, (const char *)tx_data, length);
        MMOSAL_ASSERT(ret > 0 && ret <= length);
        tx_data += ret;
        length -= ret;
    }
}

bool mmhal_uart_set_deep_sleep_mode(enum mmhal_uart_deep_sleep_mode mode)
{
    return false;
}
