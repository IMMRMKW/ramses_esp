#pragma once
#include <stdbool.h>
#include "mqtt_client.h"
void mqtt_update_connected(esp_mqtt_client_handle_t client, const char *topic);
bool mqtt_update_command(esp_mqtt_event_handle_t event);
void mqtt_update_progress(bool active, int percent);
