#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/program_binding.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/canonical_value.hpp>

namespace manyforge::deployment {

class ProgramBindingOutcomeError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// Why one authored target of an otherwise valid Program does not resolve in a
/// candidate cell. Only candidate-dependent facts appear here; malformed
/// Program input, corrupt artifacts and ambiguous retained facts abort
/// preparation instead.
enum class ProgramBindingUnresolvedReason : std::uint8_t {
  /// The addressed controllable component is absent from the candidate.
  kComponentMissing = 0,
  /// The component's type lacks a required capability or model fact.
  kComponentIncompatible,
  /// The addressed command target is absent from the component's type.
  kTargetMissing,
  /// The target lacks a required model, frame, resource or collision role.
  kTargetIncompatible,
  /// A required capability is not retained, declared or provided.
  kCapabilityUnavailable,
};

[[nodiscard]] std::string_view
to_string(ProgramBindingUnresolvedReason reason) noexcept;

inline constexpr std::size_t kMaximumProgramBindingUnresolvedRows = 256U;
inline constexpr std::size_t kMaximumProgramBindingUnresolvedDetailBytes = 256U;
inline constexpr std::size_t kMaximumProgramBindingAffectedNodes = 4096U;
inline constexpr std::size_t kMaximumProgramBindingAffectedGroups = 256U;

struct ProgramBindingUnresolvedTargetDTO final {
  core::ProgramTargetReferenceDTO target;
  ProgramBindingUnresolvedReason reason{
      ProgramBindingUnresolvedReason::kComponentMissing};
  /// Bounded diagnostic text from the failed binding check.
  std::string detail;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ProgramBindingUnresolvedTargetDTO &,
                         const ProgramBindingUnresolvedTargetDTO &) = default;
};

/// The candidate-bound result for a Program that cannot be bound to one exact
/// candidate cell. The authored Program is preserved unchanged; this value
/// states why Execute is refused against the captured candidate.
struct ProgramNonExecutableOutcomeDTO final {
  core::ProgramId program_id;
  core::ProgramRevision program_revision;
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
  /// Non-empty and sorted by exact component/target identity.
  std::vector<ProgramBindingUnresolvedTargetDTO> unresolved_targets;
  /// Sorted node instances whose target effects use an unresolved target.
  std::vector<core::ProgramNodeInstanceId> affected_nodes;
  /// Sorted authored execution groups containing an affected node. The
  /// implicit `all` group is never listed: it always contains every node.
  std::vector<std::string> affected_execution_groups;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ProgramNonExecutableOutcomeDTO &,
                         const ProgramNonExecutableOutcomeDTO &) = default;
};

/// The frames of a non-executable Program that did resolve against the
/// candidate. This is not a binding and never authorizes a Run or command; it
/// only lets preparation place authored Scene objects on frames the candidate
/// still verifies.
struct ProgramResolvedReferencesDTO final {
  /// Exact candidate-qualified frame facts for Scene projection. This is not
  /// a Program binding and grants no execution authority.
  std::vector<core::ProgramFrameBindingDTO> frame_bindings;

  friend bool operator==(const ProgramResolvedReferencesDTO &,
                         const ProgramResolvedReferencesDTO &) = default;
};

/// Canonical JSON (api `manyforge.program-binding-outcome.v2`) used by the
/// prepared activation, the Manager Apply record and status projections.
[[nodiscard]] std::string
EncodeProgramNonExecutableOutcome(const ProgramNonExecutableOutcomeDTO &value);
[[nodiscard]] ProgramNonExecutableOutcomeDTO
DecodeProgramNonExecutableOutcome(std::string_view encoded);
[[nodiscard]] core::ContentDigest
DigestProgramNonExecutableOutcome(const ProgramNonExecutableOutcomeDTO &value);

/// Nodes and named execution groups that use any of `targets`.
struct ProgramBindingAffectedScope final {
  std::vector<core::ProgramNodeInstanceId> nodes;
  std::vector<std::string> execution_groups;
};
[[nodiscard]] ProgramBindingAffectedScope program_binding_affected_scope(
    const core::ProgramExecutionTableDTO &execution_table,
    const std::vector<core::ProgramTargetReferenceDTO> &targets);

} // namespace manyforge::deployment
