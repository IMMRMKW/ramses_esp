set(component_srcs "gateway.c" "device.c")

idf_component_register(
    SRCS "${component_srcs}"
    INCLUDE_DIRS "include" "${CMAKE_CURRENT_LIST_DIR}/../ramses-network/include"
    PRIV_REQUIRES console message ramses-mqtt ramses-network gateway
)
