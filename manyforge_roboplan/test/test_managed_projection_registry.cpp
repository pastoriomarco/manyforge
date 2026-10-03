#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <latch>
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
                                      "epoch-1"},
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

} // namespace
} // namespace manyforge::roboplan_backend
