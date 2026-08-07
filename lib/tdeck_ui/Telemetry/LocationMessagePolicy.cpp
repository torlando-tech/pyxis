#include "LocationMessagePolicy.h"

namespace Telemetry {
namespace {

bool hasHumanText(const InboundLocationMessage& message) {
    return (message.title.present && message.title.size != 0U) ||
           (message.content.present && message.content.size != 0U);
}

void setChatActions(LocationMessageDecision& decision, bool enabled) {
    decision.persist = enabled;
    decision.route = enabled;
    decision.notify = enabled;
}

}  // namespace

LocationMessageDecision classifyInboundLocationMessage(
    const InboundLocationMessage& message) {
    LocationMessageDecision decision;
    decision.authenticated_sender = message.authenticated_sender;
    decision.received_at_millis = message.received_at_millis;

    if (!message.telemetry.present) {
        return decision;
    }

    const bool human_text = hasHumanText(message);
    BinaryView telemetry_payload;
    const FieldValueResult field_result = unwrapLxmfBinaryFieldValue(
        message.telemetry.raw_value.data,
        message.telemetry.raw_value.size,
        telemetry_payload);
    if (field_result != FieldValueResult::OK ||
        decodeLocationTelemetry(telemetry_payload.data,
                                telemetry_payload.size,
                                decision.location) != DecodeResult::OK) {
        decision.kind = LocationMessageKind::MALFORMED_LOCATION;
        decision.log_malformed = true;
        setChatActions(decision, human_text);
        decision.drop = !human_text;
        return decision;
    }

    BinaryView custom_meta_payload;
    if (message.custom_meta.present &&
        (unwrapLxmfBinaryFieldValue(message.custom_meta.raw_value.data,
                                    message.custom_meta.raw_value.size,
                                    custom_meta_payload) != FieldValueResult::OK ||
         decodeCustomLocationMeta(custom_meta_payload.data,
                                  custom_meta_payload.size,
                                  decision.meta) != CustomMetaResult::OK)) {
        // Metadata controls cease, expiry, and source ordering. Applying the
        // telemetry while ignoring malformed metadata would violate all three.
        decision.kind = LocationMessageKind::MALFORMED_LOCATION;
        decision.log_malformed = true;
        setChatActions(decision, human_text);
        decision.drop = !human_text;
        return decision;
    }

    if (!isValidPeerLocationInput(decision.location,
                                  decision.meta,
                                  message.received_at_millis)) {
        decision.kind = LocationMessageKind::MALFORMED_LOCATION;
        decision.log_malformed = true;
        setChatActions(decision, human_text);
        decision.drop = !human_text;
        return decision;
    }

    decision.kind = decision.meta.has_cease && decision.meta.cease
                        ? LocationMessageKind::VALID_CEASE
                        : LocationMessageKind::VALID_LOCATION;
    decision.apply_location = true;
    setChatActions(decision, human_text);
    return decision;
}

LocationMessageExecution executeLocationMessageDecision(
    const LocationMessageDecision& decision,
    LocationMessageEffects& effects) {
    LocationMessageExecution execution;
    if (decision.log_malformed) {
        effects.logMalformedLocation();
        execution.malformed_logged = true;
    }
    if (decision.apply_location) {
        execution.location_attempted = true;
        execution.location_result = effects.applyLocation(
            decision.authenticated_sender,
            decision.location,
            decision.meta,
            decision.received_at_millis);
    }
    if (!decision.persist) return execution;

    execution.persistence_attempted = true;
    execution.persisted = effects.persistMessage();
    if (!execution.persisted) return execution;

    if (decision.route) {
        effects.routeMessage();
        execution.routed = true;
    }
    if (decision.notify) {
        effects.notifyMessage();
        execution.notified = true;
    }
    return execution;
}

}  // namespace Telemetry
