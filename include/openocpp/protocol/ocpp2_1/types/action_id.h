#ifndef CHARGELAB_OPEN_FIRMWARE_2_1_ACTION_ID_H
#define CHARGELAB_OPEN_FIRMWARE_2_1_ACTION_ID_H

#include "openocpp/helpers/json.h"

// OCPP 2.1-only action IDs. These are kept separate from the OCPP 2.0.1 action-id list
// (CHARGELAB_OCPP_2_0_ACTION_IDS) so that 2.1-specific messages do not get pulled into the
// 2.0 CALL request/response handler machinery. Messages listed here are sent via the SEND
// framing (OcppRemote::sendSend) and therefore need no Response type.
#define CHARGELAB_OCPP_2_1_ACTION_IDS             \
    NotifyPeriodicEventStream


namespace chargelab::ocpp2_1 {
    CHARGELAB_JSON_ENUM(ActionId, CHARGELAB_OCPP_2_1_ACTION_IDS)
}

#endif //CHARGELAB_OPEN_FIRMWARE_2_1_ACTION_ID_H
