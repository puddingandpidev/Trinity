//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using TeamServer.Interface;
using TeamServer.Repositories;
using TeamServer.Services.Factories;
using Trinity.Shared.DTOs.Command;
using Trinity.Shared.Enums.Tasks;
using Trinity.Shared.Errors;
using Trinity.Shared.Interfaces;
using Trinity.Shared.Results;

namespace TeamServer.Services
{
    public class CommandService : ICommandService
    {
        private readonly CommandFactory _commandFactory;
        private readonly CommandRepository _commandRepo;
        private readonly TaskStatusRepository _statusRepo;
        private readonly AgentRepository _agentRepo;
        private readonly ToolRepository _toolRepo;

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Initializes a new instance of CommandService with the specified dependencies.
        /// </summary>
        /// <remarks>Dependencies are required for command creation, persistence, and agent/task status
        /// management.</remarks>
        /// <param name="database">Provides database access and persistence operations.</param>
        /// <param name="commandFactory">Factory for creating Command instances.</param>
        /// <param name="commandRepository">Persists and retrieves Command entities.</param>
        /// <param name="taskStatusRepository">Manages persistence and retrieval of task status information.</param>
        /// <param name="agentRepository">Accesses and manages agent data.</param>
        /// <param name="toolRepository">Resolves an assembly path to a registered Armoury tool.</param>
        public CommandService(DatabaseService database, CommandFactory commandFactory, CommandRepository commandRepository, TaskStatusRepository taskStatusRepository, AgentRepository agentRepository, ToolRepository toolRepository)
        {
            _commandFactory = commandFactory;
            _commandRepo = commandRepository;
            _statusRepo = taskStatusRepository;
            _agentRepo = agentRepository;
            _toolRepo = toolRepository;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Asynchronously queue a command as a task for the specified agent and persist the created task.
        /// </summary>
        /// <remarks>Assigns the task status to 'Queued', creates the task via the command factory,
        /// enqueues it through the repository, and commits changes.</remarks>
        /// <param name="command">Command to enqueue as a task.</param>
        /// <param name="type">Type of command used to create the task.</param>
        /// <param name="agentID">Identifier of the agent that will own the queued task.</param>
        /// <returns>A Result indicating success, or an error result when the specified agent is not found.</returns>
        public async Task<Result<CommandCreationResponse>> QueueTaskAsync(int agentID, TaskType type, TaskCode code, ICommand command)
        {
            var agent = await _agentRepo.GetAgentByIDAsync(agentID);
            if (agent == null)
            {
                return AgentError.NotFound(agentID);
            }

            // Refuse what the agent cannot run. The agent's dispatcher implements
            // PowerShell, Shell, Escalate, Token, Keylogger, WinTask, NetAsm, GetUid, Kill,
            // Upload, Download and Process{RunU, KillProcess}; anything else was accepted here,
            // queued, and then sat in QUEUED forever because the implant answered
            // "Unsupported task type" to a task the operator never sees resolved.
            if (!AgentCapabilities.IsImplemented(type, code))
            {
                return CommandError.Unsupported(type, code);
            }


            // File transfer tasks carry the campaign identifier so the agent can build the
            // /api/files/{campaignId}/{fileName} URL.
            switch (command)
            {
                case UploadDTO upload when string.IsNullOrEmpty(upload.TransferID):
                    upload.TransferID = agent.CampaignID.ToString();
                    break;
                case DownloadDTO download when string.IsNullOrEmpty(download.TransferID):
                    download.TransferID = agent.CampaignID.ToString();
                    break;
                case DotNetAsmDTO assembly:
                {
                    // The agent executes an in-memory blob, so the path is resolved and encoded
                    // server-side and never reaches the wire (the legacy QueueNetAsmTask did the
                    // same). The path must name a registered Armoury tool, as upstream does
                    // (89b6440 CommandService.cs:80-83) - a path the server can open is not the
                    // same thing as a tool the operator uploaded.
                    var tool = await _toolRepo.GetToolByPathAsync(assembly.AssemblyPath);
                    if (tool is null)
                    {
                        return ToolError.NotFound(assembly.AssemblyPath);
                    }
                    if (!File.Exists(assembly.AssemblyPath))
                    {
                        return FileError.FileNotFound(assembly.AssemblyPath);
                    }

                    byte[] bytes = await File.ReadAllBytesAsync(assembly.AssemblyPath);
                    command = new DotNetAsmRequest
                    {
                        Assembly = Convert.ToBase64String(bytes),
                        Arguments = assembly.Arguments,
                    };
                    break;
                }
            }

            var task = _commandFactory.Create(agentID, type, code, command);
            await _commandRepo.QueueAsync(task);
            await _commandRepo.CommitAsync();

            return FormatCreationResponse(ref type, ref code, task.ID, ref command);
        }

        private CommandCreationResponse FormatCreationResponse(ref TaskType type, ref TaskCode code, int taskID, ref ICommand command)
        {
            string message = type switch
            {

                TaskType.POWERSHELL => $"Tasked agent to run: {((PowerShellDTO)command).Commandlet}",
                TaskType.SHELL => $"Tasked agent to run: {((ShellDTO)command).Command}",
                TaskType.TOKEN => $"Tasked agent to {((TokenDTO)command).Operation} token",
                TaskType.ESCALATE => $"Tasked agent to escalate via {((EscalateDTO)command).RegKey}",
                TaskType.PROCESS when code.Equals(TaskCode.SPAWN_TO) => $"Tasked agent to set spawn to: {((SpawnToDTO)command).Path}",
                TaskType.PROCESS when code.Equals(TaskCode.KILL_PROCESS) => $"Tasked agent to kill process: {((KillProcessDTO)command).PID}",
                TaskType.PROCESS when code.Equals(TaskCode.RUN_AS) => $"Tasked agent to run command as {((RunAsDTO)command).DomainUser}",
                TaskType.PROCESS when code.Equals(TaskCode.RUN) => $"Tasked agent to run program: {((RunDTO)command).Program}",
                TaskType.PROCESS when code.Equals(TaskCode.RUN_U) => $"Tasked agent to run command under process: {((RunUDTO)command).PID}",
                TaskType.DOWNLOAD when code.Equals(TaskCode.DOWNLOAD) => $"Tasked agent to download {((DownloadDTO)command).Path}",
                TaskType.DOWNLOAD when code.Equals(TaskCode.CANCEL_DOWNLOAD) => $"Tasked agent to cancel download with taskID: {((CancelDownloadDTO)command).TaskID}",
                TaskType.UPLOAD => $"Tasked agent to upload file: {((UploadDTO)command).Path}",
                TaskType.SET when code.Equals(TaskCode.UPDATE_HOSTS) => $"Tasked agent to update hosts: {((UpdateHostsDTO)command).Hosts}",
                TaskType.SET when code.Equals(TaskCode.SLEEP) => $"Tasked agent to sleep for {((SetSleepDTO)command).Sleep}s",
                TaskType.GET_UID => $"Tasked agent to get user ID",
                TaskType.NET_ASM => $"Tasked agent to execute assembly ({(string.IsNullOrEmpty(((DotNetAsmRequest)command).Arguments) ? "no arguments" : ((DotNetAsmRequest)command).Arguments)})",
                TaskType.OBJECT_FILE => $"Tasked agent to execute BOF: {((BofDTO)command).BofPath}",
                TaskType.KEYLOGGER => $"Tasked agent to {((KeyloggerDTO)command).Operation} the keylogger",
                TaskType.WIN_TASK => $"Tasked agent to persist via scheduled task: {((WinTaskDTO)command).TaskName}",
                TaskType.KILL => $"Tasked agent to terminate",

                // TaskType.NOP is a real enum member, so a switch without this arm threw
                // SwitchExpressionException for it (and for any future value) instead of
                // answering the caller.
                _ => $"Tasked agent ({type})"
            };

            return new CommandCreationResponse(message, taskID);
        }
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//