#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <manyforge_core/types/contact_policy.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/contact_policy_codec.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kSchemaRevision =
    "sha256:79f101a9c8cc2acf8db6ecaffd3476b03723afd5a31cce2a503f7b8764b47912";

static_assert(!std::is_aggregate_v<QualifiedContactPolicy>);
static_assert(!std::is_default_constructible_v<QualifiedContactPolicy>);

[[nodiscard]] CanonicalValue component_selector() {
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("component_model_body")},
      {"component_role_id", CanonicalValue("robot")},
      {"model_role_id", CanonicalValue("arm_model")},
      {"collision_body_role_id", CanonicalValue("tool_collision")},
  });
}

[[nodiscard]] CanonicalValue runtime_selector() {
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("runtime_object")},
      {"runtime_object_id", CanonicalValue("selected_object")},
  });
}

[[nodiscard]] CanonicalValue body_pair(bool reverse = false) {
  return CanonicalValue(CanonicalValue::Object{
      {"body_a", reverse ? runtime_selector() : component_selector()},
      {"body_b", reverse ? component_selector() : runtime_selector()},
  });
}

[[nodiscard]] CanonicalValue zone(std::string_view mode,
                                  std::string_view zone_id = "tool_contact",
                                  bool reverse_pair = false,
                                  bool add_release_field = false) {
  CanonicalValue::Object result{
      {"zone_id", CanonicalValue(std::string(zone_id))},
      {"body_pairs",
       CanonicalValue(CanonicalValue::Array{body_pair(reverse_pair)})},
      {"violation_response", CanonicalValue("hard_stop")},
  };
  if (mode == "contact_release") {
    result.emplace_back("release_evidence_mode",
                        CanonicalValue("signed_penetration"));
    result.emplace_back("enforce_max_start_penetration", CanonicalValue(true));
    result.emplace_back("max_start_penetration_m", CanonicalValue(0.0));
    result.emplace_back("min_release_progress_m", CanonicalValue(0.0));
    result.emplace_back("release_tolerance_m", CanonicalValue(1e-5));
    result.emplace_back("require_strictly_decreasing_penetration",
                        CanonicalValue(true));
    result.emplace_back("require_separation_by_end", CanonicalValue(true));
    result.emplace_back("allow_recontact", CanonicalValue(false));
  } else if (mode == "bounded_penetration") {
    result.emplace_back("max_contact_penetration_m", CanonicalValue(0.005));
  } else if (mode == "contact_grasp") {
    result.emplace_back("max_penetration_m", CanonicalValue(0.002));
    result.emplace_back("terminal_window_start_fraction", CanonicalValue(0.8));
  }
  if (add_release_field) {
    result.emplace_back("release_tolerance_m", CanonicalValue(0.0));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue
policy_without_revision(std::string_view mode = "contact_grasp",
                        bool reverse_pair = false, bool duplicate_pair = false,
                        bool add_release_field = false) {
  CanonicalValue::Array zones{
      zone(mode, "tool_contact", reverse_pair, add_release_field)};
  if (duplicate_pair) {
    zones.push_back(zone(mode, "tool_contact_second"));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kContactPolicyApiVersion))},
      {"policy_id", CanonicalValue("selected_object_grasp")},
      {"description", CanonicalValue("Audited geometry contact envelope")},
      {"mode", CanonicalValue(std::string(mode))},
      {"sensing_mode", CanonicalValue("geometry_only")},
      {"default_unmatched_response", CanonicalValue("hard_stop")},
      {"zones", CanonicalValue(std::move(zones))},
  });
}

[[nodiscard]] std::string document(CanonicalValue value) {
  std::string preimage{"manyforge.content-identity.v1"};
  preimage.push_back('\0');
  preimage.append("catalog-record-revision/contact_policy");
  preimage.push_back('\0');
  preimage.append(core::kContactPolicyApiVersion);
  preimage.push_back('\0');
  preimage.append(canonical_json(value));
  auto object = value.as_object();
  object.emplace_back("version", CanonicalValue(sha256_digest(preimage)));
  return canonical_json(CanonicalValue(std::move(object)));
}

[[nodiscard]] std::string revision(std::string_view bytes) {
  const auto parsed = parse_strict_json_model_yaml(bytes);
  const auto *value = parsed.root().find_object_member("version");
  EXPECT_NE(value, nullptr);
  return value == nullptr ? std::string{} : value->as_string();
}

[[nodiscard]] core::ContactPolicyCatalogRecordRefDTO
reference_for(std::string_view bytes,
              const core::ContentAddressedStoreKey &key =
                  core::ContentAddressedStoreKey("sha256/contact-policy")) {
  return {core::ContactPolicyId("selected_object_grasp"),
          core::ContactPolicyRevision(revision(bytes)),
          core::SchemaId(std::string(core::kContactPolicyApiVersion)),
          core::SchemaRevision(std::string(kSchemaRevision)),
          core::ContentDigest(sha256_digest(bytes)),
          key};
}

TEST(ContactPolicyCodec, QualifiesEverySupportedClosedGeometryModality) {
  for (const auto mode :
       {"contact_release", "bounded_penetration", "contact_grasp"}) {
    const std::string bytes = document(policy_without_revision(mode));
    const auto reference = reference_for(bytes);
    const auto qualified = resolve_and_qualify_contact_policy(
        reference, reference.content_addressed_store_key, bytes);
    EXPECT_EQ(qualified.reference(), reference);
    ASSERT_EQ(qualified.value().zones.size(), 1U);
    EXPECT_EQ(qualified.value().zones.front().body_pairs.size(), 1U);
    EXPECT_EQ(qualified.value().sensing_mode,
              core::ContactPolicySensingModeDTO::kGeometryOnly);
  }
}

TEST(ContactPolicyCodec, ProducesExactTypedGraspEnvelopeAndSelectors) {
  const std::string bytes = document(policy_without_revision());
  const auto reference = reference_for(bytes);
  const auto qualified = resolve_and_qualify_contact_policy(
      reference, reference.content_addressed_store_key, bytes);

  EXPECT_EQ(qualified.value().contact_policy_id.value(),
            "selected_object_grasp");
  EXPECT_EQ(qualified.value().mode, core::ContactPolicyModeDTO::kContactGrasp);
  const auto &selected_zone = qualified.value().zones.front();
  ASSERT_TRUE(std::holds_alternative<core::ContactGraspEnvelopeDTO>(
      selected_zone.envelope));
  const auto &envelope =
      std::get<core::ContactGraspEnvelopeDTO>(selected_zone.envelope);
  EXPECT_DOUBLE_EQ(envelope.max_penetration_m, 0.002);
  EXPECT_DOUBLE_EQ(envelope.terminal_window_start_fraction, 0.8);
  EXPECT_TRUE(
      std::holds_alternative<core::ContactComponentModelBodySelectorDTO>(
          selected_zone.body_pairs.front().body_a));
  EXPECT_TRUE(std::holds_alternative<core::ContactRuntimeObjectSelectorDTO>(
      selected_zone.body_pairs.front().body_b));
}

TEST(ContactPolicyCodec, RejectsNoncanonicalAndCrossZoneDuplicatePairs) {
  for (const auto &candidate :
       {policy_without_revision("contact_grasp", true, false),
        policy_without_revision("contact_grasp", false, true)}) {
    const std::string bytes = document(candidate);
    const auto reference = reference_for(bytes);
    EXPECT_THROW(resolve_and_qualify_contact_policy(
                     reference, reference.content_addressed_store_key, bytes),
                 ContactPolicyCodecError);
  }
}

TEST(ContactPolicyCodec, RejectsReservedModesAndCrossModeFields) {
  for (const auto mode : {"contact_process", "contact_hold"}) {
    const std::string bytes = document(policy_without_revision(mode));
    const auto reference = reference_for(bytes);
    EXPECT_THROW(resolve_and_qualify_contact_policy(
                     reference, reference.content_addressed_store_key, bytes),
                 ContactPolicyCodecError);
  }

  const std::string bytes = document(
      policy_without_revision("bounded_penetration", false, false, true));
  const auto reference = reference_for(bytes);
  EXPECT_THROW(resolve_and_qualify_contact_policy(
                   reference, reference.content_addressed_store_key, bytes),
               ContactPolicyCodecError);
}

TEST(ContactPolicyCodec, RejectsSchemaIdentityAndReferentMismatch) {
  const std::string bytes = document(policy_without_revision());
  const auto reference = reference_for(bytes);

  auto wrong_id = reference;
  wrong_id.record_id = core::ContactPolicyId("other_policy");
  EXPECT_THROW(resolve_and_qualify_contact_policy(
                   wrong_id, wrong_id.content_addressed_store_key, bytes),
               ContactPolicyCodecError);

  auto wrong_schema = reference;
  wrong_schema.schema_revision = core::SchemaRevision(
      "sha256:"
      "0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_THROW(
      resolve_and_qualify_contact_policy(
          wrong_schema, wrong_schema.content_addressed_store_key, bytes),
      ContactPolicyCodecError);

  std::string malformed = bytes;
  const auto offset = malformed.find("\"max_penetration_m\":0.002");
  ASSERT_NE(offset, std::string::npos);
  malformed.replace(offset, std::string("\"max_penetration_m\":0.002").size(),
                    "\"max_penetration_m\":0.002,\"unknown\":false");
  auto malformed_reference = reference;
  malformed_reference.content_digest =
      core::ContentDigest(sha256_digest(malformed));
  EXPECT_THROW(resolve_and_qualify_contact_policy(
                   malformed_reference,
                   malformed_reference.content_addressed_store_key, malformed),
               ContactPolicyCodecError);
}

TEST(ContactPolicyCodec,
     ApplicabilitySelectsExactPolicyAndCompilerRetainsItsFullReference) {
  const std::string bytes = document(policy_without_revision());
  const auto reference = reference_for(bytes);
  const auto qualified = resolve_and_qualify_contact_policy(
      reference, reference.content_addressed_store_key, bytes);

  const auto graph = compute_graph_revision(prepare_cell_graph_identity(
      validate_intrinsic_cell_graph_candidate(
          decode_cell_graph_candidate(parse_strict_json_model_yaml(
              "{\"api_version\":\"manyforge.cell_graph.v1\","
              "\"graph_revision\":\"sha256:00000000000000000000000000000000"
              "00000000000000000000000000000000\",\"cell_id\":"
              "\"contact_policy_cell\",\"runtime_units\":[],"
              "\"model_instances\":[],\"controllable_components\":[],"
              "\"control_domains\":[],\"control_bindings\":[],"
              "\"component_attachments\":[]}"))),
      {}));
  const auto composition = compose_cell_occurrences(
      graph, std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const CanonicalValue policy_reference(CanonicalValue::Object{
      {"record_kind", CanonicalValue("contact_policy")},
      {"record_id", CanonicalValue(reference.record_id.value())},
      {"record_revision", CanonicalValue(reference.record_revision.value())},
      {"schema_id", CanonicalValue(reference.schema_id.value())},
      {"schema_revision", CanonicalValue(reference.schema_revision.value())},
      {"content_digest", CanonicalValue(reference.content_digest.value())},
      {"content_addressed_store_key",
       CanonicalValue(reference.content_addressed_store_key.value())},
  });
  const CanonicalValue applicability_root(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"desired_control_configurations",
       CanonicalValue(CanonicalValue::Array{})},
      {"capability_provider_bindings", CanonicalValue(CanonicalValue::Array{})},
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
  const auto applicability =
      qualify_cell_applicability(composition, inputs, {}, {}, {}, {}, {},
                                 {}, {}, {}, {}, overlay, policies);
  ASSERT_EQ(applicability.contact_policies().size(), 1U);
  EXPECT_EQ(applicability.contact_policies().front().reference(), reference);
  const auto compiled = compile_cell_ir(applicability);
  ASSERT_EQ(compiled.catalog_record_refs().size(), 1U);
  EXPECT_EQ(compiled.catalog_record_refs().front(),
            core::CatalogRecordRefDTO(reference));

  EXPECT_THROW((void)qualify_cell_applicability(composition, inputs, {}, {},
                                                {}, {}, {}, {}, {}, {}, {},
                                                overlay),
               CellApplicabilityError);
}

} // namespace
} // namespace manyforge::deployment
