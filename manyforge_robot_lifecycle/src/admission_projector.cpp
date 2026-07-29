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

#include <manyforge_robot_lifecycle/admission_projector.hpp>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace manyforge::robot_lifecycle
{
namespace
{

void require_identifier(
  const std::string & value,
  const char * label)
{
  if (value.empty() || value.size() > 256 ||
    std::any_of(
      value.begin(), value.end(),
      [](unsigned char character) {
        return character < 0x21U || character > 0x7eU;
      }))
  {
    throw std::invalid_argument(
            std::string(label) + " must be a bounded printable identifier");
  }
}

std::int64_t positive_remaining(
  std::int64_t deadline_ns,
  std::int64_t now_ns,
  std::int64_t fallback_ns)
{
  if (deadline_ns <= now_ns) {
    return fallback_ns;
  }
  return deadline_ns - now_ns;
}

}  // namespace

AdmissionProjector::AdmissionProjector(AdmissionProjectorConfig config)
: config_(std::move(config))
{
  require_identifier(
    config_.provider_instance_id, "provider_instance_id");
  require_identifier(config_.robot_id, "robot_id");
  require_identifier(
    config_.binding_contract_generation_id,
    "binding_contract_generation_id");
  require_identifier(
    config_.runtime_generation_id, "runtime_generation_id");
  require_identifier(
    config_.provider_component_epoch, "provider_component_epoch");
  require_identifier(
    config_.cell_supervisor_component_epoch,
    "cell_supervisor_component_epoch");
  if (config_.maximum_lease_ns <= 0) {
    throw std::invalid_argument("maximum_lease_ns must be positive");
  }
}

std::optional<AdmissionPublication> AdmissionProjector::project(
  const LifecycleSnapshot & snapshot,
  std::int64_t now_ns)
{
  const bool observation_fresh_consistent =
    !snapshot.observation_fresh ||
    (snapshot.observed.generation > 0 &&
    !snapshot.observed.transport_owner_component_epoch.empty() &&
    snapshot.observed.source_timestamp_ns > 0 &&
    snapshot.observed.source_timestamp_ns <=
    snapshot.observed.received_timestamp_ns &&
    snapshot.observed.received_timestamp_ns <= now_ns &&
    snapshot.observed.fresh_until_ns > now_ns);
  const bool authority_fresh_consistent =
    !snapshot.authority_fresh ||
    (snapshot.authority_session.has_value() &&
    snapshot.authority_session->profile == snapshot.authority_profile &&
    snapshot.authority_session->generation > 0 &&
    !snapshot.authority_session->session_id.empty() &&
    !snapshot.authority_session->principal_id.empty() &&
    !snapshot.authority_session->component_epoch.empty() &&
    snapshot.authority_session->valid_until_ns > now_ns);
  if (snapshot.robot_id != config_.robot_id ||
    snapshot.supervisor_instance_id.empty() ||
    snapshot.generation == 0 ||
    now_ns <= 0 ||
    !observation_fresh_consistent ||
    !authority_fresh_consistent ||
    (snapshot.controller_ready && !snapshot.observation_fresh) ||
    (last_projection_time_ns_ > 0 && now_ns < last_projection_time_ns_) ||
    (source_snapshot_generation_ > 0 &&
    snapshot.generation < source_snapshot_generation_) ||
    (snapshot.motion_admitted &&
    (!snapshot.controller_ready ||
    !snapshot.observation_fresh ||
    !snapshot.authority_fresh ||
    !snapshot.authority_session.has_value())))
  {
    return std::nullopt;
  }

  auto reason_codes = snapshot.admission_reason_codes;
  if (std::any_of(
      reason_codes.begin(), reason_codes.end(),
      [](const std::string & reason) {return reason.empty();}))
  {
    return std::nullopt;
  }
  std::sort(reason_codes.begin(), reason_codes.end());
  if (std::adjacent_find(reason_codes.begin(), reason_codes.end()) !=
    reason_codes.end())
  {
    return std::nullopt;
  }
  if (snapshot.motion_admitted && !reason_codes.empty()) {
    return std::nullopt;
  }

  SemanticState semantic;
  semantic.supervisor_instance_id = snapshot.supervisor_instance_id;
  semantic.authority_profile = snapshot.authority_profile;
  semantic.transport_owner_component_epoch =
    snapshot.observed.transport_owner_component_epoch;
  semantic.controller_ready = snapshot.controller_ready;
  semantic.motion_admitted = snapshot.motion_admitted;
  semantic.reason_codes = reason_codes;
  if (snapshot.authority_session.has_value()) {
    semantic.authority_present = true;
    semantic.authority_session_id =
      snapshot.authority_session->session_id;
    semantic.authority_session_generation =
      snapshot.authority_session->generation;
    semantic.authority_principal_id =
      snapshot.authority_session->principal_id;
    semantic.authority_component_epoch =
      snapshot.authority_session->component_epoch;
  }

  if (!semantic_state_.has_value() || *semantic_state_ != semantic) {
    if (lifecycle_generation_ ==
      std::numeric_limits<std::uint64_t>::max())
    {
      throw std::overflow_error(
              "admission lifecycle generation exhausted");
    }
    ++lifecycle_generation_;
    semantic_state_ = semantic;
  }
  if (publication_generation_ ==
    std::numeric_limits<std::uint64_t>::max())
  {
    throw std::overflow_error(
            "admission publication generation exhausted");
  }
  ++publication_generation_;

  std::int64_t lease_duration_ns = config_.maximum_lease_ns;
  if (snapshot.observation_fresh) {
    lease_duration_ns = std::min(
      lease_duration_ns,
      positive_remaining(
        snapshot.observed.fresh_until_ns,
        now_ns,
        config_.maximum_lease_ns));
  }
  if (snapshot.authority_fresh &&
    snapshot.authority_session.has_value())
  {
    lease_duration_ns = std::min(
      lease_duration_ns,
      positive_remaining(
        snapshot.authority_session->valid_until_ns,
        now_ns,
        config_.maximum_lease_ns));
  }
  if (lease_duration_ns <= 0) {
    return std::nullopt;
  }

  source_snapshot_generation_ = std::max(
    source_snapshot_generation_, snapshot.generation);
  last_projection_time_ns_ = now_ns;

  AdmissionPublication publication;
  publication.provider_instance_id =
    config_.provider_instance_id;
  publication.robot_id = config_.robot_id;
  publication.binding_contract_generation_id =
    config_.binding_contract_generation_id;
  publication.runtime_generation_id = config_.runtime_generation_id;
  publication.provider_component_epoch =
    config_.provider_component_epoch;
  publication.transport_owner_component_epoch =
    snapshot.observed.transport_owner_component_epoch;
  publication.authority_component_epoch =
    snapshot.authority_session.has_value() ?
    snapshot.authority_session->component_epoch : "";
  publication.cell_supervisor_component_epoch =
    config_.cell_supervisor_component_epoch;
  publication.publication_generation = publication_generation_;
  publication.lifecycle_generation = lifecycle_generation_;
  publication.source_snapshot_generation = snapshot.generation;
  publication.projected_at_steady_time_ns = now_ns;
  publication.lease_duration_ns = lease_duration_ns;
  publication.controller_ready = snapshot.controller_ready;
  publication.motion_admitted = snapshot.motion_admitted;
  publication.reason_codes = std::move(reason_codes);
  return publication;
}

}  // namespace manyforge::robot_lifecycle
