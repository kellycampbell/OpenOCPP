#ifndef CHARGELAB_OPEN_FIRMWARE_LOCAL_LIMITS_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_LOCAL_LIMITS_MODULE_H

#include "openocpp/interface/component/system_interface.h"

#include <vector>
#include <string>
#include <optional>
#include <iostream>

namespace chargelab {
    struct LeaseAmps {
        int created_index;
        double allocated_amps;
        SteadyPointMillis expiry;

        std::optional<int> completed_index = std::nullopt;
    };

    namespace detail {
        struct LocalLimitState {
            std::string charge_point_id_;
            std::vector<LeaseAmps> active_leases_;

            std::optional<double> requested_amps_;
            std::optional<SteadyPointMillis> requested_amps_ts_;

            // Informational
            std::optional<double> last_reported_amps_;
            std::optional<double> last_reported_watts_;
        };
    }

    class LocalLimitModule {
    public:
        explicit LocalLimitModule(
                std::shared_ptr<chargelab::SystemInterface> system,
                double max_amps
        );

    public:
        int getUniqueId(std::string const& charge_point_id);
        void setRequestedAmps(int id, double amps);
        void setCurrentAllocatedAmps(int id, double amps);
        void setCurrentWatts(int id, double watts);
        std::optional<LeaseAmps> leaseAmps(int id, std::optional<LeaseAmps> const& current);
        void activateLease(int id, LeaseAmps& current);
        void printCurrentAmps();

    private:
        LeaseAmps leaseAmpsImpl(int id);
        std::optional<LeaseAmps> removeDeadEntriesAndGetMaxLease(std::vector<LeaseAmps>& leases);

    private:
        std::shared_ptr<chargelab::SystemInterface> system_;
        double max_amps_;
        int last_id_ = 0;
        int lease_index_ = 0;

        std::mutex mutex_;
        std::map<int, detail::LocalLimitState> state_;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_LOCAL_LIMITS_MODULE_H
