# Source file selection based on PlatformIO environment
# SDKCONFIG path contains the environment name (e.g. sdkconfig.esp32dev-cvbs)
if(SDKCONFIG MATCHES "cvbs")
    FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/cvbs_viewer/*.*)
else()
    FILE(GLOB app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
endif()

idf_component_register(SRCS ${app_sources})
