// ========================================== { START OF FILE } ========================================== //
using Trinity.Shared.Enums.Tasks;

namespace TeamServer.Services
{
    /// <summary>
    /// The command surface the shipped agent actually implements.
    /// </summary>
    /// <remarks>
    /// Kept next to the queueing code so the API refuses a command the implant cannot run instead of
    /// parking it in the task queue. Source of truth: <c>Agent/src/commands/command_dispatcher.cpp</c>
    /// (the switch on <c>TaskType</c>), plus the <c>PROCESS</c> codes it handles. When a handler is
    /// added to the agent, add it here in the same change or the endpoint stays refused.
    /// </remarks>
    public static class AgentCapabilities
    {
        public static bool IsImplemented(TaskType type, TaskCode code) => type switch
        {
            TaskType.POWERSHELL => true,
            TaskType.SHELL => true,
            TaskType.ESCALATE => true,
            TaskType.TOKEN => true,
            TaskType.KEYLOGGER => true,
            TaskType.WIN_TASK => true,
            TaskType.NET_ASM => true,
            TaskType.GET_UID => true,
            TaskType.KILL => true,
            TaskType.UPLOAD => true,
            TaskType.DOWNLOAD => code == TaskCode.DOWNLOAD,
            TaskType.PROCESS => code is TaskCode.RUN_U or TaskCode.KILL_PROCESS or TaskCode.SLEEP or TaskCode.RUN,
            TaskType.SET => code == TaskCode.UPDATE_HOSTS,
            _ => false,
        };
    }
}
// ========================================== { END OF FILE } ========================================== //
