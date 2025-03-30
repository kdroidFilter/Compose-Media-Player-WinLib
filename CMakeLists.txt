cmake_minimum_required(VERSION 3.15)
project(OffscreenPlayer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

add_library(OffscreenPlayer SHARED
        OffscreenPlayer.cpp
        OffscreenPlayer.h
)

target_compile_definitions(OffscreenPlayer PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        OFFSCREENPLAYER_EXPORTS
)

# Link with Media Foundation and WASAPI - adding mfsensorgroup and dxva2 for additional codec support
target_link_libraries(OffscreenPlayer PRIVATE
        mf
        mfplat
        mfreadwrite
        mfuuid
        wmcodecdspuuid
        ole32
        oleaut32
        avrt
        mfsensorgroup  # For additional codec support
        dxva2         # For H.264 hardware decoding
)
