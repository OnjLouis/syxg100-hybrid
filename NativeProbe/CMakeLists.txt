cmake_minimum_required(VERSION 3.25)

project(SYXG100NativeProbe VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Native VL probe requires a 32-bit Windows target")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(VlNativeProbe
    VlNativeProbe.cpp
    ../Source/LeImageLoader.cpp
    ../Source/LeImageLoader.h
)
target_compile_definitions(VlNativeProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(VlNativeProbe PRIVATE -static -static-libgcc -static-libstdc++)
