#include "manyforge_headless/program_apply_owner.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/scenario_catalog.hpp>

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::string_view kPreparationIdentityDomain =
    "manyforge-program-apply-preparation";
constexpr std::string_view kAppliedRecordIdentityDomain =
    "manyforge-applied-program-record";
constexpr std::string_view kRequestIntentIdentityDomain =
    "manyforge-program-apply-request-intent";
constexpr std::string_view kRequestIntentSchemaVersion =
    "manyforge.program_apply_request_intent.v1";
constexpr std::size_t kMaximumOwnerStatePayloadBytes = 12U * 1024U * 1024U;

[[noreturn]] void Fail(std::string code, std::string message) {
  throw ProgramApplyError(std::move(code), std::move(message));
}

void Require(const bool condition, std::string code, std::string message) {
  if (!condition) {
    Fail(std::move(code), std::move(message));
  }
}

[[nodiscard]] bool IsIdentifier(const std::string_view value) noexcept {
  if (value.empty() || value.size() > 256U || value.front() < 'a' ||
      value.front() > 'z') {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](const char item) {
    return (item >= 'a' && item <= 'z') || (item >= '0' && item <= '9') ||
           item == '_' || item == '.' || item == '-';
  });
}

[[nodiscard]] const CanonicalValue &Member(const CanonicalValue &root,
                                           const std::string_view name,
                                           const std::string_view code) {
  const auto *value = root.find_object_member(name);
  Require(value != nullptr, std::string(code),
          "Program Apply value is missing field " + std::string(name));
  return *value;
}

[[nodiscard]] std::string Text(const CanonicalValue &root,
                               const std::string_view name,
                               const std::string_view code) {
  const auto *value =
      std::get_if<std::string>(&Member(root, name, code).storage());
  Require(value != nullptr && !value->empty(), std::string(code),
          "Program Apply field " + std::string(name) +
              " must be non-empty text");
  return *value;
}

[[nodiscard]] std::uint64_t Unsigned(const CanonicalValue &root,
                                     const std::string_view name,
                                     const std::string_view code) {
  const auto *value =
      std::get_if<double>(&Member(root, name, code).storage());
  Require(value != nullptr && *value >= 1.0 &&
              *value <= 9007199254740991.0 &&
              static_cast<double>(static_cast<std::uint64_t>(*value)) == *value,
          std::string(code), "Program selection field " + std::string(name) +
                                 " must be a positive safe integer");
  return static_cast<std::uint64_t>(*value);
}

struct Preparation final {
  std::string scenario_id;
  core::ScenarioCatalogRevision scenario_catalog_revision;
  core::DeploymentArtifactId deployment_artifact_id;
  core::DeploymentRevision deployment_revision;
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
  core::ContentDigest node_environment_digest;
  core::ProgramBindingId baseline_program_binding_id;
  core::ProgramId program_id;
  core::ProgramRevision program_revision;
  core::ProgramBindingId program_binding_id;
  std::string assistant_mode;
  CanonicalValue typed_parameter_overrides;
  core::ContentDigest preparation_digest;
};

[[nodiscard]] CanonicalValue
PreparationIdentityValue(const Preparation &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"baseline_program_binding_id",
       CanonicalValue(value.baseline_program_binding_id.value())},
      {"assistant_mode", CanonicalValue(value.assistant_mode)},
      {"deployment_artifact_id",
       CanonicalValue(value.deployment_artifact_id.value())},
      {"deployment_revision",
       CanonicalValue(value.deployment_revision.value())},
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"node_environment_digest",
       CanonicalValue(value.node_environment_digest.value())},
      {"program_binding_id", CanonicalValue(value.program_binding_id.value())},
      {"program_id", CanonicalValue(value.program_id.value())},
      {"program_revision", CanonicalValue(value.program_revision.value())},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"scenario_catalog_revision",
       CanonicalValue(value.scenario_catalog_revision.value())},
      {"scenario_id", CanonicalValue(value.scenario_id)},
      {"typed_parameter_overrides", value.typed_parameter_overrides},
  });
}

[[nodiscard]] core::ContentDigest PreparationDigest(const Preparation &value) {
  return core::ContentDigest(content_identity::framed_sha256(
      kPreparationIdentityDomain, kProgramApplyPreparationSchemaVersion,
      PreparationIdentityValue(value)));
}

[[nodiscard]] CanonicalValue
PreparationValue(const Preparation &value,
                 const std::string_view schema_version) {
  return CanonicalValue(CanonicalValue::Object{
      {"baseline_program_binding_id",
       CanonicalValue(value.baseline_program_binding_id.value())},
      {"assistant_mode", CanonicalValue(value.assistant_mode)},
      {"deployment_artifact_id",
       CanonicalValue(value.deployment_artifact_id.value())},
      {"deployment_revision",
       CanonicalValue(value.deployment_revision.value())},
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"node_environment_digest",
       CanonicalValue(value.node_environment_digest.value())},
      {"preparation_digest", CanonicalValue(value.preparation_digest.value())},
      {"program_binding_id", CanonicalValue(value.program_binding_id.value())},
      {"program_id", CanonicalValue(value.program_id.value())},
      {"program_revision", CanonicalValue(value.program_revision.value())},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"scenario_catalog_revision",
       CanonicalValue(value.scenario_catalog_revision.value())},
      {"scenario_id", CanonicalValue(value.scenario_id)},
      {"schema_version", CanonicalValue(std::string(schema_version))},
      {"typed_parameter_overrides", value.typed_parameter_overrides},
  });
}

[[nodiscard]] Preparation
DecodePreparationValue(const CanonicalValue &root,
                       const std::string_view expected_schema,
                       const std::string_view code) {
  Require(root.is_object() && root.as_object().size() == 15U, std::string(code),
          "Program Apply value must be one exact closed object");
  Require(Text(root, "schema_version", code) == expected_schema,
          std::string(code), "Program Apply schema version is unsupported");
  Preparation result{
      Text(root, "scenario_id", code),
      core::ScenarioCatalogRevision(
          Text(root, "scenario_catalog_revision", code)),
      core::DeploymentArtifactId(Text(root, "deployment_artifact_id", code)),
      core::DeploymentRevision(Text(root, "deployment_revision", code)),
      core::RetainedBundleId(Text(root, "retained_bundle_id", code)),
      core::GraphRevision(Text(root, "graph_revision", code)),
      core::ContentDigest(Text(root, "node_environment_digest", code)),
      core::ProgramBindingId(Text(root, "baseline_program_binding_id", code)),
      core::ProgramId(Text(root, "program_id", code)),
      core::ProgramRevision(Text(root, "program_revision", code)),
      core::ProgramBindingId(Text(root, "program_binding_id", code)),
      Text(root, "assistant_mode", code),
      Member(root, "typed_parameter_overrides", code),
      core::ContentDigest(Text(root, "preparation_digest", code)),
  };
  Require(
      IsIdentifier(result.scenario_id) &&
          IsIdentifier(result.deployment_artifact_id.value()) &&
          IsIdentifier(result.program_id.value()) &&
          content_identity::is_sha256_digest(
              result.scenario_catalog_revision.value()) &&
          content_identity::is_sha256_digest(
              result.deployment_revision.value()) &&
          content_identity::is_sha256_digest(
              result.retained_bundle_id.value()) &&
          content_identity::is_sha256_digest(result.graph_revision.value()) &&
          content_identity::is_sha256_digest(
              result.node_environment_digest.value()) &&
          content_identity::is_sha256_digest(
              result.baseline_program_binding_id.value()) &&
          content_identity::is_sha256_digest(result.program_revision.value()) &&
          content_identity::is_sha256_digest(
              result.program_binding_id.value()) &&
          (result.assistant_mode == "required" ||
           result.assistant_mode == "optional" ||
           result.assistant_mode == "forbidden") &&
          result.typed_parameter_overrides.is_object() &&
          content_identity::is_sha256_digest(
              result.preparation_digest.value()) &&
          result.preparation_digest == PreparationDigest(result),
      std::string(code), "Program Apply preparation identity is invalid");
  return result;
}

[[nodiscard]] Preparation
DecodePreparation(const std::string_view canonical_bytes) {
  Require(!canonical_bytes.empty() &&
              canonical_bytes.size() <= deployment::kMaxArtifactEncodedBytes,
          "program_apply_preparation_invalid",
          "Program Apply preparation is empty or oversized");
  CanonicalValue root;
  try {
    root = deployment::parse_strict_json_model_yaml(canonical_bytes).root();
  } catch (const std::exception &error) {
    Fail("program_apply_preparation_invalid",
         "Program Apply preparation is not strict JSON: " +
             std::string(error.what()));
  }
  Require(content_identity::canonical_json(root) == canonical_bytes,
          "program_apply_preparation_invalid",
          "Program Apply preparation is not canonical JSON");
  return DecodePreparationValue(root, kProgramApplyPreparationSchemaVersion,
                                "program_apply_preparation_invalid");
}

[[nodiscard]] core::ProgramRoleAssignmentDTO
AssignmentFrom(const deployment::ProgramBinding &binding) {
  core::ProgramRoleAssignmentDTO result;
  result.component_roles.reserve(
      binding.value().component_role_bindings.size());
  for (const auto &row : binding.value().component_role_bindings) {
    result.component_roles.push_back(core::ProgramComponentRoleAssignmentDTO{
        row.component_role_id, row.controllable_component_id});
  }
  result.target_roles.reserve(binding.value().target_role_bindings.size());
  for (const auto &row : binding.value().target_role_bindings) {
    result.target_roles.push_back(core::ProgramTargetRoleAssignmentDTO{
        row.target_role_id, row.command_target_id});
  }
  return result;
}

[[nodiscard]] deployment::VerifiedRetainedModelMap LoadModelMap(
    const deployment::RetainedBundle &bundle,
    const deployment::FilesystemVerifiedRetainedModelMapSource &source) {
  Require(bundle.value().compiled_products.symbol_role_maps.size() == 1U,
          "program_apply_bundle_invalid",
          "Program Apply retained bundle must select one exact model map");
  auto model_map = source.get_verified(deployment::RetainedModelMapQuery{
      bundle.retained_bundle_id(),
      bundle.value().compiled_products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(), "program_apply_bundle_invalid",
          "Program Apply retained model map is absent or unverifiable");
  return std::move(*model_map);
}

template <typename Function>
void ForEachProduct(const core::RetainedCompiledProductsDTO &products,
                    Function function) {
  function(products.planning_collision_model);
  if (products.viewer_model.has_value()) {
    function(*products.viewer_model);
  }
  for (const auto &group : products.control_domain_products) {
    for (const auto &product : group.products) {
      function(product);
    }
  }
  for (const auto &product : products.symbol_role_maps) {
    function(product);
  }
  for (const auto &group : products.runtime_unit_products) {
    for (const auto &product : group.products) {
      function(product);
    }
  }
}

[[nodiscard]] std::string AppliedRecordDigest(const CanonicalValue &record) {
  return content_identity::framed_sha256(
      kAppliedRecordIdentityDomain, kAppliedProgramSelectionSchemaVersion,
      record);
}

[[nodiscard]] std::string RequestIntentDigest(
    const std::string_view scenario_id,
    const std::string_view scenario_catalog_revision,
    const std::string_view program_id, const std::string_view program_revision,
    const std::optional<std::string> &expected_applied_record_digest) {
  return content_identity::framed_sha256(
      kRequestIntentIdentityDomain, kRequestIntentSchemaVersion,
      CanonicalValue(CanonicalValue::Object{
          {"expected_applied_record_digest",
           expected_applied_record_digest.has_value()
               ? CanonicalValue(*expected_applied_record_digest)
               : CanonicalValue(nullptr)},
          {"program_id", CanonicalValue(std::string(program_id))},
          {"program_revision", CanonicalValue(std::string(program_revision))},
          {"scenario_catalog_revision",
           CanonicalValue(std::string(scenario_catalog_revision))},
          {"scenario_id", CanonicalValue(std::string(scenario_id))},
      }));
}

[[nodiscard]] std::string RequestIntentDigest(
    const CanonicalValue &candidate,
    const std::optional<std::string> &expected_applied_record_digest) {
  constexpr std::string_view code = "program_apply_preparation_invalid";
  return RequestIntentDigest(
      Text(candidate, "scenario_id", code),
      Text(candidate, "scenario_catalog_revision", code),
      Text(candidate, "program_id", code),
      Text(candidate, "program_revision", code),
      expected_applied_record_digest);
}

struct RequestLedgerEntry final {
  std::string request_id;
  std::string intent_digest;
  std::uint64_t generation;
  CanonicalValue record;
  std::string record_digest;
  std::string selection_source_owner_id;
  std::string selection_source_owner_epoch;
};

[[nodiscard]] CanonicalValue LedgerEntryValue(
    const RequestLedgerEntry &entry) {
  return CanonicalValue(CanonicalValue::Object{
      {"applied_generation",
       CanonicalValue(static_cast<double>(entry.generation))},
      {"applied_record", entry.record},
      {"applied_record_digest", CanonicalValue(entry.record_digest)},
      {"intent_digest", CanonicalValue(entry.intent_digest)},
      {"request_id", CanonicalValue(entry.request_id)},
      {"selection_source_owner_epoch",
       CanonicalValue(entry.selection_source_owner_epoch)},
      {"selection_source_owner_id",
       CanonicalValue(entry.selection_source_owner_id)},
  });
}

[[nodiscard]] CanonicalValue OwnerStateValue(
    const CanonicalValue &record, const std::string &record_digest,
    const std::uint64_t generation, const std::string_view request_owner_epoch,
    const std::vector<RequestLedgerEntry> &requests) {
  CanonicalValue::Array request_values;
  request_values.reserve(requests.size());
  for (const auto &request : requests) {
    request_values.push_back(LedgerEntryValue(request));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"applied_generation", CanonicalValue(static_cast<double>(generation))},
      {"applied_record", record},
      {"applied_record_digest", CanonicalValue(record_digest)},
      {"requests", CanonicalValue(std::move(request_values))},
      {"request_owner_epoch",
       CanonicalValue(std::string(request_owner_epoch))},
      {"schema_version",
       CanonicalValue(std::string(kProgramSelectionOwnerStateSchemaVersion))},
  });
}

struct DecodedOwnerState final {
  std::uint64_t generation;
  CanonicalValue record;
  std::string record_digest;
  std::string request_owner_epoch;
  std::vector<RequestLedgerEntry> requests;
};

[[nodiscard]] bool IsBoundedIdentityText(const std::string_view value) {
  return !value.empty() && value.size() <= kMaximumSourceOwnerIdentifierBytes &&
         content_identity::is_utf8_nfc(value);
}

[[nodiscard]] RequestLedgerEntry
DecodeLedgerEntry(const CanonicalValue &value) {
  constexpr std::string_view code = "program_applied_selection_invalid";
  Require(value.is_object() && value.as_object().size() == 7U,
          std::string(code),
          "Program Apply request ledger entry must be one exact closed object");
  RequestLedgerEntry result{
      Text(value, "request_id", code),
      Text(value, "intent_digest", code),
      Unsigned(value, "applied_generation", code),
      Member(value, "applied_record", code),
      Text(value, "applied_record_digest", code),
      Text(value, "selection_source_owner_id", code),
      Text(value, "selection_source_owner_epoch", code),
  };
  Require(IsBoundedIdentityText(result.request_id) &&
              content_identity::is_sha256_digest(result.intent_digest) &&
              content_identity::is_sha256_digest(result.record_digest) &&
              AppliedRecordDigest(result.record) == result.record_digest &&
              IsBoundedIdentityText(result.selection_source_owner_id) &&
              IsBoundedIdentityText(result.selection_source_owner_epoch),
          std::string(code),
          "Program Apply request ledger entry has an invalid identity");
  static_cast<void>(DecodePreparationValue(
      result.record, kAppliedProgramSelectionSchemaVersion, code));
  return result;
}

[[nodiscard]] DecodedOwnerState DecodeOwnerState(const CanonicalValue &value) {
  constexpr std::string_view code = "program_applied_selection_invalid";
  Require(value.is_object() && value.as_object().size() == 6U,
          std::string(code),
          "Program selection owner state must be one exact closed object");
  Require(Text(value, "schema_version", code) ==
              kProgramSelectionOwnerStateSchemaVersion,
          std::string(code),
          "Program selection owner state version is invalid");
  const auto &request_values = Member(value, "requests", code);
  Require(request_values.is_array() && !request_values.as_array().empty() &&
              request_values.as_array().size() <=
                  kMaximumProgramApplyRequestLedgerEntries,
          std::string(code),
          "Program selection request ledger size is invalid");
  DecodedOwnerState result{
      Unsigned(value, "applied_generation", code),
      Member(value, "applied_record", code),
      Text(value, "applied_record_digest", code),
      Text(value, "request_owner_epoch", code),
      {},
  };
  Require(IsBoundedIdentityText(result.request_owner_epoch), std::string(code),
          "Program selection request owner epoch is invalid");
  result.requests.reserve(request_values.as_array().size());
  std::set<std::string> request_ids;
  const RequestLedgerEntry *previous = nullptr;
  for (const auto &request_value : request_values.as_array()) {
    auto request = DecodeLedgerEntry(request_value);
    Require(request_ids.insert(request.request_id).second, std::string(code),
            "Program selection request ledger contains a duplicate request ID");
    Require(request.generation <= result.generation &&
                (previous == nullptr ||
                 (request.generation == previous->generation &&
                  request.record_digest == previous->record_digest) ||
                 (request.generation == previous->generation + 1U &&
                  request.record_digest != previous->record_digest)),
            std::string(code),
            "Program selection request ledger generations are inconsistent");
    result.requests.push_back(std::move(request));
    previous = &result.requests.back();
  }
  Require(content_identity::is_sha256_digest(result.record_digest) &&
              AppliedRecordDigest(result.record) == result.record_digest,
          std::string(code),
          "Program selection owner state has a divergent record digest");
  static_cast<void>(DecodePreparationValue(
      result.record, kAppliedProgramSelectionSchemaVersion, code));
  const auto &latest = result.requests.back();
  Require(latest.generation == result.generation &&
              latest.record_digest == result.record_digest &&
              latest.record == result.record,
          std::string(code),
          "Program selection request ledger does not end at current Applied");
  return result;
}

void RequireLedgerOwner(const DecodedOwnerState &state,
                        const std::string_view source_owner_id,
                        const std::string_view source_owner_epoch) {
  Require(std::all_of(state.requests.begin(), state.requests.end(),
                      [&](const RequestLedgerEntry &entry) {
                        return entry.selection_source_owner_id ==
                               source_owner_id;
                      }) &&
              state.requests.back().selection_source_owner_epoch ==
                  source_owner_epoch,
          "program_applied_selection_invalid",
          "Program selection request ledger and outer owner disagree");
}

[[nodiscard]] std::string
AssistantModeText(const deployment::ScenarioAssistantMode mode) {
  switch (mode) {
  case deployment::ScenarioAssistantMode::kRequired:
    return "required";
  case deployment::ScenarioAssistantMode::kOptional:
    return "optional";
  case deployment::ScenarioAssistantMode::kForbidden:
    return "forbidden";
  }
  Fail("program_applied_selection_invalid",
       "Applied Program has an invalid assistant mode");
}

} // namespace

ProgramApplyError::ProgramApplyError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

ProgramSelectionOwner::ProgramSelectionOwner(
    std::filesystem::path install_root, std::filesystem::path artifact_root,
    std::filesystem::path owner_state_root, std::string owner_id,
    std::string owner_epoch)
    : scenario_source_(install_root, "maintained"),
      bundle_source_(install_root), model_map_source_(install_root),
      catalog_source_(install_root), raw_artifact_source_(install_root),
      baseline_binding_source_(install_root),
      raw_artifact_store_(artifact_root), catalog_store_(artifact_root),
      bundle_repository_(artifact_root), program_repository_(artifact_root),
      binding_repository_(artifact_root), artifact_bundle_source_(artifact_root),
      artifact_model_map_source_(artifact_root),
      owner_state_(std::move(owner_state_root),
                   std::string(kProgramSelectionOwnerStateFile)),
      source_owner_id_(std::move(owner_id)),
      source_owner_epoch_(std::move(owner_epoch)) {
  Require(IsBoundedIdentityText(source_owner_id_) &&
              IsBoundedIdentityText(source_owner_epoch_),
          "program_apply_configuration_invalid",
          "Program Apply source-owner identities are required");
}

CanonicalValue ProgramSelectionOwner::Prepare(
    const std::string_view scenario_id,
    const std::string_view expected_scenario_catalog_revision,
    const std::string_view canonical_program_bytes,
    const std::string_view active_retained_bundle_id,
    const std::string_view active_graph_revision) {
  std::lock_guard lock(mutex_);
  try {
    Require(
        content_identity::is_sha256_digest(expected_scenario_catalog_revision) &&
            content_identity::is_sha256_digest(active_retained_bundle_id) &&
            content_identity::is_sha256_digest(active_graph_revision),
        "program_apply_request_invalid",
        "Program Apply expected or active identity is invalid");
    const auto scenario =
        scenario_source_.LoadExactProgramSelection(scenario_id);
    Require(scenario.scenario_catalog_revision.value() ==
                expected_scenario_catalog_revision,
            "program_apply_scenario_catalog_stale",
            "Program Apply targets a stale scenario-catalog revision");
    Require(scenario.retained_bundle_id.value() == active_retained_bundle_id &&
                scenario.graph_revision.value() == active_graph_revision,
            "program_apply_deployment_not_active",
            "Program Apply target deployment is not the fresh active Manager "
            "selection");
    const auto bundle =
        bundle_source_.get_verified(scenario.retained_bundle_id);
    Require(bundle != nullptr, "program_apply_bundle_invalid",
            "Program Apply retained bundle is absent or unverifiable");
    auto model_map = LoadModelMap(*bundle, model_map_source_);
    const auto baseline = baseline_binding_source_.get_verified(
        scenario.program_binding_id, *bundle, model_map);
    Require(baseline != nullptr, "program_apply_baseline_binding_invalid",
            "Program Apply occurrence has no verified baseline binding");

    const auto program = [&]() {
      try {
        return deployment::verify_program_binding_contract(
            canonical_program_bytes, *bundle);
      } catch (const deployment::ProgramContractError &error) {
        Fail("program_apply_program_invalid",
             "Program Apply candidate is invalid: " +
                 std::string(error.what()));
      }
    }();
    try {
      deployment::verify_scenario_program_admission(
          program->normalized_program_root(), bundle->value(),
          scenario.deployment_compatibility, scenario.assistant_mode,
          scenario.typed_parameter_overrides);
    } catch (const deployment::ScenarioCatalogError &error) {
      Fail("program_apply_compatibility_invalid",
           "Program Apply scenario admission is invalid: " +
               std::string(error.what()));
    }
    const auto binding = [&]() {
      try {
        return deployment::compile_program_binding(
            *program, AssignmentFrom(*baseline), *bundle, model_map,
            catalog_source_);
      } catch (const deployment::ProgramBindingCompilerError &error) {
        Fail("program_apply_binding_invalid",
             "Program Apply binding is invalid: " + std::string(error.what()));
      }
    }();
    Require(program != nullptr && binding != nullptr &&
                binding->value().program_id == program->value().program_id &&
                binding->value().program_revision ==
                    program->value().program_revision &&
                binding->value().retained_bundle_id ==
                    scenario.retained_bundle_id &&
                binding->value().graph_revision == scenario.graph_revision &&
                binding->value().provenance.role_assignment_digest ==
                    baseline->value().provenance.role_assignment_digest,
            "program_apply_compilation_invalid",
            "Program Apply compiler returned a divergent Program Binding");

    // Import the complete immutable deployment closure before either Program
    // artifact. The source side is get-only and pinned to install_root; every
    // destination write is no-replace and quota checked. A failure here can
    // leave only unreachable immutable values and cannot advance Applied.
    for (const auto &record : bundle->value().catalogs.records) {
      std::visit(
          [&](const auto &reference) {
            const auto bytes = catalog_source_.get_verified(
                reference.content_addressed_store_key,
                reference.content_digest);
            Require(bytes.has_value(), "program_apply_bundle_invalid",
                    "Program Apply catalog closure is unavailable");
            static_cast<void>(catalog_store_.put_if_absent(
                reference.content_addressed_store_key,
                reference.content_digest, *bytes));
          },
          record);
    }
    const auto copy_raw = [&](const auto &reference) {
      const auto bytes = raw_artifact_source_.get_verified(
          reference.content_addressed_store_key, reference.content_digest,
          reference.byte_size);
      Require(bytes.has_value(), "program_apply_bundle_invalid",
              "Program Apply raw-artifact closure is unavailable");
      const auto stored = raw_artifact_store_.put_if_absent(*bytes);
      Require(stored.content_addressed_store_key ==
                  reference.content_addressed_store_key &&
                  stored.content_digest == reference.content_digest &&
                  stored.byte_size == reference.byte_size,
              "program_apply_repository_invalid",
              "Program Apply raw artifact reopened under another identity");
    };
    for (const auto &schema : bundle->value().catalogs.schema_artifacts) {
      const auto bytes = raw_artifact_source_.get_verified(
          schema.content_addressed_store_key, schema.content_digest);
      Require(bytes.has_value(), "program_apply_bundle_invalid",
              "Program Apply schema-artifact closure is unavailable");
      const auto stored = raw_artifact_store_.put_if_absent(*bytes);
      Require(stored.content_addressed_store_key ==
                  schema.content_addressed_store_key &&
                  stored.content_digest == schema.content_digest,
              "program_apply_repository_invalid",
              "Program Apply schema artifact reopened under another identity");
    }
    ForEachProduct(bundle->value().compiled_products,
                   [&](const core::RetainedProductDTO &product) {
                     for (const auto &artifact : product.artifact_refs) {
                       copy_raw(artifact);
                     }
                   });
    static_cast<void>(bundle_repository_.put_if_absent(bundle));
    const auto artifact_bundle =
        artifact_bundle_source_.get_verified(bundle->retained_bundle_id());
    Require(artifact_bundle != nullptr,
            "program_apply_repository_invalid",
            "Program Apply retained closure did not reopen exactly");
    auto artifact_model_map =
        LoadModelMap(*artifact_bundle, artifact_model_map_source_);
    static_cast<void>(binding_repository_.put_if_absent(
        baseline, *artifact_bundle, artifact_model_map));
    static_cast<void>(
        program_repository_.put_if_absent(program, *artifact_bundle));
    static_cast<void>(
        binding_repository_.put_if_absent(binding, *artifact_bundle,
                                          artifact_model_map));
    const auto reopened_program = program_repository_.get_verified(
        program->value().program_id, program->value().program_revision,
        *artifact_bundle);
    const auto reopened_binding = binding_repository_.get_verified(
        binding->program_binding_id(), *artifact_bundle, artifact_model_map);
    Require(reopened_program != nullptr && reopened_binding != nullptr &&
                reopened_program->canonical_program_bytes() ==
                    program->canonical_program_bytes() &&
                reopened_binding->canonical_bytes() ==
                    binding->canonical_bytes(),
            "program_apply_repository_invalid",
            "Program Apply immutable values did not reopen exactly");

    Preparation prepared{
        scenario.scenario_id,
        scenario.scenario_catalog_revision,
        scenario.deployment_artifact_id,
        scenario.deployment_revision,
        scenario.retained_bundle_id,
        scenario.graph_revision,
        scenario.node_environment_digest,
        scenario.program_binding_id,
        program->value().program_id,
        program->value().program_revision,
        binding->program_binding_id(),
        AssistantModeText(scenario.assistant_mode),
        scenario.typed_parameter_overrides,
        core::ContentDigest("sha256:" + std::string(64U, '0')),
    };
    prepared.preparation_digest = PreparationDigest(prepared);
    return PreparationValue(prepared, kProgramApplyPreparationSchemaVersion);
  } catch (const ProgramApplyError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("program_apply_preparation_failed",
         "Program Apply preparation failed: " + std::string(error.what()));
  }
}

CanonicalValue ProgramSelectionOwner::Commit(
    const std::string_view canonical_preparation_bytes,
    const std::optional<std::string> &expected_applied_record_digest,
    const std::string_view request_owner_epoch,
    const std::string_view request_id,
    const std::string_view active_retained_bundle_id,
    const std::string_view active_graph_revision,
    const std::int64_t observed_at_ns) {
  std::lock_guard lock(mutex_);
  try {
    const auto prepared = DecodePreparation(canonical_preparation_bytes);
    Require(content_identity::is_sha256_digest(active_retained_bundle_id) &&
                content_identity::is_sha256_digest(active_graph_revision) &&
                IsBoundedIdentityText(request_owner_epoch) &&
                IsBoundedIdentityText(request_id),
            "program_apply_active_selection_invalid",
            "Program Apply active Manager identity is invalid");
    if (expected_applied_record_digest.has_value()) {
      Require(
          content_identity::is_sha256_digest(*expected_applied_record_digest),
          "program_apply_expected_selection_invalid",
          "expected Applied record digest is invalid");
    }

    const CanonicalValue candidate =
        PreparationValue(prepared, kAppliedProgramSelectionSchemaVersion);
    const auto candidate_digest = AppliedRecordDigest(candidate);
    const auto intent_digest =
        RequestIntentDigest(candidate, expected_applied_record_digest);
    if (auto replay = ReplayResultLocked(intent_digest, request_owner_epoch,
                                         request_id);
        replay.has_value()) {
      return Member(*replay, "applied", "program_applied_selection_invalid");
    }
    const auto current_snapshot = owner_state_.Read();
    std::optional<DecodedOwnerState> current;
    if (current_snapshot.has_value()) {
      Require(current_snapshot->source_owner_kind == "program_selection" &&
                  current_snapshot->source_owner_id == source_owner_id_,
              "program_applied_selection_invalid",
              "Applied Program owner state belongs to another owner");
      current = DecodeOwnerState(current_snapshot->payload);
      RequireLedgerOwner(*current, current_snapshot->source_owner_id,
                         current_snapshot->source_owner_epoch);
    }

    Require(prepared.retained_bundle_id.value() == active_retained_bundle_id &&
                prepared.graph_revision.value() == active_graph_revision,
            "program_apply_deployment_not_active",
            "Program Apply target deployment is not the fresh active Manager "
            "selection");
    Require(current.has_value() == expected_applied_record_digest.has_value() &&
                (!current.has_value() ||
                 current->record_digest == *expected_applied_record_digest),
            "program_apply_selection_stale",
            "Program Apply expected another current Applied selection");
    Require(!current.has_value() ||
                current->request_owner_epoch != request_owner_epoch ||
                current->requests.size() <
                    kMaximumProgramApplyRequestLedgerEntries,
            "program_apply_request_ledger_exhausted",
            "Program Apply request ledger is exhausted for this authoring "
            "owner epoch");

    const auto bundle =
        artifact_bundle_source_.get_verified(prepared.retained_bundle_id);
    Require(bundle != nullptr && bundle->value().cell_graph.graph_revision ==
                                     prepared.graph_revision,
            "program_apply_bundle_invalid",
            "Program Apply target bundle is absent or divergent");
    auto model_map = LoadModelMap(*bundle, artifact_model_map_source_);
    const auto baseline = binding_repository_.get_verified(
        prepared.baseline_program_binding_id, *bundle, model_map);
    const auto program = program_repository_.get_verified(
        prepared.program_id, prepared.program_revision, *bundle);
    const auto binding = binding_repository_.get_verified(
        prepared.program_binding_id, *bundle, model_map);
    Require(baseline != nullptr && program != nullptr && binding != nullptr &&
                binding->value().program_id == prepared.program_id &&
                binding->value().program_revision ==
                    prepared.program_revision &&
                binding->value().provenance.role_assignment_digest ==
                    baseline->value().provenance.role_assignment_digest,
            "program_apply_preparation_invalid",
            "Program Apply prepared immutable closure is divergent");

    const auto generation =
        current.has_value() && current->record_digest == candidate_digest
            ? current->generation
            : (current.has_value() ? current->generation + 1U : 1U);
    Require(generation <= 9007199254740991ULL,
            "program_apply_selection_exhausted",
            "Program selection generation is exhausted");
    std::vector<RequestLedgerEntry> requests;
    if (current.has_value() &&
        current->request_owner_epoch == request_owner_epoch) {
      requests = current->requests;
    }
    requests.push_back(RequestLedgerEntry{
        std::string(request_id),
        intent_digest,
        generation,
        candidate,
        candidate_digest,
        source_owner_id_,
        source_owner_epoch_,
    });
    const auto owner_value = OwnerStateValue(
        candidate, candidate_digest, generation, request_owner_epoch, requests);
    Require(content_identity::canonical_json(owner_value).size() <=
                kMaximumOwnerStatePayloadBytes,
            "program_apply_request_ledger_exhausted",
            "Program Apply request ledger has reached its durable byte bound");
    try {
      const auto published = owner_state_.Publish(
          "program_selection", source_owner_id_, source_owner_epoch_,
          observed_at_ns, owner_value);
      Require(published.payload == owner_value,
              "program_apply_publication_failed",
              "Program Apply owner transaction selected divergent content");
    } catch (const SourceSnapshotError &) {
      // An error after atomic rename (for example directory fsync failure) can
      // make the syscall result ambiguous. Resolve that ambiguity immediately:
      // exact candidate means success; any other readable value means the
      // prior selection remains authoritative.
      const auto after = owner_state_.Read();
      if (!after.has_value() || after->payload != owner_value) {
        throw;
      }
    }
    applied_cache_.reset();
    applied_cache_record_digest_.clear();
    return ReadAppliedLocked();
  } catch (const ProgramApplyError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("program_apply_commit_failed",
         "Program Apply commit failed: " + std::string(error.what()));
  }
}

std::optional<CanonicalValue> ProgramSelectionOwner::Replay(
    const std::string_view canonical_preparation_bytes,
    const std::optional<std::string> &expected_applied_record_digest,
    const std::string_view request_owner_epoch,
    const std::string_view request_id) {
  std::lock_guard lock(mutex_);
  try {
    Require(IsBoundedIdentityText(request_owner_epoch) &&
                IsBoundedIdentityText(request_id),
            "program_apply_active_selection_invalid",
            "Program Apply request identity is invalid");
    if (expected_applied_record_digest.has_value()) {
      Require(content_identity::is_sha256_digest(
                  *expected_applied_record_digest),
              "program_apply_expected_selection_invalid",
              "expected Applied record digest is invalid");
    }
    const auto prepared = DecodePreparation(canonical_preparation_bytes);
    const CanonicalValue candidate =
        PreparationValue(prepared, kAppliedProgramSelectionSchemaVersion);
    auto replay = ReplayResultLocked(
        RequestIntentDigest(candidate, expected_applied_record_digest),
        request_owner_epoch, request_id);
    if (!replay.has_value()) {
      return std::nullopt;
    }
    return Member(*replay, "applied", "program_applied_selection_invalid");
  } catch (const ProgramApplyError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("program_apply_replay_failed",
         "Program Apply replay failed: " + std::string(error.what()));
  }
}

std::optional<CanonicalValue> ProgramSelectionOwner::ReplayRequest(
    const std::string_view scenario_id,
    const std::string_view scenario_catalog_revision,
    const std::string_view program_id, const std::string_view program_revision,
    const std::optional<std::string> &expected_applied_record_digest,
    const std::string_view request_owner_epoch,
    const std::string_view request_id) {
  std::lock_guard lock(mutex_);
  try {
    Require(IsIdentifier(scenario_id) && IsIdentifier(program_id) &&
                content_identity::is_sha256_digest(
                    scenario_catalog_revision) &&
                content_identity::is_sha256_digest(program_revision) &&
                IsBoundedIdentityText(request_owner_epoch) &&
                IsBoundedIdentityText(request_id) &&
                (!expected_applied_record_digest.has_value() ||
                 content_identity::is_sha256_digest(
                     *expected_applied_record_digest)),
            "program_apply_request_invalid",
            "Program Apply replay identity is invalid");
    return ReplayResultLocked(
        RequestIntentDigest(scenario_id, scenario_catalog_revision, program_id,
                            program_revision,
                            expected_applied_record_digest),
        request_owner_epoch, request_id);
  } catch (const ProgramApplyError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("program_apply_replay_failed",
         "Program Apply replay failed: " + std::string(error.what()));
  }
}

std::optional<CanonicalValue> ProgramSelectionOwner::ReplayResultLocked(
    const std::string_view intent_digest,
    const std::string_view request_owner_epoch,
    const std::string_view request_id) {
  const auto current_snapshot = owner_state_.Read();
  if (!current_snapshot.has_value()) {
    return std::nullopt;
  }
  Require(current_snapshot->source_owner_kind == "program_selection" &&
              current_snapshot->source_owner_id == source_owner_id_,
          "program_applied_selection_invalid",
          "Applied Program owner state belongs to another owner");
  const auto current = DecodeOwnerState(current_snapshot->payload);
  RequireLedgerOwner(current, current_snapshot->source_owner_id,
                     current_snapshot->source_owner_epoch);
  if (current.request_owner_epoch != request_owner_epoch) {
    return std::nullopt;
  }
  const auto replay = std::find_if(
      current.requests.begin(), current.requests.end(),
      [&](const RequestLedgerEntry &entry) {
        return entry.request_id == request_id;
      });
  if (replay == current.requests.end()) {
    return std::nullopt;
  }
  Require(replay->intent_digest == intent_digest,
          "program_apply_request_conflict",
          "Program Apply request identity was reused for another intent");
  const auto preparation = DecodePreparationValue(
      replay->record, kAppliedProgramSelectionSchemaVersion,
      "program_applied_selection_invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"applied",
       BuildAppliedLocked(replay->record, replay->record_digest,
                          replay->generation,
                          replay->selection_source_owner_id,
                          replay->selection_source_owner_epoch)},
      {"preparation",
       PreparationValue(preparation, kProgramApplyPreparationSchemaVersion)},
  });
}

CanonicalValue ProgramSelectionOwner::ReadApplied() {
  std::lock_guard lock(mutex_);
  return ReadAppliedLocked();
}

CanonicalValue ProgramSelectionOwner::ReadAppliedLocked() {
  try {
    const auto current_snapshot = owner_state_.Read();
    Require(current_snapshot.has_value(), "program_applied_selection_not_found",
            "no Program has been authoritatively Applied");
    Require(current_snapshot->source_owner_kind == "program_selection" &&
                current_snapshot->source_owner_id == source_owner_id_,
            "program_applied_selection_invalid",
            "Applied Program owner state belongs to another owner");
    const auto current = DecodeOwnerState(current_snapshot->payload);
    RequireLedgerOwner(current, current_snapshot->source_owner_id,
                       current_snapshot->source_owner_epoch);
    if (applied_cache_.has_value() &&
        applied_cache_record_digest_ == current.record_digest) {
      return *applied_cache_;
    }
    auto result = BuildAppliedLocked(
        current.record, current.record_digest, current.generation,
        current_snapshot->source_owner_id, current_snapshot->source_owner_epoch);
    applied_cache_record_digest_ = current.record_digest;
    applied_cache_ = result;
    return result;
  } catch (const ProgramApplyError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("program_applied_selection_invalid",
         "Applied Program selection is invalid: " + std::string(error.what()));
  }
}

CanonicalValue ProgramSelectionOwner::BuildAppliedLocked(
    const CanonicalValue &record, const std::string_view record_digest,
    const std::uint64_t generation,
    const std::string_view selection_source_owner_id,
    const std::string_view selection_source_owner_epoch) {
  try {
    Require(content_identity::is_sha256_digest(record_digest) &&
                AppliedRecordDigest(record) == record_digest &&
                generation >= 1U && generation <= 9007199254740991ULL &&
                IsBoundedIdentityText(selection_source_owner_id) &&
                IsBoundedIdentityText(selection_source_owner_epoch),
            "program_applied_selection_invalid",
            "Applied Program replay identity is invalid");
    const auto prepared = DecodePreparationValue(
        record, kAppliedProgramSelectionSchemaVersion,
        "program_applied_selection_invalid");
    const auto bundle =
        artifact_bundle_source_.get_verified(prepared.retained_bundle_id);
    Require(bundle != nullptr, "program_applied_selection_invalid",
            "Applied Program retained bundle is unavailable");
    auto model_map = LoadModelMap(*bundle, artifact_model_map_source_);
    const auto baseline = binding_repository_.get_verified(
        prepared.baseline_program_binding_id, *bundle, model_map);
    const auto program = program_repository_.get_verified(
        prepared.program_id, prepared.program_revision, *bundle);
    const auto binding = binding_repository_.get_verified(
        prepared.program_binding_id, *bundle, model_map);
    Require(baseline != nullptr && program != nullptr && binding != nullptr &&
                binding->value().program_id == prepared.program_id &&
                binding->value().program_revision ==
                    prepared.program_revision &&
                binding->value().provenance.role_assignment_digest ==
                    baseline->value().provenance.role_assignment_digest,
            "program_applied_selection_invalid",
            "Applied Program immutable closure is unavailable or divergent");

    return CanonicalValue(CanonicalValue::Object{
        {"assistant_mode", CanonicalValue(prepared.assistant_mode)},
        {"baseline_program_binding_id",
         CanonicalValue(prepared.baseline_program_binding_id.value())},
        {"deployment_artifact_id",
         CanonicalValue(prepared.deployment_artifact_id.value())},
        {"deployment_revision",
         CanonicalValue(prepared.deployment_revision.value())},
        {"graph_revision", CanonicalValue(prepared.graph_revision.value())},
        {"node_environment_digest",
         CanonicalValue(prepared.node_environment_digest.value())},
        {"preparation_digest",
         CanonicalValue(prepared.preparation_digest.value())},
        {"program", program->normalized_program_root()},
        {"program_binding", binding->normalized_root()},
        {"program_binding_id",
         CanonicalValue(prepared.program_binding_id.value())},
        {"program_id", CanonicalValue(prepared.program_id.value())},
        {"program_revision", CanonicalValue(prepared.program_revision.value())},
        {"retained_bundle_id",
         CanonicalValue(prepared.retained_bundle_id.value())},
        {"scenario_catalog_revision",
         CanonicalValue(prepared.scenario_catalog_revision.value())},
        {"scenario_id", CanonicalValue(prepared.scenario_id)},
        {"schema_version",
         CanonicalValue(std::string(kAppliedProgramSelectionSchemaVersion))},
        {"source",
         CanonicalValue(CanonicalValue::Object{
             {"record_digest", CanonicalValue(std::string(record_digest))},
             {"semantic_content_digest",
              CanonicalValue(std::string(record_digest))},
             {"source_generation",
              CanonicalValue(static_cast<double>(generation))},
             {"source_owner_epoch",
              CanonicalValue(std::string(selection_source_owner_epoch))},
             {"source_owner_id",
              CanonicalValue(std::string(selection_source_owner_id))},
         })},
        {"typed_parameter_overrides", prepared.typed_parameter_overrides},
    });
  } catch (const ProgramApplyError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("program_applied_selection_invalid",
         "Applied Program selection is invalid: " + std::string(error.what()));
  }
}

} // namespace manyforge::headless
