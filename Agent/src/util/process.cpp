// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32
#include <array>
#include <cstdio>
#include <string>

#include "agent/util/process.h"
#include "xorstr/xorstr.hpp"

namespace agent::util {

    std::string CaptureCommandOutput(const std::string& cmd) {
        std::string output;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (pipe == nullptr) {
            return xorstr_("Failed to execute command");
        }
        std::array<char, 4096> buffer{};
        while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
            output += buffer.data();
        }
        pclose(pipe);
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
            output.pop_back();
        }
        return output;
    }

}  // namespace agent::util
#endif
// ========================================== { END OF FILE } ========================================== //
