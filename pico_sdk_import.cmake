# Minimal pico-sdk import: uses PICO_SDK_PATH (cmake -D or environment variable).
if(DEFINED ENV{PICO_SDK_PATH} AND (NOT PICO_SDK_PATH))
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()

if(NOT PICO_SDK_PATH)
    message(FATAL_ERROR "PICO_SDK_PATH is not set. Point it at a pico-sdk checkout (with submodules).")
endif()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH BASE_DIR "${CMAKE_BINARY_DIR}")
if(NOT EXISTS ${PICO_SDK_PATH}/pico_sdk_init.cmake)
    message(FATAL_ERROR "Directory '${PICO_SDK_PATH}' does not look like a pico-sdk checkout")
endif()

set(PICO_SDK_INIT_CMAKE_FILE ${PICO_SDK_PATH}/pico_sdk_init.cmake)
include(${PICO_SDK_INIT_CMAKE_FILE})
