#include "build_manifest_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "stop_qualification_evidence_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

[[nodiscard]] auto implementation_key(
    const BuildQualifiedStopImplementationEvidenceEntry& value) {
  return std::tuple{
      value.implementation_id,
      value.implementation_source_build_fingerprint.value(),
      value.product_contract_id.value(),
      value.product_contract_revision.value(),
      value.provider_contract_ref.provider_contract_role_id.value(),
      value.provider_contract_ref.contract_schema_id.value(),
      value.provider_contract_ref.contract_schema_revision.value(),
      value.registered_command_kind_id.value(),
      value.endpoint_role_id.has_value() ? 1 : 0,
      value.endpoint_role_id.has_value() ? value.endpoint_role_id->value()
                                        : std::string{},
      value.invocation_lane_id.value()};
}

[[nodiscard]] auto substitution_key(
    const BuildQualifiedControlledStopSubstitutionEvidenceEntry& value) {
  return std::tuple{
      value.implementation_id,
      value.implementation_source_build_fingerprint.value(),
      value.product_contract_id.value(),
      value.product_contract_revision.value(),
      value.provider_contract_ref.provider_contract_role_id.value(),
      value.provider_contract_ref.contract_schema_id.value(),
      value.provider_contract_ref.contract_schema_revision.value(),
      static_cast<std::uint8_t>(value.requested_stop_mode),
      static_cast<std::uint8_t>(value.executed_stop_mode)};
}

}  // namespace

BuildQualifiedStopEvidenceClosure
BuildQualifiedRetentionAuthority::qualify_stop_evidence(
    std::span<const StopQualificationEvidenceCandidate> candidates) {
  if (candidates.size() > kMaxStopQualificationEvidenceEntries) {
    fail_retained_bundle(
        "Stop qualification candidate closure exceeds its cardinality bound");
  }

  std::optional<core::ContentDigest> implementation_runner_digest;
  std::vector<BuildQualifiedStopImplementationEvidenceEntry>
      implementation_entries;
  std::optional<core::ContentDigest> substitution_runner_digest;
  std::vector<BuildQualifiedControlledStopSubstitutionEvidenceEntry>
      substitution_entries;
  std::vector<StopImplementationBuildIdentity> implementation_build_identities;

  for (const auto& candidate : candidates) {
    if (!candidate.implementation_qualification_package_bytes.has_value() &&
        !candidate.controlled_substitution_package_bytes.has_value()) {
      fail_retained_bundle(
          "Stop qualification candidate contains no qualification package");
    }
    const auto implementation_build =
        verify_stop_implementation_build_candidate(
            candidate.implementation_build_closure_bytes,
            candidate.installed_target_bytes);
    const auto prior_build = std::find_if(
        implementation_build_identities.begin(),
        implementation_build_identities.end(), [&](const auto& value) {
          return value.implementation_id == implementation_build.implementation_id;
        });
    if (prior_build == implementation_build_identities.end()) {
      implementation_build_identities.push_back(implementation_build);
    } else if (*prior_build != implementation_build) {
      fail_retained_bundle(
          "Stop qualification candidates mix build identities for one "
          "implementation");
    }
    const auto runner_digest = verify_stop_qualification_runner_candidate(
        candidate.qualification_runner_contract_bytes,
        candidate.qualification_runner_executable_bytes);

    if (candidate.implementation_qualification_package_bytes.has_value()) {
      auto [package_runner_digest, entries] =
          parse_stop_implementation_qualification_evidence_package(
              *candidate.implementation_qualification_package_bytes);
      if (package_runner_digest != runner_digest) {
        fail_retained_bundle(
            "Stop implementation evidence names a different runner contract");
      }
      if (implementation_runner_digest.has_value() &&
          *implementation_runner_digest != package_runner_digest) {
        fail_retained_bundle(
            "Stop implementation evidence mixes runner contracts");
      }
      implementation_runner_digest = package_runner_digest;
      for (const auto& entry : entries) {
        if (entry.implementation_id != implementation_build.implementation_id ||
            entry.implementation_source_build_fingerprint !=
                implementation_build.implementation_source_build_fingerprint) {
          fail_retained_bundle(
              "Stop implementation evidence is stale for its exact target closure");
        }
      }
      implementation_entries.insert(
          implementation_entries.end(),
          std::make_move_iterator(entries.begin()),
          std::make_move_iterator(entries.end()));
    }

    if (candidate.controlled_substitution_package_bytes.has_value()) {
      auto [package_runner_digest, entries] =
          parse_controlled_stop_substitution_qualification_evidence_package(
              *candidate.controlled_substitution_package_bytes);
      if (package_runner_digest != runner_digest) {
        fail_retained_bundle(
            "controlled Stop substitution evidence names a different runner contract");
      }
      if (substitution_runner_digest.has_value() &&
          *substitution_runner_digest != package_runner_digest) {
        fail_retained_bundle(
            "controlled Stop substitution evidence mixes runner contracts");
      }
      substitution_runner_digest = package_runner_digest;
      for (const auto& entry : entries) {
        if (entry.implementation_id != implementation_build.implementation_id ||
            entry.implementation_source_build_fingerprint !=
                implementation_build.implementation_source_build_fingerprint) {
          fail_retained_bundle(
              "controlled Stop substitution evidence is stale for its exact target closure");
        }
      }
      substitution_entries.insert(
          substitution_entries.end(),
          std::make_move_iterator(entries.begin()),
          std::make_move_iterator(entries.end()));
    }
  }

  std::sort(implementation_entries.begin(), implementation_entries.end(),
            [](const auto& left, const auto& right) {
              return implementation_key(left) < implementation_key(right);
            });
  std::sort(substitution_entries.begin(), substitution_entries.end(),
            [](const auto& left, const auto& right) {
              return substitution_key(left) < substitution_key(right);
            });
  std::sort(implementation_build_identities.begin(),
            implementation_build_identities.end(),
            [](const auto& left, const auto& right) {
              return left.implementation_id < right.implementation_id;
            });
  const auto has_exact_build_identity = [&](const auto& entry) {
    const auto found = std::lower_bound(
        implementation_build_identities.begin(),
        implementation_build_identities.end(), entry.implementation_id,
        [](const auto& identity, const std::string& implementation_id) {
          return identity.implementation_id < implementation_id;
        });
    return found != implementation_build_identities.end() &&
           found->implementation_id == entry.implementation_id &&
           found->implementation_source_build_fingerprint ==
               entry.implementation_source_build_fingerprint;
  };
  if (implementation_entries.size() > kMaxStopQualificationEvidenceEntries ||
      substitution_entries.size() > kMaxStopQualificationEvidenceEntries ||
      implementation_build_identities.size() >
          kMaxStopQualificationEvidenceEntries ||
      (implementation_runner_digest.has_value() &&
       substitution_runner_digest.has_value() &&
       implementation_runner_digest != substitution_runner_digest) ||
      !std::all_of(implementation_entries.begin(), implementation_entries.end(),
                   has_exact_build_identity) ||
      !std::all_of(substitution_entries.begin(), substitution_entries.end(),
                   has_exact_build_identity) ||
      std::adjacent_find(
          implementation_entries.begin(), implementation_entries.end(),
          [](const auto& left, const auto& right) {
            return implementation_key(left) == implementation_key(right);
          }) != implementation_entries.end() ||
      std::adjacent_find(
          substitution_entries.begin(), substitution_entries.end(),
          [](const auto& left, const auto& right) {
            return substitution_key(left) == substitution_key(right);
          }) != substitution_entries.end()) {
    fail_retained_bundle(
        "Stop qualification evidence closure is duplicate or over-limit");
  }

  return BuildQualifiedStopEvidenceClosure(
      std::move(implementation_runner_digest),
      std::move(implementation_entries),
      std::move(substitution_runner_digest),
      std::move(substitution_entries),
      std::move(implementation_build_identities));
}

}  // namespace manyforge::deployment::detail
