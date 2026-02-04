# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

##
## ConfigureTarget.cmake - Set up toolchain based on target
##

# Determine target family and set appropriate toolchain
# This must be included before project() call

if(NOT DEFINED OPENRTX_TARGET)
    set(OPENRTX_TARGET "linux")
endif()

# Cortex-M4 targets (STM32F405, MK22FN512)
set(OPENRTX_CM4_TARGETS md3x0 mduv3x0 md9600 gd77 dm1801 dm1701 mod17 cs7000)

# Cortex-M7 targets (STM32H743)
set(OPENRTX_CM7_TARGETS cs7000p)

# Linux targets
set(OPENRTX_LINUX_TARGETS linux linux_smallscreen linux_mod17)

# Set toolchain file for cross-compilation
if(OPENRTX_TARGET IN_LIST OPENRTX_CM4_TARGETS)
    if(NOT CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/toolchain-arm-miosix.cmake" CACHE STRING "" FORCE)
    endif()
    set(OPENRTX_ARCH "CM4F")
elseif(OPENRTX_TARGET IN_LIST OPENRTX_CM7_TARGETS)
    if(NOT CMAKE_TOOLCHAIN_FILE)
        set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/toolchain-cm7-miosix.cmake" CACHE STRING "" FORCE)
    endif()
    set(OPENRTX_ARCH "CM7")
elseif(OPENRTX_TARGET IN_LIST OPENRTX_LINUX_TARGETS)
    set(OPENRTX_ARCH "x86_64")
endif()

message(STATUS "Architecture: ${OPENRTX_ARCH}")
