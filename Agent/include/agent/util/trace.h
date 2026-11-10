// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <cstdint>
#include <sstream>
#include <string_view>
#include <utility>

//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
// Structured trace for a live guest run — layer 1 of .pi/notes/live-debugging.md.
//
// - Compile-time switch: nothing exists unless the build defines AGENT_TRACE (the `*-debug`
//   presets do; CMakePayloadBuilder always builds Release, so a shipped payload carries neither
//   this code nor its strings).
// - One timestamped line per event in %TEMP%\agent-<pid>.log, flushed immediately: a killed
//   process runs no cleanup, so the last line must be the last thing that happened.
// - Stage breadcrumbs at every loop step; a failure also records GetLastError and, for a
//   WinHTTP request, the HTTP status code (see .pi/notes/win32-traps.md for why the status
//   does not come from a "WinHttpQueryStatusCode").
//
// The off-switch is a macro, not an inline no-op, so a Release build does not even compile the
// arguments and no trace string literal reaches the binary. Call sites pass their literals through
// xorstr_() themselves (the build is compiled with AGENT_TRACE off, so those calls disappear with the
// macro and never reach .rdata in either shape) — `Agent/xorstr.py` then sees and enforces them.
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//

namespace agent::util::trace {

#if defined(AGENT_TRACE)
    /// Appends one line — timestamp, pid, stage and message — to the trace file and flushes it.
    void Emit(std::string_view stage, std::string_view message);

    /// Appends a line carrying `what` and `code` in hex and decimal (WinHTTP status, HRESULT, …).
    void EmitCode(std::string_view stage, std::string_view what, std::uint32_t code);

    /// Appends a line carrying `what` and the calling thread's GetLastError() value.
    void EmitLastError(std::string_view stage, std::string_view what);

    /// Appends a line with the concatenation of every argument after the stage.
    template <typename... Args>
    void Line(std::string_view stage, Args&&... args) {
        std::ostringstream stream;
        (stream << ... << std::forward<Args>(args));
        Emit(stage, stream.str());
    }
#endif

}  // namespace agent::util::trace

#if defined(AGENT_TRACE)
#define AGENT_TRACE_STAGE(stage) ::agent::util::trace::Emit(stage, {})
#define AGENT_TRACE_LOG(stage, ...) ::agent::util::trace::Line(stage, __VA_ARGS__)
#define AGENT_TRACE_LAST_ERROR(stage, what) ::agent::util::trace::EmitLastError(stage, what)
#define AGENT_TRACE_CODE(stage, what, code) ::agent::util::trace::EmitCode(stage, what, code)
#else
#define AGENT_TRACE_STAGE(stage) ((void)0)
#define AGENT_TRACE_LOG(stage, ...) ((void)0)
#define AGENT_TRACE_LAST_ERROR(stage, what) ((void)0)
#define AGENT_TRACE_CODE(stage, what, code) ((void)0)
#endif
// ========================================== { END OF FILE } ========================================== //
