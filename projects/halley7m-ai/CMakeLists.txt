cmake_minimum_required(VERSION 3.16)
project(halley7m_ai LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(PLAYER "${WORKSPACE}/projects/halley7m-player")
set(SDK "${WORKSPACE}/libbare-cpu")
set(KERNEL "${SDK}/Middlewares/FreeRTOS-Kernel")
set(LVGL "${SDK}/projects/x16xx-halley6/Demos/LVGL_GUIDemo/components/lvgl")
set(AI_RESOURCE_DIR "${WORKSPACE}/build/ai-resources" CACHE PATH "Prepared AI resources")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${WORKSPACE}/scripts/prepare-ai-resources.py"
    --output "${AI_RESOURCE_DIR}" RESULT_VARIABLE prepared)
if(NOT prepared EQUAL 0)
    message(FATAL_ERROR "AI resources unavailable; see README for pinned dictionary preparation")
endif()
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "mips")
    enable_language(ASM)
    set(ARCH -mips32r2 -mabi=32 -mfp32 -mhard-float -EL -mno-abicalls -fno-pic -G0)
    add_compile_options(${ARCH} -O2 -ffunction-sections -fdata-sections -fno-common)
    add_compile_definitions(USE_NEWLIB=1 __SDE_MIPS_=1)
    add_compile_options($<$<COMPILE_LANGUAGE:ASM>:-D_ASSEMBLER_> $<$<COMPILE_LANGUAGE:ASM>:-D__ASSEMBLY__>)
    set(AI_AUDIO_BUILD_TESTS OFF CACHE BOOL "Native tests" FORCE)
    set(AI_CLOUD_BUILD_TESTS OFF CACHE BOOL "Native cloud tests" FORCE)
else()
    enable_testing()
    set(AI_AUDIO_BUILD_TESTS ON CACHE BOOL "Native tests" FORCE)
    set(AI_CLOUD_BUILD_TESTS ON CACHE BOOL "Native cloud tests" FORCE)
    if(MSVC)
        add_compile_options(/utf-8)
        add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    endif()
endif()
add_library(ai_core STATIC core/ai_config.c core/ai_history.c core/ai_assets.c)
target_include_directories(ai_core PUBLIC core)
if(MSVC)
    target_compile_options(ai_core PRIVATE /W4 /WX)
else()
    target_compile_options(ai_core PRIVATE -Wall -Wextra -Werror)
endif()
add_subdirectory(audio)
add_subdirectory(cloud)
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS "${LVGL}/src/*.c")
add_library(ai_lvgl STATIC ${LVGL_SOURCES})
target_include_directories(ai_lvgl SYSTEM PUBLIC "${LVGL}")
target_include_directories(ai_lvgl PUBLIC gui)
target_compile_definitions(ai_lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE LV_LVGL_H_INCLUDE_SIMPLE)
add_library(ai_gui STATIC gui/ai_gui.c gui/ai_font.c "${PLAYER}/gui/player_gui_surface.c")
target_include_directories(ai_gui PUBLIC gui "${PLAYER}/gui" "${PLAYER}/common")
target_include_directories(ai_gui PRIVATE "${AI_RESOURCE_DIR}")
target_link_libraries(ai_gui PUBLIC ai_core ai_lvgl)
if(MSVC)
    target_compile_options(ai_gui PRIVATE /W4 /WX)
else()
    target_compile_options(ai_gui PRIVATE -Wall -Wextra -Werror)
endif()
if(NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "mips")
    add_executable(ai_core_test tests/core_test.c)
    target_link_libraries(ai_core_test PRIVATE ai_core)
    if(MSVC)
        target_compile_options(ai_core_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(ai_core_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME ai_core COMMAND ai_core_test "${AI_RESOURCE_DIR}/ai/font.aif" "${AI_RESOURCE_DIR}/ai/pinyin.aip")
    add_executable(ai_gui_test tests/gui_test.c)
    target_link_libraries(ai_gui_test PRIVATE ai_gui)
    add_test(NAME ai_gui COMMAND ai_gui_test "${AI_RESOURCE_DIR}/ai/font.aif"
        "${AI_RESOURCE_DIR}/ai/pinyin.aip" "${CMAKE_CURRENT_BINARY_DIR}/gui-screenshots")
    return()
endif()
set(PORT "${PLAYER}/mips/port")
set(PLATFORM_INCLUDES "${PLAYER}/mips/include" "${PORT}" "${KERNEL}/include"
    "${SDK}/cpu/core/include" "${SDK}/cpu/soc-x2600/include"
    "${SDK}/drivers/drivers-x2600/include" "${WORKSPACE}/projects/halley7m-regba/platform")
add_subdirectory("${PLAYER}/display" display)
add_subdirectory("${PLAYER}/storage" storage)
add_subdirectory("${PLAYER}/touch" touch)
add_subdirectory(network)
add_executable(halley7m_ai mips/main.c mips/ai_resources.c
    "${PLAYER}/mips/start.S" "${PORT}/context.S" "${PORT}/port.c"
    "${PLAYER}/mips/runtime.c" "${PLAYER}/mips/runtime_heap.c" "${PLAYER}/mips/runtime_syscalls.c"
    "${PLAYER}/common/player_cpu_freertos.c" "${PLAYER}/common/player_cpu_diagnostics.c"
    "${PLAYER}/common/player_video.c" "${PLAYER}/gui/player_gui_display.c"
    "${WORKSPACE}/projects/halley7m-regba/platform/buttons.c"
    "${SDK}/cpu/core/fpu.S" "${SDK}/cpu/core/spinlock.c"
    "${SDK}/cpu/soc-x2600/src/interrupt.c"
    "${SDK}/drivers/drivers-x2600/src/x2600_ll_cpm.c"
    "${SDK}/drivers/drivers-x2600/src/x2600_ll_gpio.c"
    "${SDK}/drivers/drivers-x2600/src/x2600_ll_ost_core.c"
    "${SDK}/drivers/drivers-x2600/src/x2600_ll_ost_global.c"
    "${KERNEL}/tasks.c" "${KERNEL}/queue.c" "${KERNEL}/list.c" "${KERNEL}/timers.c"
    "${KERNEL}/event_groups.c" "${KERNEL}/stream_buffer.c")
target_include_directories(halley7m_ai PRIVATE ${PLATFORM_INCLUDES} "${PLAYER}/display" "${PLAYER}/gui" mips network)
target_link_libraries(halley7m_ai PRIVATE -Wl,--start-group ai_gui ai_lvgl ai_core ai_audio_core
    player_display player_storage player_touch ai_ethernet ai_lwip c m gcc -Wl,--end-group)
set_target_properties(halley7m_ai PROPERTIES SUFFIX ".elf")
target_link_options(halley7m_ai PRIVATE ${ARCH} -nostdlib "-T${PLAYER}/mips/player.lds"
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/halley7m_ai.map,--cref" -Wl,--gc-sections -Wl,--print-memory-usage)
set_property(TARGET halley7m_ai APPEND PROPERTY LINK_DEPENDS "${PLAYER}/mips/player.lds")
add_custom_command(TARGET halley7m_ai POST_BUILD
    COMMAND "${CMAKE_SIZE}" $<TARGET_FILE:halley7m_ai>
    COMMAND "${CMAKE_OBJCOPY}" -O binary $<TARGET_FILE:halley7m_ai> "${CMAKE_CURRENT_BINARY_DIR}/halley7m_ai.bin")
