#include <gtest/gtest.h>

#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <map>
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

[[nodiscard]] ManagerComponentProcessInventoryRequestDTO
InstalledInventoryRequest() {
  return {ProjectionRevisionDTO{
              RetainedBundleId("bundle-0"),
              GraphRevision("graph-0"),
              SceneRevision{SceneId(std::string(kDigestOne)), 3U,
                            ContentDigest(std::string(kDigestOne))},
          },
          ActiveBundlePublicationGeneration(5U),
          ContentDigest(std::string(kDigestOne)),
          RuntimeUnitId("runtime.kernel"),
          RuntimeEpoch("runtime-epoch-1"),
          RuntimeEpoch("manager-epoch-1")};
}

[[nodiscard]] HostApplyRequest HostRequest() {
  return {NamedApplyPublicationTransactionDTO{ApplyId("apply-1"), ApplyGeneration(2U)},
          RetainedBundleId("bundle-1"),
          GraphRevision("graph-1"),
          {ControllableComponentId("arm.1"), ControllableComponentId("gripper.1"),
           ControllableComponentId("io.1")},
          {ControllableComponentId("arm.1")},
          {ControllableComponentId("camera.1")},
          {ControlDomainId("camera.domain")}};
}

TEST(SelectedRuntimeCompositionRpcInternal, HostApplyRequestRoundTripsThroughTheRequestVariant) {
  const auto request = HostRequest();
  ASSERT_TRUE(request.valid());
  const auto encoded = EncodeSelectedRuntimeCompositionRpcRequest(request);
  EXPECT_TRUE(IsSelectedRuntimeCompositionRpcPacket(encoded));
  EXPECT_EQ(encoded.substr(0U, 8U), std::string("MFNSRC6\0", 8U));
  const auto decoded = DecodeSelectedRuntimeCompositionRpcRequestVariant(encoded);
  ASSERT_TRUE(std::holds_alternative<HostApplyRequest>(decoded));
  EXPECT_EQ(std::get<HostApplyRequest>(decoded), request);
  // Empty sets (an Apply of the empty cell over an empty cell) are valid.
  auto empty = request;
  empty.components.clear();
  empty.retained_components.clear();
  empty.removed_components.clear();
  empty.removed_domains.clear();
  ASSERT_TRUE(empty.valid());
  EXPECT_EQ(std::get<HostApplyRequest>(DecodeSelectedRuntimeCompositionRpcRequestVariant(
                EncodeSelectedRuntimeCompositionRpcRequest(empty))),
            empty);
  auto trailing = encoded;
  trailing.push_back('\0');
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(trailing),
               std::invalid_argument);
  auto old_version = encoded;
  old_version[6] = '4';
  EXPECT_FALSE(IsSelectedRuntimeCompositionRpcPacket(old_version));
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcRequestVariant(old_version),
               std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal, HostApplyRequestRefusesNoncanonicalSets) {
  const auto refused = [](const HostApplyRequest &request) {
    EXPECT_FALSE(request.valid());
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcRequest(request),
                 std::invalid_argument);
  };
  auto unsorted = HostRequest();
  std::swap(unsorted.components.front(), unsorted.components.back());
  refused(unsorted);
  auto duplicate = HostRequest();
  duplicate.components.insert(duplicate.components.begin(), duplicate.components.front());
  refused(duplicate);
  auto retained_outside = HostRequest();
  retained_outside.retained_components = {ControllableComponentId("other.1")};
  refused(retained_outside);
  auto removed_inside = HostRequest();
  removed_inside.removed_components = {ControllableComponentId("gripper.1")};
  refused(removed_inside);
  auto empty_id = HostRequest();
  empty_id.removed_domains = {ControlDomainId("")};
  refused(empty_id);
  auto no_apply = HostRequest();
  no_apply.apply = NamedApplyPublicationTransactionDTO{ApplyId(""), ApplyGeneration(2U)};
  refused(no_apply);
  auto zero_generation = HostRequest();
  zero_generation.apply.apply_generation = ApplyGeneration(0U);
  refused(zero_generation);
  auto no_bundle = HostRequest();
  no_bundle.candidate_retained_bundle_id = RetainedBundleId("");
  refused(no_bundle);
}

TEST(SelectedRuntimeCompositionRpcInternal, HostApplyResultRoundTripsBothOutcomes) {
  const HostApplyResult succeeded{HostApplyOutcome::kSucceeded, std::nullopt};
  const HostApplyResult failed{
      HostApplyOutcome::kFailed,
      ErrorDTO{"apply_requires_inactive_component",
               "configuration Apply cannot replace or remove enabled component arm.1",
               {{"component_id", "arm.1"}}}};
  for (const auto &result : {succeeded, failed}) {
    ASSERT_TRUE(result.valid());
    const auto bytes = EncodeSelectedRuntimeCompositionRpcResponse(result);
    EXPECT_TRUE(IsSelectedRuntimeCompositionRpcPacket(bytes));
    const auto decoded = DecodeSelectedRuntimeCompositionRpcResponse(bytes);
    ASSERT_TRUE(std::holds_alternative<HostApplyResult>(decoded));
    EXPECT_EQ(std::get<HostApplyResult>(decoded), result);
  }
  const HostApplyResult succeeded_with_error{HostApplyOutcome::kSucceeded, failed.error};
  const HostApplyResult failed_without_error{HostApplyOutcome::kFailed, std::nullopt};
  const HostApplyResult failed_with_empty_error{HostApplyOutcome::kFailed, ErrorDTO{"", "", {}}};
  for (const auto &invalid : {succeeded_with_error, failed_without_error, failed_with_empty_error}) {
    EXPECT_FALSE(invalid.valid());
    EXPECT_THROW((void)EncodeSelectedRuntimeCompositionRpcResponse(invalid),
                 std::invalid_argument);
  }
  auto unknown_outcome = EncodeSelectedRuntimeCompositionRpcResponse(succeeded);
  unknown_outcome[9] = 2;  // magic (8) + tag (1), then the outcome byte
  EXPECT_THROW((void)DecodeSelectedRuntimeCompositionRpcResponse(unknown_outcome),
               std::invalid_argument);
}

TEST(SelectedRuntimeCompositionRpcInternal, HostApplyTimeoutSumsBudgetsOfInstalledAndRemovedComponents) {
  const auto request = HostRequest();
  const std::map<std::string, std::int64_t, std::less<>> budgets{
      {"arm.1", 50'000'000'000LL}, {"gripper.1", 3'000'000'000LL}};
  constexpr auto kDefaultMs = kDefaultComponentInstallBudgetNs / 1'000'000;
  constexpr auto kGraceMs = manyforge::core::kControllerLaunchStopGrace.count();
  // arm.1 is retained; gripper.1 uses its budget, io.1 the default, and the
  // removed camera.1 (absent from the candidate's budgets) the default; each
  // of the three adds the host's stop grace.
  EXPECT_EQ(HostApplyTimeout(request, budgets),
            std::chrono::milliseconds(3'000 + 2 * kDefaultMs + 3 * kGraceMs));
  // A remove-only Apply waits for every removed component's stop.
  auto remove_only = request;
  remove_only.components = {ControllableComponentId("arm.1")};
  remove_only.removed_components = {ControllableComponentId("camera.1"),
                                    ControllableComponentId("camera.2")};
  ASSERT_TRUE(remove_only.valid());
  EXPECT_EQ(HostApplyTimeout(remove_only, budgets),
            std::chrono::milliseconds(2 * (kDefaultMs + kGraceMs)));
  // An Apply that installs and removes nothing waits one default budget.
  auto retain_only = remove_only;
  retain_only.removed_components.clear();
  EXPECT_EQ(HostApplyTimeout(retain_only, budgets), std::chrono::milliseconds(kDefaultMs));
}

TEST(SelectedRuntimeCompositionRpcInternal, HostApplyTimeoutCoversALoneHungComponentsRetirement) {
  // New after a one-arm Scenario whose arm node hangs. The host waits
  // the stop grace, then SIGKILLs and reaps the group: the bound must exceed
  // that, or the Manager shuts the execution host down mid-Apply.
  auto request = HostRequest();
  request.components = {};
  request.retained_components = {};
  request.removed_components = {ControllableComponentId("arm.1")};
  ASSERT_TRUE(request.valid());
  const auto bound = HostApplyTimeout(request, {});
  // Grace, then a kill and reap of a few ms; one default budget of margin.
  EXPECT_GE(bound, manyforge::core::kControllerLaunchStopGrace +
                       std::chrono::nanoseconds(kDefaultComponentInstallBudgetNs));
  // The same for a lone replaced arm (its predecessor launch retires first).
  auto replace = HostRequest();
  replace.components = {ControllableComponentId("arm.1")};
  replace.retained_components = {};
  replace.removed_components = {};
  ASSERT_TRUE(replace.valid());
  EXPECT_GT(HostApplyTimeout(replace, {}), manyforge::core::kControllerLaunchStopGrace);
}

// A component whose launch processes are all drained process
// groups (cleanup-proven) is gone and is removed or replaced without a clean
// Disable. No occurrence, an exited-but-not-drained leader, a live member or a
// local owner is not gone.
TEST(SelectedRuntimeCompositionRpcInternal, ComponentLaunchProcessesDrainedOnlyForDrainedLaunchGroups) {
  const ControllableComponentId arm("arm");
  const auto occurrence = [](std::string component, ProcessPresenceState presence,
                             ControllerLaunchCleanupState cleanup) {
    return ManagerComponentProcessOccurrenceDTO{
        ControllableComponentId(std::move(component)), "launch", 42, presence, cleanup};
  };
  const auto drained = occurrence("arm", ProcessPresenceState::kAbsent, ControllerLaunchCleanupState::kDrained);
  // Other components' rows are ignored.
  EXPECT_TRUE(ComponentLaunchProcessesDrained(
      arm, {drained, occurrence("gripper", ProcessPresenceState::kPresent,
                                ControllerLaunchCleanupState::kNotRequested)}));
  auto second = drained;
  second.launch_identity = "launch.2";
  EXPECT_TRUE(ComponentLaunchProcessesDrained(arm, {drained, second}));
  // No launch occurrence of this component.
  EXPECT_FALSE(ComponentLaunchProcessesDrained(arm, {}));
  EXPECT_FALSE(ComponentLaunchProcessesDrained(
      arm, {occurrence("gripper", ProcessPresenceState::kAbsent, ControllerLaunchCleanupState::kDrained)}));
  // An exited leader is not a drained group: a member may still be alive.
  EXPECT_FALSE(ComponentLaunchProcessesDrained(
      arm, {occurrence("arm", ProcessPresenceState::kAbsent, ControllerLaunchCleanupState::kNotRequested)}));
  EXPECT_FALSE(ComponentLaunchProcessesDrained(
      arm, {occurrence("arm", ProcessPresenceState::kAbsent, ControllerLaunchCleanupState::kPending)}));
  EXPECT_FALSE(ComponentLaunchProcessesDrained(
      arm, {drained, occurrence("arm", ProcessPresenceState::kPresent,
                                ControllerLaunchCleanupState::kNotRequested)}));
  // A local no-output owner is live, never gone.
  auto local = drained;
  local.process_owner_kind = ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal;
  EXPECT_FALSE(ComponentLaunchProcessesDrained(arm, {local}));
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
    const auto existing = InstalledInventoryRequest();
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
  const auto existing = InstalledInventoryRequest();
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
  const auto existing = InstalledInventoryRequest();
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

  auto request = EncodeSelectedRuntimeCompositionRpcRequest(HostRequest());
  request.push_back('\0');
  EXPECT_THROW(
      static_cast<void>(DecodeSelectedRuntimeCompositionRpcRequestVariant(request)),
      std::invalid_argument);
  EXPECT_THROW(
      static_cast<void>(DecodeSelectedRuntimeCompositionRpcRequestVariant(std::string(
          kMaximumSelectedRuntimeCompositionRpcPacketBytes + 1U, 'x'))),
      std::invalid_argument);
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

TEST(SelectedRuntimeCompositionRpcInternal, PhysicalInventoryDefaultPreservesExactLegacyBytes) {
  ManagerComponentProcessInventoryResultDTO result{InstalledInventoryRequest(),
      {{ControllableComponentId("component-1"), "exact-launch", 105,
        ProcessPresenceState::kUnknown, ControllerLaunchCleanupState::kUnknown}}};
  const std::string golden(
      "\x4d\x46\x4e\x53\x52\x43\x36\x00\x11\x00\x00\x00\x08\x62\x75\x6e"
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

TEST(SelectedRuntimeCompositionRpcInternal, NoneOwnerTailsAreClosedIndexedAndResultDirectionOnly) {
  constexpr std::string_view installed_magic = "MFNSRC4-INSTALLED-PNEUMATIC-NONE-V1";
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
     InstalledLifecycleFactsRoundTripAndRejectAmbiguity) {
  ManagerComponentProcessInventoryResultDTO result{
      InstalledInventoryRequest(),
      {},
      std::nullopt,
      {{ControllableComponentId("component-1"),
        ComponentLifecycleGeneration(3U),
        ComponentLifecycleStateDTO::kActive}}};
  result.request.include_component_lifecycles = true;
  ASSERT_TRUE(result.valid());
  EXPECT_EQ(
      std::get<ManagerComponentProcessInventoryRequestDTO>(
          DecodeSelectedRuntimeCompositionRpcRequestVariant(
              EncodeSelectedRuntimeCompositionRpcRequest(result.request))),
      result.request);
  EXPECT_EQ(std::get<ManagerComponentProcessInventoryResultDTO>(
                DecodeSelectedRuntimeCompositionRpcResponse(
                    EncodeSelectedRuntimeCompositionRpcResponse(result))),
            result);
  auto invalid = result;
  invalid.component_lifecycles.push_back(invalid.component_lifecycles.front());
  EXPECT_FALSE(invalid.valid());
  invalid = result;
  invalid.component_lifecycles.front().state =
      static_cast<ComponentLifecycleStateDTO>(255U);
  EXPECT_FALSE(invalid.valid());
  EXPECT_THROW(EncodeSelectedRuntimeCompositionRpcResponse(invalid),
               std::invalid_argument);
  result.component_lifecycles.clear();
  result.request.include_component_lifecycles = false;
  const auto old_bytes = EncodeSelectedRuntimeCompositionRpcResponse(result);
  EXPECT_EQ(EncodeSelectedRuntimeCompositionRpcResponse(
                std::get<ManagerComponentProcessInventoryResultDTO>(
                    DecodeSelectedRuntimeCompositionRpcResponse(old_bytes))),
            old_bytes);
}

} // namespace
} // namespace manyforge::component_manager::detail
