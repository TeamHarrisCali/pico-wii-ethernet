cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w)
include(pico_sdk_import.cmake)

project(pico_wii_ethernet C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Wi-Fi credentials live in a local file that is not part of the sources.
if(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/config/wifi_config.h)
    message(FATAL_ERROR
        "config/wifi_config.h is missing. Copy config/wifi_config.h.example to "
        "config/wifi_config.h and fill in your SSID and password.")
endif()

# Debug-only: add a USB CDC serial port (second interface) so the log can be read on a PC.
# Leave OFF for the Wii build: the Wii should see a plain single-interface AX88772.
option(WII_ETH_DEBUG_CDC "Add a USB CDC debug serial port (bench testing on a PC only)" OFF)

add_executable(pico_wii_ethernet
    src/main.c
    src/ax88772.c
    src/usb_descriptors.c
    src/usbnet.c
    src/dhcpd.c
    src/nat.c
    src/dbglog.c
)

target_include_directories(pico_wii_ethernet PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/config
)

target_compile_definitions(pico_wii_ethernet PRIVATE
    WII_ETH_DEBUG_CDC=$<BOOL:${WII_ETH_DEBUG_CDC}>
)

target_link_libraries(pico_wii_ethernet
    pico_stdlib
    pico_unique_id
    hardware_uart
    pico_cyw43_arch_lwip_poll
    tinyusb_device
)

# Log every SETUP packet (including standard requests) before TinyUSB handles it.
target_link_options(pico_wii_ethernet PRIVATE -Wl,--wrap=dcd_event_handler)

pico_enable_stdio_usb(pico_wii_ethernet 0)
pico_enable_stdio_uart(pico_wii_ethernet 0)

pico_add_extra_outputs(pico_wii_ethernet)
