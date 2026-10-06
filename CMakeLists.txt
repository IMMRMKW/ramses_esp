cmake_minimum_required(VERSION 3.16.0)

# Allow selecting a board-specific defaults file via env or fallback to sdkconfig.defaults
if(NOT DEFINED ENV{SDKCONFIG_DEFAULTS})
	# Users can set SDKCONFIG_DEFAULTS env var (comma separated) in CI for different boards
	set(ENV{SDKCONFIG_DEFAULTS} "sdkconfig.defaults")
endif()
# Ensure correct target (ESP32-C6)
set(IDF_TARGET "esp32c6" CACHE INTERNAL "IDF target")
if(DEFINED ENV{RAMSES_RELEASE_VERSION})
    set(PROJECT_VER "$ENV{RAMSES_RELEASE_VERSION}")
endif()
set(RAMSES_OTA_VERSION 0)
if(PROJECT_VER MATCHES "^v([0-9]+)\\.([0-9]+)(\\.([0-9]+))?$")
    set(RAMSES_PATCH "${CMAKE_MATCH_4}")
    if(RAMSES_PATCH STREQUAL "")
        set(RAMSES_PATCH 0)
    endif()
    math(EXPR RAMSES_OTA_VERSION "(${CMAKE_MATCH_1} << 24) | (${CMAKE_MATCH_2} << 16) | (${RAMSES_PATCH} << 8)")
endif()
add_compile_definitions(RAMSES_OTA_FILE_VERSION=${RAMSES_OTA_VERSION})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Match ZigbeeText minimal build to reduce component load
idf_build_set_property(MINIMAL_BUILD ON)

project(ramses_esp_c6)

# (Legacy / future) extra Kconfig includes can be appended here if needed
list(APPEND kconfigs "${CMAKE_SOURCE_DIR}/lib/cc1101/Kconfig.projbuild")

