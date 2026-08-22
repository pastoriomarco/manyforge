#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <limits>
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

Result<void> WatchdogUnavailable() {
  return Result<void>::Err(
      ErrorDTO{"internal_error", "Manager heartbeat watchdog unavailable",
               {{"reason", "watchdog_schedule_failed"}}});
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

ContentDigest CallbackFailureDigest() {
  return ContentDigest(content_identity::sha256_digest(
      "manyforge.manager-liveness.local-safe-callback-failed.v1"));
}

}  // namespace

struct ManagerLivenessReceiver::State final {
  State(ManagerLivenessReceiverConfig receiver_config, IClock& receiver_clock,
        IScheduler& receiver_scheduler,
        ManagerLossLocalSafeCallback receiver_local_safe_callback,
        IManagerLivenessFailSafePort& receiver_fail_safe_port,
        IManagerLivenessObservationPort& receiver_observation_port)
      : config(std::move(receiver_config)),
        clock(&receiver_clock),
        scheduler(&receiver_scheduler),
        local_safe_callback(std::move(receiver_local_safe_callback)),
        fail_safe_port(&receiver_fail_safe_port),
        observation_port(&receiver_observation_port),
        status{config.cell_id,
               config.receiver_runtime_unit_id,
               config.receiver_runtime_epoch,
               config.manager_runtime_unit_id,
               config.manager_runtime_epoch,
               config.manager_owner_epoch,
               config.policy.runtime_policy_id,
               config.policy.runtime_policy_revision,
               0U,
               1U,
               0,
               ManagerLivenessStateDTO::kUnreconciled,
               true,
               std::nullopt} {}

  ManagerLivenessReceiverConfig config;
  IClock* clock{};
  IScheduler* scheduler{};
  ManagerLossLocalSafeCallback local_safe_callback;
  IManagerLivenessFailSafePort* fail_safe_port{};
  IManagerLivenessObservationPort* observation_port{};
  mutable std::mutex mutex;
  ManagerLivenessStatusDTO status;
  std::uint64_t schedule_generation{};
  ScheduleHandle schedule_handle{};
  TimePoint last_receipt_time{};
  bool has_receipt{};
  bool destroyed{};
  std::uint64_t reconciliation_generation{};
  bool reconciliation_in_progress{};

  ManagerLivenessObservationDTO ObservationLocked() const {
    auto projected = status;
    if (has_receipt) {
      const auto now = clock->now();
      projected.local_receipt_age_ns =
          now >= last_receipt_time ? now - last_receipt_time : 0;
    }
    return {projected, config.active_bundle_id, config.required_runtime_set_digest,
            ComputeManagerLivenessStatusDigest(projected)};
  }

  void PublishNoThrow(ManagerLivenessObservationDTO observation) noexcept {
    try {
      static_cast<void>(observation_port->Publish(observation));
    } catch (...) {
    }
  }

  ManagerLivenessLossIntentDTO LossIntentLocked(
      ManagerLivenessLossKindDTO kind, const RuntimeUnitId& lost_manager_unit,
      const RuntimeEpoch& lost_manager_epoch,
      const RequestOwnerEpoch& lost_manager_owner_epoch) const {
    return {config.cell_id,
            config.active_bundle_id,
            config.required_runtime_set_digest,
            config.receiver_runtime_unit_id,
            config.receiver_runtime_epoch,
            config.policy.runtime_policy_id,
            config.policy.runtime_policy_revision,
            lost_manager_unit,
            lost_manager_epoch,
            lost_manager_owner_epoch,
            status.receiver_status_sequence,
            kind};
  }

  void SafeAndContain(const ManagerLivenessLossIntentDTO& intent) noexcept {
    std::optional<ContentDigest> evidence;
    try {
      auto candidate = local_safe_callback(intent);
      if (content_identity::is_sha256_digest(candidate.value())) {
        evidence = std::move(candidate);
      }
    } catch (...) {
    }
    if (!evidence.has_value()) {
      evidence = CallbackFailureDigest();
    }
    try {
      fail_safe_port->LatchAndContain(intent);
    } catch (...) {
      // The port is noexcept by contract. Keep the watchdog lane fail closed
      // even for a defective adapter.
    }

    std::optional<ManagerLivenessObservationDTO> observation;
    {
      std::lock_guard lock(mutex);
      if (destroyed ||
          status.receiver_status_sequence != intent.receiver_status_sequence ||
          !status.command_fenced) {
        return;
      }
      status.fault_or_effect_reference = std::move(evidence);
      observation.emplace(ObservationLocked());
    }
    PublishNoThrow(std::move(*observation));
  }

  static void ExpireImpl(const std::weak_ptr<State>& weak,
                         std::uint64_t generation) {
    const auto self = weak.lock();
    if (!self) {
      return;
    }
    std::optional<ManagerLivenessLossIntentDTO> loss;
    {
      std::lock_guard lock(self->mutex);
      if (self->destroyed || generation != self->schedule_generation ||
          self->status.state == ManagerLivenessStateDTO::kExpired ||
          !self->has_receipt ||
          self->clock->now() <
              self->last_receipt_time +
                  self->config.policy.expiry_timeout_ns) {
        return;
      }
      self->status.state = ManagerLivenessStateDTO::kExpired;
      self->status.command_fenced = true;
      ++self->status.receiver_status_sequence;
      loss.emplace(self->LossIntentLocked(
          ManagerLivenessLossKindDTO::kExpiry,
          self->config.manager_runtime_unit_id,
          self->config.manager_runtime_epoch,
          self->config.manager_owner_epoch));
    }
    self->SafeAndContain(*loss);
  }

  static void Expire(const std::weak_ptr<State>& weak,
                     std::uint64_t generation) noexcept {
    try {
      ExpireImpl(weak, generation);
    } catch (...) {
      // The state was already fenced before any effect callback. Allocation,
      // callback, or projection failures may not escape a watchdog lane.
    }
  }

  static void FailClosedForWatchdogFailure(
      const std::shared_ptr<State>& self, std::uint64_t generation) noexcept {
    std::optional<ManagerLivenessLossIntentDTO> loss;
    try {
      {
        std::lock_guard lock(self->mutex);
        if (!self->destroyed && generation == self->schedule_generation &&
            self->status.state != ManagerLivenessStateDTO::kExpired) {
          self->status.state = ManagerLivenessStateDTO::kExpired;
          self->status.command_fenced = true;
          ++self->status.receiver_status_sequence;
          loss.emplace(self->LossIntentLocked(
              ManagerLivenessLossKindDTO::kExpiry,
              self->config.manager_runtime_unit_id,
              self->config.manager_runtime_epoch,
              self->config.manager_owner_epoch));
        }
      }
      if (!loss.has_value()) {
        return;
      }
      self->SafeAndContain(*loss);
    } catch (...) {
      // The receiver was fenced before invoking the local safe action.
    }
  }
};

ManagerLivenessReceiver::ManagerLivenessReceiver(
    ManagerLivenessReceiverConfig config, IClock& clock, IScheduler& scheduler,
    ManagerLossLocalSafeCallback local_safe_callback,
    IManagerLivenessFailSafePort& fail_safe_port,
    IManagerLivenessObservationPort& observation_port)
    : state_(nullptr) {
  if (!ValidateManagerLivenessPolicy(config.policy).ok() || config.cell_id.empty() ||
      config.receiver_runtime_unit_id.empty() ||
      config.receiver_runtime_epoch.empty() ||
      config.manager_runtime_unit_id.empty() || config.manager_runtime_epoch.empty() ||
      config.manager_owner_epoch.empty() || config.active_bundle_id.empty() ||
      !content_identity::is_sha256_digest(
          config.required_runtime_set_digest.value()) ||
      !local_safe_callback) {
    throw std::invalid_argument("Manager-liveness receiver configuration is invalid");
  }
  state_ = std::make_shared<State>(
      std::move(config), clock, scheduler, std::move(local_safe_callback),
      fail_safe_port, observation_port);
  state_->PublishNoThrow(state_->ObservationLocked());
}

ManagerLivenessReceiver::ManagerLivenessReceiver(
    ManagerLivenessReceiverConfig config, IClock& clock, IScheduler& scheduler,
    LegacyManagerLossLocalSafeCallback local_safe_callback,
    IManagerLivenessFailSafePort& fail_safe_port,
    IManagerLivenessObservationPort& observation_port)
    : ManagerLivenessReceiver(
          std::move(config), clock, scheduler,
          [callback = std::move(local_safe_callback)](
              const ManagerLivenessLossIntentDTO&) { return callback(); },
          fail_safe_port, observation_port) {}

ManagerLivenessReceiver::~ManagerLivenessReceiver() {
  ScheduleHandle handle{};
  {
    std::lock_guard lock(state_->mutex);
    state_->destroyed = true;
    ++state_->schedule_generation;
    handle = state_->schedule_handle;
  }
  if (handle != 0U) {
    try {
      state_->scheduler->cancel(handle);
    } catch (...) {
    }
  }
  state_.reset();
}

Result<void> ManagerLivenessReceiver::Renew(const ManagerHeartbeatDTO& heartbeat) {
  if (!ValidateManagerHeartbeat(heartbeat).ok()) {
    return Rejected("heartbeat_invalid");
  }
  std::optional<ManagerLivenessObservationDTO> observation;
  ScheduleHandle old_handle{};
  std::uint64_t generation{};
  TimePoint deadline{};
  {
    std::lock_guard lock(state_->mutex);
    const auto& config = state_->config;
    if (state_->destroyed || heartbeat.cell_id != config.cell_id ||
        heartbeat.manager_runtime_unit_id != config.manager_runtime_unit_id ||
        heartbeat.manager_runtime_epoch != config.manager_runtime_epoch ||
        heartbeat.manager_owner_epoch != config.manager_owner_epoch ||
        heartbeat.runtime_policy_id != config.policy.runtime_policy_id ||
        heartbeat.runtime_policy_revision != config.policy.runtime_policy_revision) {
      return Rejected("heartbeat_identity_mismatch");
    }
    if (state_->status.state == ManagerLivenessStateDTO::kExpired) {
      return Rejected("manager_epoch_expired");
    }
    if (heartbeat.heartbeat_sequence <=
        state_->status.last_accepted_heartbeat_sequence) {
      return Rejected("heartbeat_sequence_not_strict");
    }
    if (state_->status.receiver_status_sequence >=
        kMaximumManagerLivenessCounter - 1U) {
      return Rejected("status_sequence_exhausted");
    }
    old_handle = state_->schedule_handle;
    const auto receipt_time = state_->clock->now();
    if (receipt_time < 0 ||
        receipt_time >
            std::numeric_limits<TimePoint>::max() -
                config.policy.expiry_timeout_ns) {
      return Rejected("receiver_clock_out_of_range");
    }
    state_->last_receipt_time = receipt_time;
    state_->has_receipt = true;
    state_->status.last_accepted_heartbeat_sequence =
        heartbeat.heartbeat_sequence;
    state_->status.state = ManagerLivenessStateDTO::kCurrent;
    state_->status.fault_or_effect_reference.reset();
    ++state_->status.receiver_status_sequence;
    generation = ++state_->schedule_generation;
    deadline = state_->last_receipt_time + config.policy.expiry_timeout_ns;
    observation.emplace(state_->ObservationLocked());
  }
  if (old_handle != 0U) {
    try {
      state_->scheduler->cancel(old_handle);
    } catch (...) {
      State::FailClosedForWatchdogFailure(state_, generation);
      return WatchdogUnavailable();
    }
  }
  const std::weak_ptr<State> weak(state_);
  ScheduleHandle handle{};
  try {
    handle = state_->scheduler->schedule(
        deadline, [weak, generation] { State::Expire(weak, generation); },
        SchedulerPriority::RuntimeSafety);
  } catch (...) {
    State::FailClosedForWatchdogFailure(state_, generation);
    return WatchdogUnavailable();
  }
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->destroyed && state_->schedule_generation == generation) {
      state_->schedule_handle = handle;
    } else {
      try {
        state_->scheduler->cancel(handle);
      } catch (...) {
        return WatchdogUnavailable();
      }
    }
  }
  return state_->observation_port->Publish(*observation);
}

Result<void> ManagerLivenessReceiver::BeginManagerEpochReplacement(
    RuntimeUnitId manager_runtime_unit_id, RuntimeEpoch manager_runtime_epoch,
    RequestOwnerEpoch manager_owner_epoch) {
  if (manager_runtime_unit_id.empty() || manager_runtime_epoch.empty() ||
      manager_owner_epoch.empty()) {
    return Rejected("replacement_identity_invalid");
  }
  std::optional<ManagerLivenessLossIntentDTO> loss;
  ScheduleHandle old_handle{};
  {
    std::lock_guard lock(state_->mutex);
    if (manager_runtime_epoch == state_->config.manager_runtime_epoch ||
        manager_owner_epoch == state_->config.manager_owner_epoch) {
      return Rejected("replacement_epoch_not_new");
    }
    if (state_->status.receiver_status_sequence >=
        kMaximumManagerLivenessCounter) {
      return Rejected("status_sequence_exhausted");
    }
    old_handle = state_->schedule_handle;
    const auto lost_manager_runtime_unit_id =
        state_->config.manager_runtime_unit_id;
    const auto lost_manager_runtime_epoch =
        state_->config.manager_runtime_epoch;
    const auto lost_manager_owner_epoch = state_->config.manager_owner_epoch;
    ++state_->schedule_generation;
    state_->schedule_handle = 0U;
    state_->has_receipt = false;
    state_->config.manager_runtime_unit_id = std::move(manager_runtime_unit_id);
    state_->config.manager_runtime_epoch = std::move(manager_runtime_epoch);
    state_->config.manager_owner_epoch = std::move(manager_owner_epoch);
    state_->status.manager_runtime_unit_id =
        state_->config.manager_runtime_unit_id;
    state_->status.manager_runtime_epoch = state_->config.manager_runtime_epoch;
    state_->status.manager_owner_epoch = state_->config.manager_owner_epoch;
    state_->status.last_accepted_heartbeat_sequence = 0U;
    ++state_->status.receiver_status_sequence;
    state_->status.local_receipt_age_ns = 0;
    state_->status.state = ManagerLivenessStateDTO::kUnreconciled;
    state_->status.command_fenced = true;
    state_->status.fault_or_effect_reference.reset();
    loss.emplace(state_->LossIntentLocked(
        ManagerLivenessLossKindDTO::kManagerEpochReplacement,
        lost_manager_runtime_unit_id, lost_manager_runtime_epoch,
        lost_manager_owner_epoch));
    ++state_->reconciliation_generation;
    state_->reconciliation_in_progress = false;
  }
  state_->SafeAndContain(*loss);
  if (old_handle != 0U) {
    try {
      state_->scheduler->cancel(old_handle);
    } catch (...) {
      return WatchdogUnavailable();
    }
  }
  return Result<void>::Ok();
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
    const auto& config = state_->config;
    if (acknowledgement.cell_id != config.cell_id ||
        acknowledgement.manager_runtime_unit_id != config.manager_runtime_unit_id ||
        acknowledgement.manager_runtime_epoch != config.manager_runtime_epoch ||
        acknowledgement.manager_owner_epoch != config.manager_owner_epoch ||
        acknowledgement.active_bundle_id != config.active_bundle_id ||
        acknowledgement.required_runtime_set_digest !=
            config.required_runtime_set_digest ||
        state_->status.state != ManagerLivenessStateDTO::kCurrent ||
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
      if (published.ok() && !state_->destroyed &&
          state_->status.state == ManagerLivenessStateDTO::kCurrent &&
          !state_->status.command_fenced) {
        committed = true;
      } else if (!published.ok() && !state_->destroyed &&
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
  std::lock_guard lock(state_->mutex);
  return state_->ObservationLocked().status;
}

}  // namespace manyforge::core
