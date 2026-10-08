#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "managed_projection_registry.hpp"

namespace manyforge::roboplan_backend {
namespace {

namespace component_manager = manyforge::component_manager;

[[nodiscard]] ManagedProjectionCandidate
Candidate(const std::string &bundle) {
  return ManagedProjectionCandidate{
      core::RetainedBundleId(bundle), core::GraphRevision("graph-" + bundle),
      core::RetainedProductDTO{
          core::ProductId("planning"),
          core::RetainedProductKind::kPlanningCollisionModel,
          core::ProductContractId("contract"),
          core::ProductContractRevision("contract-revision"),
          {}}};
}

/// Verified inputs of the fake candidate: the provider->group table that the
/// production model map supplies.
struct FakeInputs final {
  std::string bundle;
  std::vector<std::pair<std::string, std::string>> provider_groups;
};

/// Shared immutable model; weak observers prove when the cache releases it.
using FakeModel = std::shared_ptr<const std::string>;

struct FakePreparation final {
  int verifications{};
  int group_selections{};
  std::vector<std::string> constructed_groups;

  [[nodiscard]] auto Verify(const std::string &bundle) {
    return [this, bundle] {
      ++verifications;
      return FakeInputs{bundle,
                        {{"arm-a", "group-left"},
                         {"gripper-a", "group-left"},
                         {"arm-b", "group-right"}}};
    };
  }

  /// Per-request provider binding check plus group selection.
  [[nodiscard]] auto Select(const std::string &provider) {
    return [this, provider](const FakeInputs &inputs) {
      ++group_selections;
      for (const auto &[name, group] : inputs.provider_groups) {
        if (name == provider) {
          return group;
        }
      }
      throw std::runtime_error("provider is not bound by " + inputs.bundle);
    };
  }

  [[nodiscard]] auto Construct() {
    return [this](const FakeInputs &inputs, const std::string &group) {
      constructed_groups.push_back(inputs.bundle + "/" + group);
      return std::make_shared<const std::string>(inputs.bundle + "/" + group);
    };
  }
};

using FakeCache = ManagedCandidateProjectionCache<FakeInputs, FakeModel>;

TEST(ManagedCandidateProjectionCache,
     SharedPreparationRunsOncePerCandidateAndGroupAcrossProviders) {
  FakeCache cache;
  FakePreparation preparation;
  const auto candidate = Candidate("bundle-1");

  const auto arm_a = cache.Acquire(candidate, preparation.Verify("bundle-1"),
                                   preparation.Select("arm-a"),
                                   preparation.Construct());
  const auto gripper_a = cache.Acquire(
      candidate, preparation.Verify("bundle-1"),
      preparation.Select("gripper-a"), preparation.Construct());
  const auto arm_b = cache.Acquire(candidate, preparation.Verify("bundle-1"),
                                   preparation.Select("arm-b"),
                                   preparation.Construct());

  EXPECT_EQ(preparation.verifications, 1);
  EXPECT_EQ(preparation.constructed_groups,
            (std::vector<std::string>{"bundle-1/group-left",
                                      "bundle-1/group-right"}));
  // Providers of one group share the same immutable model instance.
  EXPECT_EQ(arm_a, gripper_a);
  EXPECT_EQ(*arm_b, "bundle-1/group-right");
}

TEST(ManagedCandidateProjectionCache, ProviderChecksRunOnEveryRequest) {
  FakeCache cache;
  FakePreparation preparation;
  const auto candidate = Candidate("bundle-1");

  static_cast<void>(cache.Acquire(candidate, preparation.Verify("bundle-1"),
                                  preparation.Select("arm-a"),
                                  preparation.Construct()));
  static_cast<void>(cache.Acquire(candidate, preparation.Verify("bundle-1"),
                                  preparation.Select("arm-a"),
                                  preparation.Construct()));
  EXPECT_EQ(preparation.group_selections, 2);

  // A cached candidate never admits a provider its bundle does not bind.
  EXPECT_THROW(static_cast<void>(cache.Acquire(
                   candidate, preparation.Verify("bundle-1"),
                   preparation.Select("foreign-provider"),
                   preparation.Construct())),
               std::runtime_error);
  EXPECT_EQ(preparation.group_selections, 3);
  // The refusal neither re-verifies nor discards the candidate.
  static_cast<void>(cache.Acquire(candidate, preparation.Verify("bundle-1"),
                                  preparation.Select("arm-b"),
                                  preparation.Construct()));
  EXPECT_EQ(preparation.verifications, 1);
  EXPECT_EQ(preparation.group_selections, 4);
}

TEST(ManagedCandidateProjectionCache, DifferentCandidateEvictsTheCache) {
  FakeCache cache;
  FakePreparation preparation;
  // The caller drops its handle at once; the cache keeps the model alive.
  const std::weak_ptr<const std::string> first_model =
      cache.Acquire(Candidate("bundle-1"), preparation.Verify("bundle-1"),
                    preparation.Select("arm-a"), preparation.Construct());
  ASSERT_FALSE(first_model.expired());

  static_cast<void>(cache.Acquire(Candidate("bundle-2"),
                                  preparation.Verify("bundle-2"),
                                  preparation.Select("arm-a"),
                                  preparation.Construct()));
  EXPECT_TRUE(first_model.expired());
  EXPECT_EQ(preparation.verifications, 2);

  // Exactly one candidate is held: returning to the first verifies again.
  static_cast<void>(cache.Acquire(Candidate("bundle-1"),
                                  preparation.Verify("bundle-1"),
                                  preparation.Select("arm-a"),
                                  preparation.Construct()));
  EXPECT_EQ(preparation.verifications, 3);

  // Any identity field selects a different candidate.
  auto revised = Candidate("bundle-1");
  revised.planning_collision_model.product_contract_revision =
      core::ProductContractRevision("contract-revision-2");
  static_cast<void>(cache.Acquire(revised, preparation.Verify("bundle-1"),
                                  preparation.Select("arm-a"),
                                  preparation.Construct()));
  EXPECT_EQ(preparation.verifications, 4);
}

TEST(ManagedCandidateProjectionCache, ReleaseDropsTheCandidate) {
  FakeCache cache;
  FakePreparation preparation;
  std::weak_ptr<const std::string> model;
  {
    auto acquired =
        cache.Acquire(Candidate("bundle-1"), preparation.Verify("bundle-1"),
                      preparation.Select("arm-a"), preparation.Construct());
    model = acquired;
  }
  ASSERT_FALSE(model.expired());

  cache.Release();
  EXPECT_TRUE(model.expired());

  static_cast<void>(cache.Acquire(Candidate("bundle-1"),
                                  preparation.Verify("bundle-1"),
                                  preparation.Select("arm-a"),
                                  preparation.Construct()));
  EXPECT_EQ(preparation.verifications, 2);
  EXPECT_EQ(preparation.constructed_groups.size(), 2U);
}

TEST(ManagedCandidateProjectionCache, FailedVerificationRetainsNothing) {
  FakeCache cache;
  FakePreparation preparation;
  int attempts = 0;
  EXPECT_THROW(static_cast<void>(cache.Acquire(
                   Candidate("bundle-1"),
                   [&]() -> FakeInputs {
                     ++attempts;
                     throw std::runtime_error("unverifiable");
                   },
                   preparation.Select("arm-a"), preparation.Construct())),
               std::runtime_error);
  static_cast<void>(cache.Acquire(Candidate("bundle-1"),
                                  preparation.Verify("bundle-1"),
                                  preparation.Select("arm-a"),
                                  preparation.Construct()));
  EXPECT_EQ(attempts, 1);
  EXPECT_EQ(preparation.verifications, 1);
}

TEST(ManagedCandidateProjectionCache,
     ConcurrentRequestsForOneCandidateShareOneVerification) {
  FakeCache cache;
  std::atomic<int> verifications{0};
  std::latch verifying(1);
  std::promise<void> release;
  auto released = release.get_future().share();
  const auto verify = [&] {
    ++verifications;
    verifying.count_down();
    released.wait();
    return FakeInputs{"bundle-1", {{"arm-a", "group-left"},
                                   {"arm-b", "group-right"}}};
  };
  const auto select = [](const std::string &provider) {
    return [provider](const FakeInputs &inputs) {
      for (const auto &[name, group] : inputs.provider_groups) {
        if (name == provider) {
          return group;
        }
      }
      throw std::runtime_error("unbound provider");
    };
  };
  const auto construct = [](const FakeInputs &inputs,
                            const std::string &group) {
    return std::make_shared<const std::string>(inputs.bundle + "/" + group);
  };

  auto first = std::async(std::launch::async, [&] {
    return cache.Acquire(Candidate("bundle-1"), verify, select("arm-a"),
                         construct);
  });
  verifying.wait();
  // Started while the first verification is in progress; whichever moment it
  // reaches the cache, it must reuse that one verification.
  auto second = std::async(std::launch::async, [&] {
    return cache.Acquire(Candidate("bundle-1"), verify, select("arm-b"),
                         construct);
  });
  release.set_value();

  EXPECT_EQ(*first.get(), "bundle-1/group-left");
  EXPECT_EQ(*second.get(), "bundle-1/group-right");
  EXPECT_EQ(verifications.load(), 1);
}

/// Blocks inside candidate verification until released, then refuses.
class GatedAssetSource final : public IManagedProjectionAssetSource {
public:
  GatedAssetSource(std::latch &entered, std::shared_future<void> released)
      : entered_(entered), released_(std::move(released)) {}

  [[nodiscard]] ManagedProjectionCandidateInputs
  VerifyCandidate(const ManagedProjectionCandidate &) const override {
    entered_.count_down();
    released_.wait();
    throw std::runtime_error("fake candidate is unverifiable");
  }

  [[nodiscard]] RetainedCollisionWorldProjector
  ConstructGroupProjector(const ManagedProjectionCandidateInputs &,
                          const std::string &) const override {
    throw std::logic_error("no group is constructed for a refused candidate");
  }

private:
  std::latch &entered_;
  std::shared_future<void> released_;
};

[[nodiscard]] core::ProjectionNativeRuntimeBindingDTO
Binding(const std::string &provider) {
  return core::ProjectionNativeRuntimeBindingDTO{
      core::CapabilityProviderBindingId(provider),
      core::CapabilityProviderBindingRevision(provider + "-revision"),
      core::RuntimeUnitId("native-unit"), core::RuntimeEpoch("epoch-1")};
}

TEST(ManagedProjectionRegistry,
     RegistryReadsProceedWhileCandidatePreparationRuns) {
  std::latch entered(1);
  std::promise<void> release;
  const auto released = release.get_future().share();
  std::int64_t now_ns = 1'000;
  ManagedProjectionRegistry registry(
      ManagedProjectionRegistryConfig{"/nonexistent-package", {},
                                      "/nonexistent-run", "native-unit",
                                      "epoch-1", std::nullopt},
      std::make_unique<GatedAssetSource>(entered, released),
      [&now_ns] { return now_ns; });

  const auto candidate = Candidate("bundle-1");
  const component_manager::detail::NativeProjectionPrepareRequest prepare{
      Binding("arm-a"), candidate.retained_bundle_id, candidate.graph_revision,
      candidate.planning_collision_model};
  ASSERT_TRUE(prepare.valid());
  auto preparing = std::async(std::launch::async, [&] {
    return registry.Dispatch(prepare);
  });
  entered.wait();

  // Preparation is now inside retained verification holding only the
  // candidate cache's mutex. Registry reads take the registry mutex and must
  // complete; were both held, these calls would block until release.
  EXPECT_THROW(static_cast<void>(registry.Dispatch(
                   component_manager::detail::NativeProjectionHealthQuery{
                       Binding("arm-b")})),
               std::runtime_error);
  EXPECT_FALSE(registry
                   .FindCurrentProjectionRevision(core::ProjectionRevisionDTO{},
                                                  Binding("arm-a"))
                   .has_value());
  registry.ReclaimRetiredProjections();

  release.set_value();
  EXPECT_THROW(static_cast<void>(preparing.get()), std::runtime_error);

  // The refused candidate left no projection behind.
  EXPECT_THROW(static_cast<void>(registry.Dispatch(
                   component_manager::detail::NativeProjectionHealthQuery{
                       Binding("arm-a")})),
               std::runtime_error);
}

[[nodiscard]] core::ProviderFreshnessConfirmationDTO FreshnessConfirmation() {
  return core::ProviderFreshnessConfirmationDTO{
      core::ProjectionRevisionDTO{
          core::RetainedBundleId("bundle-1"), core::GraphRevision("graph-1"),
          core::SceneRevision{core::SceneId("sha256:" + std::string(64U, 'd')), 3U,
                              core::ContentDigest(
                                  "sha256:" + std::string(64U, 'c'))}},
      core::SceneResourceId("observed-box"),
      core::Pose3DTO{{0.25, -1.5, 0.75}, {0.0, 0.0, 0.0, 1.0}},
      core::SceneObjectProviderEvidenceDTO{
          core::ProviderInstanceId("isaac.pose.observer"),
          core::ProviderManifestId("manyforge.provider.logical_scene"),
          core::ProviderManifestRevision("sha256:" + std::string(64U, 'b')),
          "sha256:" + std::string(64U, 'a'), 42U, 5'000'000'000,
          1'250'000'000, 2'000'000'000}};
}

[[nodiscard]] component_manager::detail::NativeProjectionProviderFreshnessRequest
FreshnessRequest(const std::string &provider = "arm-a") {
  return {Binding(provider), FreshnessConfirmation()};
}

TEST(NativeProjectionRpcCodec, ProviderFreshnessRequestRoundTripsExactly) {
  const auto request = FreshnessRequest();
  ASSERT_TRUE(request.valid());
  const auto decoded = component_manager::detail::DecodeNativeProjectionRpcRequest(
      component_manager::detail::EncodeNativeProjectionRpcRequest(request));
  const auto *typed = std::get_if<
      component_manager::detail::NativeProjectionProviderFreshnessRequest>(
      &decoded);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->native_runtime_binding, request.native_runtime_binding);
  EXPECT_EQ(typed->confirmation.expected_projection_revision,
            request.confirmation.expected_projection_revision);
  EXPECT_EQ(typed->confirmation.scene_resource_id,
            request.confirmation.scene_resource_id);
  EXPECT_EQ(typed->confirmation.pose_in_cell,
            request.confirmation.pose_in_cell);
  EXPECT_EQ(typed->confirmation.evidence, request.confirmation.evidence);

  // Optional raw source timestamp absence also round-trips.
  auto without_source_timestamp = request;
  without_source_timestamp.confirmation.evidence.source_timestamp_ns.reset();
  const auto decoded_without =
      component_manager::detail::DecodeNativeProjectionRpcRequest(
          component_manager::detail::EncodeNativeProjectionRpcRequest(
              without_source_timestamp));
  EXPECT_EQ(std::get<component_manager::detail::
                         NativeProjectionProviderFreshnessRequest>(
                decoded_without)
                .confirmation.evidence,
            without_source_timestamp.confirmation.evidence);
}

TEST(NativeProjectionRpcCodec, ProviderFreshnessResultRoundTripsExactly) {
  const auto confirmation = FreshnessConfirmation();
  const component_manager::detail::NativeProjectionProviderFreshnessResult
      result{confirmation.expected_projection_revision,
             confirmation.scene_resource_id,
             confirmation.evidence.provider_source_sequence};
  const auto decoded =
      component_manager::detail::DecodeNativeProjectionRpcResponse(
          component_manager::detail::EncodeNativeProjectionRpcResponse(result));
  const auto *typed = std::get_if<
      component_manager::detail::NativeProjectionProviderFreshnessResult>(
      &decoded);
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->projection_revision, result.projection_revision);
  EXPECT_EQ(typed->scene_resource_id, result.scene_resource_id);
  EXPECT_EQ(typed->provider_source_sequence, 42U);
}

TEST(NativeProjectionRpcCodec, ProviderFreshnessRejectsInvalidOrNoncanonical) {
  namespace detail = component_manager::detail;
  const auto encode = [](const detail::NativeProjectionRpcRequest &request) {
    return detail::EncodeNativeProjectionRpcRequest(request);
  };

  auto missing_object = FreshnessRequest();
  missing_object.confirmation.scene_resource_id = core::SceneResourceId("");
  EXPECT_THROW(static_cast<void>(encode(missing_object)),
               std::invalid_argument);
  auto missing_budget = FreshnessRequest();
  missing_budget.confirmation.evidence.source_freshness_budget_ns.reset();
  EXPECT_THROW(static_cast<void>(encode(missing_budget)),
               std::invalid_argument);
  auto negative_received = FreshnessRequest();
  negative_received.confirmation.evidence.received_at_ns = -1;
  EXPECT_THROW(static_cast<void>(encode(negative_received)),
               std::invalid_argument);
  auto non_finite_pose = FreshnessRequest();
  non_finite_pose.confirmation.pose_in_cell.position_m.x_m =
      std::numeric_limits<double>::infinity();
  EXPECT_THROW(static_cast<void>(encode(non_finite_pose)),
               std::invalid_argument);
  auto foreign_binding = FreshnessRequest();
  foreign_binding.native_runtime_binding.runtime_epoch = core::RuntimeEpoch("");
  EXPECT_THROW(static_cast<void>(encode(foreign_binding)),
               std::invalid_argument);

  const auto valid = encode(FreshnessRequest());
  ASSERT_GT(valid.size(), 9U);
  // Closed schema: trailing bytes, truncation, an earlier protocol version
  // and an unknown kind are all refused.
  EXPECT_THROW(static_cast<void>(
                   detail::DecodeNativeProjectionRpcRequest(valid + '\0')),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(detail::DecodeNativeProjectionRpcRequest(
                   valid.substr(0U, valid.size() - 1U))),
               std::invalid_argument);
  auto previous_version = valid;
  previous_version[7] = '3';
  EXPECT_THROW(static_cast<void>(
                   detail::DecodeNativeProjectionRpcRequest(previous_version)),
               std::invalid_argument);
  auto unknown_kind = valid;
  unknown_kind[8] = static_cast<char>(18);
  EXPECT_THROW(static_cast<void>(
                   detail::DecodeNativeProjectionRpcRequest(unknown_kind)),
               std::invalid_argument);
  // The trailing evidence field is the freshness budget, encoded as a present
  // optional (one flag byte plus eight value bytes); a zero budget violates
  // provider-evidence validity and is refused by the decoder.
  auto zero_budget = valid;
  for (std::size_t index = zero_budget.size() - 8U; index < zero_budget.size();
       ++index) {
    zero_budget[index] = '\0';
  }
  EXPECT_THROW(static_cast<void>(
                   detail::DecodeNativeProjectionRpcRequest(zero_budget)),
               std::invalid_argument);
  // A negative budget (sign bit set) is outside the non-negative domain.
  auto negative_budget = valid;
  negative_budget[negative_budget.size() - 8U] = static_cast<char>(0x80);
  EXPECT_THROW(static_cast<void>(
                   detail::DecodeNativeProjectionRpcRequest(negative_budget)),
               std::invalid_argument);

  const detail::NativeProjectionProviderFreshnessResult incomplete{
      FreshnessConfirmation().expected_projection_revision,
      core::SceneResourceId(""), 1U};
  EXPECT_THROW(static_cast<void>(
                   detail::EncodeNativeProjectionRpcResponse(incomplete)),
               std::invalid_argument);
}

TEST(ManagedProjectionRegistry, ProviderFreshnessDispatchNamesExactProjection) {
  std::latch entered(1);
  std::promise<void> release;
  release.set_value();
  ManagedProjectionRegistry registry(
      ManagedProjectionRegistryConfig{"/nonexistent-package", {},
                                      "/nonexistent-run", "native-unit",
                                      "epoch-1", std::nullopt},
      std::make_unique<GatedAssetSource>(entered,
                                         release.get_future().share()),
      [] { return std::int64_t{1'000}; });

  // A decoded wire request reaches the freshness branch, which resolves the
  // exact binding and projection revision and never falls forward to another
  // projection: an unprepared binding/revision is refused.
  const auto decoded = component_manager::detail::DecodeNativeProjectionRpcRequest(
      component_manager::detail::EncodeNativeProjectionRpcRequest(
          FreshnessRequest()));
  EXPECT_THROW(static_cast<void>(registry.Dispatch(decoded)),
               std::runtime_error);

  // A request for another runtime child is refused before any projection.
  auto foreign = FreshnessRequest();
  foreign.native_runtime_binding.runtime_unit_id =
      core::RuntimeUnitId("another-unit");
  EXPECT_THROW(static_cast<void>(registry.Dispatch(foreign)),
               std::runtime_error);

  // A locally invalid confirmation is refused by the dispatch branch itself.
  auto invalid = FreshnessRequest();
  invalid.confirmation.evidence.source_freshness_budget_ns.reset();
  EXPECT_THROW(static_cast<void>(registry.Dispatch(invalid)),
               std::runtime_error);
}

} // namespace
} // namespace manyforge::roboplan_backend
