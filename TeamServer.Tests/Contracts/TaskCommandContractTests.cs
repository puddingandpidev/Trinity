// ============================================================ { START OF FILE } ============================================================ //
using System.Text.Json;
using TeamServer.Services.Factories;
using Trinity.Shared.DTOs.Command;
using Trinity.Shared.DTOs.Task;
using Trinity.Shared.Enums.Tasks;
using Trinity.Shared.Interfaces;
using Xunit;

namespace TeamServer.Tests.Contracts
{
    /// <summary>
    /// Pins the task command wire contract.
    /// <para>
    /// The agent reads a fixed set of JSON keys per TaskType
    /// (<c>Agent/src/task/task_validator.cpp</c>). When a server-side writer dropped one of them the
    /// agent executed the command with an empty field and reported success, so the defect only
    /// showed up as a feature that "did nothing". These tests build a task from a real command DTO
    /// through the real factories and assert that every key the agent needs is present and carries
    /// a value - the agent treats an absent and an empty field the same way.
    /// </para>
    /// <para>
    /// Wire key names are frozen (see AGENTS.md): the agent also accepts some legacy lower-case
    /// keys, but the server's own schema is PascalCase and these tests fail if that changes.
    /// </para>
    /// </summary>
    public class TaskCommandContractTests
    {
        private const int AgentID = 7;

        private sealed record CommandCase(
            TaskType Type,
            TaskCode Code,
            ICommand Command,
            string[] RequiredKeys
        );

        // Keyed by name because xUnit can only serialise simple theory data; a non-serialisable
        // argument would make the runner emit a warning per case.
        private static readonly Dictionary<string, CommandCase> Cases = new()
        {
            ["shell"] = new(
                TaskType.SHELL,
                TaskCode.NO_CODE,
                new ShellDTO { Command = "whoami" },
                new[] { "Command" }
            ),
            ["powershell"] = new(
                TaskType.POWERSHELL,
                TaskCode.NO_CODE,
                new PowerShellDTO { Commandlet = "Get-Date", Arguements = "-Format s" },
                new[] { "Commandlet" }
            ),
            ["runu"] = new(
                TaskType.PROCESS,
                TaskCode.RUN_U,
                new RunUDTO
                {
                    PID = 4412,
                    Command = "C:\\Windows\\System32\\cmd.exe",
                    Arguements = "/c whoami",
                },
                new[] { "PID", "Command" }
            ),
            ["kill"] = new(
                TaskType.PROCESS,
                TaskCode.KILL_PROCESS,
                new KillProcessDTO { PID = 4412 },
                new[] { "PID" }
            ),
            ["wintask"] = new(
                TaskType.WIN_TASK,
                TaskCode.NO_CODE,
                new WinTaskDTO
                {
                    Path = "C:\\Users\\pi-user\\e2e_persist.exe",
                    Username = "pi-user",
                    TaskName = "TrinityE2E",
                },
                new[] { "Path", "TaskName" }
            ),
            ["keylogger"] = new(
                TaskType.KEYLOGGER,
                TaskCode.NO_CODE,
                new KeyloggerDTO { Operation = "start" },
                new[] { "Operation" }
            ),
            ["dotnetasm"] = new(
                TaskType.NET_ASM,
                TaskCode.NO_CODE,
                new DotNetAsmRequest { Assembly = "AA==", Arguments = "" },
                new[] { "Assembly" }
            ),
            ["escalate"] = new(
                TaskType.ESCALATE,
                TaskCode.NO_CODE,
                new EscalateDTO
                {
                    RegKey = "fodhelper",
                    TargetBinary = "C:\\Windows\\System32\\cmd.exe /c whoami",
                },
                new[] { "RegKey", "TargetBinary" }
            ),
            ["upload"] = new(
                TaskType.UPLOAD,
                TaskCode.NO_CODE,
                new UploadDTO { Path = "C:\\Users\\pi-user\\probe.txt", TransferID = "3" },
                new[] { "Path", "TransferID" }
            ),
            ["download"] = new(
                TaskType.DOWNLOAD,
                TaskCode.DOWNLOAD,
                new DownloadDTO { Path = "C:\\Users\\pi-user\\probe.txt", TransferID = "3" },
                new[] { "Path", "TransferID" }
            ),
            ["token steal"] = new(
                TaskType.TOKEN,
                TaskCode.NO_CODE,
                new TokenDTO { Operation = "steal", PID = 4412 },
                new[] { "Operation", "PID" }
            ),
            ["token make"] = new(
                TaskType.TOKEN,
                TaskCode.NO_CODE,
                new TokenDTO
                {
                    Operation = "make",
                    Username = "pi-admin",
                    Password = "secret",
                },
                new[] { "Operation", "Username", "Password" }
            ),
            ["token rev2self"] = new(
                TaskType.TOKEN,
                TaskCode.NO_CODE,
                new TokenDTO { Operation = "rev2self" },
                new[] { "Operation" }
            ),
        };

        public static IEnumerable<object[]> CaseNames => Cases.Keys.Select(name => new object[] { name });

        [Theory]
        [MemberData(nameof(CaseNames))]
        public void TaskRequest_CarriesEveryCommandKeyTheAgentRequires(string caseName)
        {
            CommandCase testCase = Cases[caseName];

            var task = new CommandFactory().Create(AgentID, testCase.Type, testCase.Code, testCase.Command);
            string json = new TaskRequestFactory().Create(task);

            using JsonDocument document = JsonDocument.Parse(json);
            JsonElement root = document.RootElement;

            Assert.Equal(task.ID, root.GetProperty("TaskID").GetInt32());
            Assert.Equal((int)testCase.Type, root.GetProperty("TaskType").GetInt32());
            Assert.Equal((int)testCase.Code, root.GetProperty("TaskCode").GetInt32());
            Assert.Equal((int)Status.QUEUED, root.GetProperty("Status").GetInt32());

            JsonElement command = root.GetProperty("Command");
            Assert.Equal(JsonValueKind.Object, command.ValueKind);

            foreach (string key in testCase.RequiredKeys)
            {
                Assert.True(
                    command.TryGetProperty(key, out JsonElement value),
                    $"{caseName}: the command is missing '{key}' (task type {testCase.Type}). The agent rejects the task and names the field."
                );
                AssertCarriesValue(caseName, key, value);
            }
        }

        /// <summary>
        /// The agent treats an empty value as a missing one, so a key that is present but empty is
        /// just as broken as an absent key.
        /// </summary>
        private static void AssertCarriesValue(string caseName, string key, JsonElement value)
        {
            switch (value.ValueKind)
            {
                case JsonValueKind.String:
                    Assert.False(
                        string.IsNullOrEmpty(value.GetString()),
                        $"{caseName}: '{key}' is present but empty; the agent reports the field as missing."
                    );
                    break;
                case JsonValueKind.Number:
                    Assert.NotEqual(0, value.GetInt32());
                    break;
                case JsonValueKind.Null:
                    Assert.Fail($"{caseName}: '{key}' is present but null; the agent reports the field as missing.");
                    break;
            }
        }

        [Fact]
        public void DownloadChunkPayload_CarriesEveryKeyTheAgentNeeds()
        {
            // The chunk document is produced by FileTransferService.PrepareDownloadChunkAsync as a
            // typed DownloadChunkPayload; this pins its wire keys, TransferID included (it was the
            // key the old anonymous writer dropped).
            var chunk = new DownloadChunkPayload(
                "C:\\Users\\pi-user\\probe.txt",
                "3",
                "AA==",
                0,
                512,
                true
            );

            using JsonDocument document = JsonDocument.Parse(JsonSerializer.Serialize(chunk));
            JsonElement command = document.RootElement;

            foreach (string key in new[] { "Path", "TransferID", "Data", "Offset", "TotalBytes", "Final" })
            {
                Assert.True(command.TryGetProperty(key, out _), $"chunk: the document is missing '{key}'.");
            }
            Assert.Equal("3", command.GetProperty("TransferID").GetString());
            Assert.Equal("C:\\Users\\pi-user\\probe.txt", command.GetProperty("Path").GetString());
        }

        [Fact]
        public void Create_WhenTheTaskIsNull_ReturnsAnEmptyRequest()
        {
            Assert.Equal(string.Empty, new TaskRequestFactory().Create(null));
        }

        [Fact]
        public void Create_WithACommandOverride_WritesTheOverrideVerbatim()
        {
            // The download-chunk path replaces the task command with a generated document. That is
            // the writer that used to drop TransferID, so the override must reach the wire intact.
            var task = new CommandFactory().Create(
                AgentID,
                TaskType.DOWNLOAD,
                TaskCode.DOWNLOAD,
                new DownloadDTO { Path = "C:\\Users\\pi-user\\probe.txt", TransferID = "3" }
            );
            using JsonDocument commandOverride = JsonDocument.Parse(
                """{"Path":"C:\\Users\\pi-user\\probe.txt","TransferID":"3","Data":"AA==","Offset":0,"TotalBytes":1,"Final":true}"""
            );

            string json = new TaskRequestFactory().Create(task, commandOverride);

            using JsonDocument document = JsonDocument.Parse(json);
            JsonElement command = document.RootElement.GetProperty("Command");
            Assert.Equal("3", command.GetProperty("TransferID").GetString());
            Assert.Equal("C:\\Users\\pi-user\\probe.txt", command.GetProperty("Path").GetString());
            Assert.True(command.GetProperty("Final").GetBoolean());
        }
    }
}
// ============================================================ { END OF FILE } ============================================================ //
