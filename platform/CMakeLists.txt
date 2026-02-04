# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

##
## Platform library - MCU and target-specific sources
##

add_library(openrtx_platform STATIC)

# Common include directories
target_include_directories(openrtx_platform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/openrtx/include
        ${CMAKE_SOURCE_DIR}/lib/minmea/include
        ${CMAKE_SOURCE_DIR}/lib/qdec/include
)

# Apply target definitions
target_compile_definitions(openrtx_platform PUBLIC ${OPENRTX_TARGET_DEFS})

##
## TYT MDx family sources
##
set(MDX_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/W25Qx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/nvmem_settings_MDx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/nvmem_MDx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/audio_MDx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_Cx000.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_C6000.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/tones/toneGenerator_MDx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_custom.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_bitbang.c
)

##
## GDx family sources (Radioddity GD-77 and Baofeng DM-1801)
##
set(GDX_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/targets/GDx/platform.c
    ${CMAKE_CURRENT_SOURCE_DIR}/targets/GDx/hwconfig.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/W25Qx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/AT24Cx_GDx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/nvmem_GDx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/CPS/cps_io_native_GDx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/ADC/ADC0_GDx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/backlight/backlight_GDx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/radio_GDx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_Cx000.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_C6000.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/AT1846S_GDx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_C6000_GDx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display/UC1701_GDx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/keyboard/keyboard_GDx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/audio_GDx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_custom.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_bitbang.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_mk22.c
)

##
## STM32F405 MCU sources
##
set(STM32F405_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/boot/startup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/boot/bsp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/boot/libc_integration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/usb/usb_bsp.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/usb/usb_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/usb/usb_dcd.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/usb/usb_dcd_int.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/usb/usbd_desc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/usb/usbd_core.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/usb/usbd_ioreq.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/usb/usbd_req.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/usb/usbd_usr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/usb_vcom.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/delays.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/rtc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/USART3.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/rng.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/rcc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/i2c_stm32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/ADC/adc_stm32f4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_stm32f4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPIO/gpio_stm32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/stm32_dac.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/stm32_adc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/stm32_pwm.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/keyboard/cap1206.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/CMSIS/Device/ST/STM32F4xx/Source/system_stm32f4xx.c
)

set(STM32F405_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/CMSIS/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/CMSIS/Device/ST/STM32F4xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32F4xx/drivers/usb
)

##
## MK22FN512 MCU sources
##
set(MK22FN512_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/boot/startup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/boot/bsp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/boot/libc_integration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/drivers/delays.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/drivers/I2C0.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/drivers/rng.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/drivers/usb/usb_device_cdc_acm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/drivers/usb/usb_device_ch9.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/drivers/usb/usb_device_dci.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/drivers/usb/usb_device_descriptor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/drivers/usb/usb_device_khci.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/drivers/usb/usb_osa_bm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/drivers/usb_vcom.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_mk22.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPIO/gpio_mk22.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/CMSIS/Device/NXP/MK22FN512xxx12/Source/system_MK22F51212.c
)

set(MK22FN512_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/CMSIS/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/CMSIS/Device/NXP/MK22FN512xxx12/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/MK22FN512xxx12/drivers
)

##
## STM32H743 MCU sources
##
set(STM32H743_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32H7xx/boot/startup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32H7xx/boot/bsp.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32H7xx/boot/libc_integration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32H7xx/drivers/rcc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32H7xx/drivers/delays.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPIO/gpio_stm32.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/ADC/adc_stm32h7.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_stm32h7.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPS/gps_stm32.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPS/nmea_rbuf.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/stm32_dac.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/stm32_adc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/CMSIS/Device/ST/STM32H7xx/Source/system_stm32h7xx.c
)

set(STM32H743_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/CMSIS/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/CMSIS/Device/ST/STM32H7xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32H7xx
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/STM32H7xx/drivers
)

##
## Linux platform sources
##
set(LINUX_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/targets/linux/emulator/emulator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/targets/linux/emulator/sdl_engine.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display/display_libSDL.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/keyboard/keyboard_linux.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/nvmem_linux.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPS/gps_linux.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/x86_64/drivers/delays.c
    ${CMAKE_CURRENT_SOURCE_DIR}/mcu/x86_64/drivers/rng.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/radio_linux.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/audio_linux.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/file_source.c
    ${CMAKE_CURRENT_SOURCE_DIR}/targets/linux/platform.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/CPS/cps_io_libc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/posix_file.c
)

set(LINUX_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/targets/linux
    ${CMAKE_CURRENT_SOURCE_DIR}/targets/linux/emulator
)

##
## Per-target source selection
##

if(OPENRTX_TARGET MATCHES "^linux")
    target_sources(openrtx_platform PRIVATE ${LINUX_SOURCES})
    target_include_directories(openrtx_platform PUBLIC ${LINUX_INCLUDES})

    # Find SDL2 and threads
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(openrtx_platform PUBLIC SDL2::SDL2 Threads::Threads m readline)

elseif(OPENRTX_TARGET STREQUAL "md3x0")
    target_sources(openrtx_platform PRIVATE
        ${STM32F405_SOURCES}
        ${MDX_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/CPS/cps_io_native_MD3x0.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/SKY72310.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/radio_MD3x0.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_C5000_MDx.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/keyboard/keyboard_MD3x.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display/HX8353_MD3x.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/backlight/backlight_MDx.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPS/gps_stm32.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPS/nmea_rbuf.c
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/MD-3x0/platform.c
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/MD-3x0/hwconfig.c
    )
    target_include_directories(openrtx_platform PUBLIC
        ${STM32F405_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/MD-3x0
    )
    target_link_options(openrtx_platform INTERFACE -T${OPENRTX_LINKER_SCRIPT})

elseif(OPENRTX_TARGET STREQUAL "mduv3x0")
    target_sources(openrtx_platform PRIVATE
        ${STM32F405_SOURCES}
        ${MDX_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/CPS/cps_io_native_MDUV3x0.c
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/MD-UV3x0/platform.c
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/MD-UV3x0/hwconfig.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPS/gps_stm32.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPS/nmea_rbuf.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/keyboard/keyboard_MD3x.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display/HX8353_MD3x.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/backlight/backlight_MDx.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/chSelector/chSelector_UV3x0.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/Cx000_dac.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/radio_UV3x0.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/AT1846S_UV3x0.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_C6000_UV3x0.cpp
    )
    target_include_directories(openrtx_platform PUBLIC
        ${STM32F405_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/MD-UV3x0
    )
    target_link_options(openrtx_platform INTERFACE -T${OPENRTX_LINKER_SCRIPT})

elseif(OPENRTX_TARGET STREQUAL "md9600")
    target_sources(openrtx_platform PRIVATE
        ${STM32F405_SOURCES}
        ${MDX_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/MD-9600/platform.c
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/MD-9600/hwconfig.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPS/gps_stm32.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPS/nmea_rbuf.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display/ST7567_MD9600.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/keyboard/keyboard_MD9600.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/backlight/backlight_MDx.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/chSelector/chSelector_MD9600.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/radio_MD9600.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/CPS/cps_io_native_MD9600.c
    )
    target_include_directories(openrtx_platform PUBLIC
        ${STM32F405_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/MD-9600
    )
    target_link_options(openrtx_platform INTERFACE -T${OPENRTX_LINKER_SCRIPT})

elseif(OPENRTX_TARGET STREQUAL "gd77")
    target_sources(openrtx_platform PRIVATE ${MK22FN512_SOURCES} ${GDX_SOURCES})
    target_include_directories(openrtx_platform PUBLIC
        ${MK22FN512_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/GDx
    )
    target_link_options(openrtx_platform INTERFACE -T${OPENRTX_LINKER_SCRIPT})

elseif(OPENRTX_TARGET STREQUAL "dm1801")
    target_sources(openrtx_platform PRIVATE ${MK22FN512_SOURCES} ${GDX_SOURCES})
    target_include_directories(openrtx_platform PUBLIC
        ${MK22FN512_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/GDx
    )
    target_link_options(openrtx_platform INTERFACE -T${OPENRTX_LINKER_SCRIPT})

elseif(OPENRTX_TARGET STREQUAL "dm1701")
    target_sources(openrtx_platform PRIVATE
        ${STM32F405_SOURCES}
        ${MDX_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/DM-1701/platform.c
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/DM-1701/hwconfig.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/keyboard/keyboard_DM1701.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/CPS/cps_io_native_MDUV3x0.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display/HX8353_MD3x.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/backlight/backlight_MDx.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/chSelector/chSelector_UV3x0.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/Cx000_dac.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/radio_UV3x0.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/AT1846S_UV3x0.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_C6000_UV3x0.cpp
    )
    target_include_directories(openrtx_platform PUBLIC
        ${STM32F405_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/DM-1701
    )
    target_link_options(openrtx_platform INTERFACE -T${OPENRTX_LINKER_SCRIPT})

elseif(OPENRTX_TARGET STREQUAL "mod17")
    target_sources(openrtx_platform PRIVATE
        ${STM32F405_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/Module17/platform.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display/display_Mod17.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display/SH110x_Mod17.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display/SSD1309_Mod17.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/keyboard/keyboard_Mod17.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/nvmem_Mod17.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/CPS/cps_io_native_Mod17.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/radio_Mod17.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/audio_Mod17.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/MAX9814_Mod17.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/MCP4551.c
    )
    target_include_directories(openrtx_platform PUBLIC
        ${STM32F405_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/Module17
    )
    target_link_options(openrtx_platform INTERFACE -T${OPENRTX_LINKER_SCRIPT})

elseif(OPENRTX_TARGET STREQUAL "cs7000")
    target_sources(openrtx_platform PRIVATE
        ${STM32F405_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/nvmem_CS7000.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/W25Qx.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/eeep.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/stubs/cps_io_stub.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/AK2365A.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/SKY72310.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_C6000.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_Cx000.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_C6000_CS7000.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/radio_CS7000.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/keyboard/keyboard_CS7000.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display/ST7735R_CS7000.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/backlight/backlight_CS7000.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/Cx000_dac.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/audio_CS7000.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPIO/gpio_shiftReg.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_custom.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_bitbang.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPS/gps_stm32.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPS/nmea_rbuf.c
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/CS7000/hwconfig.c
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/CS7000/platform.c
    )
    target_include_directories(openrtx_platform PUBLIC
        ${STM32F405_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/CS7000
    )
    target_link_options(openrtx_platform INTERFACE -T${OPENRTX_LINKER_SCRIPT})

elseif(OPENRTX_TARGET STREQUAL "cs7000p")
    target_sources(openrtx_platform PRIVATE
        ${STM32H743_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/nvmem_CS7000.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/W25Qx.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/NVM/eeep.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_C6000.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_Cx000.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/HR_C6000_CS7000.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/radio_CS7000.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/AK2365A.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/baseband/SKY72310.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/Cx000_dac.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/audio/audio_CS7000.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/stubs/cps_io_stub.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/display/ST7735R_CS7000.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/keyboard/keyboard_CS7000.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/backlight/backlight_CS7000.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/GPIO/gpio_shiftReg.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_custom.c
        ${CMAKE_CURRENT_SOURCE_DIR}/drivers/SPI/spi_bitbang.c
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/CS7000-PLUS/hwconfig.c
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/CS7000-PLUS/platform.c
    )
    target_include_directories(openrtx_platform PUBLIC
        ${STM32H743_INCLUDES}
        ${CMAKE_CURRENT_SOURCE_DIR}/targets/CS7000-PLUS
    )
    target_link_options(openrtx_platform INTERFACE
        -T${OPENRTX_LINKER_SCRIPT}
        -Wl,--print-memory-usage
    )

endif()

# Link to miosix for embedded targets to get RTOS headers
if(NOT OPENRTX_TARGET MATCHES "^linux")
    target_link_libraries(openrtx_platform PUBLIC openrtx_miosix)
endif()
