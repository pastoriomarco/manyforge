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

/// Why one authored role of an otherwise valid Program does not resolve in a
/// candidate cell. Only candidate-dependent facts appear here; malformed
/// Program input, corrupt artifacts and ambiguous retained facts abort
/// preparation instead.
enum class ProgramBindingUnresolvedReason : std::uint8_t {
  /// The assigned controllable component is absent from the candidate.
  kComponentMissing = 0,
  /// The assigned component's type lacks a required tag/model/attachment.
  kComponentIncompatible,
  /// The assigned command target is absent from the component's type.
  kTargetMissing,
  /// The target lacks a required model, frame, resource or collision role.
  kTargetIncompatible,
  /// A required capability is not retained, declared or provided.
  kCapabilityUnavailable,
  /// The target's component role is itself unresolved.
  kComponentRoleUnresolved,
};

[[nodiscard]] std::string_view
to_string(ProgramBindingUnresolvedReason reason) noexcept;

inline constexpr std::size_t kMaximumProgramBindingUnresolvedRows = 256U;
inline constexpr std::size_t kMaximumProgramBindingUnresolvedDetailBytes = 256U;
inline constexpr std::size_t kMaximumProgramBindingAffectedNodes = 4096U;
inline constexpr std::size_t kMaximumProgramBindingAffectedGroups = 256U;

struct ProgramBindingUnresolvedRoleDTO final {
  core::ComponentRoleId component_role_id;
  /// Absent for a row about the component role itself.
  std::optional<core::TargetRoleId> target_role_id;
  core::ControllableComponentId controllable_component_id;
  std::optional<core::CommandTargetId> command_target_id;
  ProgramBindingUnresolvedReason reason{
      ProgramBindingUnresolvedReason::kComponentMissing};
  /// Bounded diagnostic text from the failed binding check.
  std::string detail;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ProgramBindingUnresolvedRoleDTO &,
                         const ProgramBindingUnresolvedRoleDTO &) = default;
};

/// The candidate-bound result for a Program that cannot be bound to one exact
/// candidate cell. The authored Program and role assignment are preserved
/// unchanged; this value only states, by name, why Execute is refused.
struct ProgramNonExecutableOutcomeDTO final {
  core::ProgramId program_id;
  core::ProgramRevision program_revision;
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
  core::ContentDigest role_assignment_digest;
  /// Non-empty; ordered by component role, then the component-role row before
  /// its target rows, then target role.
  std::vector<ProgramBindingUnresolvedRoleDTO> unresolved_roles;
  /// Sorted node instances whose target effects use an unresolved role.
  std::vector<core::ProgramNodeInstanceId> affected_nodes;
  /// Sorted authored execution groups containing an affected node. The
  /// implicit `all` group is never listed: it always contains every node.
  std::vector<std::string> affected_execution_groups;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ProgramNonExecutableOutcomeDTO &,
                         const ProgramNonExecutableOutcomeDTO &) = default;
};

/// The roles of a non-executable Program that did resolve against the
/// candidate. This is not a binding and never authorizes a Run or command; it
/// only lets preparation place authored Scene objects on frames the candidate
/// still verifies (H1a).
struct ProgramResolvedRolesDTO final {
  std::vector<core::ProgramComponentRoleBindingDTO> component_roles;
  /// Verified target/frame identities only. Command capabilities, resources
  /// and collision-role bindings remain empty; a target may lack capabilities
  /// needed by the Program while its Scene frames still resolve.
  std::vector<core::ProgramTargetRoleBindingDTO> target_roles;

  friend bool operator==(const ProgramResolvedRolesDTO &,
                         const ProgramResolvedRolesDTO &) = default;
};

/// Canonical JSON (api `manyforge.program-binding-outcome.v1`) used by the
/// prepared activation, the Manager Apply record and status projections.
[[nodiscard]] std::string
EncodeProgramNonExecutableOutcome(const ProgramNonExecutableOutcomeDTO &value);
[[nodiscard]] ProgramNonExecutableOutcomeDTO
DecodeProgramNonExecutableOutcome(std::string_view encoded);
[[nodiscard]] core::ContentDigest
DigestProgramNonExecutableOutcome(const ProgramNonExecutableOutcomeDTO &value);

/// Decode one authored `program_role_assignment` object exactly (closed
/// fields, unique roles). The value stays untrusted authored selection; the
/// binding compiler decides whether it resolves.
[[nodiscard]] core::ProgramRoleAssignmentDTO
parse_program_role_assignment(const CanonicalValue &value);

/// Nodes and named execution groups that use any of `target_roles`.
struct ProgramBindingAffectedScope final {
  std::vector<core::ProgramNodeInstanceId> nodes;
  std::vector<std::string> execution_groups;
};
[[nodiscard]] ProgramBindingAffectedScope program_binding_affected_scope(
    const core::ProgramExecutionTableDTO &execution_table,
    const std::vector<core::TargetRoleId> &target_roles);

} // namespace manyforge::deployment
