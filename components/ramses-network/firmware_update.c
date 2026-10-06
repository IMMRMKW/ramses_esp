#include "firmware_update.h"
#include <string.h>
#include <stdatomic.h>
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_zigbee_ota.h"
#include "zdo/esp_zigbee_zdo_command.h"

static atomic_bool busy;
static esp_ota_handle_t handle;
static const esp_partition_t *partition;
static uint8_t element[6];
static size_t header_bytes;
static uint32_t expected, written;
static bool validated;
static bool zigbee_owned;
static int64_t last_block;

bool firmware_update_claim(void)
{
    bool available = false;
    return atomic_compare_exchange_strong(&busy, &available, true);
}

void firmware_update_release(void)
{
    atomic_store(&busy, false);
}

static esp_err_t fail(esp_err_t error)
{
    if (handle) esp_ota_abort(handle);
    handle = 0;
    validated = false;
    if (zigbee_owned) firmware_update_release();
    zigbee_owned = false;
    return error;
}

static void transfer_watchdog(uint8_t unused)
{
    if (!zigbee_owned) return;
    if (esp_timer_get_time() - last_block > 300000000) {
        fail(ESP_ERR_TIMEOUT);
        return;
    }
    esp_zb_scheduler_alarm(transfer_watchdog, 0, 60000);
}

esp_err_t firmware_zigbee_ota(const esp_zb_zcl_ota_upgrade_value_message_t *m)
{
    if (!m) return ESP_ERR_INVALID_ARG;
    if (m->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return fail(ESP_FAIL);
    switch (m->upgrade_status) {
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_START:
        if (!firmware_update_claim()) return ESP_ERR_INVALID_STATE;
        zigbee_owned = true;
        last_block = esp_timer_get_time();
        esp_zb_scheduler_alarm(transfer_watchdog, 0, 60000);
        partition = esp_ota_get_next_update_partition(NULL);
        header_bytes = expected = written = 0;
        validated = false;
        if (!partition) return fail(ESP_ERR_NOT_FOUND);
        {
            esp_err_t err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
            if (err != ESP_OK) return fail(err);
        }
        return ESP_OK;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_APPLY:
        if (m->ota_header.manufacturer_code != RAMSES_OTA_MANUFACTURER ||
            m->ota_header.image_type != RAMSES_OTA_IMAGE_TYPE ||
            m->ota_header.file_version <= RAMSES_OTA_FILE_VERSION)
            return fail(ESP_ERR_INVALID_VERSION);
        return ESP_OK;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_RECEIVE: {
        last_block = esp_timer_get_time();
        if (!handle || (!m->payload && m->payload_size)) return fail(ESP_ERR_INVALID_STATE);
        const uint8_t *data = m->payload;
        size_t size = m->payload_size;
        /* The SDK strips the OTA file header. Sub-element headers may span blocks. */
        while (header_bytes < sizeof(element) && size) {
            element[header_bytes++] = *data++;
            --size;
        }
        if (header_bytes != sizeof(element)) return ESP_OK;
        if (!expected) {
            if (element[0] || element[1]) return fail(ESP_ERR_INVALID_ARG);
            expected = (uint32_t)element[2] | (uint32_t)element[3] << 8 |
                       (uint32_t)element[4] << 16 | (uint32_t)element[5] << 24;
            if (!expected || expected > partition->size) return fail(ESP_ERR_INVALID_SIZE);
        }
        if (size > expected - written) return fail(ESP_ERR_INVALID_SIZE);
        if (size) {
            esp_err_t err = esp_ota_write(handle, data, size);
            if (err != ESP_OK) return fail(err);
            written += size;
        }
        return ESP_OK;
    }
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_CHECK: {
        last_block = esp_timer_get_time();
        if (!handle || !expected || written != expected) return fail(ESP_ERR_INVALID_SIZE);
        esp_ota_handle_t finished = handle;
        handle = 0; /* esp_ota_end consumes the handle even on error. */
        esp_err_t err = esp_ota_end(finished);
        if (err != ESP_OK) return fail(err);
        esp_app_desc_t app;
        err = esp_ota_get_partition_description(partition, &app);
        if (err != ESP_OK || strcmp(app.project_name, "ramses_esp_c6"))
            return fail(ESP_ERR_INVALID_ARG);
        validated = true;
        return ESP_OK;
    }
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_FINISH:
        if (!validated) return fail(ESP_ERR_INVALID_STATE);
        {
            esp_err_t err = esp_ota_set_boot_partition(partition);
            if (err != ESP_OK) return fail(err);
        }
        esp_restart();
        return ESP_OK;
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ABORT:
    case ESP_ZB_ZCL_OTA_UPGRADE_STATUS_ERROR:
        return fail(ESP_FAIL);
    default:
        return ESP_OK;
    }
}

void firmware_zigbee_ota_cluster(esp_zb_cluster_list_t *clusters)
{
    esp_zb_ota_cluster_cfg_t config = {
        .ota_upgrade_file_version = RAMSES_OTA_FILE_VERSION,
        .ota_upgrade_manufacturer = RAMSES_OTA_MANUFACTURER,
        .ota_upgrade_image_type = RAMSES_OTA_IMAGE_TYPE,
    };
    esp_zb_attribute_list_t *ota = esp_zb_ota_cluster_create(&config);
    esp_zb_zcl_ota_upgrade_client_variable_t variables = {
        .timer_query = 1440, .hw_version = 1, .max_data_size = 48,
    };
    uint16_t server = 0xffff;
    uint8_t endpoint = 0xff;
    ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(ota, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_CLIENT_DATA_ID, &variables));
    ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(ota, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ADDR_ID, &server));
    ESP_ERROR_CHECK(esp_zb_ota_cluster_add_attr(ota, ESP_ZB_ZCL_ATTR_OTA_UPGRADE_SERVER_ENDPOINT_ID, &endpoint));
    ESP_ERROR_CHECK(esp_zb_cluster_list_add_ota_cluster(clusters, ota, ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE));
}

static void server_found(esp_zb_zdp_status_t status, uint16_t address, uint8_t endpoint, void *context)
{
    if (status == ESP_ZB_ZDP_STATUS_SUCCESS) {
        esp_zb_ota_upgrade_client_query_interval_set(10, 1440);
        esp_zb_ota_upgrade_client_query_image_req(address, endpoint);
    }
}

void firmware_zigbee_ota_discover(uint8_t unused)
{
    uint16_t cluster = ESP_ZB_ZCL_CLUSTER_ID_OTA_UPGRADE;
    esp_zb_zdo_match_desc_req_param_t request = {
        .addr_of_interest = 0, .dst_nwk_addr = 0,
        .num_in_clusters = 1, .num_out_clusters = 0,
        .profile_id = ESP_ZB_AF_HA_PROFILE_ID, .cluster_list = &cluster,
    };
    if (esp_zb_bdb_dev_joined()) esp_zb_zdo_match_cluster(&request, server_found, NULL);
    esp_zb_scheduler_alarm(firmware_zigbee_ota_discover, 0, 86400000);
}
