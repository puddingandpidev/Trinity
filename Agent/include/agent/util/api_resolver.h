// ========================================== { START OF FILE } ========================================== //
#pragma once

#ifdef _WIN32

#include <cstdint>

#include <windows.h>

#include "api_hashing/ApiHashing.h"
#include "api_hashing/StringHashing.h"
#include "xorstr/xorstr.hpp"

// =========================================================================
// Hash-based Win32 API resolver.
//
// Resolves export functions by their CRC32b 32-bit name hash via the Maldev
// Academy ApiHashing library, so the exported names do not appear in the
// binary's import table. Module handles are likewise resolved by hash.
//
// All names are passed as xorstr-encrypted strings (xorstr_("...")) which are
// decrypted at runtime and hashed on the fly. No plaintext API names or
// precomputed hash constants appear in the binary.
//
// Ref: Maldev-Academy/ApiHashing (https://github.com/Maldev-Academy/ApiHashing)
//      GetProcAddress (https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getprocaddress)
// =========================================================================
namespace agent::util {

    // Computes the CRC32b hash of a runtime-decrypted string name.
    [[nodiscard]] inline std::uint32_t hash_name(const char* name) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) — HashStringCrc32bA is read-only
        return static_cast<std::uint32_t>(HashStringCrc32bA(const_cast<char*>(name)));
    }

    // Resolves the export identified by `name` in `module` and returns it as a
    // function pointer of type `FunctionPtr`. `FunctionPtr` is obtained with
    // `decltype(&ApiName)`, which does not emit a static import for ApiName.
    // `name` should be an xorstr-encrypted string: xorstr_("CreateProcessW").
    template <typename FunctionPtr>
    [[nodiscard]] FunctionPtr resolve_api(HMODULE module, const char* name) {
        FARPROC address = GetProcAddressH(module, hash_name(name));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return reinterpret_cast<FunctionPtr>(address);
    }

    // Returns the handle for the module identified by `name`, loading it on
    // demand if it is not already loaded. The handle is intentionally not
    // freed for the process lifetime.
    // `name` should be an xorstr-encrypted string: xorstr_("kernel32.dll").
    //
    // GetModuleHandleH only walks the PEB InLoadOrderModuleList, so modules that
    // are not yet mapped (iphlpapi.dll, winhttp.dll, ...) are absent from it.
    // Resolve LoadLibraryA by hash from kernel32 (always loaded) and load the
    // module instead of returning NULL.
    [[nodiscard]] inline HMODULE get_module(const char* name) {
        const std::uint32_t module_hash = hash_name(name);
        if (HMODULE module = GetModuleHandleH(module_hash); module != nullptr) {
            return module;
        }

        const HMODULE kernel32 = GetModuleHandleH(hash_name(xorstr_("kernel32.dll")));
        if (kernel32 == nullptr) {
            return nullptr;
        }

        using load_library_a_t = HMODULE(WINAPI*)(LPCSTR);
        const auto load_library = resolve_api<load_library_a_t>(kernel32, xorstr_("LoadLibraryA"));
        return load_library != nullptr ? load_library(name) : nullptr;
    }

}  // namespace agent::util

#endif  // _WIN32
// ========================================== { END OF FILE } ========================================== //
