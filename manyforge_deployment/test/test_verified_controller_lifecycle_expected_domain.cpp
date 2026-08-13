#include <algorithm>
#include <array>
#include <filesystem>
#include <gtest/gtest.h>
#include <manyforge_deployment/verified_controller_lifecycle_expected_domain.hpp>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unistd.h>

#include "lite6_retained_consumer_fixture.hpp"

namespace manyforge::deployment {
namespace {

static_assert(!std::is_default_constructible_v<
              VerifiedControllerLifecycleExpectedDomain>);
static_assert(
    !std::is_constructible_v<VerifiedControllerLifecycleExpectedDomain,
                             core::ControllerLifecycleExpectedDomainDTO>);

class ScopedTrustedRoot final {
public:
  ScopedTrustedRoot() {
    std::array<char, 96> pattern{};
    const std::string initial =
        "/tmp/manyforge-controller-lifecycle-expected-XXXXXX";
    std::copy(initial.begin(), initial.end(), pattern.begin());
    const char *created = ::mkdtemp(pattern.data());
    if (created == nullptr) {
      throw std::runtime_error("cannot create controller-lifecycle test root");
    }
    path_ = created;
  }

  ~ScopedTrustedRoot() { std::filesystem::remove_all(path_); }

  ScopedTrustedRoot(const ScopedTrustedRoot &) = delete;
  ScopedTrustedRoot &operator=(const ScopedTrustedRoot &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

class AbsentArtifactSource final
    : public IVerifiedRetainedProductArtifactSource {
public:
  [[nodiscard]] std::optional<VerifiedRetainedProductArtifact>
  get_verified(const RetainedProductArtifactQuery &) const override {
    return std::nullopt;
  }
};

TEST(VerifiedControllerLifecycleExpectedDomainTest,
     DirectProductDecodesOneExactSealedClosureAndRejectsStaleCandidates) {
  ScopedTrustedRoot root;
  const auto retained =
      test::materialize_lite6_retained_consumer_fixture(root.path());
  ASSERT_EQ(retained.bundle->value().desired_control_configurations.size(), 1U);
  const auto &desired =
      retained.bundle->value().desired_control_configurations.front();
  const FilesystemVerifiedRetainedProductArtifactSource source(root.path());

  const auto verified = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, desired,
      core::RuntimeUnitId(test::kLite6NativeExecutionRuntimeId),
      core::RuntimeEpoch("runtime.controller_manager.epoch.1"));
  const auto &value = verified.value();
  EXPECT_EQ(value.control_domain_id, desired.control_domain_id);
  EXPECT_EQ(value.control_domain_revision, desired.control_domain_revision);
  EXPECT_EQ(value.controller_topology_id, desired.controller_topology_id);
  EXPECT_EQ(value.topology_revision, desired.topology_revision);
  EXPECT_EQ(value.desired_configuration, desired);
  EXPECT_EQ(value.source_runtime_unit_id,
            core::RuntimeUnitId(test::kLite6NativeExecutionRuntimeId));
  EXPECT_EQ(value.source_runtime_epoch,
            core::RuntimeEpoch("runtime.controller_manager.epoch.1"));
  ASSERT_EQ(value.expected_controllers.size(), 1U);
  EXPECT_FALSE(
      value.expected_controllers.front().claimed_command_interfaces.empty());
  EXPECT_FALSE(
      value.expected_controllers.front().claimed_state_interfaces.empty());
  EXPECT_FALSE(value.expected_endpoints.empty());
  EXPECT_EQ(value.control_artifact_digest.value().substr(0U, 7U), "sha256:");
  EXPECT_EQ(value.source_conformance_digest.value().substr(0U, 7U), "sha256:");

  auto stale_route = desired;
  stale_route.binding_routes.front().control_route_id =
      core::ControlRouteId("route.not_retained");
  EXPECT_THROW(
      static_cast<void>(load_verified_controller_lifecycle_expected_domain(
          *retained.bundle, source, stale_route,
          core::RuntimeUnitId(test::kLite6NativeExecutionRuntimeId),
          core::RuntimeEpoch("runtime.controller_manager.epoch.1"))),
      RetainedBundleError);

  auto incomplete = desired;
  incomplete.binding_routes.clear();
  EXPECT_THROW(
      static_cast<void>(load_verified_controller_lifecycle_expected_domain(
          *retained.bundle, source, incomplete,
          core::RuntimeUnitId(test::kLite6NativeExecutionRuntimeId),
          core::RuntimeEpoch("runtime.controller_manager.epoch.1"))),
      RetainedBundleError);

  EXPECT_THROW(
      static_cast<void>(load_verified_controller_lifecycle_expected_domain(
          *retained.bundle, source, desired, core::RuntimeUnitId(""),
          core::RuntimeEpoch("runtime.controller_manager.epoch.1"))),
      RetainedBundleError);

  EXPECT_THROW(
      static_cast<void>(load_verified_controller_lifecycle_expected_domain(
          *retained.bundle, source, desired,
          core::RuntimeUnitId("runtime.not_in_domain"),
          core::RuntimeEpoch("runtime.controller_manager.epoch.1"))),
      RetainedBundleError);

  const AbsentArtifactSource absent;
  EXPECT_THROW(
      static_cast<void>(load_verified_controller_lifecycle_expected_domain(
          *retained.bundle, absent, desired,
          core::RuntimeUnitId(test::kLite6NativeExecutionRuntimeId),
          core::RuntimeEpoch("runtime.controller_manager.epoch.1"))),
      RetainedBundleError);
}

TEST(VerifiedControllerLifecycleExpectedDomainTest,
     ChainedProductPreservesCompleteTopologyOrderAndSelectedEndpoints) {
  ScopedTrustedRoot root;
  const auto retained = test::materialize_lite6_retained_consumer_fixture_from(
      root.path(), test::make_lite6_chained_controllable_retention_fixture());
  ASSERT_EQ(retained.bundle->value().desired_control_configurations.size(), 1U);
  const auto &desired =
      retained.bundle->value().desired_control_configurations.front();
  const FilesystemVerifiedRetainedProductArtifactSource source(root.path());

  const auto verified = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, desired,
      core::RuntimeUnitId(test::kLite6NativeExecutionRuntimeId),
      core::RuntimeEpoch("runtime.controller_manager.epoch.7"));
  const auto &value = verified.value();
  ASSERT_EQ(value.controller_topology.controllers.size(), 2U);
  ASSERT_EQ(value.expected_controllers.size(), 2U);
  ASSERT_EQ(value.controller_topology.chain_edges.size(), 1U);
  ASSERT_EQ(value.controller_topology.legal_routes.size(), 1U);
  ASSERT_EQ(value.controller_topology.legal_routes.front()
                .ordered_controller_role_ids.size(),
            2U);
  EXPECT_FALSE(value.expected_endpoints.empty());
  EXPECT_TRUE(std::all_of(
      value.expected_endpoints.begin(), value.expected_endpoints.end(),
      [&](const auto &endpoint) {
        return endpoint.control_binding_id ==
                   desired.binding_routes.front().control_binding_id &&
               endpoint.control_route_id ==
                   desired.binding_routes.front().control_route_id;
      }));
}

} // namespace
} // namespace manyforge::deployment
