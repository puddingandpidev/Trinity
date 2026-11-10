// ========================================== { START OF FILE } ========================================== //
// tools/supervisor/supervisor.cpp
//
// AgentSupervisor — logs the exit code of a payload it launches, so "the agent died silently"
// becomes a value instead of a guess (layer 1 of .pi/notes/live-debugging.md):
//
//   AgentSupervisor.exe C:\Users\pi-user\agent.exe
//
// It waits for the child, then writes one line to stdout and to %TEMP%\agent-supervisor-<pid>.log:
//
//   [2026-11-04 12:00:00.000] pid=1234 stage=child_exited child_pid=5678 exit_code=0xC0000005
//       (3221225477) name=STATUS_ACCESS_VIOLATION hint=...
//
// Reading the codes:
//   * 0xC0000005 (AV) — a real crash; a WER LocalDumps dump (if enabled) can be symbolised.
//   * 0xC000013A      — console close / CTRL_CLOSE_EVENT; the OpenSSH job-object teardown looks
//                       like this. TerminateProcess produces **no** dump.
//   * 1               — ambiguous: a plain failure, or a job teardown. Never read it alone.
//
// This is a debugging aid, never part of the payload: CMake builds it only with
// -DAGENT_SUPERVISOR=ON, and its strings are deliberately plain (it is never shipped).
// ========================================== { END OF FILE } ========================================== //

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

    std::string Timestamp() {
        std::ostringstream stamp;
#ifdef _WIN32
        SYSTEMTIME now{};
        GetLocalTime(&now);
        stamp << std::setfill('0') << std::setw(4) << now.wYear << '-' << std::setw(2) << now.wMonth << '-'
              << std::setw(2) << now.wDay << ' ' << std::setw(2) << now.wHour << ':' << std::setw(2) << now.wMinute
              << ':' << std::setw(2) << now.wSecond << '.' << std::setw(3) << now.wMilliseconds;
#endif
        return stamp.str();
    }

    std::string TempDirectory() {
        std::string directory;
#ifdef _WIN32
        std::array<char, MAX_PATH + 1> buffer{};
        const DWORD length = GetTempPathA(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length > 0 && length < buffer.size()) {
            directory = buffer.data();
        }
#endif
        return directory;
    }

    /// Opens %TEMP%\agent-supervisor-<pid>.log on first use; the stream stays closed if that fails.
    std::ofstream& LogFile() {
        static std::ofstream file;
        static bool open_attempted = false;
        if (!open_attempted) {
            open_attempted = true;
            std::ostringstream path;
            path << TempDirectory() << "agent-supervisor-" << GetCurrentProcessId() << ".log";
            file.open(path.str(), std::ios::app);
        }
        return file;
    }

    /// One supervisor line: timestamp, pid, stage and message — the same shape as the agent trace,
    /// so `grep stage=child` across both files works.
    void LogLine(const std::string& stage, const std::string& message) {
        std::ostringstream line;
        line << '[' << Timestamp() << "] pid=" << GetCurrentProcessId() << " stage=" << stage << ' ' << message;

        std::cout << line.str() << '\n' << std::flush;

        std::ofstream& file = LogFile();
        if (file.is_open()) {
            // A supervisor that is itself killed must still leave its last line behind.
            file << line.str() << '\n' << std::flush;
        }
    }

    std::string Hex(std::uint32_t value) {
        std::ostringstream stream;
        stream << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
        return stream.str();
    }

    struct ExitCodeMeaning {
        std::uint32_t code;
        const char* name;
        const char* hint;
    };

    // The codes worth naming on this guest. Anything else is reported as UNKNOWN with the raw
    // value, which is still the useful part.
    constexpr auto kExitCodes = std::to_array<ExitCodeMeaning>({
        {.code = 0x00000000, .name = "STATUS_SUCCESS", .hint = "clean exit"},
        {.code = 0x00000001,
         .name = "AMBIGUOUS_FAILURE",
         .hint = "often the OpenSSH job-object teardown (TerminateProcess, no dump) - never read alone"},
        {.code = 0xC0000005,
         .name = "STATUS_ACCESS_VIOLATION",
         .hint = "real crash - symbolise a WER LocalDumps dump if one exists"},
        {.code = 0xC000001D, .name = "STATUS_ILLEGAL_INSTRUCTION", .hint = "bad code path or CPU feature use"},
        {.code = 0xC0000094, .name = "STATUS_INTEGER_DIVIDE_BY_ZERO", .hint = "real crash"},
        {.code = 0xC00000FD, .name = "STATUS_STACK_OVERFLOW", .hint = "real crash - often runaway recursion"},
        {.code = 0xC0000135, .name = "STATUS_DLL_NOT_FOUND", .hint = "missing dependency on the guest"},
        {.code = 0xC0000139, .name = "STATUS_ENTRYPOINT_NOT_FOUND", .hint = "an import resolved to a missing export"},
        {.code = 0xC000013A,
         .name = "STATUS_CONTROL_C_EXIT",
         .hint = "console close / CTRL_CLOSE_EVENT - the job teardown looks like this"},
        {.code = 0xC0000142,
         .name = "STATUS_DLL_INIT_FAILED",
         .hint = "DllMain failed - also what CreateRestrictedToken children hit in session 0"},
        {.code = 0xC0000374, .name = "STATUS_HEAP_CORRUPTION", .hint = "real crash - heap overwrite"},
        {.code = 0xC0000409, .name = "STATUS_STACK_BUFFER_OVERRUN", .hint = "fail-fast (stack protector / __fastfail)"},
        {.code = 0xC0000602, .name = "STATUS_FAIL_FAST_EXCEPTION", .hint = "explicit fail-fast"},
        {.code = 0xFFFFFFFF,
         .name = "TERMINATED",
         .hint = "TerminateProcess(-1): Stop-Process -Force / taskkill /F - not a crash, and no dump"},
    });

    void LogExitCode(std::uint32_t exit_code, std::uint32_t child_pid) {
        std::ostringstream message;
        message << "child_pid=" << child_pid << " exit_code=" << Hex(exit_code) << " (" << exit_code << ')';

        const auto* const match = std::ranges::find_if(
            kExitCodes, [exit_code](const ExitCodeMeaning& meaning) { return meaning.code == exit_code; });
        if (match != kExitCodes.end()) {
            message << " name=" << match->name << " hint=" << match->hint;
        } else {
            message << " name=UNKNOWN";
        }
        LogLine("child_exited", message.str());

        // The agent trace is per child pid, so name it: the harness can fetch exactly this file.
        std::ostringstream trace;
        trace << TempDirectory() << "agent-" << child_pid << ".log";
        std::ostringstream trace_line;
        trace_line << "file=" << trace.str();
        if (GetFileAttributesA(trace.str().c_str()) == INVALID_FILE_ATTRIBUTES) {
            trace_line << " (absent - the payload was built without -DAGENT_TRACE=ON)";
        }
        LogLine("trace", trace_line.str());
    }

    /// The part of `GetCommandLineW()` after the supervisor's own argument, which becomes the
    /// child's command line verbatim (CreateProcess parses the executable out of it).
    const wchar_t* SkipOwnArgument(const wchar_t* command_line) {
        const wchar_t* cursor = command_line;
        while (*cursor == L' ' || *cursor == L'\t') {
            ++cursor;
        }
        if (*cursor == L'"') {
            ++cursor;
            while (*cursor != L'\0' && *cursor != L'"') {
                ++cursor;
            }
            if (*cursor != L'\0') {
                ++cursor;
            }
        } else {
            while (*cursor != L'\0' && *cursor != L' ' && *cursor != L'\t') {
                ++cursor;
            }
        }
        while (*cursor == L' ' || *cursor == L'\t') {
            ++cursor;
        }
        return cursor;
    }

}  // namespace

int main() {
#ifndef _WIN32
    std::cout << "AgentSupervisor is a Windows helper; build it with a mingw or MSVC preset.\n";
    return 2;
#else
    const wchar_t* own_argument = SkipOwnArgument(GetCommandLineW());
    if (own_argument[0] == L'\0') {
        std::cout << "usage: AgentSupervisor.exe <payload> [arguments]\n";
        return 2;
    }

    std::wstring child_command{own_argument};
    // CreateProcessW may modify the command line, so hand it writable storage.
    std::vector<wchar_t> command_line(child_command.begin(), child_command.end());
    command_line.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    std::ostringstream spawn;
    spawn << "command_line=" << std::string(child_command.begin(), child_command.end());
    LogLine("spawn", spawn.str());

    // Handles are inherited so Start-Process -RedirectStandardOutput/Error still captures the
    // payload's own stdout/stderr through this shim.
    if (CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process) ==
        FALSE) {
        LogLine("spawn_failed", "last_error=" + Hex(GetLastError()));
        return 1;
    }

    LogLine("child_started", "child_pid=" + std::to_string(process.dwProcessId));

    WaitForSingleObject(process.hProcess, INFINITE);

    DWORD exit_code = 0;
    if (GetExitCodeProcess(process.hProcess, &exit_code) == FALSE) {
        LogLine("exit_code_unreadable", "last_error=" + Hex(GetLastError()));
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 1;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    LogExitCode(exit_code, process.dwProcessId);

    // The payload's own exit status is in the log; the supervisor reports success/failure only, so
    // a caller never confuses 0xC0000005 with a shell's 8-bit view of it.
    return exit_code == 0 ? 0 : 1;
#endif
}
