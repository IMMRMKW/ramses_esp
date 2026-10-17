#include "mqtt_update.h"
#include <string.h>
#include "cJSON.h"
#include "device.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "../ramses-network/ramses_ota.h"

static esp_mqtt_client_handle_t mqtt;
static char base[64], latest[32];
static TaskHandle_t checker;
static bool updating;
static int progress;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

static void publish_state(void)
{
    if (!mqtt) return;
    char version[32];
    bool active;
    int percent;
    portENTER_CRITICAL(&lock);
    memcpy(version, latest, sizeof(version));
    active = updating;
    percent = progress;
    portEXIT_CRITICAL(&lock);
    cJSON *state = cJSON_CreateObject();
    cJSON_AddStringToObject(state, "installed_version", esp_app_get_description()->version);
    if (version[0]) cJSON_AddStringToObject(state, "latest_version", version);
    cJSON_AddBoolToObject(state, "in_progress", active);
    if (active) cJSON_AddNumberToObject(state, "update_percentage", percent);
    char release[160];
    snprintf(release, sizeof(release), "https://github.com/IMMRMKW/ramses_esp/releases/tag/%s", version);
    cJSON_AddStringToObject(state, "release_url", release);
    char *payload = cJSON_PrintUnformatted(state);
    char topic[96];
    snprintf(topic, sizeof(topic), "%s/update/state", base);
    if (payload) esp_mqtt_client_enqueue(mqtt, topic, payload, 0, 1, 1, true);
    cJSON_free(payload);
    cJSON_Delete(state);
}

void mqtt_update_progress(bool active, int percent)
{
    portENTER_CRITICAL(&lock);
    bool changed = updating != active || progress != percent;
    updating = active;
    progress = percent;
    portEXIT_CRITICAL(&lock);
    if (changed) publish_state();
}

static void check_updates(void *unused)
{
    for (;;) {
        char buffer[2048];
        char manifest_url[256];
        snprintf(manifest_url, sizeof(manifest_url), "%s/latest/download/update.json", CONFIG_OTA_URL_SERVER);
        esp_http_client_config_t config = {
            .url = manifest_url,
            .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000,
        };
        esp_http_client_handle_t http = esp_http_client_init(&config);
        if (http) {
            esp_err_t err = esp_http_client_open(http, 0);
            if (err == ESP_OK) {
                esp_http_client_fetch_headers(http);
                /* Follow GitHub's release asset redirect using the HTTP client's redirect API. */
                int redirects = 0;
                while (esp_http_client_get_status_code(http) >= 300 &&
                       esp_http_client_get_status_code(http) < 400 && redirects++ < 5) {
                    esp_http_client_set_redirection(http);
                    esp_http_client_close(http);
                    err = esp_http_client_open(http, 0);
                    if (err != ESP_OK) break;
                    esp_http_client_fetch_headers(http);
                }
                int length = err == ESP_OK && esp_http_client_get_status_code(http) == 200 ?
                    esp_http_client_read_response(http, buffer, sizeof(buffer) - 1) : -1;
                if (length > 0 && length < sizeof(buffer) - 1) {
                    buffer[length] = 0;
                    cJSON *manifest = cJSON_Parse(buffer);
                    cJSON *version = cJSON_GetObjectItemCaseSensitive(manifest, "version");
                    cJSON *project = cJSON_GetObjectItemCaseSensitive(manifest, "project");
                    cJSON *target = cJSON_GetObjectItemCaseSensitive(manifest, "target");
                    cJSON *numeric = cJSON_GetObjectItemCaseSensitive(manifest, "file_version");
                    if (cJSON_IsString(version) && strlen(version->valuestring) < sizeof(latest) &&
                        cJSON_IsString(project) && !strcmp(project->valuestring, "ramses_esp_c6") &&
                        cJSON_IsString(target) && !strcmp(target->valuestring, "esp32c6")) {
                        bool safe = version->valuestring[0] == 'v';
                        for (const char *p = version->valuestring + 1; *p; ++p)
                            safe = safe && ((*p >= '0' && *p <= '9') || *p == '.');
                        if (safe && cJSON_IsNumber(numeric)) {
                            portENTER_CRITICAL(&lock);
                            strlcpy(latest, numeric->valuedouble <= RAMSES_OTA_FILE_VERSION ?
                                    esp_app_get_description()->version : version->valuestring, sizeof(latest));
                            portEXIT_CRITICAL(&lock);
                            publish_state();
                        }
                    }
                    cJSON_Delete(manifest);
                }
            }
            esp_http_client_cleanup(http);
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(6 * 60 * 60 * 1000));
    }
}

void mqtt_update_connected(esp_mqtt_client_handle_t client, const char *topic)
{
    mqtt = client;
    strlcpy(base, topic, sizeof(base));
    cJSON *config = cJSON_CreateObject();
    char unique[80], state[96], command[96];
    snprintf(unique, sizeof(unique), "ramses_esp_%s_firmware", device());
    snprintf(state, sizeof(state), "%s/update/state", base);
    snprintf(command, sizeof(command), "%s/update/install", base);
    cJSON_AddStringToObject(config, "unique_id", unique);
    cJSON_AddStringToObject(config, "name", "Firmware");
    cJSON_AddStringToObject(config, "device_class", "firmware");
    cJSON_AddStringToObject(config, "entity_category", "config");
    cJSON_AddStringToObject(config, "state_topic", state);
    cJSON_AddStringToObject(config, "command_topic", command);
    cJSON_AddStringToObject(config, "payload_install", "INSTALL");
    cJSON_AddStringToObject(config, "availability_topic", base);
    cJSON *dev = cJSON_AddObjectToObject(config, "device");
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(unique));
    cJSON_AddStringToObject(dev, "name", "RAMSES ESP");
    cJSON_AddStringToObject(dev, "manufacturer", "ELECRAM");
    cJSON_AddStringToObject(dev, "model", "Ramses_esp32c6");
    cJSON_AddStringToObject(dev, "sw_version", esp_app_get_description()->version);
    char discovery[128];
    /* Discovery topic object IDs allow only letters, digits, '_' and '-'.
     * Keep the stable unique_id while sanitizing the RF device ID's colon. */
    char object_id[sizeof(unique)];
    strlcpy(object_id, unique, sizeof(object_id));
    for (char *p = object_id; *p; ++p) {
        if (*p == ':') *p = '_';
    }
    snprintf(discovery, sizeof(discovery), "homeassistant/update/%s/config", object_id);
    char *payload = cJSON_PrintUnformatted(config);
    if (payload) esp_mqtt_client_enqueue(mqtt, discovery, payload, 0, 1, 1, true);
    cJSON_free(payload);
    cJSON_Delete(config);
    esp_mqtt_client_subscribe(mqtt, command, 1);
    publish_state();
    if (!checker) xTaskCreate(check_updates, "update_check", 8192, NULL, 3, &checker);
    else xTaskNotifyGive(checker);
}

bool mqtt_update_command(esp_mqtt_event_handle_t event)
{
    char command[96];
    snprintf(command, sizeof(command), "%s/update/install", base);
    if (!event->topic || event->topic_len != strlen(command) ||
        memcmp(event->topic, command, event->topic_len)) return false;
    if (!event->retain && event->current_data_offset == 0 && event->total_data_len == 7 &&
        event->data_len == 7 && !memcmp(event->data, "INSTALL", 7)) {
        char version[32];
        portENTER_CRITICAL(&lock);
        memcpy(version, latest, sizeof(version));
        portEXIT_CRITICAL(&lock);
        if (version[0] && strcmp(version, esp_app_get_description()->version)) {
            ota_set_url(CONFIG_OTA_URL_SERVER);
            ota_set_filename("ramses_esp_c6.bin");
            ota_set_version(version);
            ota_start(0);
        }
    }
    return true;
}
