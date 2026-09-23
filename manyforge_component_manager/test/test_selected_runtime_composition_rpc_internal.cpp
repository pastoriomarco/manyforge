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
  };
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
      const ManagerSelectedRuntimeCompositionResultDTO result{request, state};
      const auto decoded = DecodeSelectedRuntimeCompositionRpcResponse(
          EncodeSelectedRuntimeCompositionRpcResponse(result));
      const auto *round_trip =
          std::get_if<ManagerSelectedRuntimeCompositionResultDTO>(&decoded);
      ASSERT_NE(round_trip, nullptr);
      EXPECT_EQ(*round_trip, result);
    }
  }
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
