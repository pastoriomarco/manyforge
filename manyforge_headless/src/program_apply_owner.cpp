#include "manyforge_headless/program_apply_owner.hpp"

#include <algorithm>
#include <set>
#include <utility>
#include <variant>

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

void RequireScenario(const Preparation &prepared,
                     const deployment::ScenarioProgramSelection &scenario) {
  Require(
      scenario.scenario_id == prepared.scenario_id &&
          scenario.scenario_catalog_revision ==
              prepared.scenario_catalog_revision &&
          scenario.deployment_artifact_id == prepared.deployment_artifact_id &&
          scenario.deployment_revision == prepared.deployment_revision &&
          scenario.retained_bundle_id == prepared.retained_bundle_id &&
          scenario.graph_revision == prepared.graph_revision &&
          scenario.node_environment_digest ==
              prepared.node_environment_digest &&
          scenario.program_binding_id == prepared.baseline_program_binding_id &&
          AssistantModeText(scenario.assistant_mode) ==
              prepared.assistant_mode &&
          scenario.typed_parameter_overrides ==
              prepared.typed_parameter_overrides,
      "program_apply_preparation_stale",
      "Program Apply preparation no longer names the exact scenario "
      "occurrence");
}

} // namespace

ProgramApplyError::ProgramApplyError(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

ProgramApplyOwner::ProgramApplyOwner(std::filesystem::path package_root,
                                     std::filesystem::path publication_root,
                                     std::string owner_id,
                                     std::string owner_epoch)
    : scenario_source_(package_root, "maintained"),
      bundle_source_(package_root), model_map_source_(package_root),
      catalog_source_(package_root), baseline_binding_source_(package_root),
      program_repository_(publication_root),
      binding_repository_(publication_root),
      publication_(std::move(publication_root),
                   std::string(kAppliedProgramSelectionSourceFile)),
      source_owner_id_(std::move(owner_id)),
      source_owner_epoch_(std::move(owner_epoch)) {
  Require(!source_owner_id_.empty() && !source_owner_epoch_.empty(),
          "program_apply_configuration_invalid",
          "Program Apply source-owner identities are required");
}

CanonicalValue ProgramApplyOwner::Prepare(
    const std::string_view scenario_id,
    const std::string_view expected_scenario_catalog_revision,
    const std::string_view canonical_program_bytes) {
  std::lock_guard lock(mutex_);
  try {
    Require(
        content_identity::is_sha256_digest(expected_scenario_catalog_revision),
        "program_apply_request_invalid",
        "expected scenario-catalog revision is invalid");
    const auto scenario =
        scenario_source_.LoadExactProgramSelection(scenario_id);
    Require(scenario.scenario_catalog_revision.value() ==
                expected_scenario_catalog_revision,
            "program_apply_scenario_catalog_stale",
            "Program Apply targets a stale scenario-catalog revision");
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

    static_cast<void>(program_repository_.put_if_absent(program, *bundle));
    static_cast<void>(
        binding_repository_.put_if_absent(binding, *bundle, model_map));
    const auto reopened_program = program_repository_.get_verified(
        program->value().program_id, program->value().program_revision,
        *bundle);
    const auto reopened_binding = binding_repository_.get_verified(
        binding->program_binding_id(), *bundle, model_map);
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

CanonicalValue ProgramApplyOwner::Commit(
    const std::string_view canonical_preparation_bytes,
    const std::optional<std::string> &expected_applied_record_digest,
    const std::string_view active_retained_bundle_id,
    const std::string_view active_graph_revision,
    const std::int64_t observed_at_ns) {
  std::lock_guard lock(mutex_);
  try {
    const auto prepared = DecodePreparation(canonical_preparation_bytes);
    Require(content_identity::is_sha256_digest(active_retained_bundle_id) &&
                content_identity::is_sha256_digest(active_graph_revision),
            "program_apply_active_selection_invalid",
            "Program Apply active Manager identity is invalid");
    Require(prepared.retained_bundle_id.value() == active_retained_bundle_id &&
                prepared.graph_revision.value() == active_graph_revision,
            "program_apply_deployment_not_active",
            "Program Apply target deployment is not the fresh active Manager "
            "selection");
    if (expected_applied_record_digest.has_value()) {
      Require(
          content_identity::is_sha256_digest(*expected_applied_record_digest),
          "program_apply_expected_selection_invalid",
          "expected Applied record digest is invalid");
    }

    const CanonicalValue candidate =
        PreparationValue(prepared, kAppliedProgramSelectionSchemaVersion);
    const auto current = publication_.Read();
    if (current.has_value() && current->payload == candidate) {
      return ReadAppliedLocked();
    }

    const auto scenario =
        scenario_source_.LoadExactProgramSelection(prepared.scenario_id);
    RequireScenario(prepared, scenario);
    const auto bundle =
        bundle_source_.get_verified(prepared.retained_bundle_id);
    Require(bundle != nullptr && bundle->value().cell_graph.graph_revision ==
                                     prepared.graph_revision,
            "program_apply_bundle_invalid",
            "Program Apply target bundle is absent or divergent");
    auto model_map = LoadModelMap(*bundle, model_map_source_);
    const auto baseline = baseline_binding_source_.get_verified(
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

    Require(current.has_value() == expected_applied_record_digest.has_value() &&
                (!current.has_value() ||
                 current->record_digest == *expected_applied_record_digest),
            "program_apply_selection_stale",
            "Program Apply expected another current Applied selection");

    try {
      const auto published =
          publication_.Publish("program_selection", source_owner_id_,
                               source_owner_epoch_, observed_at_ns, candidate);
      Require(published.payload == candidate,
              "program_apply_publication_failed",
              "Program Apply publication selected divergent content");
    } catch (const SourceSnapshotError &) {
      // An error after atomic rename (for example directory fsync failure) can
      // make the syscall result ambiguous. Resolve that ambiguity immediately:
      // exact candidate means success; any other readable value means the
      // prior selection remains authoritative.
      const auto after = publication_.Read();
      if (!after.has_value() || after->payload != candidate) {
        throw;
      }
    }
    return ReadAppliedLocked();
  } catch (const ProgramApplyError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("program_apply_commit_failed",
         "Program Apply commit failed: " + std::string(error.what()));
  }
}

CanonicalValue ProgramApplyOwner::ReadApplied() {
  std::lock_guard lock(mutex_);
  return ReadAppliedLocked();
}

CanonicalValue ProgramApplyOwner::ReadAppliedLocked() {
  try {
    const auto current = publication_.Read();
    Require(current.has_value(), "program_applied_selection_not_found",
            "no Program has been authoritatively Applied");
    if (applied_cache_.has_value() &&
        applied_cache_record_digest_ == current->record_digest) {
      return *applied_cache_;
    }
    Require(current->source_owner_kind == "program_selection" &&
                current->source_owner_id == source_owner_id_,
            "program_applied_selection_invalid",
            "Applied Program publication belongs to another owner");
    const auto prepared = DecodePreparationValue(
        current->payload, kAppliedProgramSelectionSchemaVersion,
        "program_applied_selection_invalid");
    const auto bundle =
        bundle_source_.get_verified(prepared.retained_bundle_id);
    Require(bundle != nullptr, "program_applied_selection_invalid",
            "Applied Program retained bundle is unavailable");
    auto model_map = LoadModelMap(*bundle, model_map_source_);
    const auto baseline = baseline_binding_source_.get_verified(
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

    CanonicalValue result(CanonicalValue::Object{
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
             {"record_digest", CanonicalValue(current->record_digest)},
             {"semantic_content_digest",
              CanonicalValue(current->semantic_content_digest)},
             {"source_generation",
              CanonicalValue(static_cast<double>(current->source_generation))},
             {"source_owner_epoch",
              CanonicalValue(current->source_owner_epoch)},
             {"source_owner_id", CanonicalValue(current->source_owner_id)},
         })},
        {"typed_parameter_overrides", prepared.typed_parameter_overrides},
    });
    applied_cache_record_digest_ = current->record_digest;
    applied_cache_ = result;
    return result;
  } catch (const ProgramApplyError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("program_applied_selection_invalid",
         "Applied Program selection is invalid: " + std::string(error.what()));
  }
}

} // namespace manyforge::headless
