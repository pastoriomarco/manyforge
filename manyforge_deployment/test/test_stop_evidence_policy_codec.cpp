#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kStopEvidencePolicySchemaRevision =
    "sha256:e516114453cac5912b204bf17267b66f08a2afa47a72a8dd0c0f0f6b808478f2";
constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

static_assert(!std::is_aggregate_v<QualifiedStopEvidencePolicy>);
static_assert(!std::is_default_constructible_v<QualifiedStopEvidencePolicy>);

struct PhaseSpec final {
  std::string id;
  std::int64_t deadline_offset_ns;
};

struct EvidenceSpec final {
  std::string id;
  std::string phase_id;
  std::string evidence_kind;
  std::string source_kind;
  std::string observation_kind;
  std::int64_t freshness_budget_ns = 25;
  std::int64_t minimum_dwell_ns = 0;
  std::vector<std::string> authority_classes{"measured"};
};

struct PolicySpec final {
  std::string policy_id{"application_stop"};
  std::string granularity{"control_domain"};
  std::string command_kind{"controller_goal_cancel"};
  std::string command_phase_id{"command"};
  std::int64_t outer_duration_ns = 1'000;
  std::int64_t kernel_completion_reserve_ns = 100;
  std::vector<PhaseSpec> phases{{"command", 100}, {"terminal", 500}};
  std::vector<EvidenceSpec> evidence{{
      "goal_terminal",
      "terminal",
      "controller_goal_terminal",
      "controller_goal_status",
      "post_command_sample",
  }};
};

[[nodiscard]] CanonicalValue provider_contract_ref() {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_role_id", CanonicalValue("stop_contract")},
      {"contract_schema_id",
       CanonicalValue("manyforge.stop_provider_contract.v1")},
      {"contract_schema_revision", CanonicalValue(std::string(kZeroDigest))},
  });
}

[[nodiscard]] CanonicalValue command_value(std::string_view kind) {
  CanonicalValue::Object object{
      {"command_kind", CanonicalValue(std::string(kind))},
  };
  if (kind == "motion_controlled_stop" || kind == "safe_output_command" ||
      kind == "logical_cancel" || kind == "registered") {
    object.emplace_back("provider_contract_ref", provider_contract_ref());
  }
  if (kind == "registered") {
    object.emplace_back("registered_command_kind_id",
                        CanonicalValue("vendor_stop_command"));
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue evidence_value(std::string_view kind) {
  CanonicalValue::Object object{
      {"evidence_kind", CanonicalValue(std::string(kind))},
  };
  if (kind == "motion_rest" || kind == "io_safe_output" ||
      kind == "logical_cancellation_no_continuing_effect" ||
      kind == "registered") {
    object.emplace_back("provider_contract_ref", provider_contract_ref());
  }
  if (kind == "registered") {
    object.emplace_back("registered_evidence_kind_id",
                        CanonicalValue("vendor_terminal_evidence"));
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue source_value(const EvidenceSpec& spec) {
  CanonicalValue::Object object{
      {"source_kind", CanonicalValue(spec.source_kind)},
      {"freshness_budget_ns",
       CanonicalValue(static_cast<double>(spec.freshness_budget_ns))},
  };
  if (spec.source_kind == "binding_state_authority") {
    object.emplace_back("state_endpoint_role_id",
                        CanonicalValue("measured_state"));
    CanonicalValue::Array authority_classes;
    authority_classes.reserve(spec.authority_classes.size());
    for (const auto& value : spec.authority_classes) {
      authority_classes.emplace_back(value);
    }
    object.emplace_back("allowed_authority_classes",
                        CanonicalValue(std::move(authority_classes)));
  } else if (spec.source_kind == "provider_evidence") {
    object.emplace_back("evidence_endpoint_role_id",
                        CanonicalValue("stop_evidence"));
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue observation_value(const EvidenceSpec& spec) {
  CanonicalValue::Object object{
      {"observation_kind", CanonicalValue(spec.observation_kind)},
  };
  if (spec.observation_kind == "post_command_dwell") {
    object.emplace_back(
        "minimum_dwell_ns",
        CanonicalValue(static_cast<double>(spec.minimum_dwell_ns)));
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue policy_identity_value(const PolicySpec& spec) {
  CanonicalValue::Array phases;
  phases.reserve(spec.phases.size());
  for (const auto& phase : spec.phases) {
    phases.emplace_back(CanonicalValue::Object{
        {"phase_id", CanonicalValue(phase.id)},
        {"deadline_offset_ns",
         CanonicalValue(static_cast<double>(phase.deadline_offset_ns))},
    });
  }

  CanonicalValue::Array evidence;
  evidence.reserve(spec.evidence.size());
  for (const auto& requirement : spec.evidence) {
    evidence.emplace_back(CanonicalValue::Object{
        {"evidence_item_id", CanonicalValue(requirement.id)},
        {"phase_id", CanonicalValue(requirement.phase_id)},
        {"evidence", evidence_value(requirement.evidence_kind)},
        {"source_requirement", source_value(requirement)},
        {"observation_semantics", observation_value(requirement)},
    });
  }

  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kStopEvidencePolicyApiVersion))},
      {"stop_evidence_policy_id", CanonicalValue(spec.policy_id)},
      {"disposition", CanonicalValue("stoppable")},
      {"containment_granularity", CanonicalValue(spec.granularity)},
      {"containment_command",
       CanonicalValue(CanonicalValue::Object{
           {"command_phase_id", CanonicalValue(spec.command_phase_id)},
           {"command", command_value(spec.command_kind)},
       })},
      {"deadline_budget",
       CanonicalValue(CanonicalValue::Object{
           {"outer_duration_ns",
            CanonicalValue(static_cast<double>(spec.outer_duration_ns))},
           {"kernel_completion_reserve_ns",
            CanonicalValue(static_cast<double>(
                spec.kernel_completion_reserve_ns))},
           {"phases", CanonicalValue(std::move(phases))},
       })},
      {"evidence_conjunction", CanonicalValue("all_of")},
      {"evidence_requirements", CanonicalValue(std::move(evidence))},
  });
}

[[nodiscard]] std::string add_semantic_revision(
    CanonicalValue identity_value) {
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/stop_evidence_policy");
  preimage.push_back('\0');
  preimage.append(core::kStopEvidencePolicyApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(identity_value));
  auto root = identity_value.as_object();
  root.emplace_back("stop_evidence_policy_revision",
                    CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(root)));
}

[[nodiscard]] std::string policy_document(const PolicySpec& spec) {
  return add_semantic_revision(policy_identity_value(spec));
}

[[nodiscard]] std::string not_stoppable_document(
    std::string_view policy_id = "non_interruptible") {
  return add_semantic_revision(CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kStopEvidencePolicyApiVersion))},
      {"stop_evidence_policy_id", CanonicalValue(std::string(policy_id))},
      {"disposition", CanonicalValue("not_stoppable")},
      {"containment_granularity", CanonicalValue("resource")},
  }));
}

[[nodiscard]] std::string revision_from_document(std::string_view document) {
  const auto parsed = parse_strict_json_model_yaml(document);
  const auto* revision =
      parsed.root().find_object_member("stop_evidence_policy_revision");
  EXPECT_NE(revision, nullptr);
  if (revision == nullptr) {
    return {};
  }
  const auto* text = std::get_if<std::string>(&revision->storage());
  EXPECT_NE(text, nullptr);
  return text == nullptr ? std::string{} : *text;
}

[[nodiscard]] core::StopEvidencePolicyCatalogRecordRefDTO reference_for(
    std::string_view policy_id, std::string_view document,
    const core::ContentAddressedStoreKey& store_key) {
  return {
      core::StopEvidencePolicyId(std::string(policy_id)),
      core::StopEvidencePolicyRevision(revision_from_document(document)),
      core::SchemaId(std::string(core::kStopEvidencePolicyApiVersion)),
      core::SchemaRevision(std::string(kStopEvidencePolicySchemaRevision)),
      core::ContentDigest(sha256_digest(document)),
      store_key,
  };
}

[[nodiscard]] QualifiedStopEvidencePolicy qualify(
    const PolicySpec& spec,
    const core::ContentAddressedStoreKey& store_key =
        core::ContentAddressedStoreKey("sha256/stop-policy")) {
  const auto document = policy_document(spec);
  return resolve_and_qualify_stop_evidence_policy(
      reference_for(spec.policy_id, document, store_key), store_key, document);
}

void expect_semantic_rejection(const PolicySpec& spec) {
  const auto document = policy_document(spec);
  const core::ContentAddressedStoreKey store_key{
      "sha256/stop-policy-semantic-negative"};
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   reference_for(spec.policy_id, document, store_key),
                   store_key, document),
               StopEvidencePolicyCodecError);
}

[[nodiscard]] std::string replace_once(std::string source,
                                       std::string_view needle,
                                       std::string_view replacement) {
  const auto offset = source.find(needle);
  EXPECT_NE(offset, std::string::npos) << needle;
  if (offset != std::string::npos) {
    source.replace(offset, needle.size(), replacement);
  }
  return source;
}

void expect_closed_schema_rejection(std::string_view policy_id,
                                    std::string_view valid_document,
                                    std::string mutated_document) {
  const core::ContentAddressedStoreKey store_key{
      "sha256/stop-policy-shape-negative"};
  auto reference = reference_for(policy_id, valid_document, store_key);
  reference.content_digest =
      core::ContentDigest(sha256_digest(mutated_document));
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   reference, store_key, mutated_document),
               ClosedSchemaError);
}

TEST(StopEvidencePolicyCodec,
     QualifiesArbitraryStoppablePolicyAndPreservesDeclarativeFacts) {
  PolicySpec spec;
  spec.policy_id = "application_control_domain_stop";
  spec.command_kind = "motion_controlled_stop";
  spec.phases = {{"command", 100}, {"ack", 300}, {"rest", 700}};
  spec.evidence = {
      {"command_ack", "command", "controller_goal_terminal",
       "controller_goal_status", "command_result"},
      {"discrete_ack", "ack", "discrete_acknowledgement",
       "command_target_status", "post_command_sample"},
      {"measured_rest", "rest", "motion_rest", "binding_state_authority",
       "post_command_dwell", 50, 500, {"measured", "predicted"}},
  };

  const auto document = policy_document(spec);
  const core::ContentAddressedStoreKey store_key{"sha256/authored-stop"};
  const auto reference =
      reference_for(spec.policy_id, document, store_key);
  const auto qualified = resolve_and_qualify_stop_evidence_policy(
      reference, store_key, document);

  EXPECT_EQ(qualified.reference(), reference);
  EXPECT_EQ(qualified.value().stop_evidence_policy_id,
            core::StopEvidencePolicyId(spec.policy_id));
  EXPECT_EQ(qualified.value().disposition,
            core::StopEvidenceDisposition::kStoppable);
  ASSERT_TRUE(qualified.value().containment_command.has_value());
  EXPECT_EQ(qualified.value().containment_command->command_kind,
            core::StopContainmentCommandKind::kMotionControlledStop);
  ASSERT_TRUE(
      qualified.value().containment_command->provider_contract_ref.has_value());
  ASSERT_TRUE(qualified.value().deadline_budget.has_value());
  EXPECT_EQ(qualified.value().deadline_budget->phases.size(), 3U);
  EXPECT_EQ(qualified.value().evidence_requirements.size(), 3U);
  EXPECT_EQ(qualified.value().evidence_requirements.back().evidence_kind,
            core::StopEvidenceRequirementKind::kMotionRest);
  EXPECT_EQ(qualified.value()
                .evidence_requirements.back()
                .source_requirement.allowed_authority_classes,
            (std::vector<core::StateAuthorityClass>{
                core::StateAuthorityClass::kMeasured,
                core::StateAuthorityClass::kPredicted}));
  EXPECT_EQ(canonical_json(qualified.normalized_root()), document);
}

TEST(StopEvidencePolicyCodec,
     QualifiesNotStoppableWithoutInventingContainmentOrEvidence) {
  const auto document = not_stoppable_document();
  const core::ContentAddressedStoreKey store_key{"sha256/not-stoppable"};
  const auto qualified = resolve_and_qualify_stop_evidence_policy(
      reference_for("non_interruptible", document, store_key), store_key,
      document);
  EXPECT_EQ(qualified.value().disposition,
            core::StopEvidenceDisposition::kNotStoppable);
  EXPECT_FALSE(qualified.value().containment_command.has_value());
  EXPECT_FALSE(qualified.value().deadline_budget.has_value());
  EXPECT_FALSE(qualified.value().evidence_conjunction.has_value());
  EXPECT_TRUE(qualified.value().evidence_requirements.empty());
}

TEST(StopEvidencePolicyCodec,
     AcceptsEveryClosedAndRegisteredContainmentCommandAlternative) {
  for (const std::string command_kind :
       {"controller_goal_cancel", "motion_controlled_stop",
        "capability_command_cancel", "safe_output_command", "logical_cancel",
        "registered"}) {
    PolicySpec spec;
    spec.policy_id = "command_" + command_kind;
    spec.command_kind = command_kind;
    EXPECT_NO_THROW(qualify(spec)) << command_kind;
  }
}

TEST(StopEvidencePolicyCodec,
     AcceptsExactEvidenceSourceObservationCompatibilityMatrix) {
  const std::vector<EvidenceSpec> requirements{
      {"e", "command", "controller_goal_terminal", "controller_goal_status",
       "command_result"},
      {"e", "terminal", "controller_goal_terminal", "controller_goal_status",
       "post_command_sample"},
      {"e", "command", "discrete_acknowledgement",
       "command_target_status", "command_result"},
      {"e", "terminal", "discrete_acknowledgement",
       "command_target_status", "post_command_sample"},
      {"e", "terminal", "motion_rest", "binding_state_authority",
       "post_command_dwell", 25, 200},
      {"e", "terminal", "io_safe_output", "binding_state_authority",
       "post_command_sample"},
      {"e", "terminal", "io_safe_output", "provider_evidence",
       "post_command_dwell", 25, 200},
      {"e", "command", "logical_cancellation_no_continuing_effect",
       "command_target_status", "command_result"},
      {"e", "terminal", "logical_cancellation_no_continuing_effect",
       "provider_evidence", "post_command_sample"},
      {"e", "terminal", "logical_cancellation_no_continuing_effect",
       "provider_evidence", "post_command_dwell", 25, 200},
  };
  for (std::size_t index = 0U; index < requirements.size(); ++index) {
    PolicySpec spec;
    spec.policy_id = "matrix_" + std::to_string(index);
    spec.evidence = {requirements[index]};
    if (requirements[index].phase_id == "command") {
      spec.phases = {{"command", 100}};
    }
    EXPECT_NO_THROW(qualify(spec)) << index;
  }
}

TEST(StopEvidencePolicyCodec,
     ExhaustivelyEnforcesEveryEvidenceSourceObservationCombination) {
  const std::vector<std::string> evidence_kinds{
      "controller_goal_terminal",
      "motion_rest",
      "discrete_acknowledgement",
      "io_safe_output",
      "logical_cancellation_no_continuing_effect",
      "registered",
  };
  const std::vector<std::string> source_kinds{
      "controller_goal_status",
      "binding_state_authority",
      "command_target_status",
      "provider_evidence",
  };
  const std::vector<std::string> observation_kinds{
      "command_result",
      "post_command_sample",
      "post_command_dwell",
  };
  const auto is_accepted = [](std::string_view evidence,
                              std::string_view source,
                              std::string_view observation) {
    if (evidence == "controller_goal_terminal") {
      return source == "controller_goal_status" &&
             (observation == "command_result" ||
              observation == "post_command_sample");
    }
    if (evidence == "motion_rest") {
      return source == "binding_state_authority" &&
             observation == "post_command_dwell";
    }
    if (evidence == "discrete_acknowledgement") {
      return source == "command_target_status" &&
             (observation == "command_result" ||
              observation == "post_command_sample");
    }
    if (evidence == "io_safe_output") {
      return (source == "binding_state_authority" ||
              source == "provider_evidence") &&
             (observation == "post_command_sample" ||
              observation == "post_command_dwell");
    }
    if (evidence == "logical_cancellation_no_continuing_effect") {
      return (source == "command_target_status" ||
              source == "provider_evidence");
    }
    return evidence == "registered" && source == "provider_evidence";
  };

  std::size_t case_index = 0U;
  for (const auto& evidence : evidence_kinds) {
    for (const auto& source : source_kinds) {
      for (const auto& observation : observation_kinds) {
        PolicySpec spec;
        spec.policy_id = "matrix_exhaustive_" + std::to_string(case_index++);
        const bool command_result = observation == "command_result";
        spec.evidence = {{
            "terminal",
            command_result ? "command" : "terminal",
            evidence,
            source,
            observation,
            25,
            observation == "post_command_dwell" ? 200 : 0,
        }};
        if (command_result) {
          spec.phases = {{"command", 100}};
        }
        const auto document = policy_document(spec);
        const core::ContentAddressedStoreKey store_key{
            "sha256/exhaustive-stop-matrix"};
        const auto reference =
            reference_for(spec.policy_id, document, store_key);
        if (is_accepted(evidence, source, observation)) {
          EXPECT_NO_THROW(resolve_and_qualify_stop_evidence_policy(
              reference, store_key, document))
              << evidence << " / " << source << " / " << observation;
        } else {
          EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                           reference, store_key, document),
                       StopEvidencePolicyCodecError)
              << evidence << " / " << source << " / " << observation;
        }
      }
    }
  }
  EXPECT_EQ(case_index, 72U);
}

TEST(StopEvidencePolicyCodec,
     DefersCommandEvidenceSufficiencyToProviderAndCompilerQualification) {
  PolicySpec safe_output_with_controller_evidence;
  safe_output_with_controller_evidence.policy_id =
      "safe_output_with_controller_evidence";
  safe_output_with_controller_evidence.command_kind = "safe_output_command";
  EXPECT_NO_THROW(qualify(safe_output_with_controller_evidence));

  PolicySpec cancel_with_io_evidence;
  cancel_with_io_evidence.policy_id = "cancel_with_io_evidence";
  cancel_with_io_evidence.evidence = {
      {"safe", "terminal", "io_safe_output", "provider_evidence",
       "post_command_sample"},
  };
  EXPECT_NO_THROW(qualify(cancel_with_io_evidence));
}

TEST(StopEvidencePolicyCodec,
     RegisteredEvidenceIsProviderTypedAndAcceptsExactlyThreeObservations) {
  for (const std::string observation :
       {"command_result", "post_command_sample", "post_command_dwell"}) {
    PolicySpec spec;
    spec.policy_id = "registered_" + observation;
    spec.evidence = {{"registered_terminal",
                      observation == "command_result" ? "command" : "terminal",
                      "registered",
                      "provider_evidence",
                      observation,
                      25,
                      observation == "post_command_dwell" ? 200 : 0}};
    if (observation == "command_result") {
      spec.phases = {{"command", 100}};
    }
    const auto qualified = qualify(spec);
    ASSERT_TRUE(
        qualified.value().evidence_requirements.front().provider_contract_ref
            .has_value());
    ASSERT_TRUE(qualified.value()
                    .evidence_requirements.front()
                    .registered_evidence_kind_id.has_value());
  }

  const auto document = policy_document(PolicySpec{});
  const auto token_offset = document.find("post_command_sample");
  ASSERT_NE(token_offset, std::string::npos);
  auto unknown = document;
  unknown.replace(token_offset, std::string("post_command_sample").size(),
                  "provider_defined_observation");
  const core::ContentAddressedStoreKey store_key{"sha256/unknown-observation"};
  auto reference = reference_for("application_stop", document, store_key);
  reference.content_digest = core::ContentDigest(sha256_digest(unknown));
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(reference, store_key,
                                                        unknown),
               ClosedSchemaError);
}

TEST(StopEvidencePolicyCodec,
     RejectsEveryEvidenceCompatibilityAndObservationPhaseDefect) {
  const std::vector<EvidenceSpec> invalid{
      {"e", "terminal", "controller_goal_terminal", "provider_evidence",
       "post_command_sample"},
      {"e", "terminal", "controller_goal_terminal", "controller_goal_status",
       "post_command_dwell", 25, 200},
      {"e", "terminal", "motion_rest", "binding_state_authority",
       "post_command_sample"},
      {"e", "terminal", "motion_rest", "provider_evidence",
       "post_command_dwell", 25, 200},
      {"e", "terminal", "discrete_acknowledgement", "controller_goal_status",
       "post_command_sample"},
      {"e", "terminal", "io_safe_output", "command_target_status",
       "post_command_sample"},
      {"e", "command", "io_safe_output", "provider_evidence",
       "command_result"},
      {"e", "terminal", "logical_cancellation_no_continuing_effect",
       "binding_state_authority", "post_command_sample"},
      {"e", "terminal", "registered", "command_target_status",
       "post_command_sample"},
      {"e", "command", "controller_goal_terminal", "controller_goal_status",
       "post_command_sample"},
      {"e", "terminal", "controller_goal_terminal", "controller_goal_status",
       "command_result"},
  };
  for (std::size_t index = 0U; index < invalid.size(); ++index) {
    PolicySpec spec;
    spec.policy_id = "invalid_matrix_" + std::to_string(index);
    spec.evidence = {invalid[index]};
    expect_semantic_rejection(spec);
  }
}

TEST(StopEvidencePolicyCodec,
     RejectsBudgetOrderingCoverageReserveAndDwellDefects) {
  PolicySpec duplicate_phase;
  duplicate_phase.phases = {{"command", 100}, {"command", 500}};
  expect_semantic_rejection(duplicate_phase);

  PolicySpec non_increasing;
  non_increasing.phases = {{"command", 500}, {"terminal", 500}};
  expect_semantic_rejection(non_increasing);

  PolicySpec wrong_command_phase;
  wrong_command_phase.command_phase_id = "terminal";
  expect_semantic_rejection(wrong_command_phase);

  PolicySpec unknown_evidence_phase;
  unknown_evidence_phase.evidence.front().phase_id = "unknown";
  expect_semantic_rejection(unknown_evidence_phase);

  PolicySpec unused_phase;
  unused_phase.phases = {{"command", 100}, {"middle", 300}, {"terminal", 500}};
  expect_semantic_rejection(unused_phase);

  PolicySpec insufficient_reserve;
  insufficient_reserve.outer_duration_ns = 550;
  insufficient_reserve.kernel_completion_reserve_ns = 100;
  expect_semantic_rejection(insufficient_reserve);

  PolicySpec excessive_dwell;
  excessive_dwell.evidence = {
      {"rest", "terminal", "motion_rest", "binding_state_authority",
       "post_command_dwell", 25, 401},
  };
  expect_semantic_rejection(excessive_dwell);

  PolicySpec duplicate_evidence;
  duplicate_evidence.evidence.push_back(duplicate_evidence.evidence.front());
  expect_semantic_rejection(duplicate_evidence);
}

TEST(StopEvidencePolicyCodec, AcceptsAndRejectsExactCollectionBoundaries) {
  PolicySpec exact;
  exact.policy_id = "exact_boundary";
  exact.outer_duration_ns = 100'000;
  exact.kernel_completion_reserve_ns = 1'000;
  exact.phases.clear();
  exact.evidence.clear();
  for (std::size_t index = 0U; index < core::kMaxStopEvidencePolicyItems;
       ++index) {
    const auto phase_id = "phase_" + std::to_string(index);
    exact.phases.push_back(
        {phase_id, static_cast<std::int64_t>((index + 1U) * 1'000U)});
    exact.evidence.push_back(
        {"evidence_" + std::to_string(index), phase_id,
         index == 0U ? "controller_goal_terminal"
                     : "discrete_acknowledgement",
         index == 0U ? "controller_goal_status" : "command_target_status",
         index == 0U ? "command_result" : "post_command_sample"});
  }
  exact.command_phase_id = exact.phases.front().id;
  EXPECT_EQ(qualify(exact).value().evidence_requirements.size(),
            core::kMaxStopEvidencePolicyItems);

  auto too_many_phases = exact;
  too_many_phases.policy_id = "too_many_phases";
  too_many_phases.phases.push_back({"overflow", 65'000});
  const auto phase_document = policy_document(too_many_phases);
  const core::ContentAddressedStoreKey store_key{"sha256/stop-policy-bound"};
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   reference_for(too_many_phases.policy_id, phase_document,
                                 store_key),
                   store_key, phase_document),
               ClosedSchemaError);

  auto too_many_evidence = exact;
  too_many_evidence.policy_id = "too_many_evidence";
  too_many_evidence.evidence.push_back(
      {"overflow", exact.phases.back().id, "discrete_acknowledgement",
       "command_target_status", "post_command_sample"});
  const auto evidence_document = policy_document(too_many_evidence);
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   reference_for(too_many_evidence.policy_id,
                                 evidence_document, store_key),
                   store_key, evidence_document),
               ClosedSchemaError);
}

TEST(StopEvidencePolicyCodec,
     AcceptsMaximumSafeDurationAndRejectsFirstUnsafeInteger) {
  PolicySpec exact;
  exact.policy_id = "maximum_duration";
  exact.outer_duration_ns = core::kMaxStopEvidencePolicyDurationNs;
  exact.kernel_completion_reserve_ns = 1;
  exact.phases = {
      {"command", core::kMaxStopEvidencePolicyDurationNs - 1},
  };
  exact.evidence = {
      {"terminal", "command", "controller_goal_terminal",
       "controller_goal_status", "command_result"},
  };
  EXPECT_EQ(qualify(exact).value().deadline_budget->outer_duration_ns,
            core::kMaxStopEvidencePolicyDurationNs);

  const auto valid_document = policy_document(exact);
  const auto unsafe_document = replace_once(
      valid_document, "9007199254740991", "9007199254740992");
  const core::ContentAddressedStoreKey store_key{"sha256/unsafe-integer"};
  auto unsafe_reference =
      reference_for(exact.policy_id, valid_document, store_key);
  unsafe_reference.content_digest =
      core::ContentDigest(sha256_digest(unsafe_document));
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   unsafe_reference, store_key, unsafe_document),
               ArtifactCodecError);
}

TEST(StopEvidencePolicyCodec,
     RejectsEmptyRequiredCollectionsAndWrongApiVersion) {
  PolicySpec no_phases;
  no_phases.policy_id = "no_phases";
  no_phases.phases.clear();
  const auto no_phase_document = policy_document(no_phases);
  expect_closed_schema_rejection(no_phases.policy_id, no_phase_document,
                                 no_phase_document);

  PolicySpec no_evidence;
  no_evidence.policy_id = "no_evidence";
  no_evidence.evidence.clear();
  const auto no_evidence_document = policy_document(no_evidence);
  expect_closed_schema_rejection(no_evidence.policy_id, no_evidence_document,
                                 no_evidence_document);

  const PolicySpec valid;
  const auto valid_document = policy_document(valid);
  const auto wrong_api = replace_once(
      valid_document, "manyforge.stop_evidence_policy.v1",
      "manyforge.stop_evidence_policy.v2");
  expect_closed_schema_rejection(valid.policy_id, valid_document, wrong_api);
}

TEST(StopEvidencePolicyCodec,
     RejectsOpenShapeBadPresenceFreshnessAndAuthoritySet) {
  const PolicySpec valid;
  const auto document = policy_document(valid);
  const core::ContentAddressedStoreKey store_key{"sha256/shape-negative"};

  auto open_shape = document;
  const auto closing = open_shape.rfind('}');
  ASSERT_NE(closing, std::string::npos);
  open_shape.insert(closing, ",\"unexpected\":true");
  auto open_reference = reference_for(valid.policy_id, document, store_key);
  open_reference.content_digest =
      core::ContentDigest(sha256_digest(open_shape));
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   open_reference, store_key, open_shape),
               ClosedSchemaError);

  auto not_stoppable_with_command = not_stoppable_document();
  const auto not_closing = not_stoppable_with_command.rfind('}');
  ASSERT_NE(not_closing, std::string::npos);
  not_stoppable_with_command.insert(
      not_closing,
      ",\"containment_command\":{\"command_phase_id\":\"command\","
      "\"command\":{\"command_kind\":\"controller_goal_cancel\"}}");
  auto presence_reference =
      reference_for("non_interruptible", not_stoppable_document(), store_key);
  presence_reference.content_digest =
      core::ContentDigest(sha256_digest(not_stoppable_with_command));
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   presence_reference, store_key, not_stoppable_with_command),
               ClosedSchemaError);

  PolicySpec zero_freshness;
  zero_freshness.evidence.front().freshness_budget_ns = 0;
  const auto zero_document = policy_document(zero_freshness);
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   reference_for(zero_freshness.policy_id, zero_document,
                                 store_key),
                   store_key, zero_document),
               ClosedSchemaError);

  PolicySpec duplicate_authority;
  duplicate_authority.evidence = {
      {"rest", "terminal", "motion_rest", "binding_state_authority",
       "post_command_dwell", 25, 200, {"measured", "measured"}},
  };
  const auto duplicate_document = policy_document(duplicate_authority);
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   reference_for(duplicate_authority.policy_id,
                                 duplicate_document, store_key),
                   store_key, duplicate_document),
               ClosedSchemaError);
}

TEST(StopEvidencePolicyCodec,
     RejectsNestedUnknownMissingNullAndConditionalContractFields) {
  PolicySpec contract_policy;
  contract_policy.policy_id = "nested_contract";
  contract_policy.command_kind = "motion_controlled_stop";
  const auto contract_document = policy_document(contract_policy);

  expect_closed_schema_rejection(
      contract_policy.policy_id, contract_document,
      replace_once(contract_document,
                   "\"provider_contract_ref\":{",
                   "\"provider_contract_ref\":{\"unexpected\":true,"));
  expect_closed_schema_rejection(
      contract_policy.policy_id, contract_document,
      replace_once(
          contract_document,
          "\"contract_schema_id\":\"manyforge.stop_provider_contract.v1\",",
          ""));
  expect_closed_schema_rejection(
      contract_policy.policy_id, contract_document,
      replace_once(contract_document, "\"freshness_budget_ns\":25",
                   "\"freshness_budget_ns\":null"));
  expect_closed_schema_rejection(
      contract_policy.policy_id, contract_document,
      replace_once(contract_document,
                   "\"command_kind\":\"motion_controlled_stop\",",
                   "\"command_kind\":\"motion_controlled_stop\","
                   "\"registered_command_kind_id\":\"not_registered\","));
  const std::string provider_contract_member =
      ",\"provider_contract_ref\":" +
      canonical_json(provider_contract_ref());
  expect_closed_schema_rejection(
      contract_policy.policy_id, contract_document,
      replace_once(contract_document, provider_contract_member, ""));

  PolicySpec no_contract_policy;
  no_contract_policy.policy_id = "conditional_no_contract";
  const auto no_contract_document = policy_document(no_contract_policy);
  expect_closed_schema_rejection(
      no_contract_policy.policy_id, no_contract_document,
      replace_once(no_contract_document,
                   "\"command_kind\":\"controller_goal_cancel\"",
                   "\"command_kind\":\"controller_goal_cancel\","
                   "\"provider_contract_ref\":{"
                   "\"contract_schema_id\":"
                   "\"manyforge.stop_provider_contract.v1\","
                   "\"contract_schema_revision\":\"" +
                       std::string(kZeroDigest) +
                       "\",\"provider_contract_role_id\":\"stop_contract\"}"));
  expect_closed_schema_rejection(
      no_contract_policy.policy_id, no_contract_document,
      replace_once(
          no_contract_document, "\"containment_command\":{",
          "\"containment_command\":{\"fallback_command\":{"
          "\"command_kind\":\"controller_goal_cancel\"},"));

  PolicySpec registered;
  registered.policy_id = "registered_presence";
  registered.command_kind = "registered";
  const auto registered_document = policy_document(registered);
  expect_closed_schema_rejection(
      registered.policy_id, registered_document,
      replace_once(
          registered_document,
          ",\"registered_command_kind_id\":\"vendor_stop_command\"", ""));
}

TEST(StopEvidencePolicyCodec,
     RejectsConditionalEvidenceAndSourceFieldsOnWrongAlternatives) {
  PolicySpec goal;
  goal.policy_id = "goal_source_shape";
  const auto goal_document = policy_document(goal);
  expect_closed_schema_rejection(
      goal.policy_id, goal_document,
      replace_once(goal_document,
                   "\"freshness_budget_ns\":25,\"source_kind\":"
                   "\"controller_goal_status\"",
                   "\"evidence_endpoint_role_id\":\"stop_evidence\","
                   "\"freshness_budget_ns\":25,\"source_kind\":"
                   "\"controller_goal_status\""));

  PolicySpec provider;
  provider.policy_id = "provider_source_shape";
  provider.evidence = {
      {"safe", "terminal", "io_safe_output", "provider_evidence",
       "post_command_sample"},
  };
  const auto provider_document = policy_document(provider);
  expect_closed_schema_rejection(
      provider.policy_id, provider_document,
      replace_once(provider_document,
                   "\"evidence_endpoint_role_id\":\"stop_evidence\",",
                   "\"allowed_authority_classes\":[\"measured\"],"
                   "\"evidence_endpoint_role_id\":\"stop_evidence\","));

  PolicySpec binding;
  binding.policy_id = "binding_source_shape";
  binding.evidence = {
      {"rest", "terminal", "motion_rest", "binding_state_authority",
       "post_command_dwell", 25, 200},
  };
  const auto binding_document = policy_document(binding);
  expect_closed_schema_rejection(
      binding.policy_id, binding_document,
      replace_once(binding_document,
                   "\"allowed_authority_classes\":[\"measured\"],", ""));
  expect_closed_schema_rejection(
      binding.policy_id, binding_document,
      replace_once(binding_document,
                   ",\"provider_contract_ref\":" +
                       canonical_json(provider_contract_ref()),
                   ""));

  PolicySpec no_evidence_contract;
  no_evidence_contract.policy_id = "evidence_contract_shape";
  const auto no_evidence_contract_document =
      policy_document(no_evidence_contract);
  expect_closed_schema_rejection(
      no_evidence_contract.policy_id, no_evidence_contract_document,
      replace_once(
          no_evidence_contract_document,
          "\"evidence_kind\":\"controller_goal_terminal\"",
          "\"evidence_kind\":\"controller_goal_terminal\","
          "\"provider_contract_ref\":{"
          "\"contract_schema_id\":"
          "\"manyforge.stop_provider_contract.v1\","
          "\"contract_schema_revision\":\"" +
              std::string(kZeroDigest) +
              "\",\"provider_contract_role_id\":\"stop_contract\"}"));

  PolicySpec registered_evidence;
  registered_evidence.policy_id = "registered_evidence_presence";
  registered_evidence.evidence = {
      {"registered", "terminal", "registered", "provider_evidence",
       "post_command_sample"},
  };
  const auto registered_evidence_document =
      policy_document(registered_evidence);
  expect_closed_schema_rejection(
      registered_evidence.policy_id, registered_evidence_document,
      replace_once(
          registered_evidence_document,
          ",\"registered_evidence_kind_id\":\"vendor_terminal_evidence\"",
          ""));
}

TEST(StopEvidencePolicyCodec,
     PresentationIsIrrelevantButAuthoredEvidenceOrderIsSemantic) {
  PolicySpec spec;
  spec.evidence = {
      {"first", "terminal", "controller_goal_terminal",
       "controller_goal_status", "post_command_sample"},
      {"second", "terminal", "discrete_acknowledgement",
       "command_target_status", "post_command_sample"},
  };
  const auto canonical = policy_document(spec);
  const std::string yaml = "\n  " + canonical + "\n";
  const core::ContentAddressedStoreKey canonical_key{"sha256/stop-canonical"};
  const core::ContentAddressedStoreKey yaml_key{"sha256/stop-yaml"};
  const auto canonical_qualified = resolve_and_qualify_stop_evidence_policy(
      reference_for(spec.policy_id, canonical, canonical_key), canonical_key,
      canonical);
  const auto yaml_qualified = resolve_and_qualify_stop_evidence_policy(
      reference_for(spec.policy_id, yaml, yaml_key), yaml_key, yaml);
  EXPECT_EQ(canonical_qualified.value(), yaml_qualified.value());
  EXPECT_NE(sha256_digest(canonical), sha256_digest(yaml));

  std::swap(spec.evidence[0], spec.evidence[1]);
  EXPECT_NE(revision_from_document(canonical),
            revision_from_document(policy_document(spec)));
}

TEST(StopEvidencePolicyCodec,
     TypedCatalogReferenceQualifiesAllFieldsAndActualRetrievalKey) {
  PolicySpec spec;
  spec.policy_id = "reference_boundary";
  const auto document = policy_document(spec);
  const core::ContentAddressedStoreKey store_key{"sha256/stop-reference"};
  auto reference = reference_for(spec.policy_id, document, store_key);
  EXPECT_NO_THROW(resolve_and_qualify_stop_evidence_policy(
      reference, store_key, document));

  auto wrong_id = reference;
  wrong_id.record_id = core::StopEvidencePolicyId("different");
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   wrong_id, store_key, document),
               StopEvidencePolicyCodecError);

  auto wrong_revision = reference;
  wrong_revision.record_revision =
      core::StopEvidencePolicyRevision(std::string(kZeroDigest));
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   wrong_revision, store_key, document),
               StopEvidencePolicyCodecError);

  auto wrong_schema_id = reference;
  wrong_schema_id.schema_id = core::SchemaId("manyforge.contact_policy.v1");
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   wrong_schema_id, store_key, document),
               StopEvidencePolicyCodecError);

  auto wrong_schema_revision = reference;
  wrong_schema_revision.schema_revision =
      core::SchemaRevision(std::string(kZeroDigest));
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   wrong_schema_revision, store_key, document),
               StopEvidencePolicyCodecError);

  auto wrong_digest = reference;
  wrong_digest.content_digest = core::ContentDigest(std::string(kZeroDigest));
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   wrong_digest, store_key, document),
               StopEvidencePolicyCodecError);

  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   reference, core::ContentAddressedStoreKey("sha256/other"),
                   document),
               StopEvidencePolicyCodecError);
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   reference, core::ContentAddressedStoreKey(""), document),
               StopEvidencePolicyCodecError);

  const std::string oversized(kMaxArtifactEncodedBytes + 1U, ' ');
  EXPECT_THROW(resolve_and_qualify_stop_evidence_policy(
                   reference, store_key, oversized),
               StopEvidencePolicyCodecError);
}

TEST(StopEvidencePolicyCodec,
     RejectsStalePayloadSelfRevisionAfterExactByteQualification) {
  PolicySpec spec;
  spec.policy_id = "stale_self_revision";
  const auto valid_document = policy_document(spec);
  auto stale_document = valid_document;
  const auto revision_offset =
      stale_document.find("\"stop_evidence_policy_revision\":\"sha256:");
  ASSERT_NE(revision_offset, std::string::npos);
  const auto digit_offset =
      revision_offset +
      std::string("\"stop_evidence_policy_revision\":\"sha256:").size();
  ASSERT_LT(digit_offset, stale_document.size());
  stale_document[digit_offset] =
      stale_document[digit_offset] == '0' ? '1' : '0';

  const core::ContentAddressedStoreKey store_key{"sha256/stale-stop-policy"};
  const auto stale_reference =
      reference_for(spec.policy_id, stale_document, store_key);
  EXPECT_THROW(
      {
        try {
          (void)resolve_and_qualify_stop_evidence_policy(
              stale_reference, store_key, stale_document);
        } catch (const StopEvidencePolicyCodecError& error) {
          EXPECT_NE(std::string(error.what()).find(
                        "does not match normalized semantic content"),
                    std::string::npos);
          throw;
        }
      },
      StopEvidencePolicyCodecError);
}

// Runtime remaining-budget shortening, provider-role resolution, topology
// expansion, concrete endpoint selection, complete invocation-plan
// construction, and Stop-certificate generation are deliberately outside
// this catalog resolver. They are compiler/Kernel qualification work.

}  // namespace
}  // namespace manyforge::deployment
