cmake_minimum_required(VERSION 3.16.0)

# Allow selecting a board-specific defaults file via env or fallback to sdkconfig.defaults
if(NOT DEFINED ENV{SDKCONFIG_DEFAULTS})
	# Users can set SDKCONFIG_DEFAULTS env var (comma separated) in CI for different boards
	set(ENV{SDKCONFIG_DEFAULTS} "sdkconfig.defaults")
endif()
# Ensure correct target (ESP32-C6)
set(IDF_TARGET "esp32c6" CACHE INTERNAL "IDF target")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Match ZigbeeText minimal build to reduce component load
idf_build_set_property(MINIMAL_BUILD ON)

project(ramses_esp_c6)

# (Legacy / future) extra Kconfig includes can be appended here if needed
list(APPEND kconfigs "${CMAKE_SOURCE_DIR}/lib/cc1101/Kconfig.projbuild")

