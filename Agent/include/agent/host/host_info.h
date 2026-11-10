// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <string>
#include <utility>

#include "agent/models/checkin_models.h"

namespace agent::host {

    std::string GetUsername();
    std::string GetInternalIP();
    std::string GetExternalIP();
    std::string GetOSVersion();
    std::string GetMachineMAC();
    std::string GetMotherboard();
    int GetRamMb();
    std::pair<double, double> GetDiskSize();
    int GetCpuCount();
    int GetPID();
    std::string GetProcessName();
    /// Code Integrity (CIG) status of the host, as reported in the check-in's Integrity field.
    /// (There is no token-integrity helper; the check-in's Integrity field is the CIG status.)
    std::string GetCodeIntegrityStatus();
    models::InitialCheckinRequest GetHostInformation();

}  // namespace agent::host
// ========================================== { END OF FILE } ========================================== //
