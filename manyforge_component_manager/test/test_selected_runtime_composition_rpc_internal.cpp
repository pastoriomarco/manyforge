#include <gtest/gtest.h>

#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/types/error_code.hpp>

#include <stdexcept>
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

[[nodiscard]] ManagerRuntimeComponentReceiptDTO RetainedReceipt() {
  return {ControllableComponentId("arm.1"),
          manyforge::deployment::ApplyComponentDisposition::kRetain,
          ContentDigest(std::string(kDigestThree)),
          {{RuntimeUnitId("runtime.kernel"), RuntimeEpoch("runtime-epoch-1")}},
          true,
          true};
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

} // namespace
} // namespace manyforge::component_manager::detail
