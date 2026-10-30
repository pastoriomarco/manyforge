#include "product_backed_runtime_participant_resolver.hpp"
#include "sealed_program_execution_capture.hpp"
#include <manyforge_core/program_frame_identity.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_deployment/program_io_stop_response_validation.hpp>

#include "manyforge_roboplan/retained_collision_world_projector.hpp"
#include "manyforge_roboplan/retained_native_runtime.hpp"

#include <algorithm>
#include <array>
#include <manyforge_deployment/cell_kinematic_index.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace manyforge::roboplan_backend {
namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

template <typename Value>
[[nodiscard]] core::Result<Value>
Reject(std::string message, std::string reason,
       core::ErrorCode code = core::ErrorCode::kExecutionRejected) {
  return core::Result<Value>::Err(
      core::ErrorDTO{std::string(core::to_string(code)),
                     std::move(message),
                     {{"reason", std::move(reason)}}});
}

[[nodiscard]] core::CapabilityId
RequiredPlannerCapability(const core::MotionParticipantIntentDTO &motion) {
  return std::visit(
      [](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, core::JointGoalDTO>) {
          return core::CapabilityId("planning.motion.joint_goal");
        } else if constexpr (std::is_same_v<Value, core::PoseGoalDTO>) {
          return core::CapabilityId("planning.motion.pose_goal");
        } else if constexpr (std::is_same_v<Value, core::NamedTargetDTO>) {
          return core::CapabilityId("planning.motion.named_target");
        } else {
          return core::CapabilityId("planning.motion.cartesian_path");
        }
      },
      motion.motion);
}

template <typename Available, typename Selected, typename Matches>
[[nodiscard]] bool IsSubset(const std::vector<Available> &available,
                            const std::vector<Selected> &selected,
                            Matches matches) {
  return std::all_of(selected.begin(), selected.end(), [&](const auto &value) {
    return std::count_if(available.begin(), available.end(),
                         [&](const auto &candidate) {
                           return matches(candidate, value);
                         }) == 1;
  });
}

[[nodiscard]] bool SupportingPathAvailable(
    const std::vector<core::RuntimeCapabilityProviderPathDTO> &available,
    const core::RuntimeCapabilityProviderPathDTO &selected) {
  return std::count(available.begin(), available.end(), selected) == 1;
}

[[nodiscard]] bool
PhysicallyContainedBy(const core::RuntimeParticipantBinding &physical,
                      const core::RuntimeParticipantBinding &selected) {
  const auto &available = physical.value();
  const auto &value = selected.value();
  return available.retained_bundle_id == value.retained_bundle_id &&
         available.graph_revision == value.graph_revision &&
         available.controllable_component_id ==
             value.controllable_component_id &&
         available.component_type_id == value.component_type_id &&
         available.component_type_revision == value.component_type_revision &&
         available.command_target_id == value.command_target_id &&
         available.capability_id == value.capability_id &&
         available.capability_definition_revision ==
             value.capability_definition_revision &&
         available.command_schema == value.command_schema &&
         IsSubset(available.model_bindings, value.model_bindings,
                  [](const auto &left, const auto &right) {
                    return left.model_instance_id == right.model_instance_id &&
                           left.model_revision == right.model_revision;
                  }) &&
         IsSubset(available.model_groups, value.model_groups,
                  [](const auto &left, const auto &right) {
                    return left.model_instance_id == right.model_instance_id &&
                           left.compiled_group_name ==
                               right.compiled_group_name &&
                           left.expanded_compiled_joint_names ==
                               right.expanded_compiled_joint_names;
                  }) &&
         IsSubset(available.frame_bindings, value.frame_bindings,
                  [](const auto &left, const auto &right) {
                    // The role keeps rows exact where two roles share one
                    // frame (a UR5's `flange` and `tcp`).
                    return core::same_program_frame(left, right);
                  }) &&
         IsSubset(available.resources, value.resources,
                  [](const auto &left, const auto &right) {
                    return left.resource_id == right.resource_id &&
                           left.source_joint_name == right.source_joint_name &&
                           left.compiled_resource_name ==
                               right.compiled_resource_name &&
                           left.coordinate_kind == right.coordinate_kind;
                  }) &&
         IsSubset(available.collision_body_bindings,
                  value.collision_body_bindings,
                  [](const auto &left, const auto &right) {
                    return left.model_instance_id == right.model_instance_id &&
                           left.model_revision == right.model_revision &&
                           left.compiled_collision_body_names ==
                               right.compiled_collision_body_names;
                  }) &&
         available.control_path == value.control_path &&
         available.capability_provider_path == value.capability_provider_path &&
         std::all_of(value.supporting_capability_provider_paths.begin(),
                     value.supporting_capability_provider_paths.end(),
                     [&](const auto &path) {
                       return SupportingPathAvailable(
                           available.supporting_capability_provider_paths,
                           path);
                     }) &&
         available.runtime_unit_epochs == value.runtime_unit_epochs;
}

[[nodiscard]] const core::RuntimeParticipantBinding &RequirePhysicalBinding(
    const deployment::RuntimeProgramTargetBinding &target,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings) {
  const core::RuntimeParticipantBinding *result = nullptr;
  for (const auto &binding : physical_bindings) {
    const auto &value = binding.value();
    if (value.controllable_component_id !=
            target.component().controllable_component_id ||
        value.command_target_id != target.target_role().command_target_id ||
        value.capability_id != target.capability().capability_id) {
      continue;
    }
    if (result != nullptr) {
      throw std::runtime_error(
          "Program target resolves to multiple physical runtime owners");
    }
    result = &binding;
  }
  if (result == nullptr) {
    throw std::runtime_error(
        "Program target has no composed physical runtime owner");
  }
  return *result;
}

[[nodiscard]] const core::RuntimeParticipantBinding *
FindExactPrecomposedBinding(
    const core::OperationIntentDTO &operation,
    const core::OperationParticipantIntentDTO &participant,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings) {
  const core::RuntimeParticipantBinding *result = nullptr;
  const auto *command =
      std::get_if<core::CapabilityCommandIntentDTO>(&participant.payload);
  const auto *output = command
                           ? std::get_if<core::IoSignalWriteCommandValueDTO>(
                                 &command->command_value)
                           : nullptr;
  for (const auto &binding : physical_bindings) {
    const auto &value = binding.value();
    if (value.io_output_binding && !output)
      continue;
    if (output &&
        (!value.io_output_binding ||
         value.io_output_binding->resource_role_id != output->resource_role_id))
      continue;
    if (value.program_id != operation.program_id ||
        value.program_revision != operation.program_revision ||
        value.program_binding_id != operation.program_binding_id ||
        value.component_role_id != participant.component_role_id ||
        value.target_role_id != participant.target_role_id ||
        value.capability_id != participant.required_capability_id) {
      continue;
    }
    if (result != nullptr) {
      throw std::runtime_error(
          "operation resolves to multiple exact precomposed runtime owners");
    }
    result = &binding;
  }
  return result;
}

[[nodiscard]] const core::RuntimeParticipantBinding &RequireOutputBinding(
    const deployment::RuntimeProgramTargetBinding &target,
    const core::ResourceRoleId &resource_role,
    const std::vector<core::RuntimeParticipantBinding> &composed) {
  const core::RuntimeParticipantBinding *exact = nullptr;
  const core::RuntimeParticipantBinding *selected = nullptr;
  bool canonical_ambiguous = false;
  for (const auto &binding : composed) {
    const auto &value = binding.value();
    if (value.controllable_component_id !=
            target.component().controllable_component_id ||
        value.command_target_id != target.target_role().command_target_id ||
        value.capability_id != target.capability().capability_id ||
        !value.io_output_binding ||
        value.io_output_binding->resource_role_id != resource_role)
      continue;
    if (value.program_binding_id == target.program_binding_id()) {
      if (exact)
        throw std::runtime_error(
            "Program output resolves to multiple composed channel owners");
      exact = &binding;
    }
    if (selected)
      canonical_ambiguous = true;
    else
      selected = &binding;
  }
  // Complete host inventories include direct and Program aliases of the same
  // channel. Prefer the exact sealed Program identity; retain the existing
  // unique canonical-owner path when no Program alias is composed.
  if (exact)
    return *exact;
  if (canonical_ambiguous)
    throw std::runtime_error(
        "Program output resolves to multiple composed channel owners");
  if (!selected)
    throw std::runtime_error(
        "Program output has no exact composed channel owner");
  return *selected;
}

[[nodiscard]] core::RuntimeProviderDomainConfigurationReadbackDTO
ClosedOutputProvenance(const core::RuntimeParticipantBinding &composed,
                       const deployment::RetainedBundle &bundle) {
  const auto &value = composed.value();
  const auto &output = *value.io_output_binding;
  const auto &graph = bundle.value().cell_graph;
  const auto domain = std::find_if(
      graph.control_domains.begin(), graph.control_domains.end(),
      [&](const auto &row) {
        return row.control_domain_id == output.provider.control_domain_id &&
               row.control_domain_revision ==
                   output.provider.control_domain_revision;
      });
  if (domain == graph.control_domains.end() || !domain->provider())
    throw std::runtime_error("Composed output has no sealed provider domain");
  const auto catalog = deployment::ResolveRetainedIoSignalCatalog(graph);
  if (!catalog.ok())
    throw std::runtime_error(catalog.error().message);
  std::vector<core::ResourceId> resources;
  for (const auto &row : catalog.value()) {
    if (row.descriptor.signal_id.controllable_component_id ==
            value.controllable_component_id &&
        row.runtime_unit_id == output.provider.provider_runtime_unit_id &&
        std::binary_search(domain->provider()->signal_names.begin(),
                           domain->provider()->signal_names.end(),
                           row.descriptor.signal_id.name) &&
        row.descriptor.output_resource)
      resources.push_back(*row.descriptor.output_resource);
  }
  std::sort(resources.begin(), resources.end());
  resources.erase(std::unique(resources.begin(), resources.end()),
                  resources.end());
  // The composed binding carries the actual CCLC/provider occurrence. This
  // structural view deliberately asserts no driver observation or eligibility;
  // ordinary Kernel admission still checks its actual current readiness map.
  return {value.retained_bundle_id,
          value.graph_revision,
          output.component_lifecycle_generation,
          {domain->control_domain_id, domain->control_domain_revision,
           core::ResolvedProviderDomainScopeDTO{
               *domain->provider(), output.provider.provider_runtime_epoch,
               std::nullopt, std::move(resources)}},
          output.provider,
          false,
          0U,
          0U};
}

struct ProviderOccurrence final {
  core::RuntimeUnitId runtime_unit_id;
  core::RuntimeEpoch runtime_epoch;

  friend bool operator==(const ProviderOccurrence &,
                         const ProviderOccurrence &) = default;
};

void InsertProviderOccurrence(
    std::map<core::ProviderInstanceId, ProviderOccurrence> &occurrences,
    const core::ProviderInstanceId &provider,
    const core::RuntimeUnitId &runtime_unit_id,
    const core::RuntimeEpoch &runtime_epoch) {
  const auto [found, inserted] = occurrences.emplace(
      provider, ProviderOccurrence{runtime_unit_id, runtime_epoch});
  if (!inserted &&
      found->second != ProviderOccurrence{runtime_unit_id, runtime_epoch}) {
    throw std::runtime_error(
        "physical runtime advertises conflicting provider occurrences");
  }
}

[[nodiscard]] deployment::ActiveRuntimeParticipantFactsDTO ActiveFacts(
    const deployment::RuntimeProgramTargetBinding &target,
    const std::span<const deployment::RuntimeProgramTargetBinding> supporting,
    const core::RuntimeParticipantBinding &physical) {
  if (!target.desired_route().has_value() ||
      !physical.value().control_path.has_value()) {
    throw std::runtime_error(
        "Program target or physical owner has no active control route");
  }
  const auto &control = *physical.value().control_path;
  if (*target.desired_route() != control.control_route_id ||
      !target.control_domain().has_value() ||
      target.control_domain()->control_domain_id != control.control_domain_id ||
      target.control_domain()->control_domain_revision !=
          control.control_domain_revision) {
    throw std::runtime_error(
        "Program target crosses the physical owner's active control route");
  }

  std::map<core::ProviderInstanceId, ProviderOccurrence> available_occurrences;
  InsertProviderOccurrence(available_occurrences, control.provider_instance_id,
                           control.provider_runtime_unit_id,
                           control.provider_runtime_epoch);
  if (physical.value().capability_provider_path.has_value()) {
    const auto &path = *physical.value().capability_provider_path;
    InsertProviderOccurrence(available_occurrences, path.provider_instance_id,
                             path.provider_runtime_unit_id,
                             path.provider_runtime_epoch);
  }
  for (const auto &path :
       physical.value().supporting_capability_provider_paths) {
    InsertProviderOccurrence(available_occurrences, path.provider_instance_id,
                             path.provider_runtime_unit_id,
                             path.provider_runtime_epoch);
  }

  std::set<core::ProviderInstanceId> required_providers;
  if (target.control_binding().has_value()) {
    required_providers.insert(target.control_binding()->provider_instance_id);
  }
  if (target.capability_provider_binding().has_value()) {
    required_providers.insert(
        target.capability_provider_binding()->provider_instance_id);
  }
  for (const auto &support : supporting) {
    if (support.capability_provider_binding().has_value()) {
      required_providers.insert(
          support.capability_provider_binding()->provider_instance_id);
    }
  }
  std::vector<deployment::RuntimeProviderEpochFactDTO> provider_epochs;
  provider_epochs.reserve(required_providers.size());
  for (const auto &provider : required_providers) {
    const auto found = available_occurrences.find(provider);
    if (found == available_occurrences.end()) {
      throw std::runtime_error(
          "Program target names a provider outside the physical owner");
    }
    provider_epochs.push_back(
        {provider, found->second.runtime_unit_id, found->second.runtime_epoch});
  }
  return deployment::ActiveRuntimeParticipantFactsDTO{
      control.control_route_id, control.active_control_configuration_id,
      control.active_control_configuration_generation,
      physical.value().runtime_unit_epochs, std::move(provider_epochs)};
}

} // namespace

core::Result<core::ProgramTargetRoleBindingDTO>
ProductBackedRuntimeParticipantResolver::ResolveOperationTarget(
    const core::ProgramBindingId &program_binding_id,
    const core::TargetRoleId &target_role_id) const {
  try {
    const auto binding = LoadProgramBinding(program_binding_id);
    if (!binding)
      throw std::invalid_argument("no exact retained Program Binding");
    const auto program = LoadProgram(binding->value().program_id,
                                     binding->value().program_revision);
    if (!program ||
        program->value().node_environment != binding->value().node_environment)
      throw std::invalid_argument("no exact canonical Program");
    const auto &targets = binding->value().target_role_bindings;
    const auto target =
        std::find_if(targets.begin(), targets.end(), [&](const auto &row) {
          return row.target_role_id == target_role_id;
        });
    if (target == targets.end())
      throw std::invalid_argument("no declared target role");
    return core::Result<core::ProgramTargetRoleBindingDTO>::Ok(*target);
  } catch (const std::exception &error) {
    return core::Result<core::ProgramTargetRoleBindingDTO>::Err(core::ErrorDTO{
        "execution_operation_target_unavailable", error.what(), {}});
  }
}

core::RuntimeUnitId retained_provider_runtime_unit(
    const core::CapabilityProviderBindingDTO &binding) {
  std::set<core::RuntimeUnitId> selected;
  for (const auto &endpoint : binding.endpoint_bindings) {
    if (endpoint.source_kind !=
        core::CapabilityEndpointSourceKind::kRuntimeUnit)
      continue;
    const auto *runtime_unit =
        std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
    if (!runtime_unit)
      throw std::invalid_argument(
          "capability-provider endpoint source kind and value differ");
    selected.insert(*runtime_unit);
  }
  if (selected.size() != 1U)
    throw std::invalid_argument(
        "capability provider has no single exact runtime-unit owner");
  return *selected.begin();
}

bool target_observation_source_is_current(
    const core::RuntimeParticipantBinding &read_binding,
    const core::ObservedStateSnapshotDTO &sample,
    const core::ActiveControlConfigurationDTO &configuration) {
  const auto &read = read_binding.value();
  if (!read.control_path)
    return false;
  const auto &path = *read.control_path;
  if (configuration.state !=
          core::ActiveControlConfigurationStateDTO::kReconciled ||
      configuration.control_domain_id != path.control_domain_id ||
      configuration.control_domain_revision != path.control_domain_revision ||
      configuration.active_control_configuration_id !=
          path.active_control_configuration_id ||
      configuration.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      configuration.controller_topology_id != path.controller_topology_id ||
      configuration.topology_revision != path.topology_revision ||
      configuration.activation_set_id != path.activation_set_id ||
      sample.controllable_component_id != read.controllable_component_id ||
      sample.command_target_id != read.command_target_id ||
      sample.control_domain_id != path.control_domain_id ||
      sample.control_domain_revision != path.control_domain_revision ||
      sample.active_control_configuration_id !=
          path.active_control_configuration_id ||
      sample.active_control_configuration_generation !=
          path.active_control_configuration_generation ||
      sample.controller_topology_id != path.controller_topology_id ||
      sample.topology_revision != path.topology_revision ||
      sample.control_route_id != path.control_route_id ||
      sample.provider_instance_id != path.provider_instance_id ||
      sample.provider_manifest_id != path.provider_manifest_id ||
      sample.provider_manifest_revision != path.provider_manifest_revision ||
      sample.source_epoch != path.provider_runtime_epoch ||
      sample.state_authority_id != path.state_authority_id ||
      sample.state_authority_class != path.state_authority_class)
    return false;
  const auto has_route = [&](const auto &id, const auto &revision) {
    return std::count(configuration.binding_routes.begin(),
                      configuration.binding_routes.end(),
                      core::ActiveControlConfigurationBindingRouteDTO{
                          id, revision, path.control_route_id}) == 1;
  };
  if (!has_route(path.control_binding_id, path.control_binding_revision) ||
      !has_route(sample.control_binding_id, sample.control_binding_revision))
    return false;
  return std::all_of(
      read.resources.begin(), read.resources.end(), [&](const auto &resource) {
        return std::count_if(
                   configuration.kinematic_resource_source_projections.begin(),
                   configuration.kinematic_resource_source_projections.end(),
                   [&](const auto &group) {
                     const auto contains = [&](const auto &id,
                                               const auto &revision) {
                       return std::count(
                                  group.contributing_control_bindings.begin(),
                                  group.contributing_control_bindings.end(),
                                  core::KinematicControlBindingSourceDTO{
                                      id, revision}) == 1;
                     };
                     return group.resource_id == resource.resource_id &&
                            group.provider_instance_id ==
                                sample.provider_instance_id &&
                            group.provider_manifest_id ==
                                sample.provider_manifest_id &&
                            group.provider_manifest_revision ==
                                sample.provider_manifest_revision &&
                            group.state_authority_id ==
                                sample.state_authority_id &&
                            group.state_authority_class ==
                                sample.state_authority_class &&
                            group.publication_source == sample.source_kind &&
                            contains(path.control_binding_id,
                                     path.control_binding_revision) &&
                            contains(sample.control_binding_id,
                                     sample.control_binding_revision);
                   }) == 1;
      });
}

ProductBackedRuntimeParticipantResolver::
    ProductBackedRuntimeParticipantResolver(
        std::filesystem::path package_root,
        std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
        deployment::VerifiedRetainedModelMap retained_model_map,
        std::vector<core::RuntimeParticipantBinding> physical_bindings,
        std::vector<core::ProgramBindingId> intrinsic_direct_bindings,
        std::filesystem::path program_artifact_root)
    : package_root_(package_root), programs_(package_root),
      bindings_(std::move(package_root)),
      artifact_programs_(
          program_artifact_root.empty()
              ? nullptr
              : std::make_unique<deployment::FilesystemVerifiedProgramSource>(
                    program_artifact_root)),
      artifact_bindings_(
          program_artifact_root.empty()
              ? nullptr
              : std::make_unique<
                    deployment::FilesystemVerifiedProgramBindingSource>(
                    std::move(program_artifact_root))),
      retained_bundle_(std::move(retained_bundle)),
      retained_model_map_(std::move(retained_model_map)),
      physical_bindings_(std::move(physical_bindings)),
      intrinsic_direct_bindings_(std::move(intrinsic_direct_bindings)) {
  ValidateComposition();
}

ProductBackedRuntimeParticipantResolver::
    ProductBackedRuntimeParticipantResolver(
        const ProductBackedRuntimeParticipantResolver &source,
        std::vector<core::RuntimeParticipantBinding> current,
        std::vector<core::ProgramBindingId> direct_bindings)
    : package_root_(source.package_root_), programs_(source.programs_),
      bindings_(source.bindings_),
      artifact_programs_(
          source.artifact_programs_
              ? std::make_unique<deployment::FilesystemVerifiedProgramSource>(
                    *source.artifact_programs_)
              : nullptr),
      artifact_bindings_(
          source.artifact_bindings_
              ? std::make_unique<
                    deployment::FilesystemVerifiedProgramBindingSource>(
                    *source.artifact_bindings_)
              : nullptr),
      retained_bundle_(source.retained_bundle_),
      retained_model_map_(source.retained_model_map_),
      physical_bindings_(std::move(current)),
      intrinsic_direct_bindings_(std::move(direct_bindings)) {
  ValidateComposition();
}

core::Result<deployment::CanonicalValue>
ProductBackedRuntimeParticipantResolver::ReadQualifiedStopResponse(
    const core::RuntimeParticipantBinding &binding,
    const std::span<const core::RuntimeParticipantBinding> current_physical)
    const {
  using Reply = core::Result<deployment::CanonicalValue>;
  const auto &value = binding.value();
  if (!value.control_path ||
      value.retained_bundle_id != retained_bundle_->retained_bundle_id() ||
      value.graph_revision !=
          retained_bundle_->value().cell_graph.graph_revision ||
      std::none_of(current_physical.begin(), current_physical.end(),
                   [&](const auto &actual) {
                     const auto &installed = actual.value();
                     return installed.controllable_component_id ==
                                value.controllable_component_id &&
                            installed.command_target_id ==
                                value.command_target_id &&
                            installed.capability_id == value.capability_id &&
                            installed.control_path == value.control_path &&
                            installed.resources == value.resources;
                   }))
    return Reply::Err({"invalid_request",
                       "Stop metadata is outside the exact composed binding",
                       {}});
  try {
    const deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(
        package_root_);
    const auto &path = *value.control_path;
    return deployment::read_qualified_io_stop_response_metadata(
        *retained_bundle_, retained_model_map_, path.control_binding_id,
        path.control_binding_revision, path.control_route_id, artifacts);
  } catch (const std::exception &error) {
    return Reply::Err({"stop_metadata_unavailable", error.what(), {}});
  }
}

std::shared_ptr<ProductBackedRuntimeParticipantResolver>
ProductBackedRuntimeParticipantResolver::WithComposedParticipants(
    std::vector<core::RuntimeParticipantBinding> current,
    std::vector<core::ProgramBindingId> additional_intrinsic_direct_bindings)
    const {
  auto direct = intrinsic_direct_bindings_;
  direct.insert(direct.end(), additional_intrinsic_direct_bindings.begin(),
                additional_intrinsic_direct_bindings.end());
  std::sort(direct.begin(), direct.end());
  direct.erase(std::unique(direct.begin(), direct.end()), direct.end());
  return std::shared_ptr<ProductBackedRuntimeParticipantResolver>(
      new ProductBackedRuntimeParticipantResolver(*this, std::move(current),
                                                  std::move(direct)));
}

void ProductBackedRuntimeParticipantResolver::ValidateComposition() {
  const bool provider_only =
      retained_bundle_ &&
      !retained_bundle_->value().cell_graph.control_domains.empty() &&
      retained_bundle_->value().cell_graph.control_bindings.empty() &&
      std::all_of(
          retained_bundle_->value().cell_graph.control_domains.begin(),
          retained_bundle_->value().cell_graph.control_domains.end(),
          [](const auto &domain) { return domain.provider() != nullptr; });
  if (retained_bundle_ == nullptr ||
      (physical_bindings_.empty() && !provider_only) ||
      retained_model_map_.query().retained_bundle_id !=
          retained_bundle_->retained_bundle_id() ||
      std::any_of(
          physical_bindings_.begin(), physical_bindings_.end(),
          [&](const auto &binding) {
            return binding.value().retained_bundle_id !=
                       retained_bundle_->retained_bundle_id() ||
                   binding.value().graph_revision !=
                       retained_bundle_->value().cell_graph.graph_revision;
          })) {
    throw std::invalid_argument(
        "product-backed participant resolver inputs do not share one active "
        "runtime product");
  }
  std::sort(intrinsic_direct_bindings_.begin(),
            intrinsic_direct_bindings_.end());
  if (std::adjacent_find(intrinsic_direct_bindings_.begin(),
                         intrinsic_direct_bindings_.end()) !=
          intrinsic_direct_bindings_.end() ||
      std::any_of(intrinsic_direct_bindings_.begin(),
                  intrinsic_direct_bindings_.end(), [&](const auto &id) {
                    return std::none_of(
                        physical_bindings_.begin(), physical_bindings_.end(),
                        [&](const auto &binding) {
                          return binding.value().program_binding_id == id;
                        });
                  })) {
    throw std::invalid_argument(
        "compiler-direct bindings must identify exact composed participants");
  }

  std::vector<collision::RetainedComponentCollisionBodyProjectionInput>
      semantic_bodies;
  for (const auto &role : retained_model_map_.model_collision_bodies()) {
    if (!role.owner_component_id.has_value()) {
      throw std::invalid_argument(
          "passive-model collision geometry has no runtime body alias");
    }
    for (std::size_t index = 0U; index < role.compiled_link_names.size();
         ++index) {
      semantic_bodies.push_back({
          *role.owner_component_id,
          role.model_instance_id,
          role.model_revision,
          role.collision_body_role_id,
          index,
          role.compiled_link_names[index],
      });
    }
  }
  if (semantic_bodies.empty() && provider_only &&
      std::all_of(physical_bindings_.begin(), physical_bindings_.end(),
                  [](const auto &binding) {
                    return binding.value().io_output_binding.has_value();
                  })) {
    // A genuine model-free provider has no collision-body aliases. Physical
    // resolution still requires the original nonempty canonical table.
    return;
  }
  const auto canonical =
      canonicalize_retained_component_bodies(semantic_bodies);
  collision_body_aliases_.reserve(canonical.aliases.size());
  for (const auto &[semantic, physical] : canonical.aliases) {
    collision_body_aliases_.push_back({semantic, physical});
  }
}

std::shared_ptr<const deployment::ProgramBinding>
ProductBackedRuntimeParticipantResolver::LoadProgramBinding(
    const core::ProgramBindingId &program_binding_id) const {
  if (artifact_bindings_) {
    auto binding = artifact_bindings_->get_verified(
        program_binding_id, *retained_bundle_, retained_model_map_);
    if (binding)
      return binding;
  }
  return bindings_.get_verified(program_binding_id, *retained_bundle_,
                                retained_model_map_);
}

core::Result<core::RuntimeParticipantBinding>
ProductBackedRuntimeParticipantResolver::BindCollisionBodyAliases(
    core::RuntimeParticipantBinding binding) const {
  return core::bind_runtime_participant_collision_body_aliases(
      std::move(binding), collision_body_aliases_);
}

std::set<core::ControllableComponentId>
ProductBackedRuntimeParticipantResolver::SelfAndMounted(
    const core::ControllableComponentId &robot) const {
  return retained_model_map_.kinematic_index().self_and_mounted(robot);
}

core::Result<std::vector<core::CollisionBodyId>>
ProductBackedRuntimeParticipantResolver::ActiveTcpContactBodies(
    const core::RuntimeParticipantBinding &robot_binding,
    const std::optional<RobotTcp> &active) const {
  using Reply = core::Result<std::vector<core::CollisionBodyId>>;
  if (!active)
    return Reply::Err({"io_source_unavailable", "Active TCP is unavailable",
                       {{"reason", "active_tcp_unavailable"}}});
  auto value = robot_binding.value();
  const auto robot = value.controllable_component_id;
  const std::array<RobotTcp, 1> rows{*active};
  auto endpoint = ResolveRobotTcp(
      core::TcpSelectorDTO{core::CurrentTcpDTO{}}, robot, rows, active->entry,
      [](const core::TargetRoleId &) { return std::optional<core::TcpEntryDTO>{}; },
      [&](const core::TcpEntryDTO &) {
        return std::pair{value.component_role_id, value.target_role_id};
      });
  if (!endpoint.ok()) return Reply::Err(endpoint.error());
  value.motion_endpoint = std::move(endpoint.value());
  auto binding = core::verify_runtime_participant_binding(std::move(value));
  if (!binding.ok()) return Reply::Err(binding.error());
  auto projected = BindCollisionBodyAliases(std::move(binding.value()));
  if (!projected.ok()) return Reply::Err(projected.error());
  std::vector<core::CollisionBodyId> bodies;
  for (const auto &body : projected.value().value().motion_endpoint->contact_body_bindings)
    bodies.insert(bodies.end(), body.collision_body_ids.begin(), body.collision_body_ids.end());
  return Reply::Ok(std::move(bodies));
}

std::shared_ptr<const deployment::ProgramBindingContract>
ProductBackedRuntimeParticipantResolver::LoadProgram(
    const core::ProgramId &program_id,
    const core::ProgramRevision &program_revision) const {
  if (artifact_programs_) {
    auto program = artifact_programs_->get_verified(
        program_id, program_revision, *retained_bundle_);
    if (program)
      return program;
  }
  return programs_.get_verified(program_id, program_revision,
                                *retained_bundle_);
}

core::Result<core::FrameRoleId>
ProductBackedRuntimeParticipantResolver::ResolveAttachActiveTcp(
    const core::ProgramBindingId &binding_id, const core::TargetRoleId &holder_role,
    const std::function<std::optional<RobotTcp>(const core::ControllableComponentId &)> &read) const {
  using Reply = core::Result<core::FrameRoleId>;
  const auto binding = LoadProgramBinding(binding_id);
  if (!binding)
    return Reply::Err({"attach_holder_unavailable", "Applied holder binding is unavailable", {}});
  const auto &targets = binding->value().target_role_bindings;
  const auto holder = std::find_if(targets.begin(), targets.end(), [&](const auto &row) {
    return row.target_role_id == holder_role;
  });
  if (holder == targets.end())
    return Reply::Err({"attach_holder_unavailable", "Applied holder role is unavailable", {}});
  const auto &index = retained_model_map_.kinematic_index();
  auto ancestors = index.ancestors_of(holder->controllable_component_id);
  ancestors.insert(ancestors.begin(), holder->controllable_component_id);
  for (const auto &robot : ancestors) {
    const auto motion = std::find_if(physical_bindings_.begin(), physical_bindings_.end(),
        [&](const auto &candidate) {
          return candidate.value().controllable_component_id == robot &&
                 candidate.value().capability_id.value() == "execution.trajectory.execute";
        });
    if (motion == physical_bindings_.end()) continue;
    const auto active = read(robot);
    if (!active)
      return Reply::Err({"attach_active_tcp_unavailable", "The holder's robot has no Active TCP", {}});
    if (!index.carries(robot, holder->controllable_component_id) ||
        active->entry.component_id != holder->controllable_component_id ||
        active->entry.target_id != holder->command_target_id)
      return Reply::Err({"attach_active_tcp_not_holder", "Active TCP does not belong to the holder", {}});
    const auto *target = retained_model_map_.find_target(
        holder->controllable_component_id, holder->command_target_id);
    if (!target || std::none_of(target->frames.begin(), target->frames.end(), [&](const auto &frame) {
          const auto *model = std::get_if<deployment::CompositionComponentModelFrame>(&frame.frame);
          return frame.frame_role_id == active->entry.frame_role_id && model &&
                 model->controllable_component_id == holder->controllable_component_id &&
                 model->model_instance_id == active->frame.model_instance_id &&
                 model->model_revision == active->frame.model_revision &&
                 model->compiled_frame_name == active->frame.compiled_frame_name;
        }))
      return Reply::Err({"attach_active_tcp_not_holder", "Active TCP frame is not a holder frame", {}});
    return Reply::Ok(active->entry.frame_role_id);
  }
  return Reply::Err({"attach_active_tcp_unavailable", "The holder has no carrying robot", {}});
}

bool ProductBackedRuntimeParticipantResolver::FrameObservationCovered(
    const core::ProgramFrameRoleBindingDTO &frame,
    const core::RuntimeParticipantBinding &observed_binding) const {
  const auto &binding = observed_binding.value();
  const auto *target = retained_model_map_.find_target(
      binding.controllable_component_id, binding.command_target_id);
  if (!target)
    return false;
  const auto selected = std::find_if(
      target->frames.begin(), target->frames.end(), [&](const auto &row) {
        return row.frame_role_id == frame.frame_role_id;
      });
  if (selected == target->frames.end())
    return false;
  const auto *terminal =
      std::get_if<deployment::CompositionComponentModelFrame>(&selected->frame);
  if (!terminal || terminal->model_instance_id != frame.model_instance_id ||
      terminal->model_revision != frame.model_revision ||
      terminal->compiled_frame_name != frame.compiled_frame_name)
    return false;
  const auto *path = retained_model_map_.kinematic_index().find_frame_path(
      core::ResolvedComponentModelFrameDTO{
          terminal->controllable_component_id, terminal->model_instance_id,
          terminal->frame_role_id, terminal->compiled_frame_name,
          terminal->model_revision});
  if (path == nullptr || !binding.control_path.has_value())
    return false;
  for (const auto &segment : path->segments) {
    const auto *joint =
        std::get_if<core::KinematicModelJointPathSegmentDTO>(&segment);
    if (joint == nullptr ||
        joint->coordinate_kind == core::KinematicJointCoordinateKind::kFixed)
      continue;
    const auto &control = *binding.control_path;
    if (std::none_of(joint->eligible_resource_dependencies.begin(),
                     joint->eligible_resource_dependencies.end(),
                     [&](const auto &dependency) {
                       return dependency.control_domain_id ==
                                  control.control_domain_id &&
                              dependency.control_domain_revision ==
                                  control.control_domain_revision &&
                              dependency.control_binding_id ==
                                  control.control_binding_id &&
                              dependency.control_binding_revision ==
                                  control.control_binding_revision &&
                              std::any_of(
                                  binding.resources.begin(),
                                  binding.resources.end(),
                                  [&](const auto &resource) {
                                    return resource.resource_id ==
                                               dependency.resource_id &&
                                           resource.coordinate_kind ==
                                               dependency.coordinate_kind;
                                  });
                     }))
      return false;
  }
  return true;
}

core::Result<deployment::RuntimeProgramTargetBinding>
ProductBackedRuntimeParticipantResolver::ResolveBoundTarget(
    const deployment::ProgramBinding &binding,
    const core::TargetRoleId &target_role_id,
    const core::CapabilityId &capability_id) const {
  try {
    return core::Result<deployment::RuntimeProgramTargetBinding>::Ok(
        deployment::resolve_runtime_program_target(
            binding, target_role_id, capability_id, *retained_bundle_,
            retained_model_map_));
  } catch (const std::exception &exception) {
    return Reject<deployment::RuntimeProgramTargetBinding>(
        std::string("exact bound target resolution failed: ") +
            exception.what(),
        "runtime_bound_target_resolution_failed");
  }
}

core::Result<ResolvedTargetObservation>
ProductBackedRuntimeParticipantResolver::ResolveObservationTarget(
    const core::ProgramBindingId &program_binding_id,
    const core::TargetRoleId &target_role_id,
    const core::CapabilityId &capability_id,
    const core::RuntimeParticipantBinding &physical,
    const std::span<const core::RuntimeParticipantBinding>
        current_representatives) const {
  using Target = ResolvedTargetObservation;
  try {
    if (capability_id.value() != "model.kinematics.fk" &&
        capability_id.value() != "tool.gripper.feedback") {
      throw std::invalid_argument("unsupported observation capability");
    }
    const bool frame_query = capability_id.value() == "model.kinematics.fk";
    auto selection = observation_selection_.load();
    if (!selection ||
        selection->target.program_binding_id() != program_binding_id ||
        selection->target.target_role().target_role_id != target_role_id ||
        selection->target.capability().capability_id != capability_id) {
      auto binding = selection && selection->binding->program_binding_id() ==
                                      program_binding_id
                         ? selection->binding
                         : nullptr;
      if (!binding) {
        binding = LoadProgramBinding(program_binding_id);
        if (!binding) {
          throw std::invalid_argument("no exact retained Program Binding");
        }
        const auto program = LoadProgram(binding->value().program_id,
                                         binding->value().program_revision);
        if (!program || program->value().node_environment !=
                            binding->value().node_environment) {
          throw std::invalid_argument("no exact canonical Program");
        }
      }
      auto target = deployment::resolve_runtime_program_target(
          *binding, target_role_id, capability_id, *retained_bundle_,
          retained_model_map_);
      auto state_target =
          frame_query
              ? std::optional{deployment::resolve_runtime_program_target(
                    *binding, target_role_id,
                    core::CapabilityId("state.observed.current"),
                    *retained_bundle_, retained_model_map_)}
              : std::nullopt;
      selection = std::make_shared<const ObservationSelection>(
          std::move(binding), std::move(target), std::move(state_target));
      observation_selection_.store(selection);
    }
    const auto &target = selection->target;
    const auto &read_target =
        selection->frame_state_target ? *selection->frame_state_target : target;
    if (!read_target.control_binding())
      throw std::invalid_argument("observation has no exact ControlBinding");
    const auto &selected = *read_target.control_binding();
    const auto representative = std::find_if(
        current_representatives.begin(), current_representatives.end(),
        [&](const auto &row) {
          const auto &path = row.value().control_path;
          return path &&
                 path->control_binding_id == selected.control_binding_id &&
                 path->control_binding_revision ==
                     selected.control_binding_revision &&
                 row.value().capability_id == selected.capability_id &&
                 row.value().capability_definition_revision ==
                     selected.capability_definition_revision;
        });
    if (representative == current_representatives.end() ||
        !retained_bindings_share_backend_owner(physical, *representative))
      throw std::invalid_argument(
          "selected observation has no current native realization");
    // The sealed read capability selects this exact retained CB. Its existing
    // native representative already owns the full coordinate realization.
    // A read-only Program need not declare execution resource roles or build
    // an operation participant merely to query its declared frame/feedback.
    if (frame_query) {
      // The native FK adapter is realized by the same exact provider occurrence
      // as this runtime's retained planning adapter, not any target-wide claim.
      const auto &provider = target.capability_provider_binding();
      if (!provider)
        throw std::invalid_argument("selected FK has no retained provider");
      const auto runtime_unit = retained_provider_runtime_unit(*provider);
      const auto *anchor =
          std::get_if<core::TargetBindingCapabilityProviderTargetDTO>(
              &provider->target);
      if (!anchor ||
          std::none_of(
              current_representatives.begin(), current_representatives.end(),
              [&](const auto &row) {
                const auto &path = row.value().control_path;
                return path &&
                       path->control_binding_id == anchor->control_binding_id &&
                       path->control_binding_revision ==
                           anchor->control_binding_revision &&
                       retained_bindings_share_backend_owner(physical, row);
              }) ||
          std::none_of(
              physical.value().supporting_capability_provider_paths.begin(),
              physical.value().supporting_capability_provider_paths.end(),
              [&](const auto &path) {
                return path.provider_instance_id ==
                           provider->provider_instance_id &&
                       path.provider_manifest_id ==
                           provider->provider_manifest_id &&
                       path.provider_manifest_revision ==
                           provider->provider_manifest_revision &&
                       path.provider_runtime_unit_id == runtime_unit &&
                       std::any_of(physical.value().runtime_unit_epochs.begin(),
                                   physical.value().runtime_unit_epochs.end(),
                                   [&](const auto &epoch) {
                                     return epoch.runtime_unit_id ==
                                                path.provider_runtime_unit_id &&
                                            epoch.runtime_epoch ==
                                                path.provider_runtime_epoch;
                                   });
              }))
        throw std::invalid_argument(
            "selected FK provider has no current native realization");
    }
    return core::Result<Target>::Ok({target, *representative});
  } catch (const std::exception &exception) {
    return core::Result<Target>::Err(core::ErrorDTO{
        "execution_target_binding_unavailable", exception.what(), {}});
  }
}

core::Result<core::ProgramRunExecutionCaptureDTO>
ProductBackedRuntimeParticipantResolver::ResolveProgramCapture(
    core::ProgramRunExecutionCaptureDTO capture) const {
  try {
    const auto request =
        detail::ValidateSealedProgramCaptureRequest(capture, *retained_bundle_);
    if (!request.ok())
      return core::Result<core::ProgramRunExecutionCaptureDTO>::Err(
          request.error());
    const auto binding = LoadProgramBinding(capture.program_binding_id);
    const auto selected_binding =
        detail::ValidateSealedProgramCaptureBinding(capture, binding.get());
    if (!selected_binding.ok())
      return core::Result<core::ProgramRunExecutionCaptureDTO>::Err(
          selected_binding.error());
    const auto program =
        LoadProgram(capture.program_id, capture.program_revision);
    return detail::ResolveSealedProgramCapture(std::move(capture), program,
                                               binding, *retained_bundle_);
  } catch (const std::exception &exception) {
    return Reject<core::ProgramRunExecutionCaptureDTO>(
        std::string("Program capture product resolution failed: ") +
            exception.what(),
        "program_capture_resolution_failed");
  }
}

namespace {

struct HostRobotTcpOwners final {
  std::mutex mutex;
  HostRobotTcpReaders owners;
};

[[nodiscard]] HostRobotTcpOwners &host_robot_tcp_owners() {
  static HostRobotTcpOwners owners;
  return owners;
}

}  // namespace

void SelectHostRobotTcps(HostRobotTcpReaders readers,
                         const std::function<void()> &select_composition) {
  auto &owners = host_robot_tcp_owners();
  std::lock_guard lock(owners.mutex);
  select_composition();
  owners.owners.swap(readers);
}

std::optional<ActiveTcp> ReadHostRobotTcps(const core::ControllableComponentId &robot) {
  std::function<std::optional<ActiveTcp>()> read;
  {
    auto &owners = host_robot_tcp_owners();
    std::lock_guard lock(owners.mutex);
    const auto found = owners.owners.find(robot);
    if (found == owners.owners.end()) return std::nullopt;
    read = found->second;
  }
  // The owner's own lock is taken outside the registry's.
  return read();
}

core::Result<core::RuntimeMotionEndpointBindingDTO>
ProductBackedRuntimeParticipantResolver::ResolveRobotTcp(
    const core::TcpSelectorDTO &selector,
    const core::ControllableComponentId &robot, std::span<const RobotTcp> rows,
    const std::optional<core::TcpEntryDTO> &active,
    const std::function<std::optional<core::TcpEntryDTO>(const core::TargetRoleId &)>
        &default_tcp_of_role,
    const std::function<std::pair<core::ComponentRoleId, core::TargetRoleId>(
        const core::TcpEntryDTO &)> &role_of,
    const bool joint_goal) const {
  using Endpoint = core::RuntimeMotionEndpointBindingDTO;
  // C-P2: Current with no Active TCP, or form 2 with no default TCP.
  const auto no_tcp = [&](const std::optional<core::TargetRoleId> &role) {
    core::ErrorDTO error{std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
                         "no end-effector TCP; choose one",
                         {{"reason", "end_effector_tcp_unavailable"}, {"robot", robot.value()}}};
    if (role) error.details.emplace("role", role->value());
    return core::Result<Endpoint>::Err(std::move(error));
  };
  std::optional<core::TcpEntryDTO> entry;
  if (std::holds_alternative<core::CurrentTcpDTO>(selector)) {
    entry = active;
    // B-run-1: a joint goal needs no TCP; with none it runs at the robot's
    // own flange. A Cartesian goal still refuses (C-P2).
    if (!entry && joint_goal) {
      const auto flange = std::find_if(rows.begin(), rows.end(), [&](const RobotTcp &row) {
        return row.entry.component_id == robot &&
               row.entry.frame_role_id == core::FrameRoleId("flange");
      });
      if (flange != rows.end()) entry = flange->entry;
    }
    if (!entry) return no_tcp(std::nullopt);
  } else if (const auto *form2 = std::get_if<core::DefaultTcpOfRoleDTO>(&selector)) {
    entry = default_tcp_of_role(form2->role);
    if (!entry) return no_tcp(form2->role);
  } else {
    entry = std::get<core::TcpEntryDTO>(selector);
  }
  // A1-1: the entry must be a TCP of a component in the robot's list.
  const auto row = std::find_if(rows.begin(), rows.end(),
                                [&](const RobotTcp &candidate) { return candidate.entry == *entry; });
  if (row == rows.end())
    return core::Result<Endpoint>::Err(core::active_tcp_unknown_error(robot, *entry));
  // T-MQ7: its owner must be carried by the robot (the robot itself or a
  // tool mounted on it).
  if (!retained_model_map_.kinematic_index().carries(robot, entry->component_id))
    return core::Result<Endpoint>::Err(core::tcp_tool_not_mounted_error(robot, *entry));
  const auto [component_role, target_role] = role_of(*entry);
  Endpoint endpoint{target_role, entry->target_id, row->frame, {}};
  // T-C1: the contact bodies are the owner's, from the exact model map.
  const auto &bodies = retained_model_map_.model_collision_bodies();
  for (const auto &role : row->frame.contact_body_role_ids) {
    const auto body = std::find_if(bodies.begin(), bodies.end(), [&](const auto &candidate) {
      return candidate.owner_component_id == entry->component_id &&
             candidate.model_instance_id == row->frame.model_instance_id &&
             candidate.model_revision == row->frame.model_revision &&
             candidate.collision_body_role_id == role;
    });
    if (body == bodies.end())
      return Reject<Endpoint>("motion endpoint body is absent from its exact frame model",
                              "runtime_motion_endpoint_body_unresolved",
                              core::ErrorCode::kInvariantViolation);
    std::vector<std::string> ids;
    for (std::size_t index = 0; index < body->compiled_link_names.size(); ++index)
      ids.push_back(core::make_component_collision_body_id(
          entry->component_id, row->frame.model_instance_id, role, index));
    endpoint.contact_body_bindings.push_back(
        {component_role, target_role, entry->component_id, entry->target_id, std::nullopt,
         role, row->frame.model_instance_id, row->frame.model_revision, std::move(ids)});
  }
  return core::Result<Endpoint>::Ok(std::move(endpoint));
}

core::Result<core::RuntimeParticipantBinding>
ProductBackedRuntimeParticipantResolver::Resolve(
    const core::OperationIntentDTO &operation,
    const std::size_t participant_index,
    const RobotTcpReader &supplied_robot_tcps) const {
  const RobotTcpReader robot_tcps =
      supplied_robot_tcps ? supplied_robot_tcps : RobotTcpReader(ReadHostRobotTcps);
  try {
    if (participant_index >= operation.participants.size() ||
        operation.expected_retained_bundle_id !=
            retained_bundle_->retained_bundle_id() ||
        operation.expected_graph_revision !=
            retained_bundle_->value().cell_graph.graph_revision) {
      return Reject<core::RuntimeParticipantBinding>(
          "operation does not name one participant in the active retained "
          "product",
          "runtime_participant_product_mismatch",
          core::ErrorCode::kMissingReference);
    }
    const auto &participant = operation.participants[participant_index];
    if (std::binary_search(intrinsic_direct_bindings_.begin(),
                           intrinsic_direct_bindings_.end(),
                           operation.program_binding_id)) {
      const auto *precomposed = FindExactPrecomposedBinding(
          operation, participant, physical_bindings_);
      if (precomposed == nullptr) {
        return Reject<core::RuntimeParticipantBinding>(
            "direct-control operation differs from its compiled product",
            "direct_control_product_mismatch",
            core::ErrorCode::kInvalidRequest);
      }
      if (precomposed->value().io_output_binding)
        return core::Result<core::RuntimeParticipantBinding>::Ok(*precomposed);
      const auto *motion =
          std::get_if<core::MotionParticipantIntentDTO>(&participant.payload);
      const auto *set_tcp =
          std::get_if<core::SetTcpParticipantIntentDTO>(&participant.payload);
      if (motion != nullptr || set_tcp != nullptr) {
        auto value = precomposed->value();
        // Robot control: the robot's list and Active TCP from its runtime
        // owner; there is no Program role, so form 2 has no default.
        const auto state =
            robot_tcps ? robot_tcps(value.controllable_component_id) : std::nullopt;
        if (!state)
          return Reject<core::RuntimeParticipantBinding>(
              "direct-control robot has no Active TCP owner",
              "runtime_motion_endpoint_unresolved",
              core::ErrorCode::kMissingReference);
        auto endpoint = ResolveRobotTcp(
            motion != nullptr ? motion->tcp_endpoint : set_tcp->tcp,
            value.controllable_component_id, state->rows(),
            state->value() ? std::optional(state->value()->entry) : std::nullopt,
            [](const core::TargetRoleId &) { return std::optional<core::TcpEntryDTO>{}; },
            [&](const core::TcpEntryDTO &) {
              return std::pair{value.component_role_id, value.target_role_id};
            },
            motion != nullptr && std::holds_alternative<core::JointGoalDTO>(motion->motion));
        if (!endpoint.ok())
          return core::Result<core::RuntimeParticipantBinding>::Err(endpoint.error());
        value.motion_endpoint = std::move(endpoint.value());
        auto captured =
            core::verify_runtime_participant_binding(std::move(value));
        if (!captured.ok())
          return captured;
        return core::bind_runtime_participant_collision_body_aliases(
            captured.value(), collision_body_aliases_);
      }
      return core::bind_runtime_participant_collision_body_aliases(
          *precomposed, collision_body_aliases_);
    }

    const auto binding = LoadProgramBinding(operation.program_binding_id);
    if (binding == nullptr ||
        binding->value().program_id != operation.program_id ||
        binding->value().program_revision != operation.program_revision) {
      return Reject<core::RuntimeParticipantBinding>(
          "operation has no exact generated Program Binding in the active "
          "product",
          "runtime_program_binding_unavailable",
          core::ErrorCode::kMissingReference);
    }
    const auto program =
        LoadProgram(operation.program_id, operation.program_revision);
    if (program == nullptr || program->value().node_environment !=
                                  binding->value().node_environment) {
      return Reject<core::RuntimeParticipantBinding>(
          "operation has no exact canonical Program in the active product",
          "runtime_program_unavailable", core::ErrorCode::kMissingReference);
    }

    auto target = deployment::resolve_runtime_program_target(
        *binding, participant.target_role_id,
        participant.required_capability_id, *retained_bundle_,
        retained_model_map_);
    if (const auto *command = std::get_if<core::CapabilityCommandIntentDTO>(
            &participant.payload)) {
      if (const auto *output = std::get_if<core::IoSignalWriteCommandValueDTO>(
              &command->command_value)) {
        const auto &composed = RequireOutputBinding(
            target, output->resource_role_id, physical_bindings_);
        auto selected = core::verify_runtime_participant_binding(
            deployment::build_runtime_io_output_binding_dto(
                target, output->resource_role_id, *retained_bundle_,
                ClosedOutputProvenance(composed, *retained_bundle_)));
        if (!selected.ok())
          return selected;
        if (selected.value().value().io_output_binding !=
                composed.value().io_output_binding ||
            !PhysicallyContainedBy(composed, selected.value()))
          return Reject<core::RuntimeParticipantBinding>(
              "resolved Program output crosses its exact composed channel "
              "owner",
              "runtime_program_output_crosses_composed_owner",
              core::ErrorCode::kInvariantViolation);
        return selected;
      }
    }
    std::vector<deployment::RuntimeProgramTargetBinding> supporting;
    if (const auto *motion = std::get_if<core::MotionParticipantIntentDTO>(
            &participant.payload)) {
      supporting.push_back(deployment::resolve_runtime_program_target(
          *binding, participant.target_role_id,
          RequiredPlannerCapability(*motion), *retained_bundle_,
          retained_model_map_));
    }
    const auto &physical = RequirePhysicalBinding(target, physical_bindings_);
    auto candidate_value = deployment::build_runtime_participant_binding_dto(
        target,
        ActiveFacts(target,
                    std::span<const deployment::RuntimeProgramTargetBinding>(
                        supporting),
                    physical),
        std::span<const deployment::RuntimeProgramTargetBinding>(supporting));
    const auto *motion =
        std::get_if<core::MotionParticipantIntentDTO>(&participant.payload);
    const auto *set_tcp =
        std::get_if<core::SetTcpParticipantIntentDTO>(&participant.payload);
    if (motion != nullptr || set_tcp != nullptr) {
      const auto &targets = binding->value().target_role_bindings;
      const auto find_role = [&](const core::TargetRoleId &role) {
        return std::find_if(targets.begin(), targets.end(),
                            [&](const auto &row) { return row.target_role_id == role; });
      };
      const auto moving = find_role(participant.target_role_id);
      if (moving == targets.end())
        return Reject<core::RuntimeParticipantBinding>(
            "moving target role is absent from its exact Program binding",
            "runtime_motion_endpoint_unresolved", core::ErrorCode::kMissingReference);
      const auto &robot = candidate_value.controllable_component_id;
      const auto state = robot_tcps ? robot_tcps(robot) : std::nullopt;
      // TCP-A1: the moving role's `robot_tcps` (absent: no list, so every
      // entry is refused) and each role's `default_tcp` (design 1.2).
      const auto &rows = moving->robot_tcps;
      auto endpoint = ResolveRobotTcp(
          motion != nullptr ? motion->tcp_endpoint : set_tcp->tcp, robot,
          rows ? std::span<const RobotTcp>(*rows) : std::span<const RobotTcp>(),
          state && state->value() ? std::optional(state->value()->entry) : std::nullopt,
          [&](const core::TargetRoleId &role) {
            const auto row = find_role(role);
            return row == targets.end() ? std::optional<core::TcpEntryDTO>{} : row->default_tcp;
          },
          [&](const core::TcpEntryDTO &entry) {
            // The owner's Program role when it has one, else the moving one.
            const auto owner = std::find_if(targets.begin(), targets.end(), [&](const auto &row) {
              return row.controllable_component_id == entry.component_id &&
                     row.command_target_id == entry.target_id;
            });
            const auto &row = owner == targets.end() ? *moving : *owner;
            return std::pair{row.component_role_id, row.target_role_id};
          },
          motion != nullptr && std::holds_alternative<core::JointGoalDTO>(motion->motion));
      if (!endpoint.ok())
        return core::Result<core::RuntimeParticipantBinding>::Err(endpoint.error());
      candidate_value.motion_endpoint = std::move(endpoint.value());
    }
    auto candidate =
        core::verify_runtime_participant_binding(std::move(candidate_value));
    if (!candidate.ok()) {
      return candidate;
    }
    candidate = core::bind_runtime_participant_collision_body_aliases(
        std::move(candidate.value()), collision_body_aliases_);
    if (!candidate.ok()) {
      return candidate;
    }
    if (!PhysicallyContainedBy(physical, candidate.value())) {
      return Reject<core::RuntimeParticipantBinding>(
          "resolved Program participant widens or crosses its composed "
          "physical runtime owner",
          "runtime_program_binding_crosses_physical_owner",
          core::ErrorCode::kInvariantViolation);
    }
    return candidate;
  } catch (const std::exception &exception) {
    return Reject<core::RuntimeParticipantBinding>(
        std::string("exact product-backed participant resolution failed: ") +
            exception.what(),
        "runtime_participant_product_resolution_failed");
  } catch (...) {
    return Reject<core::RuntimeParticipantBinding>(
        "exact product-backed participant resolution failed",
        "runtime_participant_product_resolution_failed");
  }
}

core::Result<void>
ProductBackedRuntimeParticipantResolver::ValidateExecutionOrigin(
    const core::OperationIntentDTO &operation) const {
  const bool direct = std::binary_search(intrinsic_direct_bindings_.begin(),
                                         intrinsic_direct_bindings_.end(),
                                         operation.program_binding_id);
  for (const auto &participant : operation.participants) {
    if (direct) {
      if (participant.execution_scope.has_value() ||
          FindExactPrecomposedBinding(operation, participant,
                                      physical_bindings_) == nullptr) {
        return Reject<void>(
            "direct-control operation differs from its compiled product",
            "direct_control_product_mismatch",
            core::ErrorCode::kInvalidRequest);
      }
    } else if (!participant.execution_scope.has_value()) {
      return Reject<void>(
          "Program effects require private captured node origin",
          "program_execution_scope_missing", core::ErrorCode::kInvalidRequest);
    }
  }
  return core::Result<void>::Ok();
}

core::Result<RetainedResolvedParticipant>
ProductBackedRuntimeParticipantResolver::ResolveExecutionParticipant(
    const core::OperationIntentDTO &operation,
    const std::size_t participant_index,
    const RobotTcpReader &robot_tcps) const {
  auto binding = Resolve(operation, participant_index, robot_tcps);
  if (!binding.ok())
    return core::Result<RetainedResolvedParticipant>::Err(binding.error());
  // B7: the endpoint frame's owner (the motion component or a tool mounted on
  // it) stamps the Kernel request's TCP frame.
  std::optional<core::ControllableComponentId> endpoint_owner;
  if (const auto &endpoint = binding.value().value().motion_endpoint)
    if (const auto *owner = retained_model_map_.kinematic_index().owner_of(
            endpoint->frame.model_instance_id))
      endpoint_owner = *owner;
  return core::Result<RetainedResolvedParticipant>::Ok({
      std::move(binding.value()),
      std::binary_search(intrinsic_direct_bindings_.begin(),
                         intrinsic_direct_bindings_.end(),
                         operation.program_binding_id),
      std::move(endpoint_owner),
  });
}

} // namespace manyforge::roboplan_backend
