#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_cluster.h"

/* Experimental manufacturer code; image type uniquely scopes this product. */
#define RAMSES_OTA_MANUFACTURER 0xffff
#define RAMSES_OTA_IMAGE_TYPE 0xc601
#ifndef RAMSES_OTA_FILE_VERSION
#define RAMSES_OTA_FILE_VERSION 0
#endif

bool firmware_update_claim(void);
void firmware_update_release(void);
esp_err_t firmware_zigbee_ota(const esp_zb_zcl_ota_upgrade_value_message_t *message);
void firmware_zigbee_ota_cluster(esp_zb_cluster_list_t *clusters);
void firmware_zigbee_ota_discover(uint8_t unused);
