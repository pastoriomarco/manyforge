#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/component_spec.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::deployment {

class ApplyComponentDeltaError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// What one Apply does to one controllable component's runtime.
///
/// Retain keeps the exact controller, engines, observed-state source, owner
/// epoch, eligibility and lifecycle/route generations; the component takes the
/// new CellContext. Replace is chosen exactly when its ComponentSpec changed,
/// and names the spec sections that differ.
enum class ApplyComponentDisposition : std::uint8_t {
  kRetain = 0,
  kAdd,
  kRemove,
  kReplace,
};

[[nodiscard]] std::string_view
to_string(ApplyComponentDisposition disposition) noexcept;

inline constexpr std::size_t kMaximumApplyComponentDeltaRows = 128U;
inline constexpr std::size_t kMaximumApplyComponentDeltaDomains = 64U;

struct ApplyComponentChangeDTO final {
  core::ControllableComponentId controllable_component_id;
  ApplyComponentDisposition disposition{ApplyComponentDisposition::kRetain};
  /// Absent exactly for kAdd.
  std::optional<core::ContentDigest> predecessor_runtime_identity;
  /// Absent exactly for kRemove.
  std::optional<core::ContentDigest> candidate_runtime_identity;
  /// Canonical sorted domains the component owns in each bundle.
  std::vector<core::ControlDomainId> predecessor_domains;
  std::vector<core::ControlDomainId> candidate_domains;
  /// Sorted spec section names (kComponentSpecSections); non-empty exactly
  /// for kReplace.
  std::vector<std::string> replacement_reasons;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ApplyComponentChangeDTO &,
                         const ApplyComponentChangeDTO &) = default;
};

/// One Apply's complete per-component decision, derived before any fence or
/// runtime effect. Program/Scene-only Apply is the case where every row is
/// kRetain. Shared Scene data (placements, planning/viewer models, symbol
/// maps and attachments) is reported separately: it changes projections, not
/// controller lifetimes.
struct ApplyComponentDeltaDTO final {
  /// One row per component in predecessor ∪ candidate, sorted by id.
  std::vector<ApplyComponentChangeDTO> components;
  bool shared_scene_projection_changed{false};
  /// Sorted components that are a parent or child of a changed attachment.
  std::vector<core::ControllableComponentId> attachment_changed_components;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ApplyComponentDeltaDTO &,
                         const ApplyComponentDeltaDTO &) = default;
};

/// Canonical digest binding a delta to its durable record and receipts.
[[nodiscard]] core::ContentDigest
DigestApplyComponentDelta(const ApplyComponentDeltaDTO &delta);

/// Canonical text encoding used by durable records and the Manager RPC.
[[nodiscard]] std::string
EncodeApplyComponentDelta(const ApplyComponentDeltaDTO &delta);
[[nodiscard]] ApplyComponentDeltaDTO
DecodeApplyComponentDelta(std::string_view encoded);

/// The runtime identity of one component in one sealed bundle (its
/// ComponentSpec fingerprint under this site's facts), or nullopt when the
/// bundle does not contain the component. The host uses this to report the
/// identity it actually installed.
[[nodiscard]] std::optional<core::ContentDigest>
derive_component_runtime_identity(
    const RetainedBundle &bundle, const SiteFacts &site_facts,
    const core::ControllableComponentId &controllable_component_id);

/// Every added, removed or replaced component that owns a control domain is
/// inside the Apply's fence scope. Retained components need not be: their
/// runtime and command readiness do not change.
[[nodiscard]] bool apply_component_delta_is_fenced(
    const ApplyComponentDeltaDTO &delta,
    const core::ConfigurationFenceScopeDTO &scope) noexcept;

/// Components that are attachment children in `predecessor`, remain in
/// `candidate`, and are no longer attachment children there (for example an
/// independent gripper whose robot was removed). Sorted and unique.
///
/// Such a child keeps its identity and controller; it is retained only at its
/// authored, verified candidate root pose. A component with models but no
/// root pose throws ApplyComponentDeltaError before any effect: a child is
/// never silently dropped or left unplaced. Removing it is an explicit
/// candidate choice reported as a removal in the component delta.
[[nodiscard]] std::vector<core::ControllableComponentId>
detached_attachment_children(const core::CellGraphDTO &predecessor,
                             const core::CellGraphDTO &candidate);

/// Purely classifies every component of one named Apply by comparing each
/// component's spec in both bundles under the same site facts. Both inputs
/// are sealed, independently verified retained bundles of the same cell; the
/// function performs no I/O.
[[nodiscard]] ApplyComponentDeltaDTO
derive_apply_component_delta(const RetainedBundle &predecessor,
                             const RetainedBundle &candidate,
                             const SiteFacts &site_facts);

} // namespace manyforge::deployment
