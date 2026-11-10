// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32
#include <Windows.h>

#include "agent/util/api_resolver.h"
#include "agent/util/strings.h"
#include "xorstr/xorstr.hpp"

namespace agent::util {

    std::string ToUtf8(const std::wstring& wide) {
        if (wide.empty()) {
            return {};
        }
        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto to_utf8 =
            agent::util::resolve_api<decltype(&::WideCharToMultiByte)>(kernel32, xorstr_("WideCharToMultiByte"));
        const int size = to_utf8(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) {
            return {};
        }
        std::string result(static_cast<std::size_t>(size), '\0');
        const int copied =
            to_utf8(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
        if (copied <= 0) {
            return {};
        }
        result.resize(static_cast<std::size_t>(copied));
        return result;
    }

    std::wstring ToWide(const std::string& utf8) {
        if (utf8.empty()) {
            return {};
        }
        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto to_wide =
            agent::util::resolve_api<decltype(&::MultiByteToWideChar)>(kernel32, xorstr_("MultiByteToWideChar"));
        const int size = to_wide(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (size <= 0) {
            return {};
        }
        std::wstring result(static_cast<std::size_t>(size), L'\0');
        const int copied = to_wide(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), size);
        if (copied <= 0) {
            return {};
        }
        result.resize(static_cast<std::size_t>(copied));
        return result;
    }

}  // namespace agent::util
#endif
// ========================================== { END OF FILE } ========================================== //
