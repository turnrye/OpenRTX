# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

##
## Subprojects - external dependencies
##

##

# Codec2 - Speech codec
if(OPENRTX_TARGET MATCHES "^linux")
    # For Linux, use system codec2 library
    find_library(CODEC2_LIBRARY codec2 REQUIRED)
    find_path(CODEC2_INCLUDE_DIR codec2/codec2.h)

    add_library(openrtx_codec2 INTERFACE)
    target_link_libraries(openrtx_codec2 INTERFACE ${CODEC2_LIBRARY})
    if(CODEC2_INCLUDE_DIR)
        target_include_directories(openrtx_codec2 INTERFACE ${CODEC2_INCLUDE_DIR})
    endif()
else()
    # For embedded, use codec2 submodule
    add_subdirectory(codec2)
    # Assumes codec2/CMakeLists.txt defines a target named codec2
    add_library(openrtx_codec2 ALIAS codec2)
endif()

##

# XPowersLib - Power management IC library
add_subdirectory(XPowersLib)

# tinyusb - USB stack
# tinyusb has no root CMakeLists.txt; define the target manually from known
# OpenRTX-relevant sources, mirroring the former Meson packagefile override.
# Only built for embedded targets that actually use USB (STM32F4-based).
if(NOT OPENRTX_TARGET MATCHES "^linux")
    add_library(tinyusb STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/tusb.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/device/usbd.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/common/tusb_fifo.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/device/usbd_control.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/class/cdc/cdc_device.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/portable/st/synopsys/dcd_synopsys.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/portable/st/stm32_fsdev/dcd_stm32_fsdev.c
    )

    # tusb_config.h lives in the packagefiles overlay directory (previously used by
    # Meson's packagefiles mechanism; now serves as the permanent config header).
    target_include_directories(tinyusb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/packagefiles/tinyusb/openrtx
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/hw
        ${CMAKE_SOURCE_DIR}/platform/mcu/CMSIS/Device/ST/STM32F4xx/Include
        ${CMAKE_SOURCE_DIR}/platform/mcu/CMSIS/Include
    )

    # Suppress all warnings from third-party tinyusb sources
    target_compile_options(tinyusb PRIVATE -w)
endif()
