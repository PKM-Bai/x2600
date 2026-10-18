cmake_minimum_required(VERSION 3.16)
project(halley7m_regba LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
get_filename_component(SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../libbare-cpu" ABSOLUTE)
set(KERNEL_PATH "${SDK_PATH}/Middlewares/FreeRTOS-Kernel")
set(REGBA_MAIN_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/main.c" CACHE FILEPATH "Firmware entry source")
set(REGBA_EXTRA_SOURCES "" CACHE STRING "Additional platform and ReGBA source files")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(REGBA_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/regba-core")
execute_process(COMMAND ${Python3_EXECUTABLE} "${WORKSPACE}/scripts/prepare-ui-font.py"
    --source "${WORKSPACE}/third_party/ReGBA/source/tools/unifont.hex.gz"
    --output "${REGBA_GENERATED}/ui_font.h"
    --text "${CMAKE_CURRENT_SOURCE_DIR}/core/handheld_ui.c"
    --text "${CMAKE_CURRENT_SOURCE_DIR}/core/regba_core.c"
    --text "${CMAKE_CURRENT_SOURCE_DIR}/platform/ui_draw.c"
    RESULT_VARIABLE UI_FONT_RESULT)
if(NOT UI_FONT_RESULT EQUAL 0)
    message(FATAL_ERROR "Handheld UI font generation failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${WORKSPACE}/scripts/prepare-ui-font.py"
    "${WORKSPACE}/third_party/ReGBA/source/tools/unifont.hex.gz"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/handheld_ui.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/regba_core.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/ui_draw.c")
execute_process(COMMAND ${Python3_EXECUTABLE} "${WORKSPACE}/scripts/prepare-regba.py"
    --source "${WORKSPACE}/third_party/ReGBA" --output "${REGBA_GENERATED}"
    RESULT_VARIABLE PREPARE_RESULT)
if(NOT PREPARE_RESULT EQUAL 0)
    message(FATAL_ERROR "ReGBA preparation failed; use a clean pinned checkout")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${WORKSPACE}/scripts/prepare-regba.py"
    "${WORKSPACE}/third_party/ReGBA/game_config.txt"
    "${WORKSPACE}/scripts/make-save-test-roms.py")
set(DISPLAY_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/display-sdk")
execute_process(COMMAND ${Python3_EXECUTABLE} "${WORKSPACE}/scripts/prepare-display.py"
    --sdk "${SDK_PATH}" --output "${DISPLAY_GENERATED}"
    RESULT_VARIABLE DISPLAY_PREPARE_RESULT)
if(NOT DISPLAY_PREPARE_RESULT EQUAL 0)
    message(FATAL_ERROR "Display SDK preparation failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${WORKSPACE}/scripts/prepare-display.py"
    "${SDK_PATH}/drivers/drivers-x2600/src/x2600_hal_lcd.c"
    "${SDK_PATH}/drivers/drivers-x2600/src/x2600_hal_mipi_dsi.c"
    "${SDK_PATH}/drivers/drivers-x2600/src/x2600_ll_mipi_dsi.c")
set(SSI_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/ssi-sdk")
execute_process(COMMAND ${Python3_EXECUTABLE} "${WORKSPACE}/scripts/prepare-ssi.py"
    --sdk "${SDK_PATH}" --output "${SSI_GENERATED}"
    RESULT_VARIABLE SSI_PREPARE_RESULT)
if(NOT SSI_PREPARE_RESULT EQUAL 0)
    message(FATAL_ERROR "SSI SDK preparation failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${WORKSPACE}/scripts/prepare-ssi.py"
    "${SDK_PATH}/drivers/drivers-x2600/src/x2600_hal_ssi.c")

add_executable(halley7m_regba
    platform/start.S
    platform/port/context.S
    platform/port/port.c
    platform/runtime.c
    platform/runtime_heap.c
    platform/runtime_syscalls.c
    platform/runtime_selftest.c
    platform/port/probe.S
    platform/storage.c
    platform/path_encoding.c
    platform/sdspi.c
    platform/fatfs/ff.c
    platform/fatfs/diskio.c
    platform/fatfs/unicode.c
    platform/audio.c
    platform/game_stats.c
    platform/battery_save.c
    platform/buttons.c
    platform/bt_gamepad.c
    platform/video.c
    platform/video_blit.c
    platform/ui_draw.c
    platform/panel_zc502.c
    ${DISPLAY_GENERATED}/x2600_hal_lcd.c
    ${DISPLAY_GENERATED}/x2600_hal_mipi_dsi.c
    ${DISPLAY_GENERATED}/x2600_ll_mipi_dsi.c
    core/regba_core.c
    core/handheld_ui.c
    core/playback.c
    core/resources.c
    core/cache_publish.S
    ${REGBA_GENERATED}/engine.c
    ${REGBA_GENERATED}/video.c
    ${REGBA_GENERATED}/input.c
    ${REGBA_GENERATED}/bios.c
    ${REGBA_GENERATED}/sound.c
    ${REGBA_GENERATED}/stats.c
    ${REGBA_GENERATED}/memory.c
    ${REGBA_GENERATED}/cpu_common.c
    ${REGBA_GENERATED}/cpu_asm.c
    ${REGBA_GENERATED}/sha1.c
    ${REGBA_GENERATED}/mips/stub.S
    ${SDK_PATH}/cpu/core/fpu.S
    ${SDK_PATH}/cpu/core/spinlock.c
    ${SDK_PATH}/cpu/soc-x2600/src/interrupt.c
    ${SDK_PATH}/drivers/drivers-x2600/src/x2600_ll_cpm.c
    ${SDK_PATH}/drivers/drivers-x2600/src/x2600_ll_gpio.c
    ${SDK_PATH}/drivers/drivers-x2600/src/x2600_ll_pwm.c
    ${SDK_PATH}/drivers/drivers-x2600/src/x2600_ll_ost_core.c
    ${SDK_PATH}/drivers/drivers-x2600/src/x2600_ll_ost_global.c
    ${SSI_GENERATED}/x2600_hal_ssi.c
    ${SDK_PATH}/drivers/drivers-x2600/src/x2600_hal_aic.c
    ${SDK_PATH}/drivers/drivers-x2600/src/x2600_hal_icodec.c
    ${SDK_PATH}/drivers/drivers-x2600/src/x2600_hal_pdma.c
    ${KERNEL_PATH}/tasks.c
    ${KERNEL_PATH}/queue.c
    ${KERNEL_PATH}/list.c
    ${KERNEL_PATH}/timers.c
    ${KERNEL_PATH}/event_groups.c
    ${KERNEL_PATH}/stream_buffer.c
    ${REGBA_MAIN_SOURCE}
    ${REGBA_EXTRA_SOURCES}
)
set_target_properties(halley7m_regba PROPERTIES SUFFIX ".elf")
target_include_directories(halley7m_regba PRIVATE
    include platform platform/port platform/fatfs core ${REGBA_GENERATED}
    ${REGBA_GENERATED}/mips
    ${WORKSPACE}/third_party/ReGBA/source/opendingux
    ${SDK_PATH}/cpu/core/include
    ${SDK_PATH}/cpu/soc-x2600/include
    ${SDK_PATH}/drivers/drivers-x2600/include
    ${KERNEL_PATH}/include
)
target_compile_definitions(halley7m_regba PRIVATE USE_NEWLIB=1 __SDE_MIPS_=1
    GCW_ZERO MIPS_XBURST LOAD_ALL_ROM MIPS_32R2)
target_compile_options(halley7m_regba PRIVATE
    -mips32r2 -mabi=32 -mfp32 -mhard-float -EL -mno-abicalls -fno-pic -G0
    -ffunction-sections -fdata-sections -fno-common
    $<$<CONFIG:Release>:-O2>
    $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wno-unused-parameter>
    $<$<COMPILE_LANGUAGE:ASM>:-D_ASSEMBLER_;-D__ASSEMBLY__>
)
set(REGBA_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/halley7m.lds")
set_property(TARGET halley7m_regba APPEND PROPERTY LINK_DEPENDS "${REGBA_LINKER_SCRIPT}")
target_link_options(halley7m_regba PRIVATE
    -mips32r2 -mabi=32 -mfp32 -mhard-float -EL -mno-abicalls -fno-pic -G0
    -nostdlib "-T${REGBA_LINKER_SCRIPT}"
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/halley7m_regba.map,--cref"
    -Wl,--gc-sections -Wl,--print-memory-usage
)
target_link_libraries(halley7m_regba PRIVATE -Wl,--start-group c m gcc -Wl,--end-group)
add_custom_command(TARGET halley7m_regba POST_BUILD
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:halley7m_regba>
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:halley7m_regba>
            "${CMAKE_CURRENT_BINARY_DIR}/halley7m_regba.bin"
    VERBATIM
)
