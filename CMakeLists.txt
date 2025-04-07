cmake_minimum_required(VERSION 3.15)
project(NativeVideoPlayer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

add_library(NativeVideoPlayer SHARED
        NativeVideoPlayer.cpp
        NativeVideoPlayer.h
)

target_compile_definitions(NativeVideoPlayer PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        NATIVEVIDEOPLAYER_EXPORTS
)

# Link with Media Foundation and WASAPI - adding mfsensorgroup and dxva2 for additional codec support
target_link_libraries(NativeVideoPlayer PRIVATE
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
        d3d11     # Added D3D11 library
        dxgi
)
