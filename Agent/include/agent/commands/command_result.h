// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <string>
#include <utility>

namespace agent::commands {

    /// <summary>
    /// Outcome of a dispatched command.
    /// <para>
    /// The agent reports the task as Status=SUCCESS only when <c>success</c> is true, so a command
    /// that could not run has to say so here. Returning a diagnostic string on an otherwise
    /// successful task is what made missing command fields invisible: the server stored the
    /// complaint as the task's output and marked the task complete.
    /// </para>
    /// </summary>
    struct CommandResult {
        bool success = true;
        std::string output;

        [[nodiscard]] static CommandResult Ok(std::string output) { return CommandResult{true, std::move(output)}; }

        [[nodiscard]] static CommandResult Error(std::string message) {
            return CommandResult{false, std::move(message)};
        }
    };

}  // namespace agent::commands
// ========================================== { END OF FILE } ========================================== //
