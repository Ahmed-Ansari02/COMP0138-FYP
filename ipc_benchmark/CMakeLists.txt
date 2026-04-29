cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ipc_benchmark)

# Suppress dangling-pointer warning in WAMR library code
idf_build_set_property(COMPILE_OPTIONS "-Wno-dangling-pointer" APPEND)

option(BUILD_SCALING_BENCHMARK "Build the low-memory multi-container scaling benchmark" OFF)

# Set to ON to flash AOT binaries, OFF for WASM bytecode
option(USE_AOT "Flash AOT binaries instead of WASM bytecode" OFF)

if(BUILD_SCALING_BENCHMARK)
    spiffs_create_partition_image(storage wasm_assets/scaling FLASH_IN_PROJECT)
elseif(USE_AOT)
    spiffs_create_partition_image(storage wasm_assets/aot FLASH_IN_PROJECT)
else()
    spiffs_create_partition_image(storage wasm_assets/wasm FLASH_IN_PROJECT)
endif()
