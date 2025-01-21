cmake_minimum_required(VERSION 3.29)
project(simpleplayer)

set(CMAKE_CXX_STANDARD 20)

# Creating the DLL
add_library(simpleplayer SHARED
        library.cpp
        library.h
)

# Link with necessary Media Foundation libraries
target_link_libraries(simpleplayer PRIVATE
        mfplay
        mf
        mfplat
        mfuuid
        shlwapi
        strmiids
        ole32
        oleaut32
)

target_compile_definitions(simpleplayer PRIVATE
        WINVER=_WIN32_WINNT_WIN10
        _WIN32_WINNT=0x0A00
        WIN32
        UNICODE
        _UNICODE
        MEDIAPLAYER_EXPORTS
)