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
    # For embedded, compile codec2 from source manually - same source list as
    # the former Meson packagefile. We do NOT use add_subdirectory(codec2)
    # because codec2's CMakeLists.txt runs configure_file to generate
    # codec2/version.h and does many host-system checks unsuitable for
    # cross-compilation. Instead, packagefiles/codec2/codec2/version.h is a
    # pre-generated static header (the same file Meson's packagefiles overlay
    # provided), so we just add that directory to the include path.
    add_library(openrtx_codec2 STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/dump.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/lpc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/nlp.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/phase.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/quantise.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/postfilter.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codec2.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codec2_fft.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/lsp.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/sine.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/interp.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/kiss_fft.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/kiss_fftr.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/newamp1.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codebook.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codebookd.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/pack.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codebooknewamp1.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codebooknewamp1_energy.c
    )

    # packagefiles/codec2 provides codec2/version.h (pre-generated static
    # header) and packagefiles/codec2/src is the codec2 source dir.
    target_include_directories(openrtx_codec2 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/packagefiles/codec2
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src
    )

    target_compile_definitions(openrtx_codec2 PRIVATE
        CODEC2_MODE_EN_DEFAULT=0
        CODEC2_MODE_3200_EN=1
        CODEC2_MODE_1600_EN=1
        FREEDV_MODE_EN_DEFAULT=0
    )

    # Suppress warnings from third-party codec2 sources
    target_compile_options(openrtx_codec2 PRIVATE -w)
endif()

##


# XPowersLib - Power management IC library
# Only used for Zephyr/ESP32 (ttwrplus) target
if(OPENRTX_TARGET STREQUAL "ttwrplus")
    add_subdirectory(XPowersLib)
endif()

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

    # tusb_config.h gates MCU selection on STM32F405xx being defined.
    # Pass it here so CFG_TUSB_MCU resolves correctly.
    target_compile_definitions(tinyusb PRIVATE
        STM32F405xx
        HSE_VALUE=8000000
    )

    # Suppress all warnings from third-party tinyusb sources
    target_compile_options(tinyusb PRIVATE -w)
endif()
