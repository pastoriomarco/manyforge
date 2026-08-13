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
#include <stop_token>
#include <string>
#include <string_view>

#include <manyforge_core/ports/ibrand_lifecycle_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_xarm_control/xarm_ros_transport_session.hpp>

namespace manyforge::xarm_control {

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

  [[nodiscard]] core::BrandLifecyclePrimitiveResultDTO
  Dispatch(const core::BrandLifecyclePrimitiveIntentDTO &intent,
           std::stop_token cancellation) override;

private:
  [[nodiscard]] bool matches_occurrence(
      const core::BrandLifecycleObservationQueryDTO &query) const noexcept;
  [[nodiscard]] bool matches_occurrence(
      const core::BrandLifecyclePrimitiveIntentDTO &intent) const noexcept;

  Lite6BrandLifecyclePortConfig config_;
  core::IClock &clock_;
  std::unique_ptr<ILite6BrandLifecycleTransport> transport_;
};

} // namespace manyforge::xarm_control
