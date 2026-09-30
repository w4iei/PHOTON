cmake_minimum_required(VERSION 3.13)

# PICO_SDK_PATH: any official pico-sdk >= 2.1 checkout with the tinyusb
# submodule initialized. Defaults to ~/pico-sdk (developed against 2.3.0).
if (NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{HOME}/pico-sdk")
    message(STATUS "PICO_SDK_PATH not set; using ${PICO_SDK_PATH}")
endif ()

set(PICO_PLATFORM rp2350-arm-s)
set(PICO_BOARD photon_rp2350)
list(APPEND PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)

include(pico_sdk_import.cmake)

project(photon_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(photon
    src/main.c
    src/core1/tla2518.c
    src/core1/scan.c
    src/core1/events.c
    src/ipc/rings.c
    src/comms/frame.c
    src/comms/rs485_bus.c
    src/comms/protocol.c
    src/usb/usb_descriptors.c
    src/usb/cdc_console.c
    src/usb/midi_out.c
    src/bridge/midi_map.c
    src/bridge/recorder.c
    src/bridge/sd_spi.c
    src/bridge/smf.c
    lib/fatfs/ff.c
    lib/fatfs/ffunicode.c
    src/config/config_store.c
    src/util/crc32.c
    src/util/log.c
    src/hw/psram.c
)

target_include_directories(photon PRIVATE src src/usb lib/fatfs)

# Build identity. cmake/build_id.cmake regenerates build/generated/build_id.h
# on every build with the git short hash ("-dirty" if firmware/ has
# uncommitted changes) and the build time. The console banner and `id` print
# it, and cdc_console.c stores it as picotool binary info, so
# `picotool info -a photon.uf2` (or a board sitting in BOOTSEL) names the
# commit without booting. Only cdc_console.c includes the header, so a
# rebuild recompiles just that file.
set(PHOTON_BUILD_ID_H ${CMAKE_CURRENT_BINARY_DIR}/generated/build_id.h)
add_custom_target(photon_build_id
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_LIST_DIR}
            -DOUT=${PHOTON_BUILD_ID_H}
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/build_id.cmake
    BYPRODUCTS ${PHOTON_BUILD_ID_H}
    COMMENT "Generating build_id.h"
    VERBATIM)
add_dependencies(photon photon_build_id)
target_include_directories(photon PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
# The SDK would stamp the __DATE__ of its own first compile in this build
# directory; the date from build_id.h replaces it.
target_compile_definitions(photon PRIVATE PICO_NO_BI_PROGRAM_BUILD_DATE=1)

target_compile_options(photon PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(photon
    pico_stdlib
    pico_multicore
    pico_unique_id
    hardware_spi
    hardware_uart
    hardware_dma
    hardware_flash
    hardware_sync
    hardware_irq
    hardware_timer
    tinyusb_device
    tinyusb_board
)

# All stdio goes through our own TinyUSB CDC console; no SDK stdio backends.
pico_enable_stdio_usb(photon 0)
pico_enable_stdio_uart(photon 0)

# Whole image runs from SRAM: no code fetch ever touches flash/XIP at
# runtime, so core-0 flash writes can never stall the core-1 scan loop.
pico_set_binary_type(photon copy_to_ram)

pico_add_extra_outputs(photon)

# ---------------------------------------------------------------------------
# OPTIONAL: touchscreen support for the main controller board 001 (Waveshare
# 2.8" ST7789T3 display + CST328 touch on the FPC), in src/touchscreen/.
# Not part of the PHOTON image and off by default. Turning it on fetches LVGL
# v9.3.0 from GitHub at configure time and builds the library
# photon_touchscreen (plus photon_lvgl) for firmware that wants a local UI:
#   cmake -B build -G Ninja -DPHOTON_TOUCHSCREEN=ON
# ---------------------------------------------------------------------------
option(PHOTON_TOUCHSCREEN "Build the optional 001 touchscreen drivers (fetches LVGL)" OFF)
if (PHOTON_TOUCHSCREEN)
    include(FetchContent)
    FetchContent_Declare(lvgl
        GIT_REPOSITORY https://github.com/lvgl/lvgl.git
        GIT_TAG v9.3.0
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR no-cmake)   # sources only: built from a glob below
    FetchContent_MakeAvailable(lvgl)
    file(GLOB_RECURSE PHOTON_LVGL_SOURCES CONFIGURE_DEPENDS ${lvgl_SOURCE_DIR}/src/*.c)
    add_library(photon_lvgl STATIC ${PHOTON_LVGL_SOURCES})
    target_include_directories(photon_lvgl PUBLIC ${lvgl_SOURCE_DIR}
                               ${CMAKE_CURRENT_LIST_DIR}/src/touchscreen)   # lv_conf.h
    target_compile_definitions(photon_lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE=1)
    target_compile_options(photon_lvgl PRIVATE -O2 -w)
    target_link_libraries(photon_lvgl PRIVATE pico_stdlib)

    add_library(photon_touchscreen STATIC
        src/touchscreen/lcd.c
        src/touchscreen/touch.c)
    target_include_directories(photon_touchscreen PUBLIC src)
    target_compile_options(photon_touchscreen PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(photon_touchscreen PUBLIC
        photon_lvgl pico_stdlib hardware_spi hardware_i2c hardware_dma hardware_pwm hardware_sync)
endif ()
