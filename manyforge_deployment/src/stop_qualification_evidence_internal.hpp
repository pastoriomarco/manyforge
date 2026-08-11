#pragma once

#include <array>
#include <cstdint>
#include <manyforge_core/types/stop_control.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::deployment::detail {

inline constexpr std::string_view
    kStopImplementationQualificationEvidenceApiVersion =
        "manyforge.stop_implementation_qualification_evidence.v1";
inline constexpr std::string_view
    kControlledStopSubstitutionQualificationEvidenceApiVersion =
        "manyforge.stop_controlled_substitution_qualification_evidence.v1";
inline constexpr std::string_view kStopImplementationBuildClosureApiVersion =
    "manyforge.stop_implementation_build_closure.v1";
inline constexpr std::string_view kStopQualificationRunnerContractApiVersion =
    "manyforge.stop_qualification_runner_contract.v1";
inline constexpr std::size_t kMaxStopQualificationEvidenceEntries = 256U;
inline constexpr std::size_t kMaxStopQualificationEvidenceBytes =
    4U * 1024U * 1024U;

enum class StopQualificationFaultCase : std::uint8_t {
  kNominalProviderCallBlocked = 0,
  kNominalAdapterLockBlocked,
  kNominalResultWaitBlocked,
};

struct StopQualificationFaultCaseResult final {
  StopQualificationFaultCase fault_case;
  std::uint64_t maximum_observed_issue_latency_ns;

  friend bool operator==(const StopQualificationFaultCaseResult&,
                         const StopQualificationFaultCaseResult&) = default;
};

struct BuildQualifiedStopImplementationEvidenceEntry final {
  std::string implementation_id;
  core::ContentDigest implementation_source_build_fingerprint;
  core::ProviderContractRefDTO provider_contract_ref;
  core::ProductContractId product_contract_id;
  core::ProductContractRevision product_contract_revision;
  core::RegisteredStopCommandKindId registered_command_kind_id;
  std::optional<core::EndpointRoleId> endpoint_role_id;
  core::StopInvocationLaneId invocation_lane_id;
  std::uint64_t qualified_issue_time_bound_ns;
  std::array<StopQualificationFaultCaseResult, 3U> fault_case_results;

  friend bool operator==(
      const BuildQualifiedStopImplementationEvidenceEntry&,
      const BuildQualifiedStopImplementationEvidenceEntry&) = default;
};

enum class ControlledStopSubstitutionQualificationCase : std::uint8_t {
  kSelectedBeforePhysicalDispatch = 0,
  kNominalScopePreserved,
  kNominalStageBudgetPreserved,
  kRequestedExecutedReasonRetained,
};

struct BuildQualifiedControlledStopSubstitutionEvidenceEntry final {
  std::string implementation_id;
  core::ContentDigest implementation_source_build_fingerprint;
  core::ProviderContractRefDTO provider_contract_ref;
  core::ProductContractId product_contract_id;
  core::ProductContractRevision product_contract_revision;
  core::MotionControlledStopMode requested_stop_mode{
      core::MotionControlledStopMode::kSoftOnPath};
  core::MotionControlledStopMode executed_stop_mode{
      core::MotionControlledStopMode::kSpringBackHold};
  std::array<ControlledStopSubstitutionQualificationCase, 4U> case_results;

  friend bool operator==(
      const BuildQualifiedControlledStopSubstitutionEvidenceEntry&,
      const BuildQualifiedControlledStopSubstitutionEvidenceEntry&) = default;
};

/// Closed identity projected from the CMake-produced implementation/build
/// closure. The retained native runtime product stores the exact canonical
/// closure bytes; this typed view is used only for exact package/product joins.
struct StopImplementationBuildIdentity final {
  std::string implementation_id;
  core::ContentDigest implementation_source_build_fingerprint;
  std::string installed_target_id;
  std::uint64_t installed_target_byte_size;
  core::ContentDigest installed_target_content_digest;
  std::string canonical_build_closure_bytes;

  friend bool operator==(const StopImplementationBuildIdentity&,
                         const StopImplementationBuildIdentity&) = default;
};

/// Untrusted explicit output from one offline qualification run.
///
/// The build closure is a closed canonical description of only the affected
/// implementation target. `installed_target_bytes` are independently hashed
/// by the authority; a claimed target digest is never accepted on its own.
/// Normal retained-product compilation does not execute the runner.
struct StopQualificationEvidenceCandidate final {
  std::string implementation_build_closure_bytes;
  std::string installed_target_bytes;
  std::string qualification_runner_contract_bytes;
  std::string qualification_runner_executable_bytes;
  std::optional<std::string> implementation_qualification_package_bytes;
  std::optional<std::string> controlled_substitution_package_bytes;
};

/// Private, non-caller-constructible closure accepted by I3.
class BuildQualifiedStopEvidenceClosure final {
 public:
  BuildQualifiedStopEvidenceClosure(
      const BuildQualifiedStopEvidenceClosure&) = default;
  BuildQualifiedStopEvidenceClosure& operator=(
      const BuildQualifiedStopEvidenceClosure&) = default;

  [[nodiscard]] std::span<
      const BuildQualifiedStopImplementationEvidenceEntry>
  implementation_entries() const noexcept {
    return implementation_entries_;
  }
  [[nodiscard]] std::span<
      const BuildQualifiedControlledStopSubstitutionEvidenceEntry>
  controlled_substitution_entries() const noexcept {
    return controlled_substitution_entries_;
  }
  [[nodiscard]] const std::optional<core::ContentDigest>&
  implementation_runner_contract_digest() const noexcept {
    return implementation_runner_contract_digest_;
  }
  [[nodiscard]] const std::optional<core::ContentDigest>&
  controlled_substitution_runner_contract_digest() const noexcept {
    return controlled_substitution_runner_contract_digest_;
  }
  [[nodiscard]] std::span<const StopImplementationBuildIdentity>
  implementation_build_identities() const noexcept {
    return implementation_build_identities_;
  }

 private:
  BuildQualifiedStopEvidenceClosure() = default;
  BuildQualifiedStopEvidenceClosure(
      std::optional<core::ContentDigest>
          implementation_runner_contract_digest,
      std::vector<BuildQualifiedStopImplementationEvidenceEntry>
          implementation_entries,
      std::optional<core::ContentDigest>
          controlled_substitution_runner_contract_digest,
      std::vector<BuildQualifiedControlledStopSubstitutionEvidenceEntry>
          controlled_substitution_entries,
      std::vector<StopImplementationBuildIdentity>
          implementation_build_identities);

  std::optional<core::ContentDigest>
      implementation_runner_contract_digest_;
  std::vector<BuildQualifiedStopImplementationEvidenceEntry>
      implementation_entries_;
  std::optional<core::ContentDigest>
      controlled_substitution_runner_contract_digest_;
  std::vector<BuildQualifiedControlledStopSubstitutionEvidenceEntry>
      controlled_substitution_entries_;
  std::vector<StopImplementationBuildIdentity>
      implementation_build_identities_;

  friend class BuildQualifiedRetentionAuthority;
  friend class BuildQualifiedRetentionContext;
  friend class BuildManifestTestAccess;
};

/// Parse one exact canonical build-identity artifact. This verifies the closed
/// shape and content fingerprint, but does not by itself qualify the claimed
/// installed target bytes; only BuildQualifiedRetentionAuthority can do that.
[[nodiscard]] StopImplementationBuildIdentity
parse_stop_implementation_build_identity(std::string_view canonical_bytes);

/// Verify that a canonical build closure names these exact installed bytes.
/// This is a read-neutral primitive; only the offline retention authority may
/// combine the result with runner evidence and mint a sealed closure.
[[nodiscard]] StopImplementationBuildIdentity
verify_stop_implementation_build_candidate(
    std::string_view canonical_bytes,
    std::string_view installed_target_bytes);

/// Verify one closed runner contract against the exact runner executable.
/// This returns only the contract identity and grants no retention authority.
[[nodiscard]] core::ContentDigest verify_stop_qualification_runner_candidate(
    std::string_view canonical_bytes,
    std::string_view qualification_runner_executable_bytes);

/// Parse the two closed retained package shapes independently of authority.
/// These functions return untrusted typed values and are used by readback.
[[nodiscard]] std::pair<
    core::ContentDigest,
    std::vector<BuildQualifiedStopImplementationEvidenceEntry>>
parse_stop_implementation_qualification_evidence_package(
    std::string_view canonical_bytes);

[[nodiscard]] std::pair<
    core::ContentDigest,
    std::vector<BuildQualifiedControlledStopSubstitutionEvidenceEntry>>
parse_controlled_stop_substitution_qualification_evidence_package(
    std::string_view canonical_bytes);

[[nodiscard]] std::string
emit_stop_implementation_qualification_evidence_package(
    const core::ContentDigest& qualification_runner_contract_digest,
    std::span<const BuildQualifiedStopImplementationEvidenceEntry> entries);

[[nodiscard]] std::string
emit_controlled_stop_substitution_qualification_evidence_package(
    const core::ContentDigest& qualification_runner_contract_digest,
    std::span<const BuildQualifiedControlledStopSubstitutionEvidenceEntry>
        entries);

}  // namespace manyforge::deployment::detail
