cmake_minimum_required(VERSION 3.24)

project(RasterNoteNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(RasterNoteNative WIN32
    src/main.cpp
    src/note_app.cpp
    src/note_store.cpp
    src/text_document.cpp
)

target_compile_definitions(RasterNoteNative PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(RasterNoteNative PRIVATE
        /W4
        /permissive-
        /EHsc
        /Zc:__cplusplus
    )
else()
    target_compile_options(RasterNoteNative PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

target_include_directories(RasterNoteNative PRIVATE src)

target_link_libraries(RasterNoteNative PRIVATE
    d2d1
    dwrite
    dwmapi
    imm32
    ole32
    shell32
)
