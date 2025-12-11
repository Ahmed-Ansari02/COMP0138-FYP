# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file LICENSE.rst or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "/Users/ahmedansari/esp/esp-idf/components/bootloader/subproject")
  file(MAKE_DIRECTORY "/Users/ahmedansari/esp/esp-idf/components/bootloader/subproject")
endif()
file(MAKE_DIRECTORY
  "/Users/ahmedansari/Projects/UCL/UCL_YEAR_4/FYP/playground/WAMR_ESP_32/esp_task_simulation/controller/build/bootloader"
  "/Users/ahmedansari/Projects/UCL/UCL_YEAR_4/FYP/playground/WAMR_ESP_32/esp_task_simulation/controller/build/bootloader-prefix"
  "/Users/ahmedansari/Projects/UCL/UCL_YEAR_4/FYP/playground/WAMR_ESP_32/esp_task_simulation/controller/build/bootloader-prefix/tmp"
  "/Users/ahmedansari/Projects/UCL/UCL_YEAR_4/FYP/playground/WAMR_ESP_32/esp_task_simulation/controller/build/bootloader-prefix/src/bootloader-stamp"
  "/Users/ahmedansari/Projects/UCL/UCL_YEAR_4/FYP/playground/WAMR_ESP_32/esp_task_simulation/controller/build/bootloader-prefix/src"
  "/Users/ahmedansari/Projects/UCL/UCL_YEAR_4/FYP/playground/WAMR_ESP_32/esp_task_simulation/controller/build/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/ahmedansari/Projects/UCL/UCL_YEAR_4/FYP/playground/WAMR_ESP_32/esp_task_simulation/controller/build/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/ahmedansari/Projects/UCL/UCL_YEAR_4/FYP/playground/WAMR_ESP_32/esp_task_simulation/controller/build/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
