# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
#
# SPDX-License-Identifier: GPL-3.0-or-later

##
## Subprojects - external dependencies
##

##
## Codec2 - Speech codec
##
if(OPENRTX_TARGET MATCHES "^linux")
    # For Linux, use system codec2 library
    find_library(CODEC2_LIBRARY codec2 REQUIRED)
    find_path(CODEC2_INCLUDE_DIR codec2/codec2.h)

    add_library(openrtx_codec2 INTERFACE)
    target_link_libraries(openrtx_codec2 INTERFACE ${CODEC2_LIBRARY})
    if(CODEC2_INCLUDE_DIR)
        target_include_directories(openrtx_codec2 INTERFACE ${CODEC2_INCLUDE_DIR})
    endif()
else()
    # For embedded, compile codec2 from source
    add_library(openrtx_codec2 STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/dump.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/lpc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/nlp.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/phase.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/quantise.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/postfilter.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codec2.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codec2_fft.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/lsp.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/sine.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/interp.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/kiss_fft.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/kiss_fftr.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/newamp1.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codebook.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codebookd.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/pack.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codebooknewamp1.c
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src/codebooknewamp1_energy.c
    )

    target_include_directories(openrtx_codec2 PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2
        ${CMAKE_CURRENT_SOURCE_DIR}/codec2/src
    )

    target_compile_definitions(openrtx_codec2 PRIVATE
        CODEC2_MODE_EN_DEFAULT=0
        FREEDV_MODE_EN_DEFAULT=0
        CODEC2_MODE_3200_EN=1
    )
endif()

##
## XPowersLib - Power management IC library (for targets that need it)
##
# Currently only used by Zephyr targets, so we skip it here
