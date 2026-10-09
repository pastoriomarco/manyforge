#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] std::string Digest(const char digit) {
  return "sha256:" + std::string(64U, digit);
}

[[nodiscard]] RuntimeResourceBindingDTO Resource(std::string joint, std::string resource_id) {
  return {ResourceRoleId("role." + joint), ResourceId(std::move(resource_id)), std::move(joint),
          "compiled.joint", CoordinateKind::kAngular};
}

[[nodiscard]] std::vector<RuntimeResourceBindingDTO> Resources() {
  return {Resource("joint_1", "joint.1"), Resource("joint_2", "joint.2")};
}

[[nodiscard]] std::vector<HardStopResourceLimitDTO> DefaultLimits() {
  return {{ResourceId("joint.1"), AngularVelocityDTO{0.02}},
          {ResourceId("joint.2"), AngularVelocityDTO{0.02}}};
}

[[nodiscard]] MotionInterruptStopProfileDTO StopProfile() {
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "scope.soft.v1";
  profile.jerk_limits_enabled = false;
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 0.25;
  profile.per_joint_max_deceleration = {
      {"joint_1", AngularAccelerationDTO{2.0}},
      {"joint_2", AngularAccelerationDTO{2.0}},
  };
  profile.allow_natural_completion_on_short_path = true;
  profile.recoverable_substitution_mode = MotionInterruptStopMode::kSpringBackHold;
  return profile;
}

[[nodiscard]] PreparedParticipantStopPlanContextDTO PreparedContext() {
  return {OperationId("operation.main"),
          OperationRevision(3U),
          OperationParticipantId("participant.arm"),
          ProgramId("program.main"),
          ProgramRevision(Digest('1')),
          ProgramBindingId(Digest('2')),
          ProgramRunId("run.main"),
          StopProfile(),
          ParticipantStopProfilePurposeDTO::kMotionInterruptScope,
          std::string("scope.contact"),
          MotionInterruptNodeCategory::kGeneric};
}

[[nodiscard]] RetainedArtifactRefDTO Artifact(std::string role, std::string media_type,
                                              const char digest_digit) {
  const std::string digest = Digest(digest_digit);
  return {ArtifactRoleId(std::move(role)), std::move(media_type), 17U, ContentDigest(digest),
          ContentAddressedStoreKey("sha256/" + digest.substr(7U))};
}

[[nodiscard]] ProviderContractRefDTO Contract() {
  return {ProviderContractRoleId("stop.command"),
          SchemaId("manyforge.value_schema.stop.command.v1"), SchemaRevision(Digest('1'))};
}

[[nodiscard]] ProviderContractRefDTO MotionContract() {
  return {ProviderContractRoleId("stop.command"),
          SchemaId(std::string(kMotionControlledStopContractSchemaId)),
          SchemaRevision(std::string(kMotionControlledStopContractSchemaRevision))};
}

[[nodiscard]] ResolvedStopRuntimeTargetDTO Target(std::string endpoint_role,
                                                  std::string endpoint_symbol) {
  return {ProviderInstanceId("provider.instance"),
          ProviderManifestId("provider.manifest"),
          ProviderManifestRevision(Digest('2')),
          RuntimeUnitId("runtime.unit"),
          ProductId("runtime.product"),
          ProductContractId("runtime.contract"),
          ProductContractRevision(Digest('3')),
          ContentDigest(Digest('4')),
          RuntimeEpoch("provider.epoch"),
          RuntimeEpoch("runtime.epoch"),
          EndpointRoleId(std::move(endpoint_role)),
          std::move(endpoint_symbol),
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

[[nodiscard]] StopEvidenceRequirementDTO MotionRestRequirement(std::string evidence_id,
                                                               std::string phase_id) {
  return {StopEvidenceItemId(std::move(evidence_id)),
          StopBudgetPhaseId(std::move(phase_id)),
          StopEvidenceRequirementKind::kMotionRest,
          Contract(),
          std::nullopt,
          {StopEvidenceSourceKind::kBindingStateAuthority,
           250'000'000LL,
           EndpointRoleId("state.stop"),
           {StateAuthorityClass::kMeasured}},
          {StopEvidenceObservationKind::kPostCommandDwell, 100'000'000LL}};
}

[[nodiscard]] ResolvedStopEvidenceRequirementDTO ResolvedMotionRest(std::string evidence_id,
                                                                    std::string phase_id) {
  const auto requirement = MotionRestRequirement(std::move(evidence_id), std::move(phase_id));
  return {requirement,
          {StopEvidenceSourceKind::kBindingStateAuthority, Target("state.stop", "state.stop"),
           StateAuthorityId("state.measured"), StateAuthorityClass::kMeasured, std::nullopt}};
}

[[nodiscard]] ResolvedStopEvidenceRequirementDTO ResolvedProviderEvidence(std::string evidence_id,
                                                                          std::string phase_id) {
  StopEvidenceRequirementDTO requirement{
      StopEvidenceItemId(std::move(evidence_id)),
      StopBudgetPhaseId(std::move(phase_id)),
      StopEvidenceRequirementKind::kIoSafeOutput,
      Contract(),
      std::nullopt,
      {StopEvidenceSourceKind::kProviderEvidence,
       250'000'000LL,
       EndpointRoleId("evidence.stop"),
       {}},
      {StopEvidenceObservationKind::kPostCommandSample, std::nullopt}};
  return {requirement,
          {StopEvidenceSourceKind::kProviderEvidence, Target("evidence.stop", "evidence.stop"),
           std::nullopt, std::nullopt, Contract()}};
}

[[nodiscard]] ControlledStopSubstitutionContractDTO Substitution() {
  return {Contract(), MotionControlledStopMode::kSoftOnPath,
          MotionControlledStopMode::kSpringBackHold};
}

[[nodiscard]] ResolvedStopEscalationStageDTO NominalStage() {
  JsonObjectDTO parameters;
  parameters.emplace("stop_mode", JsonValueDTO{std::string("soft_on_path")});
  return {0U,
          StopEscalationStageId("stage.nominal"),
          StopEscalationStageKind::kNominal,
          ContainmentGranularity::kResource,
          {StopBudgetPhaseId("phase.nominal.command"),
           StopContainmentCommandKind::kMotionControlledStop, Contract(), std::nullopt},
          std::move(parameters),
          {StopActionClass::kControlled, Substitution(), std::nullopt, std::nullopt, std::nullopt,
           std::nullopt},
          Target("command.stop", "command.stop"),
          ResolvedStopResourceContainmentScopeDTO{ActivationSetId("activation.main"),
                                                  {ResourceId("joint.1"), ResourceId("joint.2")}},
          StopEvidenceConjunction::kAllOf,
          {ResolvedMotionRest("evidence.nominal.rest", "phase.nominal.rest")}};
}

[[nodiscard]] ResolvedStopEscalationStageDTO HardStage() {
  return {1U,
          StopEscalationStageId("stage.hard"),
          StopEscalationStageKind::kSoftwareHard,
          ContainmentGranularity::kControllerSet,
          {StopBudgetPhaseId("phase.hard.command"), StopContainmentCommandKind::kRegistered,
           Contract(), RegisteredStopCommandKindId("manyforge.stop.software_hard")},
          JsonObjectDTO{},
          {StopActionClass::kSoftwareHard, std::nullopt, std::nullopt, std::nullopt, std::nullopt,
           std::nullopt},
          Target("command.stop", "command.stop"),
          ResolvedStopControllerSetContainmentScopeDTO{
              ActivationSetId("activation.main"),
              {{ControllerRoleId("controller.arm"), "controller.arm"}}},
          StopEvidenceConjunction::kAllOf,
          {ResolvedMotionRest("evidence.hard.rest", "phase.hard.rest")}};
}

[[nodiscard]] ResolvedStopEscalationStageDTO SafetyStage() {
  return {2U,
          StopEscalationStageId("stage.safety"),
          StopEscalationStageKind::kSafetySystemRequest,
          ContainmentGranularity::kControllerSet,
          {StopBudgetPhaseId("phase.safety.command"), StopContainmentCommandKind::kRegistered,
           Contract(), RegisteredStopCommandKindId("manyforge.stop.safety.protective_request")},
          JsonObjectDTO{},
          {StopActionClass::kSafetySystemRequest, std::nullopt,
           SafetySystemRequestActionClass::kProtectiveStopRequest,
           CommissionedStopInterfaceId("stop.interface.protective"),
           CommissionedStopInterfaceRevision(Digest('5')),
           Artifact(std::string(kStopInterfaceCommissioningArtifactRole),
                    std::string(kStopInterfaceCommissioningMediaType), '5')},
          Target("command.stop", "command.stop"),
          ResolvedStopControllerSetContainmentScopeDTO{
              ActivationSetId("activation.main"),
              {{ControllerRoleId("controller.arm"), "controller.arm"}}},
          StopEvidenceConjunction::kAllOf,
          {ResolvedMotionRest("evidence.safety.rest", "phase.safety.rest")}};
}

[[nodiscard]] ResolvedStopBasePlanDTO Candidate() {
  return {ResolvedStopBasePlanId("pending"),
          CellId("cell.main"),
          RetainedBundleId(Digest('8')),
          GraphRevision(Digest('9')),
          {ProductId("control.product"), ProductContractId("control.contract"),
           ProductContractRevision(Digest('a')),
           Artifact("resolved_stop_template",
                    "application/vnd.manyforge.resolved-stop-template.v1+json", 'b'),
           "stop.main"},
          ControlDomainId("domain.arm"),
          ControlDomainRevision(Digest('c')),
          ControlBindingId("binding.arm"),
          ControlBindingRevision(Digest('d')),
          ControllerTopologyId("topology.arm"),
          TopologyRevision(Digest('e')),
          ControlRouteId("route.arm"),
          ActivationSetId("activation.main"),
          ActiveControlConfigurationId("active.arm"),
          ActiveControlConfigurationGeneration(7U),
          RuntimeUnitId("runtime.cclc"),
          RuntimeEpoch("runtime.cclc.epoch"),
          11U,
          StopEvidencePolicyId("policy.arm"),
          StopEvidencePolicyRevision(Digest('f')),
          ContainmentGranularity::kControllerSet,
          {5'000'000'000LL,
           500'000'000LL,
           {{StopBudgetPhaseId("phase.nominal.command"), 1'000'000'000LL},
            {StopBudgetPhaseId("phase.nominal.rest"), 2'000'000'000LL},
            {StopBudgetPhaseId("phase.hard.command"), 3'000'000'000LL},
            {StopBudgetPhaseId("phase.hard.rest"), 4'000'000'000LL}}},
          {NominalStage(), HardStage()}};
}

[[nodiscard]] ResolvedStopBasePlanDTO Plan() {
  auto made = MakeResolvedStopBasePlan(Candidate());
  EXPECT_TRUE(made.ok()) << (made.ok() ? "" : made.error().details.at("reason"));
  return made.value();
}

[[nodiscard]] ResolvedStopBasePlanDTO PreparedBasePlan() {
  auto candidate = Candidate();
  auto& nominal = candidate.escalation_stages.front();
  nominal.containment_command.provider_contract_ref = MotionContract();
  nominal.command_qualification.controlled_substitution_contract->provider_contract_ref =
      MotionContract();
  JsonValueDTO::Array limits;
  for (const auto& resource : {std::string("joint.1"), std::string("joint.2")}) {
    limits.emplace_back(JsonObjectDTO{
        {"resource_id", JsonValueDTO{resource}},
        {"max_deceleration", JsonValueDTO{JsonObjectDTO{
                                 {"coordinate_kind", JsonValueDTO{std::string("angular_rad_s2")}},
                                 {"value_rad_s2", JsonValueDTO{2.0}}}}},
        {"settle_velocity_threshold",
         JsonValueDTO{JsonObjectDTO{{"coordinate_kind", JsonValueDTO{std::string("angular_rad_s")}},
                                    {"value_rad_s", JsonValueDTO{0.02}}}}},
    });
  }
  nominal.default_contract_parameters = JsonObjectDTO{
      {"stop_mode", JsonValueDTO{std::string("soft_on_path")}},
      {"jerk_limits_enabled", JsonValueDTO{false}},
      {"deceleration_time_s", JsonValueDTO{0.5}},
      {"per_resource_limits", JsonValueDTO{std::move(limits)}},
      {"allow_natural_completion_on_short_path", JsonValueDTO{true}},
      {"recoverable_substitution_mode", JsonValueDTO{std::string("spring_back_hold")}},
  };
  auto made = MakeResolvedStopBasePlan(std::move(candidate));
  EXPECT_TRUE(made.ok()) << (made.ok() ? "" : made.error().details.at("reason"));
  return made.value();
}

[[nodiscard]] std::string GoldenBytes() {
  const auto path =
      std::filesystem::path(__FILE__).parent_path() / "golden" / "resolved_stop_base_plan.v1.json";
  std::ifstream stream(path, std::ios::binary);
  EXPECT_TRUE(stream.is_open()) << path;
  std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  EXPECT_TRUE(bytes.ends_with('\n')) << path;
  if (bytes.ends_with('\n')) {
    bytes.pop_back();
  }
  return bytes;
}

[[nodiscard]] std::string ReplaceOnce(std::string input, const std::string_view before,
                                      const std::string_view after) {
  const auto position = input.find(before);
  EXPECT_NE(position, std::string::npos);
  if (position != std::string::npos) {
    input.replace(position, before.size(), after);
  }
  return input;
}

[[nodiscard]] content_identity::CanonicalValue WithCanonicalMember(
    const content_identity::CanonicalValue& object, const std::string_view name,
    content_identity::CanonicalValue replacement) {
  auto fields = object.as_object();
  const auto found = std::find_if(fields.begin(), fields.end(),
                                  [&](const auto& row) { return row.first == name; });
  EXPECT_NE(found, fields.end());
  if (found != fields.end()) {
    found->second = std::move(replacement);
  }
  return content_identity::CanonicalValue(std::move(fields));
}

TEST(ResolvedStopBasePlanCodec, EmitsStableCanonicalBytesAndOwnIdentity) {
  const auto plan = Plan();
  ASSERT_TRUE(VerifyResolvedStopBasePlan(plan).ok());
  const auto encoded = EncodeResolvedStopBasePlan(plan);
  ASSERT_TRUE(encoded.ok());
  EXPECT_EQ(GoldenBytes(), encoded.value());
  EXPECT_TRUE(encoded.value().starts_with(
      "{\"activation_set_id\":\"activation.main\",\"active_control_configuration_generation\":7"));
  EXPECT_NE(encoded.value().find("\"api_version\":\"manyforge.resolved_stop_base_plan.v1\""),
            std::string::npos);
  EXPECT_NE(encoded.value().find("\"resolved_stop_base_plan_id\":\"" +
                                 plan.resolved_stop_base_plan_id.value() + "\""),
            std::string::npos);

  // These two constants are the cross-language normalized-byte/ID golden.
  EXPECT_EQ(plan.resolved_stop_base_plan_id.value(),
            "sha256:ba4b37bfaa312d688ea229a4ba4d516fd2a519c3a9dc82e43e275366769a367a");
  EXPECT_EQ(content_identity::sha256_digest(encoded.value()),
            "sha256:6580fa0212890c5f6714fffc367c7f2488e893e016acae5e7e68993458b0e819");
}

TEST(ResolvedStopBasePlanCodec, OwnIdIsOmittedAndEverySemanticJoinRotatesIt) {
  auto different_placeholder = Candidate();
  different_placeholder.resolved_stop_base_plan_id = ResolvedStopBasePlanId(Digest('0'));
  auto remade = MakeResolvedStopBasePlan(std::move(different_placeholder));
  ASSERT_TRUE(remade.ok());
  EXPECT_EQ(remade.value().resolved_stop_base_plan_id, Plan().resolved_stop_base_plan_id);

  auto changed = Candidate();
  changed.escalation_stages.front().command_target.runtime_unit_epoch =
      RuntimeEpoch("runtime.epoch.2");
  auto changed_plan = MakeResolvedStopBasePlan(std::move(changed));
  ASSERT_TRUE(changed_plan.ok());
  EXPECT_NE(changed_plan.value().resolved_stop_base_plan_id, Plan().resolved_stop_base_plan_id);

  auto forged = Plan();
  forged.resolved_stop_base_plan_id = ResolvedStopBasePlanId(Digest('0'));
  EXPECT_FALSE(VerifyResolvedStopBasePlan(forged).ok());
  EXPECT_FALSE(EncodeResolvedStopBasePlan(forged).ok());
}

TEST(ResolvedStopBasePlanCodec, DecodesOnlyTheExactCanonicalClosedWireObject) {
  const auto plan = Plan();
  const auto encoded = EncodeResolvedStopBasePlan(plan);
  ASSERT_TRUE(encoded.ok());

  const auto decoded = DecodeResolvedStopBasePlan(encoded.value());
  ASSERT_TRUE(decoded.ok()) << (decoded.ok() ? "" : decoded.error().details.at("reason"));
  EXPECT_EQ(decoded.value(), plan);

  EXPECT_FALSE(DecodeResolvedStopBasePlan(" " + encoded.value()).ok());
  EXPECT_FALSE(DecodeResolvedStopBasePlan(ReplaceOnce(encoded.value(), "{\"activation_set_id\"",
                                                      "{\"a_unknown\":0,\"activation_set_id\""))
                   .ok());
  EXPECT_FALSE(DecodeResolvedStopBasePlan(
                   ReplaceOnce(encoded.value(),
                               "\"default_contract_parameters\":{\"stop_mode\":\"soft_on_path\"}",
                               "\"default_contract_parameters\":null"))
                   .ok());
  EXPECT_FALSE(
      DecodeResolvedStopBasePlan(ReplaceOnce(encoded.value(), "\"source_sequence\":11,", "")).ok());
  EXPECT_FALSE(DecodeResolvedStopBasePlan(ReplaceOnce(encoded.value(), "\"source_sequence\":11,",
                                                      "\"source_sequence\":11.5,"))
                   .ok());
  EXPECT_FALSE(DecodeResolvedStopBasePlan(
                   ReplaceOnce(encoded.value(), "\"resolved_stop_base_plan_id\":\"sha256:ba4b",
                               "\"resolved_stop_base_plan_id\":\"sha256:0925"))
                   .ok());
  EXPECT_FALSE(
      DecodeResolvedStopBasePlan(ReplaceOnce(encoded.value(),
                                             "\"endpoint_role_id\":\"command.stop\","
                                             "\"logical_endpoint_symbol\":\"command.stop\"",
                                             "\"endpoint_role_id\":\"command.stop\","
                                             "\"logical_endpoint_symbol\":null"))
          .ok());
  EXPECT_FALSE(DecodeResolvedStopBasePlan(
                   ReplaceOnce(encoded.value(), "\"containment_granularity\":\"resource\"",
                               "\"containment_granularity\":\"controller_set\""))
                   .ok());
  EXPECT_FALSE(DecodeResolvedStopBasePlan(ReplaceOnce(encoded.value(),
                                                      "\"stop_action_class\":\"controlled\"",
                                                      "\"stop_action_class\":\"software_hard\""))
                   .ok());
  EXPECT_FALSE(DecodeResolvedStopBasePlan(ReplaceOnce(encoded.value(),
                                                      "\"source_kind\":\"binding_state_authority\"",
                                                      "\"source_kind\":\"provider_evidence\""))
                   .ok());
}

TEST(ResolvedStopBasePlanCodec, RejectsOversizedWireArraysBeforeDtoMaterialization) {
  const auto encoded = EncodeResolvedStopBasePlan(Plan());
  ASSERT_TRUE(encoded.ok());
  const auto root = content_identity::parse_canonical_json(encoded.value());
  const auto* stages_member = root.find_object_member("escalation_stages");
  ASSERT_NE(stages_member, nullptr);

  auto too_many_stages = stages_member->as_array();
  while (too_many_stages.size() <= 8U) {
    too_many_stages.push_back(too_many_stages.front());
  }
  auto result = DecodeResolvedStopBasePlan(content_identity::canonical_json(WithCanonicalMember(
      root, "escalation_stages", content_identity::CanonicalValue(std::move(too_many_stages)))));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "escalation_stages is outside its array bound");

  auto stages = stages_member->as_array();
  const auto* evidence_member = stages.front().find_object_member("evidence_requirements");
  ASSERT_NE(evidence_member, nullptr);
  auto too_much_evidence = evidence_member->as_array();
  while (too_much_evidence.size() <= 64U) {
    too_much_evidence.push_back(too_much_evidence.front());
  }
  stages.front() =
      WithCanonicalMember(stages.front(), "evidence_requirements",
                          content_identity::CanonicalValue(std::move(too_much_evidence)));
  result = DecodeResolvedStopBasePlan(content_identity::canonical_json(WithCanonicalMember(
      root, "escalation_stages", content_identity::CanonicalValue(std::move(stages)))));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "evidence_requirements is outside its array bound");

  stages = stages_member->as_array();
  const auto* scope_member = stages.front().find_object_member("containment_scope");
  ASSERT_NE(scope_member, nullptr);
  content_identity::CanonicalValue::Array too_many_resources;
  too_many_resources.reserve(4097U);
  for (std::size_t index = 0U; index < 4097U; ++index) {
    too_many_resources.emplace_back("resource." + std::to_string(index));
  }
  stages.front() = WithCanonicalMember(
      stages.front(), "containment_scope",
      WithCanonicalMember(*scope_member, "resource_ids",
                          content_identity::CanonicalValue(std::move(too_many_resources))));
  result = DecodeResolvedStopBasePlan(content_identity::canonical_json(WithCanonicalMember(
      root, "escalation_stages", content_identity::CanonicalValue(std::move(stages)))));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "resource_ids is outside its array bound");
}

TEST(ResolvedStopBasePlanCodec, RejectsConditionalEndpointAndOverlayFields) {
  auto focused_target = Candidate();
  focused_target.escalation_stages.front().command_target.endpoint_role_id.reset();
  focused_target.escalation_stages.front().command_target.logical_endpoint_symbol.reset();
  EXPECT_TRUE(MakeResolvedStopBasePlan(std::move(focused_target)).ok());

  auto partial_endpoint = Candidate();
  partial_endpoint.escalation_stages.front().command_target.logical_endpoint_symbol.reset();
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(partial_endpoint)).ok());

  auto partial_overlay = Candidate();
  auto& target = partial_overlay.escalation_stages.front().command_target;
  target.external_resource_alias = ExternalResourceAlias("site.robot");
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(partial_overlay)).ok());

  auto complete_overlay = Candidate();
  auto& complete = complete_overlay.escalation_stages.front().command_target;
  complete.external_resource_alias = ExternalResourceAlias("site.robot");
  complete.external_resource_kind = ExternalResourceKind::kServiceEndpoint;
  complete.site_overlay_id = SiteOverlayId("site.main");
  complete.site_overlay_revision = SiteOverlayRevision(Digest('1'));
  complete.site_overlay_non_secret_digest = ContentDigest(Digest('2'));
  complete.site_overlay_value_ref = SiteOverlayValueRef("robot.service");
  EXPECT_TRUE(MakeResolvedStopBasePlan(std::move(complete_overlay)).ok());
}

TEST(ResolvedStopBasePlanCodec, RejectsSourceAndQualificationConfusion) {
  auto canonical_authorities = Candidate();
  canonical_authorities.escalation_stages.front()
      .evidence_requirements.front()
      .requirement.source_requirement.allowed_authority_classes = {StateAuthorityClass::kCommanded,
                                                                   StateAuthorityClass::kMeasured};
  EXPECT_TRUE(MakeResolvedStopBasePlan(std::move(canonical_authorities)).ok());

  auto reversed_authorities = Candidate();
  reversed_authorities.escalation_stages.front()
      .evidence_requirements.front()
      .requirement.source_requirement.allowed_authority_classes = {StateAuthorityClass::kMeasured,
                                                                   StateAuthorityClass::kCommanded};
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(reversed_authorities)).ok());

  auto wrong_source = Candidate();
  wrong_source.escalation_stages.front().evidence_requirements.front().resolved_source.source_kind =
      StopEvidenceSourceKind::kProviderEvidence;
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(wrong_source)).ok());

  auto wrong_authority = Candidate();
  wrong_authority.escalation_stages.front()
      .evidence_requirements.front()
      .resolved_source.state_authority_class = StateAuthorityClass::kPredicted;
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(wrong_authority)).ok());

  auto false_controlled_floor = Candidate();
  false_controlled_floor.escalation_stages.back().command_qualification = {
      StopActionClass::kControlled,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(false_controlled_floor)).ok());

  auto provider_evidence = Candidate();
  provider_evidence.escalation_stages.front().evidence_requirements = {
      ResolvedProviderEvidence("evidence.nominal.provider", "phase.nominal.rest")};
  EXPECT_TRUE(MakeResolvedStopBasePlan(std::move(provider_evidence)).ok());

  auto wrong_provider_contract = Candidate();
  wrong_provider_contract.escalation_stages.front().evidence_requirements = {
      ResolvedProviderEvidence("evidence.nominal.provider", "phase.nominal.rest")};
  wrong_provider_contract.escalation_stages.front()
      .evidence_requirements.front()
      .resolved_source.provider_contract_ref = ProviderContractRefDTO{
      ProviderContractRoleId("stop.other"), SchemaId("manyforge.value_schema.stop.command.v1"),
      SchemaRevision(Digest('1'))};
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(wrong_provider_contract)).ok());
}

TEST(ResolvedStopBasePlanCodec, AcceptsOnlyCompleteFinalCommissionedSafetyStage) {
  auto commissioned = Candidate();
  commissioned.deadline_budget.phases = {
      {StopBudgetPhaseId("phase.nominal.command"), 750'000'000LL},
      {StopBudgetPhaseId("phase.nominal.rest"), 1'500'000'000LL},
      {StopBudgetPhaseId("phase.hard.command"), 2'250'000'000LL},
      {StopBudgetPhaseId("phase.hard.rest"), 3'000'000'000LL},
      {StopBudgetPhaseId("phase.safety.command"), 3'750'000'000LL},
      {StopBudgetPhaseId("phase.safety.rest"), 4'500'000'000LL}};
  commissioned.escalation_stages.push_back(SafetyStage());
  EXPECT_TRUE(MakeResolvedStopBasePlan(commissioned).ok());

  auto missing_commissioning = commissioned;
  missing_commissioning.escalation_stages.back()
      .command_qualification.commissioning_evidence_artifact_ref.reset();
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(missing_commissioning)).ok());

  auto wrong_action = commissioned;
  wrong_action.escalation_stages.back().command_qualification.safety_system_request_action_class =
      SafetySystemRequestActionClass::kEmergencyStopRequest;
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(wrong_action)).ok());

  auto not_final = commissioned;
  auto trailing = HardStage();
  trailing.stage_index = 3U;
  trailing.stop_escalation_stage_id = StopEscalationStageId("stage.trailing");
  not_final.escalation_stages.push_back(std::move(trailing));
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(not_final)).ok());
}

TEST(ResolvedStopBasePlanCodec, RejectsOrderNarrowingAndPhaseGaps) {
  auto wrong_index = Candidate();
  wrong_index.escalation_stages.back().stage_index = 0U;
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(wrong_index)).ok());

  auto no_hard_floor = Candidate();
  no_hard_floor.escalation_stages.pop_back();
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(no_hard_floor)).ok());

  auto narrow = Candidate();
  narrow.escalation_stages.front().containment_granularity = ContainmentGranularity::kControllerSet;
  narrow.escalation_stages.front().containment_scope = ResolvedStopControllerSetContainmentScopeDTO{
      ActivationSetId("activation.main"),
      {{ControllerRoleId("controller.arm"), "controller.arm"},
       {ControllerRoleId("controller.tool"), "controller.tool"}}};
  narrow.escalation_stages.back().containment_scope = ResolvedStopControllerSetContainmentScopeDTO{
      ActivationSetId("activation.main"), {{ControllerRoleId("controller.arm"), "controller.arm"}}};
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(narrow)).ok());

  auto repeated_controller_role = Candidate();
  auto& controller_scope = std::get<ResolvedStopControllerSetContainmentScopeDTO>(
      repeated_controller_role.escalation_stages.back().containment_scope);
  controller_scope.controllers.push_back({ControllerRoleId("controller.arm"), "controller.second"});
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(repeated_controller_role)).ok());

  auto unordered_resources = Candidate();
  auto& resources = std::get<ResolvedStopResourceContainmentScopeDTO>(
      unordered_resources.escalation_stages.front().containment_scope);
  std::reverse(resources.resource_ids.begin(), resources.resource_ids.end());
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(unordered_resources)).ok());

  auto foreign_domain = Candidate();
  foreign_domain.escalation_stages.back().containment_granularity =
      ContainmentGranularity::kControlDomain;
  foreign_domain.escalation_stages.back().containment_scope =
      ResolvedStopControlDomainContainmentScopeDTO{ActivationSetId("activation.main"),
                                                   ControlDomainId("domain.other"),
                                                   ControlDomainRevision(Digest('c'))};
  foreign_domain.containment_granularity = ContainmentGranularity::kControlDomain;
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(foreign_domain)).ok());

  auto unused_phase = Candidate();
  unused_phase.deadline_budget.phases.push_back(
      {StopBudgetPhaseId("phase.unused"), 4'250'000'000LL});
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(unused_phase)).ok());
}

TEST(ResolvedStopBasePlanCodec, RejectsUnsafeAndNonCanonicalValues) {
  auto unsafe_sequence = Candidate();
  unsafe_sequence.source_sequence = static_cast<std::uint64_t>(9'007'199'254'740'992ULL);
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(unsafe_sequence)).ok());

  auto nonfinite = Candidate();
  nonfinite.escalation_stages.front().default_contract_parameters->at("stop_mode") =
      JsonValueDTO{std::numeric_limits<double>::infinity()};
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(nonfinite)).ok());

  auto wrong_store_key = Candidate();
  wrong_store_key.source_template.artifact_ref.content_addressed_store_key =
      ContentAddressedStoreKey("sha256/not-the-digest");
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(wrong_store_key)).ok());

  auto control_stage_claims_hard_command = Candidate();
  control_stage_claims_hard_command.escalation_stages.front().containment_command = {
      StopBudgetPhaseId("phase.nominal.command"), StopContainmentCommandKind::kRegistered,
      Contract(), RegisteredStopCommandKindId("manyforge.stop.software_hard")};
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(control_stage_claims_hard_command)).ok());

  auto exceeds_parser_ceiling = Candidate();
  JsonValueDTO::Array large_parameters;
  large_parameters.reserve(256U);
  for (std::size_t index = 0U; index < 256U; ++index) {
    large_parameters.emplace_back();
    large_parameters.back().value = std::string(65'536U, 'x');
  }
  exceeds_parser_ceiling.escalation_stages.front().default_contract_parameters->emplace(
      "large", JsonValueDTO{std::move(large_parameters)});
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(exceeds_parser_ceiling)).ok());

  auto exceeds_document_depth = Candidate();
  JsonValueDTO nested;
  nested.value = std::string("leaf");
  for (std::size_t depth = 0U; depth < 30U; ++depth) {
    JsonObjectDTO parent;
    parent.emplace("nested", std::move(nested));
    nested.value = std::move(parent);
  }
  exceeds_document_depth.escalation_stages.front().default_contract_parameters->emplace(
      "deep", std::move(nested));
  EXPECT_FALSE(MakeResolvedStopBasePlan(std::move(exceeds_document_depth)).ok());
}

TEST(PreparedParticipantStopPlanCodec, FreezesProfileMappingAndDeploymentDefaultsBeforeDispatch) {
  const auto base = PreparedBasePlan();
  const auto resources = Resources();
  const auto defaults = DefaultLimits();
  const auto made = MakePreparedParticipantStopPlan(base, PreparedContext(), resources, defaults);

  ASSERT_TRUE(made.ok()) << (made.ok() ? "" : made.error().details.at("reason"));
  const auto& plan = made.value();
  EXPECT_EQ(plan.resolved_stop_base_plan_id, base.resolved_stop_base_plan_id);
  ASSERT_EQ(plan.stage_commands.size(), base.escalation_stages.size());
  EXPECT_EQ(plan.stage_commands[0].resolved_stage, base.escalation_stages[0]);
  EXPECT_EQ(plan.stage_commands[0].parameter_source,
            StopStageParameterSourceDTO::kParticipantProfileMapping);
  EXPECT_EQ(plan.stage_commands[1].parameter_source,
            StopStageParameterSourceDTO::kDeploymentDefault);
  EXPECT_EQ(plan.stage_commands[1].validated_contract_parameters,
            base.escalation_stages[1].default_contract_parameters);
  ASSERT_TRUE(VerifyPreparedParticipantStopPlan(base, plan, resources, defaults,
                                                MotionInterruptNodeCategory::kGeneric)
                  .ok());
  const auto encoded = EncodePreparedParticipantStopPlan(base, plan, resources, defaults,
                                                         MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(encoded.ok());
  const auto decoded = DecodePreparedParticipantStopPlan(encoded.value(), base, resources, defaults,
                                                         MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), plan);
  auto invalid_unit = encoded.value();
  const auto unit_offset = invalid_unit.find("angular_rad_s2");
  ASSERT_NE(unit_offset, std::string::npos);
  invalid_unit.replace(unit_offset, std::string("angular_rad_s2").size(), "angular_bad_s2");
  EXPECT_FALSE(DecodePreparedParticipantStopPlan(invalid_unit, base, resources, defaults,
                                                 MotionInterruptNodeCategory::kGeneric)
                   .ok());
  EXPECT_NE(encoded.value().find("\"parameter_source\":\"participant_profile_mapping\""),
            std::string::npos);
  EXPECT_NE(encoded.value().find("\"parameter_source\":\"deployment_default\""), std::string::npos);
  EXPECT_EQ(content_identity::sha256_digest(encoded.value()),
            "sha256:b52156d4da6ecf586177c0533011329c2ec17e498973a93187b3137530a2b903");
  EXPECT_EQ(plan.participant_stop_invocation_plan_digest.value(),
            "sha256:960198ca5da9b7d114858f1b2bc18cfa6bac701db4aa4086df45808a4767029f");
}

TEST(PreparedParticipantStopPlanCodec, KeepsOrdinaryAndScopePlansDistinctAndRejectsForgery) {
  const auto base = PreparedBasePlan();
  const auto resources = Resources();
  const auto defaults = DefaultLimits();
  auto profiled = MakePreparedParticipantStopPlan(base, PreparedContext(), resources, defaults);
  ASSERT_TRUE(profiled.ok());

  auto ordinary_context = PreparedContext();
  ordinary_context.participant_stop_profile.reset();
  ordinary_context.participant_stop_profile_purpose = ParticipantStopProfilePurposeDTO::kOrdinary;
  ordinary_context.motion_interrupt_scope_id.reset();
  auto ordinary =
      MakePreparedParticipantStopPlan(base, std::move(ordinary_context), resources, defaults);
  ASSERT_TRUE(ordinary.ok());
  EXPECT_NE(ordinary.value().participant_stop_invocation_plan_digest,
            profiled.value().participant_stop_invocation_plan_digest);
  EXPECT_TRUE(std::all_of(ordinary.value().stage_commands.begin(),
                          ordinary.value().stage_commands.end(), [](const auto& stage) {
                            return !stage.parameter_source.has_value() ||
                                   stage.parameter_source ==
                                       StopStageParameterSourceDTO::kDeploymentDefault;
                          }));

  auto other_scope = PreparedContext();
  other_scope.motion_interrupt_scope_id = "scope.other";
  auto other = MakePreparedParticipantStopPlan(base, std::move(other_scope), resources, defaults);
  ASSERT_TRUE(other.ok());
  EXPECT_NE(other.value().participant_stop_invocation_plan_digest,
            profiled.value().participant_stop_invocation_plan_digest);

  auto forged = profiled.value();
  forged.stage_commands.front().validated_contract_parameters->at("deceleration_time_s") =
      JsonValueDTO{0.5};
  EXPECT_FALSE(VerifyPreparedParticipantStopPlan(base, forged, resources, defaults,
                                                 MotionInterruptNodeCategory::kGeneric)
                   .ok());
  forged = profiled.value();
  forged.participant_stop_invocation_plan_digest = ContentDigest(Digest('0'));
  EXPECT_FALSE(VerifyPreparedParticipantStopPlan(base, forged, resources, defaults,
                                                 MotionInterruptNodeCategory::kGeneric)
                   .ok());
}

// P3 / item 24: no Prepare-time Stop budget pre-check. A profile longer than
// the saved timeout is prepared; the Stop deadline watchdog bounds it at run
// time (as `main`).
TEST(PreparedParticipantStopPlanCodec, ReadinessCeilingDoesNotRefuseALongerProfileAtPrepare) {
  const auto base = PreparedBasePlan();
  // This pure codec check uses the DTO projection. The deployment/registration
  // tests separately seal the complete current controller-domain referent.
  RuntimeParticipantStopReadinessDTO readiness{
      ControllableComponentId("arm"),
      ComponentLifecycleGeneration(1U),
      {base.control_domain_id,
       base.control_domain_revision,
       base.controller_topology_id,
       base.topology_revision,
       ProductId("product"),
       ProductContractId("contract"),
       ProductContractRevision(Digest('1')),
       ContentDigest(Digest('2')),
       ContentDigest(Digest('3')),
       base.source_runtime_unit_id,
       base.source_runtime_epoch,
       {base.controller_topology_id, base.topology_revision, {}, {}, {}, {}},
       {base.control_domain_id,
        base.control_domain_revision,
        base.controller_topology_id,
        base.topology_revision,
        base.activation_set_id,
        {}},
       {},
       {},
       {}},
      base,
      250'000'000};
  auto context = PreparedContext();
  const auto resources = Resources();
  const auto defaults = DefaultLimits();
  EXPECT_TRUE(MakePreparedParticipantStopPlan(readiness, context, resources, defaults).ok());
  context.participant_stop_profile->deceleration_time_s = 0.5;
  for (const auto mode :
       {MotionInterruptStopMode::kSoftOnPath, MotionInterruptStopMode::kSpringBackHold}) {
    context.participant_stop_profile->stop_mode = mode;
    if (mode == MotionInterruptStopMode::kSpringBackHold) {
      context.participant_stop_profile->allow_natural_completion_on_short_path = false;
      context.participant_stop_profile->recoverable_substitution_mode.reset();
    }
    const auto prebuilt = MakePreparedParticipantStopPlan(readiness.resolved_stop_base_plan,
                                                          context, resources, defaults);
    ASSERT_TRUE(prebuilt.ok()) << prebuilt.error().message;
    EXPECT_TRUE(MakePreparedParticipantStopPlan(readiness, context, resources, defaults).ok());
    EXPECT_TRUE(VerifyPreparedParticipantStopPlan(readiness, prebuilt.value(), resources, defaults,
                                                  context.node_category)
                    .ok());
    // Historic bytes remain inspectable, not preparation authority.
    EXPECT_TRUE(EncodePreparedParticipantStopPlan(readiness.resolved_stop_base_plan,
                                                  prebuilt.value(), resources, defaults,
                                                  context.node_category)
                    .ok());
  }
  context.participant_stop_profile.reset();
  context.participant_stop_profile_purpose = ParticipantStopProfilePurposeDTO::kOrdinary;
  context.motion_interrupt_scope_id.reset();
  EXPECT_TRUE(MakePreparedParticipantStopPlan(readiness, context, resources, defaults).ok());
  readiness.maximum_controlled_stop_duration_ns.reset();
  EXPECT_TRUE(MakePreparedParticipantStopPlan(readiness, context, resources, defaults).ok());

  context = PreparedContext();
  context.participant_stop_profile->deceleration_time_s = 1.1;
  readiness.maximum_controlled_stop_duration_ns = 1'100'000'000;
  EXPECT_TRUE(MakePreparedParticipantStopPlan(readiness, context, resources, defaults).ok());
  readiness.maximum_controlled_stop_duration_ns = 1'099'999'999;
  EXPECT_TRUE(MakePreparedParticipantStopPlan(readiness, context, resources, defaults).ok());
  // Only the ceiling's own shape is still checked.
  readiness.maximum_controlled_stop_duration_ns = 0;
  EXPECT_FALSE(MakePreparedParticipantStopPlan(readiness, context, resources, defaults).ok());
}

TEST(PreparedParticipantStopPlanCodec, FreezesJerkPolicyInProfileContractAndIdentity) {
  const auto base = PreparedBasePlan();
  const auto resources = Resources();
  const auto defaults = DefaultLimits();
  auto context = PreparedContext();
  context.participant_stop_profile->jerk_limits_enabled = true;
  const auto on = MakePreparedParticipantStopPlan(base, context, resources, defaults);
  ASSERT_TRUE(on.ok()) << on.error().message;
  // Q36: only authored jerk is frozen; the default is derived per Stop phase.
  EXPECT_TRUE(on.value().participant_stop_profile->per_joint_max_jerk.empty());
  const auto encoded = EncodePreparedParticipantStopPlan(base, on.value(), resources, defaults,
                                                         MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(encoded.ok());
  const auto decoded = DecodePreparedParticipantStopPlan(encoded.value(), base, resources, defaults,
                                                         MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), on.value());
  context.participant_stop_profile->jerk_limits_enabled = false;
  const auto off = MakePreparedParticipantStopPlan(base, context, resources, defaults);
  ASSERT_TRUE(off.ok());
  EXPECT_TRUE(off.value().participant_stop_profile->per_joint_max_jerk.empty());
  EXPECT_NE(on.value().participant_stop_invocation_plan_digest,
            off.value().participant_stop_invocation_plan_digest);
}

TEST(PreparedParticipantStopPlanCodec, DeploymentDefaultFreezesTheJerkChoiceButNoDefaultJerk) {
  auto candidate = PreparedBasePlan();
  candidate.escalation_stages.front().default_contract_parameters->at("jerk_limits_enabled") =
      JsonValueDTO{true};
  const auto base = MakeResolvedStopBasePlan(candidate);
  ASSERT_TRUE(base.ok());
  auto context = PreparedContext();
  context.participant_stop_profile.reset();
  context.participant_stop_profile_purpose = ParticipantStopProfilePurposeDTO::kOrdinary;
  context.motion_interrupt_scope_id.reset();
  const auto prepared =
      MakePreparedParticipantStopPlan(base.value(), context, Resources(), DefaultLimits());
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  const auto& stage = prepared.value().stage_commands.front();
  EXPECT_EQ(stage.parameter_source, StopStageParameterSourceDTO::kDeploymentDefault);
  const auto decoded = map_motion_controlled_stop_contract_parameters_to_profile(
      *stage.validated_contract_parameters, Resources(), "default.profile");
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value().jerk_limits_enabled, true);
  // Q36: no default jerk is frozen before dispatch; the planner derives it
  // per Stop phase from the lane's lead-in (main's formula).
  EXPECT_TRUE(decoded.value().per_joint_max_jerk.empty());
  candidate.escalation_stages.front().default_contract_parameters->erase("jerk_limits_enabled");
  const auto legacy = MakeResolvedStopBasePlan(candidate);
  ASSERT_TRUE(legacy.ok());
  EXPECT_FALSE(
      MakePreparedParticipantStopPlan(legacy.value(), context, Resources(), DefaultLimits()).ok());
}

TEST(PreparedParticipantStopPlanCodec, RejectsPurposeCoverageAndSubstitutionConfusion) {
  const auto base = PreparedBasePlan();
  const auto resources = Resources();
  const auto defaults = DefaultLimits();

  auto missing_scope = PreparedContext();
  missing_scope.motion_interrupt_scope_id.reset();
  EXPECT_FALSE(
      MakePreparedParticipantStopPlan(base, std::move(missing_scope), resources, defaults).ok());

  auto incomplete = Resources();
  incomplete.pop_back();
  EXPECT_FALSE(MakePreparedParticipantStopPlan(base, PreparedContext(), incomplete, defaults).ok());

  auto unqualified_base = PreparedBasePlan();
  unqualified_base.escalation_stages.front()
      .command_qualification.controlled_substitution_contract.reset();
  auto made_unqualified = MakeResolvedStopBasePlan(std::move(unqualified_base));
  ASSERT_TRUE(made_unqualified.ok());
  EXPECT_FALSE(MakePreparedParticipantStopPlan(made_unqualified.value(), PreparedContext(),
                                               resources, defaults)
                   .ok());

  auto precision = PreparedContext();
  precision.node_category = MotionInterruptNodeCategory::kPrecisionContact;
  EXPECT_FALSE(
      MakePreparedParticipantStopPlan(base, std::move(precision), resources, defaults).ok());
}

}  // namespace
}  // namespace manyforge::core
