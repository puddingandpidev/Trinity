# ========================================== { START OF FILE } ========================================== #
# mingw-w64-clang.cmake — Clang/LLD toolchain for cross-compiling the Agent to Windows x86_64.
#
# This is the default payload toolchain (CMakePayloadBuilder and appsettings.json select it).
# It drives Clang with LLD against the MinGW-w64 ABI, producing a *structurally distinct* PE
# (different front-end codegen, different linker, different CRT/import layout) from the GCC
# toolchain (mingw-w64-x86_64.cmake), which remains available as the alternative. Behaviour is
# identical: same wire protocol, same hashed API resolution. Static signatures that key on
# GCC/MinGW codegen or GNU ld's PE layout no longer match.
#
# Clang/LLD also emit CodeView + a PDB in Debug/RelWithDebInfo builds (see the config block at
# the end of this file), which is what makes the payload debuggable with cdb/WinDbg.
#
# Clang auto-detects the mingw-w64 installation (Debian/Ubuntu), so no --sysroot or
# --gcc-toolchain flags are needed (passing them actually breaks header discovery):
#   - CRT/Windows headers under /usr/x86_64-w64-mingw32/include
#   - libstdc++ under /usr/lib/gcc/x86_64-w64-mingw32/<thread-model>/include/c++/
#
# Build with:
#   cmake -S Agent -B Agent/out/build/mingw-clang-x86_64 --preset mingw-clang-x86_64
#   cmake --build Agent/out/build/mingw-clang-x86_64 --config Release
# ========================================== { END OF FILE } ========================================== #

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Don't search for programs *on the target* (there is none). Set before find_program so
# the compiler lookup below is unaffected by the "ONLY" root-path modes.
set(CMAKE_C_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Resolve the compiler version-portably: Debian ships clang-19, Ubuntu ships clang (18).
# Probed in this order so a pinned newer toolchain wins when both exist.
find_program(AGENT_CLANG_C NAMES clang-19 clang REQUIRED)
find_program(AGENT_CLANG_CXX NAMES clang++-19 clang++ REQUIRED)
set(CMAKE_C_COMPILER "${AGENT_CLANG_C}")
set(CMAKE_CXX_COMPILER "${AGENT_CLANG_CXX}")

# Cross-compile to Windows using the MinGW-w64 (Windows GNU) ABI.
set(CMAKE_C_COMPILER_TARGET x86_64-w64-windows-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-windows-gnu)

# Self-contained PE; LLD links, no mingw runtime DLLs required on the target.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -static")

# Release/MinSizeRel: whole-program LTO via LLD. Cross-translation-unit inlining +
# dead-code elimination reshape the call graph and eliminate the per-TU codegen
# boundaries that static signatures rely on. LLD/COFF supports LTO for the MinGW target.
# `-s` drops the symbol table and debug sections (~350 KB) the mingw CRT objects carry.
set(CMAKE_C_FLAGS_RELEASE_INIT "-flto")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-flto")
set(CMAKE_C_FLAGS_MINSIZEREL_INIT "-flto")
set(CMAKE_CXX_FLAGS_MINSIZEREL_INIT "-flto")
set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT "-flto -s")
set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL_INIT "-flto -s")

# Debug/RelWithDebInfo: no LTO (it obscures line info), frame pointers kept, and CodeView
# debug info + a PDB emitted next to the PE so cdb/WinDbg can debug it and symbolise crash
# dumps (GCC can only emit DWARF, hence gdb for that toolchain). Debug builds are for the
# guest debugger only — the TeamServer's payload builder always configures Release.
set(CMAKE_C_FLAGS_DEBUG_INIT "-g -gcodeview -O0 -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-g -gcodeview -O0 -fno-omit-frame-pointer")
set(CMAKE_C_FLAGS_RELWITHDEBINFO_INIT "-g -gcodeview -O1 -fno-omit-frame-pointer -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT "-g -gcodeview -O1 -fno-omit-frame-pointer -DNDEBUG")
set(CMAKE_EXE_LINKER_FLAGS_DEBUG_INIT "-Wl,--pdb=Agent.pdb")
set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO_INIT "-Wl,--pdb=Agent.pdb")

# LLD in COFF (PE) mode rejects GNU ld's `--dependency-file` flag. CMake probes
# `ld.lld --help` in its default ELF mode, sees `--dependency-file` advertised,
# and wrongly enables linker-depfile generation for the PE target. Disable it.
set(CMAKE_C_LINKER_DEPFILE_SUPPORTED FALSE)
set(CMAKE_CXX_LINKER_DEPFILE_SUPPORTED FALSE)
set(CMAKE_C_LINK_DEPENDS_USE_LINKER FALSE)
set(CMAKE_CXX_LINK_DEPENDS_USE_LINKER FALSE)
