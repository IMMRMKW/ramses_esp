idf_component_register(
    SRCS main.c radio.c host.c platform.c
    INCLUDE_DIRS "."
    REQUIRES driver esp_app_format esp_event console esp_timer
    PRIV_REQUIRES command ramses-debug ramses-buttons ramses-led ramses-nvs ramses-network ramses-mqtt cc1101 frame message gateway
)
