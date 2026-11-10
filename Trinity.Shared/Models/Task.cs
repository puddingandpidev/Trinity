//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using System;
using System.Collections.Generic;
using System.ComponentModel.DataAnnotations;
using System.Text;
using System.Text.Json;
using Trinity.Shared.Enums;
using Trinity.Shared.Enums.Tasks;
using Trinity.Shared.Interfaces;

namespace Trinity.Shared.Models
{
    public class Task
    {
        public int ID { get; set; }
        public required int AgentID { get; set; }
        public required TaskType CommandType { get; set; }
        public required TaskCode Code { get; set; }
        public required JsonDocument Command { get; set; }
        public required Status Status { get; set; }
        public required DateTime CreatedAt { get; set; }
        public TaskResult? Result { get; set; }
        public Error? Error { get; set; }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Records one successful report for this task and returns the status it must carry.
        /// </summary>
        /// <remarks>
        /// A chunked download reports once per chunk against the same task, and
        /// <c>TaskResult.TaskID</c> is unique (<c>IX_TaskResult_TaskID</c>). Assigning a fresh
        /// <see cref="TaskResult"/> on the second report made the save fail with a duplicate key,
        /// so the check-in errored, the result was never stored, the task stayed queued and every
        /// later task starved behind it. The row is therefore updated in place, and the task stays
        /// <see cref="Status.QUEUED"/> until the chunk marked <c>Final</c>.
        /// </remarks>
        public Status RecordSuccess(string output)
        {
            if (Result is null)
            {
                Result = new TaskResult { TaskID = ID, Result = output };
            }
            else
            {
                Result.Result = output;
            }

            return IsChunkTransferPending() ? Status.QUEUED : Status.SUCCESS;
        }

        /// <summary>
        /// True while a chunked transfer still owes the agent another chunk: the server rewrites the
        /// command into a <c>DownloadChunkPayload</c> and only the chunk carrying <c>Final</c> completes
        /// the task.
        /// </summary>
        private bool IsChunkTransferPending()
        {
            return CommandType == TaskType.DOWNLOAD
                && Code == TaskCode.DOWNLOAD
                && Command.RootElement.TryGetProperty("Final", out JsonElement final)
                && final.ValueKind == JsonValueKind.False;
        }
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//