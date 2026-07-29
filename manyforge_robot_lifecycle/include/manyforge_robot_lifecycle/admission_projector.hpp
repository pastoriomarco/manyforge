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
#include <optional>
#include <string>
#include <vector>

#include <manyforge_robot_lifecycle/contract.hpp>

namespace manyforge::robot_lifecycle
{

/// Robot-family-neutral lifecycle admission prepared for an NRT transport.
///
/// Absolute receipt and expiry timestamps belong to the eventual consumer.
/// The transport carries one bounded relative lease so separate processes can
/// use their own monotonic receipt timestamp without assuming synchronized
/// wall clocks.
struct AdmissionPublication
{
  std::string schema =
    "manyforge.robot_lifecycle.execution_admission.v0.experimental";
  std::string provider_instance_id;
  std::string robot_id;
  std::string binding_contract_generation_id;
  std::string runtime_generation_id;
  std::string provider_component_epoch;
  std::string transport_owner_component_epoch;
  std::string authority_component_epoch;
  std::string cell_supervisor_component_epoch;
  std::uint64_t publication_generation = 0;
  std::uint64_t lifecycle_generation = 0;
  std::uint64_t source_snapshot_generation = 0;
  std::int64_t projected_at_steady_time_ns = 0;
  std::int64_t lease_duration_ns = 0;
  bool controller_ready = false;
  bool motion_admitted = false;
  std::vector<std::string> reason_codes;
};

struct AdmissionProjectorConfig
{
  std::string provider_instance_id;
  std::string robot_id;
  std::string binding_contract_generation_id;
  std::int64_t maximum_lease_ns = 250'000'000;
  std::string runtime_generation_id;
  std::string provider_component_epoch;
  std::string cell_supervisor_component_epoch;
};

/// Converts lifecycle snapshots into independently versioned admission.
///
/// `LifecycleSnapshot::generation` intentionally does not advance for every
/// time-derived freshness change. The projector therefore owns a semantic
/// generation: it advances whenever admission, readiness, reasons, provider,
/// or authority identity changes, while timestamp/lease-only renewal retains
/// the authority generation.
class AdmissionProjector final
{
public:
  explicit AdmissionProjector(AdmissionProjectorConfig config);

  AdmissionProjector(const AdmissionProjector &) = delete;
  AdmissionProjector & operator=(const AdmissionProjector &) = delete;

  [[nodiscard]] std::optional<AdmissionPublication> project(
    const LifecycleSnapshot & snapshot,
    std::int64_t now_ns);

private:
  struct SemanticState
  {
    std::string supervisor_instance_id;
    AuthorityProfile authority_profile = AuthorityProfile::kLocalOperator;
    bool authority_present = false;
    std::string authority_session_id;
    std::uint64_t authority_session_generation = 0;
    std::string authority_principal_id;
    std::string authority_component_epoch;
    std::string transport_owner_component_epoch;
    bool controller_ready = false;
    bool motion_admitted = false;
    std::vector<std::string> reason_codes;

    bool operator==(const SemanticState & other) const = default;
  };

  AdmissionProjectorConfig config_;
  std::optional<SemanticState> semantic_state_;
  std::uint64_t publication_generation_ = 0;
  std::uint64_t lifecycle_generation_ = 0;
  std::uint64_t source_snapshot_generation_ = 0;
  std::int64_t last_projection_time_ns_ = 0;
};

}  // namespace manyforge::robot_lifecycle
