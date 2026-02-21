# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

#
# Shared source file definitions for OpenRTX
# Used by both CMakeLists.txt (standalone) and CMakeLists.txt.zephyr (Zephyr)
#

# Base path - set by including file before include()
# For standalone: CMAKE_SOURCE_DIR
# For Zephyr: OPENRTX_ROOT (set before find_package(Zephyr))
if(NOT DEFINED OPENRTX_SOURCE_DIR)
    set(OPENRTX_SOURCE_DIR ${CMAKE_SOURCE_DIR})
endif()

##
## Core sources
##
set(OPENRTX_CORE_SOURCES
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/state.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/threads.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/battery.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/graphics.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/input.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/utils.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/queue.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/chan.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/gps.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/dsp.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/cps.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/crc.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/datetime.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/openrtx.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/audio_codec.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/audio_stream.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/audio_path.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/data_conversion.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/memory_profiling.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/voicePrompts.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/voicePromptUtils.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/voicePromptData.S
    ${OPENRTX_SOURCE_DIR}/openrtx/src/core/nvmem_access.c
)

##
## Protocol sources (M17)
##
set(OPENRTX_PROTOCOLS_SOURCES
    ${OPENRTX_SOURCE_DIR}/openrtx/src/protocols/M17/M17DSP.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/protocols/M17/M17Golay.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/protocols/M17/M17Callsign.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/protocols/M17/Callsign.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/protocols/M17/M17Modulator.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/protocols/M17/M17Demodulator.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/protocols/M17/M17FrameEncoder.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/protocols/M17/M17FrameDecoder.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/protocols/M17/M17LinkSetupFrame.cpp
)

##
## RTX sources
##
set(OPENRTX_RTX_SOURCES
    ${OPENRTX_SOURCE_DIR}/openrtx/src/rtx/rtx.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/rtx/OpMode_FM.cpp
    ${OPENRTX_SOURCE_DIR}/openrtx/src/rtx/OpMode_M17.cpp
)

##
## UI sources - Default
##
set(OPENRTX_UI_DEFAULT_SOURCES
    ${OPENRTX_SOURCE_DIR}/openrtx/src/ui/default/ui.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/ui/default/ui_main.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/ui/default/ui_menu.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/ui/default/ui_strings.c
)

##
## UI sources - Module17
##
set(OPENRTX_UI_MODULE17_SOURCES
    ${OPENRTX_SOURCE_DIR}/openrtx/src/ui/module17/ui.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/ui/module17/ui_main.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/ui/module17/ui_menu.c
    ${OPENRTX_SOURCE_DIR}/openrtx/src/ui/default/ui_strings.c
)

##
## Minmea sources
##
set(OPENRTX_MINMEA_SOURCES
    ${OPENRTX_SOURCE_DIR}/lib/minmea/minmea.c
)

##
## Common include directories
##
set(OPENRTX_INCLUDE_DIRS
    ${OPENRTX_SOURCE_DIR}/openrtx/include
    ${OPENRTX_SOURCE_DIR}/platform
    ${OPENRTX_SOURCE_DIR}/lib/minmea/include
    ${OPENRTX_SOURCE_DIR}/lib/qdec/include
)

##
## Common compile definitions (used only by zephyr/CMakeLists.txt.zephyr)
## Standalone CMake builds use per-target OPENRTX_TARGET_DEFS instead.
##
set(OPENRTX_COMMON_DEFINITIONS
    _GNU_SOURCE
    FONT_UBUNTU_REGULAR
    CODEC2_MODE_EN_DEFAULT=0
    FREEDV_MODE_EN_DEFAULT=0
    CODEC2_MODE_3200_EN=1
    M_PI=3.14159265358979323846f
)
