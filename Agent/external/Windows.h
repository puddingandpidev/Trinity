/*
{ START OF FILE }
Windows.h shim for cross-compilation on Linux (MinGW-w64).
The Maldev Academy ApiHashing library uses `#include <Windows.h>` (capital W),
which is case-sensitive and fails on Linux where the header is `windows.h`.
This shim resolves the case mismatch without modifying the vendored sources.
{ END OF FILE }
*/
#ifndef WINDOWS_H_SHIM
#define WINDOWS_H_SHIM
#include <windows.h>
#endif
