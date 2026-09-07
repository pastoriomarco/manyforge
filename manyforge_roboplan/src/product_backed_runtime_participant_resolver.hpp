#pragma once

#include <filesystem>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_core/types/operation_intent.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <memory>
#include <span>
#include <vector>

namespace manyforge::roboplan_backend {

/// Native providers must resolve to one exact managed runtime-unit owner.
[[nodiscard]] core::RuntimeUnitId retained_provider_runtime_unit(
    const core::CapabilityProviderBindingDTO &binding);

struct ResolvedTargetObservation final {
  deployment::RuntimeProgramTargetBinding target;
  core::RuntimeParticipantBinding observation_binding;
};

/// Check a capability-specific read against its current shared source group.
/// The sample keeps its actual publication binding; it is never relabelled.
[[nodiscard]] bool target_observation_source_is_current(
    const core::RuntimeParticipantBinding &read_binding,
    const core::ObservedStateSnapshotDTO &sample,
    const core::ActiveControlConfigurationDTO &configuration);

/// Exact point resolver used by the native execution owner.
///
/// Programs and generated Program Bindings are loaded by the identities in
/// each bounded operation request. The resolved semantic binding must remain
/// inside one of the already-composed physical participant inventories; this
/// object performs no repository enumeration, latest selection, or backend
/// discovery.
class ProductBackedRuntimeParticipantResolver final {
public:
  ProductBackedRuntimeParticipantResolver(
      std::filesystem::path package_root,
      std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
      deployment::VerifiedRetainedModelMap retained_model_map,
      std::vector<core::RuntimeParticipantBinding> physical_bindings);

  [[nodiscard]] core::Result<core::RuntimeParticipantBinding>
  Resolve(const core::OperationIntentDTO &operation,
          std::size_t participant_index) const;

  /// Resolve a read against the same immutable product as admission. This
  /// neither creates an OperationIntent nor grants execution authority.
  [[nodiscard]] core::Result<ResolvedTargetObservation>
  ResolveObservationTarget(const core::ProgramBindingId &program_binding_id,
                           const core::TargetRoleId &target_role_id,
                           const core::CapabilityId &capability_id,
                           const core::RuntimeParticipantBinding &physical,
                           std::span<const core::RuntimeParticipantBinding>
                               current_representatives) const;

  [[nodiscard]] bool FrameObservationCovered(
      const core::ProgramFrameRoleBindingDTO &frame,
      const core::RuntimeParticipantBinding &observed_binding) const;

  /// Resolve portable control scope from the verified Program Binding only.
  /// Operation identity and eligibility still come from the Kernel.
  [[nodiscard]] core::Result<core::ProgramTargetRoleBindingDTO>
  ResolveOperationTarget(const core::ProgramBindingId &program_binding_id,
                         const core::TargetRoleId &target_role_id) const;

  ProductBackedRuntimeParticipantResolver(
      const ProductBackedRuntimeParticipantResolver &) = delete;
  ProductBackedRuntimeParticipantResolver &
  operator=(const ProductBackedRuntimeParticipantResolver &) = delete;

private:
  deployment::FilesystemVerifiedProgramSource programs_;
  deployment::FilesystemVerifiedProgramBindingSource bindings_;
  std::shared_ptr<const deployment::RetainedBundle> retained_bundle_;
  deployment::VerifiedRetainedModelMap retained_model_map_;
  std::vector<core::RuntimeParticipantBinding> physical_bindings_;
  std::vector<core::RuntimeCollisionBodyAliasDTO> collision_body_aliases_;
};

} // namespace manyforge::roboplan_backend
