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
elseif(NOT DEFINED PROJECT_VER)
    # Do not let ESP-IDF derive a "-dirty" version from generated build files.
    file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" PROJECT_VER LIMIT_COUNT 1)
endif()
if(NOT PROJECT_VER MATCHES "^v([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "Firmware version must be vMAJOR.MINOR.PATCH, without a dirty suffix")
endif()
set(RAMSES_MAJOR "${CMAKE_MATCH_1}")
set(RAMSES_MINOR "${CMAKE_MATCH_2}")
set(RAMSES_PATCH "${CMAKE_MATCH_3}")
if(RAMSES_MAJOR GREATER 255 OR RAMSES_MINOR GREATER 255 OR RAMSES_PATCH GREATER 255)
    message(FATAL_ERROR "Firmware version components must be <= 255")
endif()
math(EXPR RAMSES_OTA_VERSION "(${RAMSES_MAJOR} << 24) | (${RAMSES_MINOR} << 16) | (${RAMSES_PATCH} << 8)")
add_compile_definitions(RAMSES_OTA_FILE_VERSION=${RAMSES_OTA_VERSION})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Match ZigbeeText minimal build to reduce component load
idf_build_set_property(MINIMAL_BUILD ON)

project(ramses_esp_c6)

# (Legacy / future) extra Kconfig includes can be appended here if needed
list(APPEND kconfigs "${CMAKE_SOURCE_DIR}/lib/cc1101/Kconfig.projbuild")

