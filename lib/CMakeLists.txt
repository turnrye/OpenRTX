# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

##
## External libraries
##

##
## minmea - lightweight GPS NMEA 0183 parser
##
add_library(openrtx_minmea STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/minmea/minmea.c
)

target_include_directories(openrtx_minmea PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/minmea/include
)

##
## qdec - header-only quadrature decoding library
##
add_library(openrtx_qdec INTERFACE)
target_include_directories(openrtx_qdec INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/qdec/include
)

##
## Miosix RTOS kernel (for embedded targets only)
##
if(NOT OPENRTX_TARGET MATCHES "^linux")
    add_library(openrtx_miosix STATIC)

    # Common Miosix sources
    set(MIOSIX_COMMON_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/kernel/scheduler/priority/priority_scheduler.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/kernel/error.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/kernel/kernel.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/kernel/pthread.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/kernel/stage_2_boot.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/kernel/sync.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/kernel/timeconversion.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/util/util.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/stdlib_integration/libc_integration.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/stdlib_integration/libstdcpp_integration.cpp
    )

    # Common Miosix include directories
    set(MIOSIX_COMMON_INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/config
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/arch/common
        ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/util
    )

    # Common Miosix definitions
    set(MIOSIX_COMMON_DEFS
        DONT_USE_CMSIS_INIT
        COMPILING_MIOSIX
        _POSIX_PRIORITY_SCHEDULING
    )

    # MCU-specific includes and definitions needed by miosix
    set(MCU_INCLUDES "")
    set(MCU_DEFS "")
    if(OPENRTX_TARGET MATCHES "^md3x0|^mduv3x0|^md9600|^dm1701|^cs7000$")
        set(MCU_INCLUDES
            ${CMAKE_SOURCE_DIR}/platform/mcu/CMSIS/Include
            ${CMAKE_SOURCE_DIR}/platform/mcu/CMSIS/Device/ST/STM32F4xx/Include
            ${CMAKE_SOURCE_DIR}/platform/mcu/STM32F4xx
            ${CMAKE_SOURCE_DIR}/platform/mcu/STM32F4xx/drivers
            ${CMAKE_SOURCE_DIR}/platform/mcu/STM32F4xx/drivers/usb
        )
        set(MCU_DEFS STM32F405xx HSE_VALUE=8000000)
    elseif(OPENRTX_TARGET MATCHES "^gd77|^dm1801")
        set(MCU_INCLUDES
            ${CMAKE_SOURCE_DIR}/platform/mcu/CMSIS/Include
            ${CMAKE_SOURCE_DIR}/platform/mcu/CMSIS/Device/NXP/MK22FN512xxx12/Include
            ${CMAKE_SOURCE_DIR}/platform/mcu/MK22FN512xxx12
            ${CMAKE_SOURCE_DIR}/platform/mcu/MK22FN512xxx12/drivers
        )
        set(MCU_DEFS MK22FN512xx)
    elseif(OPENRTX_TARGET STREQUAL "mod17")
        set(MCU_INCLUDES
            ${CMAKE_SOURCE_DIR}/platform/mcu/CMSIS/Include
            ${CMAKE_SOURCE_DIR}/platform/mcu/CMSIS/Device/ST/STM32F4xx/Include
            ${CMAKE_SOURCE_DIR}/platform/mcu/STM32F4xx
            ${CMAKE_SOURCE_DIR}/platform/mcu/STM32F4xx/drivers
        )
        set(MCU_DEFS STM32F405xx HSE_VALUE=8000000)
    elseif(OPENRTX_TARGET STREQUAL "cs7000p")
        set(MCU_INCLUDES
            ${CMAKE_SOURCE_DIR}/platform/mcu/CMSIS/Include
            ${CMAKE_SOURCE_DIR}/platform/mcu/CMSIS/Device/ST/STM32H7xx/Include
            ${CMAKE_SOURCE_DIR}/platform/mcu/STM32H7xx
            ${CMAKE_SOURCE_DIR}/platform/mcu/STM32H7xx/drivers
        )
        set(MCU_DEFS STM32H743xx)
    endif()

    # Architecture-specific sources and includes
    if(OPENRTX_ARCH STREQUAL "CM4F")
        target_sources(openrtx_miosix PRIVATE
            ${MIOSIX_COMMON_SOURCES}
            ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/arch/cortexM4F/portability.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/arch/common/core/interrupts_cortexMx.cpp
        )
        target_include_directories(openrtx_miosix PUBLIC
            ${MIOSIX_COMMON_INCLUDES}
            ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/arch/cortexM4F
            ${MCU_INCLUDES}
        )
        target_compile_definitions(openrtx_miosix PUBLIC
            ${MIOSIX_COMMON_DEFS}
            ${MCU_DEFS}
            _ARCH_CORTEXM4
        )
    elseif(OPENRTX_ARCH STREQUAL "CM7")
        target_sources(openrtx_miosix PRIVATE
            ${MIOSIX_COMMON_SOURCES}
            ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/arch/cortexM7/portability.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/arch/common/core/cache_cortexMx.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/arch/common/core/mpu_cortexMx.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/arch/common/core/interrupts_cortexMx.cpp
        )
        target_include_directories(openrtx_miosix PUBLIC
            ${MIOSIX_COMMON_INCLUDES}
            ${CMAKE_CURRENT_SOURCE_DIR}/miosix-kernel/miosix/arch/cortexM7
            ${MCU_INCLUDES}
        )
        target_compile_definitions(openrtx_miosix PUBLIC
            ${MIOSIX_COMMON_DEFS}
            ${MCU_DEFS}
            _ARCH_CORTEXM7
        )
    endif()
endif()
