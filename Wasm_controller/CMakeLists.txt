# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Wasm_controller)

# Suppress dangling-pointer warning in WAMR library code (false positive in wasm_runtime_common.c)
idf_build_set_property(COMPILE_OPTIONS "-Wno-dangling-pointer" APPEND)

# Set to ON to flash AOT binaries, OFF for WASM bytecode (must match USE_AOT in controller_wamr.c)
option(USE_AOT "Flash AOT binaries instead of WASM bytecode" OFF)

if(USE_AOT)
    spiffs_create_partition_image(storage wasm_assets/aot FLASH_IN_PROJECT)
else()
    spiffs_create_partition_image(storage wasm_assets/wasm FLASH_IN_PROJECT)
endif()