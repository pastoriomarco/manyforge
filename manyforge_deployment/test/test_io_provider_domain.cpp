#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <unistd.h>

#include <manyforge_core/io_provider_lifecycle_catalog.hpp>
#include <manyforge_deployment/io_provider_domain.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "io_retained_fixture.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"

namespace {
namespace c = manyforge::core;
namespace d = manyforge::deployment;
using Object = c::JsonValueDTO::Object;
using Array = c::JsonValueDTO::Array;

class IoProviderDomainTest : public ::testing::Test {
protected:
  void SetUp() override {
    path = std::filesystem::temp_directory_path() /
           ("manyforge-io-domain-" + std::to_string(::getpid()) + "-" +
            std::to_string(sequence.fetch_add(1)));
    ASSERT_TRUE(std::filesystem::create_directory(path));
    bundle = d::test::materialize_boolean_io_retained_fixture(path).bundle;
    ASSERT_NE(bundle, nullptr);
    for (const auto &record : bundle->value().catalogs.records) {
      if (const auto *ref =
              std::get_if<c::RuntimeProfileCatalogRecordRefDTO>(&record))
        profiles.push_back(d::resolve_and_qualify_runtime_profile(
            *ref, ref->content_addressed_store_key,
            d::detail::ros2_control_execution_runtime_profile_document()));
    }
    ASSERT_EQ(bundle->value().cell_graph.control_domains.size(), 1U);
    const auto *provider =
        bundle->value().cell_graph.control_domains.front().provider();
    ASSERT_NE(provider, nullptr);
    const d::FilesystemVerifiedRetainedProviderManifestSource source(path);
    const auto manifest = source.get_verified(
        {bundle->retained_bundle_id(), provider->provider_manifest_id,
         provider->provider_manifest_revision});
    ASSERT_TRUE(manifest);
    manifests.push_back(*manifest);
  }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  static void SetBudget(c::CellGraphDTO &graph, double seconds) {
    auto &devices = std::get<Array>(graph.runtime_units.front()
                                        .configuration.at("io_provider_devices")
                                        .value);
    std::get<Object>(devices.front().value).at("lifecycle_command_timeout_s") =
        c::JsonValueDTO{seconds};
  }
  inline static std::atomic<unsigned> sequence{};
  std::filesystem::path path;
  std::shared_ptr<const d::RetainedBundle> bundle;
  std::vector<d::QualifiedRuntimeProfile> profiles;
  std::vector<d::QualifiedProviderManifest> manifests;
};

TEST_F(IoProviderDomainTest,
       GenuineZeroArmReadbackHasActualDomainAndCatalogProof) {
  const auto &graph = bundle->value().cell_graph;
  EXPECT_TRUE(graph.model_instances.empty());
  EXPECT_TRUE(graph.control_bindings.empty());
  EXPECT_TRUE(bundle->value().desired_control_configurations.empty());
  EXPECT_TRUE(
      bundle->value().compiled_products.control_domain_products.empty());
  const auto &domain = graph.control_domains.front();
  const auto *provider = domain.provider();
  ASSERT_NE(provider, nullptr);
  EXPECT_EQ(provider->controllable_component_id,
            c::ControllableComponentId("device.io"));
  EXPECT_EQ(provider->provider_instance_id,
            c::ProviderInstanceId("provider.io"));
  EXPECT_EQ(provider->provider_runtime_unit_id,
            c::RuntimeUnitId("io.execution"));
  EXPECT_EQ(provider->signal_names, std::vector<std::string>{"ready"});
  EXPECT_EQ(provider->lifecycle_command_timeout_ns, 10'000'000'000LL);
  EXPECT_EQ(domain.control_coordinator_endpoint_role,
            c::EndpointRoleId("ros2_control_execution_status"));
  const auto catalog =
      c::MakeIoProviderLifecycleCatalog(manifests.front().value());
  ASSERT_TRUE(catalog.ok());
  EXPECT_EQ(provider->lifecycle_plan_catalog_digest,
            catalog.value().catalog_digest);
  EXPECT_NE(provider->lifecycle_plan_catalog_digest,
            manifests.front().value().lifecycle_plan_catalog->semantic_digest);
  EXPECT_TRUE(d::ValidateIoProviderDomains(
                  graph, bundle->value().capability_provider_bindings, profiles,
                  manifests)
                  .ok());
  EXPECT_TRUE(d::ValidateIoProviderDomainProjection(
                  graph, bundle->value().capability_provider_bindings, profiles)
                  .ok());
}

TEST_F(IoProviderDomainTest,
       EditedDeviceLimitChangesExactDomainProofButNotOwnerOrCatalog) {
  auto graph = bundle->value().cell_graph;
  const auto original = graph.control_domains.front();
  SetBudget(graph, 31.0);
  const auto derived = d::DeriveIoProviderDomains(
      graph, bundle->value().capability_provider_bindings, profiles, manifests);
  ASSERT_TRUE(derived.ok());
  ASSERT_EQ(derived.value().provider_domains.size(), 1U);
  const auto &changed = derived.value().provider_domains.front();
  EXPECT_EQ(changed.control_domain_id, original.control_domain_id);
  EXPECT_NE(changed.control_domain_revision, original.control_domain_revision);
  EXPECT_NE(changed.provider()->signal_configuration_digest,
            original.provider()->signal_configuration_digest);
  EXPECT_EQ(changed.provider()->lifecycle_plan_catalog_digest,
            original.provider()->lifecycle_plan_catalog_digest);
  EXPECT_EQ(changed.provider()->lifecycle_command_timeout_ns, 31'000'000'000LL);
  EXPECT_FALSE(d::ValidateIoProviderDomains(
                   graph, bundle->value().capability_provider_bindings,
                   profiles, manifests)
                   .ok());
  graph.control_domains = derived.value().provider_domains;
  EXPECT_FALSE(d::ValidateIoProviderDomains(
                   graph, bundle->value().capability_provider_bindings,
                   profiles, manifests)
                   .ok());
  auto bindings = bundle->value().capability_provider_bindings;
  for (auto &binding : bindings)
    if (auto *target = std::get_if<c::ControlDomainCapabilityProviderTargetDTO>(
            &binding.target))
      target->control_domain_revision = changed.control_domain_revision;
  EXPECT_TRUE(
      d::ValidateIoProviderDomains(graph, bindings, profiles, manifests).ok());
}

TEST_F(IoProviderDomainTest,
       AmbiguousOccurrenceAndForgedCatalogProjectionRefuse) {
  auto bindings = bundle->value().capability_provider_bindings;
  const auto input =
      std::find_if(bindings.begin(), bindings.end(), [](const auto &binding) {
        return binding.capability_id.value() == "io.gpio.get_input";
      });
  ASSERT_NE(input, bindings.end());
  input->provider_instance_id = c::ProviderInstanceId("other.provider");
  EXPECT_FALSE(d::DeriveIoProviderDomains(bundle->value().cell_graph, bindings,
                                          profiles, manifests)
                   .ok());
  auto graph = bundle->value().cell_graph;
  auto forged_provider = *graph.control_domains.front().provider();
  forged_provider.lifecycle_plan_catalog_digest =
      c::ContentDigest("sha256:" + std::string(64, '0'));
  graph.control_domains.front().payload = std::move(forged_provider);
  EXPECT_FALSE(
      d::ValidateIoProviderDomainProjection(
          graph, bundle->value().capability_provider_bindings, profiles)
          .ok());
  auto missing_lifecycle = bundle->value().capability_provider_bindings;
  std::erase_if(missing_lifecycle, [](const auto &binding) {
    return binding.capability_id.value() == "component.lifecycle.enable";
  });
  EXPECT_FALSE(d::ValidateIoProviderDomainProjection(
                   bundle->value().cell_graph, missing_lifecycle, profiles)
                   .ok());
  SetBudget(graph, 0.0);
  EXPECT_FALSE(d::DeriveIoProviderDomains(
                   graph, bundle->value().capability_provider_bindings,
                   profiles, manifests)
                   .ok());
}

TEST_F(IoProviderDomainTest,
       DeferredXArmProfileRefusesBeforeAnyProviderDomainIsInferred) {
  const auto arm =
      d::test::make_lite6_ros2_control_shared_consumer_retention_fixture();
  auto graph = arm.qualified.deployment().graph().graph();
  const auto binding = std::find_if(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [](const auto &row) {
        return row.capability_id.value() == "execution.trajectory.execute";
      });
  ASSERT_NE(binding, graph.control_bindings.end());
  const auto arm_domain =
      std::find_if(graph.control_domains.begin(), graph.control_domains.end(),
                   [&](const auto &row) {
                     return row.control_domain_id == binding->control_domain_id;
                   });
  ASSERT_NE(arm_domain, graph.control_domains.end());
  auto unit = std::find_if(graph.runtime_units.begin(),
                           graph.runtime_units.end(), [&](const auto &row) {
                             return row.runtime_unit_id ==
                                    binding->provider_runtime_unit_id;
                           });
  ASSERT_NE(unit, graph.runtime_units.end());
  const Object gpio{
      {"controllable_component_id",
       c::JsonValueDTO{binding->controllable_component_id.value()}},
      {"command_target_id",
       c::JsonValueDTO{binding->command_target_id.value()}},
      {"resource_role_id", c::JsonValueDTO{std::string("gpio_ready")}},
      {"signal_name", c::JsonValueDTO{std::string("gpio_ready")}},
      {"source_id", c::JsonValueDTO{std::string("source.arm.gpio")}},
      {"value_type", c::JsonValueDTO{std::string("bool")}},
      {"direction", c::JsonValueDTO{std::string("input")}},
      {"update_mode", c::JsonValueDTO{std::string("sampled")}},
      {"transition_capability", c::JsonValueDTO{std::string("sampled_only")}},
      {"freshness", c::JsonValueDTO{Object{
                        {"mode", c::JsonValueDTO{std::string("sample_age")}},
                        {"maximum_age_ns", c::JsonValueDTO{1000.0}}}}},
      {"provider",
       c::JsonValueDTO{Object{
           {"profile", c::JsonValueDTO{std::string("xarm_controller_input")}},
           {"topic", c::JsonValueDTO{std::string("/arm/cgpio")}},
           {"input_bit", c::JsonValueDTO{0.0}},
           {"ingress_depth", c::JsonValueDTO{4.0}}}}}};
  unit->configuration["io_signals"] =
      c::JsonValueDTO{Array{c::JsonValueDTO{gpio}}};
  const auto refused = d::ResolveRetainedIoSignalCatalog(graph);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"),
            "io_signal_provider_unsupported");
  const auto derived = d::DeriveIoProviderDomains(
      graph, bundle->value().capability_provider_bindings, profiles, manifests);
  ASSERT_FALSE(derived.ok());
  EXPECT_EQ(derived.error().details.at("reason"),
            "io_signal_provider_unsupported");
}

TEST_F(IoProviderDomainTest,
       RosConnectionOwnsItsComponentAndUnrelatedActualArmDoesNotReviseIt) {
  const auto ros_path = path / "ros-output";
  ASSERT_TRUE(std::filesystem::create_directory(ros_path));
  const auto retained = d::test::materialize_boolean_io_retained_fixture(
      ros_path, false, true, true);
  ASSERT_TRUE(retained.bundle);
  const auto &original = retained.bundle->value().cell_graph;
  ASSERT_EQ(original.control_domains.size(), 1U);
  const auto *provider = original.control_domains.front().provider();
  ASSERT_NE(provider, nullptr);
  const d::FilesystemVerifiedRetainedProviderManifestSource source(ros_path);
  const auto manifest = source.get_verified(
      {retained.bundle->retained_bundle_id(), provider->provider_manifest_id,
       provider->provider_manifest_revision});
  ASSERT_TRUE(manifest);
  const std::vector<d::QualifiedProviderManifest> ros_manifests{*manifest};
  const auto arm =
      d::test::make_lite6_ros2_control_shared_consumer_retention_fixture();
  const auto &physical = arm.qualified.deployment().graph().graph();
  ASSERT_FALSE(physical.control_bindings.empty());
  auto graph = original;
  graph.controllable_components.insert(graph.controllable_components.end(),
                                       physical.controllable_components.begin(),
                                       physical.controllable_components.end());
  graph.runtime_units.insert(graph.runtime_units.end(),
                             physical.runtime_units.begin(),
                             physical.runtime_units.end());
  graph.control_domains.insert(graph.control_domains.end(),
                               physical.control_domains.begin(),
                               physical.control_domains.end());
  graph.control_bindings = physical.control_bindings;
  const auto independent = d::DeriveIoProviderDomains(
      graph, retained.bundle->value().capability_provider_bindings, profiles,
      ros_manifests);
  ASSERT_TRUE(independent.ok()) << independent.error().message;
  ASSERT_EQ(independent.value().provider_domains.size(), 1U);
  EXPECT_EQ(independent.value().provider_domains.front(),
            original.control_domains.front());
  // Deliberately untrusted ownership mutation: merely adding a second domain
  // must not make a ROS connection share its component with a controller.
  graph.control_bindings.front().controllable_component_id =
      provider->controllable_component_id;
  const auto refused = d::DeriveIoProviderDomains(
      graph, retained.bundle->value().capability_provider_bindings, profiles,
      ros_manifests);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"),
            "io_ros_connection_component_has_controller_binding");
}

TEST_F(IoProviderDomainTest,
       RosConnectionRuleDoesNotChangeNonRosMixedComponentDerivation) {
  const auto arm =
      d::test::make_lite6_ros2_control_shared_consumer_retention_fixture();
  const auto &physical = arm.qualified.deployment().graph().graph();
  ASSERT_FALSE(physical.control_bindings.empty());
  auto graph = bundle->value().cell_graph;
  auto controller = physical.control_bindings.front();
  controller.controllable_component_id =
      graph.controllable_components.front().controllable_component_id;
  graph.control_bindings.push_back(std::move(controller));
  // Candidate-layer check only: no mutation is passed as a verified bundle.
  // The new84 ROS guard must not become a blanket rule for non-ROS providers.
  const auto derived = d::DeriveIoProviderDomains(
      graph, bundle->value().capability_provider_bindings, profiles, manifests);
  ASSERT_TRUE(derived.ok()) << derived.error().message;
  ASSERT_EQ(derived.value().provider_domains.size(), 1U);
  EXPECT_EQ(derived.value().provider_domains.front(),
            bundle->value().cell_graph.control_domains.front());
}

} // namespace
