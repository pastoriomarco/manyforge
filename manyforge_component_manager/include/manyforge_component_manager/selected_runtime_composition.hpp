#pragma once

#include <cstdint>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <optional>
#include <vector>

namespace manyforge::component_manager {

enum class ManagerSelectedRuntimeCompositionPhase {
  kPrepare,
  kFinalize,
};

/// Exact Manager-to-managed-host request to install the already selected
/// product composition while its durable Kernel configuration fence is held.
/// This is a private Apply effect, not caller authorization or a command
/// session.
struct ManagerSelectedRuntimeCompositionRequestDTO final {
  manyforge::core::ContentDigest authoritative_scene_snapshot_digest;
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ProjectionRevisionDTO candidate_projection_revision;
  manyforge::core::ActiveBundlePublicationGeneration
      expected_predecessor_publication_generation;
  manyforge::core::ContentDigest expected_predecessor_public_pointer_digest;
  manyforge::core::ContentDigest retained_bundle_canonical_content_digest;
  std::uint64_t retained_bundle_canonical_byte_size{};
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;
  manyforge::core::RuntimeEpoch manager_runtime_epoch;
  manyforge::core::ConfigurationFenceId configuration_fence_id;
  manyforge::core::ConfigurationFenceGeneration configuration_fence_generation;
  manyforge::core::ConfigurationFenceScopeDTO configuration_fence_scope;
  ManagerSelectedRuntimeCompositionPhase phase{
      ManagerSelectedRuntimeCompositionPhase::kFinalize};
  /// The exact per-component decision Manager persisted with the fence scope
  /// before any effect. The host changes only the non-retained components.
  ///
  /// kRetain keeps the component's controller lifetime: engines, controller
  /// connections, observed-state source, owner epoch, eligibility and
  /// lifecycle/route generations. Its product-bound participant bindings and
  /// Kernel registrations are still re-sealed to the candidate bundle and
  /// graph revision whenever the product identity changes; equal runtime
  /// identities prove that the re-seal changes no runtime-defining fact. The
  /// host derives the candidate bindings from the verified candidate bundle;
  /// the delta carries no second Deployment representation.
  manyforge::deployment::ApplyComponentDeltaDTO component_delta;
  /// The active product the retained controllers are currently sealed to.
  /// The host verifies its live registrations against it before re-sealing.
  manyforge::core::ProjectionRevisionDTO predecessor_projection_revision;

  [[nodiscard]] bool valid() const noexcept;

  friend bool
  operator==(const ManagerSelectedRuntimeCompositionRequestDTO &,
             const ManagerSelectedRuntimeCompositionRequestDTO &) = default;
};

/// The host's live-owner evidence for one non-removed delta row after
/// installation. A retained component reports the predecessor runtime it
/// kept; an added or replaced one reports the runtime it installed.
struct ManagerRuntimeComponentReceiptDTO final {
  manyforge::core::ControllableComponentId controllable_component_id;
  manyforge::deployment::ApplyComponentDisposition disposition{
      manyforge::deployment::ApplyComponentDisposition::kRetain};
  std::optional<manyforge::core::ContentDigest> installed_runtime_identity;
  std::vector<manyforge::core::RuntimeUnitEpochBindingDTO> runtime_unit_epochs;
  bool predecessor_runtime_retained{false};
  /// For kRetain: the retained controller's product-bound bindings and
  /// registrations were re-sealed to the candidate product without engine
  /// reconstruction. Always false for added/replaced components, which are
  /// installed fresh against the candidate.
  bool product_binding_resealed{false};

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerRuntimeComponentReceiptDTO &,
                         const ManagerRuntimeComponentReceiptDTO &) = default;
};

/// Exact receipt check against the request: one receipt per non-removed
/// delta row in delta order, the planned identity, exactly the request's
/// managed runtime unit at its runtime epoch, retention reported exactly
/// for kRetain, and a re-seal reported exactly for retained components when
/// the candidate product differs from the predecessor. Removed rows have no
/// receipt.
[[nodiscard]] bool ComponentReceiptsMatchRequest(
    const ManagerSelectedRuntimeCompositionRequestDTO &request,
    const std::vector<ManagerRuntimeComponentReceiptDTO> &receipts) noexcept;

enum class ManagerSelectedRuntimeCompositionState {
  kPreparing,
  kInstalled,
};

struct ManagerSelectedRuntimeCompositionResultDTO final {
  ManagerSelectedRuntimeCompositionRequestDTO request;
  ManagerSelectedRuntimeCompositionState state;
  /// Canonical per-component receipts, sorted by component id. Required and
  /// checked by Manager once a finalize request reports kInstalled.
  std::vector<ManagerRuntimeComponentReceiptDTO> component_receipts;

  [[nodiscard]] bool valid() const noexcept;

  friend bool
  operator==(const ManagerSelectedRuntimeCompositionResultDTO &,
             const ManagerSelectedRuntimeCompositionResultDTO &) = default;
};

class IManagerSelectedRuntimeCompositionPort {
public:
  virtual ~IManagerSelectedRuntimeCompositionPort() = default;

  [[nodiscard]] virtual ManagerSelectedRuntimeCompositionResultDTO
  InstallOrObserve(
      const ManagerSelectedRuntimeCompositionRequestDTO &request) = 0;
};

} // namespace manyforge::component_manager
