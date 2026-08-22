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

void Append(std::string& out, std::uint64_t value) {
  Append(out, std::to_string(value));
}

}  // namespace

std::string_view to_string(ManagerLivenessStateDTO value) noexcept {
  switch (value) {
    case ManagerLivenessStateDTO::kCurrent: return "current";
    case ManagerLivenessStateDTO::kExpired: return "expired";
    case ManagerLivenessStateDTO::kUnreconciled: return "unreconciled";
  }
  return "unreconciled";
}

Result<void> ValidateManagerLivenessPolicy(const ManagerLivenessPolicyDTO& policy) {
  if (!Identity(policy.runtime_policy_id) || !Identity(policy.runtime_policy_revision) ||
      policy.heartbeat_period_ns <= 0 ||
      policy.expiry_timeout_ns <= policy.heartbeat_period_ns ||
      static_cast<std::uint64_t>(policy.expiry_timeout_ns) >
          kMaximumManagerLivenessCounter ||
      !Identity(policy.local_safe_state_contract_id) ||
      !Identity(policy.local_safe_state_contract_revision)) {
    return Invalid<void>("policy_fields");
  }
  return Result<void>::Ok();
}

Result<void> ValidateManagerHeartbeat(const ManagerHeartbeatDTO& heartbeat) {
  if (!Identity(heartbeat.cell_id) || !Identity(heartbeat.manager_runtime_unit_id) ||
      !Identity(heartbeat.manager_runtime_epoch) ||
      !Identity(heartbeat.manager_owner_epoch) || heartbeat.heartbeat_sequence == 0U ||
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
      !Identity(status.manager_runtime_epoch) || !Identity(status.manager_owner_epoch) ||
      !Identity(status.runtime_policy_id) ||
      !Identity(status.runtime_policy_revision) ||
      status.last_accepted_heartbeat_sequence > kMaximumManagerLivenessCounter ||
      status.receiver_status_sequence == 0U ||
      status.receiver_status_sequence > kMaximumManagerLivenessCounter ||
      status.local_receipt_age_ns < 0 ||
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
          observation.required_runtime_set_digest.value()) ||
      !content_identity::is_sha256_digest(observation.receiver_status_digest.value()) ||
      observation.receiver_status_digest !=
          ComputeManagerLivenessStatusDigest(observation.status)) {
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
      !Identity(required_set.manager_runtime_unit_id) ||
      !Identity(required_set.manager_runtime_epoch) ||
      !Identity(required_set.manager_owner_epoch) || !canonical ||
      std::any_of(required_set.required_receivers.begin(),
                  required_set.required_receivers.end(), [](const auto& receiver) {
                    return !Identity(receiver.runtime_unit_id) ||
                           !Identity(receiver.runtime_epoch) ||
                           !Identity(receiver.runtime_policy_id) ||
                           !Identity(receiver.runtime_policy_revision);
                  }) ||
      required_set.required_runtime_set_digest !=
          ComputeRequiredRuntimeSetDigest(required_set.required_receivers)) {
    return Invalid<void>("required_set_fields");
  }
  return Result<void>::Ok();
}

Result<void> ValidateManagerLivenessLossIntent(
    const ManagerLivenessLossIntentDTO& intent) {
  if (!Identity(intent.cell_id) || !Identity(intent.active_bundle_id) ||
      !content_identity::is_sha256_digest(
          intent.required_runtime_set_digest.value()) ||
      !Identity(intent.receiver_runtime_unit_id) ||
      !Identity(intent.receiver_runtime_epoch) ||
      !Identity(intent.runtime_policy_id) ||
      !Identity(intent.runtime_policy_revision) ||
      !Identity(intent.lost_manager_runtime_unit_id) ||
      !Identity(intent.lost_manager_runtime_epoch) ||
      !Identity(intent.lost_manager_owner_epoch) ||
      intent.receiver_status_sequence == 0U ||
      intent.receiver_status_sequence > kMaximumManagerLivenessCounter ||
      (intent.kind != ManagerLivenessLossKindDTO::kExpiry &&
       intent.kind !=
           ManagerLivenessLossKindDTO::kManagerEpochReplacement)) {
    return Invalid<void>("loss_intent_fields");
  }
  return Result<void>::Ok();
}

ContentDigest ComputeManagerLivenessStatusDigest(
    const ManagerLivenessStatusDTO& status) {
  std::string material("manyforge.manager-liveness.status.v1|");
  Append(material, status.cell_id.value());
  Append(material, status.receiver_runtime_unit_id.value());
  Append(material, status.receiver_runtime_epoch.value());
  Append(material, status.manager_runtime_unit_id.value());
  Append(material, status.manager_runtime_epoch.value());
  Append(material, status.manager_owner_epoch.value());
  Append(material, status.runtime_policy_id.value());
  Append(material, status.runtime_policy_revision.value());
  Append(material, status.last_accepted_heartbeat_sequence);
  Append(material, status.receiver_status_sequence);
  Append(material, static_cast<std::uint64_t>(status.local_receipt_age_ns));
  Append(material, to_string(status.state));
  Append(material, status.command_fenced ? "true" : "false");
  if (status.fault_or_effect_reference.has_value()) {
    Append(material, status.fault_or_effect_reference->value());
  } else {
    Append(material, std::string_view{});
  }
  return ContentDigest(content_identity::sha256_digest(material));
}

}  // namespace manyforge::core
