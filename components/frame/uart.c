/********************************************************************
 * ramses_esp
 * uart.c
 *
 * (C) 2023 Peter Price
 *
 * Radio UART interface between frame and cc1101
 * Provides an RX interface using UART data
 * Provides a TX interface using the cc1101 TX fifo
 *
 */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include <driver/uart.h>
#include <hal/uart_hal.h>

static const char* TAG = "UART";
#include "esp_err.h"
#include "esp_log.h"

#include "frame.h"
#include "uart.h"

#include "ramses_debug.h"
#define DEBUG_UART(_i)  \
    do {                \
        if (_i)         \
            DEBUG2_ON;  \
        else            \
            DEBUG2_OFF; \
    } while (0)
#define DEBUG_DATA(_i)  \
    do {                \
        if (_i)         \
            DEBUG3_ON;  \
        else            \
            DEBUG3_OFF; \
    } while (0)

#if CONFIG_UART_NUM_0
static uart_port_t const uart_num
    = UART_NUM_0;
#elif CONFIG_UART_NUM_1
static uart_port_t const uart_num
    = UART_NUM_1;
#endif
static QueueHandle_t uartQ;

/*******************************************************
 * HW UART interface to cc1101
 */

/****************************************************************************/
enum uart_mode {
    uart_off,
    uart_rx,
    uart_tx

};

static enum uart_mode uartMode = uart_off;
static bool uart_rx_on = false;

/*******************************************************
 * UART off
 */

static void uart_work_off(void) { }

/*******************************************************
 * UART RX
 */

static void uart_rx_flush(void)
{
    UBaseType_t nMsg = uxQueueMessagesWaiting(uartQ);
    if (nMsg) {
        UBaseType_t i;
        for (i = 0; i < nMsg; i++) {
            uart_event_t event;
            xQueueReceive(uartQ, &event, portTICK_PERIOD_MS);
        }
    }

    uart_flush_input(uart_num);
}

//---------------------------------------------------------------------------------

static void rx_start(void)
{
    uart_flush_input(uart_num);
    uart_enable_rx_intr(uart_num);
    uart_rx_on = true;
}

//---------------------------------------------------------------------------------

static void rx_stop(void)
{
    uart_disable_rx_intr(uart_num);
    uart_rx_on = false;
}

//---------------------------------------------------------------------------------

static void uart_work_rx(void)
{
    uart_event_t event = { 0 };
    if (xQueueReceive(uartQ, &event, portTICK_PERIOD_MS)) {
        DEBUG_UART(1);
        if (event.type == UART_DATA && event.size > 0) {
            uint8_t dtmp[256];
            int i, n;
            n = uart_read_bytes(uart_num, dtmp, event.size, portTICK_PERIOD_MS / 10);
            for (i = 0; i < n; i++) {
                DEBUG_DATA(1);
                frame_rx_byte(dtmp[i]);
                DEBUG_DATA(0);
            }
        }
        DEBUG_UART(0);
    }
}

/*******************************************************
 * UART TX
 */

static void tx_start(void)
{
    uart_rx_flush();
}

//---------------------------------------------------------------------------------

static void tx_stop(void)
{
}

//---------------------------------------------------------------------------------

static void uart_work_tx(void)
{
    uart_rx_flush();
}

/***************************************************************************
****************************************************************************
** External interface
****************************************************************************
****************************************************************************/

void uart_rx_enable(void)
{
    tx_stop();
    rx_start();

    uartMode = uart_rx;
}

void uart_tx_enable(void)
{
    rx_stop();
    tx_start();

    uartMode = uart_tx;
}

void uart_disable(void)
{
    rx_stop();
    tx_stop();

    uartMode = uart_off;
}

void uart_work(void)
{
    switch (uartMode) {
    case uart_off:
        uart_work_off();
        break;
    case uart_rx:
        uart_work_rx();
        break;
    case uart_tx:
        uart_work_tx();
        break;
    }
}

void uart_init(void)
{
    static uart_config_t const uart_config = {
        .baud_rate = RADIO_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };

    static uart_intr_config_t const uintr_cfg = {
        .intr_enable_mask = UART_INTR_RXFIFO_FULL,
        .rxfifo_full_thresh = 1,
    };

    esp_log_level_set(TAG, CONFIG_UART_LOG_LEVEL);
    ESP_LOGI(TAG, "Attempt to output");
    ESP_LOGI(TAG, "%d", uart_num);
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, CONFIG_CC_GDO0_GPIO, CONFIG_CC_GDO2_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));

    // Install UART driver, and get the queue.
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 256, 0, 16, &uartQ, 0));

    ESP_ERROR_CHECK(uart_intr_config(UART_NUM_1, &uintr_cfg));

    ESP_LOGI(TAG, "initialised");
}
