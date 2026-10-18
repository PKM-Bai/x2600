cmake_minimum_required(VERSION 3.16)
#
# Core project settings
#
Project(halley7m_hello)          # HALLEY7-M / X2600M MIPS main core
enable_language(C CXX ASM)
set(CMAKE_EXPORT_COMPILE_COMMANDS   ON)
# Setup compiler settings
set(CMAKE_C_STANDARD                11)
set(CMAKE_C_STANDARD_REQUIRED       ON)
set(CMAKE_C_EXTENSIONS              ON)
set(CMAKE_CXX_STANDARD              11)
set(CMAKE_CXX_STANDARD_REQUIRED     ON)
set(CMAKE_CXX_EXTENSIONS            ON)
set(PROJ_PATH                       ${CMAKE_CURRENT_SOURCE_DIR})
set(SDK_PATH              ${PROJ_PATH}/../../libbare-cpu)
message("Build type: "              ${CMAKE_BUILD_TYPE})

# Set linker script
set(linker_script_SRC               ${PROJ_PATH}/halley7m.lds)
set(EXECUTABLE                      ${CMAKE_PROJECT_NAME})
set(CPU_PARAMETERS                  "-mips32r2")

set(CMAKE_ASM_FLAGS                 "${CPU_PARAMETERS} -EL -D_ASSEMBLER_ -D__ASSEMBLY__")

set(CMAKE_C_FLAGS                   "${CPU_PARAMETERS} -EL -mno-abicalls -fno-pic -fno-builtin -fomit-frame-pointer -G 0 -Wall -nostdlib -Wall -fdata-sections -ffunction-sections -D__SDE_MIPS_=1")

# Compiler options

if(CMAKE_BUILD_TYPE STREQUAL Debug)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g -ggdb -DDEBUG")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2")
endif()

set(CMAKE_CXX_FLAGS                 ${CMAKE_C_FLAGS})

set(CMAKE_LD_FLAGS		    "${CPU_PARAMETERS}")


set(sources_SRCS                    # Modified
    ${SDK_PATH}/cpu/core/genex.S
    ${SDK_PATH}/cpu/core/spinlock.c
    ${SDK_PATH}/cpu/core/start.S
    ${SDK_PATH}/cpu/core/traps.c
    ${SDK_PATH}/cpu/soc-x2600/src/interrupt.c
    ${SDK_PATH}/cpu/soc-x2600/src/serial.c
    ${SDK_PATH}/cpu/soc-x2600/src/startup.c
    ${SDK_PATH}/drivers/drivers-x2600/src/x2600_hal_def.c
    ${SDK_PATH}/lib/libc/minimal/ctype.c
    ${SDK_PATH}/lib/libc/minimal/div64.c
    ${SDK_PATH}/lib/libc/minimal/string.c
    ${SDK_PATH}/lib/libc/minimal/vsprintf.c
    main.c

)

if(CMAKE_EXPORT_COMPILE_COMMANDS)
    set(CMAKE_C_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES})
    set(CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES})
endif()

#
# Include directories
#
#set(include_path_DIRS               
# Modified

include_directories(
    ${PROJ_PATH}/include        
    ${SDK_PATH}/lib/libc/minimal/include
    ${SDK_PATH}/drivers/drivers-x2600/include
    ${SDK_PATH}/cpu/core/include
    ${SDK_PATH}/cpu/soc-x2600/include

)

#
# -L libdirs.
#
link_directories(
#path/to/lib
)

# Executable files
add_executable(${EXECUTABLE} ${sources_SRCS})

# Linker options
target_link_libraries(${EXECUTABLE} PRIVATE
    -T${linker_script_SRC}
    ${CMAKE_LD_FLAGS}
    -Wl,-Map=${CMAKE_PROJECT_NAME}.map,--cref
    -Wl,--gc-sections
    -EL
    -Wl,--start-group
    -Wl,--end-group
    -Wl,--print-memory-usage
)

# Execute post-build to print size
add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
     COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${EXECUTABLE}>
)

# Convert output to hex and binary
add_custom_command(TARGET ${EXECUTABLE} POST_BUILD
     COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${EXECUTABLE}> ${EXECUTABLE}.bin
 )
