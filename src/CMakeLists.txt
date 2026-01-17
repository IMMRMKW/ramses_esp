# See the build system documentation in IDF programming guide
# for more information about component CMakeLists.txt files.
# FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

# idf_component_register(
#     SRCS main.c radio.c host.c platform.c
#     INCLUDE_DIRS
#     PRIV_INCLUDE_DIRS
#     REQUIRES driver esp_app_format esp_event console 
#     PRIV_REQUIRES command ramses-debug ramses-buttons ramses-led ramses-nvs ramses-network ramses-mqtt cc1101 frame message gateway 
# )

# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB app_sources ${CMAKE_CURRENT_LIST_DIR}/*.c)

idf_component_register(
    SRCS ${app_sources}
    REQUIRES driver esp_app_format esp_event console esp_timer
    PRIV_REQUIRES command ramses-debug ramses-buttons ramses-led ramses-nvs ramses-network ramses-mqtt cc1101 frame message gateway
)


