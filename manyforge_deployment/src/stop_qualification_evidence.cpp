#include "stop_qualification_evidence_internal.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "content_identity_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

[[nodiscard]] const CanonicalValue::Object& require_object(
    const CanonicalValue& value, std::string_view path) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail_retained_bundle(std::string(path) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view path, std::size_t maximum,
    bool allow_empty = false) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr || (!allow_empty && array->empty()) ||
      array->size() > maximum) {
    fail_retained_bundle(std::string(path) +
                         " violates its closed array shape");
  }
  return *array;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view path) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& row) { return row.first == name; });
  if (found == object.end()) {
    fail_retained_bundle(std::string(path) + " is missing " +
                         std::string(name));
  }
  return found->second;
}

[[nodiscard]] const CanonicalValue* find_member(
    const CanonicalValue::Object& object, std::string_view name) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& row) { return row.first == name; });
  return found == object.end() ? nullptr : &found->second;
}

void require_exact_fields(
    const CanonicalValue::Object& object,
    std::initializer_list<std::string_view> expected,
    std::string_view path) {
  if (object.size() != expected.size()) {
    fail_retained_bundle(std::string(path) +
                         " has an incorrect closed field count");
  }
  for (const auto& [name, unused] : object) {
    static_cast<void>(unused);
    if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
      fail_retained_bundle(std::string(path) + " contains unknown member " +
                           name);
    }
  }
  for (const auto name : expected) {
    static_cast<void>(require_member(object, name, path));
  }
}

[[nodiscard]] const std::string& require_text(
    const CanonicalValue& value, std::string_view path) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr || text->empty()) {
    fail_retained_bundle(std::string(path) + " must be non-empty text");
  }
  return *text;
}

[[nodiscard]] std::string require_identity(const CanonicalValue& value,
                                           std::string_view path) {
  std::string result(require_text(value, path));
  require_retained_portable_identity(result, path);
  return result;
}

[[nodiscard]] core::ContentDigest require_digest(const CanonicalValue& value,
                                                 std::string_view path) {
  std::string result(require_text(value, path));
  require_retained_digest(result, path);
  return core::ContentDigest(std::move(result));
}

[[nodiscard]] std::uint64_t require_uint64(const CanonicalValue& value,
                                           std::string_view path,
                                           bool positive) {
  const auto* number = std::get_if<double>(&value.storage());
  constexpr double kMaximumIJsonInteger = 9007199254740991.0;
  if (number == nullptr || !std::isfinite(*number) ||
      std::trunc(*number) != *number || *number < 0.0 ||
      *number > kMaximumIJsonInteger || (positive && *number == 0.0)) {
    fail_retained_bundle(std::string(path) +
                         " must be an exact bounded non-negative integer");
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] CanonicalValue parse_canonical(std::string_view bytes,
                                             std::string_view label) {
  if (bytes.empty() || bytes.size() > kMaxStopQualificationEvidenceBytes) {
    fail_retained_bundle(std::string(label) +
                         " is empty or exceeds its byte bound");
  }
  CanonicalValue root;
  try {
    root = parse_strict_json_model_yaml(bytes).root();
  } catch (const std::exception& error) {
    fail_retained_bundle(std::string(label) + " is invalid JSON: " +
                         error.what());
  }
  if (canonical_json(root) != bytes) {
    fail_retained_bundle(std::string(label) +
                         " is not exact canonical JSON");
  }
  return root;
}

[[nodiscard]] core::ProviderContractRefDTO parse_provider_contract(
    const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(object,
                       {"provider_contract_role_id", "contract_schema_id",
                        "contract_schema_revision"},
                       path);
  return core::ProviderContractRefDTO{
      core::ProviderContractRoleId(require_identity(
          require_member(object, "provider_contract_role_id", path),
          std::string(path) + ".provider_contract_role_id")),
      core::SchemaId(require_identity(
          require_member(object, "contract_schema_id", path),
          std::string(path) + ".contract_schema_id")),
      core::SchemaRevision(require_digest(
          require_member(object, "contract_schema_revision", path),
          std::string(path) + ".contract_schema_revision").value())};
}

[[nodiscard]] CanonicalValue provider_contract_projection(
    const core::ProviderContractRefDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_role_id",
       CanonicalValue(value.provider_contract_role_id.value())},
      {"contract_schema_id", CanonicalValue(value.contract_schema_id.value())},
      {"contract_schema_revision",
       CanonicalValue(value.contract_schema_revision.value())},
  });
}

[[nodiscard]] std::string_view fault_case_token(
    StopQualificationFaultCase value) noexcept {
  switch (value) {
    case StopQualificationFaultCase::kNominalProviderCallBlocked:
      return "nominal_provider_call_blocked";
    case StopQualificationFaultCase::kNominalAdapterLockBlocked:
      return "nominal_adapter_lock_blocked";
    case StopQualificationFaultCase::kNominalResultWaitBlocked:
      return "nominal_result_wait_blocked";
  }
  return {};
}

[[nodiscard]] std::string_view substitution_case_token(
    ControlledStopSubstitutionQualificationCase value) noexcept {
  switch (value) {
    case ControlledStopSubstitutionQualificationCase::
        kSelectedBeforePhysicalDispatch:
      return "selected_before_physical_dispatch";
    case ControlledStopSubstitutionQualificationCase::kNominalScopePreserved:
      return "nominal_scope_preserved";
    case ControlledStopSubstitutionQualificationCase::
        kNominalStageBudgetPreserved:
      return "nominal_stage_budget_preserved";
    case ControlledStopSubstitutionQualificationCase::
        kRequestedExecutedReasonRetained:
      return "requested_executed_reason_retained";
  }
  return {};
}

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

[[nodiscard]] std::string require_build_input_id(
    const CanonicalValue& value, std::string_view path) {
  std::string result(require_text(value, path));
  if (!(result.front() >= 'a' && result.front() <= 'z') ||
      !std::all_of(result.begin() + 1, result.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
               byte == '-' || byte == ':';
      })) {
    fail_retained_bundle(std::string(path) +
                         " has an invalid logical build-input ID");
  }
  return result;
}

void verify_build_input_rows(const CanonicalValue& value,
                             std::string_view path,
                             std::initializer_list<std::string_view>
                                 required_prefixes = {}) {
  const auto& rows = require_array(value, path, 4096U);
  std::string previous;
  std::set<std::string, std::less<>> observed_prefixes;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string row_path =
        std::string(path) + "[" + std::to_string(index) + "]";
    const auto& row = require_object(rows[index], row_path);
    require_exact_fields(row, {"input_id", "content_digest"}, row_path);
    const std::string id = require_build_input_id(
        require_member(row, "input_id", row_path), row_path + ".input_id");
    static_cast<void>(require_digest(
        require_member(row, "content_digest", row_path),
        row_path + ".content_digest"));
    if (!previous.empty() && previous >= id) {
      fail_retained_bundle(std::string(path) +
                           " is not canonical and unique");
    }
    previous = id;
    for (const auto prefix : required_prefixes) {
      if (id.starts_with(prefix)) {
        observed_prefixes.emplace(prefix);
      }
    }
  }
  for (const auto prefix : required_prefixes) {
    if (!observed_prefixes.contains(std::string(prefix))) {
      fail_retained_bundle(std::string(path) +
                           " omits required input class " +
                           std::string(prefix));
    }
  }
}

[[nodiscard]] StopImplementationBuildIdentity
parse_implementation_build_closure(std::string_view canonical_bytes) {
  const CanonicalValue root = parse_canonical(
      canonical_bytes, "Stop implementation source/build closure");
  const auto& object = require_object(root, "stop_implementation_build");
  require_exact_fields(
      object,
      {"api_version", "implementation_id",
       "implementation_source_build_fingerprint", "source_inputs",
       "generated_semantic_inputs", "build_inputs", "installed_target"},
      "stop_implementation_build");
  if (require_text(require_member(object, "api_version",
                                  "stop_implementation_build"),
                   "stop_implementation_build.api_version") !=
      kStopImplementationBuildClosureApiVersion) {
    fail_retained_bundle(
        "Stop implementation build closure has an unsupported API");
  }
  verify_build_input_rows(
      require_member(object, "source_inputs", "stop_implementation_build"),
      "stop_implementation_build.source_inputs");
  verify_build_input_rows(
      require_member(object, "generated_semantic_inputs",
                     "stop_implementation_build"),
      "stop_implementation_build.generated_semantic_inputs");
  verify_build_input_rows(
      require_member(object, "build_inputs", "stop_implementation_build"),
      "stop_implementation_build.build_inputs",
      {"compile:", "link:", "toolchain:", "abi:"});
  const auto& target = require_object(
      require_member(object, "installed_target", "stop_implementation_build"),
      "stop_implementation_build.installed_target");
  require_exact_fields(target,
                       {"installed_target_id", "byte_size", "content_digest"},
                       "stop_implementation_build.installed_target");
  const std::string installed_target_id = require_identity(
      require_member(target, "installed_target_id",
                     "stop_implementation_build.installed_target"),
      "stop_implementation_build.installed_target.installed_target_id");
  const std::uint64_t expected_size = require_uint64(
      require_member(target, "byte_size",
                     "stop_implementation_build.installed_target"),
      "stop_implementation_build.installed_target.byte_size", true);
  const core::ContentDigest expected_digest = require_digest(
      require_member(target, "content_digest",
                     "stop_implementation_build.installed_target"),
      "stop_implementation_build.installed_target.content_digest");
  const core::ContentDigest fingerprint = require_digest(
      require_member(object, "implementation_source_build_fingerprint",
                     "stop_implementation_build"),
      "stop_implementation_build.implementation_source_build_fingerprint");
  const std::string expected_fingerprint = compute_content_identity(
      root, ContentIdentitySpec{
                .identity_domain =
                    "stop-implementation-source-build-fingerprint",
                .api_version =
                    std::string(kStopImplementationBuildClosureApiVersion),
                .own_field = "implementation_source_build_fingerprint",
            });
  if (fingerprint.value() != expected_fingerprint) {
    fail_retained_bundle(
        "Stop implementation source/build fingerprint is stale");
  }
  return {
      require_identity(require_member(object, "implementation_id",
                                      "stop_implementation_build"),
                       "stop_implementation_build.implementation_id"),
      fingerprint,
      installed_target_id,
      expected_size,
      expected_digest,
      std::string(canonical_bytes),
  };
}

[[nodiscard]] StopImplementationBuildIdentity
verify_implementation_build_closure(
    std::string_view canonical_bytes, std::string_view installed_target_bytes) {
  StopImplementationBuildIdentity identity =
      parse_implementation_build_closure(canonical_bytes);
  if (installed_target_bytes.empty() ||
      installed_target_bytes.size() >
          static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()) ||
      static_cast<std::uint64_t>(installed_target_bytes.size()) !=
          identity.installed_target_byte_size ||
      sha256_digest(installed_target_bytes) !=
          identity.installed_target_content_digest.value()) {
    fail_retained_bundle(
        "Stop implementation build closure does not match the exact "
        "installed target bytes");
  }
  return identity;
}

[[nodiscard]] core::ContentDigest verify_runner_contract(
    std::string_view canonical_bytes, std::string_view executable_bytes) {
  const CanonicalValue root = parse_canonical(
      canonical_bytes, "Stop qualification runner contract");
  const auto& object = require_object(root, "stop_qualification_runner");
  require_exact_fields(
      object,
      {"api_version", "qualification_runner_contract_digest", "runner_id",
       "source_inputs", "build_inputs", "executable", "case_contract"},
      "stop_qualification_runner");
  if (require_text(require_member(object, "api_version",
                                  "stop_qualification_runner"),
                   "stop_qualification_runner.api_version") !=
      kStopQualificationRunnerContractApiVersion) {
    fail_retained_bundle(
        "Stop qualification runner contract has an unsupported API");
  }
  static_cast<void>(require_identity(
      require_member(object, "runner_id", "stop_qualification_runner"),
      "stop_qualification_runner.runner_id"));
  verify_build_input_rows(
      require_member(object, "source_inputs", "stop_qualification_runner"),
      "stop_qualification_runner.source_inputs");
  verify_build_input_rows(
      require_member(object, "build_inputs", "stop_qualification_runner"),
      "stop_qualification_runner.build_inputs",
      {"compile:", "link:", "toolchain:", "abi:"});
  const auto& executable = require_object(
      require_member(object, "executable", "stop_qualification_runner"),
      "stop_qualification_runner.executable");
  require_exact_fields(executable, {"byte_size", "content_digest"},
                       "stop_qualification_runner.executable");
  const std::uint64_t expected_size = require_uint64(
      require_member(executable, "byte_size",
                     "stop_qualification_runner.executable"),
      "stop_qualification_runner.executable.byte_size", true);
  const core::ContentDigest expected_digest = require_digest(
      require_member(executable, "content_digest",
                     "stop_qualification_runner.executable"),
      "stop_qualification_runner.executable.content_digest");
  if (executable_bytes.empty() ||
      executable_bytes.size() >
          static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()) ||
      static_cast<std::uint64_t>(executable_bytes.size()) != expected_size ||
      sha256_digest(executable_bytes) != expected_digest.value()) {
    fail_retained_bundle(
        "Stop qualification runner contract does not match the exact executable bytes");
  }
  const auto& cases = require_object(
      require_member(object, "case_contract", "stop_qualification_runner"),
      "stop_qualification_runner.case_contract");
  require_exact_fields(cases,
                       {"implementation_fault_cases",
                        "controlled_substitution_cases"},
                       "stop_qualification_runner.case_contract");
  const auto require_fixed_tokens = [&](std::string_view field,
                                        std::span<const std::string_view> expected) {
    const std::string path =
        "stop_qualification_runner.case_contract." + std::string(field);
    const auto& rows = require_array(require_member(cases, field, path), path,
                                     expected.size());
    if (rows.size() != expected.size()) {
      fail_retained_bundle(path + " has an incorrect fixed case count");
    }
    for (std::size_t index = 0U; index < rows.size(); ++index) {
      if (require_text(rows[index], path) != expected[index]) {
        fail_retained_bundle(path + " differs from the fixed case contract");
      }
    }
  };
  constexpr std::array<std::string_view, 3U> kImplementationCases{
      "nominal_provider_call_blocked", "nominal_adapter_lock_blocked",
      "nominal_result_wait_blocked"};
  constexpr std::array<std::string_view, 4U> kSubstitutionCases{
      "selected_before_physical_dispatch", "nominal_scope_preserved",
      "nominal_stage_budget_preserved", "requested_executed_reason_retained"};
  require_fixed_tokens("implementation_fault_cases", kImplementationCases);
  require_fixed_tokens("controlled_substitution_cases", kSubstitutionCases);
  const core::ContentDigest digest = require_digest(
      require_member(object, "qualification_runner_contract_digest",
                     "stop_qualification_runner"),
      "stop_qualification_runner.qualification_runner_contract_digest");
  const std::string expected = compute_content_identity(
      root, ContentIdentitySpec{
                .identity_domain = "stop-qualification-runner-contract-digest",
                .api_version =
                    std::string(kStopQualificationRunnerContractApiVersion),
                .own_field = "qualification_runner_contract_digest",
            });
  if (digest.value() != expected) {
    fail_retained_bundle(
        "Stop qualification runner contract digest is stale");
  }
  return digest;
}

}  // namespace

StopImplementationBuildIdentity parse_stop_implementation_build_identity(
    std::string_view canonical_bytes) {
  return parse_implementation_build_closure(canonical_bytes);
}

StopImplementationBuildIdentity verify_stop_implementation_build_candidate(
    std::string_view canonical_bytes,
    std::string_view installed_target_bytes) {
  return verify_implementation_build_closure(canonical_bytes,
                                             installed_target_bytes);
}

core::ContentDigest verify_stop_qualification_runner_candidate(
    std::string_view canonical_bytes,
    std::string_view qualification_runner_executable_bytes) {
  return verify_runner_contract(canonical_bytes,
                                qualification_runner_executable_bytes);
}

BuildQualifiedStopEvidenceClosure::BuildQualifiedStopEvidenceClosure(
    std::optional<core::ContentDigest> implementation_runner_contract_digest,
    std::vector<BuildQualifiedStopImplementationEvidenceEntry>
        implementation_entries,
    std::optional<core::ContentDigest>
        controlled_substitution_runner_contract_digest,
    std::vector<BuildQualifiedControlledStopSubstitutionEvidenceEntry>
        controlled_substitution_entries,
    std::vector<StopImplementationBuildIdentity>
        implementation_build_identities)
    : implementation_runner_contract_digest_(
          std::move(implementation_runner_contract_digest)),
      implementation_entries_(std::move(implementation_entries)),
      controlled_substitution_runner_contract_digest_(
          std::move(controlled_substitution_runner_contract_digest)),
      controlled_substitution_entries_(
          std::move(controlled_substitution_entries)),
      implementation_build_identities_(
          std::move(implementation_build_identities)) {
  if (implementation_runner_contract_digest_.has_value() !=
          !implementation_entries_.empty() ||
      controlled_substitution_runner_contract_digest_.has_value() !=
          !controlled_substitution_entries_.empty() ||
      implementation_entries_.size() > kMaxStopQualificationEvidenceEntries ||
      controlled_substitution_entries_.size() >
          kMaxStopQualificationEvidenceEntries ||
      implementation_build_identities_.size() >
          kMaxStopQualificationEvidenceEntries ||
      (implementation_runner_contract_digest_.has_value() &&
       controlled_substitution_runner_contract_digest_.has_value() &&
       implementation_runner_contract_digest_ !=
           controlled_substitution_runner_contract_digest_) ||
      !std::is_sorted(implementation_entries_.begin(),
                      implementation_entries_.end(),
                      [](const auto& left, const auto& right) {
                        return implementation_key(left) <
                               implementation_key(right);
                      }) ||
      std::adjacent_find(
          implementation_entries_.begin(), implementation_entries_.end(),
          [](const auto& left, const auto& right) {
            return implementation_key(left) == implementation_key(right);
          }) != implementation_entries_.end() ||
      !std::is_sorted(controlled_substitution_entries_.begin(),
                      controlled_substitution_entries_.end(),
                      [](const auto& left, const auto& right) {
                        return substitution_key(left) < substitution_key(right);
                      }) ||
      std::adjacent_find(
          controlled_substitution_entries_.begin(),
          controlled_substitution_entries_.end(),
          [](const auto& left, const auto& right) {
            return substitution_key(left) == substitution_key(right);
          }) != controlled_substitution_entries_.end() ||
      !std::is_sorted(
          implementation_build_identities_.begin(),
          implementation_build_identities_.end(),
          [](const auto& left, const auto& right) {
            return left.implementation_id < right.implementation_id;
          }) ||
      std::adjacent_find(
          implementation_build_identities_.begin(),
          implementation_build_identities_.end(),
          [](const auto& left, const auto& right) {
            return left.implementation_id == right.implementation_id;
          }) != implementation_build_identities_.end()) {
    throw std::logic_error(
        "build-qualified Stop evidence closure is incoherent");
  }
}

std::pair<core::ContentDigest,
          std::vector<BuildQualifiedStopImplementationEvidenceEntry>>
parse_stop_implementation_qualification_evidence_package(
    std::string_view canonical_bytes) {
  const CanonicalValue root = parse_canonical(
      canonical_bytes, "Stop implementation qualification evidence package");
  const auto& object = require_object(root, "stop_qualification");
  require_exact_fields(object,
                       {"api_version", "qualification_runner_contract_digest",
                        "entries"},
                       "stop_qualification");
  if (require_text(require_member(object, "api_version", "stop_qualification"),
                   "stop_qualification.api_version") !=
      kStopImplementationQualificationEvidenceApiVersion) {
    fail_retained_bundle(
        "Stop implementation qualification evidence has an unsupported API");
  }
  core::ContentDigest runner_digest = require_digest(
      require_member(object, "qualification_runner_contract_digest",
                     "stop_qualification"),
      "stop_qualification.qualification_runner_contract_digest");
  const auto& rows = require_array(
      require_member(object, "entries", "stop_qualification"),
      "stop_qualification.entries", kMaxStopQualificationEvidenceEntries);
  std::vector<BuildQualifiedStopImplementationEvidenceEntry> result;
  result.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path =
        "stop_qualification.entries[" + std::to_string(index) + "]";
    const auto& row = require_object(rows[index], path);
    const bool has_endpoint = find_member(row, "endpoint_role_id") != nullptr;
    require_exact_fields(
        row,
        has_endpoint
            ? std::initializer_list<std::string_view>{
                  "implementation_id",
                  "implementation_source_build_fingerprint",
                  "provider_contract_ref", "product_contract_id",
                  "product_contract_revision", "registered_command_kind_id",
                  "endpoint_role_id", "invocation_lane_id",
                  "qualified_issue_time_bound_ns", "fault_case_results"}
            : std::initializer_list<std::string_view>{
                  "implementation_id",
                  "implementation_source_build_fingerprint",
                  "provider_contract_ref", "product_contract_id",
                  "product_contract_revision", "registered_command_kind_id",
                  "invocation_lane_id", "qualified_issue_time_bound_ns",
                  "fault_case_results"},
        path);
    const auto& fault_rows = require_array(
        require_member(row, "fault_case_results", path),
        path + ".fault_case_results", 3U);
    if (fault_rows.size() != 3U) {
      fail_retained_bundle(path +
                           ".fault_case_results must contain three cases");
    }
    std::array<StopQualificationFaultCaseResult, 3U> fault_results{};
    const std::array<StopQualificationFaultCase, 3U> expected_cases{
        StopQualificationFaultCase::kNominalProviderCallBlocked,
        StopQualificationFaultCase::kNominalAdapterLockBlocked,
        StopQualificationFaultCase::kNominalResultWaitBlocked};
    const std::uint64_t issue_bound = require_uint64(
        require_member(row, "qualified_issue_time_bound_ns", path),
        path + ".qualified_issue_time_bound_ns", true);
    for (std::size_t fault_index = 0U; fault_index < fault_rows.size();
         ++fault_index) {
      const std::string fault_path = path + ".fault_case_results[" +
                                     std::to_string(fault_index) + "]";
      const auto& fault = require_object(fault_rows[fault_index], fault_path);
      require_exact_fields(
          fault, {"fault_case", "outcome", "maximum_observed_issue_latency_ns"},
          fault_path);
      const auto expected = expected_cases[fault_index];
      const std::uint64_t observed = require_uint64(
          require_member(fault, "maximum_observed_issue_latency_ns", fault_path),
          fault_path + ".maximum_observed_issue_latency_ns", false);
      if (require_text(require_member(fault, "fault_case", fault_path),
                       fault_path + ".fault_case") != fault_case_token(expected) ||
          require_text(require_member(fault, "outcome", fault_path),
                       fault_path + ".outcome") != "passed" ||
          observed > issue_bound) {
        fail_retained_bundle(fault_path +
                             " is not a successful bounded fixed case");
      }
      fault_results[fault_index] = {expected, observed};
    }

    const auto command_id = core::RegisteredStopCommandKindId(require_identity(
        require_member(row, "registered_command_kind_id", path),
        path + ".registered_command_kind_id"));
    const auto* command =
        core::find_registered_stop_command_definition(command_id);
    if (command == nullptr ||
        command->stop_action_class != core::StopActionClass::kSoftwareHard) {
      fail_retained_bundle(path +
                           " does not name a registered software-hard command");
    }
    result.push_back(BuildQualifiedStopImplementationEvidenceEntry{
        require_identity(require_member(row, "implementation_id", path),
                         path + ".implementation_id"),
        require_digest(
            require_member(row, "implementation_source_build_fingerprint", path),
            path + ".implementation_source_build_fingerprint"),
        parse_provider_contract(
            require_member(row, "provider_contract_ref", path),
            path + ".provider_contract_ref"),
        core::ProductContractId(require_identity(
            require_member(row, "product_contract_id", path),
            path + ".product_contract_id")),
        core::ProductContractRevision(require_digest(
            require_member(row, "product_contract_revision", path),
            path + ".product_contract_revision").value()),
        command_id,
        has_endpoint
            ? std::optional<core::EndpointRoleId>(core::EndpointRoleId(
                  require_identity(require_member(row, "endpoint_role_id", path),
                                   path + ".endpoint_role_id")))
            : std::nullopt,
        core::StopInvocationLaneId(require_identity(
            require_member(row, "invocation_lane_id", path),
            path + ".invocation_lane_id")),
        issue_bound,
        fault_results});
  }
  if (!std::is_sorted(result.begin(), result.end(),
                      [](const auto& left, const auto& right) {
                        return implementation_key(left) <
                               implementation_key(right);
                      }) ||
      std::adjacent_find(result.begin(), result.end(),
                         [](const auto& left, const auto& right) {
                           return implementation_key(left) ==
                                  implementation_key(right);
                         }) != result.end()) {
    fail_retained_bundle(
        "Stop implementation evidence entries are not canonical and unique");
  }
  return {std::move(runner_digest), std::move(result)};
}

std::pair<
    core::ContentDigest,
    std::vector<BuildQualifiedControlledStopSubstitutionEvidenceEntry>>
parse_controlled_stop_substitution_qualification_evidence_package(
    std::string_view canonical_bytes) {
  const CanonicalValue root = parse_canonical(
      canonical_bytes, "controlled Stop substitution evidence package");
  const auto& object = require_object(root, "stop_substitution_qualification");
  require_exact_fields(object,
                       {"api_version", "qualification_runner_contract_digest",
                        "entries"},
                       "stop_substitution_qualification");
  if (require_text(require_member(object, "api_version",
                                  "stop_substitution_qualification"),
                   "stop_substitution_qualification.api_version") !=
      kControlledStopSubstitutionQualificationEvidenceApiVersion) {
    fail_retained_bundle(
        "controlled Stop substitution evidence has an unsupported API");
  }
  core::ContentDigest runner_digest = require_digest(
      require_member(object, "qualification_runner_contract_digest",
                     "stop_substitution_qualification"),
      "stop_substitution_qualification.qualification_runner_contract_digest");
  const auto& rows = require_array(
      require_member(object, "entries", "stop_substitution_qualification"),
      "stop_substitution_qualification.entries",
      kMaxStopQualificationEvidenceEntries);
  std::vector<BuildQualifiedControlledStopSubstitutionEvidenceEntry> result;
  result.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = "stop_substitution_qualification.entries[" +
                             std::to_string(index) + "]";
    const auto& row = require_object(rows[index], path);
    require_exact_fields(
        row,
        {"implementation_id", "implementation_source_build_fingerprint",
         "provider_contract_ref", "product_contract_id",
         "product_contract_revision", "requested_stop_mode",
         "executed_stop_mode", "case_results"},
        path);
    if (require_text(require_member(row, "requested_stop_mode", path),
                     path + ".requested_stop_mode") != "soft_on_path" ||
        require_text(require_member(row, "executed_stop_mode", path),
                     path + ".executed_stop_mode") != "spring_back_hold") {
      fail_retained_bundle(path + " selects an unqualified substitution pair");
    }
    const auto& case_rows = require_array(
        require_member(row, "case_results", path), path + ".case_results", 4U);
    if (case_rows.size() != 4U) {
      fail_retained_bundle(path + ".case_results must contain four cases");
    }
    const std::array<ControlledStopSubstitutionQualificationCase, 4U>
        expected_cases{
            ControlledStopSubstitutionQualificationCase::
                kSelectedBeforePhysicalDispatch,
            ControlledStopSubstitutionQualificationCase::kNominalScopePreserved,
            ControlledStopSubstitutionQualificationCase::
                kNominalStageBudgetPreserved,
            ControlledStopSubstitutionQualificationCase::
                kRequestedExecutedReasonRetained};
    for (std::size_t case_index = 0U; case_index < case_rows.size();
         ++case_index) {
      const std::string case_path = path + ".case_results[" +
                                    std::to_string(case_index) + "]";
      const auto& case_row = require_object(case_rows[case_index], case_path);
      require_exact_fields(case_row, {"case", "outcome"}, case_path);
      if (require_text(require_member(case_row, "case", case_path),
                       case_path + ".case") !=
              substitution_case_token(expected_cases[case_index]) ||
          require_text(require_member(case_row, "outcome", case_path),
                       case_path + ".outcome") != "passed") {
        fail_retained_bundle(case_path + " is not a successful fixed case");
      }
    }
    result.push_back(BuildQualifiedControlledStopSubstitutionEvidenceEntry{
        require_identity(require_member(row, "implementation_id", path),
                         path + ".implementation_id"),
        require_digest(
            require_member(row, "implementation_source_build_fingerprint", path),
            path + ".implementation_source_build_fingerprint"),
        parse_provider_contract(
            require_member(row, "provider_contract_ref", path),
            path + ".provider_contract_ref"),
        core::ProductContractId(require_identity(
            require_member(row, "product_contract_id", path),
            path + ".product_contract_id")),
        core::ProductContractRevision(require_digest(
            require_member(row, "product_contract_revision", path),
            path + ".product_contract_revision").value()),
        core::MotionControlledStopMode::kSoftOnPath,
        core::MotionControlledStopMode::kSpringBackHold,
        expected_cases});
  }
  if (!std::is_sorted(result.begin(), result.end(),
                      [](const auto& left, const auto& right) {
                        return substitution_key(left) < substitution_key(right);
                      }) ||
      std::adjacent_find(result.begin(), result.end(),
                         [](const auto& left, const auto& right) {
                           return substitution_key(left) ==
                                  substitution_key(right);
                         }) != result.end()) {
    fail_retained_bundle(
        "controlled Stop substitution entries are not canonical and unique");
  }
  return {std::move(runner_digest), std::move(result)};
}

std::string emit_stop_implementation_qualification_evidence_package(
    const core::ContentDigest& qualification_runner_contract_digest,
    std::span<const BuildQualifiedStopImplementationEvidenceEntry> entries) {
  require_retained_digest(qualification_runner_contract_digest.value(),
                          "qualification_runner_contract_digest");
  if (entries.empty() ||
      entries.size() > kMaxStopQualificationEvidenceEntries) {
    fail_retained_bundle(
        "Stop implementation evidence projection has invalid cardinality");
  }
  CanonicalValue::Array rows;
  rows.reserve(entries.size());
  for (const auto& entry : entries) {
    CanonicalValue::Array faults;
    for (const auto& fault : entry.fault_case_results) {
      faults.emplace_back(CanonicalValue::Object{
          {"fault_case",
           CanonicalValue(std::string(fault_case_token(fault.fault_case)))},
          {"outcome", CanonicalValue("passed")},
          {"maximum_observed_issue_latency_ns",
           CanonicalValue(static_cast<double>(
               fault.maximum_observed_issue_latency_ns))},
      });
    }
    CanonicalValue::Object row{
        {"implementation_id", CanonicalValue(entry.implementation_id)},
        {"implementation_source_build_fingerprint",
         CanonicalValue(entry.implementation_source_build_fingerprint.value())},
        {"provider_contract_ref",
         provider_contract_projection(entry.provider_contract_ref)},
        {"product_contract_id",
         CanonicalValue(entry.product_contract_id.value())},
        {"product_contract_revision",
         CanonicalValue(entry.product_contract_revision.value())},
        {"registered_command_kind_id",
         CanonicalValue(entry.registered_command_kind_id.value())},
        {"invocation_lane_id",
         CanonicalValue(entry.invocation_lane_id.value())},
        {"qualified_issue_time_bound_ns",
         CanonicalValue(
             static_cast<double>(entry.qualified_issue_time_bound_ns))},
        {"fault_case_results", CanonicalValue(std::move(faults))},
    };
    if (entry.endpoint_role_id.has_value()) {
      row.emplace_back("endpoint_role_id",
                       CanonicalValue(entry.endpoint_role_id->value()));
    }
    rows.emplace_back(std::move(row));
  }
  const std::string bytes = canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(
           std::string(kStopImplementationQualificationEvidenceApiVersion))},
      {"qualification_runner_contract_digest",
       CanonicalValue(qualification_runner_contract_digest.value())},
      {"entries", CanonicalValue(std::move(rows))},
  }));
  static_cast<void>(
      parse_stop_implementation_qualification_evidence_package(bytes));
  return bytes;
}

std::string emit_controlled_stop_substitution_qualification_evidence_package(
    const core::ContentDigest& qualification_runner_contract_digest,
    std::span<const BuildQualifiedControlledStopSubstitutionEvidenceEntry>
        entries) {
  require_retained_digest(qualification_runner_contract_digest.value(),
                          "qualification_runner_contract_digest");
  if (entries.empty() ||
      entries.size() > kMaxStopQualificationEvidenceEntries) {
    fail_retained_bundle(
        "controlled Stop substitution projection has invalid cardinality");
  }
  CanonicalValue::Array rows;
  rows.reserve(entries.size());
  for (const auto& entry : entries) {
    CanonicalValue::Array cases;
    for (const auto value : entry.case_results) {
      cases.emplace_back(CanonicalValue::Object{
          {"case", CanonicalValue(std::string(substitution_case_token(value)))},
          {"outcome", CanonicalValue("passed")},
      });
    }
    rows.emplace_back(CanonicalValue::Object{
        {"implementation_id", CanonicalValue(entry.implementation_id)},
        {"implementation_source_build_fingerprint",
         CanonicalValue(entry.implementation_source_build_fingerprint.value())},
        {"provider_contract_ref",
         provider_contract_projection(entry.provider_contract_ref)},
        {"product_contract_id",
         CanonicalValue(entry.product_contract_id.value())},
        {"product_contract_revision",
         CanonicalValue(entry.product_contract_revision.value())},
        {"requested_stop_mode", CanonicalValue("soft_on_path")},
        {"executed_stop_mode", CanonicalValue("spring_back_hold")},
        {"case_results", CanonicalValue(std::move(cases))},
    });
  }
  const std::string bytes = canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(
           kControlledStopSubstitutionQualificationEvidenceApiVersion))},
      {"qualification_runner_contract_digest",
       CanonicalValue(qualification_runner_contract_digest.value())},
      {"entries", CanonicalValue(std::move(rows))},
  }));
  static_cast<void>(
      parse_controlled_stop_substitution_qualification_evidence_package(bytes));
  return bytes;
}

}  // namespace manyforge::deployment::detail
