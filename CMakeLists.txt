cmake_minimum_required(VERSION 3.25)

project(SYXG100Hybrid VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "S-YXG100 Hybrid currently requires a 32-bit Windows target")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(SYXG100Hybrid SHARED
    Source/MidiRouter.cpp
    Source/MidiRouter.h
    Source/HybridVst.cpp
    Source/Vst2Abi.h
    Source/exports.def
)
set_target_properties(SYXG100Hybrid PROPERTIES
    OUTPUT_NAME "syxg100-hybrid"
    PREFIX ""
)
target_compile_definitions(SYXG100Hybrid PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(SYXG100Hybrid PRIVATE -static -static-libgcc -static-libstdc++)

add_executable(HybridHostProbe
    Tests/HybridHostProbe.cpp
    Source/Vst2Abi.h
)
target_compile_definitions(HybridHostProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_options(HybridHostProbe PRIVATE -static -static-libgcc -static-libstdc++)

enable_testing()
add_executable(MidiRouterTests
    Tests/MidiRouterTests.cpp
    Source/MidiRouter.cpp
    Source/MidiRouter.h
)
target_include_directories(MidiRouterTests PRIVATE Source)
target_link_options(MidiRouterTests PRIVATE -static -static-libgcc -static-libstdc++)
add_test(NAME MidiRouterTests COMMAND MidiRouterTests)

add_executable(LeImageLoaderTests
    Tests/LeImageLoaderTests.cpp
    Source/LeImageLoader.cpp
    Source/LeImageLoader.h
)
target_include_directories(LeImageLoaderTests PRIVATE Source)
target_link_options(LeImageLoaderTests PRIVATE -static -static-libgcc -static-libstdc++)
add_test(NAME LeImageLoaderTests COMMAND LeImageLoaderTests)
