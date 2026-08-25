cmake_minimum_required(VERSION 3.25)

project(SYXG100VlWorker VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "VL worker requires a 64-bit Windows target")
endif()
if(NOT DEFINED UNICORN_ROOT)
    message(FATAL_ERROR "Set UNICORN_ROOT to a Unicorn distribution")
endif()
if(NOT DEFINED UNICORN_LIBRARY)
    set(UNICORN_LIBRARY "${UNICORN_ROOT}/lib/unicorn.lib")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(VlWorker
    VlWorker.cpp
)
target_include_directories(VlWorker PRIVATE "${UNICORN_ROOT}/include")
target_link_libraries(VlWorker PRIVATE "${UNICORN_LIBRARY}")
target_compile_definitions(VlWorker PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(VlWorker PRIVATE -static -static-libgcc -static-libstdc++)
