#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace manyforge::core {
namespace {

Result<void> Rejected(const char* reason) {
  return Result<void>::Err(ErrorDTO{
      "invalid_request", "Manager heartbeat rejected", {{"reason", reason}}});
}

Result<void> ObservationUnavailable() {
  return Result<void>::Err(
      ErrorDTO{"internal_error", "Manager liveness observation unavailable",
               {{"reason", "observation_publish_failed"}}});
}

Result<void> ReconciliationSuperseded() {
  return Result<void>::Err(ErrorDTO{
      "invalid_state_transition", "Manager liveness reconciliation superseded",
      {{"reason", "manager_liveness_reconciliation_superseded"}}});
}

}  // namespace

struct ManagerLivenessReceiver::State final {
  State(ManagerLivenessReceiverConfig receiver_config,
        IManagerLivenessObservationPort& receiver_observation_port)
      : config(std::move(receiver_config)),
        observation_port(&receiver_observation_port),
        status{config.cell_id,
               config.receiver_runtime_unit_id,
               config.receiver_runtime_epoch,
               config.manager_runtime_unit_id,
               config.policy.runtime_policy_id,
               config.policy.runtime_policy_revision,
               0U,
               1U,
               ManagerLivenessStateDTO::kUnreconciled,
               true,
               std::nullopt} {}

  ManagerLivenessReceiverConfig config;
  IManagerLivenessObservationPort* observation_port{};
  mutable std::mutex mutex;
  ManagerLivenessStatusDTO status;
  std::uint64_t reconciliation_generation{};
  bool reconciliation_in_progress{};

  ManagerLivenessObservationDTO ObservationLocked() const {
    return {status, config.active_bundle_id, config.required_runtime_set_digest};
  }

  void PublishNoThrow(ManagerLivenessObservationDTO observation) noexcept {
    try {
      static_cast<void>(observation_port->Publish(observation));
    } catch (...) {
    }
  }
};

ManagerLivenessReceiver::ManagerLivenessReceiver(
    ManagerLivenessReceiverConfig config,
    IManagerLivenessObservationPort& observation_port)
    : state_(nullptr) {
  if (!ValidateManagerLivenessPolicy(config.policy).ok() || config.cell_id.empty() ||
      config.receiver_runtime_unit_id.empty() ||
      config.receiver_runtime_epoch.empty() ||
      config.manager_runtime_unit_id.empty() ||
      config.active_bundle_id.empty() ||
      !content_identity::is_sha256_digest(
          config.required_runtime_set_digest.value())) {
    throw std::invalid_argument("Manager-liveness receiver configuration is invalid");
  }
  state_ = std::make_shared<State>(std::move(config), observation_port);
  state_->PublishNoThrow(state_->ObservationLocked());
}

ManagerLivenessReceiver::~ManagerLivenessReceiver() = default;

Result<void> ManagerLivenessReceiver::Renew(const ManagerHeartbeatDTO& heartbeat) {
  if (!ValidateManagerHeartbeat(heartbeat).ok()) {
    return Rejected("heartbeat_invalid");
  }
  std::optional<ManagerLivenessObservationDTO> observation;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->status.receiver_status_sequence >=
        kMaximumManagerLivenessCounter - 1U) {
      return Rejected("status_sequence_exhausted");
    }
    state_->status.last_accepted_heartbeat_sequence =
        heartbeat.heartbeat_sequence;
    state_->status.state = ManagerLivenessStateDTO::kCurrent;
    state_->status.fault_or_effect_reference.reset();
    ++state_->status.receiver_status_sequence;
    observation.emplace(state_->ObservationLocked());
  }
  return state_->observation_port->Publish(*observation);
}

Result<void> ManagerLivenessReceiver::RebindRequiredSet(
    RetainedBundleId active_bundle_id,
    ContentDigest required_runtime_set_digest) {
  if (active_bundle_id.empty() ||
      !content_identity::is_sha256_digest(required_runtime_set_digest.value())) {
    return Rejected("required_set_rebind_identity_invalid");
  }
  std::optional<ManagerLivenessObservationDTO> observation;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->status.receiver_status_sequence >=
        kMaximumManagerLivenessCounter) {
      return Rejected("status_sequence_exhausted");
    }
    state_->config.active_bundle_id = std::move(active_bundle_id);
    state_->config.required_runtime_set_digest =
        std::move(required_runtime_set_digest);
    state_->status.last_accepted_heartbeat_sequence = 0U;
    ++state_->status.receiver_status_sequence;
    state_->status.state = ManagerLivenessStateDTO::kUnreconciled;
    state_->status.command_fenced = true;
    state_->status.fault_or_effect_reference.reset();
    ++state_->reconciliation_generation;
    state_->reconciliation_in_progress = false;
    observation.emplace(state_->ObservationLocked());
  }
  return state_->observation_port->Publish(*observation);
}

Result<void> ManagerLivenessReceiver::CompleteReconciliation(
    const ManagerLivenessReconciliationAck& acknowledgement) {
  if (!content_identity::is_sha256_digest(
          acknowledgement.reconciliation_evidence_digest.value())) {
    return Rejected("reconciliation_evidence_invalid");
  }
  std::optional<ManagerLivenessObservationDTO> observation;
  std::uint64_t reconciliation_generation{};
  std::uint64_t reserved_status_sequence{};
  {
    std::lock_guard lock(state_->mutex);
    if (state_->status.state != ManagerLivenessStateDTO::kCurrent ||
        !state_->status.command_fenced ||
        state_->reconciliation_in_progress) {
      return Rejected("reconciliation_identity_or_state_mismatch");
    }
    if (state_->status.receiver_status_sequence >=
        kMaximumManagerLivenessCounter - 1U) {
      return Rejected("status_sequence_exhausted");
    }
    ++state_->status.receiver_status_sequence;
    reserved_status_sequence = state_->status.receiver_status_sequence;
    reconciliation_generation = ++state_->reconciliation_generation;
    state_->reconciliation_in_progress = true;
    state_->status.command_fenced = false;
    state_->status.fault_or_effect_reference.reset();
    observation.emplace(state_->ObservationLocked());
  }

  Result<void> published = ObservationUnavailable();
  try {
    published = state_->observation_port->Publish(*observation);
  } catch (...) {
  }

  bool committed = false;
  std::optional<ManagerLivenessObservationDTO> refenced;
  {
    std::lock_guard lock(state_->mutex);
    if (state_->reconciliation_generation == reconciliation_generation) {
      state_->reconciliation_in_progress = false;
      if (published.ok() &&
          state_->status.state == ManagerLivenessStateDTO::kCurrent &&
          !state_->status.command_fenced) {
        committed = true;
      } else if (!published.ok() &&
                 state_->status.receiver_status_sequence ==
                     reserved_status_sequence &&
                 !state_->status.command_fenced &&
                 state_->status.receiver_status_sequence <
                     kMaximumManagerLivenessCounter) {
        ++state_->status.receiver_status_sequence;
        state_->status.command_fenced = true;
        refenced.emplace(state_->ObservationLocked());
      }
    }
  }
  if (refenced.has_value()) {
    state_->PublishNoThrow(std::move(*refenced));
  }
  if (!published.ok()) {
    return published;
  }
  return committed ? Result<void>::Ok() : ReconciliationSuperseded();
}

ManagerLivenessStatusDTO ManagerLivenessReceiver::status() const {
  return observation().status;
}

ManagerLivenessObservationDTO ManagerLivenessReceiver::observation() const {
  std::lock_guard lock(state_->mutex);
  return state_->ObservationLocked();
}

}  // namespace manyforge::core
