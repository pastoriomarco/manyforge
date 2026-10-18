#include <gtest/gtest.h>

#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/types/error_code.hpp>

#include <algorithm>
#include <stdexcept>
#include <limits>
#include <utility>
#include <string>
#include <string_view>
#include <variant>

#include "selected_runtime_composition_rpc_internal.hpp"

namespace manyforge::component_manager::detail {
namespace {

using namespace manyforge::core;

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kDigestThree =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";

[[nodiscard]] manyforge::deployment::ApplyComponentDeltaDTO RetainOnlyDelta() {
  const manyforge::deployment::ApplyComponentChangeDTO arm{
      ControllableComponentId("arm.1"),
      manyforge::deployment::ApplyComponentDisposition::kRetain,
      ContentDigest(std::string(kDigestThree)),
      ContentDigest(std::string(kDigestThree)),
      {ControlDomainId("arm.domain")},
      {ControlDomainId("arm.domain")},
      {}};
  return {{arm}, false, {}};
}

[[nodiscard]] ManagerSelectedRuntimeCompositionRequestDTO Request() {
  return {
      ContentDigest(std::string(kDigestOne)),
      ApplyId("apply-1"),
      ApplyGeneration(2U),
      ProjectionRevisionDTO{
          RetainedBundleId("bundle-1"),
          GraphRevision("graph-1"),
          SceneRevision{SceneId(std::string(kDigestOne)), 4U,
                        ContentDigest(std::string(kDigestTwo))},
      },
      ActiveBundlePublicationGeneration(5U),
      ContentDigest(std::string(kDigestOne)),
      ContentDigest(std::string(kDigestTwo)),
      4096U,
      RuntimeUnitId("runtime.kernel"),
      RuntimeEpoch("runtime-epoch-1"),
      RuntimeEpoch("manager-epoch-1"),
      ConfigurationFenceId("fence-1"),
      ConfigurationFenceGeneration(6U),
      {},
      ManagerSelectedRuntimeCompositionPhase::kFinalize,
      RetainOnlyDelta(),
      ProjectionRevisionDTO{
          RetainedBundleId("bundle-0"),
          GraphRevision("graph-0"),
          SceneRevision{SceneId(std::string(kDigestOne)), 3U,
                        ContentDigest(std::string(kDigestOne))},
      },
  };
}

[[nodiscard]] ManagerComponentBringUpInspectionRequestDTO InspectionRequest() {
  const auto ordinary = Request();
  return {ordinary.candidate_projection_revision,
          ordinary.retained_bundle_canonical_content_digest,
          ordinary.retained_bundle_canonical_byte_size,
          ordinary.predecessor_projection_revision,
          ordinary.expected_predecessor_publication_generation,
          ordinary.expected_predecessor_public_pointer_digest,
          ordinary.runtime_unit_id,
          ordinary.runtime_epoch,
          ordinary.manager_runtime_epoch};
}

[[nodiscard]] ManagerRuntimeComponentReceiptDTO RetainedReceipt() {
  return {ControllableComponentId("arm.1"),
          manyforge::deployment::ApplyComponentDisposition::kRetain,
          ContentDigest(std::string(kDigestThree)),
          {{RuntimeUnitId("runtime.kernel"), RuntimeEpoch("runtime-epoch-1")}},
          true,
          true};
}

[[nodiscard]] ManagerSelectedComponentBringUpRequestDTO SelectedRequest() {
  const ExpectedActiveControlConfigurationDTO expected_configuration{
      ExpectedActiveControlConfigurationPresenceDTO::kPresent,
      ActiveControlConfigurationId("configuration-1"),
      ActiveControlConfigurationGeneration(7U)};
  const ExpectedControlDomainRevisionDTO domain{
      ControlDomainId("domain-1"),
      ControlDomainRevision("domain-revision-1"),
      ControllerTopologyId("topology-1"),
      TopologyRevision("topology-revision-1"),
      expected_configuration,
      {{ControlBindingId("binding-1"),
        ControlBindingRevision("binding-revision-1"),
        ControlRouteId("shared-route")}}};
  return {{{RequestId("request-1"), RequestOwnerEpoch("owner-epoch-1"), "test",
            std::nullopt},
           ControllableComponentId("component-1"),
           ComponentLifecycleIntentKindDTO::kEnable,
           GraphRevision("graph-revision-1"),
           ComponentLifecycleGeneration(3U),
           {domain},
           std::string("operator request"),
           {}},
          {RetainedBundleId("candidate-bundle"),
           GraphRevision("candidate-graph"),
           {SceneId(std::string(kDigestOne)), 1U,
            ContentDigest(std::string(kDigestOne))}},
          ContentDigest(std::string(kDigestTwo)),
          1024U,
          RuntimeUnitId("runtime-1"),
          RuntimeEpoch("runtime-epoch-1"),
          RuntimeEpoch("manager-epoch-1")};
}

// Ordinary Core value fixtures, not an owner/qualification implementation.
[[nodiscard]] ManagerSelectedComponentBringUpResultDTO SelectedResult() {
  auto request = SelectedRequest();
  const auto &expected = request.intent.expected_control_domains.front();
  const ActiveControlConfigurationDTO configuration{
      ActiveControlConfigurationId("configuration-1"),
      expected.control_domain_id,
      expected.control_domain_revision,
      {{ControlBindingId("binding-1"), ControlBindingRevision("binding-revision-1"),
        ControlRouteId("shared-route")}},
      ActivationSetId("activation-set-1"),
      expected.controller().controller_topology_id,
      expected.controller().topology_revision,
      request.runtime_unit_id,
      request.runtime_epoch,
      1U,
      {},
      ActiveControlConfigurationGeneration(7U),
      ActiveControlConfigurationStateDTO::kReconciled};
  const ResolvedLifecycleControlDomainScopeDTO scope{
      expected.control_domain_id,
      expected.control_domain_revision,
      expected.controller().controller_topology_id,
      expected.controller().topology_revision,
      configuration,
      expected.controller().expected_control_bindings,
      {ResourceId("resource-1")}};
  const ResolvedLifecycleProviderOccurrenceDTO provider{
      scope.control_domain_id,
      scope.control_domain_revision,
      ProviderInstanceId("provider-1"),
      ProviderManifestId("manifest-1"),
      ProviderManifestRevision("manifest-revision-1"),
      request.runtime_unit_id,
      request.runtime_epoch,
      ContentDigest("catalog-digest-1")};
  const ComponentLifecycleAcceptedDTO acceptance{
      request.intent.request_context.request_id,
      RequestReplayStatusDTO::kAcceptedNew,
      LifecycleTransactionId("selected-lifecycle.4"),
      LifecycleTransactionGeneration(4U),
      ComponentLifecycleGeneration(4U),
      1'000'000,
      5'000'000};
  const ComponentLifecycleRequestedStateDTO requested{
      ComponentLifecycleRequestedStatePresenceDTO::kResolved,
      ComponentLifecycleStateDTO::kActive};
  ComponentLifecycleStatusDTO status{
      acceptance,
      request.intent,
      2U,
      request.intent.request_context.request_owner_epoch,
      ComponentLifecycleStateDTO::kPrepared,
      ComponentLifecycleStateDTO::kPrepared,
      requested,
      ComponentLifecycleTransactionPhaseDTO::kExecuting,
      {{scope.control_domain_id,
        expected,
        scope,
        provider,
        ComponentLifecycleStateDTO::kPrepared,
        ComponentLifecycleStateDTO::kPrepared,
        requested,
        ComponentLifecycleDomainActionStateDTO::kExecuting,
        ComponentLifecycleDomainEffectDTO::kNone,
        {},
        std::nullopt,
        std::nullopt}},
      false,
      std::nullopt,
      std::nullopt,
      1'000'000,
      1'000'010,
      4'999'990};
  const ContentDigest offer_digest{std::string(kDigestThree)};
  const CellAdmissionClaimIntentDTO claim{
      CellAdmissionClaimKindDTO::kInterventionOrRecovery,
      CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
      request.intent.request_context.request_owner_epoch,
      acceptance.lifecycle_transaction_id.value(),
      acceptance.request_id,
      offer_digest,
      request.intent.expected_admission_generation,
      {{ResourceId("resource-1")},
       {{ControlDomainId("domain-1"),
         ActiveControlConfigurationId("configuration-1"),
         ActiveControlConfigurationGeneration(7U)}}},
      std::nullopt,
      CellAdmissionInterventionEvidenceDTO{
          CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
          request.intent.request_context.request_owner_epoch,
          acceptance.lifecycle_transaction_id.value(), offer_digest,
          std::nullopt},
      std::nullopt};
  status.selected_bring_up = SelectedComponentBringUpEvidenceDTO{
      offer_digest,
      ContentDigest(std::string(kDigestOne)),
      "incident-1",
      StopId("stop-1"),
      StopGeneration(9U),
      request.candidate_projection_revision,
      request.candidate_retained_bundle_digest,
      request.candidate_retained_bundle_bytes,
      {ExecutionBindingEpoch(12U), {{ControlDomainId("domain-1"), 30U}}},
      {ExecutionBindingEpoch(13U), {{ControlDomainId("domain-1"), 31U}}},
      {claim, CellAdmissionClaimGeneration(15U),
       claim.expected_admission_generation, 1'000'000},
      {ConfigurationFenceId("fence-1"),
       ConfigurationFenceGeneration(22U),
       {{ControllableComponentId("component-1")},
        {{ControlDomainId("domain-1"),
          ControlDomainRevision("domain-revision-1"),
          ControlDomainRevision("domain-revision-1")}},
        {{ControlBindingId("binding-1"),
          ControlBindingRevision("binding-revision-1"),
          ControlBindingRevision("binding-revision-1")}},
        {ResourceId("resource-1")}},
       1'000'000,
       RequestReplayStatusDTO::kAcceptedNew},
      SelectedComponentBringUpSemanticRequestDigest(
          request.intent, request.candidate_projection_revision,
          request.candidate_retained_bundle_digest,
          request.candidate_retained_bundle_bytes, request.runtime_unit_id,
          request.runtime_epoch, request.manager_runtime_epoch)};
  return {request,
          CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kAcceptedNew,
                                        acceptance, status}};
}

[[nodiscard]] ManagerComponentProcessInventoryRequestDTO
InstalledInventoryRequest() {
  const auto base = Request();
  return {base.predecessor_projection_revision,
          base.expected_predecessor_publication_generation,
          base.expected_predecessor_public_pointer_digest,
          base.runtime_unit_id,
          base.runtime_epoch,
          base.manager_runtime_epoch};
}

[[nodiscard]] ManagerApplyComponentBringUpRequestDTO ApplyRequest(
    ComponentLifecycleIntentKindDTO kind = ComponentLifecycleIntentKindDTO::kPrepare) {
  auto common = SelectedRequest();
  common.intent.intent = kind;
  const ApplyFenceOwnerDTO owner{ApplyId("apply-1"), ApplyGeneration(8U)};
  const ApplyComponentBringUpReferenceDTO parent{
      owner, ContentDigest(std::string(kDigestThree))};
  const CellAdmissionClaimIntentDTO claim{
      CellAdmissionClaimKindDTO::kManagerApply, CellAdmissionOwnerRoleDTO::kManager,
      RequestOwnerEpoch("manager-owner"), "apply-1", RequestId("parent-request"),
      parent.plan_digest, AdmissionGeneration(9U), {{}, {}},
      std::nullopt, std::nullopt, std::nullopt};
  const ConfigurationFenceScopeDTO scope{
      {ControllableComponentId("component-1")},
      {{ControlDomainId("domain-1"), ControlDomainRevision("domain-revision-1"),
        ControlDomainRevision("domain-revision-1")}},
      {{ControlBindingId("binding-1"), ControlBindingRevision("binding-revision-1"),
        ControlBindingRevision("binding-revision-1")}}, {ResourceId("resource-1")}};
  return {common.intent, common.candidate_projection_revision,
          common.candidate_retained_bundle_digest, common.candidate_retained_bundle_bytes,
          common.runtime_unit_id, common.runtime_epoch, common.manager_runtime_epoch,
          {parent, {claim, CellAdmissionClaimGeneration(17U), AdmissionGeneration(9U), 1'000'000},
           {parent.owner, ConfigurationFenceId("parent-fence"), ConfigurationFenceGeneration(22U),
            scope, common.manager_runtime_epoch}}};
}

[[nodiscard]] ManagerApplyComponentBringUpResultDTO ApplyResult(
    ComponentLifecycleIntentKindDTO kind = ComponentLifecycleIntentKindDTO::kPrepare) {
  auto request = ApplyRequest(kind);
  auto value = *SelectedResult().result;
  auto &status = *value.status;
  status.selected_bring_up.reset();
  status.accepted_intent = request.intent;
  value.acceptance->lifecycle_transaction_id = LifecycleTransactionId("apply-component.4");
  status.acceptance = *value.acceptance;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kAccepted;
  status.requested_state.state = kind == ComponentLifecycleIntentKindDTO::kDisable
      ? ComponentLifecycleStateDTO::kDisabled
      : (kind == ComponentLifecycleIntentKindDTO::kPrepare
          ? ComponentLifecycleStateDTO::kPrepared : ComponentLifecycleStateDTO::kActive);
  status.domains.front().requested_state = status.requested_state;
  status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kReserved;
  status.apply_bring_up = ApplyComponentBringUpEvidenceDTO{
      request.parent_authority, request.parent_authority.held_fence_query.expected_scope,
      ApplyDelegatedComponentLifecycleSemanticRequestDigest(
          request.intent, request.candidate_projection_revision,
          request.candidate_retained_bundle_digest, request.candidate_retained_bundle_bytes,
          request.runtime_unit_id, request.runtime_epoch, request.manager_runtime_epoch,
          request.parent_authority.parent),
      request.candidate_projection_revision, request.candidate_retained_bundle_digest,
      request.candidate_retained_bundle_bytes, {request.runtime_unit_id, request.runtime_epoch},
      std::nullopt, std::nullopt, std::nullopt};
  return {request, value};
}

TEST(SelectedRuntimeCompositionRpcInternal,
     CheckedObserverFactsAreExactRequestAndResultIdentity) {
  auto request = Request();
  request.isaac_pose_observer_launch_facts = ManagerIsaacPoseObserverLaunchFactsDTO{
      "/run/manager/observer-original.json", "configured.source", "manager-spawn-1"};
  ASSERT_TRUE(request.valid());
  const auto value = EncodeManagerIsaacPoseObserverLaunchFacts(
      *request.isaac_pose_observer_launch_facts);
  EXPECT_EQ(DecodeManagerIsaacPoseObserverLaunchFacts(value),
            *request.isaac_pose_observer_launch_facts);
  EXPECT_THROW((void)DecodeManagerIsaacPoseObserverLaunchFacts(value + " "),
               std::invalid_argument);
  const auto packet = EncodeSelectedRuntimeCompositionRpcRequest(request);
  EXPECT_EQ(DecodeSelectedRuntimeCompositionRpcRequest(packet), request);
  EXPECT_EQ(std::get<ManagerSelectedRuntimeCompositionRequestDTO>(
                DecodeSelectedRuntimeCompositionRpcRequestVariant(packet)), request);
  const auto ordinary_packet = EncodeSelectedRuntimeCompositionRpcRequest(Request());
  EXPECT_EQ(packet.substr(0U, ordinary_packet.size()), ordinary_packet);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequest(packet + "extra"),
               std::invalid_argument);
  auto changed = request;
  changed.isaac_pose_observer_launch_facts->source_epoch_base = "manager-spawn-2";
  EXPECT_NE(changed, request);
  EXPECT_NE(EncodeSelectedRuntimeCompositionRpcRequest(changed), packet);
  const ManagerSelectedRuntimeCompositionResultDTO result{
      request, ManagerSelectedRuntimeCompositionState::kPreparing, {}};
  const auto result_packet = EncodeSelectedRuntimeCompositionRpcResponse(result);
  const auto decoded = DecodeSelectedRuntimeCompositionRpcResponse(result_packet);
  EXPECT_EQ(std::get<ManagerSelectedRuntimeCompositionResultDTO>(decoded), result);
  const auto ordinary_result_packet = EncodeSelectedRuntimeCompositionRpcResponse(
      ManagerSelectedRuntimeCompositionResultDTO{
          Request(), ManagerSelectedRuntimeCompositionState::kPreparing, {}});
  EXPECT_EQ(result_packet.substr(0U, ordinary_result_packet.size()), ordinary_result_packet);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(result_packet + "extra"),
               std::invalid_argument);
  auto truncated = packet;
  truncated.pop_back();
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequest(truncated),
               std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     ManagedEpochAcceptsOnlyCurrentBaseAndCanonicalBoundedRotation) {
  const ManagerIsaacPoseObserverLaunchFactsDTO facts{
      "/run/manager/original.json", "configured.source", "manager-spawn-1"};
  EXPECT_TRUE(facts.accepts_source_epoch("manager-spawn-1.1"));
  EXPECT_TRUE(facts.accepts_source_epoch("manager-spawn-1.10"));
  EXPECT_TRUE(facts.accepts_source_epoch("manager-spawn-1.9007199254740991"));
  for (const auto *epoch : {"manager-spawn-2.1", "manager-spawn-1.0",
                           "manager-spawn-1.01", "manager-spawn-1.-1",
                           "manager-spawn-1.1.extra", "manager-spawn-1.",
                           "manager-spawn-1.9007199254740992"}) {
    EXPECT_FALSE(facts.accepts_source_epoch(epoch)) << epoch;
  }
}

TEST(SelectedRuntimeCompositionRpcInternal,
     RoundTripsExactRequestAndBothResultStates) {
  for (const auto phase : {ManagerSelectedRuntimeCompositionPhase::kPrepare,
                           ManagerSelectedRuntimeCompositionPhase::kFinalize}) {
    auto request = Request();
    request.phase = phase;
    ASSERT_TRUE(request.valid());
    const auto encoded_request =
        EncodeSelectedRuntimeCompositionRpcRequest(request);
    EXPECT_TRUE(IsSelectedRuntimeCompositionRpcPacket(encoded_request));
    EXPECT_EQ(DecodeSelectedRuntimeCompositionRpcRequest(encoded_request),
              request);
    for (const auto state :
         {ManagerSelectedRuntimeCompositionState::kPreparing,
          ManagerSelectedRuntimeCompositionState::kInstalled}) {
      const ManagerSelectedRuntimeCompositionResultDTO result{request, state, {}};
      const auto decoded = DecodeSelectedRuntimeCompositionRpcResponse(
          EncodeSelectedRuntimeCompositionRpcResponse(result));
      const auto *round_trip =
          std::get_if<ManagerSelectedRuntimeCompositionResultDTO>(&decoded);
      ASSERT_NE(round_trip, nullptr);
      EXPECT_EQ(*round_trip, result);
    }
  }
}

TEST(SelectedRuntimeCompositionRpcInternal,
     RoundTripsTheDurableDeltaAndPerComponentReceipts) {
  const auto request = Request();
  ASSERT_TRUE(request.valid());
  const ManagerSelectedRuntimeCompositionResultDTO installed{
      request, ManagerSelectedRuntimeCompositionState::kInstalled, {RetainedReceipt()}};
  ASSERT_TRUE(installed.valid());
  const auto decoded = DecodeSelectedRuntimeCompositionRpcResponse(
      EncodeSelectedRuntimeCompositionRpcResponse(installed));
  const auto *round_trip =
      std::get_if<ManagerSelectedRuntimeCompositionResultDTO>(&decoded);
  ASSERT_NE(round_trip, nullptr);
  EXPECT_EQ(*round_trip, installed);
  EXPECT_EQ(round_trip->request.component_delta, RetainOnlyDelta());

  // A preparing result carries no installation receipts.
  const ManagerSelectedRuntimeCompositionResultDTO preparing{
      request, ManagerSelectedRuntimeCompositionState::kPreparing, {RetainedReceipt()}};
  EXPECT_FALSE(preparing.valid());
}

TEST(SelectedRuntimeCompositionRpcInternal, ReceiptsMustMatchTheDeltaExactly) {
  const auto request = Request();
  EXPECT_TRUE(ComponentReceiptsMatchRequest(request, {RetainedReceipt()}));
  // Missing receipt for a retained component.
  EXPECT_FALSE(ComponentReceiptsMatchRequest(request, {}));
  // A retained controller whose product changed must report the re-seal.
  auto not_resealed = RetainedReceipt();
  not_resealed.product_binding_resealed = false;
  EXPECT_FALSE(ComponentReceiptsMatchRequest(request, {not_resealed}));
  auto same_product = request;
  same_product.predecessor_projection_revision.retained_bundle_id =
      request.candidate_projection_revision.retained_bundle_id;
  same_product.predecessor_projection_revision.graph_revision =
      request.candidate_projection_revision.graph_revision;
  EXPECT_TRUE(ComponentReceiptsMatchRequest(same_product, {not_resealed}));
  EXPECT_FALSE(ComponentReceiptsMatchRequest(same_product, {RetainedReceipt()}));
  // Another installed identity, or a controller that was not retained.
  auto foreign = RetainedReceipt();
  foreign.installed_runtime_identity = ContentDigest(std::string(kDigestTwo));
  EXPECT_FALSE(ComponentReceiptsMatchRequest(request, {foreign}));
  auto rebuilt = RetainedReceipt();
  rebuilt.predecessor_runtime_retained = false;
  EXPECT_FALSE(ComponentReceiptsMatchRequest(request, {rebuilt}));
  // Evidence from a stale or foreign runtime owner, or an extra owner.
  auto stale_epoch = RetainedReceipt();
  stale_epoch.runtime_unit_epochs.front().runtime_epoch =
      RuntimeEpoch("runtime-epoch-0");
  EXPECT_FALSE(ComponentReceiptsMatchRequest(request, {stale_epoch}));
  auto foreign_unit = RetainedReceipt();
  foreign_unit.runtime_unit_epochs.front().runtime_unit_id =
      RuntimeUnitId("runtime.other");
  EXPECT_FALSE(ComponentReceiptsMatchRequest(request, {foreign_unit}));
  auto two_owners = RetainedReceipt();
  two_owners.runtime_unit_epochs.push_back(
      {RuntimeUnitId("runtime.later"), RuntimeEpoch("later-epoch-1")});
  EXPECT_FALSE(ComponentReceiptsMatchRequest(request, {two_owners}));
  auto no_owner = RetainedReceipt();
  no_owner.runtime_unit_epochs.clear();
  EXPECT_FALSE(ComponentReceiptsMatchRequest(request, {no_owner}));
  // An extra receipt for a component outside the delta.
  auto extra = RetainedReceipt();
  extra.controllable_component_id = ControllableComponentId("gripper.1");
  EXPECT_FALSE(ComponentReceiptsMatchRequest(request, {RetainedReceipt(), extra}));
}

// Exact multiline identities captured from the mock controller launch producer.
TEST(SelectedRuntimeCompositionRpcInternal, ActualMockLaunchIdentitiesRoundtripInventoryStopAndDrain) {
  const std::string launches[]{
      "mock\nlite6_robot\nlite6_lite_gripper\n/manyforge/lite6_robot\nlite6_traj_controller\nlite_gripper_controller\n\nsha256:948e121935a4e7d66ca927d1945438e48332cce2bd820f60488fc049bb18d4b1\nsha256:61f01507a85d3a8d6539b8488d9a525dbaf115dca33338dc0d1702168d3b34a4\n",
      "mock\nur5e_robot\nur5e_robotiq_2f_85\n/manyforge/ur5e_robot\njoint_trajectory_controller\nrobotiq_gripper_controller\n\nsha256:02e2b9e7733647dc75eea1a518dda0289aa07603d668d07e16dc87c79ab970fc\nsha256:d5e2658bd13c5f16c8f23c699c70fc200a4e4b904e995571c3eda1718faffec6\n"};
  EXPECT_EQ(launches[0].size(), 250U);
  EXPECT_EQ(launches[1].size(), 257U);
  for (std::size_t i = 0U; i < 2U; ++i) {
    const auto &launch = launches[i];
    const std::string component = i == 0U ? "lite6_robot" : "ur5e_robot";
    const auto existing = Request();
    ManagerComponentProcessStopRequestDTO stop{
        {RequestId("process.stop.1"), RequestOwnerEpoch("owner.1"), "composer", std::nullopt},
        existing.predecessor_projection_revision, existing.expected_predecessor_publication_generation,
        existing.expected_predecessor_public_pointer_digest, existing.runtime_unit_id,
        existing.runtime_epoch, existing.manager_runtime_epoch, ControllableComponentId(component), launch, 42};
    ASSERT_TRUE(stop.valid());
    EXPECT_EQ(std::get<ManagerComponentProcessStopRequestDTO>(DecodeSelectedRuntimeCompositionRpcRequestVariant(
        EncodeSelectedRuntimeCompositionRpcRequest(stop))), stop);
    EXPECT_EQ(std::get<ManagerComponentProcessStopReadRequestDTO>(DecodeSelectedRuntimeCompositionRpcRequestVariant(
        EncodeSelectedRuntimeCompositionRpcRequest(ManagerComponentProcessStopReadRequestDTO{stop}))).request, stop);
    ManagerComponentProcessStopResultDTO drained{stop, ControllerLaunchCleanupState::kDrained,
        ControllerLaunchGroupDrainReceipt{42, component, launch, "2026-10-13T19:02:00Z"}, std::nullopt};
    ASSERT_TRUE(drained.valid());
    const auto drained_packet = DecodeSelectedRuntimeCompositionRpcResponse(
        EncodeSelectedRuntimeCompositionRpcResponse(drained));
    const auto &roundtrip = std::get<ManagerComponentProcessStopResultDTO>(drained_packet);
    EXPECT_EQ(roundtrip.request, drained.request);
    EXPECT_EQ(roundtrip.state, drained.state);
    ASSERT_TRUE(roundtrip.receipt);
    EXPECT_EQ(roundtrip.receipt->leader_pid, drained.receipt->leader_pid);
    EXPECT_EQ(roundtrip.receipt->controllable_component_id, component);
    EXPECT_EQ(roundtrip.receipt->launch_identity, launch);
    EXPECT_EQ(roundtrip.receipt->observed_utc, drained.receipt->observed_utc);
    EXPECT_FALSE(roundtrip.error);
    ManagerComponentProcessInventoryResultDTO inventory{InstalledInventoryRequest(),
        {{ControllableComponentId(component), launch, 42, ProcessPresenceState::kAbsent,
          ControllerLaunchCleanupState::kDrained, drained.receipt, std::nullopt}}};
    ASSERT_TRUE(inventory.valid());
    EXPECT_EQ(std::get<ManagerComponentProcessInventoryResultDTO>(DecodeSelectedRuntimeCompositionRpcResponse(
        EncodeSelectedRuntimeCompositionRpcResponse(inventory))), inventory);
    drained.receipt->launch_identity += "foreign";
    EXPECT_FALSE(drained.valid());
    inventory.process_occurrences[0].receipt->launch_identity += "foreign";
    EXPECT_FALSE(inventory.valid());
    stop.controllable_component_id = ControllableComponentId(std::string(129U, 'x'));
    EXPECT_FALSE(stop.valid());
    stop.controllable_component_id = ControllableComponentId(component);
    stop.launch_identity.clear();
    EXPECT_FALSE(stop.valid());
    stop.launch_identity.assign(kMaximumSelectedRuntimeCompositionRpcPacketBytes, 'x');
    ASSERT_TRUE(stop.valid()); // Field ceiling does not remove whole-packet overhead.
    EXPECT_THROW(EncodeSelectedRuntimeCompositionRpcRequest(stop), std::invalid_argument);
    inventory.process_occurrences[0].launch_identity = stop.launch_identity;
    inventory.process_occurrences[0].receipt->launch_identity = stop.launch_identity;
    ASSERT_TRUE(inventory.valid());
    EXPECT_THROW(EncodeSelectedRuntimeCompositionRpcResponse(inventory), std::invalid_argument);
    stop.launch_identity.push_back('x');
    EXPECT_FALSE(stop.valid());
    inventory.process_occurrences[0].launch_identity.push_back('x');
    EXPECT_FALSE(inventory.valid());
  }
}

TEST(SelectedRuntimeCompositionRpcInternal, ProcessStopPreservesExactRequestAndReadWithoutAliases) {
  const auto existing = Request();
  const ManagerComponentProcessStopRequestDTO stop{
      {RequestId("process.stop.1"), RequestOwnerEpoch("owner.1"), "composer", std::string("close.1")},
      existing.predecessor_projection_revision, existing.expected_predecessor_publication_generation,
      existing.expected_predecessor_public_pointer_digest, existing.runtime_unit_id,
      existing.runtime_epoch, existing.manager_runtime_epoch, ControllableComponentId("arm.1"),
      "actual-launch-identity", 42};
  ASSERT_TRUE(stop.valid());
  const auto packet = EncodeSelectedRuntimeCompositionRpcRequest(stop);
  EXPECT_EQ(std::get<ManagerComponentProcessStopRequestDTO>(
      DecodeSelectedRuntimeCompositionRpcRequestVariant(packet)), stop);
  const auto read = EncodeSelectedRuntimeCompositionRpcRequest(ManagerComponentProcessStopReadRequestDTO{stop});
  EXPECT_NE(packet, read);
  EXPECT_EQ(std::get<ManagerComponentProcessStopReadRequestDTO>(
      DecodeSelectedRuntimeCompositionRpcRequestVariant(read)).request, stop);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(packet + "extra"), std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(packet.substr(0U, packet.size() - 1U)), std::invalid_argument);
  auto other = stop;
  other.runtime_epoch = RuntimeEpoch("replacement-host");
  EXPECT_NE(EncodeSelectedRuntimeCompositionRpcRequest(other), packet);
  other = stop;
  other.leader_pid = 0;
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcRequest(other), std::invalid_argument);
  other = stop;
  other.leader_pid = -1;
  EXPECT_FALSE(other.valid());
}

TEST(SelectedRuntimeCompositionRpcInternal, ProcessStopDrainRequiresExactReceiptAndUncertaintyHasNoProof) {
  const auto existing = Request();
  const ManagerComponentProcessStopRequestDTO stop{
      {RequestId("process.stop.1"), RequestOwnerEpoch("owner.1"), "composer", std::nullopt},
      existing.predecessor_projection_revision, existing.expected_predecessor_publication_generation,
      existing.expected_predecessor_public_pointer_digest, existing.runtime_unit_id,
      existing.runtime_epoch, existing.manager_runtime_epoch, ControllableComponentId("arm.1"),
      "actual-launch-identity", 42};
  ManagerComponentProcessStopResultDTO result{stop, ControllerLaunchCleanupState::kDrained,
      ControllerLaunchGroupDrainReceipt{42, "arm.1", "actual-launch-identity", "2026-10-13T12:00:00Z"}};
  ASSERT_TRUE(result.valid());
  const auto decoded = DecodeSelectedRuntimeCompositionRpcResponse(
      EncodeSelectedRuntimeCompositionRpcResponse(result));
  const auto &roundtrip = std::get<ManagerComponentProcessStopResultDTO>(decoded);
  EXPECT_EQ(roundtrip.request, stop);
  EXPECT_EQ(roundtrip.receipt, result.receipt);
  result.receipt->leader_pid = 43;
  EXPECT_FALSE(result.valid());
  result.receipt->leader_pid = 42;
  result.receipt->launch_identity = "foreign-launch";
  EXPECT_FALSE(result.valid());
  result.receipt->launch_identity = "actual-launch-identity";
  result.receipt->controllable_component_id = "other.component";
  EXPECT_FALSE(result.valid());
  result.receipt.reset();
  EXPECT_FALSE(result.valid());
  result.state = ControllerLaunchCleanupState::kUnknown;
  result.error = ErrorDTO{"owner_unavailable", "original host unavailable", {}};
  ASSERT_TRUE(result.valid());
  const auto unknown = std::get<ManagerComponentProcessStopResultDTO>(
      DecodeSelectedRuntimeCompositionRpcResponse(EncodeSelectedRuntimeCompositionRpcResponse(result)));
  EXPECT_EQ(unknown.state, ControllerLaunchCleanupState::kUnknown);
  EXPECT_FALSE(unknown.receipt);
  EXPECT_EQ(unknown.error, result.error);
  result.receipt = ControllerLaunchGroupDrainReceipt{42, "arm.1", "actual-launch-identity", "2026-10-13T12:00:00Z"};
  EXPECT_FALSE(result.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(result), std::invalid_argument);
  result.receipt.reset();
  result.state = static_cast<ControllerLaunchCleanupState>(255);
  EXPECT_FALSE(result.valid());
}

TEST(SelectedRuntimeCompositionRpcInternal, RejectsUnknownPhase) {
  auto request = Request();
  request.phase = static_cast<ManagerSelectedRuntimeCompositionPhase>(255);
  EXPECT_FALSE(request.valid());
  EXPECT_THROW(EncodeSelectedRuntimeCompositionRpcRequest(request),
               std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     RoundTripsFailureAndRejectsMalformedOrOversizedPackets) {
  const SelectedRuntimeCompositionRpcFailure failure{
      ErrorDTO{"composition_failed",
               "candidate could not be built",
               {{"reason", "invalid_product"}}}};
  const auto decoded = DecodeSelectedRuntimeCompositionRpcResponse(
      EncodeSelectedRuntimeCompositionRpcResponse(failure));
  const auto *round_trip =
      std::get_if<SelectedRuntimeCompositionRpcFailure>(&decoded);
  ASSERT_NE(round_trip, nullptr);
  EXPECT_EQ(round_trip->error.code, failure.error.code);
  EXPECT_EQ(round_trip->error.message, failure.error.message);
  EXPECT_EQ(round_trip->error.details, failure.error.details);

  auto request = EncodeSelectedRuntimeCompositionRpcRequest(Request());
  request.push_back('\0');
  EXPECT_THROW(
      static_cast<void>(DecodeSelectedRuntimeCompositionRpcRequest(request)),
      std::invalid_argument);
  EXPECT_THROW(
      static_cast<void>(DecodeSelectedRuntimeCompositionRpcRequest(std::string(
          kMaximumSelectedRuntimeCompositionRpcPacketBytes + 1U, 'x'))),
      std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     OrdinaryTagsKeepFrozenByteFingerprints) {
  // Independently spelled MFNSRC4 framing and canonical delta fixture.
  // Selected alternatives must not append absent fields to ordinary packets.
  const auto request = Request();
  const auto encoded = EncodeSelectedRuntimeCompositionRpcRequest(request);
  EXPECT_EQ(encoded.size(), 1235U);
  EXPECT_EQ(manyforge::content_identity::sha256_digest(encoded),
            "sha256:"
            "d8d91528d557444ebb29a7629351062fd6c7a3b76bb0ed3716a05181168858e2");
  const auto variant =
      DecodeSelectedRuntimeCompositionRpcRequestVariant(encoded);
  ASSERT_TRUE(
      std::holds_alternative<ManagerSelectedRuntimeCompositionRequestDTO>(
          variant));
  EXPECT_EQ(std::get<ManagerSelectedRuntimeCompositionRequestDTO>(variant),
            request);
  for (const auto &[state, fingerprint] :
       {std::pair{
            ManagerSelectedRuntimeCompositionState::kPreparing,
            "sha256:"
            "c39644ea2acfd53ba31b8bd03c80f0de11965f890c97136f815e925c5d39a057"},
        std::pair{ManagerSelectedRuntimeCompositionState::kInstalled,
                  "sha256:"
                  "486e7fe1e6a9aeabe6dfbb3577c9a48b2107d655b6a8793a4d88529aa70b"
                  "f1fd"}}) {
    const auto bytes = EncodeSelectedRuntimeCompositionRpcResponse(
        ManagerSelectedRuntimeCompositionResultDTO{request, state, {}});
    EXPECT_EQ(bytes.size(), 1239U);
    EXPECT_EQ(manyforge::content_identity::sha256_digest(bytes), fingerprint);
    EXPECT_FALSE(std::get<ManagerSelectedRuntimeCompositionResultDTO>(
        DecodeSelectedRuntimeCompositionRpcResponse(bytes)).isaac_pose_observer_retirement_receipt);
  }
  const auto failure = EncodeSelectedRuntimeCompositionRpcResponse(
      SelectedRuntimeCompositionRpcFailure{{"composition_failed",
                                            "candidate could not be built",
                                            {{"reason", "invalid_product"}}}});
  EXPECT_EQ(failure.size(), 96U);
  EXPECT_EQ(manyforge::content_identity::sha256_digest(failure),
            "sha256:"
            "9d9bd33d084b37c7d6ddc274471a46bf1c467e0c1485ccbbee5caf701a0432bb");
}

TEST(SelectedRuntimeCompositionRpcInternal,
     SelectedSubmitAndReadRemainDistinctAndExact) {
  auto request = SelectedRequest();
  request.candidate_retained_bundle_bytes =
      std::numeric_limits<std::uint64_t>::max();
  ASSERT_TRUE(request.valid());
  const auto submit = EncodeSelectedRuntimeCompositionRpcRequest(request);
  const auto read = EncodeSelectedRuntimeCompositionRpcRequest(
      ManagerSelectedComponentBringUpReadRequestDTO{request});
  ASSERT_GT(submit.size(), 8U);
  EXPECT_EQ(static_cast<unsigned char>(submit[8]), 5U);
  EXPECT_EQ(static_cast<unsigned char>(read[8]), 6U);
  EXPECT_EQ(submit.substr(9U), read.substr(9U));
  const auto submit_value =
      DecodeSelectedRuntimeCompositionRpcRequestVariant(submit);
  const auto read_value =
      DecodeSelectedRuntimeCompositionRpcRequestVariant(read);
  ASSERT_TRUE(std::holds_alternative<ManagerSelectedComponentBringUpRequestDTO>(
      submit_value));
  ASSERT_TRUE(
      std::holds_alternative<ManagerSelectedComponentBringUpReadRequestDTO>(
          read_value));
  EXPECT_EQ(std::get<ManagerSelectedComponentBringUpRequestDTO>(submit_value),
            request);
  EXPECT_EQ(std::get<ManagerSelectedComponentBringUpReadRequestDTO>(read_value)
                .request,
            request);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequest(submit),
               std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequest(read),
               std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     SelectedShapeRefusesOtherAuthorityAndInvalidFields) {
  const auto check_invalid = [](auto change) {
    auto request = SelectedRequest();
    change(request);
    EXPECT_FALSE(request.valid());
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcRequest(request),
                 std::invalid_argument);
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcRequest(
                     ManagerSelectedComponentBringUpReadRequestDTO{request}),
                 std::invalid_argument);
  };
  check_invalid([](auto &request) {
    request.intent.intent = ComponentLifecycleIntentKindDTO::kPrepare;
  });
  check_invalid([](auto &request) {
    request.intent.intent = static_cast<ComponentLifecycleIntentKindDTO>(255);
  });
  check_invalid([](auto &request) {
    request.intent.program_node_evidence =
        ProgramNodeAdmissionEvidenceDTO{RequestOwnerEpoch("runner"),
                                        ProgramRunId("run"),
                                        CellAdmissionClaimGeneration(1U),
                                        "node",
                                        "attempt",
                                        "cclc.lifecycle"};
  });
  check_invalid(
      [](auto &request) { request.intent.expected_control_domains.clear(); });
  check_invalid([](auto &request) {
    request.candidate_projection_revision.graph_revision = GraphRevision("");
  });
  check_invalid([](auto &request) {
    request.candidate_retained_bundle_digest = ContentDigest("sha256:ABC");
  });
  check_invalid(
      [](auto &request) { request.candidate_retained_bundle_bytes = 0U; });
  check_invalid(
      [](auto &request) { request.runtime_unit_id = RuntimeUnitId(""); });
  check_invalid(
      [](auto &request) { request.runtime_epoch = RuntimeEpoch(""); });
  check_invalid(
      [](auto &request) { request.manager_runtime_epoch = RuntimeEpoch(""); });
}

TEST(SelectedRuntimeCompositionRpcInternal,
     SelectedStatusRoundTripsAndRequiresAllEchoJoins) {
  const auto result = SelectedResult();
  ASSERT_TRUE(result.result->locally_valid());
  ASSERT_TRUE(result.valid());
  const auto packet = EncodeSelectedRuntimeCompositionRpcResponse(result);
  EXPECT_EQ(static_cast<unsigned char>(packet[8]), 7U);
  const auto decoded = DecodeSelectedRuntimeCompositionRpcResponse(packet);
  const auto *round_trip =
      std::get_if<ManagerSelectedComponentBringUpResultDTO>(&decoded);
  ASSERT_NE(round_trip, nullptr);
  EXPECT_EQ(round_trip->request, result.request);
  ASSERT_TRUE(round_trip->result.has_value());
  EXPECT_EQ(EncodeSelectedComponentBringUpRequestResult(*round_trip->result),
            EncodeSelectedComponentBringUpRequestResult(*result.result));
  EXPECT_NE(
      result.result->status->selected_bring_up->original_request_payload_digest,
      result.result->status->selected_bring_up->offer_digest);
  auto replayed = result;
  replayed.result->replay_status = RequestReplayStatusDTO::kReplayedExact;
  replayed.result->acceptance->replay_status =
      RequestReplayStatusDTO::kReplayedExact;
  ASSERT_TRUE(replayed.valid());
  const auto replay_value = DecodeSelectedRuntimeCompositionRpcResponse(
      EncodeSelectedRuntimeCompositionRpcResponse(replayed));
  EXPECT_EQ(std::get<ManagerSelectedComponentBringUpResultDTO>(replay_value)
                .result->replay_status,
            RequestReplayStatusDTO::kReplayedExact);
  const auto check_foreign = [&](auto change) {
    auto foreign = result;
    change(foreign);
    EXPECT_TRUE(foreign.result->locally_valid());
    EXPECT_FALSE(foreign.valid());
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(foreign),
                 std::invalid_argument);
  };
  check_foreign([](auto &value) {
    value.request.intent.request_context.request_id = RequestId("foreign");
  });
  check_foreign([](auto &value) {
    value.request.intent.request_context.request_owner_epoch =
        RequestOwnerEpoch("foreign");
  });
  check_foreign([](auto &value) {
    value.request.intent.controllable_component_id =
        ControllableComponentId("foreign");
  });
  check_foreign([](auto &value) {
    value.request.intent.expected_graph_revision = GraphRevision("foreign");
  });
  check_foreign([](auto &value) {
    value.request.intent.expected_component_lifecycle_generation =
        ComponentLifecycleGeneration(2U);
  });
  check_foreign([](auto &value) {
    value.request.intent.expected_control_domains.front()
        .control_domain_revision = ControlDomainRevision("foreign");
  });
  check_foreign([](auto &value) { value.request.intent.reason = "foreign"; });
  check_foreign([](auto &value) {
    value.request.intent.expected_admission_generation =
        AdmissionGeneration(2U);
  });
  check_foreign([](auto &value) {
    value.request.candidate_projection_revision.graph_revision =
        GraphRevision("foreign");
  });
  check_foreign([](auto &value) {
    value.request.candidate_retained_bundle_digest =
        ContentDigest(std::string(kDigestOne));
  });
  check_foreign(
      [](auto &value) { ++value.request.candidate_retained_bundle_bytes; });
  check_foreign([](auto &value) {
    value.request.runtime_unit_id = RuntimeUnitId("foreign");
  });
  check_foreign([](auto &value) {
    value.request.runtime_epoch = RuntimeEpoch("foreign");
  });
  check_foreign([](auto &value) {
    value.request.manager_runtime_epoch = RuntimeEpoch("foreign");
  });
  check_foreign([](auto &value) {
    value.result->status->selected_bring_up->original_request_payload_digest =
        value.result->status->selected_bring_up->offer_digest;
  });
  auto attributed = result;
  attributed.request.intent.attribution = {
      RequestTransportDTO::kHttp, RequestAssuranceDTO::kExternallyAuthenticated,
      std::string("receiver-principal")};
  EXPECT_TRUE(
      attributed.valid()); // Semantic replay omits receiver attribution.
  auto replaced_owner = result;
  replaced_owner.result->status->status_owner_epoch =
      RequestOwnerEpoch("replacement-owner");
  replaced_owner.result->status->remaining_duration_ns = 0;
  EXPECT_TRUE(replaced_owner
                  .valid()); // Stored observation, never a live effect receipt.
  auto foreign_packet = packet;
  const auto epoch = foreign_packet.find("manager-epoch-1");
  ASSERT_NE(epoch, std::string::npos);
  foreign_packet.replace(epoch, std::string("manager-epoch-1").size(),
                         "manager-epoch-2");
  EXPECT_THROW(
      (void)DecodeSelectedRuntimeCompositionRpcResponse(foreign_packet),
      std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     UnknownReadAndNoAdmissionResultsKeepClosedShapes) {
  const auto request = SelectedRequest();
  const ManagerSelectedComponentBringUpResultDTO unknown{request, std::nullopt};
  const auto decoded_unknown = DecodeSelectedRuntimeCompositionRpcResponse(
      EncodeSelectedRuntimeCompositionRpcResponse(unknown));
  ASSERT_TRUE(std::holds_alternative<ManagerSelectedComponentBringUpResultDTO>(
      decoded_unknown));
  EXPECT_FALSE(
      std::get<ManagerSelectedComponentBringUpResultDTO>(decoded_unknown)
          .result.has_value());
  for (const auto replay : {RequestReplayStatusDTO::kRequestResultExpired,
                            RequestReplayStatusDTO::kRequestIdConflict,
                            RequestReplayStatusDTO::kRequestLedgerFull,
                            RequestReplayStatusDTO::kStaleRequestOwnerEpoch}) {
    const ManagerSelectedComponentBringUpResultDTO result{
        request,
        CCLCLifecycleRequestResultDTO{replay, std::nullopt, std::nullopt}};
    ASSERT_TRUE(result.valid());
    const auto decoded = DecodeSelectedRuntimeCompositionRpcResponse(
        EncodeSelectedRuntimeCompositionRpcResponse(result));
    const auto &returned =
        std::get<ManagerSelectedComponentBringUpResultDTO>(decoded);
    ASSERT_TRUE(returned.result.has_value());
    EXPECT_EQ(returned.result->replay_status, replay);
    EXPECT_FALSE(returned.result->status.has_value());
    EXPECT_FALSE(returned.result->acceptance.has_value());
  }
  for (const auto replay : {RequestReplayStatusDTO::kAcceptedNew,
                            RequestReplayStatusDTO::kReplayedExact}) {
    const ManagerSelectedComponentBringUpResultDTO rejection{
        request,
        CCLCLifecycleRequestResultDTO{replay, std::nullopt, std::nullopt,
                                      ErrorDTO{"selected_refused",
                                               "no effect was started",
                                               {{"effect", "not_started"}}}}};
    ASSERT_TRUE(rejection.valid());
    const auto decoded = DecodeSelectedRuntimeCompositionRpcResponse(
        EncodeSelectedRuntimeCompositionRpcResponse(rejection));
    const auto &returned =
        std::get<ManagerSelectedComponentBringUpResultDTO>(decoded);
    ASSERT_TRUE(returned.result.has_value());
    EXPECT_EQ(returned.result->rejection, rejection.result->rejection);
    EXPECT_FALSE(returned.result->status.has_value());
  }
  auto malformed = SelectedResult();
  malformed.result->replay_status = static_cast<RequestReplayStatusDTO>(255);
  EXPECT_FALSE(malformed.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(malformed),
               std::invalid_argument);
  auto ordinary = SelectedResult();
  ordinary.result->status->selected_bring_up.reset();
  ASSERT_TRUE(ordinary.result->locally_valid());
  EXPECT_FALSE(ordinary.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(ordinary),
               std::invalid_argument);
  const ManagerSelectedComponentBringUpResultDTO incident_recovery{
      request,
      CCLCLifecycleRequestResultDTO{
          RequestReplayStatusDTO::kAcceptedNew, std::nullopt, std::nullopt,
          std::nullopt,
          ComponentIncidentRecoveryDTO{
              "incident-1", request.intent.controllable_component_id,
              LifecycleBlockingCauseClassDTO::kUnknown, "operator", 0}}};
  ASSERT_TRUE(incident_recovery.result->locally_valid());
  EXPECT_FALSE(incident_recovery.valid());
  EXPECT_THROW(
      (void)EncodeSelectedRuntimeCompositionRpcResponse(incident_recovery),
      std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     SelectedPacketsRefuseUnknownTagsLengthsAndTrailingFields) {
  const auto submit =
      EncodeSelectedRuntimeCompositionRpcRequest(SelectedRequest());
  const auto read = EncodeSelectedRuntimeCompositionRpcRequest(
      ManagerSelectedComponentBringUpReadRequestDTO{SelectedRequest()});
  const auto result =
      EncodeSelectedRuntimeCompositionRpcResponse(SelectedResult());
  for (const auto &bytes : {submit, read}) {
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(
                     bytes + "extra"),
                 std::invalid_argument);
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(
                     bytes.substr(0U, bytes.size() - 1U)),
                 std::invalid_argument);
    auto bad_tag = bytes;
    bad_tag[8] = static_cast<char>(255);
    EXPECT_THROW(
        (void)DecodeSelectedRuntimeCompositionRpcRequestVariant(bad_tag),
        std::invalid_argument);
    auto bad_length = bytes;
    bad_length[9] = static_cast<char>(255);
    EXPECT_THROW(
        (void)DecodeSelectedRuntimeCompositionRpcRequestVariant(bad_length),
        std::invalid_argument);
    auto bad_intent = bytes;
    const auto component = bad_intent.find("component-1");
    ASSERT_NE(component, std::string::npos);
    bad_intent[component + std::string("component-1").size()] =
        static_cast<char>(255);
    EXPECT_THROW(
        (void)DecodeSelectedRuntimeCompositionRpcRequestVariant(bad_intent),
        std::invalid_argument);
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(bytes),
                 std::invalid_argument);
  }
  EXPECT_THROW(
      (void)DecodeSelectedRuntimeCompositionRpcResponse(result + "extra"),
      std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(
                   result.substr(0U, result.size() - 1U)),
               std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(result),
               std::invalid_argument);
  const ManagerSelectedComponentBringUpResultDTO unknown{SelectedRequest(),
                                                         std::nullopt};
  auto noncanonical_optional =
      EncodeSelectedRuntimeCompositionRpcResponse(unknown);
  noncanonical_optional.back() = static_cast<char>(2);
  EXPECT_THROW(
      (void)DecodeSelectedRuntimeCompositionRpcResponse(noncanonical_optional),
      std::invalid_argument);
  auto oversized = submit;
  oversized.resize(kMaximumSelectedRuntimeCompositionRpcPacketBytes + 1U, '\0');
  EXPECT_THROW(
      (void)DecodeSelectedRuntimeCompositionRpcRequestVariant(oversized),
      std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(oversized),
               std::invalid_argument);
  auto oversized_request = SelectedRequest();
  oversized_request.manager_runtime_epoch = RuntimeEpoch(
      std::string(kMaximumSelectedRuntimeCompositionRpcPacketBytes, 'x'));
  ASSERT_TRUE(oversized_request.valid());
  EXPECT_THROW(
      (void)EncodeSelectedRuntimeCompositionRpcRequest(oversized_request),
      std::invalid_argument);
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(
                   ManagerSelectedComponentBringUpResultDTO{oversized_request,
                                                            std::nullopt}),
               std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     OrdinaryPortDefaultsRefuseSelectedOperations) {
  class OrdinaryOnlyPort final : public IManagerSelectedRuntimeCompositionPort {
  public:
    ManagerSelectedRuntimeCompositionResultDTO InstallOrObserve(
        const ManagerSelectedRuntimeCompositionRequestDTO &request) override {
      ++ordinary_calls;
      return {request, ManagerSelectedRuntimeCompositionState::kInstalled, {}};
    }
    unsigned ordinary_calls{};
  } port;
  const auto submit = port.SubmitSelectedComponentBringUp(SelectedRequest());
  const auto read = port.ReadSelectedComponentBringUp({SelectedRequest()});
  for (const auto *result : {&submit, &read}) {
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error(),
              (ErrorDTO{"selected_component_bring_up_unavailable",
                        "selected component bring-up is not composed",
                        {}}));
  }
  EXPECT_EQ(port.ordinary_calls, 0U);
  EXPECT_EQ(port.InstallOrObserve(Request()).request, Request());
  EXPECT_EQ(port.ordinary_calls, 1U);
}

TEST(SelectedRuntimeCompositionRpcInternal, ApplyPrepareEnableAndDisableUseSeparateClosedTags) {
  for (const auto kind : {ComponentLifecycleIntentKindDTO::kPrepare,
                          ComponentLifecycleIntentKindDTO::kEnable,
                          ComponentLifecycleIntentKindDTO::kDisable}) {
    const auto request = ApplyRequest(kind);
    ASSERT_TRUE(request.valid());
    const auto submit = EncodeSelectedRuntimeCompositionRpcRequest(request);
    EXPECT_EQ(static_cast<unsigned char>(submit[8]), 8U);
    EXPECT_EQ(std::get<ManagerApplyComponentBringUpRequestDTO>(
        DecodeSelectedRuntimeCompositionRpcRequestVariant(submit)), request);
    const ManagerApplyComponentBringUpReadRequestDTO read{request};
    const auto read_bytes = EncodeSelectedRuntimeCompositionRpcRequest(read);
    EXPECT_EQ(static_cast<unsigned char>(read_bytes[8]), 9U);
    EXPECT_EQ(std::get<ManagerApplyComponentBringUpReadRequestDTO>(
        DecodeSelectedRuntimeCompositionRpcRequestVariant(read_bytes)), read);
    const auto result = ApplyResult(kind);
    ASSERT_TRUE(result.result->locally_valid());
    ASSERT_TRUE(result.valid());
    const auto bytes = EncodeSelectedRuntimeCompositionRpcResponse(result);
    EXPECT_EQ(static_cast<unsigned char>(bytes[8]), 10U);
    const auto returned = std::get<ManagerApplyComponentBringUpResultDTO>(
        DecodeSelectedRuntimeCompositionRpcResponse(bytes));
    EXPECT_EQ(returned.request, request);
    EXPECT_EQ(EncodeApplyDelegatedComponentLifecycleRequestResult(*returned.result),
              EncodeApplyDelegatedComponentLifecycleRequestResult(*result.result));
  }
  auto recovery = ApplyResult(ComponentLifecycleIntentKindDTO::kEnable);
  auto &evidence = *recovery.result->status->apply_bring_up;
  evidence.recovery = ApplyComponentBringUpRecoveryEvidenceDTO{
      ContentDigest(std::string(kDigestOne)), "incident-1", StopId("stop-1"), StopGeneration(9U)};
  evidence.predecessor_occurrence = SelectedControllerOccurrenceDTO{
      ExecutionBindingEpoch(12U), {{ControlDomainId("domain-1"), 30U}}};
  evidence.successor_occurrence_floor = SelectedControllerOccurrenceDTO{
      ExecutionBindingEpoch(13U), {{ControlDomainId("domain-1"), 31U}}};
  ASSERT_TRUE(recovery.valid());
  const auto recovered = std::get<ManagerApplyComponentBringUpResultDTO>(
      DecodeSelectedRuntimeCompositionRpcResponse(EncodeSelectedRuntimeCompositionRpcResponse(recovery)));
  EXPECT_EQ(recovered.result->status->apply_bring_up, recovery.result->status->apply_bring_up);
  EXPECT_EQ(static_cast<unsigned char>(EncodeSelectedRuntimeCompositionRpcRequest(Request())[8]), 1U);
  EXPECT_EQ(static_cast<unsigned char>(EncodeSelectedRuntimeCompositionRpcRequest(SelectedRequest())[8]), 5U);
  EXPECT_EQ(static_cast<unsigned char>(EncodeSelectedRuntimeCompositionRpcRequest(
      ManagerSelectedComponentBringUpReadRequestDTO{SelectedRequest()})[8]), 6U);
  EXPECT_EQ(static_cast<unsigned char>(EncodeSelectedRuntimeCompositionRpcResponse(SelectedResult())[8]), 7U);
}

TEST(SelectedRuntimeCompositionRpcInternal, ApplyRequestRequiresExactParentAndManagerValues) {
  auto invalid = ApplyRequest();
  invalid.intent.intent = ComponentLifecycleIntentKindDTO::kRecover;
  EXPECT_FALSE(invalid.valid());
  invalid = ApplyRequest();
  invalid.parent_authority.parent_claim.intent.request_payload_digest = ContentDigest(std::string(kDigestOne));
  EXPECT_FALSE(invalid.valid());
  invalid = ApplyRequest();
  invalid.parent_authority.held_fence_query.owner_transaction.apply_id = ApplyId("foreign-apply");
  EXPECT_FALSE(invalid.valid());
  invalid = ApplyRequest();
  invalid.manager_runtime_epoch = RuntimeEpoch("other-manager");
  EXPECT_FALSE(invalid.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcRequest(invalid), std::invalid_argument);
  invalid = ApplyRequest();
  invalid.candidate_retained_bundle_bytes = 0U;
  EXPECT_FALSE(invalid.valid());
  invalid = ApplyRequest();
  invalid.runtime_epoch = RuntimeEpoch("");
  EXPECT_FALSE(invalid.valid());
  invalid = ApplyRequest();
  invalid.intent.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{
      RequestOwnerEpoch("program-owner"), ProgramRunId("program-run"),
      CellAdmissionClaimGeneration(1U), "node", "attempt", "cclc.lifecycle"};
  ASSERT_TRUE(invalid.intent.locally_valid());
  EXPECT_FALSE(invalid.valid());
}

TEST(SelectedRuntimeCompositionRpcInternal, ApplyResultJoinsFullAuthorityAndOriginalRequest) {
  const auto good = ApplyResult();
  ASSERT_TRUE(good.valid());
  const auto rejects_outer = [&](ManagerApplyComponentBringUpRequestDTO request) {
    ASSERT_TRUE(request.valid());
    const ManagerApplyComponentBringUpResultDTO mismatch{request, good.result};
    EXPECT_FALSE(mismatch.valid());
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(mismatch), std::invalid_argument);
  };
  auto request = good.request;
  request.parent_authority.parent_claim.claim_generation = CellAdmissionClaimGeneration(18U);
  rejects_outer(request);
  request = good.request;
  request.parent_authority.parent_claim.accepted_at_ns += 1;
  rejects_outer(request);
  request = good.request;
  request.parent_authority.held_fence_query.configuration_fence_id = ConfigurationFenceId("other-fence");
  rejects_outer(request);
  request = good.request;
  request.parent_authority.parent.plan_digest = ContentDigest(std::string(kDigestOne));
  request.parent_authority.parent_claim.intent.request_payload_digest = request.parent_authority.parent.plan_digest;
  rejects_outer(request);
  request = good.request;
  request.candidate_projection_revision.graph_revision = GraphRevision("other-graph");
  rejects_outer(request);
  request = good.request;
  request.candidate_retained_bundle_digest = ContentDigest(std::string(kDigestOne));
  rejects_outer(request);
  request = good.request;
  request.candidate_retained_bundle_bytes += 1U;
  rejects_outer(request);
  request = good.request;
  request.runtime_unit_id = RuntimeUnitId("other-host");
  rejects_outer(request);
  request = good.request;
  request.runtime_epoch = RuntimeEpoch("other-host-epoch");
  rejects_outer(request);
  request = good.request;
  request.manager_runtime_epoch = RuntimeEpoch("other-manager");
  request.parent_authority.held_fence_query.manager_runtime_epoch = request.manager_runtime_epoch;
  rejects_outer(request);
  request = good.request;
  request.intent.request_context.request_id = RequestId("other-request");
  rejects_outer(request);
  request = good.request;
  request.intent.reason = "other semantic reason";
  rejects_outer(request);
}

TEST(SelectedRuntimeCompositionRpcInternal, ApplyStableDigestExcludesReceiptButWireRetainsAttribution) {
  auto request = ApplyRequest();
  const auto digest = [&](const ManagerApplyComponentBringUpRequestDTO &value) {
    return ApplyDelegatedComponentLifecycleSemanticRequestDigest(
        value.intent, value.candidate_projection_revision, value.candidate_retained_bundle_digest,
        value.candidate_retained_bundle_bytes, value.runtime_unit_id, value.runtime_epoch,
        value.manager_runtime_epoch, value.parent_authority.parent);
  };
  const auto original = digest(request);
  request.parent_authority.parent_claim.claim_generation = CellAdmissionClaimGeneration(18U);
  ASSERT_TRUE(request.valid());
  EXPECT_EQ(digest(request), original);
  request.intent.attribution = RequestAttributionDTO{
      RequestTransportDTO::kHttp, RequestAssuranceDTO::kExternallyAuthenticated,
      std::string("operator")};
  EXPECT_EQ(digest(request), original);
  const auto round_trip = std::get<ManagerApplyComponentBringUpRequestDTO>(
      DecodeSelectedRuntimeCompositionRpcRequestVariant(EncodeSelectedRuntimeCompositionRpcRequest(request)));
  EXPECT_EQ(round_trip, request);
}

TEST(SelectedRuntimeCompositionRpcInternal, ApplyReadAndRefusalRemainObservationsOnly) {
  const auto request = ApplyRequest();
  const ManagerApplyComponentBringUpResultDTO unknown{request, std::nullopt};
  ASSERT_TRUE(unknown.valid());
  EXPECT_FALSE(std::get<ManagerApplyComponentBringUpResultDTO>(
      DecodeSelectedRuntimeCompositionRpcResponse(EncodeSelectedRuntimeCompositionRpcResponse(unknown))).result);
  for (const auto replay : {RequestReplayStatusDTO::kRequestResultExpired,
                            RequestReplayStatusDTO::kRequestIdConflict,
                            RequestReplayStatusDTO::kRequestLedgerFull,
                            RequestReplayStatusDTO::kStaleRequestOwnerEpoch}) {
    const ManagerApplyComponentBringUpResultDTO result{
        request, CCLCLifecycleRequestResultDTO{replay, std::nullopt, std::nullopt}};
    ASSERT_TRUE(result.valid());
    const auto returned = std::get<ManagerApplyComponentBringUpResultDTO>(
        DecodeSelectedRuntimeCompositionRpcResponse(EncodeSelectedRuntimeCompositionRpcResponse(result)));
    EXPECT_EQ(returned.result->replay_status, replay);
    EXPECT_FALSE(returned.result->acceptance);
  }
  const ManagerApplyComponentBringUpResultDTO rejection{
      request, CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kAcceptedNew,
        std::nullopt, std::nullopt, ErrorDTO{"apply_refused", "parent not held", {{"effect", "not_started"}}}}};
  ASSERT_TRUE(rejection.valid());
  EXPECT_EQ(std::get<ManagerApplyComponentBringUpResultDTO>(
      DecodeSelectedRuntimeCompositionRpcResponse(EncodeSelectedRuntimeCompositionRpcResponse(rejection)))
      .result->rejection, rejection.result->rejection);
  auto ordinary = ApplyResult();
  ordinary.result->status->apply_bring_up.reset();
  ASSERT_TRUE(ordinary.result->locally_valid());
  EXPECT_FALSE(ordinary.valid());
  const ManagerApplyComponentBringUpResultDTO direct{ApplyRequest(ComponentLifecycleIntentKindDTO::kEnable),
                                                    SelectedResult().result};
  ASSERT_TRUE(direct.result->locally_valid());
  EXPECT_FALSE(direct.valid());
}

TEST(SelectedRuntimeCompositionRpcInternal, ApplyPacketsRefuseMalformedDirectionAndAuthority) {
  const auto request = ApplyRequest();
  const auto submit = EncodeSelectedRuntimeCompositionRpcRequest(request);
  const auto read = EncodeSelectedRuntimeCompositionRpcRequest(ManagerApplyComponentBringUpReadRequestDTO{request});
  const auto result = EncodeSelectedRuntimeCompositionRpcResponse(ApplyResult());
  for (const auto &bytes : {submit, read}) {
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(bytes + "extra"), std::invalid_argument);
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(bytes.substr(0U, bytes.size() - 1U)), std::invalid_argument);
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(bytes), std::invalid_argument);
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequest(bytes), std::invalid_argument);
    auto malformed = bytes;
    const auto authority = EncodeApplyComponentBringUpAuthority(request.parent_authority);
    const auto offset = malformed.find(authority);
    ASSERT_NE(offset, std::string::npos);
    malformed[offset] = 'X';
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(malformed), std::invalid_argument);
  }
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(result + "extra"), std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(result), std::invalid_argument);
  auto malformed = EncodeSelectedRuntimeCompositionRpcResponse(ManagerApplyComponentBringUpResultDTO{request, std::nullopt});
  malformed.back() = 2;
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed), std::invalid_argument);
  auto oversized = request;
  oversized.manager_runtime_epoch = RuntimeEpoch(std::string(kMaximumSelectedRuntimeCompositionRpcPacketBytes, 'x'));
  oversized.parent_authority.held_fence_query.manager_runtime_epoch = oversized.manager_runtime_epoch;
  ASSERT_TRUE(oversized.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcRequest(oversized), std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal, OrdinaryPortDefaultsRefuseApplyDelegation) {
  class OrdinaryOnlyPort final : public IManagerSelectedRuntimeCompositionPort {
  public:
    ManagerSelectedRuntimeCompositionResultDTO InstallOrObserve(
        const ManagerSelectedRuntimeCompositionRequestDTO &request) override {
      ++ordinary_calls;
      return {request, ManagerSelectedRuntimeCompositionState::kInstalled, {}};
    }
    unsigned ordinary_calls{};
  } port;
  const auto submit = port.SubmitApplyComponentBringUp(ApplyRequest());
  const auto read = port.ReadApplyComponentBringUp({ApplyRequest()});
  for (const auto *result : {&submit, &read}) {
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error(), (ErrorDTO{"apply_component_bring_up_unavailable",
                                       "Apply component bring-up is not composed", {}}));
  }
  EXPECT_EQ(port.ordinary_calls, 0U);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InspectionWithoutObserverPreservesExactTag12GoldenBytes) {
  const ManagerComponentBringUpInspectionResultDTO result{
      InspectionRequest(),
      {{ControllableComponentId("arm.1"), ProcessPresenceState::kUnknown},
       {ControllableComponentId("arm.2"), ProcessPresenceState::kAbsent},
       {ControllableComponentId("arm.3"), ProcessPresenceState::kPresent}}};
  const std::string golden(
      "\x4d\x46\x4e\x53\x52\x43\x34\x00\x0c\x00\x00\x00\x08\x62\x75\x6e"
      "\x64\x6c\x65\x2d\x31\x00\x00\x00\x07\x67\x72\x61\x70\x68\x2d\x31"
      "\x00\x00\x00\x47\x73\x68\x61\x32\x35\x36\x3a\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x00\x00\x00\x00\x00"
      "\x00\x00\x04\x00\x00\x00\x47\x73\x68\x61\x32\x35\x36\x3a\x32\x32"
      "\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32"
      "\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32"
      "\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32"
      "\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x00\x00"
      "\x00\x47\x73\x68\x61\x32\x35\x36\x3a\x32\x32\x32\x32\x32\x32\x32"
      "\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32"
      "\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32"
      "\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32\x32"
      "\x32\x32\x32\x32\x32\x32\x32\x32\x32\x00\x00\x00\x00\x00\x00\x10"
      "\x00\x00\x00\x00\x08\x62\x75\x6e\x64\x6c\x65\x2d\x30\x00\x00\x00"
      "\x07\x67\x72\x61\x70\x68\x2d\x30\x00\x00\x00\x47\x73\x68\x61\x32"
      "\x35\x36\x3a\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x00\x00\x00\x00\x00\x00\x00\x03\x00\x00\x00\x47\x73"
      "\x68\x61\x32\x35\x36\x3a\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x00\x00\x00\x00\x00\x00\x00\x05\x00\x00"
      "\x00\x47\x73\x68\x61\x32\x35\x36\x3a\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x00\x00\x00\x0e\x72\x75\x6e"
      "\x74\x69\x6d\x65\x2e\x6b\x65\x72\x6e\x65\x6c\x00\x00\x00\x0f\x72"
      "\x75\x6e\x74\x69\x6d\x65\x2d\x65\x70\x6f\x63\x68\x2d\x31\x00\x00"
      "\x00\x0f\x6d\x61\x6e\x61\x67\x65\x72\x2d\x65\x70\x6f\x63\x68\x2d"
      "\x31\x00\x00\x00\x03\x00\x00\x00\x05\x61\x72\x6d\x2e\x31\x00\x00"
      "\x00\x00\x05\x61\x72\x6d\x2e\x32\x01\x00\x00\x00\x05\x61\x72\x6d"
      "\x2e\x33\x02",
      627U);
  EXPECT_EQ(EncodeSelectedRuntimeCompositionRpcResponse(result), golden);
  EXPECT_EQ(std::get<ManagerComponentBringUpInspectionResultDTO>(
                DecodeSelectedRuntimeCompositionRpcResponse(golden)), result);
  // The literal tag12 fixture carries the same legacy request body followed
  // by its 4-byte count and three 10-byte observation rows.
  auto request_golden = golden.substr(0U, golden.size() - 34U);
  request_golden[8U] = static_cast<char>(11U);
  EXPECT_EQ(EncodeSelectedRuntimeCompositionRpcRequest(result.request), request_golden);
  EXPECT_EQ(std::get<ManagerComponentBringUpInspectionRequestDTO>(
      DecodeSelectedRuntimeCompositionRpcRequestVariant(request_golden)), result.request);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InspectionEchoesReferencesAndAllThreeProcessStates) {
  auto request = InspectionRequest();
  request.candidate_retained_bundle_bytes =
      std::numeric_limits<std::uint64_t>::max();
  ASSERT_TRUE(request.valid());
  const auto request_bytes =
      EncodeSelectedRuntimeCompositionRpcRequest(request);
  EXPECT_EQ(request_bytes.substr(0U, 8U), std::string("MFNSRC4\0", 8U));
  EXPECT_EQ(static_cast<unsigned char>(request_bytes[8]), 11U);
  ASSERT_TRUE(IsSelectedRuntimeCompositionRpcPacket(request_bytes));
  EXPECT_EQ(
      std::get<ManagerComponentBringUpInspectionRequestDTO>(
          DecodeSelectedRuntimeCompositionRpcRequestVariant(request_bytes)),
      request);
  const ManagerComponentBringUpInspectionResultDTO result{
      request,
      {{ControllableComponentId("arm.1"), ProcessPresenceState::kUnknown},
       {ControllableComponentId("arm.2"), ProcessPresenceState::kAbsent},
       {ControllableComponentId("arm.3"), ProcessPresenceState::kPresent}}};
  ASSERT_TRUE(result.valid());
  const auto result_bytes = EncodeSelectedRuntimeCompositionRpcResponse(result);
  EXPECT_EQ(static_cast<unsigned char>(result_bytes[8]), 12U);
  EXPECT_EQ(std::get<ManagerComponentBringUpInspectionResultDTO>(
                DecodeSelectedRuntimeCompositionRpcResponse(result_bytes)),
            result);
  // A candidate with no configured components has an explicit empty membership.
  const ManagerComponentBringUpInspectionResultDTO empty{request, {}};
  ASSERT_TRUE(empty.valid());
  EXPECT_EQ(std::get<ManagerComponentBringUpInspectionResultDTO>(
                DecodeSelectedRuntimeCompositionRpcResponse(
                    EncodeSelectedRuntimeCompositionRpcResponse(empty))),
            empty);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InspectionRefusesIncompleteReferencesAndNoncanonicalMembership) {
  const auto rejects_request = [](auto mutate) {
    auto request = InspectionRequest();
    mutate(request);
    EXPECT_FALSE(request.valid());
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcRequest(request),
                 std::invalid_argument);
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(
                     ManagerComponentBringUpInspectionResultDTO{request, {}}),
                 std::invalid_argument);
  };
  rejects_request([](auto &request) {
    request.candidate_projection_revision.retained_bundle_id =
        RetainedBundleId("");
  });
  rejects_request([](auto &request) {
    request.candidate_retained_bundle_digest = ContentDigest("sha256:ABC");
  });
  rejects_request(
      [](auto &request) { request.candidate_retained_bundle_bytes = 0U; });
  rejects_request([](auto &request) {
    request.predecessor_projection_revision.graph_revision = GraphRevision("");
  });
  rejects_request([](auto &request) {
    request.expected_predecessor_publication_generation =
        ActiveBundlePublicationGeneration(0U);
  });
  rejects_request([](auto &request) {
    request.expected_predecessor_public_pointer_digest =
        ContentDigest("foreign-pointer");
  });
  rejects_request(
      [](auto &request) { request.runtime_unit_id = RuntimeUnitId(""); });
  rejects_request(
      [](auto &request) { request.runtime_epoch = RuntimeEpoch(""); });
  rejects_request(
      [](auto &request) { request.manager_runtime_epoch = RuntimeEpoch(""); });

  const auto rejects_membership = [](const auto &rows) {
    const ManagerComponentBringUpInspectionResultDTO result{InspectionRequest(),
                                                            rows};
    EXPECT_FALSE(result.valid());
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(result),
                 std::invalid_argument);
  };
  rejects_membership(std::vector<ManagerComponentProcessObservationDTO>{
      {ControllableComponentId(""), ProcessPresenceState::kAbsent}});
  rejects_membership(std::vector<ManagerComponentProcessObservationDTO>{
      {ControllableComponentId("arm.1"),
       static_cast<ProcessPresenceState>(255)}});
  rejects_membership(std::vector<ManagerComponentProcessObservationDTO>{
      {ControllableComponentId("arm.2"), ProcessPresenceState::kPresent},
      {ControllableComponentId("arm.1"), ProcessPresenceState::kAbsent}});
  rejects_membership(std::vector<ManagerComponentProcessObservationDTO>{
      {ControllableComponentId("arm.1"), ProcessPresenceState::kPresent},
      {ControllableComponentId("arm.1"), ProcessPresenceState::kAbsent}});
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InspectionPacketsRejectMalformedDirectionAndForeignShapes) {
  const auto request =
      EncodeSelectedRuntimeCompositionRpcRequest(InspectionRequest());
  const auto result = EncodeSelectedRuntimeCompositionRpcResponse(
      ManagerComponentBringUpInspectionResultDTO{
          InspectionRequest(),
          {{ControllableComponentId("arm.1"), ProcessPresenceState::kUnknown},
           {ControllableComponentId("arm.2"),
            ProcessPresenceState::kPresent}}});
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(request),
               std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(result),
               std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequest(request),
               std::invalid_argument);
  for (const auto &bytes : {request, result}) {
    const auto rejects_packet = [&](const std::string &packet) {
      if (static_cast<unsigned char>(bytes[8]) == 11U) {
        EXPECT_THROW(
            (void)DecodeSelectedRuntimeCompositionRpcRequestVariant(packet),
            std::invalid_argument);
      } else {
        EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(packet),
                     std::invalid_argument);
      }
    };
    rejects_packet(bytes + "extra");
    rejects_packet(bytes.substr(0U, bytes.size() - 1U));
    auto malformed = bytes;
    malformed[8] = static_cast<char>(255);
    rejects_packet(malformed);
    malformed = bytes;
    malformed[9] = static_cast<char>(255);
    rejects_packet(malformed);
    malformed = bytes;
    const auto digest = malformed.find(kDigestTwo);
    ASSERT_NE(digest, std::string::npos);
    malformed[digest] = 'X';
    rejects_packet(malformed);
  }
  auto malformed = result;
  malformed.back() = static_cast<char>(255);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed),
               std::invalid_argument);
  malformed = result;
  const auto first_component = malformed.find("arm.1");
  ASSERT_NE(first_component, std::string::npos);
  malformed.replace(first_component, 5U, "zzz.1");
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed),
               std::invalid_argument);
  malformed = result;
  const auto second_component = malformed.find("arm.2");
  ASSERT_NE(second_component, std::string::npos);
  malformed.replace(second_component, 5U, "arm.1");
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed),
               std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InspectionRetainsExistingPacketAndCollectionBounds) {
  const ManagerComponentBringUpInspectionResultDTO empty{InspectionRequest(),
                                                         {}};
  auto excessive_count = EncodeSelectedRuntimeCompositionRpcResponse(empty);
  // The final U32 is the empty membership count. 4097 exceeds the existing
  // bound.
  excessive_count[excessive_count.size() - 2U] = static_cast<char>(0x10);
  excessive_count.back() = static_cast<char>(0x01);
  EXPECT_THROW(
      (void)DecodeSelectedRuntimeCompositionRpcResponse(excessive_count),
      std::invalid_argument);
  auto result = empty;
  for (unsigned index = 0U; index <= 4096U; ++index) {
    const auto number = std::to_string(index);
    result.configured_components.push_back(
        {ControllableComponentId("component." +
                                 std::string(4U - number.size(), '0') + number),
         ProcessPresenceState::kUnknown});
  }
  ASSERT_TRUE(result.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(result),
               std::invalid_argument);
  auto oversized = InspectionRequest();
  oversized.runtime_epoch = RuntimeEpoch(
      std::string(kMaximumSelectedRuntimeCompositionRpcPacketBytes, 'x'));
  ASSERT_TRUE(oversized.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcRequest(oversized),
               std::invalid_argument);
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(
                   ManagerComponentBringUpInspectionResultDTO{oversized, {}}),
               std::invalid_argument);
  auto oversized_packet =
      EncodeSelectedRuntimeCompositionRpcRequest(InspectionRequest());
  oversized_packet.resize(kMaximumSelectedRuntimeCompositionRpcPacketBytes + 1U,
                          '\0');
  EXPECT_THROW(
      (void)DecodeSelectedRuntimeCompositionRpcRequestVariant(oversized_packet),
      std::invalid_argument);
  EXPECT_THROW(
      (void)DecodeSelectedRuntimeCompositionRpcResponse(oversized_packet),
      std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     ObserverInspectPreservesBytesExitFactsAndStableIdAcrossPersistenceStates) {
  ManagerCellObserverObservationDTO observer{
      CellId("cell.main"), std::string("source\0camera", 13U), "manager-base"};
  for (const auto presence : {ProcessPresenceState::kUnknown,
                             ProcessPresenceState::kAbsent,
                             ProcessPresenceState::kPresent}) {
    observer.process_presence = presence;
    const ManagerComponentBringUpInspectionResultDTO result{
        InspectionRequest(), {}, observer};
    ASSERT_TRUE(result.valid());
    EXPECT_EQ(std::get<ManagerComponentBringUpInspectionResultDTO>(
                  DecodeSelectedRuntimeCompositionRpcResponse(
                      EncodeSelectedRuntimeCompositionRpcResponse(result))),
              result);
  }
  observer.process_presence = ProcessPresenceState::kAbsent;
  observer.exit = ManagerCellObserverExitDTO{
      123, std::uint8_t{0}, std::nullopt, false, "2026-10-12T14:00:00Z",
      std::numeric_limits<std::int64_t>::max()};
  observer.incident_id = "observer." + std::string(64U, 'a');
  const auto stable_id = observer.incident_id;
  for (const bool signalled : {false, true}) {
    observer.exit->exit_status =
        signalled ? std::nullopt : std::optional<std::uint8_t>{0U};
    observer.exit->signal =
        signalled ? std::optional<std::uint8_t>{11U} : std::nullopt;
    observer.exit->core_dumped = signalled;
    for (const auto persistence : {ManagerCellObserverIncidentPersistenceDTO::kPending,
                                   ManagerCellObserverIncidentPersistenceDTO::kPersisted,
                                   ManagerCellObserverIncidentPersistenceDTO::kFailed,
                                   ManagerCellObserverIncidentPersistenceDTO::kUnknown}) {
      observer.incident_persistence = persistence;
      const ManagerComponentBringUpInspectionResultDTO result{
          InspectionRequest(), {}, observer};
      ASSERT_TRUE(result.valid());
      const auto decoded = std::get<ManagerComponentBringUpInspectionResultDTO>(
          DecodeSelectedRuntimeCompositionRpcResponse(
              EncodeSelectedRuntimeCompositionRpcResponse(result)));
      EXPECT_EQ(decoded, result);
      ASSERT_TRUE(decoded.cell_observer);
      EXPECT_EQ(decoded.cell_observer->source_id, std::string("source\0camera", 13U));
      EXPECT_EQ(decoded.cell_observer->incident_id, stable_id);
    }
  }
}

TEST(SelectedRuntimeCompositionRpcInternal,
     ObserverExitWithUnknownSlotPreservesUnavailableWithoutInventingAnIncident) {
  ManagerCellObserverObservationDTO observer{
      CellId("cell.main"), "source", "base", ProcessPresenceState::kAbsent,
      ManagerCellObserverExitDTO{123, std::nullopt, std::uint8_t{11}, true,
                               "2026-10-12T14:00:00Z", 12},
      std::nullopt, ManagerCellObserverIncidentPersistenceDTO::kUnknown};
  const ManagerComponentBringUpInspectionResultDTO unknown{InspectionRequest(), {}, observer};
  ASSERT_TRUE(unknown.valid());
  EXPECT_EQ(std::get<ManagerComponentBringUpInspectionResultDTO>(
                DecodeSelectedRuntimeCompositionRpcResponse(
                    EncodeSelectedRuntimeCompositionRpcResponse(unknown))), unknown);
  for (const auto persistence : {ManagerCellObserverIncidentPersistenceDTO::kPending,
                                ManagerCellObserverIncidentPersistenceDTO::kPersisted,
                                ManagerCellObserverIncidentPersistenceDTO::kFailed,
                                ManagerCellObserverIncidentPersistenceDTO::kNotApplicable}) {
    observer.incident_persistence = persistence;
    const ManagerComponentBringUpInspectionResultDTO invalid{InspectionRequest(), {}, observer};
    EXPECT_FALSE(invalid.valid());
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(invalid), std::invalid_argument);
  }
  observer.incident_persistence = ManagerCellObserverIncidentPersistenceDTO::kUnknown;
  observer.incident_id = "";
  EXPECT_FALSE(observer.valid());
  observer.incident_id = std::string(core::kMaxComponentIncidentIdBytes + 1U, 'i');
  EXPECT_FALSE(observer.valid());
}

TEST(SelectedRuntimeCompositionRpcInternal,
     ObserverInspectRejectsInvalidIdentitiesFactsAndPersistenceCombinations) {
  const ManagerCellObserverObservationDTO observer{
      CellId("cell.main"), "source", "base", ProcessPresenceState::kAbsent,
      ManagerCellObserverExitDTO{1, std::uint8_t{0}, std::nullopt, false, "", 0},
      "observer.id", ManagerCellObserverIncidentPersistenceDTO::kFailed};
  const auto rejects = [&](auto mutate) {
    auto invalid = observer;
    mutate(invalid);
    const ManagerComponentBringUpInspectionResultDTO result{
        InspectionRequest(), {}, invalid};
    EXPECT_FALSE(result.valid());
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(result),
                 std::invalid_argument);
  };
  rejects([](auto &value) { value.cell_id = CellId(""); });
  rejects([](auto &value) { value.cell_id = CellId(std::string(257U, 'c')); });
  rejects([](auto &value) { value.source_id.clear(); });
  rejects([](auto &value) { value.source_id.resize(257U, 's'); });
  rejects([](auto &value) { value.source_epoch_base.clear(); });
  rejects([](auto &value) { value.source_epoch_base.resize(240U, 'b'); });
  rejects([](auto &value) { value.source_epoch_base = std::string("b\0ase", 5U); });
  rejects([](auto &value) {
    value.process_presence = static_cast<ProcessPresenceState>(255);
  });
  rejects([](auto &value) { value.exit->pid = 0; });
  rejects([](auto &value) { value.exit->pid = -1; });
  rejects([](auto &value) { value.exit->exit_status.reset(); });
  rejects([](auto &value) { value.exit->signal = 11U; });
  rejects([](auto &value) { value.exit->observed_utc.resize(129U, 't'); });
  rejects([](auto &value) { value.exit->detected_at_ns = -1; });
  rejects([](auto &value) { value.incident_id.reset(); });
  rejects([](auto &value) { value.incident_id = ""; });
  rejects([](auto &value) { value.incident_id = std::string(129U, 'i'); });
  rejects([](auto &value) {
    value.incident_persistence =
        ManagerCellObserverIncidentPersistenceDTO::kNotApplicable;
  });
  rejects([](auto &value) {
    value.incident_persistence =
        static_cast<ManagerCellObserverIncidentPersistenceDTO>(255);
  });
  rejects([](auto &value) { value.exit.reset(); });
  rejects([](auto &value) {
    value.exit.reset();
    value.incident_id.reset();
  });
  rejects([](auto &value) {
    value.exit.reset();
    value.incident_persistence =
        ManagerCellObserverIncidentPersistenceDTO::kNotApplicable;
  });
}

TEST(SelectedRuntimeCompositionRpcInternal,
     ObserverInspectTailIsClosedAndRejectsMalformedWireDomains) {
  const ManagerComponentBringUpInspectionResultDTO absent{InspectionRequest(), {}};
  const auto body = EncodeSelectedRuntimeCompositionRpcResponse(absent);
  const ManagerCellObserverObservationDTO observer{
      CellId("c"), "s", "b", ProcessPresenceState::kAbsent,
      ManagerCellObserverExitDTO{1, std::uint8_t{0}, std::nullopt, false, "", 0},
      "i", ManagerCellObserverIncidentPersistenceDTO::kFailed};
  const auto packet = EncodeSelectedRuntimeCompositionRpcResponse(
      ManagerComponentBringUpInspectionResultDTO{InspectionRequest(), {}, observer});
  ASSERT_EQ(packet.substr(0U, body.size()), body);
  const std::string golden_tail(
      "\x01\x00\x00\x00\x01\x63\x00\x00\x00\x01\x73\x00\x00\x00\x01"
      "\x62\x01\x01\x00\x00\x00\x01\x01\x00\x00\x00\x00\x00\x00\x00"
      "\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x01\x69\x03",
      45U);
  EXPECT_EQ(packet.substr(body.size()), golden_tail);
  const auto rejects = [](const auto &bytes) {
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(bytes),
                 std::invalid_argument);
  };
  rejects(body + std::string(1U, '\0'));  // Explicit false is not canonical absence.
  rejects(packet + packet.substr(body.size()));  // Duplicate extension.
  rejects(packet + "extra");
  for (std::size_t size = body.size() + 1U; size < packet.size(); ++size)
    rejects(packet.substr(0U, size));
  const auto presence = body.size() + 1U + 3U * 5U;
  const auto exit = presence + 1U;
  // Extension, presence, exit/status/signal/core/incident booleans, persistence.
  for (const auto offset : {body.size(), presence, exit, exit + 5U,
                            exit + 7U, exit + 8U, exit + 21U, packet.size() - 1U}) {
    auto malformed = packet;
    malformed[offset] = static_cast<char>(255);
    rejects(malformed);
  }
  auto malformed = packet;
  malformed[exit + 1U] = static_cast<char>(0x80);  // PID exceeds positive int32.
  rejects(malformed);
  malformed = packet;
  malformed[exit + 13U] = static_cast<char>(0x80);  // Time exceeds nonnegative int64.
  rejects(malformed);
  malformed = packet;
  // Canonically encoded exit with neither status nor signal.
  malformed[exit + 5U] = '\0';
  malformed.erase(exit + 6U, 1U);
  rejects(malformed);
  malformed = packet;
  malformed[exit + 7U] = '\1';  // Both status and signal.
  malformed.insert(exit + 8U, 1U, '\13');
  rejects(malformed);
  malformed = packet;
  malformed[exit + 21U] = '\0';  // Exit without incident ID.
  malformed.erase(exit + 22U, 5U);
  rejects(malformed);
  auto no_exit = observer;
  no_exit.exit.reset();
  no_exit.incident_id.reset();
  no_exit.incident_persistence =
      ManagerCellObserverIncidentPersistenceDTO::kNotApplicable;
  malformed = EncodeSelectedRuntimeCompositionRpcResponse(
      ManagerComponentBringUpInspectionResultDTO{InspectionRequest(), {}, no_exit});
  malformed.back() =
      static_cast<char>(ManagerCellObserverIncidentPersistenceDTO::kPending);
  rejects(malformed);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     ObserverInspectMaximumTailKeepsWholePacketCapWithoutDroppingComponents) {
  const ManagerCellObserverObservationDTO observer{
      CellId(std::string(256U, 'c')), std::string(255U, 's') + '\0',
      std::string(239U, 'b'), ProcessPresenceState::kAbsent,
      ManagerCellObserverExitDTO{std::numeric_limits<std::int32_t>::max(),
                                std::uint8_t{255}, std::nullopt, false,
                                std::string(128U, 't'),
                                std::numeric_limits<std::int64_t>::max()},
      std::string(128U, 'i'), ManagerCellObserverIncidentPersistenceDTO::kUnknown};
  ManagerComponentBringUpInspectionResultDTO result{
      InspectionRequest(),
      {{ControllableComponentId("arm.1"), ProcessPresenceState::kPresent}},
      observer};
  ASSERT_TRUE(result.valid());
  auto absent = result;
  absent.cell_observer.reset();
  const auto body = EncodeSelectedRuntimeCompositionRpcResponse(absent);
  const auto packet = EncodeSelectedRuntimeCompositionRpcResponse(result);
  // The approved 1049-byte upper bound counts both optional status/signal
  // payloads; exactly one is valid, so the attainable maximum is 1048.
  EXPECT_EQ(packet.size() - body.size(), 1048U);
  EXPECT_LE(packet.size() - body.size(), 1049U);
  EXPECT_EQ(packet.substr(0U, body.size()), body);
  result.request.runtime_epoch = RuntimeEpoch(
      std::string(result.request.runtime_epoch.value().size() +
                      kMaximumSelectedRuntimeCompositionRpcPacketBytes - packet.size(),
                  'e'));
  const auto maximum = EncodeSelectedRuntimeCompositionRpcResponse(result);
  EXPECT_EQ(maximum.size(), kMaximumSelectedRuntimeCompositionRpcPacketBytes);
  EXPECT_EQ(std::get<ManagerComponentBringUpInspectionResultDTO>(
                DecodeSelectedRuntimeCompositionRpcResponse(maximum)),
            result);
  result.request.runtime_epoch =
      RuntimeEpoch(result.request.runtime_epoch.value() + "e");
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(result),
               std::invalid_argument);
  // A legal existing body at the cap cannot acquire a tail by truncating rows.
  absent.request = result.request;
  absent.request.runtime_epoch = RuntimeEpoch(
      absent.request.runtime_epoch.value() + std::string(1047U, 'e'));
  EXPECT_EQ(EncodeSelectedRuntimeCompositionRpcResponse(absent).size(),
            kMaximumSelectedRuntimeCompositionRpcPacketBytes);
  absent.cell_observer = observer;
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(absent),
               std::invalid_argument);
  EXPECT_EQ(absent.configured_components.size(), 1U);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(maximum + "e"),
               std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     OrdinaryPortInspectionDefaultHasNoEffects) {
  class OrdinaryOnlyPort final : public IManagerSelectedRuntimeCompositionPort {
  public:
    ManagerSelectedRuntimeCompositionResultDTO InstallOrObserve(
        const ManagerSelectedRuntimeCompositionRequestDTO &request) override {
      ++ordinary_calls;
      return {request, ManagerSelectedRuntimeCompositionState::kInstalled, {}};
    }
    unsigned ordinary_calls{};
  } port;
  const auto result = port.InspectComponentBringUp(InspectionRequest());
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error(),
            (ErrorDTO{"component_bring_up_inspection_unavailable",
                      "component process inspection is not composed",
                      {}}));
  EXPECT_EQ(port.ordinary_calls, 0U);
}

[[nodiscard]] ManagerComponentBringUpPlanningFactsDTO PlanningFacts() {
  auto domains = SelectedRequest().intent.expected_control_domains;
  const ExpectedActiveControlConfigurationDTO absent_configuration{
      ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
  domains.front().controller().expected_active_control_configuration = absent_configuration;
  return {ControllableComponentId("arm.1"), RequestOwnerEpoch("owner.current"),
          {std::nullopt, ComponentLifecycleGeneration(1U), 0U}, std::move(domains),
          LifecycleTargetStateDTO::kActive, ComponentLifecycleIntentKindDTO::kPrepare};
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InspectionPlanningFactsRoundTripWithoutChangingLegacyOrObserverPrefixes) {
  for (const auto with_observer : {false, true}) {
    for (const auto qualified : {false, true}) {
      SCOPED_TRACE(with_observer);
      SCOPED_TRACE(qualified);
      auto request = InspectionRequest();
      const auto legacy_request_bytes = EncodeSelectedRuntimeCompositionRpcRequest(request);
      request.planning_component_id = ControllableComponentId("arm.1");
      ASSERT_TRUE(request.valid());
      const auto request_bytes = EncodeSelectedRuntimeCompositionRpcRequest(request);
      EXPECT_EQ(request_bytes.substr(0U, legacy_request_bytes.size()), legacy_request_bytes);
      EXPECT_EQ(std::get<ManagerComponentBringUpInspectionRequestDTO>(
          DecodeSelectedRuntimeCompositionRpcRequestVariant(request_bytes)), request);
      auto facts = PlanningFacts();
      if (qualified) {
        facts.generation_selection = {std::nullopt, ComponentLifecycleGeneration(2U), 0U,
            ApplyComponentQualifiedPredecessorDTO{ComponentLifecycleGeneration(1U), ContentDigest(std::string(kDigestOne))}};
      }
      ASSERT_TRUE(facts.valid());
      std::optional<ManagerCellObserverObservationDTO> observer;
      if (with_observer) {
        observer = ManagerCellObserverObservationDTO{CellId("cell.1"), "observer.source", "observer.epoch",
            ProcessPresenceState::kPresent, std::nullopt, std::nullopt,
            ManagerCellObserverIncidentPersistenceDTO::kNotApplicable};
      }
      const ManagerComponentBringUpInspectionResultDTO result{request,
          {{ControllableComponentId("arm.1"), ProcessPresenceState::kUnknown}}, observer, facts};
      ASSERT_TRUE(result.valid());
      auto legacy = result;
      legacy.request.planning_component_id.reset();
      legacy.planning_facts.reset();
      const auto legacy_bytes = EncodeSelectedRuntimeCompositionRpcResponse(legacy);
      const auto bytes = EncodeSelectedRuntimeCompositionRpcResponse(result);
      EXPECT_EQ(bytes.substr(0U, legacy_bytes.size()), legacy_bytes);
      const auto decoded = std::get<ManagerComponentBringUpInspectionResultDTO>(
          DecodeSelectedRuntimeCompositionRpcResponse(bytes));
      EXPECT_EQ(decoded, result);
      EXPECT_EQ(EncodeSelectedRuntimeCompositionRpcResponse(decoded), bytes);
      EXPECT_EQ(decoded.cell_observer, observer);
      EXPECT_FALSE(decoded.planning_facts->generation_selection.predecessor_generation);
    }
  }
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InspectionPlanningFactsRequireExactMembershipOwnerSelectionAndCandidateClosure) {
  auto request = InspectionRequest();
  request.planning_component_id = ControllableComponentId("arm.1");
  const ManagerComponentBringUpInspectionResultDTO actual{request,
      {{ControllableComponentId("arm.1"), ProcessPresenceState::kAbsent}}, std::nullopt, PlanningFacts()};
  ASSERT_TRUE(actual.valid());
  const auto exact = EncodeSelectedRuntimeCompositionRpcResponse(actual);
  EXPECT_EQ(std::get<ManagerComponentBringUpInspectionResultDTO>(DecodeSelectedRuntimeCompositionRpcResponse(exact)), actual);
  for (unsigned mutation = 0U; mutation != 12U; ++mutation) {
    SCOPED_TRACE(mutation);
    auto foreign = actual;
    auto &facts = *foreign.planning_facts;
    if (mutation == 0U) foreign.request.planning_component_id.reset();
    if (mutation == 2U) facts.component_id = ControllableComponentId("arm.foreign");
    if (mutation == 3U) foreign.configured_components.clear();
    if (mutation == 4U) facts.owner_epoch = RequestOwnerEpoch("");
    if (mutation == 5U) facts.owner_epoch = RequestOwnerEpoch(std::string(kMaxLifecycleIdentityBytes + 1U, 'x'));
    if (mutation == 6U) facts.generation_selection.prior_reservation_generation = 7U;
    if (mutation == 7U) facts.generation_selection = {ComponentLifecycleGeneration(3U), ComponentLifecycleGeneration(4U), 0U,
        ApplyComponentQualifiedPredecessorDTO{ComponentLifecycleGeneration(1U), ContentDigest(std::string(kDigestOne))}};
    if (mutation == 8U) facts.candidate_expected_domains.clear();
    if (mutation == 9U) facts.candidate_expected_domains.push_back(facts.candidate_expected_domains.front());
    if (mutation == 10U)
      facts.candidate_expected_domains.front().controller().topology_revision =
          TopologyRevision("");
    if (mutation == 11U)
      facts.candidate_expected_domains.front()
          .controller()
          .expected_control_bindings.front()
          .control_route_id = ControlRouteId("");
    if (mutation == 1U) foreign.planning_facts.reset();
    EXPECT_FALSE(foreign.valid());
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(foreign), std::invalid_argument);
  }
  request.planning_component_id = ControllableComponentId(std::string(kMaxLifecycleIdentityBytes + 1U, 'x'));
  EXPECT_FALSE(request.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcRequest(request), std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InspectionPlanningTailRejectsForeignDirectionTruncationDuplicateAndMalformedValues) {
  auto request = InspectionRequest();
  const auto legacy_request = EncodeSelectedRuntimeCompositionRpcRequest(request);
  const ManagerComponentBringUpInspectionResultDTO legacy{request,
      {{ControllableComponentId("arm.1"), ProcessPresenceState::kUnknown}}};
  const auto legacy_response = EncodeSelectedRuntimeCompositionRpcResponse(legacy);
  request.planning_component_id = ControllableComponentId("arm.1");
  const auto request_bytes = EncodeSelectedRuntimeCompositionRpcRequest(request);
  const ManagerComponentBringUpInspectionResultDTO result{request, legacy.configured_components,
      std::nullopt, PlanningFacts()};
  const auto response_bytes = EncodeSelectedRuntimeCompositionRpcResponse(result);
  const auto request_tail = request_bytes.substr(legacy_request.size());
  const auto response_tail = response_bytes.substr(legacy_response.size());
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(legacy_request + response_tail), std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(legacy_response + request_tail), std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(request_bytes + request_tail), std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(response_bytes + response_tail), std::invalid_argument);
  for (std::size_t size = legacy_request.size() + 1U; size < request_bytes.size(); ++size) {
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(request_bytes.substr(0U, size)), std::invalid_argument);
  }
  for (std::size_t size = legacy_response.size() + 1U; size < response_bytes.size(); ++size) {
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(response_bytes.substr(0U, size)), std::invalid_argument);
  }
  auto foreign_request = request_bytes;
  foreign_request[legacy_request.size()] = static_cast<char>(3U);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(foreign_request), std::invalid_argument);
  auto foreign_response = response_bytes;
  foreign_response[legacy_response.size()] = static_cast<char>(0U);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(foreign_response), std::invalid_argument);
  const auto echo = legacy_response.size() + 1U + 4U + std::string_view("MFNSRC4-INSPECT-PLANNING-RESULT-V2").size() + 4U;
  foreign_response = response_bytes;
  foreign_response[echo] = 'x';
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(foreign_response), std::invalid_argument);
  const auto watermark_presence = echo + 5U + 4U + 5U + 4U + result.planning_facts->owner_epoch.value().size();
  foreign_response = response_bytes;
  foreign_response[watermark_presence] = static_cast<char>(2U);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(foreign_response), std::invalid_argument);
  foreign_response = response_bytes;
  foreign_response[watermark_presence + 8U] = static_cast<char>(0U);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(foreign_response), std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InspectionPlanningFactsKeepExistingDomainBindingAndWholePacketBounds) {
  auto request = InspectionRequest();
  request.planning_component_id = ControllableComponentId("arm.1");
  auto facts = PlanningFacts();
  const auto domain = facts.candidate_expected_domains.front();
  facts.candidate_expected_domains.clear();
  for (std::size_t index = 0U; index <= kMaxLifecycleExpectedDomains; ++index) {
    auto row = domain;
    row.control_domain_id = ControlDomainId("domain." + std::to_string(index));
    facts.candidate_expected_domains.push_back(std::move(row));
  }
  std::sort(facts.candidate_expected_domains.begin(), facts.candidate_expected_domains.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.control_domain_id < rhs.control_domain_id; });
  EXPECT_FALSE(facts.valid());
  const ManagerComponentBringUpInspectionResultDTO too_many{request,
      {{ControllableComponentId("arm.1"), ProcessPresenceState::kUnknown}}, std::nullopt, facts};
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(too_many), std::invalid_argument);
  facts = PlanningFacts();
  facts.candidate_expected_domains.front().controller().expected_control_bindings.clear();
  for (std::size_t index = 0U; index < kMaxLifecycleExpectedBindings; ++index) {
    facts.candidate_expected_domains.front().controller().expected_control_bindings.push_back(
        {ControlBindingId(std::to_string(index) +
                          std::string(kMaxLifecycleIdentityBytes - 4U, 'b')),
         ControlBindingRevision(std::string(kMaxLifecycleIdentityBytes, 'r')),
         ControlRouteId(std::string(kMaxLifecycleIdentityBytes, 'q'))});
  }
  std::sort(facts.candidate_expected_domains.front().controller().expected_control_bindings.begin(),
            facts.candidate_expected_domains.front().controller().expected_control_bindings.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.control_binding_id < rhs.control_binding_id;
            });
  ASSERT_TRUE(facts.valid());
  const ManagerComponentBringUpInspectionResultDTO oversized{request,
      {{ControllableComponentId("arm.1"), ProcessPresenceState::kUnknown}}, std::nullopt, facts};
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(oversized), std::invalid_argument);
}

[[nodiscard]] ManagerSelectedRuntimeCompositionResultDTO PrepareRetirementResult() {
  auto request = Request();
  request.phase = ManagerSelectedRuntimeCompositionPhase::kPrepare;
  return {request, ManagerSelectedRuntimeCompositionState::kInstalled, {},
      ManagerIsaacPoseObserverRetirementReceiptDTO{
          {"/run/manager/predecessor.json", "observer.source", "predecessor-spawn"}, 1234}};
}

TEST(SelectedRuntimeCompositionRpcInternal,
     PrepareRetirementReceiptRoundTripsWithOrWithoutSuccessorFacts) {
  for (const auto with_successor : {false, true}) {
    SCOPED_TRACE(with_successor);
    auto result = PrepareRetirementResult();
    if (with_successor) {
      result.request.isaac_pose_observer_launch_facts = ManagerIsaacPoseObserverLaunchFactsDTO{
          "/run/manager/successor.json", "observer.source", "successor-spawn"};
    }
    ASSERT_TRUE(result.valid());
    const auto request_bytes = EncodeSelectedRuntimeCompositionRpcRequest(result.request);
    auto absent = result;
    absent.isaac_pose_observer_retirement_receipt.reset();
    // Installed alone remains an observation, not positive reap evidence.
    ASSERT_TRUE(absent.valid());
    const auto absent_bytes = EncodeSelectedRuntimeCompositionRpcResponse(absent);
    const auto bytes = EncodeSelectedRuntimeCompositionRpcResponse(result);
    EXPECT_EQ(bytes.substr(0U, absent_bytes.size()), absent_bytes);
    EXPECT_EQ(EncodeSelectedRuntimeCompositionRpcRequest(result.request), request_bytes);
    const auto decoded = std::get<ManagerSelectedRuntimeCompositionResultDTO>(
        DecodeSelectedRuntimeCompositionRpcResponse(bytes));
    EXPECT_EQ(decoded, result);
    EXPECT_EQ(EncodeSelectedRuntimeCompositionRpcResponse(decoded), bytes);
    EXPECT_EQ(std::get<ManagerSelectedRuntimeCompositionResultDTO>(
        DecodeSelectedRuntimeCompositionRpcResponse(absent_bytes)), absent);
  }
  auto maximum = PrepareRetirementResult();
  maximum.isaac_pose_observer_retirement_receipt->leader_pid = std::numeric_limits<std::int32_t>::max();
  ASSERT_TRUE(maximum.valid());
  EXPECT_EQ(std::get<ManagerSelectedRuntimeCompositionResultDTO>(DecodeSelectedRuntimeCompositionRpcResponse(
      EncodeSelectedRuntimeCompositionRpcResponse(maximum))), maximum);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     RetirementReceiptRequiresPrepareInstalledAndExactDistinctPredecessor) {
  const auto actual = PrepareRetirementResult();
  ASSERT_TRUE(actual.valid());
  for (unsigned mutation = 0U; mutation != 10U; ++mutation) {
    SCOPED_TRACE(mutation);
    auto foreign = actual;
    auto &receipt = *foreign.isaac_pose_observer_retirement_receipt;
    if (mutation == 0U) foreign.request.phase = ManagerSelectedRuntimeCompositionPhase::kFinalize;
    if (mutation == 1U) foreign.state = ManagerSelectedRuntimeCompositionState::kPreparing;
    if (mutation == 2U) receipt.leader_pid = 0;
    if (mutation == 3U) receipt.leader_pid = -1;
    if (mutation == 4U) receipt.leader_pid = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1;
    if (mutation == 5U) receipt.predecessor.retained_snapshot_path = "relative.json";
    if (mutation == 6U) receipt.predecessor.source_id.clear();
    if (mutation == 7U) receipt.predecessor.source_epoch_base.clear();
    if (mutation == 8U) foreign.request.isaac_pose_observer_launch_facts = receipt.predecessor;
    if (mutation == 9U) {
      foreign.request.isaac_pose_observer_launch_facts = ManagerIsaacPoseObserverLaunchFactsDTO{
          "/run/manager/different-path.json", "different.source", receipt.predecessor.source_epoch_base};
    }
    EXPECT_FALSE(foreign.valid());
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(foreign), std::invalid_argument);
  }
}

TEST(SelectedRuntimeCompositionRpcInternal,
     RetirementResultTailRejectsMalformedTruncatedDuplicateAndRequestPackets) {
  const auto result = PrepareRetirementResult();
  auto absent = result;
  absent.isaac_pose_observer_retirement_receipt.reset();
  const auto legacy = EncodeSelectedRuntimeCompositionRpcResponse(absent);
  const auto bytes = EncodeSelectedRuntimeCompositionRpcResponse(result);
  const auto tail = bytes.substr(legacy.size());
  ASSERT_GT(tail.size(), 8U);
  for (std::size_t length = legacy.size() + 1U; length < bytes.size(); ++length) {
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(bytes.substr(0U, length)), std::invalid_argument);
  }
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(bytes + tail), std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequest(
      EncodeSelectedRuntimeCompositionRpcRequest(result.request) + tail), std::invalid_argument);
  auto malformed = bytes;
  malformed[legacy.size()] = static_cast<char>(4U);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed), std::invalid_argument);
  malformed = bytes;
  malformed[legacy.size() + 5U] = 'X';
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed), std::invalid_argument);
  // The trailing eight bytes are the full unsigned wire PID.
  malformed = bytes;
  std::fill(malformed.end() - 8, malformed.end(), static_cast<char>(0xffU));
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed), std::invalid_argument);
  malformed = bytes;
  std::fill(malformed.end() - 8, malformed.end(), '\0');
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed), std::invalid_argument);
  malformed = bytes;
  malformed[8U] = static_cast<char>(2U); // Preparing cannot carry positive reap.
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed), std::invalid_argument);
  auto oversized = result;
  oversized.request.runtime_epoch = RuntimeEpoch(std::string(kMaximumSelectedRuntimeCompositionRpcPacketBytes, 'e'));
  ASSERT_TRUE(oversized.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(oversized), std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InspectionPlanningTargetAndNextActionRemainDistinctClosedFacts) {
  for (const auto target : {LifecycleTargetStateDTO::kActive, LifecycleTargetStateDTO::kPrepared,
                            LifecycleTargetStateDTO::kDisabled}) {
    for (const auto intent : {ComponentLifecycleIntentKindDTO::kPrepare, ComponentLifecycleIntentKindDTO::kEnable,
                              ComponentLifecycleIntentKindDTO::kDisable, ComponentLifecycleIntentKindDTO::kRecover}) {
      SCOPED_TRACE(static_cast<unsigned>(target));
      SCOPED_TRACE(static_cast<unsigned>(intent));
      auto request = InspectionRequest();
      request.planning_component_id = ControllableComponentId("arm.1");
      auto facts = PlanningFacts();
      facts.requested_target = target;
      facts.next_intent = intent;
      const bool permissible =
          (target == LifecycleTargetStateDTO::kActive &&
           (intent == ComponentLifecycleIntentKindDTO::kPrepare || intent == ComponentLifecycleIntentKindDTO::kEnable)) ||
          (target == LifecycleTargetStateDTO::kPrepared && intent == ComponentLifecycleIntentKindDTO::kPrepare) ||
          (target == LifecycleTargetStateDTO::kDisabled && intent == ComponentLifecycleIntentKindDTO::kDisable);
      const ManagerComponentBringUpInspectionResultDTO result{request,
          {{ControllableComponentId("arm.1"), ProcessPresenceState::kUnknown}}, std::nullopt, facts};
      EXPECT_EQ(result.valid(), permissible);
      if (permissible) {
        const auto bytes = EncodeSelectedRuntimeCompositionRpcResponse(result);
        EXPECT_EQ(std::get<ManagerComponentBringUpInspectionResultDTO>(DecodeSelectedRuntimeCompositionRpcResponse(bytes)), result);
        auto malformed = bytes;
        malformed[malformed.size() - 2U] = static_cast<char>(255U);
        EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed), std::invalid_argument);
        malformed = bytes;
        malformed.back() = static_cast<char>(ComponentLifecycleIntentKindDTO::kRecover);
        EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed), std::invalid_argument);
        const auto magic = malformed.find("MFNSRC4-INSPECT-PLANNING-RESULT-V2");
        ASSERT_NE(magic, std::string::npos);
        malformed = bytes;
        malformed[magic + std::string_view("MFNSRC4-INSPECT-PLANNING-RESULT-V").size()] = '1';
        EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(malformed), std::invalid_argument);
      } else {
        EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(result), std::invalid_argument);
      }
    }
  }
  auto unknown = PlanningFacts();
  unknown.next_intent = static_cast<ComponentLifecycleIntentKindDTO>(255U);
  EXPECT_FALSE(unknown.valid());
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InstalledInventoryKeepsKnownLaunchesAndExactScope) {
  const auto request = InstalledInventoryRequest();
  ASSERT_TRUE(request.valid());
  const auto decoded = DecodeSelectedRuntimeCompositionRpcRequestVariant(
      EncodeSelectedRuntimeCompositionRpcRequest(request));
  ASSERT_TRUE(
      std::holds_alternative<ManagerComponentProcessInventoryRequestDTO>(
          decoded));
  EXPECT_EQ(std::get<ManagerComponentProcessInventoryRequestDTO>(decoded),
            request);
  ManagerComponentProcessInventoryResultDTO result{
      request,
      {{ControllableComponentId("component-1"), "launch-old", 101,
        ProcessPresenceState::kAbsent, ControllerLaunchCleanupState::kDrained,
        ControllerLaunchGroupDrainReceipt{101, "component-1", "launch-old",
                                          "2026-10-13T12:00:00Z"},
        ContentDigest(std::string(kDigestOne))},
       {ControllableComponentId("component-1"), "launch-target", 102,
        ProcessPresenceState::kPresent,
        ControllerLaunchCleanupState::kNotRequested, std::nullopt,
        ContentDigest(std::string(kDigestTwo))},
       {ControllableComponentId("removed-component"), "failed-launch", 103,
        ProcessPresenceState::kAbsent, ControllerLaunchCleanupState::kUnknown,
        std::nullopt, ContentDigest(std::string(kDigestThree))}}};
  ASSERT_TRUE(result.valid());
  const auto roundtrip = DecodeSelectedRuntimeCompositionRpcResponse(
      EncodeSelectedRuntimeCompositionRpcResponse(result));
  ASSERT_TRUE(std::holds_alternative<ManagerComponentProcessInventoryResultDTO>(
      roundtrip));
  EXPECT_EQ(std::get<ManagerComponentProcessInventoryResultDTO>(roundtrip),
            result);
  auto closed = EncodeSelectedRuntimeCompositionRpcRequest(request);
  closed.push_back('x');
  EXPECT_THROW(DecodeSelectedRuntimeCompositionRpcRequestVariant(closed),
               std::invalid_argument);
  auto foreign = request;
  foreign.runtime_epoch = RuntimeEpoch("another-host");
  EXPECT_NE(std::get<ManagerComponentProcessInventoryRequestDTO>(
                DecodeSelectedRuntimeCompositionRpcRequestVariant(
                    EncodeSelectedRuntimeCompositionRpcRequest(foreign))),
            request);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InstalledInventoryPreservesCurrentOwnerFailureAndDoesNotInventDrain) {
  const ManagerComponentProcessInventoryResultDTO result{
      InstalledInventoryRequest(),
      {{ControllableComponentId("component-1"), "exact-launch", 105,
        ProcessPresenceState::kUnknown,
        ControllerLaunchCleanupState::kUnknown}},
      ErrorDTO{"process_inventory_owner_changing",
               "actual current owner coverage is unsettled", {}}};
  ASSERT_TRUE(result.valid());
  EXPECT_EQ(std::get<ManagerComponentProcessInventoryResultDTO>(
                DecodeSelectedRuntimeCompositionRpcResponse(
                    EncodeSelectedRuntimeCompositionRpcResponse(result))),
            result);
  EXPECT_FALSE(result.process_occurrences.front().receipt.has_value());
  // A stable empty known union is successful. It makes no assertion about
  // unobserved external processes or a previous host's launches.
  const ManagerComponentProcessInventoryResultDTO empty{
      InstalledInventoryRequest(), {}, std::nullopt};
  ASSERT_TRUE(empty.valid());
  EXPECT_EQ(std::get<ManagerComponentProcessInventoryResultDTO>(
                DecodeSelectedRuntimeCompositionRpcResponse(
                    EncodeSelectedRuntimeCompositionRpcResponse(empty))),
            empty);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     InstalledInventoryRejectsForeignDrainDuplicateAndTruncation) {
  ManagerComponentProcessInventoryResultDTO result{
      InstalledInventoryRequest(),
      {{ControllableComponentId("component-1"), "launch-old", 101,
        ProcessPresenceState::kAbsent, ControllerLaunchCleanupState::kDrained,
        ControllerLaunchGroupDrainReceipt{101, "component-1", "launch-old",
                                          "2026-10-13T12:00:00Z"}}}};
  ASSERT_TRUE(result.valid());
  const auto exact = result;
  result.process_occurrences.front().process_presence =
      ProcessPresenceState::kPresent;
  EXPECT_THROW(EncodeSelectedRuntimeCompositionRpcResponse(result),
               std::invalid_argument);
  result = exact;
  result.process_occurrences.front().receipt->leader_pid = 999;
  EXPECT_THROW(EncodeSelectedRuntimeCompositionRpcResponse(result),
               std::invalid_argument);
  result = exact;
  result.process_occurrences.front().receipt->launch_identity =
      "foreign-launch";
  EXPECT_THROW(EncodeSelectedRuntimeCompositionRpcResponse(result),
               std::invalid_argument);
  result = exact;
  result.process_occurrences.front().cleanup_state =
      ControllerLaunchCleanupState::kUnknown;
  EXPECT_THROW(EncodeSelectedRuntimeCompositionRpcResponse(result),
               std::invalid_argument);
  result = exact;
  result.process_occurrences.push_back(result.process_occurrences.front());
  EXPECT_THROW(EncodeSelectedRuntimeCompositionRpcResponse(result),
               std::invalid_argument);
  result = exact;
  result.process_occurrences.assign(kMaximumComponentProcessInventoryEntries +
                                        1U,
                                    exact.process_occurrences.front());
  EXPECT_THROW(EncodeSelectedRuntimeCompositionRpcResponse(result),
               std::invalid_argument);
}

TEST(
    SelectedRuntimeCompositionRpcInternal,
    InstalledReceiptReportsActualOwnerRetentionWithoutChangingTheAuthoredDelta) {
  const auto authored_retain = Request();
  auto restarted = RetainedReceipt();
  restarted.predecessor_runtime_retained = false;
  restarted.product_binding_resealed = false;
  ASSERT_TRUE(restarted.valid());
  EXPECT_TRUE(ComponentReceiptsMatchRequest(authored_retain, {restarted}));
  const ManagerSelectedRuntimeCompositionResultDTO result{
      authored_retain,
      ManagerSelectedRuntimeCompositionState::kInstalled,
      {restarted}};
  ASSERT_TRUE(result.valid());
  EXPECT_EQ(std::get<ManagerSelectedRuntimeCompositionResultDTO>(
                DecodeSelectedRuntimeCompositionRpcResponse(
                    EncodeSelectedRuntimeCompositionRpcResponse(result))),
            result);
  EXPECT_EQ(result.request.component_delta, authored_retain.component_delta);
  auto target_already_installed = authored_retain;
  auto &change = target_already_installed.component_delta.components.front();
  change.disposition =
      manyforge::deployment::ApplyComponentDisposition::kReplace;
  change.predecessor_runtime_identity = ContentDigest(std::string(kDigestTwo));
  change.replacement_reasons = {"bindings"};
  target_already_installed.configuration_fence_scope = {
      {change.controllable_component_id},
      {{ControlDomainId("arm.domain"),
        ControlDomainRevision("arm.domain.predecessor"),
        ControlDomainRevision("arm.domain.candidate")}},
      {{ControlBindingId("arm.binding"),
        ControlBindingRevision("arm.binding.predecessor"),
        ControlBindingRevision("arm.binding.candidate")}},
      {ResourceId("arm.resource")}};
  ASSERT_TRUE(change.valid());
  ASSERT_TRUE(target_already_installed.configuration_fence_scope.valid());
  auto kept = RetainedReceipt();
  kept.disposition = change.disposition;
  ASSERT_TRUE(target_already_installed.valid());
  ASSERT_TRUE(kept.valid());
  EXPECT_TRUE(ComponentReceiptsMatchRequest(target_already_installed, {kept}));
  // Re-sealing still belongs only to a genuinely retained owner.
  restarted.product_binding_resealed = true;
  EXPECT_FALSE(restarted.valid());
  kept.product_binding_resealed = false;
  EXPECT_FALSE(ComponentReceiptsMatchRequest(target_already_installed, {kept}));
  auto foreign = RetainedReceipt();
  foreign.installed_runtime_identity = ContentDigest(std::string(kDigestTwo));
  EXPECT_FALSE(ComponentReceiptsMatchRequest(authored_retain, {foreign}));
}

[[nodiscard]] ManagerPneumaticNoneProcessOwnerDTO NoneProcessOwnerProof() {
  return {CommandTargetId("gripper.target"), ControlBindingId("gripper.binding"),
          ControlBindingRevision("gripper.binding.revision")};
}

[[nodiscard]] ManagerComponentProcessOccurrenceDTO NoneInventoryRow(
    std::string component = "gripper.1") {
  return {ControllableComponentId(std::move(component)), "", 0,
          ProcessPresenceState::kAbsent, ControllerLaunchCleanupState::kNotRequested,
          std::nullopt, ContentDigest(std::string(kDigestTwo)),
          ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal, NoneProcessOwnerProof()};
}

[[nodiscard]] ManagerComponentProcessObservationDTO NoneInspectionRow() {
  return {ControllableComponentId("gripper.1"), ProcessPresenceState::kAbsent,
          ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal, NoneProcessOwnerProof(),
          ContentDigest(std::string(kDigestTwo))};
}

TEST(SelectedRuntimeCompositionRpcInternal, PhysicalInventoryDefaultPreservesExactLegacyBytes) {
  ManagerComponentProcessInventoryResultDTO result{InstalledInventoryRequest(),
      {{ControllableComponentId("component-1"), "exact-launch", 105,
        ProcessPresenceState::kUnknown, ControllerLaunchCleanupState::kUnknown}}};
  const std::string golden(
      "\x4d\x46\x4e\x53\x52\x43\x34\x00\x11\x00\x00\x00\x08\x62\x75\x6e"
      "\x64\x6c\x65\x2d\x30\x00\x00\x00\x07\x67\x72\x61\x70\x68\x2d\x30"
      "\x00\x00\x00\x47\x73\x68\x61\x32\x35\x36\x3a\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x00\x00\x00\x00\x00"
      "\x00\x00\x03\x00\x00\x00\x47\x73\x68\x61\x32\x35\x36\x3a\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x00\x00"
      "\x00\x00\x00\x00\x00\x05\x00\x00\x00\x47\x73\x68\x61\x32\x35\x36"
      "\x3a\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31\x31"
      "\x31\x00\x00\x00\x0e\x72\x75\x6e\x74\x69\x6d\x65\x2e\x6b\x65\x72"
      "\x6e\x65\x6c\x00\x00\x00\x0f\x72\x75\x6e\x74\x69\x6d\x65\x2d\x65"
      "\x70\x6f\x63\x68\x2d\x31\x00\x00\x00\x0f\x6d\x61\x6e\x61\x67\x65"
      "\x72\x2d\x65\x70\x6f\x63\x68\x2d\x31\x00\x00\x00\x01\x00\x00\x00"
      "\x0b\x63\x6f\x6d\x70\x6f\x6e\x65\x6e\x74\x2d\x31\x00\x00\x00\x0c"
      "\x65\x78\x61\x63\x74\x2d\x6c\x61\x75\x6e\x63\x68\x00\x00\x00\x69"
      "\x00\x02\x00\x00\x00",
      373U);
  EXPECT_EQ(EncodeSelectedRuntimeCompositionRpcResponse(result), golden);
  const auto decoded = std::get<ManagerComponentProcessInventoryResultDTO>(
      DecodeSelectedRuntimeCompositionRpcResponse(golden));
  EXPECT_EQ(decoded, result);
  EXPECT_EQ(decoded.process_occurrences[0].process_owner_kind,
            ManagerComponentProcessOwnerKindDTO::kControllerLaunch);
  EXPECT_FALSE(decoded.process_occurrences[0].pneumatic_none_owner);
  // Explicit default is the same old physical branch, without a new tail.
  result.process_occurrences[0].process_owner_kind =
      ManagerComponentProcessOwnerKindDTO::kControllerLaunch;
  EXPECT_EQ(EncodeSelectedRuntimeCompositionRpcResponse(result), golden);
  result.process_occurrences[0].leader_pid = 0;
  EXPECT_FALSE(result.valid());
}

TEST(SelectedRuntimeCompositionRpcInternal, NoneInventoryRoundTripsActualOwnerWithoutProcessOrDrain) {
  const ManagerComponentProcessInventoryResultDTO result{InstalledInventoryRequest(),
      {{ControllableComponentId("arm.1"), "actual-arm-launch", 101,
        ProcessPresenceState::kPresent, ControllerLaunchCleanupState::kNotRequested},
       NoneInventoryRow(),
       {ControllableComponentId("retired.1"), "retired-launch", 102,
        ProcessPresenceState::kAbsent, ControllerLaunchCleanupState::kDrained,
        ControllerLaunchGroupDrainReceipt{102, "retired.1", "retired-launch", "observed"}}}};
  ASSERT_TRUE(result.valid());
  const auto bytes = EncodeSelectedRuntimeCompositionRpcResponse(result);
  EXPECT_EQ(std::get<ManagerComponentProcessInventoryResultDTO>(
                DecodeSelectedRuntimeCompositionRpcResponse(bytes)), result);
  const auto &none = result.process_occurrences[1];
  EXPECT_TRUE(none.launch_identity.empty());
  EXPECT_EQ(none.leader_pid, 0);
  EXPECT_EQ(none.process_presence, ProcessPresenceState::kAbsent);
  EXPECT_EQ(none.cleanup_state, ControllerLaunchCleanupState::kNotRequested);
  EXPECT_FALSE(none.receipt);
  const auto request = result.request;
  const ManagerComponentProcessStopRequestDTO impossible_stop{
      {RequestId("stop-none"), RequestOwnerEpoch("owner"), "test", std::nullopt},
      request.predecessor_projection_revision, request.expected_predecessor_publication_generation,
      request.expected_predecessor_public_pointer_digest, request.runtime_unit_id,
      request.runtime_epoch, request.manager_runtime_epoch, none.controllable_component_id,
      none.launch_identity, none.leader_pid};
  EXPECT_FALSE(impossible_stop.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcRequest(impossible_stop), std::invalid_argument);
  // Valid shape is observation only: foreign scope and owner identity remain
  // distinct values for the independent Manager predecessor/installed join.
  auto foreign = result;
  foreign.request.runtime_epoch = RuntimeEpoch("foreign-host");
  foreign.process_occurrences[1].installed_runtime_identity = ContentDigest(std::string(kDigestThree));
  EXPECT_NE(std::get<ManagerComponentProcessInventoryResultDTO>(DecodeSelectedRuntimeCompositionRpcResponse(
      EncodeSelectedRuntimeCompositionRpcResponse(foreign))), result);
}

TEST(SelectedRuntimeCompositionRpcInternal, NoneRowsRejectPhysicalFactsForeignKindsAndIncompleteProof) {
  const auto original = NoneInventoryRow();
  ASSERT_TRUE(original.valid());
  for (int mutation = 0; mutation < 16; ++mutation) {
    auto row = original;
    switch (mutation) {
      case 0: row.process_owner_kind = static_cast<ManagerComponentProcessOwnerKindDTO>(255); break;
      case 1: row.process_owner_kind = ManagerComponentProcessOwnerKindDTO::kControllerLaunch; break;
      case 2: row.launch_identity = "fake-launch"; break;
      case 3: row.leader_pid = 123; break;
      case 4: row.process_presence = ProcessPresenceState::kPresent; break;
      case 5: row.process_presence = ProcessPresenceState::kUnknown; break;
      case 6: row.cleanup_state = ControllerLaunchCleanupState::kDrained; break;
      case 7: row.cleanup_state = ControllerLaunchCleanupState::kPending; break;
      case 8: row.receipt = ControllerLaunchGroupDrainReceipt{123, "gripper.1", "fake", "now"}; break;
      case 9: row.installed_runtime_identity.reset(); break;
      case 10: row.installed_runtime_identity = ContentDigest("not-a-digest"); break;
      case 11: row.pneumatic_none_owner.reset(); break;
      case 12: row.pneumatic_none_owner->command_target_id = CommandTargetId(""); break;
      case 13: row.pneumatic_none_owner->control_binding_id = ControlBindingId(""); break;
      case 14: row.pneumatic_none_owner->control_binding_revision = ControlBindingRevision(std::string(kMaxLifecycleIdentityBytes + 1, 'x')); break;
      case 15: row.pneumatic_none_owner->command_target_id = CommandTargetId(std::string("target\0foreign", 14)); break;
    }
    EXPECT_FALSE(row.valid()) << mutation;
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(
        ManagerComponentProcessInventoryResultDTO{InstalledInventoryRequest(), {row}}),
        std::invalid_argument) << mutation;
  }
  auto duplicate = ManagerComponentProcessInventoryResultDTO{InstalledInventoryRequest(), {original, original}};
  EXPECT_FALSE(duplicate.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(duplicate), std::invalid_argument);
  auto physical = original;
  physical.process_owner_kind = ManagerComponentProcessOwnerKindDTO::kControllerLaunch;
  physical.launch_identity = "actual-launch";
  physical.leader_pid = 42;
  physical.process_presence = ProcessPresenceState::kPresent;
  EXPECT_FALSE(physical.valid()); // None proof cannot accompany physical mode.
  physical.pneumatic_none_owner.reset();
  EXPECT_TRUE(physical.valid());
}

TEST(SelectedRuntimeCompositionRpcInternal, CandidateNoneObservationIsActualAbsentOwnerNeverCandidateInstallation) {
  auto request = InspectionRequest();
  request.planning_component_id = ControllableComponentId("arm.1");
  const auto facts = PlanningFacts();
  ManagerCellObserverObservationDTO observer{CellId("cell"), "source", "base",
      ProcessPresenceState::kPresent};
  const ManagerComponentBringUpInspectionResultDTO result{request,
      {{ControllableComponentId("arm.1"), ProcessPresenceState::kPresent}, NoneInspectionRow()}, observer, facts};
  ASSERT_TRUE(result.valid());
  const auto exact = EncodeSelectedRuntimeCompositionRpcResponse(result);
  EXPECT_EQ(std::get<ManagerComponentBringUpInspectionResultDTO>(DecodeSelectedRuntimeCompositionRpcResponse(exact)), result);
  for (int mutation = 0; mutation < 7; ++mutation) {
    auto wrong = result;
    auto &row = wrong.configured_components[1];
    if (mutation == 0) row.process_presence = ProcessPresenceState::kPresent;
    if (mutation == 1) row.process_presence = ProcessPresenceState::kUnknown;
    if (mutation == 2) row.pneumatic_none_owner.reset();
    if (mutation == 3) row.installed_runtime_identity.reset();
    if (mutation == 4) row.installed_runtime_identity = ContentDigest("foreign");
    if (mutation == 5) row.process_owner_kind = static_cast<ManagerComponentProcessOwnerKindDTO>(255);
    if (mutation == 6) row.process_owner_kind = ManagerComponentProcessOwnerKindDTO::kControllerLaunch;
    EXPECT_FALSE(wrong.valid()) << mutation;
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(wrong), std::invalid_argument) << mutation;
  }
  // A new candidate with no actual installed owner is plain absent. Removing
  // optional owner evidence cannot turn process absence into presence/readiness.
  const ManagerComponentBringUpInspectionResultDTO no_owner{InspectionRequest(),
      {{ControllableComponentId("gripper.1"), ProcessPresenceState::kAbsent}}};
  ASSERT_TRUE(no_owner.valid());
  const auto ordinary = std::get<ManagerComponentBringUpInspectionResultDTO>(
      DecodeSelectedRuntimeCompositionRpcResponse(EncodeSelectedRuntimeCompositionRpcResponse(no_owner)));
  EXPECT_EQ(ordinary, no_owner);
  EXPECT_EQ(ordinary.configured_components[0].process_owner_kind, ManagerComponentProcessOwnerKindDTO::kControllerLaunch);
  EXPECT_FALSE(ordinary.configured_components[0].installed_runtime_identity);
}

TEST(SelectedRuntimeCompositionRpcInternal, NoneOwnerTailsAreClosedIndexedAndResultDirectionOnly) {
  constexpr std::string_view installed_magic = "MFNSRC4-INSTALLED-PNEUMATIC-NONE-V1";
  constexpr std::string_view inspection_magic = "MFNSRC4-INSPECT-PNEUMATIC-NONE-V1";
  const ManagerComponentProcessInventoryResultDTO result{InstalledInventoryRequest(),
      {NoneInventoryRow("gripper.1"), NoneInventoryRow("gripper.2")}};
  const auto bytes = EncodeSelectedRuntimeCompositionRpcResponse(result);
  const auto magic_at = bytes.find(installed_magic);
  ASSERT_NE(magic_at, std::string::npos);
  const auto tail_at = magic_at - 5U;
  const auto count_at = magic_at + installed_magic.size();
  const auto first_index = count_at + 4U;
  const auto kind_at = first_index + 4U;
  const auto proof = *result.process_occurrences[0].pneumatic_none_owner;
  const auto second_index = kind_at + 1U + 12U + proof.command_target_id.value().size() +
      proof.control_binding_id.value().size() + proof.control_binding_revision.value().size();
  const auto set_u32 = [](std::string &data, const std::size_t at, std::uint32_t value) {
    for (int i = 3; i >= 0; --i) { data[at + i] = static_cast<char>(value & 255U); value >>= 8U; }
  };
  const auto rejects = [](const std::string &data) {
    EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(data), std::invalid_argument);
  };
  for (auto cut = tail_at; cut < bytes.size(); ++cut) rejects(bytes.substr(0U, cut));
  auto bad = bytes;
  bad[kind_at] = static_cast<char>(255); rejects(bad);
  bad[kind_at] = 0; rejects(bad); // A redundant physical tail is not canonical.
  bad = bytes; set_u32(bad, count_at, 0); rejects(bad);
  bad = bytes; set_u32(bad, count_at, kMaximumComponentProcessInventoryEntries + 1U); rejects(bad);
  bad = bytes; set_u32(bad, first_index, 2); rejects(bad);
  bad = bytes; set_u32(bad, second_index, 0); rejects(bad);
  bad = bytes; set_u32(bad, first_index, 1); set_u32(bad, second_index, 0); rejects(bad);
  bad = bytes; bad[tail_at] = 5; rejects(bad);
  bad = bytes; bad[magic_at] = 'X'; rejects(bad);
  bad = bytes; bad[bad.find("gripper.target")] = '\0'; rejects(bad);
  rejects(bytes + bytes.substr(tail_at));
  const auto request = EncodeSelectedRuntimeCompositionRpcRequest(result.request);
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(request + bytes.substr(tail_at)), std::invalid_argument);
  const ManagerComponentBringUpInspectionResultDTO inspection{InspectionRequest(), {NoneInspectionRow()}};
  const auto inspection_bytes = EncodeSelectedRuntimeCompositionRpcResponse(inspection);
  const auto inspect_magic_at = inspection_bytes.find(inspection_magic);
  ASSERT_NE(inspect_magic_at, std::string::npos);
  const auto inspect_tail = inspect_magic_at - 5U;
  const auto inspect_count = inspect_magic_at + inspection_magic.size();
  bad = inspection_bytes; bad[inspect_count + 8U] = static_cast<char>(255); rejects(bad);
  bad = inspection_bytes; set_u32(bad, inspect_count, 0); rejects(bad);
  bad = inspection_bytes; set_u32(bad, inspect_count + 4U, 1); rejects(bad);
  rejects(inspection_bytes.substr(0, inspection_bytes.size() - 1U));
  const ManagerComponentBringUpInspectionResultDTO plain{InspectionRequest(),
      {{ControllableComponentId("gripper.1"), ProcessPresenceState::kAbsent}}};
  rejects(EncodeSelectedRuntimeCompositionRpcResponse(plain) + bytes.substr(tail_at));
  rejects(bytes.substr(0, tail_at) + inspection_bytes.substr(inspect_tail));
  rejects(inspection_bytes + inspection_bytes.substr(inspect_tail));
  const auto stripped = std::get<ManagerComponentBringUpInspectionResultDTO>(
      DecodeSelectedRuntimeCompositionRpcResponse(inspection_bytes.substr(0, inspect_tail)));
  EXPECT_EQ(stripped, plain);
}

TEST(SelectedRuntimeCompositionRpcInternal, NoneOwnerProofAndPacketBoundsRemainStrict) {
  auto proof = NoneProcessOwnerProof();
  proof.command_target_id = CommandTargetId(std::string(kMaxLifecycleIdentityBytes, 't'));
  proof.control_binding_id = ControlBindingId(std::string(kMaxLifecycleIdentityBytes, 'b'));
  proof.control_binding_revision = ControlBindingRevision(std::string(kMaxLifecycleIdentityBytes, 'r'));
  EXPECT_TRUE(proof.valid());
  proof.control_binding_id = ControlBindingId(std::string(kMaxLifecycleIdentityBytes + 1U, 'b'));
  EXPECT_FALSE(proof.valid());
  auto row = NoneInventoryRow();
  row.controllable_component_id = ControllableComponentId(std::string(kMaxLifecycleIdentityBytes + 1U, 'c'));
  EXPECT_FALSE(row.valid());
  ManagerComponentProcessInventoryResultDTO inventory{InstalledInventoryRequest(), {NoneInventoryRow()}};
  inventory.process_occurrences.resize(kMaximumComponentProcessInventoryEntries + 1U, NoneInventoryRow());
  EXPECT_FALSE(inventory.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(inventory), std::invalid_argument);
  inventory.process_occurrences.clear();
  for (std::size_t index = 0; index < kMaximumComponentProcessInventoryEntries; ++index) {
    inventory.process_occurrences.push_back(NoneInventoryRow("component-" + std::to_string(10000U + index)));
  }
  ASSERT_TRUE(inventory.valid());
  EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(inventory), std::invalid_argument);
  std::string oversized(kMaximumSelectedRuntimeCompositionRpcPacketBytes + 1U, 'x');
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(oversized), std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     ProviderScopeUsesExplicitEnvelopeAndRejectsVersionAliases) {
  auto request = Request();
  const ProviderConfigurationFenceOccurrenceDTO occurrence{
      ControllableComponentId("arm.1"),
      ProviderInstanceId("provider.io"),
      ContentDigest(std::string(kDigestOne)),
      {}};
  request.configuration_fence_scope = {
      {occurrence.controllable_component_id},
      {{ControlDomainId("arm.domain"), ControlDomainRevision("old"),
        ControlDomainRevision("new"),
        ProviderConfigurationFenceDomainDTO{occurrence, occurrence}}},
      {},
      {}};
  ASSERT_TRUE(request.valid());
  const auto packet = EncodeSelectedRuntimeCompositionRpcRequest(request);
  EXPECT_EQ(packet.substr(0U, 8U), std::string("MFNSRC5\0", 8U));
  EXPECT_TRUE(IsSelectedRuntimeCompositionRpcPacket(packet));
  EXPECT_EQ(DecodeSelectedRuntimeCompositionRpcRequest(packet), request);
  auto alias = packet;
  alias[6] = '4';
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequest(alias),
               std::invalid_argument);
  const ManagerSelectedRuntimeCompositionResultDTO result{
      request, ManagerSelectedRuntimeCompositionState::kPreparing, {}};
  const auto reply = EncodeSelectedRuntimeCompositionRpcResponse(result);
  EXPECT_EQ(reply.substr(0U, 8U), std::string("MFNSRC5\0", 8U));
  EXPECT_EQ(std::get<ManagerSelectedRuntimeCompositionResultDTO>(
                DecodeSelectedRuntimeCompositionRpcResponse(reply)),
            result);
  const auto legacy = EncodeSelectedRuntimeCompositionRpcRequest(Request());
  EXPECT_EQ(legacy.substr(0U, 8U), std::string("MFNSRC4\0", 8U));
  alias = legacy;
  alias[6] = '5';
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequest(alias),
               std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal,
     ProviderPlanningFactsRetainTrueDomainWithoutControllerIdentity) {
  auto request = InspectionRequest();
  request.planning_component_id = ControllableComponentId("arm.1");
  auto facts = PlanningFacts();
  const ProviderControlDomainDTO provider{
      ControllableComponentId("arm.1"),
      RuntimeUnitId("runtime.io"),
      ProviderInstanceId("provider.io"),
      ProviderManifestId("manyforge.provider.io_signals"),
      ProviderManifestRevision(std::string(kDigestOne)),
      ContentDigest(std::string(kDigestTwo)),
      ContentDigest(std::string(kDigestThree)),
      {"ready"},
      30'000'000'000};
  facts.candidate_expected_domains = {ExpectedControlDomainRevisionDTO{
      ControlDomainId("io.domain"),
      ControlDomainRevision(std::string(kDigestOne)),
      ExpectedProviderDomainScopeDTO{
          provider,
          {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt,
           std::nullopt}}}};
  const ManagerComponentBringUpInspectionResultDTO result{
      request,
      {{ControllableComponentId("arm.1"), ProcessPresenceState::kAbsent}},
      std::nullopt,
      facts};
  ASSERT_TRUE(result.valid());
  const auto packet = EncodeSelectedRuntimeCompositionRpcResponse(result);
  EXPECT_EQ(packet.substr(0U, 8U), std::string("MFNSRC5\0", 8U));
  const auto decoded = DecodeSelectedRuntimeCompositionRpcResponse(packet);
  EXPECT_EQ(std::get<ManagerComponentBringUpInspectionResultDTO>(decoded),
            result);
  auto alias = packet;
  alias[6] = '4';
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(alias),
               std::invalid_argument);
}

} // namespace
} // namespace manyforge::component_manager::detail
