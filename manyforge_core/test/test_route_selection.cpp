#include <gtest/gtest.h>

#include <manyforge_core/controller_lifecycle_evaluator.hpp>
#include <manyforge_core/ports/icclc_kernel_route_configuration_fence_port.hpp>
#include <manyforge_core/ports/icclc_route_configuration_publication_port.hpp>
#include <manyforge_core/ports/icclc_route_selection_current_snapshot_source.hpp>
#include <manyforge_core/route_selection.hpp>
#include <manyforge_core/route_selection_owner.hpp>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

using ObserveRouteFenceAdmissionSignature =
    Result<RouteConfigurationFenceAdmissionSnapshotDTO> (ICCLCKernelRouteConfigurationFencePort::*)(
        const RouteConfigurationFenceAdmissionQueryDTO&) const;
using AcquireRouteFenceSignature = Result<RouteConfigurationFenceAcceptedDTO> (
    ICCLCKernelRouteConfigurationFencePort::*)(const RouteConfigurationFenceIntentDTO&);
using ReconcileRouteFenceAcquireSignature =
    Result<RouteConfigurationFenceAcquireAttemptReconciliationDTO> (
        ICCLCKernelRouteConfigurationFencePort::*)(const RouteConfigurationFenceIntentDTO&) const;
using ObserveRouteFenceStatusSignature = Result<RouteConfigurationFenceStatusDTO> (
    ICCLCKernelRouteConfigurationFencePort::*)(const RouteConfigurationFenceStatusQueryDTO&) const;
using ReleaseRouteFenceSignature = Result<RouteConfigurationFenceReleasedDTO> (
    ICCLCKernelRouteConfigurationFencePort::*)(const RouteConfigurationFenceReleaseRequestDTO&);
using ReconcileRouteFenceReleaseSignature =
    Result<RouteConfigurationFenceReleaseAttemptReconciliationDTO> (
        ICCLCKernelRouteConfigurationFencePort::*)(
        const RouteConfigurationFenceReleaseRequestDTO&) const;

static_assert(
    std::is_same_v<
        decltype(&ICCLCKernelRouteConfigurationFencePort::ObserveRouteConfigurationFenceAdmission),
        ObserveRouteFenceAdmissionSignature>);
static_assert(std::is_same_v<
              decltype(&ICCLCKernelRouteConfigurationFencePort::AcquireRouteConfigurationFence),
              AcquireRouteFenceSignature>);
static_assert(std::is_same_v<decltype(&ICCLCKernelRouteConfigurationFencePort::
                                          ReconcileRouteConfigurationFenceAcquireAttempt),
                             ReconcileRouteFenceAcquireSignature>);
static_assert(
    std::is_same_v<
        decltype(&ICCLCKernelRouteConfigurationFencePort::ObserveRouteConfigurationFenceStatus),
        ObserveRouteFenceStatusSignature>);
static_assert(std::is_same_v<
              decltype(&ICCLCKernelRouteConfigurationFencePort::ReleaseRouteConfigurationFence),
              ReleaseRouteFenceSignature>);
static_assert(std::is_same_v<decltype(&ICCLCKernelRouteConfigurationFencePort::
                                          ReconcileRouteConfigurationFenceReleaseAttempt),
                             ReconcileRouteFenceReleaseSignature>);

template <typename T>
concept ExposesRouteFenceStop = requires(T& value) { value.Stop(); };

static_assert(!ExposesRouteFenceStop<ICCLCKernelRouteConfigurationFencePort>);

[[nodiscard]] std::string Sha(char value) {
  return std::string("sha256:") + std::string(64U, value);
}

[[nodiscard]] ControllerTopologyDTO Topology() {
  const ResourceInterfaceClaimDTO position{ResourceRoleId("arm_joints"), "position"};
  return {ControllerTopologyId("topology.arm"),
          TopologyRevision(Sha('1')),
          {{ControllerRoleId("hardware"), {}, {position}, {}}},
          {},
          {{ActivationSetId("activation.direct"), {ControllerRoleId("hardware")}, {}},
           {ActivationSetId("activation.legacy"), {ControllerRoleId("hardware")}, {}}},
          {{ControlRouteId("route.direct"),
            ControlInterfaceContractId("contract.position"),
            ControlInterfaceRevision(Sha('2')),
            {ControllerRoleId("hardware")},
            {ActivationSetId("activation.direct")}},
           {ControlRouteId("route.legacy"),
            ControlInterfaceContractId("contract.position"),
            ControlInterfaceRevision(Sha('2')),
            {ControllerRoleId("hardware")},
            {ActivationSetId("activation.legacy")}}}};
}

[[nodiscard]] DesiredControlConfigurationDTO Desired() {
  return {ControlDomainId("domain.arm"),
          ControlDomainRevision(Sha('3')),
          ControllerTopologyId("topology.arm"),
          TopologyRevision(Sha('1')),
          ActivationSetId("activation.direct"),
          {{ControlBindingId("binding.a"), ControlBindingRevision(Sha('4')),
            ControlRouteId("route.direct")},
           {ControlBindingId("binding.b"), ControlBindingRevision(Sha('5')),
            ControlRouteId("route.direct")}}};
}

[[nodiscard]] ControllerLifecycleObservedConfigurationDTO ObservedConfiguration(
    const DesiredControlConfigurationDTO& desired) {
  std::vector<ActiveControlConfigurationBindingRouteDTO> routes;
  routes.reserve(desired.binding_routes.size());
  for (const auto& route : desired.binding_routes) {
    routes.push_back(
        {route.control_binding_id, route.control_binding_revision, route.control_route_id});
  }
  return {desired.activation_set_id, std::move(routes)};
}

[[nodiscard]] ControllerLifecycleExpectedDomainDTO ExpectedDomain() {
  const auto desired = Desired();
  std::vector<ControllerLifecycleExpectedEndpointDTO> endpoints;
  for (const auto& route : desired.binding_routes) {
    for (const auto& route_id : {ControlRouteId("route.direct"), ControlRouteId("route.legacy")}) {
      endpoints.push_back({route.control_binding_id, route.control_binding_revision, route_id,
                           EndpointRoleId("endpoint.command"), "/cell/arm/command", true, false});
    }
  }
  return {desired.control_domain_id,
          desired.control_domain_revision,
          desired.controller_topology_id,
          desired.topology_revision,
          ProductId("product.control.arm"),
          ProductContractId("manyforge.control-domain-control.v2"),
          ProductContractRevision(Sha('6')),
          ContentDigest(Sha('7')),
          ContentDigest(Sha('8')),
          RuntimeUnitId("runtime.controller-manager"),
          RuntimeEpoch("runtime-epoch.1"),
          Topology(),
          desired,
          {{ControlBindingId("binding.a"),
            ControlBindingRevision(Sha('4')),
            ControlInterfaceContractId("contract.position"),
            ControlInterfaceRevision(Sha('2')),
            {{ResourceId("joint.a"), "position"}},
            {{ResourceId("joint.a"), "position"}}},
           {ControlBindingId("binding.b"),
            ControlBindingRevision(Sha('5')),
            ControlInterfaceContractId("contract.position"),
            ControlInterfaceRevision(Sha('2')),
            {{ResourceId("joint.b"), "position"}},
            {{ResourceId("joint.b"), "position"}}}},
          {{ControllerRoleId("hardware"),
            ControllerImplementationId("implementation.hardware"),
            ControllerImplementationRevision(Sha('9')),
            "controller_interface::ControllerInterface",
            ContentDigest(Sha('a')),
            {},
            {},
            {},
            {}}},
          std::move(endpoints)};
}

[[nodiscard]] ControllerLifecycleObservationDTO DesiredObservation() {
  const auto expected = ExpectedDomain();
  std::vector<ControllerLifecycleObservedControllerDTO> controllers;
  for (const auto& controller : expected.expected_controllers) {
    controllers.push_back({controller, ControllerLifecycleActivityDTO::kActive});
  }
  std::vector<ControllerLifecycleObservedEndpointDTO> endpoints;
  for (const auto& endpoint : expected.expected_endpoints) {
    endpoints.push_back({endpoint, true});
  }
  return {MakeControllerLifecycleObservationQuery(expected),
          1U,
          100,
          101,
          1'000,
          ObservedConfiguration(expected.desired_configuration),
          std::move(controllers),
          {},
          std::move(endpoints)};
}

[[nodiscard]] ControllerLifecycleReconciliationDTO DesiredReconciliation() {
  const auto expected = ExpectedDomain();
  return {MakeControllerLifecycleObservationQuery(expected),
          1U,
          ControllerLifecycleReconciliationDispositionDTO::kReconciled,
          expected.desired_configuration.activation_set_id,
          expected.desired_configuration.binding_routes,
          {}};
}

[[nodiscard]] std::vector<RouteSelectionComponentExpectationDTO> Components() {
  return {{ControllableComponentId("component.a"), ComponentLifecycleGeneration(4U),
           ComponentLifecycleStateDTO::kPrepared},
          {ControllableComponentId("component.b"), ComponentLifecycleGeneration(7U),
           ComponentLifecycleStateDTO::kActive}};
}

[[nodiscard]] ActiveControlConfigurationDTO CurrentConfiguration() {
  const auto desired = Desired();
  return {ActiveControlConfigurationId("configuration.current"),
          desired.control_domain_id,
          desired.control_domain_revision,
          {{ControlBindingId("binding.a"), ControlBindingRevision(Sha('4')),
            ControlRouteId("route.legacy")},
           {ControlBindingId("binding.b"), ControlBindingRevision(Sha('5')),
            ControlRouteId("route.legacy")}},
          ActivationSetId("activation.legacy"),
          desired.controller_topology_id,
          desired.topology_revision,
          RuntimeUnitId("runtime.controller-manager"),
          RuntimeEpoch("runtime-epoch.1"),
          8U,
          {},
          ActiveControlConfigurationGeneration(5U),
          ActiveControlConfigurationStateDTO::kReconciled};
}

[[nodiscard]] ExpectedActiveControlConfigurationDTO ExpectedCurrent() {
  return {ExpectedActiveControlConfigurationPresenceDTO::kPresent,
          ActiveControlConfigurationId("configuration.current"),
          ActiveControlConfigurationGeneration(5U)};
}

[[nodiscard]] ConfigurationFenceScopeDTO Scope() {
  const auto desired = Desired();
  return {{ControllableComponentId("component.a"), ControllableComponentId("component.b")},
          {{desired.control_domain_id, desired.control_domain_revision,
            desired.control_domain_revision}},
          {{ControlBindingId("binding.a"), ControlBindingRevision(Sha('4')),
            ControlBindingRevision(Sha('4'))},
           {ControlBindingId("binding.b"), ControlBindingRevision(Sha('5')),
            ControlBindingRevision(Sha('5'))}},
          {ResourceId("joint.a"), ResourceId("joint.b")}};
}

[[nodiscard]] RouteSelectionIntentDTO Intent() {
  return {{RequestId("request.route.1"), RequestOwnerEpoch("cclc-owner.1"), "test", std::nullopt},
          RetainedBundleId("bundle.1"),
          GraphRevision("graph.1"),
          Desired(),
          ExpectedCurrent(),
          Components(),
          {RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified, std::nullopt},
          AdmissionGeneration(1U)};
}

[[nodiscard]] RouteConfigurationFenceAdmissionSnapshotDTO FenceSnapshot(std::uint64_t queued = 0U,
                                                                        std::uint64_t active = 0U) {
  const RouteConfigurationFenceAdmissionQueryDTO query{
      RouteSelectionTransactionId("route-transaction.1"),
      RouteSelectionTransactionGeneration(9U),
      Scope(),
      RetainedBundleId("bundle.1"),
      GraphRevision("graph.1"),
      ExpectedCurrent()};
  return {query,
          {RuntimeUnitId("runtime.kernel"), RuntimeEpoch("kernel-runtime-epoch.1"),
           RequestOwnerEpoch("kernel-owner.1"), ContentDigest(Sha('b')),
           ActiveBundlePublicationGeneration(3U)},
          {RetainedBundleId("bundle.1"),
           GraphRevision("graph.1"),
           {SceneId(Sha('c')), 2U, ContentDigest(Sha('d'))}},
          ActiveBundlePublicationGeneration(3U),
          ContentDigest(Sha('e')),
          11U,
          ConfigurationFenceGeneration(4U),
          ExpectedCurrent(),
          queued,
          active,
          ContentDigest(Sha('f'))};
}

[[nodiscard]] RouteSelectionAdmissionCandidate Candidate(std::uint64_t queued = 0U,
                                                         std::uint64_t active = 0U) {
  return {RouteSelectionTransactionId("route-transaction.1"),
          RouteSelectionTransactionGeneration(9U),
          ActiveControlConfigurationId("configuration.candidate"),
          ActiveControlConfigurationGeneration(6U),
          ActiveControlConfigurationGeneration(5U),
          100,
          10'000,
          RetainedBundleId("bundle.1"),
          GraphRevision("graph.1"),
          Components(),
          CurrentConfiguration(),
          ExpectedDomain(),
          Scope(),
          FenceSnapshot(queued, active)};
}

[[nodiscard]] RouteConfigurationFenceIntentDTO FenceIntent() {
  const auto snapshot = FenceSnapshot();
  return {{RequestId("kernel.acquire.1"), snapshot.execution_host_epoch.request_owner_epoch,
           "cclc-route-selection", std::nullopt},
          RequestId("request.route.1"),
          snapshot.query.route_selection_transaction_id,
          snapshot.query.route_selection_transaction_generation,
          snapshot.query.scope,
          snapshot.snapshot_digest,
          RouteConfigurationFenceBusyDispositionDTO::kRejectIfAffectedBusy,
          snapshot.query.expected_retained_bundle_id,
          snapshot.query.expected_graph_revision,
          snapshot.operation_resource_generation,
          snapshot.configuration_fence_generation,
          snapshot.current_active_control_configuration,
          ActiveControlConfigurationId("configuration.candidate"),
          ActiveControlConfigurationGeneration(6U),
          "complete-domain route selection"};
}

[[nodiscard]] RouteConfigurationFenceAcceptedDTO AcceptedFence() {
  const auto intent = FenceIntent();
  return {intent.owner_request_context.request_id,
          RequestReplayStatusDTO::kAcceptedNew,
          intent.route_selection_transaction_id,
          intent.route_selection_transaction_generation,
          ConfigurationFenceId("route-fence.1"),
          ConfigurationFenceGeneration(5U),
          intent.scope,
          intent.candidate_active_control_configuration_id,
          intent.candidate_active_control_configuration_generation,
          120,
          ContentDigest(Sha('1'))};
}

[[nodiscard]] RouteConfigurationFenceStatusQueryDTO FenceStatusQuery() {
  const auto accepted = AcceptedFence();
  return {accepted.route_selection_transaction_id, accepted.route_selection_transaction_generation,
          accepted.configuration_fence_id, accepted.configuration_fence_generation,
          accepted.captured_scope};
}

[[nodiscard]] RouteConfigurationFenceReconciliationEvidenceDTO HeldEvidence() {
  const auto query = FenceStatusQuery();
  const auto snapshot = FenceSnapshot();
  return {snapshot.execution_host_epoch,
          query.route_selection_transaction_id,
          query.route_selection_transaction_generation,
          query.configuration_fence_id,
          query.configuration_fence_generation,
          query.expected_scope,
          12U,
          RetainedBundleId("bundle.1"),
          GraphRevision("graph.1"),
          snapshot.active_projection_revision,
          snapshot.active_publication_generation,
          snapshot.active_public_pointer_digest,
          ExpectedCurrent(),
          0U,
          0U,
          ContentDigest(Sha('2'))};
}

[[nodiscard]] RouteConfigurationFenceStatusDTO HeldStatus() {
  return {FenceStatusQuery(),
          HeldEvidence(),
          RouteConfigurationFenceStateDTO::kHeld,
          true,
          std::nullopt,
          {},
          ContentDigest(Sha('3')),
          std::nullopt};
}

[[nodiscard]] RouteConfigurationFenceReleaseRequestDTO ReleaseRequest() {
  const auto evidence = HeldEvidence();
  return {{RequestId("kernel.release.1"), evidence.execution_host_epoch.request_owner_epoch,
           "cclc-route-selection", std::nullopt},
          RequestId("request.route.1"),
          evidence.route_selection_transaction_id,
          evidence.route_selection_transaction_generation,
          AcceptedFence(),
          7U,
          ContentDigest(Sha('4')),
          ExpectedCurrent(),
          evidence};
}

[[nodiscard]] RouteConfigurationFenceReleasedDTO ReleasedFence() {
  const auto request = ReleaseRequest();
  return {request,
          RequestReplayStatusDTO::kAcceptedNew,
          200,
          request.accepted_fence.configuration_fence_id,
          request.accepted_fence.configuration_fence_generation,
          ContentDigest(Sha('5')),
          ContentDigest(Sha('6'))};
}

[[nodiscard]] RouteSelectionCurrentSnapshotDTO CurrentSnapshot() {
  return {RetainedBundleId("bundle.1"), GraphRevision("graph.1"), Components(),
          CurrentConfiguration(),       ExpectedDomain(),         Scope()};
}

[[nodiscard]] ActiveControlConfigurationDTO ResultingConfiguration(
    ActiveControlConfigurationStateDTO state = ActiveControlConfigurationStateDTO::kReconciled) {
  const auto desired = Desired();
  return {ActiveControlConfigurationId("configuration.candidate"),
          desired.control_domain_id,
          desired.control_domain_revision,
          {{ControlBindingId("binding.a"), ControlBindingRevision(Sha('4')),
            ControlRouteId("route.direct")},
           {ControlBindingId("binding.b"), ControlBindingRevision(Sha('5')),
            ControlRouteId("route.direct")}},
          desired.activation_set_id,
          desired.controller_topology_id,
          desired.topology_revision,
          RuntimeUnitId("runtime.controller-manager"),
          RuntimeEpoch("runtime-epoch.1"),
          1U,
          {},
          ActiveControlConfigurationGeneration(6U),
          state};
}

[[nodiscard]] RouteSelectionStatusDTO PublishingStatus(
    ActiveControlConfigurationStateDTO state = ActiveControlConfigurationStateDTO::kReconciled) {
  const auto prepared = PrepareRouteSelectionAdmission(Intent(), Candidate());
  EXPECT_TRUE(prepared.ok());
  auto status = prepared.value().initial_status;
  status.status_sequence = 2U;
  status.phase = RouteSelectionTransactionPhaseDTO::kPublishing;
  status.controller_observation = DesiredObservation();
  status.controller_reconciliation = DesiredReconciliation();
  if (state == ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled) {
    status.controller_reconciliation->disposition =
        ControllerLifecycleReconciliationDispositionDTO::kExternalOrUnknownUnreconciled;
    status.controller_reconciliation->reason_codes = {"controller_activity_mismatch"};
    status.effect = RouteSelectionEffectDTO::kPartialEffectObserved;
  }
  status.accepted_fence = AcceptedFence();
  status.resulting_active_control_configuration = ResultingConfiguration(state);
  status.latest_updated_at_ns = 130;
  status.remaining_duration_ns = 9'970;
  return status;
}

[[nodiscard]] RouteReadinessPublicationReceiptDTO ReadinessReceipt(bool open = true) {
  return {RouteSelectionTransactionId("route-transaction.1"),
          RouteSelectionTransactionGeneration(9U),
          ActiveControlConfigurationId("configuration.candidate"),
          ActiveControlConfigurationGeneration(6U),
          {{ControllableComponentId("component.a"), ComponentLifecycleGeneration(4U),
            ControlBindingId("binding.a"), ControlBindingRevision(Sha('4')),
            ResolvedStopBasePlanId(Sha('a'))},
           {ControllableComponentId("component.b"), ComponentLifecycleGeneration(7U),
            ControlBindingId("binding.b"), ControlBindingRevision(Sha('5')),
            ResolvedStopBasePlanId(Sha('b'))}},
          open,
          140,
          ContentDigest(Sha('c'))};
}

TEST(RouteSelectionAdmission, PreparesOneCompleteMultiComponentDomain) {
  const auto intent = Intent();
  ASSERT_TRUE(intent.locally_valid());
  ASSERT_TRUE(Candidate().expected_controller_domain.locally_valid());

  const auto prepared = PrepareRouteSelectionAdmission(intent, Candidate());
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().reserved_control_domains,
            std::vector<ControlDomainId>{ControlDomainId("domain.arm")});
  EXPECT_EQ(prepared.value().acceptance.component_expectations, Components());
  EXPECT_EQ(prepared.value().acceptance.candidate_active_control_configuration_generation,
            ActiveControlConfigurationGeneration(6U));
  EXPECT_EQ(prepared.value().initial_status.phase, RouteSelectionTransactionPhaseDTO::kAccepted);
  EXPECT_EQ(prepared.value().initial_status.affected_control_binding_ids,
            (std::vector<ControlBindingId>{ControlBindingId("binding.a"),
                                           ControlBindingId("binding.b")}));
  EXPECT_EQ(prepared.value().initial_status.affected_controller_role_ids,
            std::vector<ControllerRoleId>{ControllerRoleId("hardware")});
  EXPECT_TRUE(prepared.value().initial_status.locally_valid());
}

TEST(RouteSelectionAdmission, AttributionDoesNotChangeSemanticReplayEquality) {
  auto first = Intent();
  auto second = first;
  second.attribution = {RequestTransportDTO::kHttp, RequestAssuranceDTO::kExternallyAuthenticated,
                        std::string("operator.1")};
  EXPECT_NE(first, second);
  EXPECT_TRUE(first.replay_equal(second));

  second.desired_configuration.activation_set_id = ActivationSetId("activation.other");
  EXPECT_FALSE(first.replay_equal(second));
}

TEST(RouteSelectionAdmission, RejectsPartialStaleOrBusyInputBeforeEffect) {
  auto intent = Intent();
  intent.component_expectations.pop_back();
  auto result = PrepareRouteSelectionAdmission(intent, Candidate());
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "component_expectations_changed");

  auto stale = Candidate();
  stale.current_active_control_configuration->source_runtime_epoch = RuntimeEpoch("epoch.changed");
  result = PrepareRouteSelectionAdmission(Intent(), stale);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(
      result.value().acceptance.predecessor_active_control_configuration->source_runtime_epoch,
      RuntimeEpoch("epoch.changed"));

  auto foreign_source = Candidate();
  foreign_source.current_active_control_configuration->source_runtime_unit_id =
      RuntimeUnitId("runtime.foreign");
  result = PrepareRouteSelectionAdmission(Intent(), foreign_source);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "predecessor_identity_or_runtime_changed");

  auto incomplete = Candidate();
  incomplete.current_active_control_configuration->binding_routes.pop_back();
  result = PrepareRouteSelectionAdmission(Intent(), incomplete);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "predecessor_domain_closure_changed");

  auto unknown_route = Candidate();
  unknown_route.current_active_control_configuration->binding_routes.front().control_route_id =
      ControlRouteId("route.unknown");
  result = PrepareRouteSelectionAdmission(Intent(), unknown_route);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "predecessor_domain_closure_changed");

  auto wrong_watermark = Candidate();
  wrong_watermark.durable_active_configuration_generation_watermark =
      ActiveControlConfigurationGeneration(4U);
  result = PrepareRouteSelectionAdmission(Intent(), wrong_watermark);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "active_configuration_watermark_regressed");

  auto burned_generation = Candidate();
  burned_generation.durable_active_configuration_generation_watermark =
      ActiveControlConfigurationGeneration(8U);
  burned_generation.candidate_active_control_configuration_generation =
      ActiveControlConfigurationGeneration(9U);
  result = PrepareRouteSelectionAdmission(Intent(), burned_generation);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().acceptance.candidate_active_control_configuration_generation,
            ActiveControlConfigurationGeneration(9U));

  result = PrepareRouteSelectionAdmission(Intent(), Candidate(1U, 0U));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "route_selection_scope_busy");
}

TEST(RouteSelectionAdmission, RejectsSingularRouteAndInvalidComponentState) {
  auto partial = Intent();
  partial.desired_configuration.binding_routes.pop_back();
  auto result = PrepareRouteSelectionAdmission(partial, Candidate());
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "desired_domain_or_query_mismatch");

  auto invalid_state = Intent();
  invalid_state.component_expectations.front().expected_component_lifecycle_state =
      ComponentLifecycleStateDTO::kDisabled;
  EXPECT_FALSE(invalid_state.locally_valid());
}

TEST(RouteSelectionFenceContracts, AcquireReleaseAndLostResponseShapesAreClosed) {
  const auto acquire = FenceIntent();
  const auto snapshot = FenceSnapshot();
  ASSERT_TRUE(acquire.matches(snapshot));
  const auto accepted = AcceptedFence();
  ASSERT_TRUE(accepted.matches(acquire));
  const auto held = HeldStatus();
  ASSERT_TRUE(held.locally_valid());

  EXPECT_TRUE((RouteConfigurationFenceAcquireAttemptReconciliationDTO{
                   acquire, RouteConfigurationFenceAcquireAttemptDispositionDTO::kAcceptedExact,
                   accepted, held, std::nullopt})
                  .locally_valid());
  EXPECT_TRUE((RouteConfigurationFenceAcquireAttemptReconciliationDTO{
                   acquire, RouteConfigurationFenceAcquireAttemptDispositionDTO::kNotSeen,
                   std::nullopt, std::nullopt, std::nullopt})
                  .locally_valid());

  const auto release = ReleaseRequest();
  ASSERT_TRUE(release.locally_valid());
  const auto receipt = ReleasedFence();
  ASSERT_TRUE(receipt.locally_valid());
  const RouteConfigurationFenceStatusDTO released{FenceStatusQuery(),
                                                  HeldEvidence(),
                                                  RouteConfigurationFenceStateDTO::kReleased,
                                                  false,
                                                  receipt,
                                                  {},
                                                  ContentDigest(Sha('7')),
                                                  std::nullopt};
  EXPECT_TRUE(released.locally_valid());
  EXPECT_TRUE((RouteConfigurationFenceReleaseAttemptReconciliationDTO{
                   release, RouteConfigurationFenceReleaseAttemptDispositionDTO::kReleasedExact,
                   receipt, released, std::nullopt})
                  .locally_valid());
}

TEST(RouteSelectionEffectContracts, PreparedAndCompletedVariantsCannotCrossKinds) {
  const auto acquire = FenceIntent();
  CCLCRouteEffectAttemptDTO prepared{acquire.route_selection_transaction_id,
                                     acquire.route_selection_transaction_generation,
                                     1U,
                                     CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire,
                                     CCLCRouteEffectAttemptStateDTO::kPrepared,
                                     acquire,
                                     std::nullopt};
  ASSERT_TRUE(prepared.locally_valid());

  prepared.state = CCLCRouteEffectAttemptStateDTO::kCompleted;
  prepared.result = RouteConfigurationFenceAcquireEffectResultDTO{
      acquire, RouteConfigurationFenceAcquireEffectDispositionDTO::kAccepted, AcceptedFence()};
  EXPECT_TRUE(prepared.locally_valid());

  prepared.effect_kind = CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease;
  EXPECT_FALSE(prepared.locally_valid());
}

TEST(RouteSelectionJournalContracts, WatermarksAndReplayPresenceAreClosed) {
  EXPECT_TRUE(
      (CCLCRouteEffectSequenceWatermarkDTO{RouteSelectionTransactionId("route-transaction.1"),
                                           RouteSelectionTransactionGeneration(9U), 0U})
          .locally_valid());
  EXPECT_TRUE((ControlDomainActiveConfigurationGenerationWatermarkDTO{
                   ControlDomainId("domain.arm"), ActiveControlConfigurationGeneration(6U)})
                  .locally_valid());

  const auto prepared = PrepareRouteSelectionAdmission(Intent(), Candidate());
  ASSERT_TRUE(prepared.ok());
  const CCLCRouteSelectionRequestResultDTO accepted{RequestReplayStatusDTO::kAcceptedNew,
                                                    prepared.value().acceptance,
                                                    prepared.value().initial_status};
  EXPECT_TRUE(accepted.locally_valid());

  auto mismatched = accepted;
  mismatched.acceptance->route_selection_transaction_generation =
      RouteSelectionTransactionGeneration(10U);
  EXPECT_FALSE(mismatched.locally_valid());

  EXPECT_TRUE((CCLCRouteSelectionRequestResultDTO{RequestReplayStatusDTO::kRequestIdConflict,
                                                  std::nullopt, std::nullopt})
                  .locally_valid());
  EXPECT_FALSE((CCLCRouteSelectionRequestResultDTO{RequestReplayStatusDTO::kRequestIdConflict,
                                                   prepared.value().acceptance, std::nullopt})
                   .locally_valid());
}

TEST(RouteSelectionStatusContracts, PreFenceKnownNoEffectIsTheSoleReceiptFreeTerminal) {
  const auto prepared = PrepareRouteSelectionAdmission(Intent(), Candidate());
  ASSERT_TRUE(prepared.ok());
  auto status = prepared.value().initial_status;
  status.phase = RouteSelectionTransactionPhaseDTO::kFailed;
  status.effect = RouteSelectionEffectDTO::kKnownNoEffect;
  status.terminal_result = RouteSelectionTerminalResultDTO::kFailedKnownNoEffect;
  status.error = ErrorDTO{"route_fence_not_acquired", "Kernel proved no Acquire effect", {}};
  status.latest_updated_at_ns = 120;
  status.remaining_duration_ns = 0;
  EXPECT_TRUE(status.locally_valid());

  status.controller_observation = DesiredObservation();
  status.controller_reconciliation = DesiredReconciliation();
  EXPECT_FALSE(status.locally_valid());
  status.controller_observation.reset();
  status.controller_reconciliation.reset();

  status.effect = RouteSelectionEffectDTO::kPartialEffectObserved;
  status.terminal_result = RouteSelectionTerminalResultDTO::kFailedPartialEffect;
  EXPECT_FALSE(status.locally_valid());
}

TEST(RouteSelectionStatusContracts, EmptyAffectedSetsRequireFreshDesiredReconciliation) {
  const auto prepared = PrepareRouteSelectionAdmission(Intent(), Candidate());
  ASSERT_TRUE(prepared.ok());
  auto status = prepared.value().initial_status;
  status.phase = RouteSelectionTransactionPhaseDTO::kObserving;
  status.accepted_fence = AcceptedFence();
  status.latest_updated_at_ns = 120;
  status.remaining_duration_ns = 9'980;
  status.affected_control_binding_ids.clear();
  status.affected_controller_role_ids.clear();
  EXPECT_FALSE(status.locally_valid());

  status.controller_observation = DesiredObservation();
  EXPECT_FALSE(status.locally_valid());

  status.controller_reconciliation = DesiredReconciliation();
  EXPECT_TRUE(status.locally_valid());
}

TEST(RouteSelectionOwnerContracts, SealsOnlyCompleteCoherentCurrentSnapshots) {
  const auto sealed = verify_route_selection_current_snapshot(CurrentSnapshot());
  ASSERT_TRUE(sealed.ok()) << sealed.error().message;
  EXPECT_EQ(sealed.value().expected_controller_domain().value(), ExpectedDomain());

  auto incomplete = CurrentSnapshot();
  incomplete.configuration_fence_scope.bindings.pop_back();
  auto result = verify_route_selection_current_snapshot(std::move(incomplete));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "scope_incomplete");

  auto unreconciled = CurrentSnapshot();
  unreconciled.active_control_configuration->state =
      ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  result = verify_route_selection_current_snapshot(std::move(unreconciled));
  ASSERT_TRUE(result.ok()) << result.error().message;

  auto stale = CurrentSnapshot();
  stale.active_control_configuration->source_runtime_epoch = RuntimeEpoch("epoch.stale");
  result = verify_route_selection_current_snapshot(std::move(stale));
  ASSERT_TRUE(result.ok()) << result.error().message;

  auto stale_unreconciled = CurrentSnapshot();
  stale_unreconciled.active_control_configuration->source_runtime_epoch =
      RuntimeEpoch("epoch.stale");
  stale_unreconciled.active_control_configuration->state =
      ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  result = verify_route_selection_current_snapshot(std::move(stale_unreconciled));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "current_configuration_invalid");

  auto foreign_source = CurrentSnapshot();
  foreign_source.active_control_configuration->source_runtime_unit_id =
      RuntimeUnitId("runtime.foreign");
  result = verify_route_selection_current_snapshot(std::move(foreign_source));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "current_configuration_invalid");
}

TEST(RouteSelectionOwnerContracts, ResolvesOnlyObservedConfigurationTruth) {
  const auto prepared = PrepareRouteSelectionAdmission(Intent(), Candidate());
  ASSERT_TRUE(prepared.ok());
  const auto sealed = verify_controller_lifecycle_expected_domain(ExpectedDomain());
  ASSERT_TRUE(sealed.ok()) << sealed.error().message;

  const auto desired_observation = DesiredObservation();
  const auto desired_reconciliation = ReconcileControllerLifecycle(
      sealed.value().value(), desired_observation, desired_observation.received_at_ns);
  ASSERT_TRUE(desired_reconciliation.ok()) << desired_reconciliation.error().message;
  const auto reconciled = ResolveRouteSelectionActiveControlConfiguration(
      prepared.value().acceptance, sealed.value(), desired_observation,
      desired_reconciliation.value(), RouteSelectionEffectDTO::kKnownNoEffect, {});
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  EXPECT_EQ(reconciled.value().state, ActiveControlConfigurationStateDTO::kReconciled);
  EXPECT_EQ(reconciled.value().binding_routes,
            desired_observation.active_configuration->binding_routes);

  auto external_observation = desired_observation;
  external_observation.source_sequence = 2U;
  external_observation.active_configuration = ControllerLifecycleObservedConfigurationDTO{
      ActivationSetId("activation.legacy"),
      {{ControlBindingId("binding.a"), ControlBindingRevision(Sha('4')),
        ControlRouteId("route.legacy")},
       {ControlBindingId("binding.b"), ControlBindingRevision(Sha('5')),
        ControlRouteId("route.legacy")}}};
  const auto external_reconciliation = ReconcileControllerLifecycle(
      sealed.value().value(), external_observation, external_observation.received_at_ns);
  ASSERT_TRUE(external_reconciliation.ok()) << external_reconciliation.error().message;
  EXPECT_EQ(external_reconciliation.value().disposition,
            ControllerLifecycleReconciliationDispositionDTO::kExternalOrUnknownUnreconciled);
  const auto external = ResolveRouteSelectionActiveControlConfiguration(
      prepared.value().acceptance, sealed.value(), external_observation,
      external_reconciliation.value(), RouteSelectionEffectDTO::kPartialEffectObserved, {});
  ASSERT_TRUE(external.ok()) << external.error().message;
  EXPECT_EQ(external.value().state,
            ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled);
  EXPECT_EQ(external.value().activation_set_id, ActivationSetId("activation.legacy"));
  EXPECT_EQ(external.value().binding_routes,
            external_observation.active_configuration->binding_routes);

  auto absent = external_observation;
  absent.active_configuration.reset();
  const auto absent_reconciliation =
      ReconcileControllerLifecycle(sealed.value().value(), absent, absent.received_at_ns);
  ASSERT_TRUE(absent_reconciliation.ok());
  const auto rejected = ResolveRouteSelectionActiveControlConfiguration(
      prepared.value().acceptance, sealed.value(), absent, absent_reconciliation.value(),
      RouteSelectionEffectDTO::kPartialEffectObserved, {});
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "observed_configuration_absent");
}

TEST(RouteSelectionOwnerContracts, ReadinessReceiptClosesReconciledAndUnreconciledPublication) {
  const auto status = PublishingStatus();
  ASSERT_TRUE(status.locally_valid());
  const auto receipt = ReadinessReceipt();
  ASSERT_TRUE(receipt.locally_valid());
  EXPECT_TRUE(VerifyRouteReadinessPublicationReceipt(status, receipt).ok());

  auto incomplete = receipt;
  incomplete.readiness_rows.pop_back();
  EXPECT_FALSE(VerifyRouteReadinessPublicationReceipt(status, incomplete).ok());

  auto unreconciled =
      PublishingStatus(ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled);
  ASSERT_TRUE(unreconciled.locally_valid());
  auto gated = receipt;
  gated.readiness_rows.clear();
  gated.operation_admission_open = false;
  EXPECT_TRUE(VerifyRouteReadinessPublicationReceipt(unreconciled, gated).ok());
  gated.operation_admission_open = true;
  EXPECT_FALSE(VerifyRouteReadinessPublicationReceipt(unreconciled, gated).ok());
}

}  // namespace
}  // namespace manyforge::core
