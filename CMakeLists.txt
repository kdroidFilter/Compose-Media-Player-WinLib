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

# Lien avec Media Foundation et WASAPI - ajout de mfsensorgroup et dxva2
target_link_libraries(OffscreenPlayer PRIVATE
        mf
        mfplat
        mfreadwrite
        mfuuid
        wmcodecdspuuid
        ole32
        oleaut32
        avrt
        mfsensorgroup  # Pour support codec supplémentaires
        dxva2          # Pour le décodage matériel H.264
)