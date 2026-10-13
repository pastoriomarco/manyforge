#include "retained_lifecycle_product_fixture.hpp"

#include <gtest/gtest.h>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_roboplan/retained_native_lifecycle_product.hpp>

namespace manyforge::roboplan_backend {
namespace {

TEST(RetainedNativeLifecycleProductTest,
     ExactRegistryReadsActualOwnedPureHealthWithoutObservation) {
  using State = core::ComponentLifecycleStateDTO;
  product_test::PackageRoot root;
  auto anchor = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId("lite6_arm_provider"),
      core::MakeNativeControlLifecyclePlans());
  ASSERT_TRUE(anchor.ok());
  const auto fixture = deployment::test::
      materialize_lite6_native_arm_only_cclc_product_retained_consumer_fixture(
          root.path(), anchor.value().value());
  const auto binding = product_test::ArmBinding(*fixture.bundle, root.path());
  const auto domain = binding.value().control_path->control_domain_id;
  const deployment::FilesystemVerifiedRetainedProviderManifestSource manifests(
      root.path());
  core::FakeClock clock;
  clock.set(100);
  bool engine_present = true;
  unsigned engine_reads = 0;
  auto product = RetainedNativeLifecycleProduct::Create(
      fixture.bundle, manifests, {binding}, clock, 1000,
      [&](const core::ControlDomainId &requested) {
        EXPECT_EQ(requested, domain);
        ++engine_reads;
        return engine_present
                   ? core::Result<void>::Ok()
                   : core::Result<void>::Err(
                         {"native_engine_missing", "engine absent", {}});
      },
      [&](const core::ControlDomainId &requested) {
        EXPECT_EQ(requested, domain);
        return core::Result<bool>::Ok(true);
      },
      [](const core::ControllableComponentId &) {
        return core::Result<std::optional<State>>::Ok(State::kPrepared);
      },
      false);
  ASSERT_TRUE(product.ok()) << product.error().message;
  ASSERT_EQ(product.value()->brand_ports().size(), 1U);
  const auto occurrence =
      product.value()->brand_ports().front().provider_occurrence;
  auto projection = product.value()->brand_ports().front().health_projection;
  ASSERT_TRUE(projection);
  const std::weak_ptr<core::IBrandLifecyclePort> weak_port =
      product.value()->brand_ports().front().port;
  {
    core::FixedCCLCPortRegistry registry(product.value()->brand_ports(), {});
    product.value().reset();
    for (int read = 0; read < 3; ++read) {
      const auto health =
          registry.ReadExactLifecycleHealth(occurrence, 200 + read);
      ASSERT_TRUE(health.ok()) << health.error().message;
      EXPECT_EQ(health.value().observed_state, State::kPrepared);
    }
    auto foreign = occurrence;
    foreign.provider_runtime_epoch = core::RuntimeEpoch("foreign.epoch");
    const auto before = engine_reads;
    const auto rejected = registry.ReadExactLifecycleHealth(foreign, 200);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().details.at("reason"),
              "provider_occurrence_not_registered");
    EXPECT_EQ(engine_reads, before);
    // Caller time reaches the pure projection; it does not read Observe's
    // clock.
    clock.set(-1);
    EXPECT_TRUE(registry.ReadExactLifecycleHealth(occurrence, 200).ok());
    EXPECT_FALSE(projection(-1).ok());
    clock.set(100);
    engine_present = false;
    const auto absent = registry.ReadExactLifecycleHealth(occurrence, 200);
    ASSERT_FALSE(absent.ok());
    EXPECT_EQ(absent.error().code, "native_engine_missing");
    engine_present = true;
    auto port = registry.ResolveExactLifecyclePort(occurrence);
    ASSERT_TRUE(port.ok());
    const auto first = port.value()->Observe(product_test::Query(
        occurrence, binding.value().controllable_component_id));
    ASSERT_TRUE(first.ok());
    EXPECT_EQ(first.value().source_sequence, 1U);
  }
  ASSERT_FALSE(weak_port.expired());
  EXPECT_TRUE(projection(200).ok());
  projection = {};
  EXPECT_TRUE(weak_port.expired());
}

} // namespace
} // namespace manyforge::roboplan_backend
