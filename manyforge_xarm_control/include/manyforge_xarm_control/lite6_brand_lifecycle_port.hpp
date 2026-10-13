// Copyright 2026 Flexin Group SRL
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

#include <manyforge_core/ports/ibrand_lifecycle_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_xarm_control/xarm_ros_transport_session.hpp>

namespace manyforge::xarm_control {

/// Diagnostic vendor-code facts copied verbatim from the latest fresh vendor
/// report for incident records (addendum 2 `vendor_codes`). They are never
/// catalog predicate inputs. Rendering: a token is the code itself (`C2`), an
/// integer is `<last id segment>=<value>` (`mode=0`, `state=4`). Facts whose
/// value is not established by the report are omitted.
inline constexpr std::string_view kVendorCodeErrorFactId =
    "manyforge.vendor_code.error";
inline constexpr std::string_view kVendorCodeModeFactId =
    "manyforge.vendor_code.mode";
inline constexpr std::string_view kVendorCodePowerCycleFactId =
    "manyforge.vendor_code.power_cycle";
inline constexpr std::string_view kVendorCodeStateFactId =
    "manyforge.vendor_code.state";
inline constexpr std::string_view kVendorCodeWarningFactId =
    "manyforge.vendor_code.warning";
/// Token value of `kVendorCodePowerCycleFactId` for servo faults C10-C17.
inline constexpr std::string_view kPowerCycleRequiredToken =
    "power_cycle_required";

/// Exact immutable provider-occurrence identity selected by the active
/// deployment/runtime join. One adapter instance serves only this occurrence.
struct Lite6BrandLifecyclePortConfig {
  core::ControllableComponentId controllable_component_id;
  core::ControlDomainId control_domain_id;
  core::ControlDomainRevision control_domain_revision;
  core::ProviderInstanceId provider_instance_id;
  core::ProviderManifestRevision provider_manifest_revision;
  core::RuntimeUnitId provider_runtime_unit_id;
  core::RuntimeEpoch provider_runtime_epoch;
  /// Exact xarm_ros2 supervised source session selected for this runtime
  /// occurrence. This is transport fencing evidence, not the deployment
  /// RuntimeEpoch and must be joined by composition rather than equated to it.
  std::string transport_owner_component_epoch;
};

/// Bounded transport seam used by the thin brand adapter. The production
/// implementation below owns the existing restricted xarm_ros2 session;
/// focused tests provide a deterministic implementation of this same seam.
class ILite6BrandLifecycleTransport {
public:
  virtual ~ILite6BrandLifecycleTransport() = default;

  // Bounded copy of the supervised observation cache; no service wait or
  // controller effect. Production reads may be serialized by the adapter.
  [[nodiscard]] virtual XArmObservedState
  observe(std::int64_t now_ns) const = 0;
  [[nodiscard]] virtual XArmLifecycleCommandResult
  execute(XArmLifecycleCommand command, std::string_view request_id,
          std::uint64_t expected_generation, std::int64_t remaining_duration_ns,
          std::stop_token cancellation) = 0;
};

/// Production bounded transport over the retained restricted ROS session.
/// It refuses before the ROS call when the CCLC remainder is shorter than the
/// session's exact configured one-call bound.
class Lite6RestrictedRosLifecycleTransport final
    : public ILite6BrandLifecycleTransport {
public:
  explicit Lite6RestrictedRosLifecycleTransport(
      XArmRosTransportSessionConfig config);

  [[nodiscard]] XArmObservedState observe(std::int64_t now_ns) const override;
  [[nodiscard]] XArmLifecycleCommandResult
  execute(XArmLifecycleCommand command, std::string_view request_id,
          std::uint64_t expected_generation, std::int64_t remaining_duration_ns,
          std::stop_token cancellation) override;

private:
  std::int64_t execute_timeout_ns_;
  XArmRosTransportSession session_;
};

/// Thin ROS-independent CCLC primitive port over the restricted xarm_ros2
/// transport. CCLC retains all transaction sequencing, retry, deadline,
/// reservation, and terminal-result ownership.
class Lite6BrandLifecyclePort final : public core::IBrandLifecyclePort {
public:
  Lite6BrandLifecyclePort(
      Lite6BrandLifecyclePortConfig config, core::IClock &clock,
      std::unique_ptr<ILite6BrandLifecycleTransport> transport);
  Lite6BrandLifecyclePort(Lite6BrandLifecyclePortConfig config,
                          core::IClock &clock,
                          XArmRosTransportSessionConfig transport_config);
  ~Lite6BrandLifecyclePort() override;

  Lite6BrandLifecyclePort(const Lite6BrandLifecyclePort &) = delete;
  Lite6BrandLifecyclePort &operator=(const Lite6BrandLifecyclePort &) = delete;

  [[nodiscard]] core::Result<core::BrandLifecycleObservationDTO>
  Observe(const core::BrandLifecycleObservationQueryDTO &query) override;

  /// Reads the existing transport cache and classifies a copy of vendor history.
  [[nodiscard]] core::Result<core::LifecycleDomainHealthDTO>
  ReadLifecycleHealth(const core::ControlDomainId &domain,
                      core::TimePoint now_ns) const;

  [[nodiscard]] core::BrandLifecyclePrimitiveResultDTO
  Dispatch(const core::BrandLifecyclePrimitiveIntentDTO &intent,
           std::stop_token cancellation) override;

private:
  [[nodiscard]] bool matches_occurrence(
      const core::BrandLifecycleObservationQueryDTO &query) const noexcept;
  [[nodiscard]] bool matches_occurrence(
      const core::BrandLifecyclePrimitiveIntentDTO &intent) const noexcept;
  /// One vendor mode/state pair; -1 is unknown.
  struct VendorControlPair final {
    int mode = -1;
    int state = -1;
  };

  /// The own primitive whose expected vendor effect explains a mode/state
  /// change while it is in flight or for a bounded settle window after it
  /// returned, until a report shows its final expected pair.
  struct OwnPrimitiveExpectation final {
    XArmLifecycleCommand command;
    VendorControlPair base;
    std::optional<std::int64_t> completed_at_ns;
  };

  struct VendorTrackingState final {
    // Semantic generation and owner publication sequence are independently
    // nondecreasing within the fenced transport occurrence. Repeated samples
    // and heartbeat publications with unchanged semantic generation are valid.
    std::uint64_t last_observation_generation_ = 0U;
    std::uint64_t last_owner_publication_sequence_ = 0U;
    core::LifecycleBlockingCauseClassDTO last_intervention_cause_{
        core::LifecycleBlockingCauseClassDTO::kNone};
    // Unrequested mode/state change detection (guarded by cause_mutex_).
    std::optional<VendorControlPair> explained_pair_;
    std::optional<VendorControlPair> last_reported_pair_;
    std::optional<OwnPrimitiveExpectation> own_expectation_;
    bool external_control_detected_ = false;
    // Vendor error code of the latest fresh report; -1 when unknown.
    int last_reported_error_code_ = -1;
  };

  struct ClassifiedHealth final {
    core::LifecycleDomainHealthDTO health;
    VendorTrackingState tracking;
  };

  static void TrackVendorControl(const XArmObservedState &observed,
                                 std::int64_t now_ns,
                                 VendorTrackingState &tracking);
  [[nodiscard]] static core::Result<ClassifiedHealth> ComputeHealth(
      const XArmObservedState &observed, std::int64_t now_ns,
      const VendorTrackingState &tracking,
      core::LifecycleDomainHealthDTO health);
  [[nodiscard]] core::Result<XArmObservedState>
  ReadCachedObservation(std::int64_t now_ns) const;

  Lite6BrandLifecyclePortConfig config_;
  core::IClock &clock_;
  std::unique_ptr<ILite6BrandLifecycleTransport> transport_;
  // Serializes cache capture with classification; Dispatch never takes it.
  mutable std::mutex observation_mutex_;
  mutable std::mutex cause_mutex_;
  VendorTrackingState tracking_;
};

} // namespace manyforge::xarm_control
