cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(player_display_probe LANGUAGES C)
endif()
get_filename_component(PLAYER_DISPLAY_WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(PLAYER_DISPLAY_SDK "${PLAYER_DISPLAY_WORKSPACE}/libbare-cpu")
set(PLAYER_DISPLAY_DERIVED "${CMAKE_CURRENT_BINARY_DIR}/derived-display")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PLAYER_DISPLAY_WORKSPACE}/scripts/prepare-player-display.py"
    --sdk "${PLAYER_DISPLAY_SDK}" --output "${PLAYER_DISPLAY_DERIVED}"
    RESULT_VARIABLE PLAYER_DISPLAY_RESULT)
if(NOT PLAYER_DISPLAY_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to derive player composer/DSI drivers")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PLAYER_DISPLAY_WORKSPACE}/scripts/prepare-player-display.py"
    "${PLAYER_DISPLAY_WORKSPACE}/scripts/prepare-display.py"
    "${PLAYER_DISPLAY_SDK}/drivers/drivers-x2600/src/x2600_hal_lcd.c"
    "${PLAYER_DISPLAY_SDK}/drivers/drivers-x2600/src/x2600_hal_mipi_dsi.c"
    "${PLAYER_DISPLAY_SDK}/drivers/drivers-x2600/src/x2600_ll_mipi_dsi.c")
add_library(player_display STATIC player_display.c player_rotator.c
    "${PLAYER_DISPLAY_WORKSPACE}/projects/halley7m-regba/platform/panel_zc502.c"
    "${PLAYER_DISPLAY_DERIVED}/x2600_hal_lcd.c"
    "${PLAYER_DISPLAY_DERIVED}/x2600_hal_mipi_dsi.c"
    "${PLAYER_DISPLAY_DERIVED}/x2600_ll_mipi_dsi.c"
    "${PLAYER_DISPLAY_SDK}/drivers/drivers-x2600/src/x2600_ll_pwm.c")
set_target_properties(player_display PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(player_display PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../common")
target_include_directories(player_display SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../mips/include" "${CMAKE_CURRENT_SOURCE_DIR}/../mips/port"
    "${PLAYER_DISPLAY_WORKSPACE}/projects/halley7m-regba/platform"
    "${PLAYER_DISPLAY_SDK}/Middlewares/FreeRTOS-Kernel/include"
    "${PLAYER_DISPLAY_SDK}/cpu/core/include" "${PLAYER_DISPLAY_SDK}/cpu/soc-x2600/include"
    "${PLAYER_DISPLAY_SDK}/drivers/drivers-x2600/include")
target_compile_definitions(player_display PRIVATE USE_NEWLIB=1 __SDE_MIPS_=1)
target_compile_options(player_display PRIVATE -mips32r2 -mabi=32 -mfp32 -mhard-float -EL
    -mno-abicalls -fno-pic -G0 -O2 -ffunction-sections -fdata-sections -fno-common
    -Wall -Wextra -Wno-unused-parameter)
set_source_files_properties(player_display.c player_rotator.c PROPERTIES COMPILE_OPTIONS "-Werror")
