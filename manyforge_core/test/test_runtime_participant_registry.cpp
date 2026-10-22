#include <gtest/gtest.h>

#include <manyforge_core/io_output_participant_engine.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/runtime_participant_registry.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace manyforge::core;

[[nodiscard]] RuntimeParticipantBinding binding_for(
    std::string target_role, std::string command_target, std::string capability,
    CoordinateKind kind, std::optional<ProgramCommandSchemaRefDTO> command_schema = std::nullopt) {
  RuntimeParticipantBindingDTO value{
      ProgramId("program.pick"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      RetainedBundleId("bundle"),
      GraphRevision("graph"),
      ComponentRoleId("robot"),
      TargetRoleId(std::move(target_role)),
      ControllableComponentId("robot.1"),
      ComponentTypeId("aggregate.robot"),
      ComponentTypeRevision("aggregate-revision"),
      CommandTargetId(std::move(command_target)),
      CapabilityId(std::move(capability)),
      CapabilityDefinitionRevision("capability-revision"),
      std::move(command_schema),
      {ProgramModelRoleBindingDTO{ModelRoleId("body"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision")}},
      {RuntimeModelGroupBindingDTO{
          ModelGroupRoleId("command_group"),
          ModelInstanceId("robot.model"),
          "mf2_robot_group",
          {kind == CoordinateKind::kAngular ? "mf2_robot_joint" : "mf2_gripper_joint"}}},
      {ProgramFrameRoleBindingDTO{FrameRoleId("base"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "mf2_robot_base"},
       ProgramFrameRoleBindingDTO{FrameRoleId("tcp"), ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision"), "mf2_robot_tcp"}},
      {RuntimeResourceBindingDTO{
          ResourceRoleId(kind == CoordinateKind::kAngular ? "joint" : "finger"),
          ResourceId(kind == CoordinateKind::kAngular ? "robot.joint" : "robot.finger"),
          kind == CoordinateKind::kAngular ? "joint" : "finger_joint",
          kind == CoordinateKind::kAngular ? "mf2_robot_joint" : "mf2_gripper_joint", kind}},
      {},
      RuntimeControlPathDTO{
          ControlDomainId(kind == CoordinateKind::kAngular ? "domain.arm" : "domain.gripper"),
          ControlDomainRevision("domain-revision"),
          ControlBindingId(kind == CoordinateKind::kAngular ? "binding.arm" : "binding.gripper"),
          ControlBindingRevision("binding-revision"),
          ControlInterfaceContractId("interface"),
          ControlInterfaceRevision("interface-revision"),
          ControllerTopologyId("topology.native"),
          TopologyRevision("topology-revision"),
          ControlRouteId("route.native"),
          ActivationSetId("activation.native"),
          ActiveControlConfigurationId("active.native"),
          ActiveControlConfigurationGeneration(1U),
          ProviderInstanceId("provider.native"),
          ProviderManifestId("provider-manifest.native"),
          ProviderManifestRevision("provider-manifest-revision"),
          RuntimeUnitId("runtime.native"),
          RuntimeEpoch("runtime-epoch.1"),
          StateAuthorityId("state.native"),
          StateAuthorityClass::kPredicted,
          StopEvidencePolicyId("stop.native"),
          StopEvidencePolicyRevision("stop-revision"),
          ContainmentGranularity::kResource,
          std::nullopt},
      std::nullopt,
      {},
      {RuntimeUnitEpochBindingDTO{RuntimeUnitId("runtime.native"),
                                  RuntimeEpoch("runtime-epoch.1")}}};
  value.motion_endpoint = RuntimeMotionEndpointBindingDTO{
      value.target_role_id, value.command_target_id, value.frame_bindings.back(), {}};
  auto sealed = verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] MotionParticipantIntentDTO motion_payload(std::string owner = "arm") {
  return MotionParticipantIntentDTO{
      MoveId("move.1"),
      FrameRoleId("base"),
      MotionEndpointReferenceDTO{TargetRoleId(std::move(owner)), FrameRoleId("tcp")},
      TolerancesDTO{0.01, 0.001, 0.01},
      SpeedProfileDTO{},
      TimeoutsDTO{},
      CacheMode::kDefault,
      PlanningProfileDTO::kBalanced,
      MotionTargetModeDTO::kAbsolute,
      std::nullopt,
      std::nullopt,
      JointGoalDTO{{"joint"}, {0.2}},
      std::nullopt,
      {},
      {}};
}

[[nodiscard]] OperationIntentDTO operation_for(std::string target_role, std::string capability,
                                               OperationParticipantPayloadDTO payload) {
  return OperationIntentDTO{
      PublicRequestContextDTO{RequestId("request.1"), RequestOwnerEpoch("owner.epoch.1"), "test",
                              std::nullopt},
      ProgramId("program.pick"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      ProgramRunId("run.1"),
      GraphRevision("graph"),
      RetainedBundleId("bundle"),
      std::nullopt,
      {OperationParticipantIntentDTO{ComponentRoleId("robot"), TargetRoleId(std::move(target_role)),
                                     CapabilityId(std::move(capability)), std::nullopt,
                                     std::move(payload)}},
      OperationCoordinationPolicy::kIndependent};
}

TEST(RuntimeParticipantRegistryTest, ResolvesExactAddressWithoutRobotOrGroupFallback) {
  RuntimeParticipantRegistry registry(
      {binding_for("arm", "arm", "planning.motion.joint_goal", CoordinateKind::kAngular)});
  const auto operation = operation_for("arm", "planning.motion.joint_goal", motion_payload());

  auto resolved = registry.resolve(operation, 0U);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_EQ(resolved.value().value().controllable_component_id.value(), "robot.1");
  EXPECT_EQ(resolved.value().value().model_groups.front().compiled_group_name, "mf2_robot_group");
  EXPECT_EQ(registry.resource_inventory().size(), 1U);
}

TEST(RuntimeParticipantRegistryTest, RejectsWrongProgramBindingContext) {
  RuntimeParticipantRegistry registry(
      {binding_for("arm", "arm", "planning.motion.joint_goal", CoordinateKind::kAngular)});
  auto operation = operation_for("arm", "planning.motion.joint_goal", motion_payload());
  operation.program_binding_id = ProgramBindingId("other-binding");

  auto resolved = registry.resolve(operation, 0U);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().details.at("reason"), "runtime_participant_unavailable");
}

TEST(RuntimeParticipantRegistryTest, RejectsTrajectoryMotionAgainstPrismaticCommandTarget) {
  RuntimeParticipantRegistry registry(
      {binding_for("gripper", "gripper", "planning.motion.joint_goal", CoordinateKind::kLinear)});
  const auto operation =
      operation_for("gripper", "planning.motion.joint_goal", motion_payload("gripper"));

  auto resolved = registry.resolve(operation, 0U);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().details.at("reason"), "runtime_motion_resource_not_angular");
}

TEST(RuntimeParticipantRegistryTest, RejectsCommandCoordinateUnitMismatch) {
  const ProgramCommandSchemaRefDTO schema{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision))};
  RuntimeParticipantRegistry registry(
      {binding_for("gripper", "gripper", "tool.gripper.command", CoordinateKind::kLinear, schema)});
  CapabilityCommandIntentDTO command{
      schema.command_schema_id,
      schema.command_schema_revision,
      GripperPositionCommandValueDTO{AngularPositionDTO{0.2}, std::nullopt},
      2.0,
      {}};
  const auto operation = operation_for("gripper", "tool.gripper.command", std::move(command));

  auto resolved = registry.resolve(operation, 0U);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().details.at("reason"), "runtime_command_resource_unit_mismatch");
}

TEST(RuntimeParticipantRegistryTest, RejectsAmbiguousRegistrationAtStartup) {
  auto first = binding_for("arm", "arm", "planning.motion.joint_goal", CoordinateKind::kAngular);
  auto duplicate = first;
  EXPECT_THROW(RuntimeParticipantRegistry({std::move(first), std::move(duplicate)}),
               std::invalid_argument);
}

[[nodiscard]] RuntimeParticipantBindingDTO output_binding_for(std::string role) {
  const auto hash = std::string("sha256:") + std::string(64U, 'a');
  auto value = binding_for("gpio", "gpio", "io.gpio.set_output", CoordinateKind::kLinear).value();
  value.model_bindings.clear();
  value.model_groups.clear();
  value.frame_bindings.clear();
  value.motion_endpoint.reset();
  value.resources.clear();
  value.control_path.reset();
  value.command_schema =
      ProgramCommandSchemaRefDTO{CommandSchemaId("schema.output"), CommandSchemaRevision(hash)};
  value.capability_provider_path =
      RuntimeCapabilityProviderPathDTO{value.capability_id,
                                       value.capability_definition_revision,
                                       CapabilityProviderBindingId("provider.binding.output"),
                                       CapabilityProviderBindingRevision(hash),
                                       ProviderInstanceId("provider.native"),
                                       ProviderManifestId("provider-manifest.native"),
                                       ProviderManifestRevision(hash),
                                       RuntimeUnitId("runtime.native"),
                                       RuntimeEpoch("runtime-epoch.1")};
  const IoSignalDescriptorDTO signal{{value.controllable_component_id, "output.channel"},
                                     InterruptSourceId("provider.outputs"),
                                     IoSignalValueType::kBoolean,
                                     IoSignalDirection::kOutput,
                                     IoSignalUpdateMode::kChangeOnly,
                                     IoTransitionCapability::kSampledOnly,
                                     {IoFreshnessMode::kProviderValidity, std::nullopt},
                                     ResourceId("actual.provider.channel")};
  const auto digest = ComputeIoSignalDescriptorDigest(signal);
  if (!digest.ok()) throw std::logic_error(digest.error().message);
  value.io_output_binding = RuntimeIoOutputBindingDTO{
      ResourceRoleId(std::move(role)),
      signal,
      digest.value(),
      ComponentLifecycleGeneration(3U),
      {ControlDomainId("domain.io"), ControlDomainRevision(hash),
       ProviderInstanceId("provider.native"), ProviderManifestId("provider-manifest.native"),
       ProviderManifestRevision(hash), RuntimeUnitId("runtime.native"),
       RuntimeEpoch("runtime-epoch.1"), ContentDigest(hash)},
      ContentDigest(hash),
      IoOutputCompletionPolicyDTO(IoOutputSourceAckBudgetDTO{2'000'000'000LL})};
  return value;
}

TEST(RuntimeParticipantRegistryTest,
     ResolvesOutputRoleAndAliasesOneActualChannelWithoutCoordinates) {
  auto first = verify_runtime_participant_binding(output_binding_for("open"));
  auto alias = verify_runtime_participant_binding(output_binding_for("valve"));
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(alias.ok()) << alias.error().message;
  RuntimeParticipantRegistry registry({first.value(), alias.value()});
  ASSERT_EQ(registry.resource_inventory(),
            std::vector<ResourceId>{ResourceId("actual.provider.channel")});
  CapabilityCommandIntentDTO command{
      CommandSchemaId("schema.output"),
      CommandSchemaRevision(std::string("sha256:") + std::string(64U, 'a')),
      IoSignalWriteCommandValueDTO{ResourceRoleId("valve"), true},
      std::nullopt,
      {}};
  auto operation = operation_for("gpio", "io.gpio.set_output", command);
  const auto resolved = registry.resolve(operation, 0U);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_EQ(resolved.value().value().io_output_binding->resource_role_id, ResourceRoleId("valve"));
  EXPECT_TRUE(resolved.value().value().resources.empty());
  EXPECT_FALSE(resolved.value().value().control_path);
  const auto identity = runtime_participant_binding_identity(resolved.value().value());
  ASSERT_TRUE(identity.ok());
  EXPECT_NE(identity.value().provider(), nullptr);
  EXPECT_EQ(identity.value().controller(), nullptr);
  std::get<IoSignalWriteCommandValueDTO>(command.command_value).resource_role_id =
      ResourceRoleId("missing");
  EXPECT_FALSE(registry.resolve(operation_for("gpio", "io.gpio.set_output", command), 0U).ok());
  EXPECT_FALSE(RuntimeParticipantRegistry::validate_resolved(operation, 0U, first.value()).ok());
}

TEST(RuntimeParticipantRegistryTest,
     RejectsOutputFakeCoordinatesChangedDigestAndStaleProviderEpoch) {
  auto value = output_binding_for("valve");
  value.resources =
      binding_for("gripper", "gripper", "tool.gripper.command", CoordinateKind::kLinear)
          .value()
          .resources;
  EXPECT_FALSE(verify_runtime_participant_binding(value).ok());
  value = output_binding_for("valve");
  value.io_output_binding->signal.signal_id.name = "changed.channel";
  EXPECT_FALSE(verify_runtime_participant_binding(value).ok());
  value = output_binding_for("valve");
  value.io_output_binding->provider.provider_runtime_epoch = RuntimeEpoch("stale.epoch");
  EXPECT_FALSE(verify_runtime_participant_binding(value).ok());
  value = output_binding_for("valve");
  value.capability_provider_path->provider_runtime_epoch = RuntimeEpoch("stale.epoch");
  EXPECT_FALSE(verify_runtime_participant_binding(value).ok());
}

class ScriptedOutputProvider final : public IIoOutputProvider {
 public:
  explicit ScriptedOutputProvider(FakeClock& clock) : clock(clock) {}
  IoOutputEvidence StrongestOutputEvidence() const noexcept override { return strongest; }
  Result<IoOutputObservationDTO> SubmitOutput(const IoOutputWriteRequestDTO& request) override {
    ++sends;
    retained_request = request;
    if (throw_submit) throw std::runtime_error("lost send reply");
    observation = IoOutputObservationDTO{request.identity, first, clock.now(), {}};
    if (throw_after_receipt) throw std::runtime_error("lost retained receipt");
    clock.advance(return_delay_ns);
    return Result<IoOutputObservationDTO>::Ok(*observation);
  }
  Result<IoOutputObservationDTO> ObserveOutputResult(
      const IoOutputCommandIdentityDTO&) const override {
    return Result<IoOutputObservationDTO>::Ok(*observation);
  }
  Result<void> ExcludeFromGroupModel(const IoOutputCommandIdentityDTO& identity) override {
    if (!retained_request || retained_request->identity != identity)
      return Result<void>::Err(ErrorCode::kInvalidRequest);
    ++exclusion_attempts;
    if (fail_exclusion)
      return Result<void>::Err({"model_exclusion_failed", "test barrier refused", {}});
    model_excluded = true;
    return Result<void>::Ok();
  }
  Result<void> RetireOutput(const IoOutputCommandIdentityDTO& identity) override {
    if (!retained_request || retained_request->identity != identity)
      return Result<void>::Err(ErrorCode::kInvalidRequest);
    ++retires;
    retained_request.reset();
    return Result<void>::Ok();
  }
  FakeClock& clock;
  IoOutputEvidence strongest{IoOutputEvidence::kSourceAccepted};
  IoOutputEvidence first{IoOutputEvidence::kSourceAccepted};
  bool throw_submit{};
  bool throw_after_receipt{};
  bool fail_exclusion{};
  bool model_excluded{};
  std::size_t exclusion_attempts{};
  Duration return_delay_ns{};
  std::size_t sends{}, retires{};
  std::optional<IoOutputWriteRequestDTO> retained_request;
  mutable std::optional<IoOutputObservationDTO> observation;
};

[[nodiscard]] ResolvedPreparedCapabilityCommandExecutionRequestDTO output_engine_request(
    std::string operation = "output.operation", bool value = true,
    IoOutputCompletionPolicyDTO policy = IoOutputCompletionPolicyDTO(IoOutputSourceAckBudgetDTO{
        2'000'000'000LL})) {
  auto binding = output_binding_for("valve");
  binding.io_output_binding->completion_policy = std::move(policy);
  const auto schema = builtin_io_signal_write_command_schema();
  binding.command_schema =
      ProgramCommandSchemaRefDTO{schema.command_schema_id, schema.command_schema_revision};
  const auto sealed = verify_runtime_participant_binding(std::move(binding));
  if (!sealed.ok()) throw std::logic_error(sealed.error().message);
  const CapabilityCommandIntentDTO command{
      schema.command_schema_id,
      schema.command_schema_revision,
      IoSignalWriteCommandValueDTO{ResourceRoleId("valve"), value},
      std::nullopt,
      {}};
  auto made =
      MakePreparedIoOutputParticipant(OperationId(std::move(operation)), OperationRevision(1U),
                                      OperationParticipantId("output.participant"),
                                      ProgramRunId("output.run"), command, sealed.value());
  if (!made.ok()) throw std::logic_error(made.error().message);
  auto product = std::make_shared<const PreparedOperationParticipantDTO>(std::move(made.value()));
  const auto& output = std::get<PreparedIoOutputCommandDTO>(product->payload);
  return {product, sealed.value(),
          ResolvedPreparedIoOutputCommandAuthorityDTO{
              output.signal_id, output.output_resource, output.command_value.value,
              product->provider()->lifecycle_provider, output.completion_policy}};
}

TEST(RuntimeParticipantRegistryTest, OutputImmediatePolicyHasNoSyntheticDeadlineOrWait) {
  for (const auto first : {IoOutputEvidence::kSourceAccepted, IoOutputEvidence::kSubmitted}) {
    FakeClock clock;
    ScriptedOutputProvider provider(clock);
    provider.first = first;
    provider.return_delay_ns = 3'000'000'000LL;
    IoOutputParticipantEngine engine(provider, clock);
    auto request = output_engine_request("output.immediate", true,
                                         IoOutputCompletionPolicyDTO(IoOutputImmediateDTO{}));
    const auto digest = request.participant->prepared_contract_digest;
    const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
    ASSERT_TRUE(prepared.ok());
    ASSERT_TRUE(engine
                    .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                       ControllerGoalId("output.goal"),
                                                       ControllerGoalGeneration(1U))
                    .ok());
    ASSERT_TRUE(provider.retained_request);
    EXPECT_FALSE(provider.retained_request->absolute_deadline_ns);
    const auto status = engine.get_status();
    ASSERT_TRUE(status.fact);
    EXPECT_EQ(status.fact->effect, first == IoOutputEvidence::kSourceAccepted
                                       ? ParticipantEffectKnowledge::kEffectSettled
                                       : ParticipantEffectKnowledge::kEffectUncertain);
    EXPECT_EQ(provider.sends, 1U);
  }
}

TEST(RuntimeParticipantRegistryTest,
     SubmittedOnlyOutputCompletesWithItsActualReceiptAndNoDeadline) {
  FakeClock clock;
  ScriptedOutputProvider provider(clock);
  provider.strongest = IoOutputEvidence::kSubmitted;
  provider.first = IoOutputEvidence::kSubmitted;
  provider.return_delay_ns = 3'000'000'000LL;
  IoOutputParticipantEngine engine(provider, clock);
  auto request = output_engine_request("output.submitted", true,
                                       IoOutputCompletionPolicyDTO(IoOutputSubmittedDTO{}));
  const auto digest = request.participant->prepared_contract_digest;
  const auto encoded = EncodePreparedIoOutputParticipant(*request.participant, request.binding);
  ASSERT_TRUE(encoded.ok());
  EXPECT_NE(encoded.value().find("\"kind\":\"submitted_only\""), std::string::npos);
  EXPECT_NE(encoded.value().find("manyforge.prepared_operation_participant.v2"),
            std::string::npos);
  EXPECT_NE(encoded.value().find("\"signal_id\":{\"controllable_component_id\":"),
            std::string::npos);
  EXPECT_EQ(encoded.value().find("\"component_id\""), std::string::npos);
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  const auto dispatched = engine.DispatchPreparedCapabilityCommand(
      prepared.value(), digest, ControllerGoalId("output.goal"), ControllerGoalGeneration(1U));
  ASSERT_TRUE(dispatched.ok());
  ASSERT_TRUE(provider.retained_request);
  EXPECT_FALSE(provider.retained_request->absolute_deadline_ns);
  const auto observed = engine.ObservePreparedCapabilityCommand(prepared.value());
  ASSERT_TRUE(observed.ok());
  ASSERT_TRUE(observed.value().output_observation);
  EXPECT_EQ(observed.value().output_observation->evidence, IoOutputEvidence::kSubmitted);
  const auto status = engine.get_status();
  ASSERT_TRUE(status.fact);
  EXPECT_EQ(status.fact->kind, ParticipantExecutionFactKind::kComputedCompletion);
  clock.advance(100'000'000'000LL);
  EXPECT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                     ControllerGoalId("output.goal"),
                                                     ControllerGoalGeneration(1U))
                  .ok());
  EXPECT_EQ(provider.sends, 1U);
}

TEST(RuntimeParticipantRegistryTest, OutputMechanismCapabilityMismatchRefusesBeforeSubmission) {
  for (const bool submitted_port : {false, true}) {
    SCOPED_TRACE(submitted_port);
    FakeClock clock;
    ScriptedOutputProvider provider(clock);
    provider.strongest =
        submitted_port ? IoOutputEvidence::kSubmitted : IoOutputEvidence::kSourceAccepted;
    IoOutputParticipantEngine engine(provider, clock);
    auto request = submitted_port
                       ? output_engine_request()
                       : output_engine_request("output.wrong.port", true,
                                               IoOutputCompletionPolicyDTO(IoOutputSubmittedDTO{}));
    const auto refused = engine.PrepareCapabilityCommand(std::move(request));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"), "io_output_mechanism_capability_mismatch");
    EXPECT_EQ(provider.sends, 0U);
  }
}

TEST(RuntimeParticipantRegistryTest, SubmittedOnlyOutputCannotInventAcknowledgementOrNoEffect) {
  for (const auto receipt : {IoOutputEvidence::kSourceAccepted, IoOutputEvidence::kSourceRejected,
                             IoOutputEvidence::kNotSubmitted}) {
    SCOPED_TRACE(to_string(receipt));
    FakeClock clock;
    ScriptedOutputProvider provider(clock);
    provider.strongest = IoOutputEvidence::kSubmitted;
    provider.first = receipt;
    IoOutputParticipantEngine engine(provider, clock);
    auto request = output_engine_request("output.submitted.receipt", false,
                                         IoOutputCompletionPolicyDTO(IoOutputSubmittedDTO{}));
    const auto digest = request.participant->prepared_contract_digest;
    const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
    ASSERT_TRUE(prepared.ok());
    ASSERT_TRUE(engine
                    .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                       ControllerGoalId("output.goal"),
                                                       ControllerGoalGeneration(1U))
                    .ok());
    const auto observed = engine.ObservePreparedCapabilityCommand(prepared.value());
    ASSERT_TRUE(observed.ok());
    ASSERT_TRUE(observed.value().output_observation);
    EXPECT_EQ(observed.value().output_observation->evidence,
              receipt == IoOutputEvidence::kNotSubmitted ? IoOutputEvidence::kNotSubmitted
                                                         : IoOutputEvidence::kAcceptanceUnknown);
    const auto status = engine.get_status();
    ASSERT_TRUE(status.fact);
    EXPECT_EQ(status.fact->effect, receipt == IoOutputEvidence::kNotSubmitted
                                       ? ParticipantEffectKnowledge::kNoEffect
                                       : ParticipantEffectKnowledge::kEffectUncertain);
    EXPECT_EQ(provider.sends, 1U);
  }
}

TEST(RuntimeParticipantRegistryTest, LostSubmittedReturnExcludesModelBeforeUnknownIsObserved) {
  FakeClock clock;
  ScriptedOutputProvider provider(clock);
  provider.strongest = IoOutputEvidence::kSubmitted;
  provider.first = IoOutputEvidence::kSubmitted;
  provider.throw_after_receipt = true;
  IoOutputParticipantEngine engine(provider, clock);
  auto request = output_engine_request("output.lost.receipt", true,
                                       IoOutputCompletionPolicyDTO(IoOutputSubmittedDTO{}));
  const auto digest = request.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok());
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                     ControllerGoalId("output.goal"),
                                                     ControllerGoalGeneration(1U))
                  .ok());
  ASSERT_TRUE(provider.observation);
  EXPECT_EQ(provider.observation->evidence, IoOutputEvidence::kSubmitted);
  EXPECT_TRUE(provider.model_excluded);
  EXPECT_EQ(provider.exclusion_attempts, 1U);
  const auto observed = engine.ObservePreparedCapabilityCommand(prepared.value());
  ASSERT_TRUE(observed.ok());
  ASSERT_TRUE(observed.value().output_observation);
  EXPECT_EQ(observed.value().output_observation->evidence, IoOutputEvidence::kAcceptanceUnknown);
  ASSERT_TRUE(engine.get_status().fact);
  EXPECT_EQ(engine.get_status().fact->effect, ParticipantEffectKnowledge::kEffectUncertain);
  EXPECT_FALSE(engine.acknowledge_completed(prepared.value()).accepted);
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                     ControllerGoalId("output.goal"),
                                                     ControllerGoalGeneration(1U))
                  .ok());
  EXPECT_EQ(provider.sends, 1U);
  EXPECT_EQ(provider.retires, 0U);
  EXPECT_EQ(provider.exclusion_attempts, 1U);
}

TEST(RuntimeParticipantRegistryTest,
     FailedGroupExclusionExposesNoTerminalPermissionAndRetriesSameIdentity) {
  FakeClock clock;
  ScriptedOutputProvider provider(clock);
  provider.strongest = IoOutputEvidence::kSubmitted;
  provider.first = IoOutputEvidence::kSubmitted;
  provider.throw_after_receipt = true;
  provider.fail_exclusion = true;
  IoOutputParticipantEngine engine(provider, clock);
  auto request = output_engine_request("output.exclusion.failure", true,
                                       IoOutputCompletionPolicyDTO(IoOutputSubmittedDTO{}));
  const auto digest = request.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok());
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                     ControllerGoalId("output.goal"),
                                                     ControllerGoalGeneration(1U))
                  .ok());
  const auto refused = engine.ObservePreparedCapabilityCommand(prepared.value());
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "model_exclusion_failed");
  EXPECT_FALSE(engine.get_status().fact);
  EXPECT_FALSE(engine.get_recovery_assessment().eligible_close);
  EXPECT_FALSE(provider.model_excluded);
  EXPECT_EQ(provider.retires, 0U);
  provider.fail_exclusion = false;
  const auto observed = engine.ObservePreparedCapabilityCommand(prepared.value());
  ASSERT_TRUE(observed.ok());
  ASSERT_TRUE(observed.value().output_observation);
  EXPECT_EQ(observed.value().output_observation->evidence, IoOutputEvidence::kAcceptanceUnknown);
  EXPECT_TRUE(provider.model_excluded);
  EXPECT_EQ(provider.sends, 1U);
  EXPECT_EQ(provider.retires, 0U);
}

TEST(RuntimeParticipantRegistryTest, OutputPolicyIsSealedAndCallerTimeoutIsForbidden) {
  auto request = output_engine_request();
  auto changed = *request.participant;
  std::get<PreparedIoOutputCommandDTO>(changed.payload).completion_policy =
      IoOutputCompletionPolicyDTO(IoOutputImmediateDTO{});
  EXPECT_FALSE(VerifyPreparedIoOutputParticipant(changed, request.binding).ok());
  const auto schema = builtin_io_signal_write_command_schema();
  CapabilityCommandIntentDTO caller{schema.command_schema_id,
                                    schema.command_schema_revision,
                                    IoSignalWriteCommandValueDTO{ResourceRoleId("valve"), true},
                                    0.5,
                                    {}};
  EXPECT_FALSE(validate_operation_intent(operation_for("gpio", "io.gpio.set_output", caller)).ok());
  EXPECT_FALSE(MakePreparedIoOutputParticipant(OperationId("new.output"), OperationRevision(1U),
                                               OperationParticipantId("output.participant"),
                                               ProgramRunId("output.run"), caller, request.binding)
                   .ok());
  caller.completion_timeout_s.reset();
  EXPECT_TRUE(validate_operation_intent(operation_for("gpio", "io.gpio.set_output", caller)).ok());
  auto invalid_binding = request.binding.value();
  invalid_binding.io_output_binding->completion_policy =
      IoOutputCompletionPolicyDTO(IoOutputSourceAckBudgetDTO{0});
  EXPECT_FALSE(verify_runtime_participant_binding(std::move(invalid_binding)).ok());
}

TEST(RuntimeParticipantRegistryTest, OutputPolicyRetainsFullDurationWithoutDoubleRounding) {
  constexpr Duration budget = 9'223'372'036'000'000'000LL;
  auto request = output_engine_request(
      "output.wide", true, IoOutputCompletionPolicyDTO(IoOutputSourceAckBudgetDTO{budget}));
  const auto encoded = EncodePreparedIoOutputParticipant(*request.participant, request.binding);
  ASSERT_TRUE(encoded.ok());
  EXPECT_NE(encoded.value().find("\"timeout_ns\":\"9223372036000000000\""), std::string::npos);
  FakeClock clock;
  clock.advance(1'000'000'000LL);
  ScriptedOutputProvider provider(clock);
  IoOutputParticipantEngine engine(provider, clock);
  const auto digest = request.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok());
  EXPECT_FALSE(engine
                   .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                      ControllerGoalId("output.goal"),
                                                      ControllerGoalGeneration(1U))
                   .ok());
  EXPECT_EQ(provider.sends, 0U);
}

TEST(RuntimeParticipantRegistryTest, OutputSessionExactReplaySendsOnceAndRequiresOwnerRetirement) {
  FakeClock clock;
  ScriptedOutputProvider provider(clock);
  IoOutputParticipantEngine engine(provider, clock);
  auto request = output_engine_request();
  const auto digest = request.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok());
  const auto sent = engine.DispatchPreparedCapabilityCommand(
      prepared.value(), digest, ControllerGoalId("output.goal"), ControllerGoalGeneration(1U));
  ASSERT_TRUE(sent.ok());
  EXPECT_TRUE(sent.value().accepted);
  ASSERT_TRUE(engine.get_status().fact);
  EXPECT_EQ(engine.get_status().fact->kind, ParticipantExecutionFactKind::kComputedCompletion);
  EXPECT_EQ(provider.sends, 1U);
  clock.advance(3'000'000'000LL);
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                     ControllerGoalId("output.goal"),
                                                     ControllerGoalGeneration(1U))
                  .ok());
  EXPECT_EQ(provider.sends, 1U);
  EXPECT_FALSE(engine.PrepareCapabilityCommand(output_engine_request("next.operation")).ok());
  ASSERT_TRUE(engine.acknowledge_completed(prepared.value()).accepted);
  EXPECT_EQ(provider.retires, 1U);
  EXPECT_TRUE(engine.PrepareCapabilityCommand(output_engine_request("next.operation")).ok());
}

TEST(RuntimeParticipantRegistryTest, OutputSessionBudgetExpiresOnceAndLateAckCannotClearUnknown) {
  FakeClock clock;
  ScriptedOutputProvider provider(clock);
  provider.first = IoOutputEvidence::kSubmitted;
  IoOutputParticipantEngine engine(provider, clock);
  auto request = output_engine_request();
  const auto digest = request.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok());
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                     ControllerGoalId("output.goal"),
                                                     ControllerGoalGeneration(1U))
                  .ok());
  EXPECT_FALSE(engine.get_status().fact);
  clock.advance(2'000'000'000LL);
  const auto expired = engine.get_status();
  ASSERT_TRUE(expired.fact);
  EXPECT_EQ(expired.fact->effect, ParticipantEffectKnowledge::kEffectUncertain);
  provider.observation->evidence = IoOutputEvidence::kSourceAccepted;
  EXPECT_EQ(engine.get_status().fact, expired.fact);
  EXPECT_FALSE(engine.get_recovery_assessment().eligible_close);
  EXPECT_FALSE(engine.acknowledge_completed(prepared.value()).accepted);
  EXPECT_EQ(provider.retires, 0U);
  EXPECT_EQ(provider.sends, 1U);
}

TEST(RuntimeParticipantRegistryTest, OutputTimelySourceReceiptSurvivesLateReconciliation) {
  for (const bool delayed_submit_return : {false, true}) {
    FakeClock clock;
    ScriptedOutputProvider provider(clock);
    provider.first =
        delayed_submit_return ? IoOutputEvidence::kSourceAccepted : IoOutputEvidence::kSubmitted;
    provider.return_delay_ns = delayed_submit_return ? 3'000'000'000LL : 0;
    IoOutputParticipantEngine engine(provider, clock);
    auto request = output_engine_request();
    const auto digest = request.participant->prepared_contract_digest;
    const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
    ASSERT_TRUE(prepared.ok());
    ASSERT_TRUE(engine
                    .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                       ControllerGoalId("output.goal"),
                                                       ControllerGoalGeneration(1U))
                    .ok());
    if (!delayed_submit_return) {
      clock.advance(1'000'000'000LL);
      provider.observation->evidence = IoOutputEvidence::kSourceAccepted;
      provider.observation->trusted_received_at_ns = clock.now();
      clock.advance(2'000'000'000LL);
    }
    const auto observed = engine.ObservePreparedCapabilityCommand(prepared.value());
    ASSERT_TRUE(observed.ok());
    ASSERT_TRUE(observed.value().output_observation);
    EXPECT_EQ(observed.value().output_observation->evidence, IoOutputEvidence::kSourceAccepted);
    ASSERT_TRUE(observed.value().output_execution_status->fact);
    EXPECT_EQ(observed.value().output_execution_status->fact->kind,
              ParticipantExecutionFactKind::kComputedCompletion);
    EXPECT_EQ(provider.retained_request->absolute_deadline_ns, 2'000'000'000LL);
    EXPECT_EQ(provider.sends, 1U);
  }
}

TEST(RuntimeParticipantRegistryTest, OutputExpiredAndInvalidReceiptsNeverBecomeSourceAcceptance) {
  for (int mutation = 0; mutation < 5; ++mutation) {
    FakeClock clock;
    ScriptedOutputProvider provider(clock);
    provider.first = IoOutputEvidence::kSubmitted;
    IoOutputParticipantEngine engine(provider, clock);
    auto request = output_engine_request();
    const auto digest = request.participant->prepared_contract_digest;
    const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
    ASSERT_TRUE(prepared.ok());
    ASSERT_TRUE(engine
                    .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                       ControllerGoalId("output.goal"),
                                                       ControllerGoalGeneration(1U))
                    .ok());
    clock.advance(3'000'000'000LL);
    auto& result = *provider.observation;
    result.evidence = IoOutputEvidence::kSourceAccepted;
    result.trusted_received_at_ns = 1'000'000'000LL;
    if (mutation == 0) result.trusted_received_at_ns = 2'000'000'000LL;
    if (mutation == 1) result.trusted_received_at_ns = -1;
    if (mutation == 2) result.trusted_received_at_ns = clock.now() + 1;
    if (mutation == 3)
      result.identity.command.command_id = ControllerGoalId("foreign.goal");
    if (mutation == 4) result.evidence = IoOutputEvidence::kSubmitted;
    const auto status = engine.get_status();
    ASSERT_TRUE(status.fact);
    EXPECT_EQ(status.fact->effect, ParticipantEffectKnowledge::kEffectUncertain);
    result.evidence = IoOutputEvidence::kSourceAccepted;
    result.trusted_received_at_ns = 1'000'000'000LL;
    EXPECT_EQ(engine.get_status().fact, status.fact);
    EXPECT_EQ(provider.sends, 1U);
    EXPECT_EQ(provider.retires, 0U);
  }
}

TEST(RuntimeParticipantRegistryTest, OutputDefinitelyNotSubmittedProofSurvivesExpiredBudget) {
  FakeClock clock;
  ScriptedOutputProvider provider(clock);
  provider.first = IoOutputEvidence::kNotSubmitted;
  provider.return_delay_ns = 3'000'000'000LL;
  IoOutputParticipantEngine engine(provider, clock);
  auto request = output_engine_request();
  const auto digest = request.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok());
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                     ControllerGoalId("output.goal"),
                                                     ControllerGoalGeneration(1U))
                  .ok());
  ASSERT_TRUE(engine.get_status().fact);
  EXPECT_EQ(engine.get_status().fact->effect, ParticipantEffectKnowledge::kNoEffect);
  EXPECT_TRUE(engine.get_recovery_assessment().eligible_close);
  EXPECT_EQ(provider.sends, 1U);
}

TEST(RuntimeParticipantRegistryTest, OutputSessionExceptionAfterPossibleSendIsNotResentOrNoEffect) {
  FakeClock clock;
  ScriptedOutputProvider provider(clock);
  provider.throw_submit = true;
  IoOutputParticipantEngine engine(provider, clock);
  auto request = output_engine_request();
  const auto digest = request.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok());
  const auto first = engine.DispatchPreparedCapabilityCommand(
      prepared.value(), digest, ControllerGoalId("output.goal"), ControllerGoalGeneration(1U));
  ASSERT_TRUE(first.ok());
  EXPECT_TRUE(first.value().accepted);  // Local receipt, distinct from provider acceptance.
  ASSERT_TRUE(engine.get_status().fact);
  EXPECT_EQ(engine.get_status().fact->effect, ParticipantEffectKnowledge::kEffectUncertain);
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                     ControllerGoalId("output.goal"),
                                                     ControllerGoalGeneration(1U))
                  .ok());
  EXPECT_EQ(provider.sends, 1U);
}

TEST(RuntimeParticipantRegistryTest, OutputSessionPrecommitCancelAndAlteredProductHaveNoSend) {
  FakeClock clock;
  ScriptedOutputProvider provider(clock);
  IoOutputParticipantEngine engine(provider, clock);
  auto request = output_engine_request();
  auto changed = *request.participant;
  std::get<PreparedIoOutputCommandDTO>(changed.payload).command_value.value = false;
  EXPECT_FALSE(VerifyPreparedIoOutputParticipant(changed, request.binding).ok());
  auto invalid = request;
  invalid.participant = std::make_shared<const PreparedOperationParticipantDTO>(std::move(changed));
  EXPECT_FALSE(engine.PrepareCapabilityCommand(std::move(invalid)).ok());
  const auto digest = request.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok());
  ASSERT_TRUE(engine.CancelPreparedCapabilityCommand(prepared.value()).ok());
  EXPECT_FALSE(engine
                   .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                      ControllerGoalId("output.goal"),
                                                      ControllerGoalGeneration(1U))
                   .ok());
  ASSERT_TRUE(engine.get_status().fact);
  EXPECT_EQ(engine.get_status().fact->effect, ParticipantEffectKnowledge::kNoEffect);
  EXPECT_EQ(provider.sends, 0U);
}

TEST(RuntimeParticipantRegistryTest,
     OutputPublishOnlyCompletionRetainsSubmittedInsteadOfSourceAck) {
  FakeClock clock;
  ScriptedOutputProvider provider(clock);
  provider.strongest = IoOutputEvidence::kSubmitted;
  provider.first = IoOutputEvidence::kSubmitted;
  IoOutputParticipantEngine engine(provider, clock);
  auto request = output_engine_request(
      "output.publish_only", true, IoOutputCompletionPolicyDTO(IoOutputSubmittedDTO{}));
  const auto digest = request.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok());
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                     ControllerGoalId("output.goal"),
                                                     ControllerGoalGeneration(1U))
                  .ok());
  const auto observed = engine.ObservePreparedCapabilityCommand(prepared.value());
  ASSERT_TRUE(observed.ok());
  ASSERT_TRUE(observed.value().output_observation);
  EXPECT_EQ(observed.value().output_observation->evidence, IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(observed.value().output_execution_status->fact);
  EXPECT_EQ(observed.value().output_execution_status->fact->kind,
            ParticipantExecutionFactKind::kComputedCompletion);
}

TEST(RuntimeParticipantRegistryTest, OutputUnknownResetRequiresExactAcknowledgedLocalRecovery) {
  FakeClock clock;
  ScriptedOutputProvider provider(clock);
  provider.first = IoOutputEvidence::kSubmitted;
  IoOutputParticipantEngine engine(provider, clock);
  auto request = output_engine_request();
  const auto binding = request.binding.value();
  const auto digest = request.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok());
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                     ControllerGoalId("output.goal"),
                                                     ControllerGoalGeneration(1U))
                  .ok());
  ASSERT_TRUE(provider.retained_request);
  const auto identity = provider.retained_request->identity;
  clock.advance(2'000'000'000LL);
  const auto before = engine.get_status();
  ASSERT_TRUE(before.fact);
  ASSERT_EQ(before.fact->effect, ParticipantEffectKnowledge::kEffectUncertain);
  const auto target = binding.command_target_id.value();
  EXPECT_FALSE(engine.reset_capability_command(target, prepared.value(), 1U).ok());
  EXPECT_EQ(provider.retires, 0U);
  IoOutputResetAuthorityDTO authority{
      identity,
      RequestId("operator.reset.output"),
      OperationRevision(2U),
      {binding.retained_bundle_id, binding.graph_revision, binding.controllable_component_id,
       identity.provider, LifecycleTransactionId("actual.disable"),
       LifecycleTransactionId("actual.enable"), ComponentLifecycleGeneration(4U),
       ComponentLifecycleGeneration(5U)}};
  for (int mutation = 0; mutation < 6; ++mutation) {
    auto wrong = authority;
    if (mutation == 0) wrong.command.prepared_contract_digest = ContentDigest("foreign");
    if (mutation == 1)
      wrong.local_resynchronization.provider.provider_runtime_epoch = RuntimeEpoch("foreign");
    if (mutation == 2)
      wrong.local_resynchronization.disabled_generation = ComponentLifecycleGeneration(3U);
    if (mutation == 3)
      wrong.local_resynchronization.enabled_generation = ComponentLifecycleGeneration(4U);
    if (mutation == 4)
      wrong.local_resynchronization.enable_transaction_id =
          wrong.local_resynchronization.disable_transaction_id;
    if (mutation == 5) wrong.acknowledgement_request_id = RequestId("");
    EXPECT_FALSE(engine.reset_capability_command(target, prepared.value(), 1U, wrong).ok());
    EXPECT_EQ(provider.retires, 0U);
    EXPECT_EQ(engine.get_status().fact, before.fact);
  }
  // Local retirement is authorized despite residual remote uncertainty. The
  // recorded source evidence remains unknown; no resend or remote undo occurs.
  ASSERT_TRUE(engine.reset_capability_command(target, prepared.value(), 1U, authority).ok());
  EXPECT_EQ(provider.retires, 1U);
  EXPECT_EQ(provider.sends, 1U);
  const auto retained = engine.ObservePreparedCapabilityCommand(prepared.value());
  ASSERT_TRUE(retained.ok());
  ASSERT_TRUE(retained.value().output_observation);
  EXPECT_EQ(retained.value().output_observation->evidence, IoOutputEvidence::kAcceptanceUnknown);
  EXPECT_TRUE(engine.reset_capability_command(target, prepared.value(), 1U, authority).ok());
  EXPECT_EQ(provider.retires, 1U);
  EXPECT_FALSE(engine.reset_capability_command(target, prepared.value(), 1U).ok());
  authority.acknowledgement_request_id = RequestId("another.operator.request");
  EXPECT_FALSE(engine.reset_capability_command(target, prepared.value(), 1U, authority).ok());
}

TEST(RuntimeParticipantRegistryTest, OutputKnownRejectionResetNeedsNoUncertaintyAcknowledgement) {
  FakeClock clock;
  ScriptedOutputProvider provider(clock);
  provider.first = IoOutputEvidence::kSourceRejected;
  IoOutputParticipantEngine engine(provider, clock);
  auto request = output_engine_request();
  const auto target = request.binding.value().command_target_id.value();
  const auto digest = request.participant->prepared_contract_digest;
  const auto prepared = engine.PrepareCapabilityCommand(std::move(request));
  ASSERT_TRUE(prepared.ok());
  ASSERT_TRUE(engine
                  .DispatchPreparedCapabilityCommand(prepared.value(), digest,
                                                     ControllerGoalId("output.goal"),
                                                     ControllerGoalGeneration(1U))
                  .ok());
  ASSERT_TRUE(engine.get_status().fact);
  EXPECT_EQ(engine.get_status().fact->effect, ParticipantEffectKnowledge::kEffectSettled);
  EXPECT_TRUE(engine.reset_capability_command(target, prepared.value(), 1U).ok());
  EXPECT_TRUE(engine.reset_capability_command(target, prepared.value(), 1U).ok());
  EXPECT_EQ(provider.retires, 1U);
  EXPECT_EQ(provider.sends, 1U);
}

TEST(RuntimeParticipantRegistryTest,
     QualifiedTcpMustMatchTheSelectedCaptureWithoutChangingArmOwnership) {
  auto value =
      binding_for("arm", "arm", "planning.motion.joint_goal", CoordinateKind::kAngular).value();
  value.motion_endpoint =
      RuntimeMotionEndpointBindingDTO{TargetRoleId("tool"),
                                      CommandTargetId("gripper"),
                                      {FrameRoleId("fingertip"), ModelInstanceId("robot.model"),
                                       ModelRevision("model-revision"), "mf2_fingertip"},
                                      {}};
  auto selected = verify_runtime_participant_binding(value);
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  RuntimeParticipantRegistry registry({selected.value()});
  auto motion = motion_payload();
  motion.tcp_endpoint = MotionEndpointReferenceDTO{TargetRoleId("tool"), FrameRoleId("fingertip")};
  auto operation = operation_for("arm", "planning.motion.joint_goal", motion);
  const auto resolved = registry.resolve(operation, 0U);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_EQ(resolved.value().value().command_target_id, CommandTargetId("arm"));
  EXPECT_EQ(resolved.value().value().resources, value.resources);
  auto& requested = std::get<MotionParticipantIntentDTO>(operation.participants.front().payload);
  requested.tcp_endpoint.target_role_id = TargetRoleId("other_tool");
  EXPECT_FALSE(registry.resolve(operation, 0U).ok());
  requested.tcp_endpoint.target_role_id = TargetRoleId("tool");
  requested.tcp_endpoint.frame_role_id = FrameRoleId("other_tip");
  EXPECT_FALSE(registry.resolve(operation, 0U).ok());
  requested = motion_payload();
  EXPECT_FALSE(registry.resolve(operation, 0U)
                   .ok());  // A different owner/frame cannot consume this captured TCP.
}

}  // namespace
