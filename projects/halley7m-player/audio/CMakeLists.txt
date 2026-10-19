cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(player_audio_probe LANGUAGES C)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(PLAYER_AUDIO_HOST_TEST "Build only the private PCM queue host tests" OFF)
get_filename_component(PLAYER_AUDIO_WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_library(player_pcm_queue STATIC player_pcm_queue.c)
target_include_directories(player_pcm_queue PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(PLAYER_AUDIO_HOST_TEST)
    add_executable(player_pcm_queue_test tests/pcm_queue_test.c)
    target_link_libraries(player_pcm_queue_test PRIVATE player_pcm_queue)
    if(MSVC)
        target_compile_options(player_pcm_queue PRIVATE /std:c11 /utf-8 /W4 /WX)
        target_compile_options(player_pcm_queue_test PRIVATE /std:c11 /utf-8 /W4 /WX)
    else()
        target_compile_options(player_pcm_queue PRIVATE -Wall -Wextra -Werror)
        target_compile_options(player_pcm_queue_test PRIVATE -Wall -Wextra -Werror)
    endif()
    enable_testing()
    add_test(NAME player_pcm_queue COMMAND player_pcm_queue_test)
    return()
endif()
set(PLAYER_AUDIO_SDK "${PLAYER_AUDIO_WORKSPACE}/libbare-cpu")
set(PLAYER_AUDIO_CORE "${CMAKE_SYSTEM_PROCESSOR}" CACHE STRING "Selected audio hardware owner: riscv32 or mips")
set(PLAYER_AUDIO_FLAGS -fno-pic -fno-builtin -O2 -ffunction-sections -fdata-sections -fno-common)
if(PLAYER_AUDIO_CORE MATCHES "^riscv")
    list(APPEND PLAYER_AUDIO_FLAGS -march=rv32imc -mabi=ilp32 -msmall-data-limit=0 -mno-relax)
    set(PLAYER_AUDIO_PLATFORM_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/../riscv/include"
        "${PLAYER_AUDIO_SDK}/cpu/core-riscv/port_freertos" "${PLAYER_AUDIO_SDK}/cpu/core-riscv/include")
elseif(PLAYER_AUDIO_CORE STREQUAL "mips")
    list(APPEND PLAYER_AUDIO_FLAGS -mips32r2 -mabi=32 -mfp32 -mhard-float -EL -mno-abicalls -G0)
    set(PLAYER_AUDIO_PLATFORM_INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/../mips/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../mips/port" "${PLAYER_AUDIO_SDK}/cpu/core/include")
else()
    message(FATAL_ERROR "Audio hardware requires an RV32IMC or MIPS toolchain; use PLAYER_AUDIO_HOST_TEST for host tests")
endif()
set(PLAYER_RISCV_CLOCK_HZ "576000000" CACHE STRING "Victory0 core timer clock")
add_library(player_audio_hal STATIC
    "${PLAYER_AUDIO_SDK}/drivers/drivers-x2600/src/x2600_ll_cpm.c"
    "${PLAYER_AUDIO_SDK}/drivers/drivers-x2600/src/x2600_ll_gpio.c"
    "${PLAYER_AUDIO_SDK}/drivers/drivers-x2600/src/x2600_hal_pdma.c"
    "${PLAYER_AUDIO_SDK}/drivers/drivers-x2600/src/x2600_hal_aic.c"
    "${PLAYER_AUDIO_SDK}/drivers/drivers-x2600/src/x2600_hal_icodec.c")
add_library(player_audio STATIC player_audio.c)
target_link_libraries(player_audio PUBLIC player_pcm_queue player_audio_hal)
target_include_directories(player_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
foreach(PLAYER_AUDIO_TARGET player_audio player_audio_hal)
    # This standalone HAL config adds audio headers without changing the RV
    # firmware's stage-2 config. All register sources use the same IMC ABI.
    target_include_directories(${PLAYER_AUDIO_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_include_directories(${PLAYER_AUDIO_TARGET} SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../common" ${PLAYER_AUDIO_PLATFORM_INCLUDES}
        "${PLAYER_AUDIO_SDK}/Middlewares/FreeRTOS-Kernel/include"
        "${PLAYER_AUDIO_SDK}/cpu/soc-x2600/include"
        "${PLAYER_AUDIO_SDK}/drivers/drivers-x2600/include")
    target_compile_definitions(${PLAYER_AUDIO_TARGET} PRIVATE USE_NEWLIB=1
        PLAYER_RISCV_CLOCK_HZ=${PLAYER_RISCV_CLOCK_HZ})
    if(PLAYER_AUDIO_CORE STREQUAL "mips")
        target_compile_definitions(${PLAYER_AUDIO_TARGET} PRIVATE __SDE_MIPS_=1)
    endif()
    target_compile_options(${PLAYER_AUDIO_TARGET} PRIVATE ${PLAYER_AUDIO_FLAGS})
endforeach()
target_compile_options(player_pcm_queue PRIVATE ${PLAYER_AUDIO_FLAGS} -Wall -Wextra -Werror)
target_compile_options(player_audio PRIVATE -Wall -Wextra -Werror)
# Preserve SDK source and license headers; warnings are kept on our adapter.
target_compile_options(player_audio_hal PRIVATE -w)
# Deliberately no libgcc: the installed archive contains RV32E/ILP32E objects.
# The final firmware must verify no RVE e_flags or incompatible helper objects.
