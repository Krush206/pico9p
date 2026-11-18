cmake_minimum_required(VERSION 3.13)

# ---------------------------------------------------------------------------
# CMake build for the c9 9P library ported to the Raspberry Pi Pico RP2040.
#
# Builds:
#   c9               static 9P library (server + client interface)
#   pico9p           9P file server over USB CDC for RP2040 (no UART)
#
# Requires PICO_SDK_PATH for the Pico target:
#   mkdir build
#   cd build
#   cmake -DPICO_SDK_PATH=/path/to/pico-sdk-2.3.1 \
#         -DPICO_TOOLCHAIN_PATH=/path/to/gcc-arm-none-eabi-10.3-2021.10 \
#         ..
#   make
# ---------------------------------------------------------------------------

if(NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH "/opt/pico-sdk-2.3.1" CACHE PATH "Path to Raspberry Pi Pico SDK")
endif()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to Raspberry Pi Pico SDK" FORCE)

include(${PICO_SDK_PATH}/pico_sdk_init.cmake)

project(pico9p VERSION 1.0.0 LANGUAGES C CXX ASM)

pico_sdk_init()

# ---------------------------------------------------------------------------
# c9 library: portable freestanding C (<stdint.h>, <string.h> only).
# fs9p.c uses the client interface (c9version, c9attach, c9proc, c9stat),
# so the library is built with both client and server enabled.
# -Werror is intentionally not applied: fs9p.c contains stub callbacks with
# unused parameters by design.
# ---------------------------------------------------------------------------
add_library(c9 STATIC
    src/lib/proto.c
    src/lib/fs9p.c
)

target_include_directories(c9 PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/src/include
)

target_compile_options(c9 PRIVATE
    -Wall -Wextra -Wno-unused-parameter
)

# ---------------------------------------------------------------------------
# 9P file server over USB CDC for RP2040
#
# Tree served:
#   /                 directory
#   /dev/             directory
#   /dev/gpio0..gpio29  read=>"0"/"1", write "0"/"1" sets the pin
#   /flash/           directory
#   /flash/<name>     file stored in the reserved flash region
#
# Transport: USB CDC (TinyUSB). No UART is used on this target.
# ---------------------------------------------------------------------------
add_executable(pico9p
    src/main.c
    src/9psrv.c
    src/usb.c
)

target_link_libraries(pico9p PRIVATE
    c9
    tinyusb_device
    pico_stdlib
    hardware_flash
    hardware_gpio
    hardware_irq
    hardware_timer
)

target_include_directories(pico9p PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/include
)

# TinyUSB: CDC device only, bare metal (no RTOS), debug off.
target_compile_definitions(pico9p PRIVATE
    CFG_TUD_CDC=1
    CFG_TUD_CDC_RX_BUFSIZE=4096
    CFG_TUD_CDC_TX_BUFSIZE=4096
)

# Reserve the last portion of flash for the 9P flash/ store. Override with
# -DPICO_9P_FLASH_OFFSET=... -DPICO_9P_FLASH_SIZE=... if needed.
if(NOT DEFINED PICO_9P_FLASH_OFFSET)
    set(PICO_9P_FLASH_OFFSET 0x00180000 CACHE STRING "Flash offset for 9P storage")
endif()
if(NOT DEFINED PICO_9P_FLASH_SIZE)
    set(PICO_9P_FLASH_SIZE 0x00040000 CACHE STRING "Flash size for 9P storage")
endif()

target_compile_definitions(pico9p PRIVATE
    PICO_9P_FLASH_OFFSET=${PICO_9P_FLASH_OFFSET}
    PICO_9P_FLASH_SIZE=${PICO_9P_FLASH_SIZE}
)

# Disable SDK stdio over USB/UART so we own the CDC interface entirely.
set(PICO_STDIO_USB 0 CACHE STRING "" FORCE)
set(PICO_STDIO_UART 0 CACHE STRING "" FORCE)

pico_enable_stdio_usb(pico9p 0)
pico_enable_stdio_uart(pico9p 0)
pico_set_program_name(pico9p "pico9p")
