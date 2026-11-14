cmake_minimum_required(VERSION 3.10)
project(ctw C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 14)

if(NOT CTW_ARCH)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
        set(CTW_ARCH aarch64)
    else()
        set(CTW_ARCH x86_64)
    endif()
endif()

set(ARCH_DIR ${CMAKE_SOURCE_DIR}/arch/${CTW_ARCH})

if(NOT EXISTS ${ARCH_DIR})
    message(FATAL_ERROR "Unsupported CTW_ARCH '${CTW_ARCH}', expected 'aarch64' or 'x86_64'")
endif()

add_executable(ctw
    src/egl_shim.c
    src/jni_shim.c
    src/error.c
    src/util.c
    src/music_player.c
    src/action_tree_helper.cpp
    ${ARCH_DIR}/pthread_bridge.c
    ${ARCH_DIR}/so_util.c
    ${ARCH_DIR}/imports.c
    ${ARCH_DIR}/main.c
)

target_include_directories(ctw PRIVATE ${ARCH_DIR} src)

target_link_libraries(ctw PRIVATE
    SDL2
    EGL
    GLESv2
    openal
    mpg123
    m
    pthread
    dl
    stdc++
)

if(CTW_ARCH STREQUAL aarch64)
    target_sources(ctw PRIVATE ${ARCH_DIR}/movie_player.c)
    target_link_libraries(ctw PRIVATE
        avformat
        avcodec
        swresample
        avutil
        -Wl,--allow-shlib-undefined
    )
else()
    target_sources(ctw PRIVATE src/debug_hooks.c src/zip_fs.c)
endif()
