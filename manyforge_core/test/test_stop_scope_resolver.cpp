#include <gtest/gtest.h>

#include <manyforge_core/stop_scope_resolver.hpp>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core {
namespace {

[[nodiscard]] PublicRequestContextDTO request_context(
    std::string owner_epoch = "kernel.epoch.current") {
  return PublicRequestContextDTO{RequestId("request.stop.1"),
                                 RequestOwnerEpoch(std::move(owner_epoch)), "test-client",
                                 std::nullopt};
}

[[nodiscard]] StopScopeResolverSnapshotDTO independent_snapshot() {
  return StopScopeResolverSnapshotDTO{
      RequestOwnerEpoch("kernel.epoch.current"),
      GraphRevision("graph.current"),
      20U,
      StopGeneration(7U),
      {StopScopeOperationSnapshotDTO{OperationId("operation.a"),
                                     ProgramRunId("program-run.a"),
                                     OperationRevision(3U),
                                     StopGeneration(4U),
                                     OperationStateKind::kExecuting,
                                     true,
                                     false,
                                     {ControllableComponentId("robot.a")},
                                     {ResourceId("robot.a.joint.1")}},
       StopScopeOperationSnapshotDTO{OperationId("operation.b"),
                                     ProgramRunId("program-run.b"),
                                     OperationRevision(5U),
                                     StopGeneration(6U),
                                     OperationStateKind::kPaused,
                                     true,
                                     false,
                                     {ControllableComponentId("robot.b")},
                                     {ResourceId("robot.b.joint.1")}}},
      {StopScopeComponentSnapshotDTO{ControllableComponentId("robot.a"),
                                     ComponentLifecycleGeneration(10U),
                                     {ResourceId("robot.a.joint.1")}},
       StopScopeComponentSnapshotDTO{ControllableComponentId("robot.b"),
                                     ComponentLifecycleGeneration(11U),
                                     {ResourceId("robot.b.joint.1")}}},
      {StopScopeResourceSnapshotDTO{ResourceId("robot.a.joint.1"), StopGeneration(4U), false},
       StopScopeResourceSnapshotDTO{ResourceId("robot.b.joint.1"), StopGeneration(6U), false}},
      {}};
}

[[nodiscard]] StopIntentDTO operation_stop(std::vector<StopOperationTargetDTO> targets = {
                                               {OperationId("operation.a"), OperationRevision(3U),
                                                StopGeneration(4U)}}) {
  return StopIntentDTO{request_context(), std::move(targets), std::nullopt, {}};
}

[[nodiscard]] StopIntentDTO program_run_stop(std::string program_run_id = "program-run.a") {
  return StopIntentDTO{request_context(),
                       std::vector<StopProgramRunTargetDTO>{
                           StopProgramRunTargetDTO{ProgramRunId(std::move(program_run_id))}},
                       std::nullopt,
                       {}};
}

[[nodiscard]] StopIntentDTO component_stop(
    std::string component = "robot.a",
    GraphRevision graph_revision = GraphRevision("graph.current"),
    ComponentLifecycleGeneration lifecycle_generation = ComponentLifecycleGeneration(10U)) {
  return StopIntentDTO{
      request_context(),
      std::vector<StopComponentTargetDTO>{{ControllableComponentId(std::move(component)),
                                           std::move(graph_revision), lifecycle_generation}},
      std::nullopt,
      {}};
}

[[nodiscard]] StopIntentDTO resource_stop(
    std::string resource = "robot.a.joint.1",
    GraphRevision graph_revision = GraphRevision("graph.current"),
    std::uint64_t operation_resource_generation = 20U,
    StopGeneration stop_generation = StopGeneration(4U)) {
  return StopIntentDTO{request_context(),
                       std::vector<StopResourceTargetDTO>{
                           {ResourceId(std::move(resource)), std::move(graph_revision),
                            operation_resource_generation, stop_generation}},
                       std::nullopt,
                       {}};
}

[[nodiscard]] StopAcceptedDTO accepted_stop() {
  return StopAcceptedDTO{
      RequestId("request.stop.1"),
      RequestReplayStatusDTO::kAcceptedNew,
      StopId("stop.1"),
      StopGeneration(8U),
      ContentDigest("sha256:1111111111111111111111111111111111111111111111111111111111111111"),
      StopResolutionDispositionDTO::kExact,
      StopScopeResolutionReasonDTO::kExactCurrentScope,
      {},
      ContentDigest("sha256:2222222222222222222222222222222222222222222222222222222222222222"),
      {},
      {{OperationId("operation.a"), OperationRevision(4U)}},
      1'000,
      100,
      800,
      123'456,
      {{StopId("stop.1"),
        StopGeneration(8U),
        OperationId("operation.a"),
        OperationParticipantId("participant.a"),
        ContentDigest("sha256:3333333333333333333333333333333333333333333333333333333333333333"),
        {{StopBudgetPhaseId("command"), 250, 50}, {StopBudgetPhaseId("evidence"), 750, 550}}}}};
}

[[nodiscard]] StopDeadlineBudgetDTO fixed_budget() {
  return StopDeadlineBudgetDTO{
      1'000, 100, {{StopBudgetPhaseId("command"), 250}, {StopBudgetPhaseId("evidence"), 750}}};
}

[[nodiscard]] PreparedParticipantStopPlanDTO prepared_plan(
    std::string participant_id = "participant.a") {
  return PreparedParticipantStopPlanDTO{
      ContentDigest("sha256:3333333333333333333333333333333333333333333333333333333333333333"),
      ResolvedStopBasePlanId("base-plan.a"),
      OperationId("operation.a"),
      OperationRevision(3U),
      OperationParticipantId(std::move(participant_id)),
      ProgramId("program.a"),
      ProgramRevision("program.revision.a"),
      ProgramBindingId("binding.a"),
      ProgramRunId("run.a"),
      std::nullopt,
      ParticipantStopProfilePurposeDTO::kOrdinary,
      std::nullopt,
      fixed_budget(),
      {}};
}

TEST(StopScopeResolverTest, ResolvesExactOperationWithoutWideningToIndependentWork) {
  const auto result = resolve_stop_scope(operation_stop(), independent_snapshot());
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().disposition, StopResolutionDispositionDTO::kExact);
  EXPECT_FALSE(result.value().full_stop);
  EXPECT_EQ(result.value().operations, std::vector<OperationId>{OperationId("operation.a")});
  EXPECT_EQ(result.value().controllable_components,
            std::vector<ControllableComponentId>{ControllableComponentId("robot.a")});
  EXPECT_EQ(result.value().resources, std::vector<ResourceId>{ResourceId("robot.a.joint.1")});
  EXPECT_TRUE(result.value().stale_diagnostics.empty());
}

TEST(StopScopeResolverTest, ProgramStopIncludesAttachedManualWithoutChangingItsRunIdentity) {
  auto snapshot = independent_snapshot();
  snapshot.operations[1].state = OperationStateKind::kExecuting;
  snapshot.operations[1].parent_program_run_id = ProgramRunId("program-run.a");
  const auto result = resolve_stop_scope(program_run_stop(), snapshot);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().operations,
            (std::vector<OperationId>{OperationId("operation.a"), OperationId("operation.b")}));
  EXPECT_EQ(snapshot.operations[1].program_run_id, ProgramRunId("program-run.b"));
  snapshot.operations[1].parent_program_run_id = ProgramRunId("");
  EXPECT_FALSE(resolve_stop_scope(program_run_stop(), snapshot).ok());
}

TEST(StopScopeResolverTest, MalformedIngressSelectorIdentityIsStableAndRouteBound) {
  const MalformedStopIngressDTO ingress{
      RequestId("malformed-stop.http.1"), RequestOwnerEpoch("kernel.epoch.current"),
      RequestAttributionDTO{RequestTransportDTO::kHttp, RequestAssuranceDTO::kUnverified,
                            std::nullopt},
      "/api/execution/stops", "recognized_stop_payload_invalid"};
  EXPECT_EQ(malformed_stop_ingress_field_error(ingress), std::nullopt);
  const auto first = ComputeRequestedStopSelectorDigest(ingress);
  const auto second = ComputeRequestedStopSelectorDigest(ingress);
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_EQ(first.value(), second.value());

  auto other_route = ingress;
  other_route.recognized_route = "/api/execution/full-stop";
  const auto changed = ComputeRequestedStopSelectorDigest(other_route);
  ASSERT_TRUE(changed.ok()) << changed.error().message;
  EXPECT_NE(changed.value(), first.value());

  auto unknown_reason = ingress;
  unknown_reason.reason = "caller_supplied_reason";
  EXPECT_EQ(malformed_stop_ingress_field_error(unknown_reason),
            "malformed_stop_ingress_reason_invalid");
}

TEST(StopScopeResolverTest, ComponentAndResourceSelectorsExpandCoordinatedOperation) {
  auto snapshot = independent_snapshot();
  snapshot.operations = {StopScopeOperationSnapshotDTO{
      OperationId("operation.ab"),
      ProgramRunId("program-run.a"),
      OperationRevision(8U),
      StopGeneration(2U),
      OperationStateKind::kDispatching,
      true,
      false,
      {ControllableComponentId("robot.a"), ControllableComponentId("robot.b")},
      {ResourceId("robot.a.joint.1"), ResourceId("robot.b.joint.1")}}};

  const auto component = resolve_stop_scope(component_stop(), snapshot);
  ASSERT_TRUE(component.ok());
  EXPECT_EQ(component.value().operations, std::vector<OperationId>{OperationId("operation.ab")});
  EXPECT_EQ(component.value().controllable_components,
            (std::vector<ControllableComponentId>{ControllableComponentId("robot.a"),
                                                  ControllableComponentId("robot.b")}));
  EXPECT_EQ(component.value().resources, (std::vector<ResourceId>{ResourceId("robot.a.joint.1"),
                                                                  ResourceId("robot.b.joint.1")}));

  const auto resource = resolve_stop_scope(resource_stop(), snapshot);
  ASSERT_TRUE(resource.ok());
  EXPECT_EQ(resource.value().operations, component.value().operations);
  EXPECT_EQ(resource.value().controllable_components, component.value().controllable_components);
  EXPECT_EQ(resource.value().resources, component.value().resources);
}

TEST(StopScopeResolverTest, RetainsEveryTypedStaleMismatchAndUsesCurrentCompleteScope) {
  auto intent = operation_stop();
  intent.request_context = request_context("kernel.epoch.old");
  intent.selector = std::vector<StopOperationTargetDTO>{
      {OperationId("operation.a"), OperationRevision(2U), StopGeneration(1U)}};

  const auto result = resolve_stop_scope(intent, independent_snapshot());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().disposition,
            StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved);
  ASSERT_EQ(result.value().stale_diagnostics.size(), 3U);
  EXPECT_EQ(result.value().stale_diagnostics[0].field, StopScopeStaleFieldDTO::kRequestOwnerEpoch);
  EXPECT_EQ(std::get<RequestOwnerEpoch>(result.value().stale_diagnostics[0].expected),
            RequestOwnerEpoch("kernel.epoch.old"));
  EXPECT_EQ(std::get<RequestOwnerEpoch>(result.value().stale_diagnostics[0].current),
            RequestOwnerEpoch("kernel.epoch.current"));
  EXPECT_EQ(result.value().stale_diagnostics[1].field, StopScopeStaleFieldDTO::kOperationRevision);
  EXPECT_EQ(std::get<OperationRevision>(result.value().stale_diagnostics[1].expected),
            OperationRevision(2U));
  EXPECT_EQ(std::get<OperationRevision>(result.value().stale_diagnostics[1].current),
            OperationRevision(3U));
  EXPECT_EQ(result.value().stale_diagnostics[2].field,
            StopScopeStaleFieldDTO::kOperationStopGeneration);
  EXPECT_EQ(result.value().operations, std::vector<OperationId>{OperationId("operation.a")});
}

TEST(StopScopeResolverTest, RetainsComponentAndResourceGenerationMismatches) {
  const auto snapshot = independent_snapshot();
  const auto component = resolve_stop_scope(
      component_stop("robot.a", GraphRevision("graph.old"), ComponentLifecycleGeneration(9U)),
      snapshot);
  ASSERT_TRUE(component.ok());
  ASSERT_EQ(component.value().stale_diagnostics.size(), 2U);
  EXPECT_EQ(component.value().stale_diagnostics[0].field, StopScopeStaleFieldDTO::kGraphRevision);
  EXPECT_EQ(component.value().stale_diagnostics[1].field,
            StopScopeStaleFieldDTO::kComponentLifecycleGeneration);

  const auto resource = resolve_stop_scope(
      resource_stop("robot.a.joint.1", GraphRevision("graph.old"), 19U, StopGeneration(3U)),
      snapshot);
  ASSERT_TRUE(resource.ok());
  ASSERT_EQ(resource.value().stale_diagnostics.size(), 3U);
  EXPECT_EQ(resource.value().stale_diagnostics[0].field, StopScopeStaleFieldDTO::kGraphRevision);
  EXPECT_EQ(resource.value().stale_diagnostics[1].field,
            StopScopeStaleFieldDTO::kOperationResourceGeneration);
  EXPECT_EQ(resource.value().stale_diagnostics[2].field,
            StopScopeStaleFieldDTO::kResourceStopGeneration);
  EXPECT_EQ(std::get<std::uint64_t>(resource.value().stale_diagnostics[1].current), 20U);
}

TEST(StopScopeResolverTest, KnownTerminalAndIdleTargetsAreIdempotentNoEffect) {
  auto snapshot = independent_snapshot();
  snapshot.operations[0].state = OperationStateKind::kCompleted;

  auto stale_terminal = operation_stop();
  stale_terminal.selector = std::vector<StopOperationTargetDTO>{
      {OperationId("operation.a"), OperationRevision(2U), StopGeneration(3U)}};
  const auto terminal = resolve_stop_scope(stale_terminal, snapshot);
  ASSERT_TRUE(terminal.ok());
  EXPECT_EQ(terminal.value().disposition, StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
  EXPECT_TRUE(terminal.value().operations.empty());
  EXPECT_EQ(terminal.value().stale_diagnostics.size(), 2U);

  snapshot.operations.clear();
  const auto component = resolve_stop_scope(component_stop(), snapshot);
  ASSERT_TRUE(component.ok());
  EXPECT_EQ(component.value().disposition, StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
  EXPECT_FALSE(component.value().full_stop);

  const auto resource = resolve_stop_scope(resource_stop(), snapshot);
  ASSERT_TRUE(resource.ok());
  EXPECT_EQ(resource.value().disposition, StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
  EXPECT_FALSE(resource.value().full_stop);
}

TEST(StopScopeResolverTest, InternalProgramRunSelectorCapturesOnlyCurrentRun) {
  const auto snapshot = independent_snapshot();
  const auto selected = resolve_stop_scope(program_run_stop(), snapshot);
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  EXPECT_EQ(selected.value().disposition, StopResolutionDispositionDTO::kExact);
  EXPECT_FALSE(selected.value().full_stop);
  EXPECT_EQ(selected.value().operations, std::vector<OperationId>{OperationId("operation.a")});

  const auto unknown = resolve_stop_scope(program_run_stop("program-run.unknown"), snapshot);
  ASSERT_TRUE(unknown.ok()) << unknown.error().message;
  EXPECT_EQ(unknown.value().disposition, StopResolutionDispositionDTO::kPromotedToFull);
  EXPECT_EQ(unknown.value().reason, StopScopeResolutionReasonDTO::kUnknownProgramRun);

  auto idle = snapshot;
  idle.operations.clear();
  idle.known_program_runs = {ProgramRunId("program-run.a")};
  const auto known_idle = resolve_stop_scope(program_run_stop(), idle);
  ASSERT_TRUE(known_idle.ok()) << known_idle.error().message;
  EXPECT_EQ(known_idle.value().disposition, StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
  EXPECT_FALSE(known_idle.value().full_stop);
  EXPECT_TRUE(known_idle.value().operations.empty());
}

TEST(StopScopeResolverTest, UnknownOrMalformedOrdinaryScopePromotesToCurrentFullCapture) {
  const auto snapshot = independent_snapshot();
  const auto unknown = resolve_stop_scope(
      operation_stop(
          {{OperationId("operation.unknown"), OperationRevision(1U), StopGeneration(0U)}}),
      snapshot);
  ASSERT_TRUE(unknown.ok());
  EXPECT_EQ(unknown.value().disposition, StopResolutionDispositionDTO::kPromotedToFull);
  EXPECT_EQ(unknown.value().reason, StopScopeResolutionReasonDTO::kUnknownOperation);
  EXPECT_TRUE(unknown.value().full_stop);
  EXPECT_EQ(unknown.value().operations,
            (std::vector<OperationId>{OperationId("operation.a"), OperationId("operation.b")}));

  auto malformed = operation_stop();
  malformed.selector = std::vector<StopOperationTargetDTO>{};
  const auto promoted = resolve_stop_scope(malformed, snapshot);
  ASSERT_TRUE(promoted.ok());
  EXPECT_EQ(promoted.value().disposition, StopResolutionDispositionDTO::kPromotedToFull);
  EXPECT_EQ(promoted.value().reason, StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent);
  EXPECT_EQ(promoted.value().operations, unknown.value().operations);

  const auto unknown_component = resolve_stop_scope(component_stop("robot.unknown"), snapshot);
  ASSERT_TRUE(unknown_component.ok());
  EXPECT_EQ(unknown_component.value().reason, StopScopeResolutionReasonDTO::kUnknownComponent);
  EXPECT_EQ(unknown_component.value().operations, unknown.value().operations);

  const auto unknown_resource =
      resolve_stop_scope(resource_stop("robot.unknown.joint.1"), snapshot);
  ASSERT_TRUE(unknown_resource.ok());
  EXPECT_EQ(unknown_resource.value().reason, StopScopeResolutionReasonDTO::kUnknownResource);
  EXPECT_EQ(unknown_resource.value().operations, unknown.value().operations);
}

TEST(StopScopeResolverTest, InvalidOptionalReasonCannotBroadenOtherwiseValidScope) {
  auto intent = operation_stop();
  intent.reason = std::string(kMaximumStopReasonBytes + 1U, 'x');
  const auto result = resolve_stop_scope(intent, independent_snapshot());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().disposition, StopResolutionDispositionDTO::kExact);
  EXPECT_FALSE(result.value().full_stop);
  EXPECT_EQ(result.value().operations, std::vector<OperationId>{OperationId("operation.a")});

  intent.selector = std::vector<StopOperationTargetDTO>{};
  const auto malformed = resolve_stop_scope(intent, independent_snapshot());
  ASSERT_TRUE(malformed.ok());
  EXPECT_EQ(malformed.value().disposition, StopResolutionDispositionDTO::kPromotedToFull);
}

TEST(StopScopeResolverTest, RejectsInvalidOrAmbiguousOwnerSnapshotsWithoutResolution) {
  auto incomplete = independent_snapshot();
  incomplete.operations[0].scope_complete = false;
  incomplete.operations[0].controllable_components.clear();
  incomplete.operations[0].resources.clear();
  const auto result = resolve_stop_scope(operation_stop(), incomplete);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "invariant_violation");
  EXPECT_EQ(result.error().details.at("field"), "stop_scope_effectful_operation_scope_incomplete");

  auto effect_uncertain_terminal = independent_snapshot();
  effect_uncertain_terminal.operations[0].state = OperationStateKind::kFailed;
  effect_uncertain_terminal.operations[0].effect_uncertain = true;
  effect_uncertain_terminal.operations[0].scope_complete = false;
  effect_uncertain_terminal.operations[0].controllable_components.clear();
  effect_uncertain_terminal.operations[0].resources.clear();
  EXPECT_EQ(stop_scope_snapshot_field_error(effect_uncertain_terminal),
            "stop_scope_effectful_operation_scope_incomplete");

  auto ambiguous = independent_snapshot();
  ambiguous.components[1].resources = {ResourceId("robot.a.joint.1"),
                                       ResourceId("robot.b.joint.1")};
  EXPECT_EQ(stop_scope_snapshot_field_error(ambiguous), "stop_scope_resource_owner_ambiguous");

  auto unsorted = independent_snapshot();
  std::swap(unsorted.operations[0], unsorted.operations[1]);
  EXPECT_EQ(stop_scope_snapshot_field_error(unsorted), "stop_scope_operations_not_canonical");
}

TEST(StopScopeResolverTest, MixedLiveAndTerminalSelectionCapturesOnlyCurrentEffectfulWork) {
  auto snapshot = independent_snapshot();
  snapshot.operations[1].state = OperationStateKind::kStopped;
  const auto result = resolve_stop_scope(
      operation_stop({{OperationId("operation.a"), OperationRevision(3U), StopGeneration(4U)},
                      {OperationId("operation.b"), OperationRevision(5U), StopGeneration(6U)}}),
      snapshot);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().disposition, StopResolutionDispositionDTO::kExact);
  EXPECT_EQ(result.value().operations, std::vector<OperationId>{OperationId("operation.a")});
  EXPECT_EQ(result.value().resources, std::vector<ResourceId>{ResourceId("robot.a.joint.1")});
}

TEST(StopScopeResolverTest, TerminalButEffectUncertainOperationRemainsInContainmentScope) {
  auto snapshot = independent_snapshot();
  snapshot.operations[0].state = OperationStateKind::kFailed;
  snapshot.operations[0].effect_uncertain = true;
  const auto result = resolve_stop_scope(operation_stop(), snapshot);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().disposition, StopResolutionDispositionDTO::kExact);
  EXPECT_EQ(result.value().operations, std::vector<OperationId>{OperationId("operation.a")});
  EXPECT_EQ(result.value().resources, std::vector<ResourceId>{ResourceId("robot.a.joint.1")});
}

TEST(StopScopeResolverTest, FullStopCapturesLiveAndEffectUncertainButNotCleanTerminalScope) {
  auto snapshot = independent_snapshot();
  snapshot.operations[1].state = OperationStateKind::kCompleted;
  snapshot.resources.insert(
      snapshot.resources.begin(),
      StopScopeResourceSnapshotDTO{ResourceId("orphan.resource"), StopGeneration(8U), true});
  const FullStopIntentDTO intent{request_context(), StopGeneration(7U), {}};
  const auto result = resolve_full_stop_scope(intent, snapshot);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().disposition, StopResolutionDispositionDTO::kExact);
  EXPECT_TRUE(result.value().full_stop);
  EXPECT_EQ(result.value().operations, std::vector<OperationId>{OperationId("operation.a")});
  EXPECT_EQ(result.value().resources, (std::vector<ResourceId>{ResourceId("orphan.resource"),
                                                               ResourceId("robot.a.joint.1")}));
}

TEST(StopScopeResolverTest, FullStopRetainsOwnerAndCellGenerationMismatches) {
  const FullStopIntentDTO intent{request_context("kernel.epoch.old"), StopGeneration(6U), {}};
  const auto result = resolve_full_stop_scope(intent, independent_snapshot());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().disposition,
            StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved);
  ASSERT_EQ(result.value().stale_diagnostics.size(), 2U);
  EXPECT_EQ(result.value().stale_diagnostics[0].field, StopScopeStaleFieldDTO::kRequestOwnerEpoch);
  EXPECT_EQ(result.value().stale_diagnostics[1].field, StopScopeStaleFieldDTO::kCellStopGeneration);
}

TEST(StopScopeResolverTest, AcceptsMaximumCanonicalIdleComponentSelector) {
  auto snapshot = independent_snapshot();
  snapshot.operations.clear();
  snapshot.components.clear();
  snapshot.resources.clear();
  std::vector<StopComponentTargetDTO> targets;
  snapshot.components.reserve(kMaximumStopSelectorTargets);
  targets.reserve(kMaximumStopSelectorTargets);
  for (std::size_t index = 0U; index < kMaximumStopSelectorTargets; ++index) {
    auto suffix = std::to_string(index);
    suffix.insert(0U, 4U - suffix.size(), '0');
    const auto id = "robot." + suffix;
    snapshot.components.push_back(StopScopeComponentSnapshotDTO{
        ControllableComponentId(id), ComponentLifecycleGeneration(1U), {}});
    targets.push_back(StopComponentTargetDTO{ControllableComponentId(id),
                                             GraphRevision("graph.current"),
                                             ComponentLifecycleGeneration(1U)});
  }
  const StopIntentDTO intent{request_context(), std::move(targets), std::nullopt, {}};
  const auto result = resolve_stop_scope(intent, snapshot);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().disposition, StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
  EXPECT_TRUE(result.value().controllable_components.empty());
}

TEST(StopScopeResolverTest, ComputesCanonicalSelectorAndCapturedScopeDigests) {
  const auto selector = ComputeRequestedStopSelectorDigest(operation_stop());
  ASSERT_TRUE(selector.ok()) << selector.error().message;
  EXPECT_EQ(
      selector.value(),
      ContentDigest("sha256:4f531e68584699956092df33fe8efe137d6af5b134c11c0f6b626bff4fc1ebe9"));
  EXPECT_EQ(selector.value(), ComputeRequestedStopSelectorDigest(operation_stop()).value());
  EXPECT_NE(selector.value(), ComputeRequestedStopSelectorDigest(component_stop()).value());
  EXPECT_NE(selector.value(), ComputeRequestedStopSelectorDigest(
                                  FullStopIntentDTO{request_context(), StopGeneration(4U), {}})
                                  .value());

  const auto resolution = resolve_stop_scope(operation_stop(), independent_snapshot());
  ASSERT_TRUE(resolution.ok());
  const auto captured = ComputeCapturedStopScopeDigest(resolution.value());
  ASSERT_TRUE(captured.ok()) << captured.error().message;
  EXPECT_EQ(
      captured.value(),
      ContentDigest("sha256:68aa87e2bbfae4941093e45b07718363ecd4060aff2c7e0a0111125abefb8866"));
  auto diagnostic_only = resolution.value();
  diagnostic_only.stale_diagnostics.push_back(StopScopeStaleDiagnosticDTO{
      StopScopeStaleFieldDTO::kOperationRevision, OperationId("operation.a"), OperationRevision(2U),
      OperationRevision(3U)});
  EXPECT_EQ(captured.value(), ComputeCapturedStopScopeDigest(diagnostic_only).value());
  diagnostic_only.full_stop = true;
  diagnostic_only.disposition = StopResolutionDispositionDTO::kPromotedToFull;
  diagnostic_only.reason = StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent;
  EXPECT_NE(captured.value(), ComputeCapturedStopScopeDigest(diagnostic_only).value());
}

TEST(StopScopeResolverTest, DigestHelpersRejectNoncanonicalOrUnsafeInputs) {
  auto unsorted =
      operation_stop({{OperationId("operation.b"), OperationRevision(1U), StopGeneration(0U)},
                      {OperationId("operation.a"), OperationRevision(1U), StopGeneration(0U)}});
  EXPECT_FALSE(ComputeRequestedStopSelectorDigest(unsorted).ok());

  auto unsafe = operation_stop();
  std::get<std::vector<StopOperationTargetDTO>>(unsafe.selector)[0].expected_operation_revision =
      OperationRevision(9'007'199'254'740'992ULL);
  EXPECT_FALSE(ComputeRequestedStopSelectorDigest(unsafe).ok());

  auto resolution = resolve_stop_scope(operation_stop(), independent_snapshot()).value();
  resolution.operations.push_back(OperationId("operation.a"));
  EXPECT_FALSE(ComputeCapturedStopScopeDigest(resolution).ok());
}

TEST(StopScopeResolverTest, ReserveOwnedPromotionReasonsRequirePromotedFullScope) {
  const auto base = resolve_stop_scope(operation_stop(), independent_snapshot()).value();
  for (const auto reason : {StopScopeResolutionReasonDTO::kRequestIdentityConflict,
                            StopScopeResolutionReasonDTO::kOverlappingStopTransactions,
                            StopScopeResolutionReasonDTO::kEmergencyReserveCoalesced}) {
    auto promoted = base;
    promoted.disposition = StopResolutionDispositionDTO::kPromotedToFull;
    promoted.reason = reason;
    promoted.full_stop = true;
    EXPECT_TRUE(ComputeCapturedStopScopeDigest(promoted).ok());

    auto scoped = promoted;
    scoped.full_stop = false;
    EXPECT_FALSE(ComputeCapturedStopScopeDigest(scoped).ok());

    auto exact = promoted;
    exact.disposition = StopResolutionDispositionDTO::kExact;
    EXPECT_FALSE(ComputeCapturedStopScopeDigest(exact).ok());
  }
}

TEST(StopScopeResolverTest, NormalizedIntentDigestClosesReplayFieldsButExcludesAttribution) {
  const auto intent = operation_stop();
  const auto digest = ComputeNormalizedStopIntentDigest(intent);
  ASSERT_TRUE(digest.ok()) << digest.error().message;
  EXPECT_EQ(digest.value(), ComputeNormalizedStopIntentDigest(intent).value());

  auto changed_source = intent;
  changed_source.request_context.source_label = "different-source";
  EXPECT_NE(digest.value(), ComputeNormalizedStopIntentDigest(changed_source).value());
  auto changed_reason = intent;
  changed_reason.reason = "operator-requested";
  EXPECT_NE(digest.value(), ComputeNormalizedStopIntentDigest(changed_reason).value());
  auto changed_attribution = intent;
  changed_attribution.attribution.transport = RequestTransportDTO::kHttp;
  EXPECT_EQ(digest.value(), ComputeNormalizedStopIntentDigest(changed_attribution).value());

  const FullStopIntentDTO full{request_context(), StopGeneration(4U), {}};
  ASSERT_TRUE(ComputeNormalizedStopIntentDigest(full).ok());
  auto stale_full = full;
  stale_full.expected_stop_generation = StopGeneration(3U);
  EXPECT_NE(ComputeNormalizedStopIntentDigest(full).value(),
            ComputeNormalizedStopIntentDigest(stale_full).value());
}

TEST(StopScopeResolverTest, MakesCanonicalParticipantQualifiedAcceptance) {
  const auto intent = operation_stop();
  auto resolution = resolve_stop_scope(intent, independent_snapshot());
  ASSERT_TRUE(resolution.ok());
  auto second_plan = prepared_plan("participant.b");
  second_plan.deadline_budget.phases = {{StopBudgetPhaseId("brake"), 300},
                                        {StopBudgetPhaseId("rest"), 700}};
  const std::vector plans{prepared_plan("participant.a"), std::move(second_plan)};
  const auto accepted =
      MakeStopAccepted(intent, StopId("stop.8"), StopGeneration(8U), resolution.value(),
                       {{OperationId("operation.a"), OperationRevision(4U)}}, fixed_budget(), 800,
                       8'000'000'000LL, plans);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_EQ(accepted.value().requested_selector_digest,
            ComputeRequestedStopSelectorDigest(intent).value());
  EXPECT_EQ(accepted.value().captured_scope_digest,
            ComputeCapturedStopScopeDigest(resolution.value()).value());
  ASSERT_EQ(accepted.value().participant_budget_allocations.size(), 2U);
  EXPECT_EQ(accepted.value().participant_budget_allocations[0].participant_id,
            OperationParticipantId("participant.a"));
  EXPECT_EQ(accepted.value().participant_budget_allocations[1].participant_id,
            OperationParticipantId("participant.b"));
  EXPECT_EQ(accepted.value().participant_budget_allocations[0].phases[0],
            (StopAcceptedPhaseBudgetDTO{StopBudgetPhaseId("command"), 250, 50}));
  EXPECT_EQ(accepted.value().participant_budget_allocations[1].phases[0],
            (StopAcceptedPhaseBudgetDTO{StopBudgetPhaseId("brake"), 300, 100}));
  EXPECT_EQ(accepted.value().participant_budget_allocations[1].phases[1],
            (StopAcceptedPhaseBudgetDTO{StopBudgetPhaseId("rest"), 700, 500}));
}

TEST(StopScopeResolverTest, MakesNoEffectAcceptanceWithoutInventedParticipantPlan) {
  auto snapshot = independent_snapshot();
  snapshot.operations.front().state = OperationStateKind::kCompleted;
  const auto intent = operation_stop();
  auto resolution = resolve_stop_scope(intent, snapshot);
  ASSERT_TRUE(resolution.ok());
  ASSERT_EQ(resolution.value().disposition, StopResolutionDispositionDTO::kAlreadyTerminalNoEffect);
  const auto accepted =
      MakeStopAccepted(intent, StopId("stop.8"), StopGeneration(8U), resolution.value(), {},
                       fixed_budget(), 1'000, 8'000'000'000LL, {});
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  EXPECT_TRUE(accepted.value().affected_operation_revisions.empty());
  EXPECT_TRUE(accepted.value().participant_budget_allocations.empty());
  EXPECT_EQ(accepted.value().accepted_at_ns, 8'000'000'000LL);
}

TEST(StopScopeResolverTest, AggregatesPredecessorsWithoutRebasingOrWideningTheirTruth) {
  const auto prior_intent = operation_stop();
  const auto prior_resolution = resolve_stop_scope(prior_intent, independent_snapshot());
  ASSERT_TRUE(prior_resolution.ok());
  const auto prior =
      MakeStopAccepted(prior_intent, StopId("stop.7"), StopGeneration(7U), prior_resolution.value(),
                       {{OperationId("operation.a"), OperationRevision(4U)}}, fixed_budget(), 800,
                       100, std::vector{prepared_plan("participant.a")});
  ASSERT_TRUE(prior.ok()) << prior.error().message;

  auto new_plan = prepared_plan("participant.b");
  new_plan.operation_id = OperationId("operation.b");
  new_plan.operation_revision = OperationRevision(4U);
  StopScopeResolutionDTO aggregate_resolution{
      StopResolutionDispositionDTO::kPromotedToFull,
      StopScopeResolutionReasonDTO::kOverlappingStopTransactions,
      true,
      {OperationId("operation.a"), OperationId("operation.b")},
      {},
      {},
      {}};
  const FullStopIntentDTO full{request_context(), StopGeneration(7U), {}};
  const std::vector predecessors{StopAcceptedPredecessorInput{prior.value(), 600}};
  const auto aggregate = MakeAggregatedStopAccepted(
      full, StopId("stop.8"), StopGeneration(8U), aggregate_resolution,
      {{OperationId("operation.b"), OperationRevision(5U)}}, fixed_budget(), 900, 200,
      std::span<const PreparedParticipantStopPlanDTO>(&new_plan, 1U), predecessors);
  ASSERT_TRUE(aggregate.ok()) << aggregate.error().message;
  ASSERT_EQ(aggregate.value().joined_predecessor_stops.size(), 1U);
  EXPECT_EQ(aggregate.value().joined_predecessor_stops.front().predecessor_stop_id,
            StopId("stop.7"));
  EXPECT_EQ(aggregate.value().remaining_outer_duration_ns, 600);
  ASSERT_EQ(aggregate.value().affected_operation_revisions.size(), 2U);
  EXPECT_EQ(aggregate.value().affected_operation_revisions.front(),
            prior.value().affected_operation_revisions.front());
  ASSERT_EQ(aggregate.value().participant_budget_allocations.size(), 2U);
  EXPECT_EQ(aggregate.value().participant_budget_allocations.front().origin_stop_id,
            StopId("stop.7"));
  EXPECT_EQ(aggregate.value().participant_budget_allocations.back().origin_stop_id,
            StopId("stop.8"));
  EXPECT_EQ(aggregate.value()
                .participant_budget_allocations.front()
                .phases.front()
                .remaining_deadline_offset_ns,
            0);
  EXPECT_EQ(stop_accepted_field_error(aggregate.value()), std::nullopt);

  auto widened = aggregate.value();
  widened.remaining_outer_duration_ns = 700;
  const auto elapsed = widened.fixed_outer_duration_ns - widened.remaining_outer_duration_ns;
  for (auto& participant : widened.participant_budget_allocations) {
    for (auto& budget : participant.phases) {
      budget.remaining_deadline_offset_ns =
          widened.remaining_outer_duration_ns <= widened.fixed_kernel_completion_reserve_ns
              ? 0
              : std::max<std::int64_t>(0, std::min(budget.fixed_deadline_offset_ns - elapsed,
                                                   widened.remaining_outer_duration_ns -
                                                       widened.fixed_kernel_completion_reserve_ns));
    }
  }
  EXPECT_EQ(stop_accepted_field_error(widened), "stop_accepted_predecessor_deadline_widened");
  auto wrong_origin = aggregate.value();
  wrong_origin.participant_budget_allocations.front().origin_stop_id = StopId("stop.missing");
  EXPECT_EQ(stop_accepted_field_error(wrong_origin), "stop_accepted_participant_budget_invalid");

  auto second_resolution = prior_resolution.value();
  second_resolution.operations = {OperationId("operation.b")};
  second_resolution.resources = {ResourceId("robot.b")};
  auto second_plan = prepared_plan("participant.b");
  second_plan.operation_id = OperationId("operation.b");
  const auto second_prior =
      MakeStopAccepted(prior_intent, StopId("stop.6"), StopGeneration(6U), second_resolution,
                       {{OperationId("operation.b"), OperationRevision(4U)}}, fixed_budget(), 700,
                       100, std::span<const PreparedParticipantStopPlanDTO>(&second_plan, 1U));
  ASSERT_TRUE(second_prior.ok()) << second_prior.error().message;
  const std::vector two_predecessors{StopAcceptedPredecessorInput{second_prior.value(), 500},
                                     StopAcceptedPredecessorInput{prior.value(), 600}};
  const auto predecessor_only =
      MakeAggregatedStopAccepted(full, StopId("stop.9"), StopGeneration(9U), aggregate_resolution,
                                 {}, fixed_budget(), 900, 200, {}, two_predecessors);
  ASSERT_TRUE(predecessor_only.ok())
      << predecessor_only.error().message << ": " << predecessor_only.error().details.at("reason");
  EXPECT_EQ(predecessor_only.value().joined_predecessor_stops.size(), 2U);
  EXPECT_EQ(predecessor_only.value().remaining_outer_duration_ns, 500);
}

TEST(StopScopeResolverTest, RetainsDispatchPlanAcrossControlRevisionsAndRejectsMismatch) {
  const auto intent = operation_stop();
  const auto resolution = resolve_stop_scope(intent, independent_snapshot());
  ASSERT_TRUE(resolution.ok());
  auto control_advanced_plan = prepared_plan();
  control_advanced_plan.operation_revision = OperationRevision(2U);
  auto accepted = MakeStopAccepted(
      intent, StopId("stop.8"), StopGeneration(8U), resolution.value(),
      {{OperationId("operation.a"), OperationRevision(4U)}}, fixed_budget(), 800, 100,
      std::span<const PreparedParticipantStopPlanDTO>(&control_advanced_plan, 1U));
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;

  auto wrong_revision = prepared_plan();
  wrong_revision.operation_revision = OperationRevision(4U);
  accepted =
      MakeStopAccepted(intent, StopId("stop.8"), StopGeneration(8U), resolution.value(),
                       {{OperationId("operation.a"), OperationRevision(4U)}}, fixed_budget(), 800,
                       100, std::span<const PreparedParticipantStopPlanDTO>(&wrong_revision, 1U));
  ASSERT_FALSE(accepted.ok());
  EXPECT_EQ(accepted.error().details.at("reason"), "stop_acceptance_participant_plan_mismatch");

  auto wrong_budget = fixed_budget();
  wrong_budget.kernel_completion_reserve_ns = wrong_budget.outer_duration_ns;
  accepted = MakeStopAccepted(intent, StopId("stop.8"), StopGeneration(8U), resolution.value(),
                              {{OperationId("operation.a"), OperationRevision(4U)}}, wrong_budget,
                              800, 100, {});
  ASSERT_FALSE(accepted.ok());
  EXPECT_EQ(accepted.error().details.at("reason"), "stop_acceptance_budget_invalid");

  auto mismatched_plan_budget = prepared_plan();
  mismatched_plan_budget.deadline_budget.outer_duration_ns += 1;
  accepted = MakeStopAccepted(
      intent, StopId("stop.8"), StopGeneration(8U), resolution.value(),
      {{OperationId("operation.a"), OperationRevision(4U)}}, fixed_budget(), 800, 100,
      std::span<const PreparedParticipantStopPlanDTO>(&mismatched_plan_budget, 1U));
  ASSERT_FALSE(accepted.ok());
  EXPECT_EQ(accepted.error().details.at("reason"), "stop_acceptance_participant_plan_mismatch");

  auto exhausted_revision = prepared_plan();
  exhausted_revision.operation_revision = OperationRevision(9'007'199'254'740'991ULL);
  accepted = MakeStopAccepted(
      intent, StopId("stop.8"), StopGeneration(8U), resolution.value(),
      {{OperationId("operation.a"), OperationRevision(9'007'199'254'740'991ULL)}}, fixed_budget(),
      800, 100, std::span<const PreparedParticipantStopPlanDTO>(&exhausted_revision, 1U));
  ASSERT_FALSE(accepted.ok());
  EXPECT_EQ(accepted.error().details.at("reason"), "stop_acceptance_participant_plan_mismatch");
}

TEST(StopScopeResolverTest, ValidatesImmutableAcceptedResultShape) {
  auto accepted = accepted_stop();
  EXPECT_EQ(stop_accepted_field_error(accepted), std::nullopt);

  accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
  EXPECT_EQ(stop_accepted_field_error(accepted), std::nullopt);

  accepted.resolution_disposition = StopResolutionDispositionDTO::kAlreadyTerminalNoEffect;
  accepted.resolution_reason = StopScopeResolutionReasonDTO::kKnownTerminalOrIdleNoEffect;
  accepted.affected_operation_revisions.clear();
  accepted.participant_budget_allocations.clear();
  EXPECT_EQ(stop_accepted_field_error(accepted), std::nullopt);
}

TEST(StopScopeResolverTest, RejectsMixedOrNoncanonicalAcceptedResultEvidence) {
  auto accepted = accepted_stop();
  accepted.affected_operation_revisions.push_back(
      {OperationId("operation.a"), OperationRevision(5U)});
  EXPECT_EQ(stop_accepted_field_error(accepted), "stop_accepted_operation_revisions_invalid");

  accepted = accepted_stop();
  accepted.resolution_disposition = StopResolutionDispositionDTO::kExact;
  accepted.resolution_reason = StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope;
  EXPECT_EQ(stop_accepted_field_error(accepted), "stop_accepted_resolution_invalid");

  accepted = accepted_stop();
  accepted.requested_selector_digest = ContentDigest("sha256:not-a-digest");
  EXPECT_EQ(stop_accepted_field_error(accepted), "stop_accepted_digest_invalid");

  accepted = accepted_stop();
  accepted.participant_budget_allocations[0].phases[1].remaining_deadline_offset_ns = 900;
  EXPECT_EQ(stop_accepted_field_error(accepted), "stop_accepted_phase_budget_invalid");

  accepted = accepted_stop();
  accepted.participant_budget_allocations[0].participant_stop_invocation_plan_digest =
      ContentDigest("sha256:not-a-digest");
  EXPECT_EQ(stop_accepted_field_error(accepted), "stop_accepted_participant_budget_invalid");

  accepted = accepted_stop();
  accepted.participant_budget_allocations.clear();
  EXPECT_EQ(stop_accepted_field_error(accepted),
            "stop_accepted_participant_budget_coverage_invalid");

  accepted = accepted_stop();
  accepted.resolution_disposition = StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved;
  accepted.resolution_reason = StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope;
  EXPECT_EQ(stop_accepted_field_error(accepted),
            "stop_accepted_stale_diagnostics_presence_invalid");
  accepted.stale_diagnostics.push_back(StopScopeStaleDiagnosticDTO{
      StopScopeStaleFieldDTO::kOperationRevision, ResourceId("resource.wrong-kind"),
      OperationRevision(2U), OperationRevision(3U)});
  EXPECT_EQ(stop_accepted_field_error(accepted), "stop_accepted_stale_diagnostics_invalid");
}

}  // namespace
}  // namespace manyforge::core
