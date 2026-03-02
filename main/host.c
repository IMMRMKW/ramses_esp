
/********************************************************************
 * ramses_esp
 * host.c
 *
 * (C) 2023 Peter Price
 *
 * Host interface
 * Common interface between Radio and supported communication protocols
 *
 * Keeps track of active protocol(s) and manages release of message resources
 *
 */
static const char* TAG = "HOST";
#include "esp_log.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"

#include "cmd.h"
#include "gateway.h"
#include "ramses-mqtt.h"
#include <ramses_buttons.h>

#include "host.h"
#include "platform.h"

struct host_data {
    BaseType_t coreID;
    TaskHandle_t task;
    uint8_t platforms;
};

/*************************************************************************
 * Use FUNC button to trigger system restart
 */
#define RESTART_INTERVAL (CONFIG_RESTART_DELAY * 1000 / portTICK_PERIOD_MS)

static struct restart {
    StaticTimer_t timer;
    TimerHandle_t hTimer;
    uint8_t expired;
} restart;

void host_button_cb(struct button_event* event)
{
    static int8_t level = -1;

    if (event) {
        if (event->level != level) {
            switch (event->level) {
            case 0: // Button pressed
                xTimerStartFromISR(restart.hTimer, NULL);
                restart.expired = 0;
                break;

            default: // Button released
                xTimerStopFromISR(restart.hTimer, NULL);
                if (restart.expired) {
                    esp_restart();
                }
                break;
            }

            level = event->level;
        }
    }
}

static void restartTimerCB(TimerHandle_t xTimer)
{
    struct restart* restart = pvTimerGetTimerID(xTimer);
    if (!restart->expired) {
        restart->expired = 1;
        printf("Restarting...\n");
        fflush(stdout);
    }
}

static void enable_restart(void)
{
    restart.hTimer = xTimerCreateStatic("Restart", RESTART_INTERVAL, pdFALSE, &restart, restartTimerCB, &restart.timer);
}

/*************************************************************************
 * TASK
 */

static void Host_Task(void* param)
{
    struct host_data* ctxt = param;
    struct cmd_data* cmd_data;

    ESP_LOGI(TAG, "Task Started");

    // Basic console initialisation
    cmd_data = cmd_init();
    ramses_mqtt_init(ctxt->coreID);

    enable_restart();
    button_register(BUTTON_FUNC, host_button_cb);

    if (ctxt->platforms & PLATFORM_GW)
        gateway_init(ctxt->coreID);
    ESP_LOGI(TAG, "going to cmd work");
    cmd_work(cmd_data);
}

/*************************************************************************
 * External API
 */

static struct host_data* host_context(void)
{
    static struct host_data host;

    static struct host_data* ctxt = NULL;
    if (!ctxt) {
        ctxt = &host;
        // Initialisation?
    }

    return ctxt;
}

void Host_init(BaseType_t coreID, uint8_t platforms)
{
    struct host_data* ctxt = host_context();

    ctxt->coreID = coreID;
    ctxt->platforms = platforms;

    xTaskCreatePinnedToCore(Host_Task, "Host", 4096, ctxt, 10, &ctxt->task, ctxt->coreID);
}
