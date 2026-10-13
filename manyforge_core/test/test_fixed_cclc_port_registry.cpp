#include <gtest/gtest.h>

#include <manyforge_core/fixed_cclc_port_registry.hpp>
#include <stdexcept>

namespace manyforge::core {
namespace {

[[nodiscard]] std::string Sha(char value) {
  return std::string("sha256:") + std::string(64U, value);
}

class BrandPort final : public IBrandLifecyclePort {
 public:
  Result<BrandLifecycleObservationDTO> Observe(const BrandLifecycleObservationQueryDTO&) override {
    throw std::logic_error("unused brand observation");
  }

  BrandLifecyclePrimitiveResultDTO Dispatch(const BrandLifecyclePrimitiveIntentDTO&,
                                            std::stop_token) override {
    throw std::logic_error("unused brand dispatch");
  }
};

class ControllerPort final : public IControllerLifecyclePort {
 public:
  Result<ControllerLifecycleObservationDTO> Observe(
      const ControllerLifecycleObservationQueryDTO&) override {
    throw std::logic_error("unused controller observation");
  }

  ControllerLifecycleTransitionResultDTO ApplyTransitionStep(
      const ControllerLifecycleTransitionIntentDTO&, std::stop_token) override {
    throw std::logic_error("unused controller transition");
  }
};

[[nodiscard]] ResolvedLifecycleProviderOccurrenceDTO BrandKey() {
  return {ControlDomainId("domain.arm"),
          ControlDomainRevision(Sha('1')),
          ProviderInstanceId("provider.arm"),
          ProviderManifestId("manifest.arm"),
          ProviderManifestRevision(Sha('2')),
          RuntimeUnitId("runtime.arm"),
          RuntimeEpoch("epoch.1"),
          ContentDigest(Sha('3'))};
}

[[nodiscard]] ControllerLifecycleObservationQueryDTO ControllerKey() {
  return {ControlDomainId("domain.arm"),
          ControlDomainRevision(Sha('1')),
          ControllerTopologyId("topology.arm"),
          TopologyRevision(Sha('4')),
          ProductId("product.control.arm"),
          ProductContractId("manyforge.control-domain-control.v2"),
          ProductContractRevision(Sha('5')),
          ContentDigest(Sha('6')),
          ContentDigest(Sha('7')),
          RuntimeUnitId("runtime.controller-manager"),
          RuntimeEpoch("epoch.1")};
}

TEST(FixedCCLCPortRegistry, ResolvesOnlyExactImmutableRegistrations) {
  auto brand = std::make_shared<BrandPort>();
  auto controller = std::make_shared<ControllerPort>();
  const auto brand_key = BrandKey();
  const auto controller_key = ControllerKey();
  FixedCCLCPortRegistry registry({{brand_key, brand}}, {{controller_key, controller}});

  const auto resolved_brand = registry.ResolveExactLifecyclePort(brand_key);
  ASSERT_TRUE(resolved_brand.ok());
  EXPECT_EQ(resolved_brand.value(), brand);
  const auto resolved_controller = registry.ResolveExactControllerLifecyclePort(controller_key);
  ASSERT_TRUE(resolved_controller.ok());
  EXPECT_EQ(resolved_controller.value(), controller);

  auto stale_brand = brand_key;
  stale_brand.provider_runtime_epoch = RuntimeEpoch("epoch.2");
  EXPECT_FALSE(registry.ResolveExactLifecyclePort(stale_brand).ok());
  auto stale_controller = controller_key;
  stale_controller.source_runtime_epoch = RuntimeEpoch("epoch.2");
  EXPECT_FALSE(registry.ResolveExactControllerLifecyclePort(stale_controller).ok());
}

TEST(FixedCCLCPortRegistry, RejectsNullDuplicateAndMalformedRows) {
  auto brand = std::make_shared<BrandPort>();
  auto controller = std::make_shared<ControllerPort>();
  EXPECT_THROW((FixedCCLCPortRegistry({{BrandKey(), brand}, {BrandKey(), brand}}, {})),
               std::invalid_argument);
  EXPECT_THROW(
      (FixedCCLCPortRegistry({}, {{ControllerKey(), controller}, {ControllerKey(), controller}})),
      std::invalid_argument);
  EXPECT_THROW((FixedCCLCPortRegistry({{BrandKey(), nullptr}}, {})), std::invalid_argument);
  auto malformed = ControllerKey();
  malformed.source_runtime_epoch = RuntimeEpoch("");
  EXPECT_THROW((FixedCCLCPortRegistry({}, {{malformed, controller}})), std::invalid_argument);
}

TEST(FixedCCLCPortRegistry, HealthUsesAllExactKeyFieldsAndNoObservationFallback) {
  int calls = 0;
  std::int64_t received_time = -1;
  const auto key = BrandKey();
  const LifecycleDomainHealthDTO health{ComponentLifecycleStateDTO::kDeclared,
      {{LifecycleFactId("custom.mode"),
        {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt, "manual"}}}, std::nullopt};
  FixedCCLCPortRegistry registry({{key, std::make_shared<BrandPort>(),
      [&](std::int64_t now) {
        ++calls;
        received_time = now;
        return Result<LifecycleDomainHealthDTO>::Ok(health);
      }}}, {});
  for (int mutation = 0; mutation < 8; ++mutation) {
    auto foreign = key;
    switch (mutation) {
      case 0: foreign.control_domain_id = ControlDomainId("foreign"); break;
      case 1: foreign.control_domain_revision = ControlDomainRevision(Sha('8')); break;
      case 2: foreign.provider_instance_id = ProviderInstanceId("foreign"); break;
      case 3: foreign.provider_manifest_id = ProviderManifestId("foreign"); break;
      case 4: foreign.provider_manifest_revision = ProviderManifestRevision(Sha('8')); break;
      case 5: foreign.provider_runtime_unit_id = RuntimeUnitId("foreign"); break;
      case 6: foreign.provider_runtime_epoch = RuntimeEpoch("foreign"); break;
      case 7: foreign.lifecycle_plan_catalog_digest = ContentDigest(Sha('8')); break;
    }
    EXPECT_FALSE(registry.ReadExactLifecycleHealth(foreign, 42).ok()) << mutation;
  }
  EXPECT_FALSE(registry.ReadExactLifecycleHealth(key, -1).ok());
  EXPECT_EQ(calls, 0);
  for (int repeat = 0; repeat < 2; ++repeat) {
    const auto read = registry.ReadExactLifecycleHealth(key, 42);
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(read.value(), health);
  }
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(received_time, 42);
  FixedCCLCPortRegistry missing({{key, std::make_shared<BrandPort>()}}, {});
  const auto absent = missing.ReadExactLifecycleHealth(key, 42);
  ASSERT_FALSE(absent.ok());
  EXPECT_EQ(absent.error().details.at("reason"), "lifecycle_health_projection_missing");
}

TEST(FixedCCLCPortRegistry, HealthPreservesSourceRefusalAndRejectsInvalidOrThrowingProjection) {
  const auto key = BrandKey();
  const ErrorDTO original{"source_unavailable", "original bounded source refusal",
                          {{"reason", "source_missing"}}};
  FixedCCLCPortRegistry refused({{key, std::make_shared<BrandPort>(),
      [&](std::int64_t) { return Result<LifecycleDomainHealthDTO>::Err(original); }}}, {});
  const auto read = refused.ReadExactLifecycleHealth(key, 0);
  ASSERT_FALSE(read.ok());
  EXPECT_EQ(read.error(), original);
  FixedCCLCPortRegistry invalid({{key, std::make_shared<BrandPort>(),
      [](std::int64_t) { return Result<LifecycleDomainHealthDTO>::Ok(
          {static_cast<ComponentLifecycleStateDTO>(255), {}, std::nullopt}); }}}, {});
  const auto bad = invalid.ReadExactLifecycleHealth(key, 0);
  ASSERT_FALSE(bad.ok());
  EXPECT_EQ(bad.error().details.at("reason"), "lifecycle_health_projection_invalid");
  FixedCCLCPortRegistry throwing({{key, std::make_shared<BrandPort>(),
      [](std::int64_t) -> Result<LifecycleDomainHealthDTO> { throw std::runtime_error("source"); }}}, {});
  const auto unavailable = throwing.ReadExactLifecycleHealth(key, 0);
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().details.at("reason"), "lifecycle_health_projection_exception");
}

}  // namespace
}  // namespace manyforge::core
