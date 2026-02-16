/********************************************************************
 * ramses_esp
 * ramses_sntp.h
 *
 * (C) 2024 Peter Price
 *
 * RAMSES SNTP Client
 *
 */

#include <string.h>
#include <sys/time.h>
#include <time.h>

static const char* TAG = "SNTP";
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "ramses_sntp.h"
#include "ramses_wifi.h"

/****************************************************
 * States
 */
#define SNTP_STATE_LIST                   \
    SNTP_STATE(SNTP_IDLE, "Idle")         \
    SNTP_STATE(SNTP_WAIT, "Wait")         \
    SNTP_STATE(SNTP_START, "Start")       \
    SNTP_STATE(SNTP_STARTING, "Starting") \
    SNTP_STATE(SNTP_ACTIVE, "Active")

#define SNTP_STATE(_e, _t) _e,
enum sntp_state {
    SNTP_STATE_LIST
        SNTP_STATE_MAX
};
#undef SNTP_STATE

static char const* sntp_state_text(enum sntp_state state)
{
    static char const* const state_text[SNTP_STATE_MAX] = {
#define SNTP_STATE(_e, _t) _t,
        SNTP_STATE_LIST
#undef SNTP_STATE
    };

    char const* text = "Unknown";
    if (state < SNTP_STATE_MAX)
        text = state_text[state];

    return text;
}

/********************************************************
 * SNTP context data
 */
struct sntp_data {
    BaseType_t coreID;
    TaskHandle_t task;

    enum sntp_state state;

    char server[32];
};

static struct sntp_data* sntp_ctxt(void)
{
    static struct sntp_data data = {
        .server = "0.0.0.0"
    };

    static struct sntp_data* ctxt = NULL;
    if (!ctxt) {
        ctxt = &data;
        // Runtime initialisation?
    }

    return ctxt;
}

/************************************************************
 * SNTP task
 *
 */

static void sntp_set_state(struct sntp_data* ctxt, enum sntp_state newState)
{
    if (ctxt) {
        if (ctxt->state != newState) {
            ESP_LOGI(TAG, "state %s->%s ", sntp_state_text(ctxt->state), sntp_state_text(newState));
            ctxt->state = newState;
        }
    }
}

/**************************************************************/

static void time_sync_notification_cb(struct timeval* tv)
{
    struct sntp_data* ctxt = sntp_ctxt();
    ESP_LOGI(TAG, "Notification of a time synchronization event");
    sntp_set_state(ctxt, SNTP_ACTIVE);
}

/**************************************************************/

static void Sntp_init(char* server)
{
    ESP_LOGI(TAG, "Initializing");

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);

    config.start = false;
    config.sync_cb = time_sync_notification_cb; // only if we need the notification function

#if LWIP_DHCP_GET_NTP_SRV
    config.server_from_dhcp = true; // accept NTP offers from DHCP server, if any (need to enable *before* connecting)
    config.renew_servers_after_new_IP = true; // let esp-netif update configured SNTP server(s) after receiving DHCP lease
    config.index_of_first_server = 1; // updates from server num 1, leaving server 0 (from DHCP) intact
    config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
#endif /* LWIP_DHCP_GET_NTP_SRV */

    esp_netif_sntp_init(&config);
}

/**************************************************************/

static esp_err_t Sntp_start(void)
{
    ESP_LOGI(TAG, "Starting");

    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "failed to start SNTP (err=%d)", err);

    return err;
}

/**************************************************************/

static bool sntp_state_machine(struct sntp_data* ctxt)
{
    bool terminate = false;
    esp_err_t err;

    switch (ctxt->state) {
    case SNTP_IDLE:
        Sntp_init(ctxt->server);
        sntp_set_state(ctxt, SNTP_WAIT);
        break;

    case SNTP_WAIT:
        if (wifi_is_connected())
            sntp_set_state(ctxt, SNTP_START);
        break;

    case SNTP_START:
        err = Sntp_start();
        if (err == ESP_OK)
            sntp_set_state(ctxt, SNTP_ACTIVE);
        break;

    case SNTP_STARTING:
        break;

    case SNTP_ACTIVE: {
        time_t now = 0;
        struct tm timeinfo = { 0 };
        char strftime_buf[64];

        time(&now);
        localtime_r(&now, &timeinfo);
        strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

        ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);
    }
        sntp_set_state(ctxt, SNTP_STARTING);
        break;

    default:
        terminate = true;
    }

    return terminate;
}

static void Sntp(void* param)
{
    struct sntp_data* ctxt = param;

    ESP_LOGI(TAG, "Task Started");

    while (!sntp_state_machine(ctxt))
        vTaskDelay(50 / portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "Task Exiting");
}

void ramses_sntp_init(BaseType_t coreID, char* server)
{
    struct sntp_data* ctxt = sntp_ctxt();
    ctxt->coreID = coreID;

    if (server && server[0] != '\0')
        snprintf(ctxt->server, sizeof(ctxt->server), "%s", server);

    ESP_LOGI(TAG, "Will connect to SNTP server name: %s", server);

    sntp_state_machine(ctxt);

    xTaskCreatePinnedToCore(Sntp, "SNTP", 4096, ctxt, 10, &ctxt->task, ctxt->coreID);
}
