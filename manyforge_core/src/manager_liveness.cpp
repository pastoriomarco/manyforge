#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/manager_liveness.hpp>
#include <string>

namespace manyforge::core {
namespace {

template <typename T>
Result<T> Invalid(const char* reason) {
  return Result<T>::Err(
      ErrorDTO{"invalid_request", "Manager-liveness value is invalid", {{"reason", reason}}});
}

bool Identity(const auto& value) {
  return !value.empty() && value.value().size() <= 128U;
}

void Append(std::string& out, std::string_view value) {
  out += std::to_string(value.size());
  out.push_back(':');
  out.append(value);
  out.push_back('|');
}

}  // namespace

std::string_view to_string(ManagerLivenessStateDTO value) noexcept {
  switch (value) {
    case ManagerLivenessStateDTO::kCurrent: return "current";
    case ManagerLivenessStateDTO::kUnreconciled: return "unreconciled";
  }
  return "unreconciled";
}

Result<void> ValidateManagerLivenessPolicy(const ManagerLivenessPolicyDTO& policy) {
  if (!Identity(policy.runtime_policy_id) || !Identity(policy.runtime_policy_revision) ||
      policy.heartbeat_period_ns <= 0) {
    return Invalid<void>("policy_fields");
  }
  return Result<void>::Ok();
}

Result<void> ValidateManagerHeartbeat(const ManagerHeartbeatDTO& heartbeat) {
  if (!Identity(heartbeat.cell_id) || !Identity(heartbeat.manager_runtime_unit_id) ||
      heartbeat.heartbeat_sequence == 0U ||
      heartbeat.heartbeat_sequence > kMaximumManagerLivenessCounter ||
      !Identity(heartbeat.runtime_policy_id) ||
      !Identity(heartbeat.runtime_policy_revision)) {
    return Invalid<void>("heartbeat_fields");
  }
  return Result<void>::Ok();
}

Result<void> ValidateManagerLivenessStatus(const ManagerLivenessStatusDTO& status) {
  if (!Identity(status.cell_id) || !Identity(status.receiver_runtime_unit_id) ||
      !Identity(status.receiver_runtime_epoch) ||
      !Identity(status.manager_runtime_unit_id) ||
      !Identity(status.runtime_policy_id) ||
      !Identity(status.runtime_policy_revision) ||
      status.last_accepted_heartbeat_sequence > kMaximumManagerLivenessCounter ||
      status.receiver_status_sequence == 0U ||
      status.receiver_status_sequence > kMaximumManagerLivenessCounter ||
      (status.state == ManagerLivenessStateDTO::kCurrent &&
       status.last_accepted_heartbeat_sequence == 0U) ||
      (status.fault_or_effect_reference.has_value() &&
       !content_identity::is_sha256_digest(
           status.fault_or_effect_reference->value())) ||
      (status.state != ManagerLivenessStateDTO::kCurrent &&
       !status.command_fenced)) {
    return Invalid<void>("status_fields");
  }
  return Result<void>::Ok();
}

Result<void> ValidateManagerLivenessObservation(
    const ManagerLivenessObservationDTO& observation) {
  if (!ValidateManagerLivenessStatus(observation.status).ok() ||
      !Identity(observation.active_bundle_id) ||
      !content_identity::is_sha256_digest(
          observation.required_runtime_set_digest.value())) {
    return Invalid<void>("observation_fields");
  }
  return Result<void>::Ok();
}

ContentDigest ComputeRequiredRuntimeSetDigest(
    const std::vector<RequiredManagerLivenessReceiverDTO>& receivers) {
  std::string material("manyforge.manager-liveness.required-runtime-set.v1|");
  for (const auto& receiver : receivers) {
    Append(material, receiver.runtime_unit_id.value());
    Append(material, receiver.runtime_epoch.value());
    Append(material, receiver.runtime_policy_id.value());
    Append(material, receiver.runtime_policy_revision.value());
  }
  return ContentDigest(content_identity::sha256_digest(material));
}

Result<void> ValidateManagerLivenessRequiredSet(
    const ManagerLivenessRequiredSetDTO& required_set) {
  const bool canonical =
      required_set.required_receivers.size() <= kMaximumManagerLivenessReceivers &&
      std::is_sorted(required_set.required_receivers.begin(),
                     required_set.required_receivers.end(),
                     [](const auto& left, const auto& right) {
                       return left.runtime_unit_id < right.runtime_unit_id;
                     }) &&
      std::adjacent_find(required_set.required_receivers.begin(),
                         required_set.required_receivers.end(),
                         [](const auto& left, const auto& right) {
                           return left.runtime_unit_id == right.runtime_unit_id;
                         }) == required_set.required_receivers.end();
  if (!Identity(required_set.cell_id) || !Identity(required_set.active_bundle_id) ||
      !Identity(required_set.manager_runtime_unit_id) || !canonical ||
      std::any_of(required_set.required_receivers.begin(),
                  required_set.required_receivers.end(), [](const auto& receiver) {
                    return !Identity(receiver.runtime_unit_id) ||
                           !Identity(receiver.runtime_epoch) ||
                           !Identity(receiver.runtime_policy_id) ||
                           !Identity(receiver.runtime_policy_revision);
                  })) {
    return Invalid<void>("required_set_fields");
  }
  return Result<void>::Ok();
}

}  // namespace manyforge::core
