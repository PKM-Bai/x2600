cmake_minimum_required(VERSION 3.16)
project(halley7m_player LANGUAGES C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PLAYER_CORE "mips" CACHE STRING "Firmware processor: mips or riscv")
set(PLAYER_AUDIO_OWNER "riscv" CACHE STRING "Audio hardware owner: mips or riscv")
option(PLAYER_MIPS_ONLY "Build main-core diagnostics without a RISC-V firmware" OFF)
option(PLAYER_DIAGNOSTICS_ONLY "Run serial probes without the player GUI/session worker" OFF)
set(PLAYER_RISCV_CLOCK_HZ "576000000" CACHE STRING "Victory0 timer clock; measured against the board OST")
get_filename_component(WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(SDK_PATH "${WORKSPACE}/libbare-cpu")
set(KERNEL_PATH "${SDK_PATH}/Middlewares/FreeRTOS-Kernel")
if(PLAYER_AUDIO_OWNER STREQUAL "mips")
    set(AUDIO_OWNER_ID 1)
elseif(PLAYER_AUDIO_OWNER STREQUAL "riscv")
    set(AUDIO_OWNER_ID 2)
else()
    message(FATAL_ERROR "PLAYER_AUDIO_OWNER must be mips or riscv")
endif()
set(KERNEL_SOURCES ${KERNEL_PATH}/tasks.c ${KERNEL_PATH}/queue.c
    ${KERNEL_PATH}/list.c ${KERNEL_PATH}/timers.c ${KERNEL_PATH}/event_groups.c
    ${KERNEL_PATH}/stream_buffer.c)
if(PLAYER_CORE STREQUAL "riscv")
    set(FIRMWARE player_riscv)
    set(PORT_PATH "${SDK_PATH}/cpu/core-riscv/port_freertos")
    set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/riscv/player.lds")
    add_executable(${FIRMWARE}
        riscv/start.S riscv/main.c riscv/runtime.c common/player_amp.c
        common/player_cpu_diagnostics.c common/player_cpu_freertos.c
        ${PORT_PATH}/genex.S ${PORT_PATH}/port.c ${PORT_PATH}/portASM.S ${PORT_PATH}/heap_4.c
        ${SDK_PATH}/cpu/core-riscv/traps.c ${SDK_PATH}/cpu/core-riscv/spinlock.c
        ${SDK_PATH}/cpu/soc-x2600/src/interrupt.c
        ${SDK_PATH}/lib/libc/minimal/string.c ${KERNEL_SOURCES})
    target_include_directories(${FIRMWARE} PRIVATE riscv/include common ${PORT_PATH}
        ${SDK_PATH}/cpu/core-riscv/include ${SDK_PATH}/cpu/soc-x2600/include
        ${SDK_PATH}/drivers/drivers-x2600/include ${KERNEL_PATH}/include)
    set(ARCH_OPTIONS -march=rv32imc -mabi=ilp32 -msmall-data-limit=0 -mno-relax)
    target_compile_options(${FIRMWARE} PRIVATE -fno-pic -fno-builtin)
    target_link_options(${FIRMWARE} PRIVATE -Wl,--no-relax)
    target_link_libraries(${FIRMWARE} PRIVATE gcc)
elseif(PLAYER_CORE STREQUAL "mips")
    set(FIRMWARE halley7m_player)
    set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/mips/player.lds")
    set(PLAYER_RISCV_BIN "" CACHE FILEPATH "Verified RISC-V binary to embed")
    set(PLAYER_EMBED_HEADER "" CACHE FILEPATH "Header written by verify-player.py")
    set(PLAYER_TEST_AVI "" CACHE FILEPATH "Optional controlled AVI embedded for stage-3 board diagnostics")
    set(TEST_MEDIA_SOURCES)
    if(PLAYER_TEST_AVI)
        if(NOT EXISTS "${PLAYER_TEST_AVI}")
            message(FATAL_ERROR "PLAYER_TEST_AVI does not exist")
        endif()
        file(TO_CMAKE_PATH "${PLAYER_TEST_AVI}" PLAYER_TEST_AVI)
        configure_file(mips/test_media.S.in "${CMAKE_CURRENT_BINARY_DIR}/test_media.S" @ONLY)
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/test_media.S" PROPERTIES OBJECT_DEPENDS "${PLAYER_TEST_AVI}")
        list(APPEND TEST_MEDIA_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/test_media.S")
    endif()
    if(PLAYER_MIPS_ONLY)
        if(NOT PLAYER_AUDIO_OWNER STREQUAL "mips")
            message(FATAL_ERROR "PLAYER_MIPS_ONLY requires PLAYER_AUDIO_OWNER=mips")
        endif()
        set(AMP_SOURCES mips/amp_unavailable.c)
    else()
        if(NOT EXISTS "${PLAYER_RISCV_BIN}" OR NOT EXISTS "${PLAYER_EMBED_HEADER}")
            message(FATAL_ERROR "Build and verify RISC-V first; pass PLAYER_RISCV_BIN and PLAYER_EMBED_HEADER")
        endif()
        file(TO_CMAKE_PATH "${PLAYER_RISCV_BIN}" PLAYER_RISCV_BIN)
        configure_file(mips/riscv_embed.S.in "${CMAKE_CURRENT_BINARY_DIR}/riscv_embed.S" @ONLY)
        configure_file("${PLAYER_EMBED_HEADER}" "${CMAKE_CURRENT_BINARY_DIR}/player_embed.h" COPYONLY)
        set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/riscv_embed.S" PROPERTIES OBJECT_DEPENDS "${PLAYER_RISCV_BIN}")
        set(AMP_SOURCES mips/amp_boot.c "${CMAKE_CURRENT_BINARY_DIR}/riscv_embed.S")
    endif()
    add_executable(${FIRMWARE}
        mips/start.S mips/port/context.S mips/port/port.c
        mips/runtime.c mips/runtime_heap.c mips/runtime_syscalls.c
        mips/main.c mips/player_probe.c mips/player_audio_probe.c mips/player_audio_client.c
        mips/player_session.c mips/player_diagnostics.c mips/player_measurement.c
        mips/player_ui_sequence.c
        ${WORKSPACE}/projects/halley7m-regba/platform/buttons.c
        ${AMP_SOURCES} ${TEST_MEDIA_SOURCES} common/player_amp.c
        common/player_video.c common/player_media.c common/player_session_core.c
        common/player_cpu_diagnostics.c common/player_cpu_freertos.c
        ${SDK_PATH}/cpu/core/fpu.S ${SDK_PATH}/cpu/core/spinlock.c
        ${SDK_PATH}/cpu/soc-x2600/src/interrupt.c
        ${SDK_PATH}/drivers/drivers-x2600/src/x2600_ll_cpm.c
        ${SDK_PATH}/drivers/drivers-x2600/src/x2600_ll_gpio.c
        ${SDK_PATH}/drivers/drivers-x2600/src/x2600_ll_ost_core.c
        ${SDK_PATH}/drivers/drivers-x2600/src/x2600_ll_ost_global.c
        ${KERNEL_SOURCES})
    target_include_directories(${FIRMWARE} PRIVATE mips/include mips mips/port common
        "${CMAKE_CURRENT_BINARY_DIR}" ${SDK_PATH}/cpu/core/include
        ${SDK_PATH}/cpu/soc-x2600/include ${SDK_PATH}/drivers/drivers-x2600/include
        ${KERNEL_PATH}/include ${WORKSPACE}/projects/halley7m-regba/platform)
    target_compile_definitions(${FIRMWARE} PRIVATE USE_NEWLIB=1 __SDE_MIPS_=1)
    if(PLAYER_TEST_AVI)
        target_compile_definitions(${FIRMWARE} PRIVATE PLAYER_HAVE_TEST_MEDIA=1)
    endif()
    if(PLAYER_DIAGNOSTICS_ONLY)
        target_compile_definitions(${FIRMWARE} PRIVATE PLAYER_DIAGNOSTICS_ONLY=1)
    endif()
    set(ARCH_OPTIONS -mips32r2 -mabi=32 -mfp32 -mhard-float -EL -mno-abicalls -fno-pic -G0)
    add_subdirectory(codec)
    add_subdirectory(display)
    add_subdirectory(storage)
    add_subdirectory(gui)
    target_link_libraries(${FIRMWARE} PRIVATE -Wl,--start-group
        player_gui player_lvgl player_codec player_h264_vendor player_display player_storage c m gcc -Wl,--end-group)
else()
    message(FATAL_ERROR "PLAYER_CORE must be mips or riscv")
endif()
if((PLAYER_CORE STREQUAL "riscv" AND PLAYER_AUDIO_OWNER STREQUAL "riscv") OR
   (PLAYER_CORE STREQUAL "mips" AND PLAYER_AUDIO_OWNER STREQUAL "mips"))
    add_subdirectory(audio)
    target_link_libraries(${FIRMWARE} PRIVATE player_audio)
endif()
set_target_properties(${FIRMWARE} PROPERTIES SUFFIX ".elf")
target_compile_definitions(${FIRMWARE} PRIVATE PLAYER_AUDIO_OWNER=${AUDIO_OWNER_ID}
    PLAYER_RISCV_CLOCK_HZ=${PLAYER_RISCV_CLOCK_HZ})
target_compile_options(${FIRMWARE} PRIVATE ${ARCH_OPTIONS}
    -ffunction-sections -fdata-sections -fno-common
    $<$<CONFIG:Release>:-O2>
    $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wno-unused-parameter>
    $<$<COMPILE_LANGUAGE:ASM>:-D_ASSEMBLER_;-D__ASSEMBLY__>)
target_link_options(${FIRMWARE} PRIVATE ${ARCH_OPTIONS} -nostdlib "-T${LINKER_SCRIPT}"
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${FIRMWARE}.map,--cref"
    -Wl,--gc-sections -Wl,--print-memory-usage)
set_property(TARGET ${FIRMWARE} APPEND PROPERTY LINK_DEPENDS "${LINKER_SCRIPT}")
add_custom_command(TARGET ${FIRMWARE} POST_BUILD
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${FIRMWARE}>
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${FIRMWARE}>
            "${CMAKE_CURRENT_BINARY_DIR}/${FIRMWARE}.bin" VERBATIM)
