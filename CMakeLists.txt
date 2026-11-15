cmake_minimum_required(VERSION 3.20)

project(NVMeSlotSwap
    VERSION 1.0.0
    DESCRIPTION "Dual-image NVMe firmware activation without interrupting host I/O"
    LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(NVMESS_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(NVMESS_TSAN "Build with ThreadSanitizer" OFF)
option(NVMESS_BACKEND_VFIO "Build the Linux VFIO/uio backend (compile-only on macOS)" OFF)
option(NVMESS_BUILD_TESTS "Build the GoogleTest suite" ON)

if(NVMESS_SANITIZE AND NVMESS_TSAN)
    message(FATAL_ERROR "NVMESS_SANITIZE and NVMESS_TSAN are mutually exclusive")
endif()

if(NVMESS_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

if(NVMESS_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
endif()

if(NVMESS_BACKEND_VFIO)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR "NVMESS_BACKEND_VFIO requires a Linux host")
    endif()
    add_compile_definitions(NVMESS_BACKEND_VFIO=1)
endif()

# Warnings are errors on both languages for every target in the project.
add_compile_options(
    $<$<COMPILE_LANGUAGE:C>:-Wall>
    $<$<COMPILE_LANGUAGE:C>:-Wextra>
    $<$<COMPILE_LANGUAGE:C>:-Werror>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    $<$<COMPILE_LANGUAGE:CXX>:-Werror>)

find_package(Threads REQUIRED)

set(NVMESS_CORE_SOURCES
    src/queue.c
    src/device_sim.c
    src/fw_slots.c
    src/sha256.c
    src/driver.cpp
    src/workload.cpp
    src/latency.cpp)

if(NVMESS_BACKEND_VFIO)
    list(APPEND NVMESS_CORE_SOURCES src/backend_vfio.cpp)
endif()

add_library(nvmeslotswap_core STATIC ${NVMESS_CORE_SOURCES})
target_include_directories(nvmeslotswap_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nvmeslotswap_core PUBLIC Threads::Threads)
target_compile_features(nvmeslotswap_core PUBLIC c_std_11 cxx_std_17)

add_executable(nvmeslotswap src/main.cpp)
target_link_libraries(nvmeslotswap PRIVATE nvmeslotswap_core)
target_compile_features(nvmeslotswap PRIVATE cxx_std_17)

if(NVMESS_BUILD_TESTS)
    include(FetchContent)
    set(FETCHCONTENT_QUIET OFF)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_subdirectory(tests)
endif()

# `make format` is a convenience wrapper; see .clang-format.
find_program(CLANG_FORMAT_EXE clang-format)
if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE NVMESS_FORMAT_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXE} -i ${NVMESS_FORMAT_SOURCES}
        COMMENT "Formatting C/C++ sources with clang-format")
endif()
