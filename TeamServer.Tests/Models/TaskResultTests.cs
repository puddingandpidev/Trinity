// ============================================================ { START OF FILE } ============================================================ //
using System.Text.Json;
using Trinity.Shared.Enums.Tasks;
using Trinity.Shared.Models;
using SharedTask = Trinity.Shared.Models.Task;

namespace TeamServer.Tests.Models
{
    /// <summary>
    /// Pins how a task records a successful report.
    /// <para>
    /// A chunked download reports once per 512 KiB chunk against the same task, and
    /// <c>TaskResult.TaskID</c> carries a unique index. The original code assigned a fresh
    /// <c>TaskResult</c> on every report, so the second chunk's save failed with
    /// <c>23505 duplicate key value violates unique constraint "IX_TaskResult_TaskID"</c>: the check-in
    /// errored, the result was never stored, the task stayed queued forever and every later task
    /// starved behind it. It also reported the task as SUCCESS after the first chunk.
    /// </para>
    /// </summary>
    public class TaskResultTests
    {
        private static SharedTask DownloadTask(string command)
        {
            return new SharedTask
            {
                AgentID = 7,
                CommandType = TaskType.DOWNLOAD,
                Code = TaskCode.DOWNLOAD,
                Command = JsonDocument.Parse(command),
                Status = Status.QUEUED,
                CreatedAt = DateTime.UtcNow,
            };
        }

        [Fact]
        public void RecordSuccess_FirstReport_StoresTheOutputUnderTheTaskID()
        {
            SharedTask task = DownloadTask("""{"Path":"C:\\probe.txt","Final":true}""");

            Status status = task.RecordSuccess("Download successful: probe.txt");

            Assert.NotNull(task.Result);
            Assert.Equal(task.ID, task.Result!.TaskID);
            Assert.Equal("Download successful: probe.txt", task.Result.Result);
            Assert.Equal(Status.SUCCESS, status);
        }

        [Fact]
        public void RecordSuccess_SecondReport_UpdatesTheSameRowInsteadOfInsertingOne()
        {
            SharedTask task = DownloadTask("""{"Path":"C:\\probe.bin","Final":true}""");

            Status first = task.RecordSuccess("File chunk stored (524288/2097152)");
            TaskResult? stored = task.Result;

            Status second = task.RecordSuccess("File chunk stored (2097152/2097152) - transfer complete");

            // The unique index is on TaskID, so a second row would not save at all; the fix is to
            // keep the row and overwrite its output.
            Assert.Same(stored, task.Result);
            Assert.Equal("File chunk stored (2097152/2097152) - transfer complete", task.Result!.Result);
            Assert.Equal(task.ID, task.Result.TaskID);
            Assert.Equal(Status.SUCCESS, first);
            Assert.Equal(Status.SUCCESS, second);
        }

        [Fact]
        public void RecordSuccess_NonFinalChunk_KeepsTheTaskQueued()
        {
            // The server rewrites the command into a DownloadChunkPayload; only the chunk carrying
            // Final completes the transfer, so an earlier chunk must not read as SUCCESS.
            SharedTask task = DownloadTask(
                """{"Path":"C:\\probe.bin","Offset":524288,"TotalBytes":2097152,"Final":false}"""
            );

            Assert.Equal(Status.QUEUED, task.RecordSuccess("File chunk stored (524288/2097152)"));
            Assert.NotNull(task.Result);
            Assert.Equal("File chunk stored (524288/2097152)", task.Result!.Result);
        }

        [Fact]
        public void RecordSuccess_FinalChunk_CompletesTheTask()
        {
            SharedTask task = DownloadTask(
                """{"Path":"C:\\probe.bin","Offset":2097152,"TotalBytes":2097152,"Final":true}"""
            );

            Assert.Equal(
                Status.SUCCESS,
                task.RecordSuccess("File chunk stored (2097152/2097152) - transfer complete")
            );
        }

        [Fact]
        public void RecordSuccess_OfAPlainTask_IsSuccess()
        {
            // A command the server never rewrote (no Final key) is terminal on its first report.
            SharedTask download = DownloadTask("""{"Path":"C:\\probe.txt","TransferID":"3"}""");

            Assert.Equal(Status.SUCCESS, download.RecordSuccess("Download successful: probe.txt"));

            SharedTask shell = new()
            {
                AgentID = 7,
                CommandType = TaskType.SHELL,
                Code = TaskCode.NO_CODE,
                Command = JsonDocument.Parse("""{"Command":"whoami"}"""),
                Status = Status.QUEUED,
                CreatedAt = DateTime.UtcNow,
            };

            Assert.Equal(Status.SUCCESS, shell.RecordSuccess("pi-user"));
        }
    }
}
// ============================================================ { END OF FILE } ============================================================ //
