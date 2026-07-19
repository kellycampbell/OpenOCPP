#ifndef CHARGELAB_OPEN_FIRMWARE_2_1_NOTIFY_PERIODIC_EVENT_STREAM_H
#define CHARGELAB_OPEN_FIRMWARE_2_1_NOTIFY_PERIODIC_EVENT_STREAM_H

#include "openocpp/protocol/ocpp2_1/types/action_id.h"
#include "openocpp/protocol/common/date_time.h"
#include "openocpp/helpers/json.h"

#include <optional>
#include <string>
#include <vector>

namespace chargelab::ocpp2_1 {
    using DateTime = common::DateTime;

    // One data point in a periodic event stream: a value 'v' captured at time offset 't'
    // (seconds) relative to the message's basetime.
    struct StreamDataElementType {
        double t {};
        std::string v {};
        CHARGELAB_JSON_INTRUSIVE(StreamDataElementType, t, v)
    };

    // OCPP 2.1 NotifyPeriodicEventStream. Sent as a SEND (message type 6), so it is
    // fire-and-forget and has no associated Response type.
    struct NotifyPeriodicEventStreamRequest {
        std::vector<StreamDataElementType> data {};
        int id {};
        std::optional<int> pending {};
        DateTime basetime {};
        CHARGELAB_JSON_INTRUSIVE_CALL(NotifyPeriodicEventStreamRequest, kNotifyPeriodicEventStream, data, id, pending, basetime)
    };
}

#endif //CHARGELAB_OPEN_FIRMWARE_2_1_NOTIFY_PERIODIC_EVENT_STREAM_H
