#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>

namespace manyforge::core {

Result<ContentDigest> ComputeIoSignalDescriptorDigest(const IoSignalDescriptorDTO& descriptor) {
  using content_identity::CanonicalValue;
  const auto invalid = [] {
    return Result<ContentDigest>::Err({"invalid_request",
                                       "I/O signal descriptor is invalid",
                                       {{"reason", "io_signal_descriptor_invalid"}}});
  };
  const auto text_valid = [](const std::string& value) {
    return !value.empty() && value.size() <= 512U && value.find('\0') == std::string::npos;
  };
  if (!text_valid(descriptor.signal_id.controllable_component_id.value()) ||
      !text_valid(descriptor.signal_id.name) || !text_valid(descriptor.source_id.value()) ||
      descriptor.value_type != IoSignalValueType::kBoolean)
    return invalid();
  const auto direction = descriptor.direction == IoSignalDirection::kInput    ? "input"
                         : descriptor.direction == IoSignalDirection::kOutput ? "output"
                                                                              : "";
  const auto update = descriptor.update_mode == IoSignalUpdateMode::kSampled      ? "sampled"
                      : descriptor.update_mode == IoSignalUpdateMode::kChangeOnly ? "change_only"
                                                                                  : "";
  const auto transition =
      descriptor.transition_capability == IoTransitionCapability::kSampledOnly ? "sampled_only"
      : descriptor.transition_capability == IoTransitionCapability::kCaptured  ? "captured"
                                                                               : "";
  const auto freshness = descriptor.freshness.mode == IoFreshnessMode::kSampleAge ? "sample_age"
                         : descriptor.freshness.mode == IoFreshnessMode::kProviderValidity
                             ? "provider_validity"
                             : "";
  if (*direction == '\0' || *update == '\0' || *transition == '\0' || *freshness == '\0' ||
      (descriptor.freshness.mode == IoFreshnessMode::kSampleAge &&
       (!descriptor.freshness.maximum_age_ns || *descriptor.freshness.maximum_age_ns <= 0)) ||
      (descriptor.freshness.mode == IoFreshnessMode::kProviderValidity &&
       descriptor.freshness.maximum_age_ns))
    return invalid();
  if (descriptor.direction == IoSignalDirection::kInput &&
      (descriptor.output_resource || !descriptor.liveness))
    return invalid();
  if (descriptor.direction == IoSignalDirection::kOutput &&
      (!descriptor.output_resource || !text_valid(descriptor.output_resource->value()) ||
       descriptor.liveness))
    return invalid();
  const char* liveness = nullptr;
  if (descriptor.liveness) {
    switch (*descriptor.liveness) {
      case IoSignalLivenessMode::kInProcess:
        liveness = "in_process";
        break;
      case IoSignalLivenessMode::kPeriodic:
        liveness = "periodic";
        break;
      case IoSignalLivenessMode::kChangeOnly:
        liveness = "change_only";
        break;
      default:
        return invalid();
    }
    if ((*descriptor.liveness == IoSignalLivenessMode::kPeriodic) !=
        (descriptor.freshness.mode == IoFreshnessMode::kSampleAge))
      return invalid();
  }
  try {
    CanonicalValue::Object fields{
        {"schema_version", CanonicalValue("manyforge.io_signal_descriptor.v2")},
        {"controllable_component_id", CanonicalValue(descriptor.signal_id.controllable_component_id.value())},
        {"signal_name", CanonicalValue(descriptor.signal_id.name)},
        {"source_id", CanonicalValue(descriptor.source_id.value())},
        {"value_type", CanonicalValue("bool")},
        {"direction", CanonicalValue(direction)},
        {"update_mode", CanonicalValue(update)},
        {"transition_capability", CanonicalValue(transition)},
        {"freshness", CanonicalValue(CanonicalValue::Object{
                          {"mode", CanonicalValue(freshness)},
                          {"maximum_age_ns", descriptor.freshness.maximum_age_ns
                                                 ? CanonicalValue(std::to_string(
                                                       *descriptor.freshness.maximum_age_ns))
                                                 : CanonicalValue(nullptr)}})},
        {"output_resource", descriptor.output_resource
                                ? CanonicalValue(descriptor.output_resource->value())
                                : CanonicalValue(nullptr)},
        // Retain the legacy canonical slot; it is never an output policy.
        {"output_completion", CanonicalValue(nullptr)}};
    if (liveness)
      fields.emplace_back(
          "liveness", CanonicalValue(CanonicalValue::Object{{"mode", CanonicalValue(liveness)}}));
    const CanonicalValue value(std::move(fields));
    return Result<ContentDigest>::Ok(ContentDigest(content_identity::framed_sha256(
        "io-signal-descriptor", "manyforge.io_signal_descriptor.v2", value)));
  } catch (const content_identity::CanonicalValueError&) {
    return invalid();
  }
}

}  // namespace manyforge::core
