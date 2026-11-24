#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <latch>
#include <limits>
#include <memory>
#include <optional>
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

[[nodiscard]] core::ProjectionExecutionHostBindingDTO
Binding(const std::string &provider) {
  return core::ProjectionExecutionHostBindingDTO{
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
  std::atomic<unsigned> notifications{0U};
  ManagedProjectionRegistry registry(
      ManagedProjectionRegistryConfig{"/nonexistent-package", {},
                                      "/nonexistent-run", "native-unit",
                                      "epoch-1", std::nullopt},
      std::make_unique<GatedAssetSource>(entered, released),
      [&now_ns] { return now_ns; },
      [&notifications] {
        ++notifications;
      });

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
  EXPECT_EQ(notifications.load(), 0U);

  // The refused candidate left no projection behind.
  EXPECT_THROW(static_cast<void>(registry.Dispatch(
                   component_manager::detail::NativeProjectionHealthQuery{
                       Binding("arm-a")})),
               std::runtime_error);
  EXPECT_EQ(notifications.load(), 0U);
}

[[nodiscard]] core::ProjectionRevisionDTO SelectedRevision() {
  return core::ProjectionRevisionDTO{
      core::RetainedBundleId("bundle-1"), core::GraphRevision("graph-1"),
      core::SceneRevision{core::SceneId("sha256:" + std::string(64U, 'd')), 3U,
                          core::ContentDigest(
                              "sha256:" + std::string(64U, 'c'))}};
}

TEST(NativeProjectionRpcCodec, HealthSelectorPreservesLegacyAndHasOneEncoding) {
  namespace detail = component_manager::detail;
  // Frozen MFNPRPC7/tag-2 request, independent of codec writers.
  static constexpr char legacy_bytes[] = "MFNPRPC7\x02"
                                         "\x00\x00\x00\x05"
                                         "arm-a"
                                         "\x00\x00\x00\x0e"
                                         "arm-a-revision"
                                         "\x00\x00\x00\x0b"
                                         "native-unit"
                                         "\x00\x00\x00\x07"
                                         "epoch-1";
  const std::string legacy(legacy_bytes, sizeof(legacy_bytes) - 1U);
  const detail::NativeProjectionHealthQuery ordinary{Binding("arm-a")};
  EXPECT_EQ(detail::EncodeNativeProjectionRpcRequest(ordinary), legacy);
  const auto ordinary_decoded =
      detail::DecodeNativeProjectionRpcRequest(legacy);
  const auto &plain =
      std::get<detail::NativeProjectionHealthQuery>(ordinary_decoded);
  EXPECT_EQ(plain.execution_host_binding, ordinary.execution_host_binding);
  EXPECT_FALSE(plain.selected_projection_revision.has_value());
  // An earlier version is refused like an unknown magic.
  auto old_version = legacy;
  old_version[7] = '6';
  EXPECT_THROW(static_cast<void>(
                   detail::DecodeNativeProjectionRpcRequest(old_version)),
               std::invalid_argument);

  static constexpr char selector_bytes[] =
      "\x01"
      "\x00\x00\x00\x08"
      "bundle-1"
      "\x00\x00\x00\x07"
      "graph-1"
      "\x00\x00\x00\x47"
      "sha256:"
      "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
      "\x00\x00\x00\x00\x00\x00\x00\x03"
      "\x00\x00\x00\x47"
      "sha256:"
      "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
  const auto selected_revision = SelectedRevision();
  const detail::NativeProjectionHealthQuery selected{Binding("arm-a"),
                                                     selected_revision};
  const std::string canonical =
      legacy + std::string(selector_bytes, sizeof(selector_bytes) - 1U);
  EXPECT_EQ(detail::EncodeNativeProjectionRpcRequest(selected), canonical);
  const auto decoded = detail::DecodeNativeProjectionRpcRequest(canonical);
  const auto &typed = std::get<detail::NativeProjectionHealthQuery>(decoded);
  EXPECT_EQ(typed.execution_host_binding, selected.execution_host_binding);
  EXPECT_EQ(typed.selected_projection_revision,
            selected.selected_projection_revision);

  for (const char marker : {'\x00', '\x02', '\xff'}) {
    EXPECT_THROW(static_cast<void>(detail::DecodeNativeProjectionRpcRequest(
                     legacy + std::string(1U, marker))),
                 std::invalid_argument);
  }
  EXPECT_THROW(static_cast<void>(detail::DecodeNativeProjectionRpcRequest(
                   legacy + std::string(1U, '\x01'))),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(detail::DecodeNativeProjectionRpcRequest(
                   canonical.substr(0U, canonical.size() - 1U))),
               std::invalid_argument);
  EXPECT_THROW(static_cast<void>(detail::DecodeNativeProjectionRpcRequest(
                   canonical + std::string(1U, '\x00'))),
               std::invalid_argument);
  // Present selector with an empty retained-bundle identity is not absence.
  const std::string invalid_empty_bundle =
      legacy + std::string("\x01\x00\x00\x00\x00", 5U) +
      std::string(selector_bytes + 13U, sizeof(selector_bytes) - 1U - 13U);
  EXPECT_THROW(static_cast<void>(detail::DecodeNativeProjectionRpcRequest(
                   invalid_empty_bundle)),
               std::invalid_argument);
  auto invalid = selected;
  invalid.selected_projection_revision->graph_revision =
      core::GraphRevision("");
  EXPECT_THROW(
      static_cast<void>(detail::EncodeNativeProjectionRpcRequest(invalid)),
      std::invalid_argument);
  EXPECT_THROW(
      static_cast<void>(detail::DecodeNativeProjectionRpcResponse(canonical)),
      std::invalid_argument);
}

TEST(NativeProjectionRpcCodec, EffectivePolicyCarriesStaticBodiesNotPairs) {
  namespace detail = component_manager::detail;
  // OD-376: 200 cell-fixed objects and a static robot base are 201 static
  // bodies. Their 20100 static pairs once exceeded the codec's entry bound.
  core::CollisionPolicySnapshot policy;
  policy.revision = {2U, "sha256:" + std::string(64U, 'e')};
  policy.static_body_ids.push_back(
      "component/arm/model/arm.model/body/base/collision/0");
  for (int index = 0; index < 200; ++index) {
    policy.static_body_ids.push_back("scene/fixed-" +
                                     std::to_string(1000 + index).substr(1U) +
                                     "/shape/0");
  }
  policy.allowed_touch_link_pairs = {
      {"component/arm/model/arm.model/body/tcp/collision/0",
       "scene/part/shape/0"}};
  const detail::NativeProjectionRpcResponse response{policy};
  const auto decoded = detail::DecodeNativeProjectionRpcResponse(
      detail::EncodeNativeProjectionRpcResponse(response));
  ASSERT_TRUE(std::holds_alternative<core::CollisionPolicySnapshot>(decoded));
  EXPECT_EQ(std::get<core::CollisionPolicySnapshot>(decoded), policy);
}

TEST(NativeProjectionRpcCodec, ABoundBreachRefusesTheOneRequest) {
  namespace detail = component_manager::detail;
  // 4097 static bodies exceed the entry bound of one list: the execution host
  // answers this request with a typed refusal and keeps running.
  core::CollisionPolicySnapshot policy;
  policy.revision = {2U, "sha256:" + std::string(64U, 'e')};
  for (int index = 0; index < 4097; ++index) {
    policy.static_body_ids.push_back("scene/fixed-" +
                                     std::to_string(10000 + index).substr(1U) +
                                     "/shape/0");
  }
  const detail::NativeProjectionRpcResponse oversized{policy};
  EXPECT_THROW(static_cast<void>(
                   detail::EncodeNativeProjectionRpcResponse(oversized)),
               detail::NativeProjectionRpcBoundExceeded);
  const auto refusal = detail::DecodeNativeProjectionRpcResponse(
      detail::EncodeNativeProjectionRpcResponseOrRefusal(oversized));
  const auto *failure =
      std::get_if<detail::NativeProjectionRpcFailure>(&refusal);
  ASSERT_NE(failure, nullptr);
  EXPECT_EQ(failure->error.code, "native_projection_rpc_bound_exceeded");

  // Within the bound the same call encodes the response itself.
  policy.static_body_ids.pop_back();
  const detail::NativeProjectionRpcResponse bounded{policy};
  EXPECT_EQ(detail::EncodeNativeProjectionRpcResponseOrRefusal(bounded),
            detail::EncodeNativeProjectionRpcResponse(bounded));

  // A request beyond the packet byte bound is refused the same way.
  EXPECT_THROW(static_cast<void>(detail::EncodeNativeProjectionRpcRequest(
                   detail::NativeProjectionHealthQuery{Binding(
                       std::string(detail::kMaximumNativeProjectionRpcPacketBytes,
                                   'a'))})),
               detail::NativeProjectionRpcBoundExceeded);
}

TEST(NativeProjectionRpcCodec, CommitPreflightRoundTripsWithoutACandidate) {
  namespace detail = component_manager::detail;
  const std::string digest = "sha256:" + std::string(64U, 'e');
  const core::SceneTransactionDTO transaction{
      core::RequestKey::Make(core::RequestKeyClass::kHttpClient, 0U, 1U, 2U),
      {},
      4U,
      SelectedRevision(),
      SelectedRevision(),
      core::ContentDigest(digest),
      core::ResolvedRemoveSceneObjectDTO{core::SceneResourceId("part")},
      std::nullopt};
  for (const auto &selected :
       {detail::NativeProjectionCommitPreflightRequest{Binding("arm-a"),
                                                       SelectedRevision()},
        detail::NativeProjectionCommitPreflightRequest{
            Binding("arm-a"), SelectedRevision(), transaction}}) {
    const auto encoded = detail::EncodeNativeProjectionRpcRequest(selected);
    const auto decoded = detail::DecodeNativeProjectionRpcRequest(encoded);
    const auto &typed =
        std::get<detail::NativeProjectionCommitPreflightRequest>(decoded);
    EXPECT_EQ(typed.execution_host_binding, selected.execution_host_binding);
    EXPECT_EQ(typed.selected_projection_revision,
              selected.selected_projection_revision);
    EXPECT_EQ(typed.transaction, selected.transaction);
    EXPECT_EQ(detail::EncodeNativeProjectionRpcRequest(decoded), encoded);
  }
  auto invalid = detail::NativeProjectionCommitPreflightRequest{
      Binding("arm-a"), SelectedRevision()};
  invalid.selected_projection_revision.graph_revision = core::GraphRevision("");
  EXPECT_THROW(
      static_cast<void>(detail::EncodeNativeProjectionRpcRequest(invalid)),
      std::invalid_argument);

  const core::ProjectionHealthDTO health{
      Binding("arm-a"), 7U, core::ProjectionHealthState::kHealthy,
      SelectedRevision(), 3U, core::ContentDigest(digest), std::nullopt};
  core::CollisionPolicySnapshot policy;
  policy.policy_id = "free_space";
  policy.revision = {3U, digest};
  policy.static_body_ids = {"body-a", "body-b"};
  const core::ErrorDTO refusal{"invariant_violation", "delta does not apply",
                               {{"reason", "native_projection_resolved_delta_mismatch"}}};
  using PolicyResult =
      std::optional<std::variant<core::CollisionPolicySnapshot, core::ErrorDTO>>;
  for (const auto &effective_policy :
       {PolicyResult{}, PolicyResult{policy}, PolicyResult{refusal}}) {
    const detail::NativeProjectionCommitPreflightResult result{
        health, effective_policy};
    const auto encoded = detail::EncodeNativeProjectionRpcResponse(result);
    const auto decoded = detail::DecodeNativeProjectionRpcResponse(encoded);
    const auto &typed =
        std::get<detail::NativeProjectionCommitPreflightResult>(decoded);
    EXPECT_EQ(typed.health.execution_host_binding,
              health.execution_host_binding);
    EXPECT_EQ(typed.health.active_projection_revision,
              health.active_projection_revision);
    EXPECT_EQ(typed.effective_policy, effective_policy);
    EXPECT_EQ(detail::EncodeNativeProjectionRpcResponse(decoded), encoded);
  }
}

TEST(ManagedProjectionRegistry, CommitPreflightRefusesAnUnpreparedBinding) {
  std::latch entered(1);
  ManagedProjectionRegistry registry(
      ManagedProjectionRegistryConfig{"/nonexistent-package", {},
                                      "/nonexistent-run", "native-unit",
                                      "epoch-1", std::nullopt},
      std::make_unique<GatedAssetSource>(entered, std::shared_future<void>{}),
      [] { return std::int64_t{1'000}; }, [] {});
  EXPECT_THROW(static_cast<void>(registry.Dispatch(
                   component_manager::detail::NativeProjectionCommitPreflightRequest{
                       Binding("arm-a"), SelectedRevision()})),
               std::runtime_error);
}

} // namespace
} // namespace manyforge::roboplan_backend
