# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

##
## Unit tests - link against static libraries
##

# Only build tests for Linux targets
if(NOT OPENRTX_TARGET MATCHES "^linux")
    return()
endif()

# Common include directories for test sources
set(TEST_INCLUDES
    ${CMAKE_SOURCE_DIR}/openrtx/include
    ${CMAKE_SOURCE_DIR}/platform
    ${CMAKE_SOURCE_DIR}/platform/targets/linux
    ${CMAKE_SOURCE_DIR}/platform/targets/linux/emulator
    ${CMAKE_SOURCE_DIR}/lib/minmea/include
    ${CMAKE_SOURCE_DIR}/lib/qdec/include
)

# Common compile definitions for tests
set(TEST_DEFS
    PLATFORM_LINUX
    VP_USE_FILESYSTEM
    CONFIG_SCREEN_WIDTH=160
    CONFIG_SCREEN_HEIGHT=128
    CONFIG_PIX_FMT_RGB565
    CONFIG_GPS
    CONFIG_RTC
    FONT_UBUNTU_REGULAR
    sniprintf=snprintf
    vsniprintf=vsnprintf
)

# Find required packages
find_package(SDL2 REQUIRED)
find_package(Threads REQUIRED)
find_library(CODEC2_LIBRARY codec2 REQUIRED)

# Libraries needed for tests - use linker group to handle circular deps
set(TEST_LINK_LIBS
    -Wl,--start-group
    openrtx_ui
    openrtx_rtx
    openrtx_protocols
    openrtx_core
    openrtx_platform
    openrtx_minmea
    openrtx_codec2
    -Wl,--end-group
    SDL2::SDL2
    Threads::Threads
    ${CODEC2_LIBRARY}
    m
    readline
)

# Helper function to create test executable that links to libraries
function(add_openrtx_test TEST_NAME TEST_SOURCE)
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_include_directories(${TEST_NAME} PRIVATE ${TEST_INCLUDES})
    target_compile_definitions(${TEST_NAME} PRIVATE ${TEST_DEFS})
    target_link_libraries(${TEST_NAME} PRIVATE ${TEST_LINK_LIBS})
    
    # Add compile flags
    target_compile_options(${TEST_NAME} PRIVATE -ffunction-sections -fdata-sections)
    target_link_options(${TEST_NAME} PRIVATE -Wl,--gc-sections)
endfunction()

##
## Unit tests
##

# M17 Golay Unit Test
add_openrtx_test(m17_golay_test ${CMAKE_CURRENT_SOURCE_DIR}/unit/M17_golay.cpp)
add_test(NAME "M17 Golay Unit Test" COMMAND m17_golay_test)

# M17 Viterbi Unit Test
add_openrtx_test(m17_viterbi_test ${CMAKE_CURRENT_SOURCE_DIR}/unit/M17_viterbi.cpp)
add_test(NAME "M17 Viterbi Unit Test" COMMAND m17_viterbi_test)

# M17 RRC Test
add_openrtx_test(m17_rrc_test ${CMAKE_CURRENT_SOURCE_DIR}/unit/M17_rrc.cpp)
add_test(NAME "M17 RRC Test" COMMAND m17_rrc_test)

# M17 Callsign Unit Test
add_openrtx_test(m17_callsign_test ${CMAKE_CURRENT_SOURCE_DIR}/unit/M17_callsign.cpp)
add_test(NAME "M17 Callsign Unit Test" COMMAND m17_callsign_test)

# Codeplug Test
add_openrtx_test(cps_test ${CMAKE_CURRENT_SOURCE_DIR}/unit/cps.c)
add_test(NAME "Codeplug Test" COMMAND cps_test)

# minmea conversion Test
add_openrtx_test(minmea_conversion_test ${CMAKE_CURRENT_SOURCE_DIR}/unit/convert_minmea_coord.c)
add_test(NAME "minmea conversion Test" COMMAND minmea_conversion_test)

# The following tests are currently skipped (same as meson build)
# - M17 Demodulator Test: Has broken includes (.h instead of .hpp)
# - Linux InputStream Test: Requires special setup
# - Sine Test: Requires audio hardware
# - Voice Prompts Test: Requires voice prompt files
