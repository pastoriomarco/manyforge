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

}  // namespace
}  // namespace manyforge::core
