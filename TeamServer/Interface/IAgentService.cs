// ========================================================================== { START OF FILE } ========================================================================== //
using Trinity.Shared.DTOs.Agent;
using Trinity.Shared.Results;

namespace TeamServer.Interface
{
    public interface IAgentService
    {
        Task<Result<List<AgentResponse>>> GetAgentsAsync();
        /// <summary>
        /// Processes a check-in. <paramref name="sleepMs"/> and <paramref name="jitter"/> come from
        /// the agent's own headers and are stored so the operator sees the interval the agent is
        /// actually using, not the one the server last asked for.
        /// </summary>
        Task<Result<string>> Checkin(string blob, int sleepMs = 0, int jitter = 0);
    }
}
// ========================================================================== { END OF FILE } ========================================================================== //