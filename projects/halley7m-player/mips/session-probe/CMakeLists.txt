cmake_minimum_required(VERSION 3.16)
project(player_session_probe LANGUAGES C)
get_filename_component(PLAYER_SESSION_WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(PLAYER_SESSION_SDK "${PLAYER_SESSION_WORKSPACE}/libbare-cpu")
add_library(player_session_probe STATIC ../player_session.c ../../common/player_session_core.c)
target_include_directories(player_session_probe PRIVATE .. ../../common ../../codec ../../display ../../storage)
target_include_directories(player_session_probe SYSTEM PRIVATE ../include ../port
    "${PLAYER_SESSION_SDK}/Middlewares/FreeRTOS-Kernel/include"
    "${PLAYER_SESSION_SDK}/cpu/core/include" "${PLAYER_SESSION_SDK}/cpu/soc-x2600/include"
    "${PLAYER_SESSION_SDK}/drivers/drivers-x2600/include")
target_compile_definitions(player_session_probe PRIVATE USE_NEWLIB=1 __SDE_MIPS_=1)
target_compile_options(player_session_probe PRIVATE -std=gnu11 -mips32r2 -mabi=32 -mfp32 -mhard-float -EL
    -mno-abicalls -fno-pic -G0 -O2 -ffunction-sections -fdata-sections -fno-common -Wall -Wextra -Werror)
