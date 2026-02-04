# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

##
## TargetOptions.cmake - Per-target compile definitions and settings
##

# Target-specific definitions
if(OPENRTX_TARGET STREQUAL "linux")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_LINUX
        VP_USE_FILESYSTEM
        CONFIG_SCREEN_WIDTH=160
        CONFIG_SCREEN_HEIGHT=128
        CONFIG_PIX_FMT_RGB565
        CONFIG_GPS
        CONFIG_RTC
        sniprintf=snprintf
        vsniprintf=vsnprintf
    )
    set(OPENRTX_USE_UI "default")

elseif(OPENRTX_TARGET STREQUAL "linux_smallscreen")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_LINUX
        VP_USE_FILESYSTEM
        CONFIG_SCREEN_WIDTH=128
        CONFIG_SCREEN_HEIGHT=64
        CONFIG_PIX_FMT_BW
        CONFIG_GPS
        CONFIG_RTC
        sniprintf=snprintf
        vsniprintf=vsnprintf
    )
    set(OPENRTX_USE_UI "default")

elseif(OPENRTX_TARGET STREQUAL "linux_mod17")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_LINUX
        VP_USE_FILESYSTEM
        CONFIG_SCREEN_WIDTH=128
        CONFIG_SCREEN_HEIGHT=64
        CONFIG_PIX_FMT_BW
        sniprintf=snprintf
        vsniprintf=vsnprintf
    )
    set(OPENRTX_USE_UI "module17")

elseif(OPENRTX_TARGET STREQUAL "md3x0")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_MD3x0
        STM32F405xx
        HSE_VALUE=8000000
        timegm=mktime
    )
    set(OPENRTX_USE_UI "default")
    set(OPENRTX_MCU "STM32F405")
    set(OPENRTX_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/platform/mcu/STM32F4xx/linker_script_MDx.ld")
    set(OPENRTX_RADIO_FAMILY "MDx")
    set(OPENRTX_WRAP_TYPE "MD380")
    set(OPENRTX_LOAD_ADDR "0x0800C000")

elseif(OPENRTX_TARGET STREQUAL "mduv3x0")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_MDUV3x0
        STM32F405xx
        HSE_VALUE=8000000
        timegm=mktime
    )
    set(OPENRTX_USE_UI "default")
    set(OPENRTX_MCU "STM32F405")
    set(OPENRTX_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/platform/mcu/STM32F4xx/linker_script_MDx.ld")
    set(OPENRTX_RADIO_FAMILY "MDx")
    set(OPENRTX_WRAP_TYPE "UV3X0")
    set(OPENRTX_LOAD_ADDR "0x0800C000")

elseif(OPENRTX_TARGET STREQUAL "md9600")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_MD9600
        STM32F405xx
        HSE_VALUE=8000000
    )
    set(OPENRTX_USE_UI "default")
    set(OPENRTX_MCU "STM32F405")
    set(OPENRTX_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/platform/mcu/STM32F4xx/linker_script_MDx.ld")
    set(OPENRTX_RADIO_FAMILY "MDx")
    set(OPENRTX_WRAP_TYPE "MD9600")
    set(OPENRTX_LOAD_ADDR "0x0800C000")

elseif(OPENRTX_TARGET STREQUAL "gd77")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_GD77
        MK22FN512xx
    )
    set(OPENRTX_USE_UI "default")
    set(OPENRTX_MCU "MK22FN512")
    set(OPENRTX_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/platform/mcu/MK22FN512xxx12/linker_script.ld")
    set(OPENRTX_RADIO_FAMILY "GDx")
    set(OPENRTX_WRAP_TYPE "GD-77")
    set(OPENRTX_LOAD_ADDR "0x0800C000")

elseif(OPENRTX_TARGET STREQUAL "dm1801")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_DM1801
        MK22FN512xx
    )
    set(OPENRTX_USE_UI "default")
    set(OPENRTX_MCU "MK22FN512")
    set(OPENRTX_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/platform/mcu/MK22FN512xxx12/linker_script.ld")
    set(OPENRTX_RADIO_FAMILY "GDx")
    set(OPENRTX_WRAP_TYPE "DM-1801")
    set(OPENRTX_LOAD_ADDR "0x0800C000")

elseif(OPENRTX_TARGET STREQUAL "dm1701")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_DM1701
        STM32F405xx
        HSE_VALUE=8000000
        timegm=mktime
    )
    set(OPENRTX_USE_UI "default")
    set(OPENRTX_MCU "STM32F405")
    set(OPENRTX_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/platform/mcu/STM32F4xx/linker_script_MDx.ld")
    set(OPENRTX_RADIO_FAMILY "MDx")
    set(OPENRTX_WRAP_TYPE "DM1701")
    set(OPENRTX_LOAD_ADDR "0x0800C000")

elseif(OPENRTX_TARGET STREQUAL "mod17")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_MOD17
        STM32F405xx
        HSE_VALUE=8000000
    )
    set(OPENRTX_USE_UI "module17")
    set(OPENRTX_MCU "STM32F405")
    set(OPENRTX_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/platform/mcu/STM32F4xx/stm32_1m+192k_rom.ld")
    set(OPENRTX_LOAD_ADDR "0x08000000")

elseif(OPENRTX_TARGET STREQUAL "cs7000")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_CS7000
        STM32F405xx
        HSE_VALUE=8000000
        timegm=mktime
    )
    set(OPENRTX_USE_UI "default")
    set(OPENRTX_MCU "STM32F405")
    set(OPENRTX_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/platform/mcu/STM32F4xx/stm32_1m+192k_rom.ld")
    set(OPENRTX_WRAP_TYPE "cs7000")
    set(OPENRTX_LOAD_ADDR "0x08000000")

elseif(OPENRTX_TARGET STREQUAL "cs7000p")
    set(OPENRTX_TARGET_DEFS
        PLATFORM_CS7000P
        STM32H743xx
        HSE_VALUE=25000000
        timegm=mktime
    )
    set(OPENRTX_USE_UI "default")
    set(OPENRTX_MCU "STM32H743")
    set(OPENRTX_LINKER_SCRIPT "${CMAKE_SOURCE_DIR}/platform/mcu/STM32H7xx/linker_script_cs7000p.ld")
    set(OPENRTX_LOAD_ADDR "0x08100000")

endif()
