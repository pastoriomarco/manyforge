#include <manyforge_roboplan/retained_ros2_control_admission_owner.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::roboplan_backend {
namespace {

template <typename Value>
[[nodiscard]] core::Result<Value> reject(std::string reason,
                                         std::string message) {
  return core::Result<Value>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message), {{"reason", std::move(reason)}}});
}

[[nodiscard]] std::string binding_epoch(
    const ros2_control_execution::ControllerBindingSnapshot &binding) {
  std::string preimage("manyforge.ros2-control-transport-owner-epoch.v1");
  preimage.push_back('\0');
  preimage.append(binding.contract_generation_id);
  preimage.push_back('\0');
  preimage.append(std::to_string(binding.activity_generation));
  return manyforge::content_identity::sha256_digest(preimage);
}

[[nodiscard]] std::string semantic_signature(
    const bool controller_ready, const bool motion_admitted,
    const std::string &transport_epoch, const std::string &authority_epoch,
    const std::vector<std::string> &reason_codes) {
  std::ostringstream stream;
  stream << controller_ready << ':' << motion_admitted << ':'
         << transport_epoch << ':' << authority_epoch;
  for (const auto &reason : reason_codes) {
    stream << ':' << reason;
  }
  return stream.str();
}

} // namespace

bool RetainedRos2ControlAdmissionOwnerConfig::locally_valid() const noexcept {
  return !controllable_component_id.empty() && !runtime_epoch.empty() &&
         !binding_contract_generation_id.empty() &&
         !provider_component_epoch.empty() &&
         !cell_supervisor_component_epoch.empty() &&
         publication_period_ns > 0 && freshness_ns > publication_period_ns &&
         freshness_ns <= 1'000'000'000LL;
}

core::Result<std::unique_ptr<RetainedRos2ControlAdmissionOwner>>
RetainedRos2ControlAdmissionOwner::Create(
    RetainedRos2ControlAdmissionOwnerConfig config,
    std::shared_ptr<RetainedRos2ControlRuntimeHandle> runtime_handle,
    core::IClock &clock,
    RetainedCurrentComponentSnapshotSource component_snapshot_source) {
  if (!config.locally_valid() || runtime_handle == nullptr ||
      !component_snapshot_source) {
    return reject<std::unique_ptr<RetainedRos2ControlAdmissionOwner>>(
        "ros2_control_admission_owner_input_invalid",
        "ROS execution admission requires one exact lifecycle/controller "
        "join");
  }
  auto owner = std::unique_ptr<RetainedRos2ControlAdmissionOwner>(
      new RetainedRos2ControlAdmissionOwner(
          std::move(config), std::move(runtime_handle), clock,
          std::move(component_snapshot_source)));
  owner->worker_ = std::jthread(
      [self = owner.get()](const std::stop_token stop_token) {
        self->Run(stop_token);
      });
  return core::Result<std::unique_ptr<RetainedRos2ControlAdmissionOwner>>::Ok(
      std::move(owner));
}

RetainedRos2ControlAdmissionOwner::RetainedRos2ControlAdmissionOwner(
    RetainedRos2ControlAdmissionOwnerConfig config,
    std::shared_ptr<RetainedRos2ControlRuntimeHandle> runtime_handle,
    core::IClock &clock,
    RetainedCurrentComponentSnapshotSource component_snapshot_source)
    : config_(std::move(config)), runtime_handle_(std::move(runtime_handle)),
      clock_(clock),
      component_snapshot_source_(std::move(component_snapshot_source)) {}

RetainedRos2ControlAdmissionOwner::~RetainedRos2ControlAdmissionOwner() {
  worker_.request_stop();
  wakeup_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

RetainedRos2ControlAdmissionOwnerStatus
RetainedRos2ControlAdmissionOwner::status() const {
  std::lock_guard lock(status_mutex_);
  return status_;
}

void RetainedRos2ControlAdmissionOwner::Run(
    const std::stop_token stop_token) noexcept {
  std::uint64_t publication_generation = 0U;
  std::uint64_t lifecycle_generation = 0U;
  std::uint64_t consecutive_binding_lease_expirations = 0U;
  bool component_readback_gap_deferred = false;
  bool exact_component_readback_observed = false;
  std::string previous_semantics;
  // Bound the projection hold by the admission snapshot lease.
  // The backend independently checks the controller-binding lease at dispatch
  // and every active-goal tick, so this only prevents a slower join from
  // publishing a stale semantic transition just before a successful refresh.
  const auto admission_lease_periods =
      config_.freshness_ns / config_.publication_period_ns;
  const auto binding_lease_expiration_deferral_limit =
      static_cast<std::uint64_t>(
          std::max<core::Duration>(1, admission_lease_periods));
  const auto wait_for_next_observation = [this, &stop_token]() {
    std::unique_lock wait_lock(wakeup_mutex_);
    wakeup_.wait_for(wait_lock, stop_token,
                     std::chrono::nanoseconds(config_.publication_period_ns),
                     [] { return false; });
  };
  {
    std::lock_guard lock(status_mutex_);
    status_.running = true;
    status_.healthy = true;
  }
  try {
    while (!stop_token.stop_requested()) {
      std::optional<core::ComponentLifecycleCurrentSnapshotDTO> component;
      bool defer_component_readback_gap = false;
      try {
        auto readback = component_snapshot_source_();
        if (readback.ok() && readback.value().locally_valid() &&
            readback.value().controllable_component_id ==
                config_.controllable_component_id) {
          component = std::move(readback.value());
          exact_component_readback_observed = true;
          component_readback_gap_deferred = false;
        }
      } catch (...) {
        component.reset();
      }
      if (!component.has_value() && exact_component_readback_observed &&
          !component_readback_gap_deferred) {
        // A current-fact read joins multiple durable read-only stores and can
        // lose one observation to bounded reader contention. The already
        // published admission has its own short lease, so hold exactly one
        // publication period instead of inventing an inactive lifecycle
        // transition. A second consecutive gap still fails closed below.
        component_readback_gap_deferred = true;
        defer_component_readback_gap = true;
      }
      const auto binding = runtime_handle_->controller_binding_snapshot();
      // Timestamp the joined observation after both source reads. The durable
      // component readback can be slower than a controller-monitor refresh;
      // sampling time before that read would make a newly published binding
      // appear to come from the future and falsely revoke admission.
      const auto now = clock_.now();
      if (now <= 0 ||
          now > std::numeric_limits<core::TimePoint>::max() -
                    config_.freshness_ns) {
        throw std::runtime_error("ROS admission clock is outside its bound");
      }
      bool controller_ready = false;
      bool defer_binding_lease_expiration = false;
      std::vector<std::string> reasons;
      if (!binding.has_value()) {
        consecutive_binding_lease_expirations = 0U;
        reasons.emplace_back("controller_binding_unavailable");
      } else if (binding->contract_generation_id !=
                 config_.binding_contract_generation_id) {
        consecutive_binding_lease_expirations = 0U;
        reasons.emplace_back("controller_binding_contract_mismatch");
      } else if (binding->received_timestamp_ns > now) {
        consecutive_binding_lease_expirations = 0U;
        reasons.emplace_back("controller_binding_stale");
      } else if (!binding->valid) {
        consecutive_binding_lease_expirations = 0U;
        reasons.emplace_back("controller_binding_invalid");
      } else if (binding->fresh_until_ns <= now) {
        // The backend checks this same lease directly at dispatch and every
        // active-goal tick. Do not let a bounded sub-lease expiry observed only
        // by this slower lifecycle join publish a false semantic transition
        // immediately before the asynchronous refresh completes. The hold is
        // bounded by this owner's existing admission lease; its expiry and
        // the backend's direct binding check still close a persistent gap,
        // while activity/invalid binding evidence above remains immediate.
        if (consecutive_binding_lease_expirations <
            binding_lease_expiration_deferral_limit) {
          ++consecutive_binding_lease_expirations;
          defer_binding_lease_expiration = true;
        } else {
          reasons.emplace_back("controller_binding_stale");
        }
      } else {
        consecutive_binding_lease_expirations = 0U;
        controller_ready = true;
      }

      if (defer_binding_lease_expiration) {
        {
          std::lock_guard lock(status_mutex_);
          status_.healthy = true;
          ++status_.binding_lease_expirations_deferred_total;
          status_.reason_code.clear();
        }
        wait_for_next_observation();
        continue;
      }
      if (defer_component_readback_gap && controller_ready) {
        {
          std::lock_guard lock(status_mutex_);
          status_.healthy = true;
          ++status_.component_readback_gaps_deferred_total;
          status_.reason_code.clear();
        }
        wait_for_next_observation();
        continue;
      }

      bool lifecycle_active = false;
      std::string authority_epoch;
      if (!component.has_value()) {
        reasons.emplace_back("component_lifecycle_snapshot_unavailable");
      } else {
        lifecycle_active = component->component_lifecycle_state ==
                           core::ComponentLifecycleStateDTO::kActive;
        authority_epoch = component->snapshot_digest.value();
        if (!lifecycle_active) {
          reasons.emplace_back("component_lifecycle_not_active");
        }
      }
      std::sort(reasons.begin(), reasons.end());
      reasons.erase(std::unique(reasons.begin(), reasons.end()), reasons.end());

      const bool motion_admitted = controller_ready && lifecycle_active;
      const std::string transport_epoch =
          motion_admitted ? binding_epoch(*binding) : std::string{};
      if (!motion_admitted) {
        authority_epoch.clear();
      }
      const std::string semantics =
          semantic_signature(controller_ready, motion_admitted,
                             transport_epoch, authority_epoch, reasons);
      if (semantics != previous_semantics) {
        if (lifecycle_generation ==
            std::numeric_limits<std::uint64_t>::max()) {
          throw std::runtime_error(
              "ROS admission lifecycle generation is exhausted");
        }
        ++lifecycle_generation;
        previous_semantics = semantics;
      }
      if (publication_generation ==
          std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error(
            "ROS admission publication generation is exhausted");
      }
      ++publication_generation;

      ros2_control_execution::ExecutionAdmissionSnapshot snapshot;
      snapshot.generation = publication_generation;
      snapshot.lifecycle_generation = lifecycle_generation;
      snapshot.robot_id = config_.controllable_component_id.value();
      snapshot.binding_contract_generation_id =
          config_.binding_contract_generation_id;
      snapshot.runtime_generation_id = config_.runtime_epoch.value();
      snapshot.provider_component_epoch = config_.provider_component_epoch;
      snapshot.transport_owner_component_epoch = transport_epoch;
      snapshot.authority_component_epoch = authority_epoch;
      snapshot.cell_supervisor_component_epoch =
          config_.cell_supervisor_component_epoch;
      snapshot.observed_timestamp_ns = now;
      snapshot.received_timestamp_ns = now;
      snapshot.fresh_until_ns = now + config_.freshness_ns;
      snapshot.controller_ready = controller_ready;
      snapshot.motion_admitted = motion_admitted;
      snapshot.reason_codes = std::move(reasons);
      if (!runtime_handle_->publish_execution_admission_snapshot(
              std::move(snapshot))) {
        throw std::runtime_error(
            "ROS backend rejected its CCLC execution admission publication");
      }
      {
        std::lock_guard lock(status_mutex_);
        status_.healthy = true;
        status_.publication_generation = publication_generation;
        status_.lifecycle_generation = lifecycle_generation;
        status_.last_publication_ns = now;
        status_.reason_code.clear();
      }

      wait_for_next_observation();
    }
  } catch (const std::exception &error) {
    std::lock_guard lock(status_mutex_);
    status_.healthy = false;
    status_.reason_code = error.what();
  } catch (...) {
    std::lock_guard lock(status_mutex_);
    status_.healthy = false;
    status_.reason_code = "ros2_control_admission_owner_unknown_failure";
  }
  std::lock_guard lock(status_mutex_);
  status_.running = false;
}

} // namespace manyforge::roboplan_backend
