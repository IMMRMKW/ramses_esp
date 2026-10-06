set(component_srcs "ramses-mqtt.c" "mqtt_update.c")

idf_component_register(
    SRCS ${component_srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mqtt json app_update esp_http_client mbedtls ramses-network gateway message command
)
