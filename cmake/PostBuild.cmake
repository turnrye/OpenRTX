# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

##
## PostBuild.cmake - Post-build targets for flashing and wrapping
##

# Find required tools
find_program(OBJCOPY_EXECUTABLE
    NAMES arm-miosix-eabi-objcopy objcopy
    DOC "Path to objcopy"
)

# Look for radio_tool in subprojects first, then in PATH
find_program(RADIO_TOOL_EXECUTABLE
    NAMES radio_tool
    PATHS ${CMAKE_SOURCE_DIR}/subprojects/radio_tool
    DOC "Path to radio_tool"
)
find_program(DFU_UTIL_EXECUTABLE dfu-util DOC "Path to dfu-util")

# For Linux targets, no post-build steps needed
if(OPENRTX_TARGET MATCHES "^linux")
    return()
endif()

# Create binary target
if(OBJCOPY_EXECUTABLE)
    add_custom_target(openrtx_${OPENRTX_TARGET}_bin
        COMMAND ${OBJCOPY_EXECUTABLE} -O binary
            $<TARGET_FILE:openrtx_${OPENRTX_TARGET}>
            ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_bin
        DEPENDS openrtx_${OPENRTX_TARGET}
        BYPRODUCTS ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_bin
        COMMENT "Creating binary for ${OPENRTX_TARGET}"
    )
endif()

# Target-specific wrap and flash targets
if(OPENRTX_TARGET STREQUAL "gd77" OR OPENRTX_TARGET STREQUAL "dm1801")
    # GD-77 and DM-1801 use bin2sgl for wrapping
    set(BIN2SGL_EXECUTABLE ${CMAKE_SOURCE_DIR}/scripts/bin2sgl.py)
    set(GD77_LOADER_EXECUTABLE ${CMAKE_SOURCE_DIR}/scripts/gd-77_firmware_loader.py)

    if(EXISTS ${BIN2SGL_EXECUTABLE})
        add_custom_target(openrtx_${OPENRTX_TARGET}_wrap
            COMMAND python3 ${BIN2SGL_EXECUTABLE}
                --in ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_bin
                --out ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_bin.sgl
                --mode ${OPENRTX_WRAP_TYPE}
            DEPENDS openrtx_${OPENRTX_TARGET}_bin
            BYPRODUCTS ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_bin.sgl
            COMMENT "Wrapping ${OPENRTX_TARGET} firmware"
        )

        if(EXISTS ${GD77_LOADER_EXECUTABLE})
            add_custom_target(openrtx_${OPENRTX_TARGET}_flash
                COMMAND python3 ${GD77_LOADER_EXECUTABLE}
                    -f ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_bin.sgl
                    -m ${OPENRTX_WRAP_TYPE}
                DEPENDS openrtx_${OPENRTX_TARGET}_wrap
                COMMENT "Flashing ${OPENRTX_TARGET}"
            )
        endif()
    endif()

elseif(OPENRTX_TARGET STREQUAL "mod17")
    # Module17 uses dfu-util for flashing
    if(DFU_UTIL_EXECUTABLE)
        add_custom_target(openrtx_${OPENRTX_TARGET}_flash
            COMMAND ${DFU_UTIL_EXECUTABLE}
                -d 0483:df11
                -a 0
                -D ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_bin
                -s 0x08000000
            DEPENDS openrtx_${OPENRTX_TARGET}_bin
            COMMENT "Flashing ${OPENRTX_TARGET} via DFU"
        )
    endif()

elseif(OPENRTX_TARGET STREQUAL "cs7000" OR OPENRTX_TARGET STREQUAL "cs7000p")
    # CS7000 uses custom wrapper and dfu-util
    set(CS7000_WRAP_EXECUTABLE ${CMAKE_SOURCE_DIR}/scripts/cs7000_wrap.py)
    set(DFU_CONVERT_EXECUTABLE ${CMAKE_SOURCE_DIR}/scripts/dfu-convert.py)

    if(EXISTS ${DFU_CONVERT_EXECUTABLE})
        add_custom_target(openrtx_${OPENRTX_TARGET}_dfu
            COMMAND python3 ${DFU_CONVERT_EXECUTABLE}
                -b ${OPENRTX_LOAD_ADDR}:${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_bin
                ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_dfu
            DEPENDS openrtx_${OPENRTX_TARGET}_bin
            BYPRODUCTS ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_dfu
            COMMENT "Creating DFU file for ${OPENRTX_TARGET}"
        )
    endif()

    if(EXISTS ${CS7000_WRAP_EXECUTABLE})
        add_custom_target(openrtx_${OPENRTX_TARGET}_wrap
            COMMAND python3 ${CS7000_WRAP_EXECUTABLE}
                ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_bin
                ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_wrap
            DEPENDS openrtx_${OPENRTX_TARGET}_bin
            BYPRODUCTS ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_wrap
            COMMENT "Wrapping ${OPENRTX_TARGET} firmware"
        )
    endif()

    if(DFU_UTIL_EXECUTABLE)
        add_custom_target(openrtx_${OPENRTX_TARGET}_flash
            COMMAND ${DFU_UTIL_EXECUTABLE}
                -d 0483:df11
                -a 0
                -D ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_bin
                -s ${OPENRTX_LOAD_ADDR}
            DEPENDS openrtx_${OPENRTX_TARGET}_bin
            COMMENT "Flashing ${OPENRTX_TARGET} via DFU"
        )
    endif()

else()
    # MD3x0, MDUV3x0, MD9600, DM1701 use radio_tool
    if(RADIO_TOOL_EXECUTABLE)
        add_custom_target(openrtx_${OPENRTX_TARGET}_wrap
            COMMAND ${RADIO_TOOL_EXECUTABLE}
                --wrap
                -o ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_wrap
                -r ${OPENRTX_WRAP_TYPE}
                -s ${OPENRTX_LOAD_ADDR}:${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_bin
            DEPENDS openrtx_${OPENRTX_TARGET}_bin
            BYPRODUCTS ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_wrap
            COMMENT "Wrapping ${OPENRTX_TARGET} firmware"
        )

        add_custom_target(openrtx_${OPENRTX_TARGET}_flash
            COMMAND ${RADIO_TOOL_EXECUTABLE}
                -d 0
                -f
                -i ${CMAKE_BINARY_DIR}/openrtx_${OPENRTX_TARGET}_wrap
            DEPENDS openrtx_${OPENRTX_TARGET}_wrap
            COMMENT "Flashing ${OPENRTX_TARGET}"
        )
    endif()
endif()
