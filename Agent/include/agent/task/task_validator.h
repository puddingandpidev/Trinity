// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <string>

#include "agent/models/checkin_models.h"

namespace agent::task {

    /// <summary>
    /// Checks a parsed task against the fields its command type requires and returns the name of
    /// the first one that is missing, or an empty string when the command is complete.
    /// <para>
    /// The names returned are the server's wire keys (Path, TransferID, Commandlet, ...), so a
    /// failure points straight at the field a writer dropped instead of at the command as a whole.
    /// This is deliberately platform-neutral: it is pure validation over the parsed model, so it
    /// can be exercised on a host build without a Windows target.
    /// </para>
    /// </summary>
    /// <param name="task">The task parsed from the check-in response.</param>
    /// <returns>The missing wire key, or an empty string when the command is complete.</returns>
    [[nodiscard]] std::string MissingCommandField(const models::CheckinResponse& task);

    /// <summary>
    /// Human-readable name of a task type (and code, for PROCESS) for diagnostics.
    /// </summary>
    /// <param name="type">The TaskType value from the wire.</param>
    /// <param name="code">The TaskCode value from the wire.</param>
    /// <returns>A short upper-case label, e.g. "UPLOAD" or "PROCESS/RUN_U".</returns>
    [[nodiscard]] std::string DescribeTaskType(int type, int code);

}  // namespace agent::task
// ========================================== { END OF FILE } ========================================== //
