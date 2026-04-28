cmake_minimum_required(VERSION 3.20)
project(qnx_micro C CXX)

# ─── qnx-micro ─────────────────────────────────────────────────────────────
#
# C++26 microkernel inspired by QNX Neutrino. BSD 2-Clause.
# Message-passing IPC, preemptive priority scheduler,
# capability-based memory, user-space resource managers.
#
# Configure:
#   cmake -S . -B build \
#     -DCMAKE_C_COMPILER=$(which clang) \
#     -DCMAKE_CXX_COMPILER=$(which clang++)
#
# ─────────────────────────────────────────────────────────────────────────────

# ─── Platform gate ───────────────────────────────────────────────────────────

if(WIN32 OR CYGWIN OR MSYS OR MINGW)
    message(FATAL_ERROR
        "Native Windows is not supported.\n"
        "  Use WSL 2 (Ubuntu 24.04+): wsl --install -d Ubuntu")
endif()

# ─── Compiler gate ───────────────────────────────────────────────────────────

if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
    message(FATAL_ERROR
        "AppleClang detected. This project requires upstream LLVM Clang.\n"
        "  macOS:  brew install llvm\n"
        "  Linux:  apt install clang-22")
elseif(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Non-Clang compiler: ${CMAKE_C_COMPILER_ID}")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── C++26 std module ───────────────────────────────────────────────────────

get_filename_component(LLVM_ROOT "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(LLVM_ROOT "${LLVM_ROOT}" DIRECTORY)
set(STD_CPPM "${LLVM_ROOT}/share/libc++/v1/std.cppm")

if(NOT EXISTS "${STD_CPPM}")
    message(FATAL_ERROR "Cannot find std.cppm at ${STD_CPPM}")
endif()

set(STD_PCM "${CMAKE_BINARY_DIR}/std.pcm")
set(CXX ${CMAKE_CXX_COMPILER})
set(MODFLAGS -std=c++26 -stdlib=libc++ -Wno-reserved-module-identifier)

add_custom_command(
    OUTPUT  ${STD_PCM}
    COMMAND ${CXX} ${MODFLAGS} --precompile ${STD_CPPM} -o ${STD_PCM}
    DEPENDS ${STD_CPPM}
    COMMENT "Precompiling std module")

# ─── Project modules (DAG: types -> scheduler -> ipc -> driver -> kernel) ───

set(S ${CMAKE_SOURCE_DIR}/src)

# qnx.types
set(TYPES_PCM ${CMAKE_BINARY_DIR}/qnx.types.pcm)
add_custom_command(
    OUTPUT  ${TYPES_PCM}
    COMMAND ${CXX} ${MODFLAGS} -fmodule-file=std=${STD_PCM}
            --precompile ${S}/types/types.cppm -o ${TYPES_PCM}
    DEPENDS ${STD_PCM} ${S}/types/types.cppm
    COMMENT "Precompiling qnx.types")

# qnx.scheduler (depends on types)
set(SCHED_PCM ${CMAKE_BINARY_DIR}/qnx.scheduler.pcm)
add_custom_command(
    OUTPUT  ${SCHED_PCM}
    COMMAND ${CXX} ${MODFLAGS}
            -fmodule-file=std=${STD_PCM}
            -fmodule-file=qnx.types=${TYPES_PCM}
            --precompile ${S}/scheduler/scheduler.cppm -o ${SCHED_PCM}
    DEPENDS ${STD_PCM} ${TYPES_PCM} ${S}/scheduler/scheduler.cppm
    COMMENT "Precompiling qnx.scheduler")

# qnx.ipc (depends on types, scheduler)
set(IPC_PCM ${CMAKE_BINARY_DIR}/qnx.ipc.pcm)
add_custom_command(
    OUTPUT  ${IPC_PCM}
    COMMAND ${CXX} ${MODFLAGS}
            -fmodule-file=std=${STD_PCM}
            -fmodule-file=qnx.types=${TYPES_PCM}
            -fmodule-file=qnx.scheduler=${SCHED_PCM}
            --precompile ${S}/ipc/ipc.cppm -o ${IPC_PCM}
    DEPENDS ${STD_PCM} ${TYPES_PCM} ${SCHED_PCM} ${S}/ipc/ipc.cppm
    COMMENT "Precompiling qnx.ipc")

# qnx.memory (depends on types)
set(MEM_PCM ${CMAKE_BINARY_DIR}/qnx.memory.pcm)
add_custom_command(
    OUTPUT  ${MEM_PCM}
    COMMAND ${CXX} ${MODFLAGS}
            -fmodule-file=std=${STD_PCM}
            -fmodule-file=qnx.types=${TYPES_PCM}
            --precompile ${S}/memory/memory.cppm -o ${MEM_PCM}
    DEPENDS ${STD_PCM} ${TYPES_PCM} ${S}/memory/memory.cppm
    COMMENT "Precompiling qnx.memory")

# qnx.driver (depends on types, ipc)
set(DRV_PCM ${CMAKE_BINARY_DIR}/qnx.driver.pcm)
add_custom_command(
    OUTPUT  ${DRV_PCM}
    COMMAND ${CXX} ${MODFLAGS}
            -fmodule-file=std=${STD_PCM}
            -fmodule-file=qnx.types=${TYPES_PCM}
            -fmodule-file=qnx.scheduler=${SCHED_PCM}
            -fmodule-file=qnx.ipc=${IPC_PCM}
            --precompile ${S}/driver/driver.cppm -o ${DRV_PCM}
    DEPENDS ${STD_PCM} ${TYPES_PCM} ${SCHED_PCM} ${IPC_PCM} ${S}/driver/driver.cppm
    COMMENT "Precompiling qnx.driver")

# qnx.pps (depends on types)
set(PPS_PCM ${CMAKE_BINARY_DIR}/qnx.pps.pcm)
add_custom_command(
    OUTPUT  ${PPS_PCM}
    COMMAND ${CXX} ${MODFLAGS}
            -fmodule-file=std=${STD_PCM}
            -fmodule-file=qnx.types=${TYPES_PCM}
            --precompile ${S}/pps/pps.cppm -o ${PPS_PCM}
    DEPENDS ${STD_PCM} ${TYPES_PCM} ${S}/pps/pps.cppm
    COMMENT "Precompiling qnx.pps")

# qnx.kernel (depends on all)
set(KERN_PCM ${CMAKE_BINARY_DIR}/qnx.kernel.pcm)
add_custom_command(
    OUTPUT  ${KERN_PCM}
    COMMAND ${CXX} ${MODFLAGS}
            -fmodule-file=std=${STD_PCM}
            -fmodule-file=qnx.types=${TYPES_PCM}
            -fmodule-file=qnx.scheduler=${SCHED_PCM}
            -fmodule-file=qnx.ipc=${IPC_PCM}
            -fmodule-file=qnx.memory=${MEM_PCM}
            -fmodule-file=qnx.driver=${DRV_PCM}
            -fmodule-file=qnx.pps=${PPS_PCM}
            --precompile ${S}/kernel/kernel.cppm -o ${KERN_PCM}
    DEPENDS ${STD_PCM} ${TYPES_PCM} ${SCHED_PCM} ${IPC_PCM} ${MEM_PCM} ${DRV_PCM} ${PPS_PCM}
            ${S}/kernel/kernel.cppm
    COMMENT "Precompiling qnx.kernel")

# ─── Compile module implementations (.pcm -> .o) ────────────────────────────

set(MOD_FILES
    std=${STD_PCM}
    qnx.types=${TYPES_PCM}
    qnx.scheduler=${SCHED_PCM}
    qnx.ipc=${IPC_PCM}
    qnx.memory=${MEM_PCM}
    qnx.driver=${DRV_PCM}
    qnx.pps=${PPS_PCM}
    qnx.kernel=${KERN_PCM})

set(ALL_MOD_FLAGS "")
foreach(m ${MOD_FILES})
    list(APPEND ALL_MOD_FLAGS -fmodule-file=${m})
endforeach()

# Each module PCM needs to be compiled to an object file
macro(compile_module_obj mod_name pcm_var)
    set(${mod_name}_OBJ ${CMAKE_BINARY_DIR}/${mod_name}.o)
    add_custom_command(
        OUTPUT  ${${mod_name}_OBJ}
        COMMAND ${CXX} ${MODFLAGS} ${ALL_MOD_FLAGS}
                -c ${${pcm_var}} -o ${${mod_name}_OBJ}
        DEPENDS ${${pcm_var}}
        COMMENT "Compiling ${mod_name}.o")
endmacro()

compile_module_obj(qnx_types TYPES_PCM)
compile_module_obj(qnx_scheduler SCHED_PCM)
compile_module_obj(qnx_ipc IPC_PCM)
compile_module_obj(qnx_memory MEM_PCM)
compile_module_obj(qnx_driver DRV_PCM)
compile_module_obj(qnx_pps PPS_PCM)
compile_module_obj(qnx_kernel KERN_PCM)

set(ALL_OBJS ${qnx_types_OBJ} ${qnx_scheduler_OBJ} ${qnx_ipc_OBJ}
             ${qnx_memory_OBJ} ${qnx_driver_OBJ} ${qnx_pps_OBJ} ${qnx_kernel_OBJ})
set(ALL_PCMS ${STD_PCM} ${TYPES_PCM} ${SCHED_PCM} ${IPC_PCM} ${MEM_PCM} ${DRV_PCM} ${KERN_PCM})

# ─── Main binary ────────────────────────────────────────────────────────────

add_custom_command(
    OUTPUT  ${CMAKE_BINARY_DIR}/qnx_micro
    COMMAND ${CXX} ${MODFLAGS} ${ALL_MOD_FLAGS}
            ${S}/main.cpp ${ALL_OBJS} -o ${CMAKE_BINARY_DIR}/qnx_micro
    DEPENDS ${ALL_PCMS} ${ALL_OBJS} ${S}/main.cpp
    COMMENT "Building qnx_micro")

add_custom_target(qnx_micro_bin ALL DEPENDS ${CMAKE_BINARY_DIR}/qnx_micro)

# ─── Tests (Catch2) ─────────────────────────────────────────────────────────

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

set(TEST_BIN ${CMAKE_BINARY_DIR}/run_tests)

add_custom_command(
    OUTPUT  ${TEST_BIN}
    COMMAND ${CXX} ${MODFLAGS} ${ALL_MOD_FLAGS}
            -isystem ${catch2_SOURCE_DIR}/src
            -isystem ${catch2_BINARY_DIR}/generated-includes
            ${CMAKE_SOURCE_DIR}/test/test_scheduler.cpp
            ${CMAKE_SOURCE_DIR}/test/test_ipc.cpp
            ${CMAKE_SOURCE_DIR}/test/test_pps.cpp
            ${CMAKE_SOURCE_DIR}/test/test_memory.cpp
            ${CMAKE_SOURCE_DIR}/test/test_api_compat.cpp
            ${CMAKE_SOURCE_DIR}/test/test_qnx_scenarios.cpp
            ${ALL_OBJS}
            -L ${catch2_BINARY_DIR}/src
            -lCatch2Main -lCatch2
            -o ${TEST_BIN}
    DEPENDS ${ALL_PCMS} ${ALL_OBJS}
            ${CMAKE_SOURCE_DIR}/test/test_scheduler.cpp
            ${CMAKE_SOURCE_DIR}/test/test_ipc.cpp
            Catch2 Catch2WithMain
    COMMENT "Building tests")

add_custom_target(test
    COMMAND ${TEST_BIN} --colour-mode ansi
    DEPENDS ${TEST_BIN}
    COMMENT "Running tests")

# ─── Doxygen ─────────────────────────────────────────────────────────────────

find_program(DOXYGEN doxygen)
if(DOXYGEN)
    add_custom_target(doc
        COMMAND ${DOXYGEN} ${CMAKE_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running doxygen")
endif()

# ─── Clean ───────────────────────────────────────────────────────────────────

add_custom_target(distclean
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}
    COMMENT "Removing build/")

# ─── STM32H7 Eval2 bare-metal target (IN PROGRESS) ─────────────────────────
#
# Target: STM32H743ZI — Cortex-M7, 480 MHz, 1 MB flash, 1 MB RAM
# Board:  NUCLEO-H743ZI2 or STM32H7 Eval2
#
# What works:
#   - Preset configures arm-none-eabi cross toolchain
#   - Cortex-M7 flags: -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard
#   - Fixed-capacity arrays, pool allocators = zero heap = bare-metal ready
#
# What's blocked:
#   - import std requires libc++ module support — not available bare-metal
#   - Need freestanding shim: replace import std with minimal <cstdint>,
#     <cstddef>, <array>, <span>, <bitset>, <expected>, <optional>
#   - Linker script for STM32H743 flash/RAM layout (board/stm32h743.ld)
#   - Startup code: vector table, SystemInit, clock tree (HSE -> PLL -> 480 MHz)
#   - No Catch2 on target — tests run host-only, binary runs on-target via SWD
#
# To activate:
#   1. brew install arm-none-eabi-gcc   (macOS)
#      apt install gcc-arm-none-eabi    (Ubuntu)
#   2. Add board/stm32h743.ld linker script
#   3. Add board/startup_stm32h743.s vector table + reset handler
#   4. Build freestanding std shim (src/freestanding/std.cppm)
#   5. cmake --preset stm32h7 && cmake --build build-stm32
#   6. Flash: openocd -f board/stm32h7x3_eval.cfg -c "program build-stm32/qnx_micro.bin 0x08000000 verify reset exit"
#
# if(STM32H7_TARGET)
#     set(CMAKE_CXX_STANDARD 23)  # arm-none-eabi-g++ C++23 max until modules land
#     set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/board/stm32h743.ld)
#
#     add_executable(qnx_micro_stm32
#         ${S}/main.cpp
#         ${S}/types/types.cppm
#         ${S}/scheduler/scheduler.cppm
#         ${S}/ipc/ipc.cppm
#         ${S}/memory/memory.cppm
#         ${S}/pps/pps.cppm
#         ${S}/driver/driver.cppm
#         ${S}/kernel/kernel.cppm
#         board/startup_stm32h743.s)
#
#     target_compile_definitions(qnx_micro_stm32 PRIVATE
#         STM32H743xx
#         HSE_VALUE=25000000
#         QNX_MICRO_FREESTANDING=1)
#
#     target_link_options(qnx_micro_stm32 PRIVATE
#         -T${LINKER_SCRIPT}
#         -Wl,--gc-sections
#         -Wl,-Map=${CMAKE_BINARY_DIR}/qnx_micro.map)
#
#     # Post-build: generate .bin and .hex for flashing, print size
#     add_custom_command(TARGET qnx_micro_stm32 POST_BUILD
#         COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:qnx_micro_stm32>
#                 ${CMAKE_BINARY_DIR}/qnx_micro.bin
#         COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:qnx_micro_stm32>
#                 ${CMAKE_BINARY_DIR}/qnx_micro.hex
#         COMMAND ${CMAKE_SIZE} $<TARGET_FILE:qnx_micro_stm32>
#         COMMENT "Generating .bin/.hex and size report")
# endif()

# ─── C++23 backport (yes, we know) ─────────────────────────────────────────
#
# Every kernel type used here landed in C++23 or earlier:
#
#   std::expected<T,E>   — C++23   (the entire error model)
#   std::span<T>         — C++20   (zero-copy buffer views)
#   std::bitset<N>       — C++98   (scheduler ready_mask_)
#   std::array<T,N>      — C++11   (fixed-capacity everything)
#   std::optional<T>     — C++17   (nullable handles)
#   std::string_view     — C++17   (PPS paths/keys)
#   [[nodiscard]]        — C++17   (every query)
#   designated init      — C++20   ({.id = x, .owner = y})
#   std::ranges::*       — C++20   (find_if, count_if, any_of)
#   std::erase_if        — C++20   (swap-free removal)
#   auto -> T returns    — C++14   (trailing return types)
#
# The ONLY C++26 dependency is `import std` and `export module`.
# The logic is identical on C++23. Zero changes to any algorithm,
# data structure, or API surface.
#
# Backport cage — drop this at the top of each .cppm:
#
#   // ── qnx_micro_compat.h ──────────────────────────────────────────
#   #ifndef QNX_MICRO_COMPAT_H
#   #define QNX_MICRO_COMPAT_H
#
#   #if __cplusplus >= 202600L && __has_include(<module>)
#     // C++26: modules — the .cppm files work as-is
#   #else
#     // C++23 fallback: headers replace modules, namespace replaces export
#     #include <cstdint>
#     #include <cstddef>
#     #include <array>
#     #include <span>
#     #include <bitset>
#     #include <expected>
#     #include <optional>
#     #include <string_view>
#     #include <vector>
#     #include <algorithm>
#     #include <ranges>
#     #include <functional>
#     #define QNX_MICRO_NO_MODULES 1
#   #endif
#
#   #endif
#   // ─────────────────────────────────────────────────────────────────
#
# Then each module becomes:
#
#   // types.cppm (C++26) vs types.h (C++23)
#   // ─────────────────────────────────────
#   #ifndef QNX_MICRO_NO_MODULES
#     export module qnx.types;
#     import std;
#   #else
#     #pragma once
#     #include "qnx_micro_compat.h"
#     namespace qnx::types {  // replaces export module
#   #endif
#
#   // ... all kernel code identical ...
#
#   #ifdef QNX_MICRO_NO_MODULES
#     }  // namespace qnx::types
#   #endif
#
# That's it. Same fixed arrays, same pool allocators, same MsgPool,
# same FixedString<N>, same swap-with-last, same O(1) scheduler.
# The bare-metal discipline doesn't depend on the module system.
# It depends on not touching the heap.
#
# We chose C++26 modules because they express the DAG:
#   types -> scheduler -> ipc -> memory -> driver -> pps -> kernel
# Headers would compile. Modules make the dependency graph a
# first-class artifact the compiler enforces.

# ─── Status ──────────────────────────────────────────────────────────────────

message(STATUS "")
message(STATUS "=== qnx-micro ===")
message(STATUS "clang   : ${CMAKE_C_COMPILER} (${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION})")
message(STATUS "clang++ : ${CMAKE_CXX_COMPILER}")
message(STATUS "std.cppm: ${STD_CPPM}")
message(STATUS "")
message(STATUS "Module DAG:")
message(STATUS "  std -> qnx.types -> qnx.scheduler -> qnx.ipc -> qnx.driver")
message(STATUS "                   -> qnx.memory                -> qnx.kernel")
message(STATUS "")
message(STATUS "=== TARGETS ===")
message(STATUS "  cmake --build build                  Build qnx_micro")
message(STATUS "  cmake --build build --target test    Run Catch2 tests")
if(DOXYGEN)
    message(STATUS "  cmake --build build --target doc             Generate API docs")
endif()
message(STATUS "  cmake --build build --target clean   Remove compiled artifacts")
message(STATUS "  cmake --build build --target distclean  Remove build/ entirely")
message(STATUS "")
