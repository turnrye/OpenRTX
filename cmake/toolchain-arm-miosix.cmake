# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

##
## toolchain-arm-miosix.cmake - Cortex-M4 cross-compilation toolchain
##

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# Specify the cross compiler
set(CMAKE_C_COMPILER arm-miosix-eabi-gcc)
set(CMAKE_CXX_COMPILER arm-miosix-eabi-g++)
set(CMAKE_ASM_COMPILER arm-miosix-eabi-gcc)
set(CMAKE_AR arm-miosix-eabi-ar)
set(CMAKE_OBJCOPY arm-miosix-eabi-objcopy)
set(CMAKE_OBJDUMP arm-miosix-eabi-objdump)
set(CMAKE_SIZE arm-miosix-eabi-size)

# CPU flags for Cortex-M4 with FPU
set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16")

# Common C flags
set(CMAKE_C_FLAGS_INIT
    "${CPU_FLAGS} -D_DEFAULT_SOURCE=1 -ffunction-sections -fdata-sections -Wall -Werror=return-type -g -Os"
)

# Common C++ flags
set(CMAKE_CXX_FLAGS_INIT
    "${CPU_FLAGS} -D_DEFAULT_SOURCE=1 -ffunction-sections -fdata-sections -Wall -Werror=return-type -g -fno-exceptions -fno-rtti -D__NO_EXCEPTIONS -Os"
)

# Common linker flags
# Note: -lstdc++ -lc -lm -lgcc -latomic must be linked at the END (after all objects)
# They are added in the main CMakeLists.txt via target_link_libraries with AFTER keyword
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "${CPU_FLAGS} -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti -Wl,-L${CMAKE_SOURCE_DIR}/lib/miosix-kernel -Wl,--gc-sections -Wl,-Map,main.map -Wl,--warn-common -Wl,--print-memory-usage -nostdlib"
)

# Disable compiler tests (we're cross-compiling)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Search paths
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
