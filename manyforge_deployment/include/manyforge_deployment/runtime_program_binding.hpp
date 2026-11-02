#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/runtime_route_configuration.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/program_binding.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace manyforge::deployment {

class RuntimeProgramBindingResolutionError final
    : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// Sealed runtime resolution of one Program target/capability.
///
/// Construction proves that the exact Program Binding, retained bundle,
/// graph, retained model map, desired route, and optional provider binding
/// agree. Active runtime configuration and epochs remain later owner facts.
class RuntimeProgramTargetBinding final {
public:
  ~RuntimeProgramTargetBinding() = default;
  RuntimeProgramTargetBinding(const RuntimeProgramTargetBinding &) = default;
  RuntimeProgramTargetBinding &
  operator=(const RuntimeProgramTargetBinding &) = default;
  RuntimeProgramTargetBinding(RuntimeProgramTargetBinding &&) noexcept =
      default;
  RuntimeProgramTargetBinding &
  operator=(RuntimeProgramTargetBinding &&) noexcept = default;

  [[nodiscard]] const core::ProgramId &program_id() const noexcept {
    return program_id_;
  }
  [[nodiscard]] const core::ProgramRevision &program_revision() const noexcept {
    return program_revision_;
  }
  [[nodiscard]] const core::ProgramBindingId &
  program_binding_id() const noexcept {
    return program_binding_id_;
  }
  [[nodiscard]] const core::RetainedBundleId &
  retained_bundle_id() const noexcept {
    return retained_bundle_id_;
  }
  [[nodiscard]] const core::GraphRevision &graph_revision() const noexcept {
    return graph_revision_;
  }
  [[nodiscard]] const core::ProgramComponentRoleBindingDTO &
  component_role() const noexcept {
    return component_role_;
  }
  [[nodiscard]] const core::ProgramTargetRoleBindingDTO &
  target_role() const noexcept {
    return target_role_;
  }
  [[nodiscard]] const core::ProgramCapabilityBindingDTO &
  capability() const noexcept {
    return capability_;
  }
  [[nodiscard]] const core::ControllableComponentDTO &
  component() const noexcept {
    return component_;
  }
  [[nodiscard]] const CompositionTargetResolution &
  model_target() const noexcept {
    return model_target_;
  }
  [[nodiscard]] const std::optional<core::ControlBindingDTO> &
  control_binding() const noexcept {
    return control_binding_;
  }
  [[nodiscard]] const std::optional<core::ControlDomainDTO> &
  control_domain() const noexcept {
    return control_domain_;
  }
  [[nodiscard]] const std::optional<core::DesiredControlConfigurationDTO> &
  desired_control_configuration() const noexcept {
    return desired_control_configuration_;
  }
  [[nodiscard]] const std::optional<core::ControlRouteId> &
  desired_route() const noexcept {
    return desired_route_;
  }
  [[nodiscard]] const std::optional<CompositionControlBindingCoordinateMap> &
  coordinate_map() const noexcept {
    return coordinate_map_;
  }
  [[nodiscard]] const std::optional<core::CapabilityProviderBindingDTO> &
  capability_provider_binding() const noexcept {
    return capability_provider_binding_;
  }
  [[nodiscard]] const std::vector<core::ProgramContactGraspBindingDTO> &
  contact_grasp_bindings() const noexcept {
    return contact_grasp_bindings_;
  }

private:
  RuntimeProgramTargetBinding(
      core::ProgramId program_id, core::ProgramRevision program_revision,
      core::ProgramBindingId program_binding_id,
      core::RetainedBundleId retained_bundle_id,
      core::GraphRevision graph_revision,
      core::ProgramComponentRoleBindingDTO component_role,
      core::ProgramTargetRoleBindingDTO target_role,
      core::ProgramCapabilityBindingDTO capability,
      core::ControllableComponentDTO component,
      CompositionTargetResolution model_target,
      std::optional<core::ControlBindingDTO> control_binding,
      std::optional<core::ControlDomainDTO> control_domain,
      std::optional<core::DesiredControlConfigurationDTO>
          desired_control_configuration,
      std::optional<core::ControlRouteId> desired_route,
      std::optional<CompositionControlBindingCoordinateMap> coordinate_map,
      std::optional<core::CapabilityProviderBindingDTO>
          capability_provider_binding,
      std::vector<core::ProgramContactGraspBindingDTO> contact_grasp_bindings)
      : program_id_(std::move(program_id)),
        program_revision_(std::move(program_revision)),
        program_binding_id_(std::move(program_binding_id)),
        retained_bundle_id_(std::move(retained_bundle_id)),
        graph_revision_(std::move(graph_revision)),
        component_role_(std::move(component_role)),
        target_role_(std::move(target_role)),
        capability_(std::move(capability)), component_(std::move(component)),
        model_target_(std::move(model_target)),
        control_binding_(std::move(control_binding)),
        control_domain_(std::move(control_domain)),
        desired_control_configuration_(
            std::move(desired_control_configuration)),
        desired_route_(std::move(desired_route)),
        coordinate_map_(std::move(coordinate_map)),
        capability_provider_binding_(std::move(capability_provider_binding)),
        contact_grasp_bindings_(std::move(contact_grasp_bindings)) {}

  core::ProgramId program_id_;
  core::ProgramRevision program_revision_;
  core::ProgramBindingId program_binding_id_;
  core::RetainedBundleId retained_bundle_id_;
  core::GraphRevision graph_revision_;
  core::ProgramComponentRoleBindingDTO component_role_;
  core::ProgramTargetRoleBindingDTO target_role_;
  core::ProgramCapabilityBindingDTO capability_;
  core::ControllableComponentDTO component_;
  CompositionTargetResolution model_target_;
  std::optional<core::ControlBindingDTO> control_binding_;
  std::optional<core::ControlDomainDTO> control_domain_;
  std::optional<core::DesiredControlConfigurationDTO>
      desired_control_configuration_;
  std::optional<core::ControlRouteId> desired_route_;
  std::optional<CompositionControlBindingCoordinateMap> coordinate_map_;
  std::optional<core::CapabilityProviderBindingDTO>
      capability_provider_binding_;
  std::vector<core::ProgramContactGraspBindingDTO> contact_grasp_bindings_;

  friend RuntimeProgramTargetBinding resolve_runtime_program_target(
      const ProgramBinding &, const core::TargetRoleId &,
      const core::CapabilityId &, const RetainedBundle &,
      const VerifiedRetainedModelMap &);
};

struct RuntimeProviderEpochFactDTO final {
  core::ProviderInstanceId provider_instance_id;
  core::RuntimeUnitId provider_runtime_unit_id;
  core::RuntimeEpoch runtime_epoch;

  friend bool operator==(const RuntimeProviderEpochFactDTO &,
                         const RuntimeProviderEpochFactDTO &) = default;
};

/// Current runtime-owner facts supplied after Component Manager convergence.
/// Retained desired configuration is deliberately not active truth.
struct ActiveRuntimeParticipantFactsDTO final {
  core::ControlRouteId control_route_id;
  core::ActiveControlConfigurationId active_control_configuration_id;
  core::ActiveControlConfigurationGeneration
      active_control_configuration_generation;
  std::vector<core::RuntimeUnitEpochBindingDTO> runtime_unit_epochs;
  std::vector<RuntimeProviderEpochFactDTO> provider_epochs;
};

/// Pure exact-point resolution used when constructing one runtime participant.
/// It performs no active-bundle lookup, route mutation, process discovery,
/// scene access, or hardware I/O.
[[nodiscard]] RuntimeProgramTargetBinding resolve_runtime_program_target(
    const ProgramBinding &program_binding,
    const core::TargetRoleId &target_role_id,
    const core::CapabilityId &capability_id,
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map);

/// Combine one sealed retained target with current owner-issued runtime facts.
/// I6 admits the retained desired route; generic route switching starts at I7.
/// The Kernel independently verifies and seals the returned owner-to-owner DTO.
[[nodiscard]] core::RuntimeParticipantBindingDTO
build_runtime_participant_binding_dto(
    const RuntimeProgramTargetBinding &target,
    const ActiveRuntimeParticipantFactsDTO &active_facts,
    std::span<const RuntimeProgramTargetBinding> supporting_capabilities = {});

} // namespace manyforge::deployment
