cmake_minimum_required(VERSION 3.16)
project(keyboard-inspector LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()

option(BUILD_TESTS "Build C++ test suite" ON)
option(VENDOR_KISSFFT "Vendor kissfft source" ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui Concurrent Network)

if(VENDOR_KISSFFT)
    include(FetchContent)
    FetchContent_Declare(
        kissfft
        GIT_REPOSITORY https://github.com/mborgerding/kissfft.git
        GIT_TAG        master
        UPDATE_DISCONNECTED TRUE
    )
    if(EXISTS ${CMAKE_BINARY_DIR}/_deps/kissfft-src/kiss_fft.h)
        set(kissfft_SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/kissfft-src)
    else()
        FetchContent_GetProperties(kissfft)
        if(NOT kissfft_POPULATED)
            FetchContent_Populate(kissfft)
        endif()
    endif()
    add_library(kissfft STATIC
        ${kissfft_SOURCE_DIR}/kiss_fft.c
        ${kissfft_SOURCE_DIR}/kiss_fftr.c
    )
    target_compile_definitions(kissfft PUBLIC kiss_fft_scalar=double)
    target_include_directories(kissfft PUBLIC ${kissfft_SOURCE_DIR})
endif()

if(BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.5.3
        UPDATE_DISCONNECTED TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()

add_subdirectory(src)
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
