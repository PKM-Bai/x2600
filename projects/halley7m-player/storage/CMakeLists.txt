cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(player_storage_probe LANGUAGES C)
endif()
get_filename_component(PLAYER_STORAGE_WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(PLAYER_STORAGE_SDK "${PLAYER_STORAGE_WORKSPACE}/libbare-cpu")
set(PLAYER_STORAGE_PLATFORM "${PLAYER_STORAGE_WORKSPACE}/projects/halley7m-regba/platform")
set(PLAYER_STORAGE_DERIVED "${CMAKE_CURRENT_BINARY_DIR}/derived-storage")
set(PLAYER_STORAGE_SSI "${CMAKE_CURRENT_BINARY_DIR}/derived-ssi")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PLAYER_STORAGE_WORKSPACE}/scripts/prepare-ssi.py"
    --sdk "${PLAYER_STORAGE_SDK}" --output "${PLAYER_STORAGE_SSI}"
    RESULT_VARIABLE PLAYER_STORAGE_SSI_RESULT)
if(NOT PLAYER_STORAGE_SSI_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to derive validated SSI polling driver")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare-storage.py"
    --output "${PLAYER_STORAGE_DERIVED}" --ssi "${PLAYER_STORAGE_SSI}/x2600_hal_ssi.c"
    RESULT_VARIABLE PLAYER_STORAGE_RESULT)
if(NOT PLAYER_STORAGE_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to derive read-only player storage")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/prepare-storage.py" "${CMAKE_CURRENT_SOURCE_DIR}/ffconf.h"
    "${PLAYER_STORAGE_WORKSPACE}/scripts/prepare-ssi.py" "${PLAYER_STORAGE_PLATFORM}/sdspi.c"
    "${PLAYER_STORAGE_PLATFORM}/fatfs/ff.c" "${PLAYER_STORAGE_PLATFORM}/fatfs/ff.h"
    "${PLAYER_STORAGE_PLATFORM}/fatfs/integer.h" "${PLAYER_STORAGE_PLATFORM}/fatfs/diskio.h"
    "${PLAYER_STORAGE_PLATFORM}/fatfs/unicode.c"
    "${PLAYER_STORAGE_SDK}/drivers/drivers-x2600/src/x2600_hal_ssi.c")
add_library(player_storage STATIC player_storage.c diskio.c
    "${PLAYER_STORAGE_DERIVED}/sdspi_readonly.c"
    "${PLAYER_STORAGE_DERIVED}/fatfs/ff.c" "${PLAYER_STORAGE_DERIVED}/fatfs/unicode.c"
    "${PLAYER_STORAGE_PLATFORM}/path_encoding.c"
    "${PLAYER_STORAGE_DERIVED}/x2600_hal_ssi_polling.c")
set_target_properties(player_storage PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(player_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_include_directories(player_storage PRIVATE "${PLAYER_STORAGE_DERIVED}/fatfs"
    "${PLAYER_STORAGE_PLATFORM}")
target_include_directories(player_storage SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../mips/include" "${CMAKE_CURRENT_SOURCE_DIR}/../mips/port"
    "${PLAYER_STORAGE_SDK}/Middlewares/FreeRTOS-Kernel/include"
    "${PLAYER_STORAGE_SDK}/cpu/core/include" "${PLAYER_STORAGE_SDK}/cpu/soc-x2600/include"
    "${PLAYER_STORAGE_SDK}/drivers/drivers-x2600/include")
target_compile_definitions(player_storage PRIVATE USE_NEWLIB=1 __SDE_MIPS_=1)
target_compile_options(player_storage PRIVATE -mips32r2 -mabi=32 -mfp32 -mhard-float -EL
    -mno-abicalls -fno-pic -G0 -O2 -ffunction-sections -fdata-sections -fno-common
    -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties("${PLAYER_STORAGE_DERIVED}/sdspi_readonly.c"
    "${PLAYER_STORAGE_DERIVED}/x2600_hal_ssi_polling.c" PROPERTIES
    COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/player_storage_hal.h")
set_source_files_properties(player_storage.c diskio.c PROPERTIES COMPILE_OPTIONS "-Werror")
