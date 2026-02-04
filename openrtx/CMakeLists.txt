# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

##
## OpenRTX core libraries - platform-independent code
##

# Include shared source definitions
set(OPENRTX_SOURCE_DIR ${CMAKE_SOURCE_DIR})
include(${CMAKE_SOURCE_DIR}/cmake/Sources.cmake)

# Per-target include directories (for hwconfig.h)
if(OPENRTX_TARGET MATCHES "^linux")
    list(APPEND OPENRTX_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/platform/targets/linux)
elseif(OPENRTX_TARGET STREQUAL "md3x0")
    list(APPEND OPENRTX_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/platform/targets/MD-3x0)
elseif(OPENRTX_TARGET STREQUAL "mduv3x0")
    list(APPEND OPENRTX_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/platform/targets/MD-UV3x0)
elseif(OPENRTX_TARGET STREQUAL "md9600")
    list(APPEND OPENRTX_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/platform/targets/MD-9600)
elseif(OPENRTX_TARGET STREQUAL "gd77" OR OPENRTX_TARGET STREQUAL "dm1801")
    list(APPEND OPENRTX_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/platform/targets/GDx)
elseif(OPENRTX_TARGET STREQUAL "dm1701")
    list(APPEND OPENRTX_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/platform/targets/DM-1701)
elseif(OPENRTX_TARGET STREQUAL "mod17")
    list(APPEND OPENRTX_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/platform/targets/Module17)
elseif(OPENRTX_TARGET STREQUAL "cs7000")
    list(APPEND OPENRTX_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/platform/targets/CS7000)
elseif(OPENRTX_TARGET STREQUAL "cs7000p")
    list(APPEND OPENRTX_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/platform/targets/CS7000-PLUS)
endif()

##
## Core library
##
add_library(openrtx_core STATIC ${OPENRTX_CORE_SOURCES})

target_include_directories(openrtx_core PUBLIC ${OPENRTX_INCLUDE_DIRS})
target_compile_definitions(openrtx_core PUBLIC ${OPENRTX_TARGET_DEFS})

# Link to platform for embedded targets to inherit MCU/miosix includes
if(NOT OPENRTX_TARGET MATCHES "^linux")
    target_link_libraries(openrtx_core PUBLIC openrtx_platform openrtx_codec2)
else()
    target_link_libraries(openrtx_core PUBLIC openrtx_codec2)
endif()

# Handle assembly file - compile as ASM (GAS syntax), not C
# The .incbin directive uses path "../voiceprompts.vpc".
# GAS resolves .incbin paths relative to the current working directory, NOT the source file.
# CMake runs the assembly from ${CMAKE_BINARY_DIR}/openrtx/, so "../voiceprompts.vpc"
# needs to exist at ${CMAKE_BINARY_DIR}/voiceprompts.vpc
if(NOT OPENRTX_TARGET MATCHES "^linux")
    # For embedded targets (no VP_USE_FILESYSTEM), create symlink in build directory
    file(CREATE_LINK
        ${CMAKE_SOURCE_DIR}/voiceprompts.vpc
        ${CMAKE_BINARY_DIR}/voiceprompts.vpc
        SYMBOLIC
    )
endif()

set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/voicePromptData.S
    PROPERTIES LANGUAGE ASM
)

##
## Protocols library (M17)
##
add_library(openrtx_protocols STATIC ${OPENRTX_PROTOCOLS_SOURCES})

target_include_directories(openrtx_protocols PUBLIC ${OPENRTX_INCLUDE_DIRS})
target_compile_definitions(openrtx_protocols PUBLIC ${OPENRTX_TARGET_DEFS})

# Link to platform for embedded targets to inherit MCU/miosix includes
if(NOT OPENRTX_TARGET MATCHES "^linux")
    target_link_libraries(openrtx_protocols PUBLIC openrtx_platform)
endif()

##
## RTX library (radio transceiver control)
##
add_library(openrtx_rtx STATIC ${OPENRTX_RTX_SOURCES})

target_include_directories(openrtx_rtx PUBLIC ${OPENRTX_INCLUDE_DIRS})
target_compile_definitions(openrtx_rtx PUBLIC ${OPENRTX_TARGET_DEFS})

# Link to platform for embedded targets to inherit MCU/miosix includes
if(NOT OPENRTX_TARGET MATCHES "^linux")
    target_link_libraries(openrtx_rtx PUBLIC openrtx_platform)
endif()

##
## UI library (selected based on target)
##
add_library(openrtx_ui STATIC)

if(OPENRTX_USE_UI STREQUAL "module17")
    target_sources(openrtx_ui PRIVATE ${OPENRTX_UI_MODULE17_SOURCES})
else()
    # Default UI
    target_sources(openrtx_ui PRIVATE ${OPENRTX_UI_DEFAULT_SOURCES})
endif()

target_include_directories(openrtx_ui PUBLIC ${OPENRTX_INCLUDE_DIRS})
target_compile_definitions(openrtx_ui PUBLIC ${OPENRTX_TARGET_DEFS})

# Link to platform for embedded targets to inherit MCU/miosix includes
if(NOT OPENRTX_TARGET MATCHES "^linux")
    target_link_libraries(openrtx_ui PUBLIC openrtx_platform)
endif()
