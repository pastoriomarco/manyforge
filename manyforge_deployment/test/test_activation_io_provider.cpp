#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <optional>
#include <set>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_io_provider_internal.hpp"
#include "activation_prepared_model_internal.hpp"
#include "prepared_activation_materialization_internal.hpp"

namespace {
namespace d = manyforge::deployment;
namespace c = manyforge::core;
namespace detail = manyforge::deployment::detail;
using Value = d::CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;

Value With(Value value, std::string_view key, Value selected) {
  auto object = value.as_object();
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [&](const auto &row) { return row.first == key; });
  if (found == object.end())
    object.emplace_back(key, std::move(selected));
  else
    found->second = std::move(selected);
  return Value(std::move(object));
}

Value Device(std::string id, std::optional<double> timeout = std::nullopt) {
  Object policy{{"controllable_component_id", Value(id)}};
  if (timeout)
    policy.emplace_back("lifecycle_command_timeout_s", Value(*timeout));
  return Value(Object{
      {"component_id", Value(id)},
      {"component_profile_id", Value("io_device_v0")},
      {"target_settings", Value(Object{})},
      {"configuration",
       Value(Object{
           {"runtime_kind", Value("io_provider")},
           {"runtime_configuration",
            Value(Object{
                {"io_provider_devices", Value(Array{Value(std::move(policy))})},
                {"io_signals",
                 Value(Array{
                     Value(Object{
                         {"controllable_component_id", Value(id)},
                         {"command_target_id", Value("io")},
                         {"resource_role_id", Value("ready")},
                         {"signal_name", Value("ready")},
                         {"source_id", Value(id + ".ready")},
                         {"value_type", Value("bool")},
                         {"direction", Value("input")},
                         {"update_mode", Value("change_only")},
                         {"transition_capability", Value("sampled_only")},
                         {"freshness",
                          Value(Object{{"mode", Value("provider_validity")}})},
                         {"provider",
                          Value(Object{{"profile", Value("mock")}})}}),
                     Value(Object{
                         {"controllable_component_id", Value(id)},
                         {"command_target_id", Value("io")},
                         {"resource_role_id", Value("lamp")},
                         {"signal_name", Value("lamp")},
                         {"source_id", Value(id + ".lamp")},
                         {"value_type", Value("bool")},
                         {"direction", Value("output")},
                         {"provider",
                          Value(Object{
                              {"profile", Value("mock_output")},
                              {"channel", Value(id + ".lamp")}})}})})}})}})}});
}

Value Intent(Array devices) {
  return Value(Object{{"api_version", Value("manyforge.deployment_intent.v2")},
                      {"deployment_artifact_id", Value("authored_io")},
                      {"name", Value("authored_io")},
                      {"compatibility", Value(Object{
                          {"lane", Value("manyforge-only")},
                          {"provides", Value(Array{})},
                          {"tags", Value(Array{})}})},
                      {"attachments", Value(Array{})},
                      {"components", Value(std::move(devices))},
                      {"policies", Value(Object{})}});
}

TEST(
    ActivationIoProvider,
    ActualAuthoredModelFreePipelineProducesQualifiedCatalogArtifactsAndDomains) {
  const auto intent = detail::parse_activation_deployment_intent(
      Intent(Array{Device("station")}), {});
  ASSERT_EQ(intent.components.size(), 1U);
  EXPECT_FALSE(intent.components.front().model_source);
  EXPECT_FALSE(intent.components.front().root_pose_in_cell);
  const auto root = std::filesystem::path(MANYFORGE_WORKSPACE_ROOT);
  const auto profiles =
      detail::resolve_activation_component_profiles(root, intent);
  ASSERT_EQ(profiles.size(), 1U);
  EXPECT_EQ(profiles.front().runtime_kind,
            detail::ActivationRuntimeKind::kIoProvider);
  const auto models =
      detail::qualify_activation_prepared_models(intent, profiles, {});
  EXPECT_TRUE(models.empty());
  const auto materialized = detail::materialize_prepared_activation(
      root, models, intent, c::CellId("authored_io_cell"), profiles, true);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &bundle = *materialized.bundle;
  const auto &graph = bundle.value().cell_graph;
  EXPECT_TRUE(graph.model_instances.empty());
  EXPECT_TRUE(graph.control_bindings.empty());
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  ASSERT_EQ(graph.controllable_components.size(), 1U);
  ASSERT_EQ(graph.control_domains.size(), 1U);
  ASSERT_NE(graph.control_domains.front().provider(), nullptr);
  EXPECT_EQ(
      graph.control_domains.front().provider()->lifecycle_command_timeout_ns,
      30'000'000'000LL);
  EXPECT_EQ(graph.control_domains.front().provider()->provider_runtime_unit_id,
            graph.runtime_units.front().runtime_unit_id);
  EXPECT_TRUE(bundle.value().desired_control_configurations.empty());
  EXPECT_TRUE(bundle.value().compiled_products.control_domain_products.empty());
  ASSERT_TRUE(materialized.model_map);
  const auto *target = materialized.model_map->find_target(
      c::ControllableComponentId("station"), c::CommandTargetId("io"));
  ASSERT_NE(target, nullptr);
  EXPECT_TRUE(target->model_groups.empty());
  EXPECT_TRUE(target->frames.empty());
  EXPECT_TRUE(target->collision_bodies.empty());
  ASSERT_NE(materialized.canonical_empty_bundle, nullptr);
  EXPECT_TRUE(materialized.canonical_empty_bundle->value()
                  .cell_graph.controllable_components.empty());
  const auto &empty = materialized.canonical_empty_bundle->value();
  const auto empty_signals = d::ResolveRetainedIoSignalCatalog(empty.cell_graph);
  const auto empty_devices = d::ResolveRetainedIoProviderDevices(empty.cell_graph);
  ASSERT_TRUE(empty_signals.ok());
  ASSERT_TRUE(empty_devices.ok());
  EXPECT_TRUE(empty_signals.value().empty());
  EXPECT_TRUE(empty_devices.value().empty());
  EXPECT_EQ(empty.compiled_products.runtime_unit_products,
            bundle.value().compiled_products.runtime_unit_products);
  const auto &component = graph.controllable_components.front();
  const auto reference = std::find_if(
      bundle.value().catalogs.records.begin(),
      bundle.value().catalogs.records.end(), [&](const auto &row) {
        const auto *type =
            std::get_if<c::ComponentTypeCatalogRecordRefDTO>(&row);
        return type && type->record_id == component.component_type_id &&
               type->record_revision == component.component_type_revision;
      });
  ASSERT_NE(reference, bundle.value().catalogs.records.end());
  const auto &type = std::get<c::ComponentTypeCatalogRecordRefDTO>(*reference);
  const auto artifact =
      std::find_if(materialized.catalog_artifacts.begin(),
                   materialized.catalog_artifacts.end(), [&](const auto &row) {
                     return row.content_addressed_store_key ==
                            type.content_addressed_store_key;
                   });
  ASSERT_NE(artifact, materialized.catalog_artifacts.end());
  EXPECT_EQ(d::sha256_digest(artifact->bytes), type.content_digest.value());
  EXPECT_TRUE(d::verify_retained_bundle_identity(bundle));
}

TEST(ActivationIoProvider,
     DeviceSettingsDoNotChangeReusableStructuralTypeOrCreateAnotherHost) {
  const auto intent = detail::parse_activation_deployment_intent(
      Intent(Array{Device("station_a", 7.0), Device("station_b", 19.0)}), {});
  const auto root = std::filesystem::path(MANYFORGE_WORKSPACE_ROOT);
  const auto profiles =
      detail::resolve_activation_component_profiles(root, intent);
  const auto materialized = detail::materialize_prepared_activation(
      root, {}, intent, c::CellId("authored_io_pair"), profiles, false);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &graph = materialized.bundle->value().cell_graph;
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  ASSERT_EQ(graph.controllable_components.size(), 2U);
  ASSERT_EQ(graph.control_domains.size(), 2U);
  EXPECT_EQ(graph.controllable_components[0].component_type_id,
            graph.controllable_components[1].component_type_id);
  EXPECT_EQ(graph.controllable_components[0].component_type_revision,
            graph.controllable_components[1].component_type_revision);
  const auto *first = graph.control_domains[0].provider();
  const auto *second = graph.control_domains[1].provider();
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_NE(first->provider_instance_id, second->provider_instance_id);
  EXPECT_EQ(first->provider_runtime_unit_id, second->provider_runtime_unit_id);
  EXPECT_EQ(first->lifecycle_plan_catalog_digest,
            second->lifecycle_plan_catalog_digest);
  EXPECT_NE(first->signal_configuration_digest,
            second->signal_configuration_digest);
  EXPECT_EQ((std::set<c::Duration>{first->lifecycle_command_timeout_ns,
                                   second->lifecycle_command_timeout_ns}),
            (std::set<c::Duration>{7'000'000'000LL, 19'000'000'000LL}));
}

TEST(ActivationIoProvider,
     ModelSourcePoseAndStopFieldsCannotBorrowModelFreeAuthority) {
  for (const auto key : {"model_source", "root_pose_in_cell", "frame_prefix"}) {
    auto device = With(Device("station"), key, Value(Object{}));
    EXPECT_THROW(
        detail::parse_activation_deployment_intent(Intent(Array{device}), {}),
        std::invalid_argument);
  }
  auto device = Device("station");
  const auto configuration = *device.find_object_member("configuration");
  device =
      With(device, "configuration",
           With(configuration, "default_fault_stop_profile", Value(Object{})));
  EXPECT_THROW(
      detail::parse_activation_deployment_intent(Intent(Array{device}), {}),
      std::invalid_argument);
}
TEST(ActivationIoProvider,
     InputOnlySelectsActualManifestOffersWithoutInstallingOutputCommandSchema) {
  auto device = Device("station");
  auto configuration = *device.find_object_member("configuration");
  auto runtime = *configuration.find_object_member("runtime_configuration");
  auto signals = runtime.find_object_member("io_signals")->as_array();
  signals.pop_back();
  device = With(device, "configuration",
                With(configuration, "runtime_configuration",
                     With(runtime, "io_signals", Value(std::move(signals)))));
  const auto intent =
      detail::parse_activation_deployment_intent(Intent(Array{device}), {});
  const auto root = std::filesystem::path(MANYFORGE_WORKSPACE_ROOT);
  const auto profiles =
      detail::resolve_activation_component_profiles(root, intent);
  const auto materialized = detail::materialize_prepared_activation(
      root, {}, intent, c::CellId("input_only_cell"), profiles, false);
  ASSERT_NE(materialized.bundle, nullptr);
  const auto &records = materialized.bundle->value().catalogs.records;
  EXPECT_TRUE(std::any_of(records.begin(), records.end(), [](const auto &row) {
    const auto *capability =
        std::get_if<c::CapabilityDefinitionCatalogRecordRefDTO>(&row);
    return capability && capability->record_id.value() == "io.gpio.set_output";
  }));
  EXPECT_FALSE(std::any_of(records.begin(), records.end(), [](const auto &row) {
    const auto *schema = std::get_if<c::CommandSchemaCatalogRecordRefDTO>(&row);
    return schema && schema->record_id.value() == "io.signal_write_boolean";
  }));
}

TEST(ActivationIoProvider,
     ForeignSignalAndArmProviderRefuseInActualComponentCompilation) {
  for (const bool arm_provider : {false, true}) {
    auto device = Device("station");
    auto configuration = *device.find_object_member("configuration");
    auto runtime = *configuration.find_object_member("runtime_configuration");
    auto signals = runtime.find_object_member("io_signals")->as_array();
    if (arm_provider) {
      auto row = With(signals.front(), "update_mode", Value("sampled"));
      row = With(row, "freshness",
                 Value(Object{{"mode", Value("sample_age")},
                              {"maximum_age_ns", Value(1000.0)}}));
      signals.front() =
          With(row, "provider",
               Value(Object{{"profile", Value("xarm_controller_input")},
                            {"topic", Value("/arm/cgpio")},
                            {"input_bit", Value(0.0)},
                            {"ingress_depth", Value(4.0)}}));
    } else {
      signals.front() =
          With(signals.front(), "controllable_component_id", Value("foreign"));
    }
    runtime = With(runtime, "io_signals", Value(std::move(signals)));
    device = With(device, "configuration",
                  With(configuration, "runtime_configuration", runtime));
    const auto intent =
        detail::parse_activation_deployment_intent(Intent(Array{device}), {});
    const auto root = std::filesystem::path(MANYFORGE_WORKSPACE_ROOT);
    const auto profiles =
        detail::resolve_activation_component_profiles(root, intent);
    try {
      (void)detail::materialize_prepared_activation(
          root, {}, intent, c::CellId("invalid_io_cell"), profiles, false);
      FAIL() << "foreign/arm I/O source accepted on generic device";
    } catch (const d::ClosedSchemaError &error) {
      EXPECT_TRUE(arm_provider);
      EXPECT_NE(std::string(error.what()).find(
                    "unknown discriminator xarm_controller_input"),
                std::string::npos);
    } catch (const std::invalid_argument &error) {
      EXPECT_FALSE(arm_provider);
      EXPECT_NE(std::string(error.what())
                    .find("I/O-only component has an invalid signal owner, "
                          "name or arm provider"),
                std::string::npos);
    }
  }
}
} // namespace
