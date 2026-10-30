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
#include <manyforge_roboplan/active_tcp.hpp>
#include <functional>
#include <memory>
#include <map>
#include <span>
#include <set>
#include <vector>

namespace manyforge::roboplan_backend {

struct RetainedResolvedParticipant;

/// Active TCP (M7, design 5.2): a robot's TCP list and Active TCP as its
/// runtime owner holds them now; none when the robot has no runtime owner
/// here. Robot control resolves its moves and Set TCP through it, and a
/// Current selector reads the Active TCP from it.
using RobotTcpReader =
    std::function<std::optional<ActiveTcp>(const core::ControllableComponentId &robot)>;

using HostRobotTcpReader = std::function<std::optional<ActiveTcp>()>;
using HostRobotTcpReaders = std::map<core::ControllableComponentId, HostRobotTcpReader>;

/// Publish the complete selected inventory and change its composition while
/// holding the registry lock. Readers already holding a copied callback may
/// finish against that previous owner.
void SelectHostRobotTcps(HostRobotTcpReaders readers,
                         const std::function<void()> &select_composition);
[[nodiscard]] std::optional<ActiveTcp>
ReadHostRobotTcps(const core::ControllableComponentId &robot);

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
/// inside an already-composed physical participant or exact logical output;
/// this object performs no repository enumeration, latest selection, or backend
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

  /// Same sealed product/model-map and exact artifact sources, with a new
  /// immutable current composition. Direct IDs are supplied by the actual
  /// compiler; this never infers direct authority from an operation label.
  [[nodiscard]] std::shared_ptr<ProductBackedRuntimeParticipantResolver>
  WithComposedParticipants(std::vector<core::RuntimeParticipantBinding> current,
                           std::vector<core::ProgramBindingId>
                               additional_intrinsic_direct_bindings = {}) const;

  /// Active TCP (M7): a motion's or a Set TCP's selector resolves through
  /// one path for Programs and Robot control: the row of the moving robot's
  /// TCP list (the Program binding's, or the runtime owner's for Robot
  /// control; Current from `robot_tcps`, else the host's registered owners),
  /// then `carries` (T-MQ7), then the endpoint with the owner's contact
  /// bodies.
  [[nodiscard]] core::Result<core::RuntimeParticipantBinding>
  Resolve(const core::OperationIntentDTO &operation,
          std::size_t participant_index,
          const RobotTcpReader &robot_tcps = {}) const;

  [[nodiscard]] core::Result<RetainedResolvedParticipant>
  ResolveExecutionParticipant(const core::OperationIntentDTO &operation,
                              std::size_t participant_index,
                              const RobotTcpReader &robot_tcps = {}) const;

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

  /// Pure target qualification against this exact product, for an already
  /// verified authored or compiler-direct Binding. Grants no runtime admission.
  [[nodiscard]] core::Result<deployment::RuntimeProgramTargetBinding>
  ResolveBoundTarget(const deployment::ProgramBinding &binding,
                     const core::TargetRoleId &target_role_id,
                     const core::CapabilityId &capability_id) const;

  /// Resolve a read against the same immutable product as admission. This
  /// neither creates an OperationIntent nor grants execution authority.
  [[nodiscard]] core::Result<ResolvedTargetObservation>
  ResolveObservationTarget(const core::ProgramBindingId &program_binding_id,
                           const core::TargetRoleId &target_role_id,
                           const core::CapabilityId &capability_id,
                           const core::RuntimeParticipantBinding &physical,
                           std::span<const core::RuntimeParticipantBinding>
                               current_representatives) const;

  /// Apply the exact collision-body projection already derived from this
  /// retained model map. Independent observation adapters use this instead of
  /// reconstructing the semantic-to-physical alias authority.
  [[nodiscard]] core::Result<core::RuntimeParticipantBinding>
  BindCollisionBodyAliases(core::RuntimeParticipantBinding binding) const;

  /// Verified static mount membership and live TCP body projection for I/O.
  [[nodiscard]] std::set<core::ControllableComponentId>
  SelfAndMounted(const core::ControllableComponentId &robot) const;
  [[nodiscard]] core::Result<std::vector<core::CollisionBodyId>>
  ActiveTcpContactBodies(const core::RuntimeParticipantBinding &robot_binding,
                         const std::optional<RobotTcp> &active) const;

  /// Resolve the current TCP for a verified bound Scene holder.
  [[nodiscard]] core::Result<core::FrameRoleId> ResolveAttachActiveTcp(
      const core::ProgramBindingId &binding_id, const core::TargetRoleId &holder_role,
      const std::function<std::optional<RobotTcp>(const core::ControllableComponentId &)> &read) const;

  [[nodiscard]] bool FrameObservationCovered(
      const core::ProgramFrameRoleBindingDTO &frame,
      const core::RuntimeParticipantBinding &observed_binding) const;

  /// Resolve portable control scope from the verified Program Binding only.
  /// Operation identity and eligibility still come from the Kernel.
  [[nodiscard]] core::Result<core::ProgramTargetRoleBindingDTO>
  ResolveOperationTarget(const core::ProgramBindingId &program_binding_id,
                         const core::TargetRoleId &target_role_id) const;

  /// Declarative qualified retained stages for the exact Applied bound route.
  /// No current readiness, preparation or execution authority is asserted.
  /// `binding` must exactly equal (full control path included) one row of
  /// `current_physical`: the owning runtime's current physical bindings, read
  /// under its route exclusion. Route publication rebinds route, activation
  /// set and active configuration generation without recomposing this
  /// resolver, so its composed rows are never the authority for them.
  [[nodiscard]] core::Result<deployment::CanonicalValue>
  ReadQualifiedStopResponse(
      const core::RuntimeParticipantBinding &binding,
      std::span<const core::RuntimeParticipantBinding> current_physical) const;

  ProductBackedRuntimeParticipantResolver(
      const ProductBackedRuntimeParticipantResolver &) = delete;
  ProductBackedRuntimeParticipantResolver &
  operator=(const ProductBackedRuntimeParticipantResolver &) = delete;

private:
  ProductBackedRuntimeParticipantResolver(
      const ProductBackedRuntimeParticipantResolver &source,
      std::vector<core::RuntimeParticipantBinding> current,
      std::vector<core::ProgramBindingId> direct_bindings);
  void ValidateComposition();

  struct ObservationSelection final {
    // This exact Binding's Program was verified when the slot was populated.
    // Changing only role/capability must not reparse the immutable Program.
    std::shared_ptr<const deployment::ProgramBinding> binding;
    deployment::RuntimeProgramTargetBinding target;
    std::optional<deployment::RuntimeProgramTargetBinding> frame_state_target;
  };

  [[nodiscard]] std::shared_ptr<const deployment::ProgramBinding>
  LoadProgramBinding(const core::ProgramBindingId &program_binding_id) const;
  /// Active TCP (M7): the selector's row of `rows`, the `carries` check and
  /// the endpoint (labelled with `role`, the owner's contact bodies).
  [[nodiscard]] core::Result<core::RuntimeMotionEndpointBindingDTO>
  ResolveRobotTcp(const core::TcpSelectorDTO &selector,
                  const core::ControllableComponentId &robot,
                  std::span<const RobotTcp> rows,
                  const std::optional<core::TcpEntryDTO> &active,
                  const std::function<std::optional<core::TcpEntryDTO>(
                      const core::TargetRoleId &)> &default_tcp_of_role,
                  const std::function<std::pair<core::ComponentRoleId, core::TargetRoleId>(
                      const core::TcpEntryDTO &)> &role_of,
                  bool joint_goal = false) const;
  [[nodiscard]] std::shared_ptr<const deployment::ProgramBindingContract>
  LoadProgram(const core::ProgramId &program_id,
              const core::ProgramRevision &program_revision) const;

  const std::filesystem::path package_root_;
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
