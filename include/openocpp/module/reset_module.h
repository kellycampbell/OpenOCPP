#ifndef CHARGELAB_OPEN_FIRMWARE_RESET_MODULE_H
#define CHARGELAB_OPEN_FIRMWARE_RESET_MODULE_H

#include "openocpp/module/common_templates.h"
#include "openocpp/module/connector_status_module.h"
#include "openocpp/interface/component/system_interface.h"
#include "openocpp/common/settings.h"

namespace chargelab {
    class ResetModule : public ServiceStatefulGeneral {
    private:
        static constexpr int kMinimumResetDelayMillis = 1*1000; // 3 seconds

    public:
        ResetModule(
                std::shared_ptr<Settings> settings,
                std::shared_ptr<SystemInterface> system,
                std::shared_ptr<ConnectorStatusModule> connector_status_module
        );

        ~ResetModule() override;

    public:
        [[nodiscard]] bool pendingReset() const;

        /**
         * Resets the charger when it's no longer in use with the provided reason code. This method is thread safe.
         *
         * @param reason
         */
        void resetOnIdle(ocpp2_0::BootReasonEnumType reason);

        /**
         * Resets the charger immediately with the provided reason code. This method is thread safe.
         *
         * @param reason
         */
        void resetImmediately(ocpp2_0::BootReasonEnumType reason);

    private:
        void runStep(ocpp1_6::OcppRemote&) override;
        void runStep(ocpp2_0::OcppRemote&) override;

        std::optional<ocpp2_0::ResponseToRequest<ocpp2_0::ResetResponse>>
        onResetReq(const ocpp2_0::ResetRequest &req) override;

        std::optional<ocpp1_6::ResponseToRequest<ocpp1_6::ResetRsp>>
        onResetReq(const ocpp1_6::ResetReq &req) override;

    private:
        void runStepCommon();

    private:
        std::shared_ptr<Settings> settings_;
        std::shared_ptr<SystemInterface> system_;
        std::shared_ptr<ConnectorStatusModule> connector_status_module_;

        std::optional<SteadyPointMillis> hard_reset_threshold_ = std::nullopt;
        std::optional<SteadyPointMillis> soft_reset_threshold_ = std::nullopt;
        std::atomic<ocpp2_0::BootReasonEnumType> reset_reason_ = {ocpp2_0::BootReasonEnumType::kUnknown};
        std::atomic<bool> soft_reset_requested_ = false;
        std::atomic<bool> hard_reset_requested_ = false;
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_RESET_MODULE_H
