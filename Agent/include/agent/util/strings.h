// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <string>

namespace agent::util {

    std::string ToUtf8(const std::wstring& wide);
    std::wstring ToWide(const std::string& narrow);

}  // namespace agent::util
// ========================================== { END OF FILE } ========================================== //
