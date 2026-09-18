#pragma once

#include <atomic>
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

struct RetainedResolvedParticipant;

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
      std::vector<core::RuntimeParticipantBinding> physical_bindings,
      std::vector<core::ProgramBindingId> intrinsic_direct_bindings,
      std::filesystem::path program_artifact_root = {});

  [[nodiscard]] core::Result<core::RuntimeParticipantBinding>
  Resolve(const core::OperationIntentDTO &operation,
          std::size_t participant_index) const;

  [[nodiscard]] core::Result<RetainedResolvedParticipant>
  ResolveExecutionParticipant(const core::OperationIntentDTO &operation,
                              std::size_t participant_index) const;

  /// Effect-only ingress check. Preview/query resolution does not acquire
  /// authority. Captured Program effects still require the Kernel's exact
  /// claim/node checks; compiler-direct effects still require manual admission.
  [[nodiscard]] core::Result<void>
  ValidateExecutionOrigin(const core::OperationIntentDTO &operation) const;

  /// Resolve identity-only Run registration to a const, sealed Program table.
  /// This does not acquire a claim or authorize effects; Kernel installation
  /// checks the actual held Program-cycle claim after this cold product read.
  [[nodiscard]] core::Result<core::ProgramRunExecutionCaptureDTO>
  ResolveProgramCapture(core::ProgramRunExecutionCaptureDTO capture) const;

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
  struct ObservationSelection final {
    // This exact Binding's Program was verified when the slot was populated.
    // Changing only role/capability must not reparse the immutable Program.
    std::shared_ptr<const deployment::ProgramBinding> binding;
    deployment::RuntimeProgramTargetBinding target;
    std::optional<deployment::RuntimeProgramTargetBinding> frame_state_target;
  };

  [[nodiscard]] std::shared_ptr<const deployment::ProgramBinding>
  LoadProgramBinding(const core::ProgramBindingId &program_binding_id) const;
  [[nodiscard]] std::shared_ptr<const deployment::ProgramBindingContract>
  LoadProgram(const core::ProgramId &program_id,
              const core::ProgramRevision &program_revision) const;

  deployment::FilesystemVerifiedProgramSource programs_;
  deployment::FilesystemVerifiedProgramBindingSource bindings_;
  // Program Apply publishes immutable, content-addressed Programs and Bindings
  // outside the installed package. Exact point reads prefer that repository
  // and fall back to the installed baseline; neither source is enumerated.
  std::unique_ptr<deployment::FilesystemVerifiedProgramSource>
      artifact_programs_;
  std::unique_ptr<deployment::FilesystemVerifiedProgramBindingSource>
      artifact_bindings_;
  std::shared_ptr<const deployment::RetainedBundle> retained_bundle_;
  deployment::VerifiedRetainedModelMap retained_model_map_;
  std::vector<core::RuntimeParticipantBinding> physical_bindings_;
  // Supplied by the actual direct-control compiler at composition, never
  // inferred from an operation label or a precomposed Program alias.
  std::vector<core::ProgramBindingId> intrinsic_direct_bindings_;
  std::vector<core::RuntimeCollisionBodyAliasDTO> collision_body_aliases_;
  // One immutable resolution per composition, never a live source/sample.
  // Concurrent misses may duplicate bounded cold work; neither waits for the
  // other's I/O and replacement cannot grow a service-lifetime cache.
  mutable std::atomic<std::shared_ptr<const ObservationSelection>>
      observation_selection_;
};

} // namespace manyforge::roboplan_backend
