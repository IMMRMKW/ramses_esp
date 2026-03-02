set(component_srcs
    "ramses_network.c" "network_cmd.c"
    "ramses_wifi.c" "wifi_cmd.c"
    "zigbee_cmd.c"
    "ramses_ota.c" "ota_cmd.c"
    "ramses_sntp.c"
)

idf_component_register(
    SRCS "zigbee.c" "${component_srcs}"
    INCLUDE_DIRS "include" "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-zigbee-lib/include" "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-zboss-lib/include"
    REQUIRES command nvs_flash esp_wifi esp_netif console mbedtls app_update esp_partition esp_https_ota esp_http_client message espressif__esp-zigbee-lib espressif__esp-zboss-lib ramses-buttons ramses-led ramses-mqtt
)

set(zigbee_target "${idf_target}")
if(NOT zigbee_target)
    set(zigbee_target "${IDF_TARGET}")
endif()

if(CONFIG_ZB_ZED)
    target_link_libraries(${COMPONENT_LIB} PUBLIC
        "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-zigbee-lib/lib/${zigbee_target}/libesp_zb_api.ed.a"
        "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-zboss-lib/lib/${zigbee_target}/libzboss_stack.ed.a"
    )
elseif(CONFIG_ZB_ZCZR)
    target_link_libraries(${COMPONENT_LIB} PUBLIC
        "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-zigbee-lib/lib/${zigbee_target}/libesp_zb_api.zczr.a"
        "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-zboss-lib/lib/${zigbee_target}/libzboss_stack.zczr.a"
    )
else()
    # Default to ZED libs when Zigbee Kconfig is not available at configure time.
    target_link_libraries(${COMPONENT_LIB} PUBLIC
        "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-zigbee-lib/lib/${zigbee_target}/libesp_zb_api.ed.a"
        "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-zboss-lib/lib/${zigbee_target}/libzboss_stack.ed.a"
    )
endif()

# Always link the zboss port library for esp-zigbee.
target_link_libraries(${COMPONENT_LIB} PUBLIC "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp-zigbee-lib/lib/${zigbee_target}/libzboss_port.native.a")