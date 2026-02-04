# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.20)

set(OPENRTX_VERSION "0.4.3")

set(OPENRTX_AVAILABLE_TARGETS
    linux linux_smallscreen linux_mod17
    md3x0 mduv3x0 md9600
    gd77 dm1801 dm1701
    mod17 cs7000 cs7000p
    ttwrplus
)

set(OPENRTX_TARGET "linux" CACHE STRING "Build target")
set_property(CACHE OPENRTX_TARGET PROPERTY STRINGS ${OPENRTX_AVAILABLE_TARGETS})

option(OPENRTX_ASAN "Enable AddressSanitizer" OFF)
option(OPENRTX_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

set(OPENRTX_TEST "" CACHE STRING "Replace main with specialized platform test")

if(NOT OPENRTX_TARGET IN_LIST OPENRTX_AVAILABLE_TARGETS)
    message(FATAL_ERROR "OPENRTX_TARGET must be one of: ${OPENRTX_AVAILABLE_TARGETS}")
endif()

if(OPENRTX_TARGET STREQUAL "ttwrplus")
    message(FATAL_ERROR "Zephyr targets should be built using west. See CMakeLists.txt.zephyr")
endif()

include(cmake/ConfigureTarget.cmake)

project(OpenRTX
    VERSION ${OPENRTX_VERSION}
    LANGUAGES C CXX ASM
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

execute_process(
    COMMAND git describe --tags --dirty --always
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_VERSION)
    set(GIT_VERSION "unknown")
endif()

message(STATUS "OpenRTX ${OPENRTX_VERSION} (${GIT_VERSION})")
message(STATUS "Target: ${OPENRTX_TARGET}")

add_compile_definitions(
    FONT_UBUNTU_REGULAR
    GIT_VERSION="${GIT_VERSION}"
)

if(OPENRTX_TARGET MATCHES "^linux")
    if(OPENRTX_ASAN)
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
        message(STATUS "AddressSanitizer enabled")
    endif()
    if(OPENRTX_UBSAN)
        add_compile_options(-fsanitize=undefined)
        add_link_options(-fsanitize=undefined)
        message(STATUS "UndefinedBehaviorSanitizer enabled")
    endif()
endif()

include(cmake/TargetOptions.cmake)

add_subdirectory(lib)
add_subdirectory(subprojects)
add_subdirectory(openrtx)
add_subdirectory(platform)

add_executable(openrtx_${OPENRTX_TARGET})

if(OPENRTX_TEST)
    target_sources(openrtx_${OPENRTX_TARGET} PRIVATE tests/platform/${OPENRTX_TEST}.c)
    target_compile_definitions(openrtx_${OPENRTX_TARGET} PRIVATE RUNNING_TESTSUITE)
else()
    target_sources(openrtx_${OPENRTX_TARGET} PRIVATE openrtx/src/main.c)
endif()

# Create an INTERFACE library for the combined openrtx libraries
# to handle circular dependencies between static libraries
add_library(openrtx_all INTERFACE)

# Use linker group to handle circular dependencies
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
    if(NOT OPENRTX_TARGET MATCHES "^linux")
        # Embedded targets - include miosix and stdlib in the linker group
        # The linker group allows circular dependencies between our libs
        # and the standard libraries (e.g., libm needs __errno from miosix)
        set(OPENRTX_LIBS
            -Wl,--start-group
            openrtx_ui
            openrtx_rtx
            openrtx_protocols
            openrtx_core
            openrtx_platform
            openrtx_minmea
            openrtx_codec2
            openrtx_miosix
            stdc++ c m gcc atomic
            -Wl,--end-group
        )
    else()
        # Linux targets
        set(OPENRTX_LIBS
            -Wl,--start-group
            openrtx_ui
            openrtx_rtx
            openrtx_protocols
            openrtx_core
            openrtx_platform
            openrtx_minmea
            openrtx_codec2
            -Wl,--end-group
        )
    endif()
else()
    # For other compilers, repeat the libraries
    set(OPENRTX_LIBS
        openrtx_ui
        openrtx_rtx
        openrtx_protocols
        openrtx_core
        openrtx_platform
        openrtx_ui
        openrtx_rtx
        openrtx_protocols
        openrtx_core
        openrtx_platform
        openrtx_minmea
        openrtx_codec2
    )
endif()

target_link_libraries(openrtx_${OPENRTX_TARGET}
    PRIVATE
        ${OPENRTX_LIBS}
)

include(cmake/PostBuild.cmake)

if(OPENRTX_TARGET MATCHES "^linux")
    enable_testing()
    add_subdirectory(tests)
endif()
