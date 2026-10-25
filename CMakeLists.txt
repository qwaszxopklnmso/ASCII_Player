cmake_minimum_required(VERSION 3.20)
project(AsciiPlayer LANGUAGES CXX)

add_executable(ASCIIPlayer
    src/audio.cpp
    src/common.cpp
    src/json.cpp
    src/main.cpp
    src/media.cpp
    src/process.cpp
    src/render.cpp
)

target_compile_features(ASCIIPlayer PRIVATE cxx_std_17)
target_compile_definitions(ASCIIPlayer PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(ASCIIPlayer PRIVATE /utf-8 /W3 /EHsc)
    set_property(TARGET ASCIIPlayer PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_compile_options(ASCIIPlayer PRIVATE -O2 -Wall)
endif()

target_link_libraries(ASCIIPlayer PRIVATE winmm)
