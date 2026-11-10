// ========================================== { START OF FILE } ========================================== //
#include "agent/util/trace.h"

#if defined(AGENT_TRACE) && defined(_WIN32)

#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

#include <windows.h>

#include "xorstr/xorstr.hpp"

namespace agent::util::trace {

    namespace {

        std::mutex g_lock;

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        // Wall-clock stamp with milliseconds. A killed process has no exit hook, so the
        // timestamp is the only ordering evidence a crash leaves behind.
        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        [[nodiscard]] std::string Timestamp() {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            std::ostringstream stamp;
            stamp << std::setfill('0') << std::setw(4) << now.wYear << '-' << std::setw(2) << now.wMonth << '-'
                  << std::setw(2) << now.wDay << ' ' << std::setw(2) << now.wHour << ':' << std::setw(2) << now.wMinute
                  << ':' << std::setw(2) << now.wSecond << '.' << std::setw(3) << now.wMilliseconds;
            return stamp.str();
        }

        /// %TEMP%\agent-<pid>.log — one file per process, so parallel runs never interleave.
        [[nodiscard]] std::string TracePath() {
            std::ostringstream path;
            std::array<char, MAX_PATH + 1> temp{};
            const DWORD length = GetTempPathA(static_cast<DWORD>(temp.size()), temp.data());
            if (length > 0 && length < temp.size()) {
                path << temp.data();
            }
            path << "agent-" << GetCurrentProcessId() << ".log";
            return path.str();
        }

        /// Opens the trace file on first use; the stream stays closed when that fails.
        std::ofstream& File() {
            static std::ofstream file;
            static bool open_attempted = false;
            if (!open_attempted) {
                open_attempted = true;
                file.open(TracePath(), std::ios::app);
            }
            return file;
        }

        void Write(std::string_view stage, std::string_view message) {
            const std::lock_guard<std::mutex> guard(g_lock);
            std::ofstream& file = File();
            if (!file.is_open()) {
                return;
            }
            file << '[' << Timestamp() << "] pid=" << GetCurrentProcessId() << ' ' << "stage=" << stage << ' '
                 << message
                 << '\n'
                 // A killed process runs no cleanup: never leave a line sitting in a buffer.
                 << std::flush;
        }

        /// Formats `what` and `code` once, in hex and decimal, so both greps work.
        void WriteCode(std::string_view stage, std::string_view what, std::uint32_t code) {
            std::ostringstream message;
            message << what << "=0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << code
                    << std::dec << " (" << code << ')';
            Write(stage, message.str());
        }

    }  // namespace

    void Emit(std::string_view stage, std::string_view message) { Write(stage, message); }

    void EmitCode(std::string_view stage, std::string_view what, std::uint32_t code) { WriteCode(stage, what, code); }

    void EmitLastError(std::string_view stage, std::string_view what) {
        WriteCode(stage, std::string{what} + xorstr_(" last_error"), GetLastError());
    }

}  // namespace agent::util::trace

#endif  // AGENT_TRACE && _WIN32
// ========================================== { END OF FILE } ========================================== //
