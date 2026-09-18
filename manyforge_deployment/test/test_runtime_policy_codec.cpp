#include <gtest/gtest.h>

#include <manyforge_core/types/runtime_policy.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/runtime_policy_codec.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kSchemaRevision =
    "sha256:fa386ae2d58f4bf2e19897a114cfb5e5f80cbd08a23a23eeac38e3342ced7fb4";

static_assert(!std::is_aggregate_v<QualifiedRuntimePolicy>);
static_assert(!std::is_default_constructible_v<QualifiedRuntimePolicy>);

[[nodiscard]] CanonicalValue policy_without_revision(
    bool reverse_interrupts = false, bool duplicate_acknowledgement = false,
    bool include_target = true,
    std::string_view policy_id = "runtime.default") {
  const CanonicalValue acknowledgement(CanonicalValue::Object{
      {"controllable_component_id", CanonicalValue("robot_a")},
      {"command_target_id", CanonicalValue("gripper")},
      {"environment", CanonicalValue("simulation")},
      {"allowed", CanonicalValue(true)}});
  CanonicalValue::Array acknowledgements;
  if (include_target) acknowledgements.push_back(acknowledgement);
  if (duplicate_acknowledgement) acknowledgements.push_back(acknowledgement);
  CanonicalValue::Array interrupt_ids{
      CanonicalValue("fault.stop"), CanonicalValue("probe.trip")};
  if (reverse_interrupts) std::reverse(interrupt_ids.begin(), interrupt_ids.end());
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kRuntimePolicyApiVersion))},
      {"runtime_policy_id", CanonicalValue(std::string(policy_id))},
      {"description", CanonicalValue("Maintained native policy")},
      {"interrupt_policy_ids", CanonicalValue(std::move(interrupt_ids))},
      {"open_loop_grasp_acknowledgements",
       CanonicalValue(std::move(acknowledgements))}});
}

[[nodiscard]] std::string document(CanonicalValue value) {
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/runtime_policy");
  preimage.push_back('\0');
  preimage.append(core::kRuntimePolicyApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(value));
  auto object = value.as_object();
  object.emplace_back("runtime_policy_revision",
                      CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(object)));
}

[[nodiscard]] std::string revision(std::string_view bytes) {
  const auto parsed = parse_strict_json_model_yaml(bytes);
  const auto* value =
      parsed.root().find_object_member("runtime_policy_revision");
  EXPECT_NE(value, nullptr);
  return value == nullptr ? std::string{} : value->as_string();
}

[[nodiscard]] core::RuntimePolicyCatalogRecordRefDTO reference_for(
    std::string_view bytes,
    std::string_view policy_id = "runtime.default",
    const core::ContentAddressedStoreKey& key =
        core::ContentAddressedStoreKey("sha256/runtime-policy")) {
  return {core::RuntimePolicyId(std::string(policy_id)),
          core::RuntimePolicyRevision(revision(bytes)),
          core::SchemaId(std::string(core::kRuntimePolicyApiVersion)),
          core::SchemaRevision(std::string(kSchemaRevision)),
          core::ContentDigest(sha256_digest(bytes)), key};
}

[[nodiscard]] CanonicalValue reference_value(
    const core::RuntimePolicyCatalogRecordRefDTO& reference) {
  return CanonicalValue(CanonicalValue::Object{
      {"record_kind", CanonicalValue("runtime_policy")},
      {"record_id", CanonicalValue(reference.record_id.value())},
      {"record_revision",
       CanonicalValue(reference.record_revision.value())},
      {"schema_id", CanonicalValue(reference.schema_id.value())},
      {"schema_revision", CanonicalValue(reference.schema_revision.value())},
      {"content_digest", CanonicalValue(reference.content_digest.value())},
      {"content_addressed_store_key",
       CanonicalValue(reference.content_addressed_store_key.value())},
  });
}

TEST(RuntimePolicyCodec, ResolvesExactTypedPolicyAndMaterializedDefaults) {
  const std::string bytes = document(policy_without_revision());
  const auto reference = reference_for(bytes);
  const auto qualified = resolve_and_qualify_runtime_policy(
      reference, reference.content_addressed_store_key, bytes);

  EXPECT_EQ(qualified.reference(), reference);
  EXPECT_EQ(qualified.value().runtime_policy_id.value(), "runtime.default");
  ASSERT_EQ(qualified.value().interrupt_policy_ids.size(), 2U);
  EXPECT_EQ(qualified.value().interrupt_policy_ids[0].value(), "fault.stop");
  ASSERT_EQ(qualified.value().open_loop_grasp_acknowledgements.size(), 1U);
  EXPECT_TRUE(
      qualified.value().open_loop_grasp_acknowledgements[0].allowed);
}

TEST(RuntimePolicyCodec, RejectsNoncanonicalInterruptsAndDuplicateAcknowledgements) {
  for (const auto& candidate :
       {policy_without_revision(true, false),
        policy_without_revision(false, true)}) {
    const std::string bytes = document(candidate);
    const auto reference = reference_for(bytes);
    EXPECT_THROW(resolve_and_qualify_runtime_policy(
                     reference, reference.content_addressed_store_key, bytes),
                 RuntimePolicyCodecError);
  }
}

TEST(RuntimePolicyCodec, RejectsSchemaIdentityAndReferentMismatch) {
  const std::string bytes = document(policy_without_revision());
  const auto reference = reference_for(bytes);

  auto wrong_id = reference;
  wrong_id.record_id = core::RuntimePolicyId("runtime.other");
  EXPECT_THROW(resolve_and_qualify_runtime_policy(
                   wrong_id, wrong_id.content_addressed_store_key, bytes),
               RuntimePolicyCodecError);

  auto wrong_schema = reference;
  wrong_schema.schema_revision = core::SchemaRevision(
      "sha256:0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(resolve_and_qualify_runtime_policy(
                   wrong_schema, wrong_schema.content_addressed_store_key,
                   bytes),
               RuntimePolicyCodecError);

  std::string malformed = bytes;
  const auto offset = malformed.find("\"allowed\":true");
  ASSERT_NE(offset, std::string::npos);
  malformed.replace(offset, std::string("\"allowed\":true").size(),
                    "\"allowed\":true,\"unknown\":false");
  auto malformed_reference = reference;
  malformed_reference.content_digest =
      core::ContentDigest(sha256_digest(malformed));
  EXPECT_THROW(resolve_and_qualify_runtime_policy(
                   malformed_reference,
                   malformed_reference.content_addressed_store_key,
                   malformed),
               RuntimePolicyCodecError);
}

TEST(RuntimePolicyCodec,
     ApplicabilitySelectsExactPolicyAndCompilerRetainsItsFullReference) {
  const std::string bytes = document(policy_without_revision(false, false,
                                                              false));
  const auto reference = reference_for(bytes);
  const auto qualified = resolve_and_qualify_runtime_policy(
      reference, reference.content_addressed_store_key, bytes);

  const auto graph = compute_graph_revision(prepare_cell_graph_identity(
      validate_intrinsic_cell_graph_candidate(decode_cell_graph_candidate(
          parse_strict_json_model_yaml(
              "{\"api_version\":\"manyforge.cell_graph.v1\","
              "\"graph_revision\":\"sha256:00000000000000000000000000000000"
              "00000000000000000000000000000000\",\"cell_id\":"
              "\"runtime_policy_cell\",\"runtime_units\":[],"
              "\"model_instances\":[],\"controllable_components\":[],"
              "\"control_domains\":[],\"control_bindings\":[],"
              "\"component_attachments\":[]}"))),
      {}));
  const auto composition = compose_cell_occurrences(
      graph, std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});

  const CanonicalValue policy_reference = reference_value(reference);
  const CanonicalValue applicability_root(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"desired_control_configurations",
       CanonicalValue(CanonicalValue::Array{})},
      {"capability_provider_bindings",
       CanonicalValue(CanonicalValue::Array{})},
      {"policies",
       CanonicalValue(CanonicalValue::Object{
           {"retained_policy_records",
            CanonicalValue(CanonicalValue::Array{policy_reference})},
           {"stop_stage_parameters", CanonicalValue(CanonicalValue::Array{})},
           {"commissioned_stop_interfaces",
            CanonicalValue(CanonicalValue::Array{})},
           {"interrupt_sources", CanonicalValue(CanonicalValue::Array{})},
           {"interrupt_policies", CanonicalValue(CanonicalValue::Array{})},
           {"interrupt_config",
            CanonicalValue(CanonicalValue::Object{
                {"queue_depth", CanonicalValue(8.0)},
                {"cancel_validation_timeout_ns", CanonicalValue(1000.0)},
                {"motion_scope_arm_timeout_ns", CanonicalValue(1000.0)},
                {"supervisory_poll_rate_hz", CanonicalValue(10.0)},
            })},
       })},
  });
  const auto inputs = compute_capability_provider_binding_revisions(
      decode_deployment_applicability_inputs_candidate(
          parse_strict_json_model_yaml(canonical_json(applicability_root))));
  const auto overlay = select_site_overlay_for_deployment(
      inputs.value().site_overlay_ref, inputs.value().external_resources,
      nullptr);
  const std::array policies{qualified};
  const auto applicability = qualify_cell_applicability(
      composition, inputs, {}, policies, {}, {}, {}, {}, {}, {}, {}, {},
      overlay);
  ASSERT_EQ(applicability.runtime_policies().size(), 1U);
  EXPECT_EQ(applicability.runtime_policies().front().reference(), reference);
  const auto compiled = compile_cell_ir(applicability);
  ASSERT_EQ(compiled.catalog_record_refs().size(), 1U);
  EXPECT_EQ(compiled.catalog_record_refs().front(),
            core::CatalogRecordRefDTO(reference));

  EXPECT_THROW(
      (void)qualify_cell_applicability(composition, inputs, {}, {}, {}, {},
                                       {}, {}, {}, {}, {}, {}, overlay),
      CellApplicabilityError);

  const std::string second_bytes = document(policy_without_revision(
      false, false, false, "runtime.secondary"));
  const auto second_reference = reference_for(
      second_bytes, "runtime.secondary",
      core::ContentAddressedStoreKey("sha256/runtime-policy-secondary"));
  const auto second_qualified = resolve_and_qualify_runtime_policy(
      second_reference, second_reference.content_addressed_store_key,
      second_bytes);
  const CanonicalValue two_policy_root(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"desired_control_configurations",
       CanonicalValue(CanonicalValue::Array{})},
      {"capability_provider_bindings",
       CanonicalValue(CanonicalValue::Array{})},
      {"policies",
       CanonicalValue(CanonicalValue::Object{
           {"retained_policy_records",
            CanonicalValue(CanonicalValue::Array{
                policy_reference, reference_value(second_reference)})},
           {"stop_stage_parameters", CanonicalValue(CanonicalValue::Array{})},
           {"commissioned_stop_interfaces",
            CanonicalValue(CanonicalValue::Array{})},
           {"interrupt_sources", CanonicalValue(CanonicalValue::Array{})},
           {"interrupt_policies", CanonicalValue(CanonicalValue::Array{})},
           {"interrupt_config",
            CanonicalValue(CanonicalValue::Object{
                {"queue_depth", CanonicalValue(8.0)},
                {"cancel_validation_timeout_ns", CanonicalValue(1000.0)},
                {"motion_scope_arm_timeout_ns", CanonicalValue(1000.0)},
                {"supervisory_poll_rate_hz", CanonicalValue(10.0)},
            })},
       })},
  });
  const auto two_policy_inputs =
      compute_capability_provider_binding_revisions(
          decode_deployment_applicability_inputs_candidate(
              parse_strict_json_model_yaml(canonical_json(two_policy_root))));
  const std::array two_policies{qualified, second_qualified};
  EXPECT_THROW(
      (void)qualify_cell_applicability(
          composition, two_policy_inputs, {}, two_policies, {}, {}, {}, {},
          {}, {}, {}, {}, select_site_overlay_for_deployment(
                               two_policy_inputs.value().site_overlay_ref,
                               two_policy_inputs.value().external_resources,
                               nullptr)),
      CellApplicabilityError);
}

}  // namespace
}  // namespace manyforge::deployment
