cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBMP3LAME_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/enc_lame.c
)

SET(LIBMP3LAME_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libmp3lame.a
)

SET(LIBMP3LAME_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libmp3lame
        "${LIBMP3LAME_SRC}"
        "${LIBMP3LAME_LIB}"
        []
        ""
        "${LIBMP3LAME_INC}"
        ""
        "1")
