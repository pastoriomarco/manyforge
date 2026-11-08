#include <algorithm>
#include <cmath>
#include <manyforge_roboplan/retained_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <limits>
#include <map>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] core::ErrorDTO gripper_runtime_error(std::string code, std::string message) {
  return {std::move(code), std::move(message), {}};
}

[[nodiscard]] const core::JsonValueDTO* json_member(const core::JsonObjectDTO& object,
                                                    const std::string_view name) noexcept {
  const auto found = object.find(name);
  return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] const core::JsonValueDTO::Object& json_object(const core::JsonValueDTO& value,
                                                            const std::string_view field) {
  const auto* object = std::get_if<core::JsonValueDTO::Object>(&value.value);
  if (object == nullptr) {
    throw std::invalid_argument(std::string(field) + " is not an object");
  }
  return *object;
}

[[nodiscard]] const core::JsonValueDTO::Array& json_array(const core::JsonValueDTO& value,
                                                          const std::string_view field) {
  const auto* array = std::get_if<core::JsonValueDTO::Array>(&value.value);
  if (array == nullptr) {
    throw std::invalid_argument(std::string(field) + " is not an array");
  }
  return *array;
}

[[nodiscard]] const std::string& json_string(const core::JsonValueDTO& value,
                                             const std::string_view field) {
  const auto* text = std::get_if<std::string>(&value.value);
  if (text == nullptr || text->empty()) {
    throw std::invalid_argument(std::string(field) + " is not a non-empty string");
  }
  return *text;
}

[[nodiscard]] double json_positive_number(const core::JsonValueDTO& value,
                                          const std::string_view field) {
  const auto* number = std::get_if<double>(&value.value);
  if (number == nullptr || !std::isfinite(*number) || *number <= 0.0) {
    throw std::invalid_argument(std::string(field) + " is not a finite positive number");
  }
  return *number;
}

/// Keep Stop defaults derived from the same sealed readiness that is
/// copied into the registration. Do not infer limits from a model or adapter.
[[nodiscard]] std::vector<core::HardStopResourceLimitDTO> retained_default_stop_limits(
    const core::RuntimeParticipantBinding& binding,
    const std::optional<core::RuntimeParticipantStopReadiness>& readiness) {
  if (!readiness.has_value()) {
    return {};
  }
  const auto& stages = readiness->value().resolved_stop_base_plan.escalation_stages;
  if (stages.empty() || !stages.front().default_contract_parameters.has_value()) {
    return {};
  }
  const auto* limits_value =
      json_member(*stages.front().default_contract_parameters, "per_resource_limits");
  if (limits_value == nullptr) {
    return {};
  }
  const auto& limits = json_array(*limits_value, "Stop default per_resource_limits");
  if (limits.size() != binding.value().resources.size()) {
    throw std::invalid_argument(
        "Stop default resource limits do not cover the exact gripper binding");
  }

  std::map<core::ResourceId, core::CoordinateKind> expected;
  for (const auto& resource : binding.value().resources) {
    if (!expected.emplace(resource.resource_id, resource.coordinate_kind).second) {
      throw std::invalid_argument("gripper binding repeats a Stop resource identity");
    }
  }
  std::vector<core::HardStopResourceLimitDTO> result;
  result.reserve(limits.size());
  std::set<core::ResourceId> actual;
  for (const auto& value : limits) {
    const auto& row = json_object(value, "Stop default resource limit");
    const auto* resource_value = json_member(row, "resource_id");
    const auto* threshold_value = json_member(row, "settle_velocity_threshold");
    if (resource_value == nullptr || threshold_value == nullptr) {
      throw std::invalid_argument("Stop default resource limit is missing an exact field");
    }
    const core::ResourceId resource_id(json_string(*resource_value, "Stop default resource_id"));
    const auto expected_kind = expected.find(resource_id);
    if (expected_kind == expected.end() || !actual.insert(resource_id).second) {
      throw std::invalid_argument(
          "Stop default resource limits are duplicated or outside the gripper");
    }
    const auto& threshold = json_object(*threshold_value, "Stop default settle velocity");
    const auto* kind_value = json_member(threshold, "coordinate_kind");
    if (kind_value == nullptr) {
      throw std::invalid_argument("Stop default settle velocity has no coordinate kind");
    }
    const auto& kind = json_string(*kind_value, "Stop default settle coordinate kind");
    if (expected_kind->second == core::CoordinateKind::kAngular) {
      const auto* number = json_member(threshold, "value_rad_s");
      if (kind != "angular_rad_s" || number == nullptr) {
        throw std::invalid_argument(
            "Stop default settle velocity differs from "
            "an angular gripper resource");
      }
      result.push_back({resource_id, core::AngularVelocityDTO{json_positive_number(
                                         *number, "Stop default value_rad_s")}});
    } else {
      const auto* number = json_member(threshold, "value_m_s");
      if (kind != "linear_m_s" || number == nullptr) {
        throw std::invalid_argument(
            "Stop default settle velocity differs from "
            "a linear gripper resource");
      }
      result.push_back({resource_id, core::LinearVelocityDTO{
                                         json_positive_number(*number, "Stop default value_m_s")}});
    }
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.resource_id < right.resource_id;
  });
  return result;
}

[[nodiscard]] bool same_gripper_runtime_identity(const core::RuntimeParticipantBinding& left,
                                                 const core::RuntimeParticipantBinding& right) {
  const auto& lhs = left.value();
  const auto& rhs = right.value();
  if (!lhs.control_path.has_value() || !rhs.control_path.has_value()) {
    return false;
  }
  const auto& left_path = *lhs.control_path;
  const auto& right_path = *rhs.control_path;
  return lhs.controllable_component_id == rhs.controllable_component_id &&
         lhs.component_type_id == rhs.component_type_id &&
         lhs.component_type_revision == rhs.component_type_revision &&
         lhs.command_target_id == rhs.command_target_id && lhs.capability_id == rhs.capability_id &&
         lhs.capability_definition_revision == rhs.capability_definition_revision &&
         lhs.command_schema == rhs.command_schema && lhs.resources == rhs.resources &&
         lhs.model_bindings == rhs.model_bindings && lhs.model_groups == rhs.model_groups &&
         lhs.frame_bindings == rhs.frame_bindings &&
         lhs.collision_body_bindings == rhs.collision_body_bindings &&
         lhs.supporting_capability_provider_paths == rhs.supporting_capability_provider_paths &&
         lhs.runtime_unit_epochs == rhs.runtime_unit_epochs &&
         left_path.control_domain_id == right_path.control_domain_id &&
         left_path.control_domain_revision == right_path.control_domain_revision &&
         left_path.control_binding_id == right_path.control_binding_id &&
         left_path.control_binding_revision == right_path.control_binding_revision &&
         left_path.control_interface_contract_id == right_path.control_interface_contract_id &&
         left_path.control_interface_revision == right_path.control_interface_revision &&
         left_path.controller_topology_id == right_path.controller_topology_id &&
         left_path.topology_revision == right_path.topology_revision &&
         left_path.activation_set_id == right_path.activation_set_id &&
         left_path.provider_instance_id == right_path.provider_instance_id &&
         left_path.provider_manifest_id == right_path.provider_manifest_id &&
         left_path.provider_manifest_revision == right_path.provider_manifest_revision &&
         left_path.provider_runtime_unit_id == right_path.provider_runtime_unit_id &&
         left_path.provider_runtime_epoch == right_path.provider_runtime_epoch &&
         left_path.state_authority_id == right_path.state_authority_id &&
         left_path.state_authority_class == right_path.state_authority_class &&
         left_path.stop_evidence_policy_id == right_path.stop_evidence_policy_id &&
         left_path.stop_evidence_policy_revision == right_path.stop_evidence_policy_revision &&
         left_path.containment_granularity == right_path.containment_granularity;
}

[[nodiscard]] core::Result<void> validate_readiness(
    const core::RuntimeParticipantBinding& binding,
    const std::optional<core::RuntimeParticipantStopReadiness>& readiness) {
  if (!readiness.has_value()) {
    return core::Result<void>::Ok();
  }
  if (!binding.value().control_path.has_value() ||
      readiness->value().controllable_component_id != binding.value().controllable_component_id ||
      readiness->value().expected_controller_domain.control_domain_id !=
          binding.value().control_path->control_domain_id ||
      readiness->value().expected_controller_domain.control_domain_revision !=
          binding.value().control_path->control_domain_revision) {
    return core::Result<void>::Err(
        gripper_runtime_error("native_gripper_readiness_crosses_binding",
                              "gripper Stop readiness does not match the sealed participant"));
  }
  auto expected_domain = core::verify_controller_lifecycle_expected_domain(
      readiness->value().expected_controller_domain);
  if (!expected_domain.ok()) {
    return core::Result<void>::Err(expected_domain.error());
  }
  const auto verified = core::verify_runtime_participant_stop_readiness(readiness->value(), binding,
                                                                        expected_domain.value());
  if (!verified.ok()) {
    return core::Result<void>::Err(verified.error());
  }
  return core::Result<void>::Ok();
}

[[nodiscard]] core::Result<void> validate_state_channel(
    const core::RuntimeParticipantBinding& binding,
    const std::shared_ptr<IRetainedObservedStateChannel>& state_channel) {
  if (state_channel == nullptr) {
    return core::Result<void>::Err(
        gripper_runtime_error("native_gripper_state_channel_unavailable",
                              "native gripper has no retained observed-state channel"));
  }
  try {
    const auto actual = state_channel->query_context();
    const auto expected =
        core::make_observed_state_query_context(binding, actual.freshness_budget_ns);
    if (!expected.ok()) {
      return core::Result<void>::Err(expected.error());
    }
    if (actual == expected.value()) {
      return core::Result<void>::Ok();
    }
  } catch (const std::exception& exception) {
    return core::Result<void>::Err(gripper_runtime_error(
        "native_gripper_state_channel_binding_mismatch",
        std::string("native gripper observed-state channel verification failed: ") +
            exception.what()));
  } catch (...) {
    return core::Result<void>::Err(
        gripper_runtime_error("native_gripper_state_channel_binding_mismatch",
                              "native gripper observed-state channel verification failed"));
  }
  return core::Result<void>::Err(
      gripper_runtime_error("native_gripper_state_channel_binding_mismatch",
                            "native gripper observed-state channel does not "
                            "match the exact sealed binding"));
}

}  // namespace

RetainedGripperRuntime::RetainedGripperRuntime(RetainedGripperRuntimeInput input)
    : binding_(std::move(input.binding)),
      clock_(std::move(input.clock)),
      scheduler_(std::move(input.scheduler)),
      state_channel_(std::move(input.state_channel)),
      backend_(std::move(input.backend)),
      stop_execution_port_(input.stop_execution_port),
      stop_readiness_(std::move(input.stop_readiness)),
      position_limits_(std::move(input.position_limits)),
      intrinsic_direct_control_(input.intrinsic_direct_control),
      rebind_route_(std::move(input.rebind_route)),
      prepare_measured_sources_(std::move(input.prepare_measured_sources)) {
  if (backend_ == nullptr || clock_ == nullptr) {
    throw std::invalid_argument("native gripper runtime requires a backend and retained clock");
  }
  if (!binding_.value().control_path.has_value() || binding_.value().resources.empty()) {
    throw std::invalid_argument("native gripper runtime requires a control route and resources");
  }
  const auto state_channel_result = validate_state_channel(binding_, state_channel_);
  if (!state_channel_result.ok()) {
    throw std::invalid_argument(state_channel_result.error().message);
  }
  const auto readiness_result = validate_readiness(binding_, stop_readiness_);
  if (!readiness_result.ok()) {
    throw std::invalid_argument(readiness_result.error().message);
  }
  retained_default_stop_limits_ = retained_default_stop_limits(binding_, stop_readiness_);
  participant_engine_ = std::make_shared<core::GripperParticipantEngine>(*backend_, *clock_);
  if (stop_execution_port_ == nullptr) stop_execution_port_ = participant_engine_.get();
  if (input.control_binding_representatives.empty())
    input.control_binding_representatives.push_back(binding_);
  std::vector<std::optional<core::RuntimeParticipantStopReadiness>> readiness;
  readiness.reserve(input.control_binding_representatives.size());
  for (const auto& representative : input.control_binding_representatives)
    readiness.push_back(representative.value() == binding_.value() ? stop_readiness_ : std::nullopt);
  auto staged = PrepareRegistrations(input.control_binding_representatives, readiness);
  if (!staged.ok()) throw std::invalid_argument(staged.error().message);
  participant_registrations_ = std::move(staged.value());
}

void RetainedGripperRuntime::BindCellKernel(core::ResourceAddressedExecutionKernel& kernel) {
  std::lock_guard lock(mutex_);
  if (cell_kernel_ && cell_kernel_ != &kernel)
    throw std::invalid_argument("retained gripper cannot rotate its stable cell Kernel");
  cell_kernel_ = &kernel;
}

std::shared_ptr<const RetainedPreparedRobotMeasuredSources>
RetainedGripperRuntime::SnapshotPreparedMeasuredSources() const {
  std::lock_guard lock(mutex_);
  return prepared_measured_sources_;
}

bool RetainedGripperRuntime::MatchesPreparedMeasuredSources(
    const std::shared_ptr<const RetainedPreparedRobotMeasuredSources> &expected) const {
  std::lock_guard lock(mutex_);
  return prepared_measured_sources_ == expected;
}

bool RetainedGripperRuntime::WithExactPreparedMeasuredSources(
    const std::shared_ptr<const RetainedPreparedRobotMeasuredSources> &expected,
    void (*commit)(void *) noexcept, void *context) const {
  std::lock_guard lock(mutex_);
  if (!commit || prepared_measured_sources_ != expected) return false;
  commit(context);
  return true;
}

core::Result<core::RobotMeasuredStateSourceView> RetainedGripperRuntime::PrepareCurrentMeasuredSources() {
  using Result = core::Result<core::RobotMeasuredStateSourceView>;
  std::unique_lock lock(mutex_);
  if (!cell_kernel_ || !prepare_measured_sources_)
    return Result::Err({"measured_source_unavailable", "Gripper has no actual measured producer", {}});
  const auto binding = binding_;
  auto* const kernel = cell_kernel_;
  const auto previous = prepared_measured_sources_;
  const auto route = kernel->current_runtime_route_configuration(binding.value().control_path->control_domain_id);
  if (!route.ok()) return Result::Err(route.error());
  if (!route.value()) return Result::Err({"measured_source_unavailable", "Gripper route is not current", {}});
  const auto prepare = prepare_measured_sources_;
  lock.unlock();
  auto staged = prepare(binding, route.value()->active_configuration);
  if (!staged.ok()) return Result::Err(staged.error());
  if (!staged.value()) return Result::Err({"measured_source_unavailable", "Gripper measured producer unavailable", {}});
  if (staged.value()->source_view.sources.empty() &&
      backend_->gripper_actuation_kind(binding.value().command_target_id.value()) !=
          core::GripperActuationKind::kPneumatic)
    return Result::Err({"measured_source_unavailable", "Gripper measured producer has no source", {}});
  const auto valid = ValidateRetainedPreparedMeasuredSources(*staged.value(),
      binding.value().controllable_component_id,
      std::vector<core::RuntimeParticipantBinding>{binding},
      {route.value()->active_configuration});
  if (!valid.ok()) return Result::Err(valid.error());
  lock.lock();
  auto current = kernel->current_runtime_route_configuration(binding.value().control_path->control_domain_id);
  if (!current.ok()) return Result::Err(current.error());
  if (cell_kernel_ != kernel || binding_.value() != binding.value() || prepared_measured_sources_ != previous ||
      current.value() != route.value())
    return Result::Err({"measured_source_unavailable", "Gripper source association changed while preparing", {}});
  if (staged.value()->matches_transport_owner && !staged.value()->matches_transport_owner())
    return Result::Err({"measured_source_unavailable", "Gripper transport changed while preparing sources", {}});
  auto prepared = std::move(staged.value());
  prepared_measured_sources_.swap(prepared);
  prepared_measured_sources_->enable_publication();
  return Result::Ok(prepared_measured_sources_->source_view);
}


core::RuntimeParticipantExecutionRegistration RetainedGripperRuntime::registration() const {
  std::lock_guard lock(mutex_);
  return core::RuntimeParticipantExecutionRegistration{
      .binding = binding_,
      .stop_readiness = stop_readiness_,
      .retained_default_stop_limits = retained_default_stop_limits_,
      .stop_node_category = core::MotionInterruptNodeCategory::kGeneric,
      .engine = participant_engine_.get(),
      .stop_execution_port = stop_execution_port_,
      .participant_policy = {},
      .prepared_motion_engine = dynamic_cast<
          core::IResolvedPreparedParticipantExecutionEngine *>(
          participant_engine_.get()),
      .default_operator_stop_profile = {},
      .default_fault_stop_profile = {},
      .deployment_maximum_tcp_linear_speed_mps = {},
      .intrinsic_direct_control = intrinsic_direct_control_,
      .direct_control_joint_limits = {}};
}

std::vector<core::RuntimeParticipantExecutionRegistration>
RetainedGripperRuntime::registrations() const {
  std::lock_guard lock(mutex_);
  auto rows = participant_registrations_;
  for (auto& row : rows) {
    if (row.binding.value().capability_id == binding_.value().capability_id) {
      row.binding = binding_;
      row.stop_readiness = stop_readiness_;
      row.retained_default_stop_limits = retained_default_stop_limits_;
    }
  }
  return rows;
}

core::Result<std::vector<core::RuntimeParticipantExecutionRegistration>>
RetainedGripperRuntime::PrepareRegistrations(
    const std::vector<core::RuntimeParticipantBinding>& representatives,
    const std::vector<std::optional<core::RuntimeParticipantStopReadiness>>& readiness) const {
  using Result = core::Result<std::vector<core::RuntimeParticipantExecutionRegistration>>;
  std::lock_guard lock(mutex_);
  if (representatives.empty() || representatives.size() != readiness.size())
    return Result::Err(gripper_runtime_error("native_gripper_alias_set_invalid",
        "Gripper representatives and readiness must cover the same nonempty set"));
  if (!participant_registrations_.empty() && representatives.size() != participant_registrations_.size())
    return Result::Err(gripper_runtime_error("native_gripper_alias_set_invalid",
        "Route publication must preserve the complete installed binding set"));
  const auto primary = std::find_if(representatives.begin(), representatives.end(), [&](const auto& row) {
    return row.value().capability_id == binding_.value().capability_id;
  });
  if (primary == representatives.end() || !same_gripper_runtime_identity(binding_, *primary))
    return Result::Err(gripper_runtime_error("native_gripper_alias_owner_mismatch",
        "Gripper representatives lost their exact primary command owner"));
  const auto primary_index = static_cast<std::size_t>(std::distance(representatives.begin(), primary));
  std::set<core::ControlBindingId> ids;
  std::set<core::CapabilityId> capabilities;
  std::vector<core::RuntimeParticipantExecutionRegistration> result;
  result.reserve(representatives.size());
  for (std::size_t index = 0; index < representatives.size(); ++index) {
    const auto& binding = representatives[index];
    const auto& value = binding.value();
    const bool command = value.capability_id == primary->value().capability_id;
    const auto known = std::find_if(participant_registrations_.begin(), participant_registrations_.end(),
        [&](const auto& row) { return row.binding.value().capability_id == value.capability_id; });
    if (!value.control_path || (!command && !retained_bindings_share_backend_owner(*primary, binding)) ||
        !ids.insert(value.control_path->control_binding_id).second ||
        !capabilities.insert(value.capability_id).second ||
        (!command && (primary->value().capability_id.value() != "tool.gripper.command" ||
                      value.capability_id.value() != "tool.gripper.feedback" || value.command_schema.has_value())) ||
        (!participant_registrations_.empty() &&
         (known == participant_registrations_.end() ||
          !same_gripper_runtime_identity(known->binding, binding) ||
          known->binding.value().capability_provider_path != value.capability_provider_path)))
      return Result::Err(gripper_runtime_error("native_gripper_alias_owner_mismatch",
          "Gripper alias crosses its exact command owner or repeats a capability/binding"));
    auto qualified = validate_readiness(binding, readiness[index]);
    if (!qualified.ok()) return Result::Err(qualified.error());
    if (!command && readiness[index] &&
        (!readiness[primary_index] ||
         readiness[index]->value().component_lifecycle_generation != readiness[primary_index]->value().component_lifecycle_generation ||
         readiness[index]->value().expected_controller_domain != readiness[primary_index]->value().expected_controller_domain))
      return Result::Err(gripper_runtime_error("native_gripper_alias_readiness_mismatch",
          "Feedback readiness must share the primary component generation and exact domain seal"));
    result.push_back(core::RuntimeParticipantExecutionRegistration{
        .binding = binding,
        .stop_readiness = readiness[index],
        .retained_default_stop_limits = retained_default_stop_limits(binding, readiness[index]),
        .stop_node_category = core::MotionInterruptNodeCategory::kGeneric,
        .engine = participant_engine_.get(),
        .stop_execution_port = stop_execution_port_,
        .participant_policy = {},
        .prepared_motion_engine = command ? dynamic_cast<core::IResolvedPreparedParticipantExecutionEngine*>(participant_engine_.get()) : nullptr,
        .default_operator_stop_profile = {}, .default_fault_stop_profile = {},
        .deployment_maximum_tcp_linear_speed_mps = {},
        .intrinsic_direct_control = command && intrinsic_direct_control_,
        .direct_control_joint_limits = {}});
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.binding.value().control_path->control_binding_id < right.binding.value().control_path->control_binding_id;
  });
  return Result::Ok(std::move(result));
}

void RetainedGripperRuntime::AdoptVerifiedRegistrations(
    std::vector<core::RuntimeParticipantExecutionRegistration>&& registrations) noexcept {
  std::lock_guard lock(mutex_);
  participant_registrations_.swap(registrations);
}

core::RuntimeParticipantBinding RetainedGripperRuntime::binding() const {
  std::lock_guard lock(mutex_);
  return binding_;
}

std::optional<core::RuntimeParticipantStopReadiness> RetainedGripperRuntime::stop_readiness()
    const {
  std::lock_guard lock(mutex_);
  return stop_readiness_;
}

void RetainedGripperRuntime::AdoptVerifiedReadiness(
    std::optional<core::RuntimeParticipantStopReadiness>&& readiness) noexcept {
  std::lock_guard lock(mutex_);
  stop_readiness_.swap(readiness);
}

std::shared_ptr<IRetainedObservedStateChannel> RetainedGripperRuntime::state_channel() const {
  std::lock_guard lock(mutex_);
  return state_channel_;
}

std::optional<RetainedGripperPositionLimits>
RetainedGripperRuntime::position_limits() const noexcept {
  return position_limits_;
}

core::GripperParticipantEngine* RetainedGripperRuntime::participant_engine() const noexcept {
  return participant_engine_.get();
}

std::optional<core::GripperOwnerRecoveryEvidence>
RetainedGripperRuntime::GripperOwnerRecoveryEvidence(
    const bool measured_lane, const std::string &target,
    const std::shared_ptr<const void> &owner) const {
  std::shared_ptr<const RetainedPreparedRobotMeasuredSources> sources;
  std::vector<core::HardStopResourceLimitDTO> limits;
  {
    std::lock_guard lock(mutex_);
    if (!owner || binding_.value().command_target_id.value() != target) return std::nullopt;
    sources = prepared_measured_sources_;
    limits = retained_default_stop_limits_;
  }
  core::GripperOwnerRecoveryEvidence evidence;
  evidence.owner = owner;
  if (!measured_lane) {
    evidence.mode = core::GripperOwnerRecoveryProofMode::kNoFeedback;
    return evidence;
  }
  if (!sources || !sources->owner_rebuild_boundary || sources->source_view.sources.size() != 1U)
    return std::nullopt;
  const auto boundary = sources->owner_rebuild_boundary(0U);
  if (!boundary || boundary->owner != owner) return std::nullopt;
  const auto &source = sources->source_view.sources.front();
  const auto *publication = dynamic_cast<const core::AddressedObservedStatePublication *>(source.reader.get());
  const auto &resources = source.descriptor.publication.query_context.resources;
  if (!publication || resources.empty() || limits.size() != resources.size()) return std::nullopt;
  auto view = publication->numeric_publication().acquire(clock_->now());
  if (!view || view.sequence() < boundary->next_sequence) return std::nullopt;
  core::ObservedStateEvidenceDTO sample;
  sample.sequence = view.sequence();
  sample.sampled_at_ns = view.received_timestamp_ns();
  sample.freshness_budget_ns = view.freshness_budget_ns();
  if (sample.freshness_budget_ns == 0)
    sample.freshness_budget_ns = std::numeric_limits<std::int64_t>::max() -
                                 std::max<std::int64_t>(0, sample.sampled_at_ns);
  sample.complete = view.complete();
  sample.joint_order_token = std::string(view.joint_order_token());
  sample.source_identity = std::string(view.source_id());
  sample.authority_profile = std::string(view.authority_profile_id());
  sample.measured_authority = view.authority_profile() == core::ObservedRobotStateAuthorityProfile::kMeasured;
  sample.at_rest = view.velocities_complete() && view.velocities_rad_s().size() == resources.size();
  if (sample.at_rest) {
    for (std::size_t i = 0; i < resources.size(); ++i) {
      const auto limit = std::find_if(limits.begin(), limits.end(),
          [&](const auto &row) { return row.resource_id == resources[i]; });
      if (limit == limits.end()) return std::nullopt;
      const auto threshold = std::visit([](const auto &value) {
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, core::AngularVelocityDTO>)
          return value.value_rad_s;
        else return value.value_m_s;
      }, limit->settle_velocity_threshold);
      const double velocity = view.velocities_rad_s()[i];
      if (!std::isfinite(velocity) || std::abs(velocity) > threshold) sample.at_rest = false;
    }
  }
  evidence.mode = core::GripperOwnerRecoveryProofMode::kMeasured;
  evidence.first_sequence = boundary->next_sequence;
  evidence.joint_order_token = sample.joint_order_token;
  evidence.sample = std::move(sample);
  return evidence;
}

core::IGripperCommandBackend* RetainedGripperRuntime::backend() const noexcept {
  return backend_.get();
}

std::shared_ptr<core::IGripperCommandBackend>
RetainedGripperRuntime::backend_owner() const noexcept {
  return backend_;
}

core::IControlDomainExecutionPort* RetainedGripperRuntime::stop_execution_port()
    const noexcept {
  return stop_execution_port_;
}

core::Result<void> RetainedGripperRuntime::RebindRoute(
    core::RuntimeParticipantBinding candidate,
    std::shared_ptr<IRetainedObservedStateChannel> candidate_channel,
    std::optional<core::RuntimeParticipantStopReadiness> readiness) {
  return RebindRouteImpl(std::move(candidate), std::move(candidate_channel),
                         std::move(readiness), false);
}

core::Result<void> RetainedGripperRuntime::RebindPredecessorRoute(
    core::RuntimeParticipantBinding candidate,
    std::shared_ptr<IRetainedObservedStateChannel> candidate_channel) {
  return RebindRouteImpl(std::move(candidate), std::move(candidate_channel),
                         std::nullopt, true);
}

core::Result<void> RetainedGripperRuntime::RebindRouteImpl(
    core::RuntimeParticipantBinding candidate,
    std::shared_ptr<IRetainedObservedStateChannel> candidate_channel,
    std::optional<core::RuntimeParticipantStopReadiness> readiness,
    const bool predecessor_recovery) {
  std::lock_guard lock(mutex_);
  if (!same_gripper_runtime_identity(binding_, candidate)) {
    return core::Result<void>::Err(
        gripper_runtime_error("native_gripper_retained_identity_changed",
                              "candidate binding changes the retained gripper or its controller"));
  }
  const auto state_channel_result = validate_state_channel(candidate, candidate_channel);
  if (!state_channel_result.ok()) {
    return state_channel_result;
  }
  if (stop_readiness_.has_value() && !readiness.has_value() &&
      !predecessor_recovery) {
    return core::Result<void>::Err(gripper_runtime_error(
        "native_gripper_readiness_dropped",
        "a retained Stop-qualified gripper cannot lose its readiness record"));
  }
  if (readiness.has_value() && stop_execution_port_ == nullptr) {
    return core::Result<void>::Err(
        gripper_runtime_error("native_gripper_stop_port_unavailable",
                              "candidate Stop readiness has no retained Stop-stage owner"));
  }
  if (!rebind_route_) {
    return core::Result<void>::Err(
        gripper_runtime_error("native_gripper_route_rebind_unverified",
                              "retained gripper has no owner that can verify candidate routing"));
  }
  const auto readiness_result = validate_readiness(candidate, readiness);
  if (!readiness_result.ok()) {
    return readiness_result;
  }
  core::GripperCommandStatusDTO status;
  try {
    status = backend_->get_gripper_status(binding_.value().command_target_id.value());
  } catch (const std::exception& exception) {
    return core::Result<void>::Err(
        gripper_runtime_error("native_gripper_status_unavailable", exception.what()));
  } catch (...) {
    return core::Result<void>::Err(gripper_runtime_error("native_gripper_status_unavailable",
                                                         "native gripper status read failed"));
  }
  if (status.gripper_id != binding_.value().command_target_id.value() ||
      status.state == core::GripperCommandState::kExecuting) {
    return core::Result<void>::Err(
        gripper_runtime_error("native_gripper_route_rebind_busy",
                              "retained gripper status is not a verified idle or terminal state"));
  }

  std::vector<core::HardStopResourceLimitDTO> candidate_defaults;
  try {
    candidate_defaults = retained_default_stop_limits(candidate, readiness);
  } catch (const std::exception& exception) {
    return core::Result<void>::Err(
        gripper_runtime_error("native_gripper_readiness_invalid", exception.what()));
  }
  core::Result<void> rebound = core::Result<void>::Err(
      gripper_runtime_error("native_gripper_route_rebind_unverified",
                            "native gripper route rebind did not return a verified result"));
  try {
    rebound = rebind_route_(candidate, candidate_channel, readiness);
  } catch (const std::exception& exception) {
    return core::Result<void>::Err(
        gripper_runtime_error("native_gripper_route_rebind_failed", exception.what()));
  } catch (...) {
    return core::Result<void>::Err(gripper_runtime_error("native_gripper_route_rebind_failed",
                                                         "native gripper route rebind failed"));
  }
  if (!rebound.ok()) {
    return rebound;
  }
  const auto rebound_channel_result = validate_state_channel(candidate, candidate_channel);
  if (!rebound_channel_result.ok()) {
    return rebound_channel_result;
  }
  binding_ = std::move(candidate);
  // A re-seal changes the exact route query. The old measured association may
  // not be returned while the actual producer for the new route is prepared.
  prepared_measured_sources_.reset();
  state_channel_ = std::move(candidate_channel);
  stop_readiness_ = std::move(readiness);
  retained_default_stop_limits_ = std::move(candidate_defaults);
  return core::Result<void>::Ok();
}

}  // namespace manyforge::roboplan_backend
