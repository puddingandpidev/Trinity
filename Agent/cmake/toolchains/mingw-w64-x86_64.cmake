set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)

# Link MinGW runtime libraries into executables instead of requiring them at runtime.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")

# Release/MinSizeRel: self-contained and stripped. `-s` drops the symbol table and DWARF
# debug sections (a few hundred KB) that the Debian mingw CRT startup objects embed in
# every link; nothing the payload needs at runtime.
set(CMAKE_EXE_LINKER_FLAGS_RELEASE_INIT "-s")
set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL_INIT "-s")

# Debug/RelWithDebInfo: keep DWARF and frame pointers so the payload can be debugged on
# the guest (gdb reads DWARF for the MinGW ABI). These builds are for debugging only —
# the TeamServer's payload builder always configures Release.
set(CMAKE_C_FLAGS_DEBUG_INIT "-g -O0 -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-g -O0 -fno-omit-frame-pointer")
set(CMAKE_C_FLAGS_RELWITHDEBINFO_INIT "-g -O1 -fno-omit-frame-pointer -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO_INIT "-g -O1 -fno-omit-frame-pointer -DNDEBUG")

# Don't try to find programs *on the target* (there is none)
set(CMAKE_C_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)