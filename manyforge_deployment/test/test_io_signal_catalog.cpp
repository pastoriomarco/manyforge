#include <gtest/gtest.h>

#include <limits>

#include "ros2_control_execution_runtime_unit_internal.hpp"
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <map>
#include <set>
#include <tuple>

namespace {
using namespace manyforge::core;
using manyforge::deployment::ResolveRetainedIoSignalCatalog;
using Object = JsonValueDTO::Object;
using Array = JsonValueDTO::Array;

Object input_row(std::string name = "ready", std::string source = "source.io") {
  return {
      {"controllable_component_id", JsonValueDTO{std::string("device.io")}},
      {"command_target_id", JsonValueDTO{std::string("gpio")}},
      {"resource_role_id", JsonValueDTO{name}},
      {"signal_name", JsonValueDTO{name}},
      {"source_id", JsonValueDTO{std::move(source)}},
      {"value_type", JsonValueDTO{std::string("bool")}},
      {"direction", JsonValueDTO{std::string("input")}},
      {"update_mode", JsonValueDTO{std::string("change_only")}},
      {"transition_capability", JsonValueDTO{std::string("sampled_only")}},
      {"freshness",
       JsonValueDTO{
           Object{{"mode", JsonValueDTO{std::string("provider_validity")}}}}},
      {"provider",
       JsonValueDTO{Object{{"profile", JsonValueDTO{std::string("mock")}},
                           {"initial_value", JsonValueDTO{false}},
                           {"ingress_depth", JsonValueDTO{4.0}}}}}};
}

CellGraphDTO io_graph(Array rows = {JsonValueDTO{input_row()}}) {
  return {"manyforge.cell_graph.v1",
          GraphRevision("graph.io"),
          CellId("cell.io"),
          {{RuntimeUnitId("host.io"),
            RuntimeProfileId("profile.io"),
            RuntimeProfileRevision("revision.io"),
            {},
            {{"io_signals", JsonValueDTO{std::move(rows)}},
             {"controller_profiles", JsonValueDTO{Array{}}},
             {"initial_joint_positions_rad", JsonValueDTO{Array{}}},
             {"motion_target_limits", JsonValueDTO{Array{}}}}}},
          {},
          {{ControllableComponentId("device.io"),
            ComponentTypeId("io.module"),
            ComponentTypeRevision("revision.io"),
            {},
            {RuntimeUnitId("host.io")},
            {}}},
          {},
          {},
          {}};
}

Object &provider(Object &row) {
  return std::get<Object>(row.at("provider").value);
}

// An adapter profile: any profile token but the generic ones. Its provider
// object is opaque here; the adapter serving it validates it.
constexpr const char *kAdapterProfile = "vendor_digital";

Object adapter_input(std::string name, std::string source, double pin) {
  auto row = input_row(std::move(name), std::move(source));
  row["update_mode"] = JsonValueDTO{std::string("sampled")};
  row["freshness"] = JsonValueDTO{Object{
      {"mode", JsonValueDTO{std::string("sample_age")}},
      {"maximum_age_ns", JsonValueDTO{250'000'000.0}}}};
  provider(row) = {{"profile", JsonValueDTO{std::string(kAdapterProfile)}},
                   {"bank", JsonValueDTO{std::string("tool")}},
                   {"pin", JsonValueDTO{pin}}, {"ingress_depth", JsonValueDTO{4.0}}};
  return row;
}

TEST(IoSignalCatalogTest, AdapterInputKeepsItsProviderOpaqueUnderGenericRules) {
  const auto first = adapter_input("first", "source.bank", 0.0);
  const auto second = adapter_input("second", "source.bank", 7.0);
  auto result = ResolveRetainedIoSignalCatalog(
      io_graph({JsonValueDTO{first}, JsonValueDTO{second}}));
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().size(), 2U);
  const auto &signal = result.value().front();
  EXPECT_EQ(signal.provider.profile, kAdapterProfile);
  EXPECT_EQ(signal.provider.ingress_depth, 4U);
  EXPECT_EQ(signal.provider.adapter_fields,
            (Object{{"bank", JsonValueDTO{std::string("tool")}},
                    {"pin", JsonValueDTO{0.0}}}));
  EXPECT_EQ(signal.descriptor.liveness, IoSignalLivenessMode::kPeriodic);
  // The generic rules still hold: an ingress queue, sampled updates and one
  // depth per source.
  auto no_depth = first;
  provider(no_depth).erase("ingress_depth");
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{no_depth}})).ok());
  auto change_only = first;
  change_only["update_mode"] = JsonValueDTO{std::string("change_only")};
  result = ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{change_only}}));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "io_signal_transition_unsupported");
  auto other_depth = second;
  provider(other_depth)["ingress_depth"] = JsonValueDTO{8.0};
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                   io_graph({JsonValueDTO{first}, JsonValueDTO{other_depth}}))
                   .ok());
  // A generic profile keeps its own closed provider shape.
  auto generic = first;
  provider(generic)["profile"] = JsonValueDTO{std::string("ros_bool")};
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{generic}})).ok());
}

ControlBindingDTO capability_binding(std::string component, std::string target,
                                     std::string capability) {
  const std::string digest = "sha256:" + std::string(64, 'a');
  return {ControlBindingId{component + "." + target + "." + capability},
          ControlBindingRevision{digest},
          ControllableComponentId{component},
          CommandTargetId{target},
          CapabilityId{std::move(capability)},
          CapabilityDefinitionRevision{digest},
          ControlDomainId{"domain.robot"},
          {},
          ControlInterfaceContractId{"interface"},
          ControlInterfaceRevision{digest},
          ControllerTopologyId{"topology"},
          TopologyRevision{digest},
          {},
          {},
          ProviderInstanceId{"provider"},
          RuntimeUnitId("host.io"),
          ProviderManifestId{"manifest"},
          ProviderManifestRevision{digest},
          StateAuthorityId{"state"},
          StateAuthorityClass::kMeasured,
          StopEvidencePolicyId{"stop"},
          StopEvidencePolicyRevision{digest},
          ContainmentGranularity::kResource,
          {}};
}

TEST(IoSignalCatalogTest, CompiledCatalogDeclaresOneLostSignalPerGripperTarget) {
  using manyforge::deployment::ResolveCompiledIoSignalCatalog;
  auto graph = io_graph();
  graph.control_bindings = {
      capability_binding("robot.a", "gripper", "tool.gripper.command"),
      // A second capability of the same gripper target adds nothing.
      capability_binding("robot.a", "gripper", "tool.gripper.feedback"),
      capability_binding("robot.a", "arm", "execution.trajectory.execute"),
      capability_binding("robot.b", "hand", "tool.gripper.command")};
  // The authored catalog is unchanged.
  const auto authored = ResolveRetainedIoSignalCatalog(graph);
  ASSERT_TRUE(authored.ok());
  ASSERT_EQ(authored.value().size(), 1U);
  const auto compiled = ResolveCompiledIoSignalCatalog(graph);
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  ASSERT_EQ(compiled.value().size(), 3U);
  std::vector<std::pair<std::string, std::string>> system;
  for (const auto &row : compiled.value()) {
    if (row.provider.profile != "system_carried_object")
      continue;
    system.emplace_back(row.descriptor.signal_id.controllable_component_id.value(),
                        row.descriptor.signal_id.name);
    EXPECT_EQ(row.resource_role_id.value(), "carried_object_lost");
    EXPECT_EQ(row.runtime_unit_id, RuntimeUnitId("host.io"));
    EXPECT_EQ(row.descriptor.source_id.value(), "system.carried_object_lost");
    EXPECT_EQ(row.descriptor.direction, IoSignalDirection::kInput);
    EXPECT_EQ(row.descriptor.value_type, IoSignalValueType::kBoolean);
    EXPECT_EQ(row.descriptor.update_mode, IoSignalUpdateMode::kChangeOnly);
    EXPECT_EQ(row.descriptor.transition_capability,
              IoTransitionCapability::kSampledOnly);
    EXPECT_EQ(row.descriptor.freshness.mode, IoFreshnessMode::kProviderValidity);
    EXPECT_EQ(row.descriptor.liveness, IoSignalLivenessMode::kInProcess);
    EXPECT_TRUE(ComputeIoSignalDescriptorDigest(row.descriptor).ok());
  }
  EXPECT_EQ(system, (std::vector<std::pair<std::string, std::string>>{
                        {"robot.a", "carried_object_lost.gripper"},
                        {"robot.b", "carried_object_lost.hand"}}));
  // An authored row cannot take a system identity.
  auto taken = input_row("carried_object_lost.gripper");
  taken["controllable_component_id"] = JsonValueDTO{std::string("robot.a")};
  graph.controllable_components.push_back(
      {ControllableComponentId("robot.a"), ComponentTypeId("robot"),
       ComponentTypeRevision("revision.robot"), {}, {RuntimeUnitId("host.io")}, {}});
  graph.runtime_units.front().configuration["io_signals"] =
      JsonValueDTO{Array{JsonValueDTO{input_row()}, JsonValueDTO{taken}}};
  ASSERT_TRUE(ResolveRetainedIoSignalCatalog(graph).ok());
  const auto conflict = ResolveCompiledIoSignalCatalog(graph);
  ASSERT_FALSE(conflict.ok());
  EXPECT_EQ(conflict.error().details.at("reason"), "io_system_signal_conflict");
  // An authored row on the reserved system source is refused too (the
  // generated rows share that source with each other, never with authored).
  const auto reserved = input_row("authored_name", "system.carried_object_lost");
  graph.runtime_units.front().configuration["io_signals"] =
      JsonValueDTO{Array{JsonValueDTO{input_row()}, JsonValueDTO{reserved}}};
  ASSERT_TRUE(ResolveRetainedIoSignalCatalog(graph).ok());
  const auto reserved_conflict = ResolveCompiledIoSignalCatalog(graph);
  ASSERT_FALSE(reserved_conflict.ok());
  EXPECT_EQ(reserved_conflict.error().details.at("reason"),
            "io_system_signal_conflict");
}

// `controller_owned` is authored per output (default false);
// Mock outputs report the written value back, so they have read-back.
TEST(IoSignalCatalogTest, OutputControllerOwnedIsAuthoredAndMockOutputsReadBack) {
  auto valve = input_row("valve", "source.valve");
  valve["direction"] = JsonValueDTO{std::string("output")};
  valve.erase("update_mode");
  valve.erase("transition_capability");
  valve.erase("freshness");
  provider(valve) = {{"profile", JsonValueDTO{std::string("mock_output")}},
                     {"channel", JsonValueDTO{std::string("valve")}}};
  auto owned = valve;
  owned["signal_name"] = JsonValueDTO{std::string("owned")};
  owned["resource_role_id"] = JsonValueDTO{std::string("owned")};
  owned["source_id"] = JsonValueDTO{std::string("source.owned")};
  owned["controller_owned"] = JsonValueDTO{true};
  provider(owned)["channel"] = JsonValueDTO{std::string("owned")};
  const auto result = ResolveRetainedIoSignalCatalog(
      io_graph({JsonValueDTO{valve}, JsonValueDTO{owned}}));
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().size(), 2U);
  for (const auto &row : result.value()) {
    EXPECT_TRUE(row.descriptor.readback);
    EXPECT_EQ(HoldsChangedOutside(row.descriptor),
              row.descriptor.signal_id.name != "owned");
    EXPECT_EQ(row.descriptor.controller_owned,
              row.descriptor.signal_id.name == "owned");
  }
  const auto *found = manyforge::deployment::FindChangedOutsideOutput(
      result.value(), ControllableComponentId("device.io"),
      "valve.changed_outside");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->descriptor.signal_id.name, "valve");
  const auto derived = manyforge::deployment::FindProgramSignalMatches(
      result.value(), {ControllableComponentId("device.io"), "valve.changed_outside"});
  ASSERT_EQ(derived.size(), 1U);
  EXPECT_EQ(derived.front().retained, found);
  EXPECT_TRUE(derived.front().changed_outside);
  EXPECT_EQ(derived.front().descriptor, ChangedOutsideDescriptor(found->descriptor));
  const auto derived_binding = manyforge::deployment::resolve_program_signal_binding(
      {ControllableComponentId("device.io"), "valve.changed_outside"}, result.value());
  EXPECT_EQ(derived_binding.target.component_id, found->descriptor.signal_id.controllable_component_id);
  EXPECT_EQ(derived_binding.target.command_target_id, found->command_target_id);
  EXPECT_EQ(derived_binding.runtime_unit_id, found->runtime_unit_id);
  const auto derived_digest = ComputeIoSignalDescriptorDigest(derived.front().descriptor);
  ASSERT_TRUE(derived_digest.ok());
  EXPECT_EQ(derived_binding.signal_descriptor_digest, derived_digest.value());
  const auto ordinary = manyforge::deployment::FindProgramSignalMatches(
      result.value(), {ControllableComponentId("device.io"), "valve"});
  ASSERT_EQ(ordinary.size(), 1U);
  EXPECT_FALSE(ordinary.front().changed_outside);
  EXPECT_EQ(ordinary.front().descriptor, found->descriptor);
  EXPECT_TRUE(manyforge::deployment::FindProgramSignalMatches(
      result.value(), {ControllableComponentId("device.io"), "owned.changed_outside"}).empty());
  // Static derived references require readback as well as an unowned output.
  // Model a retained output without readback independently of the Mock provider.
  auto without_readback = result.value();
  for (auto &row : without_readback) {
    if (row.descriptor.signal_id.name == "valve")
      row.descriptor.readback = false;
  }
  EXPECT_TRUE(manyforge::deployment::FindProgramSignalMatches(
      without_readback,
      {ControllableComponentId("device.io"), "valve.changed_outside"}).empty());
  auto ambiguous = result.value();
  ambiguous.push_back(*found);
  EXPECT_EQ(manyforge::deployment::FindProgramSignalMatches(
      ambiguous, {ControllableComponentId("device.io"), "valve.changed_outside"}).size(), 2U);
  EXPECT_THROW(static_cast<void>(manyforge::deployment::resolve_program_signal_binding(
                   {ControllableComponentId("device.io"), "valve.changed_outside"}, ambiguous)),
               manyforge::deployment::ProgramBindingCompilerError);
  EXPECT_EQ(manyforge::deployment::FindChangedOutsideOutput(
                result.value(), ControllableComponentId("device.io"), "valve"),
            nullptr);

  owned["controller_owned"] = JsonValueDTO{std::string("yes")};
  const auto invalid = ResolveRetainedIoSignalCatalog(
      io_graph({JsonValueDTO{valve}, JsonValueDTO{owned}}));
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().details.at("reason"), "io_signal_configuration_invalid");
  auto input = input_row();
  input["controller_owned"] = JsonValueDTO{false};
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{input}})).ok());
}

// The `.changed_outside` suffix names the core-held signal of an output.
TEST(IoSignalCatalogTest, AuthoredSignalNamesCannotTakeTheChangedOutsideSuffix) {
  const auto result = ResolveRetainedIoSignalCatalog(
      io_graph({JsonValueDTO{input_row("valve.changed_outside")}}));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "io_system_signal_conflict");
}

TEST(IoSignalCatalogTest, AdapterOutputAliasesShareResourceAndUseAckBudget) {
  auto output = input_row("valve", "source.valve");
  output["direction"] = JsonValueDTO{std::string("output")};
  output.erase("update_mode");
  output.erase("transition_capability");
  output.erase("freshness");
  provider(output) = {{"profile", JsonValueDTO{std::string(kAdapterProfile)}},
                      {"bank", JsonValueDTO{std::string("tool")}},
                      {"pin", JsonValueDTO{4.0}}};
  auto alias = output;
  alias["signal_name"] = JsonValueDTO{std::string("valve_alias")};
  alias["resource_role_id"] = JsonValueDTO{std::string("valve_alias")};
  auto graph = io_graph({JsonValueDTO{output}, JsonValueDTO{alias}});
  ASSERT_TRUE(manyforge::deployment::MaterializeRetainedIoProviderDevices(graph).ok());
  const auto signals = ResolveRetainedIoSignalCatalog(graph);
  ASSERT_TRUE(signals.ok());
  ASSERT_EQ(signals.value().size(), 2U);
  EXPECT_EQ(signals.value()[0].descriptor.output_resource,
            signals.value()[1].descriptor.output_resource);
  const auto devices = manyforge::deployment::ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(devices.ok());
  ASSERT_EQ(devices.value().size(), 1U);
  EXPECT_EQ(devices.value().front().output_source_ack_timeout_ns, 500'000'000);
  // Another address is another channel; an output has no ingress queue.
  provider(alias)["pin"] = JsonValueDTO{5.0};
  const auto other = ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{alias}}));
  ASSERT_TRUE(other.ok());
  EXPECT_NE(other.value().front().descriptor.output_resource,
            signals.value()[0].descriptor.output_resource);
  provider(output)["ingress_depth"] = JsonValueDTO{4.0};
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{output}})).ok());
}

TEST(IoSignalCatalogTest,
     ModelFreeHostResolvesSealedSignalsWithoutCoordinateResources) {
  const auto graph = io_graph();
  const auto catalog = ResolveRetainedIoSignalCatalog(graph);
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  ASSERT_EQ(catalog.value().size(), 1U);
  EXPECT_TRUE(graph.model_instances.empty());
  EXPECT_TRUE(
      graph.controllable_components.front().model_role_bindings.empty());
  EXPECT_EQ(catalog.value().front().descriptor.signal_id,
            (IoSignalId{ControllableComponentId("device.io"), "ready"}));
  EXPECT_EQ(catalog.value().front().provider.initial_value, false);
  EXPECT_EQ(catalog.value().front().descriptor.freshness.mode,
            IoFreshnessMode::kProviderValidity);
  EXPECT_FALSE(catalog.value().front().descriptor.freshness.maximum_age_ns);
  EXPECT_EQ(catalog.value().front().provider.ingress_depth, 4U);
  EXPECT_FALSE(manyforge::deployment::detail::
                   require_ros2_control_execution_runtime_required_rows(
                       graph, graph.runtime_units.front()));
}

TEST(IoSignalCatalogTest, ObsoleteMockAutoAnswerFieldsAreRejected) {
  ASSERT_TRUE(ResolveRetainedIoSignalCatalog(io_graph()).ok());
  for (const auto *field : {"auto_answer_delay_ns", "manual_only"}) {
    auto row = input_row();
    provider(row)[field] = std::string_view(field) == "auto_answer_delay_ns"
                               ? JsonValueDTO{250000000.0}
                               : JsonValueDTO{false};
    const auto result =
        ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}}));
    EXPECT_FALSE(result.ok()) << field;
  }
}

TEST(IoSignalCatalogTest, OptionalAbsenceIsInertAndMalformedRowsFailClosed) {
  auto graph = io_graph();
  graph.runtime_units.front().configuration.erase("io_signals");
  ASSERT_TRUE(ResolveRetainedIoSignalCatalog(graph).ok());
  EXPECT_TRUE(ResolveRetainedIoSignalCatalog(graph).value().empty());
  auto row = input_row();
  provider(row).erase("initial_value");
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  row = input_row();
  provider(row)["initial_value"] = JsonValueDTO{1.0};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  row = input_row();
  provider(row)["ingress_depth"] = JsonValueDTO{0.0};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  row = input_row();
  provider(row)["ingress_depth"] = JsonValueDTO{1.5};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  // The product maximum: 1024 is accepted, 1025 refused with the same code.
  row = input_row();
  provider(row)["ingress_depth"] = JsonValueDTO{1024.0};
  EXPECT_TRUE(ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  provider(row)["ingress_depth"] = JsonValueDTO{1025.0};
  const auto above = ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}}));
  ASSERT_FALSE(above.ok());
  EXPECT_EQ(above.error().details.at("reason"), "io_signal_configuration_invalid");
  row = input_row();
  row["unknown"] = JsonValueDTO{true};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
}

TEST(IoSignalCatalogTest,
     SharedSourceRequiresCoherentDepthAndKeepsPerInputInitialValuesIndependent) {
  auto second = input_row("other");
  provider(second)["initial_value"] = JsonValueDTO{true};
  EXPECT_TRUE(ResolveRetainedIoSignalCatalog(
                  io_graph({JsonValueDTO{input_row()}, JsonValueDTO{second}}))
                  .ok());
  provider(second)["ingress_depth"] = JsonValueDTO{5.0};
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                   io_graph({JsonValueDTO{input_row()}, JsonValueDTO{second}}))
                   .ok());
  second["source_id"] = JsonValueDTO{std::string("source.other")};
  EXPECT_TRUE(ResolveRetainedIoSignalCatalog(
                  io_graph({JsonValueDTO{input_row()}, JsonValueDTO{second}}))
                  .ok());
  provider(second)["initial_value"] = JsonValueDTO{-1.0};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{second}})).ok());
  auto periodic = input_row();
  provider(periodic)["sample_period_ns"] = JsonValueDTO{10.0};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{periodic}})).ok());
}

TEST(IoSignalCatalogTest,
     DuplicateIdentitiesWrongOwnerAndUnsupportedProvidersRefuse) {
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(
          io_graph({JsonValueDTO{input_row()}, JsonValueDTO{input_row()}}))
          .ok());
  auto graph = io_graph();
  graph.controllable_components.front().runtime_unit_ids.clear();
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(graph).ok());
  auto row = input_row();
  provider(row)["profile"] = JsonValueDTO{std::string("uninstalled")};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  row = input_row();
  row["update_mode"] = JsonValueDTO{std::string("sampled")};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
}

TEST(IoSignalCatalogTest,
     DescriptorDigestExcludesAddressButBindsFreshnessAndIdentity) {
  auto row = input_row();
  auto a = ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}}));
  ASSERT_TRUE(a.ok());
  provider(row)["ingress_depth"] = JsonValueDTO{99.0};
  auto b = ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}}));
  ASSERT_TRUE(b.ok());
  const auto digest =
      ComputeIoSignalDescriptorDigest(a.value().front().descriptor);
  ASSERT_TRUE(digest.ok());
  EXPECT_EQ(
      digest.value(),
      ComputeIoSignalDescriptorDigest(b.value().front().descriptor).value());
  auto changed = b.value().front().descriptor;
  changed.freshness = {IoFreshnessMode::kSampleAge, 21};
  changed.liveness = IoSignalLivenessMode::kPeriodic;
  EXPECT_NE(digest.value(), ComputeIoSignalDescriptorDigest(changed).value());
  changed = b.value().front().descriptor;
  changed.source_id = InterruptSourceId("another.source");
  EXPECT_NE(digest.value(), ComputeIoSignalDescriptorDigest(changed).value());
  changed.freshness.maximum_age_ns = 0;
  EXPECT_FALSE(ComputeIoSignalDescriptorDigest(changed).ok());
  changed = b.value().front().descriptor;
  changed.signal_id.name = std::string(1U, static_cast<char>(0xff));
  EXPECT_FALSE(ComputeIoSignalDescriptorDigest(changed).ok());
}

TEST(IoSignalCatalogTest,
     RuntimeSchemaMaterializesApprovedDepthWithoutCreatingAbsentSignals) {
  using namespace manyforge::deployment;
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      SchemaId(std::string(
          detail::kRos2ControlExecutionRuntimeConfigurationSchemaId)),
      SchemaRevision(std::string(
          detail::kRos2ControlExecutionRuntimeConfigurationSchemaRevision)));
  ASSERT_NE(schema, nullptr);
  const auto ordinary =
      ros2_control_execution_runtime_contract::empty_host_configuration();
  const auto unchanged =
      validate_and_normalize_build_qualified_value(*schema, ordinary);
  EXPECT_EQ(unchanged.normalized_value(), ordinary);
  EXPECT_EQ(unchanged.normalized_value().find_object_member("io_signals"),
            nullptr);
  auto configuration = ordinary.as_object();
  const auto rows = parse_strict_json_model_yaml(R"([
    {"controllable_component_id":"device.io","command_target_id":"gpio",
     "resource_role_id":"ready","signal_name":"ready","source_id":"source.io",
     "value_type":"bool","direction":"input","update_mode":"sampled",
     "transition_capability":"sampled_only",
     "freshness":{"mode":"sample_age","maximum_age_ns":20},
     "provider":{"profile":"ros_bool","topic":"/gpio/ready","liveness":{"mode":"periodic"}}}
  ])")
                        .root();
  configuration.emplace_back("io_signals", rows);
  const auto normalized = validate_and_normalize_build_qualified_value(
      *schema, CanonicalValue(configuration));
  const auto &signal = normalized.normalized_value()
                           .find_object_member("io_signals")
                           ->as_array()
                           .front();
  EXPECT_EQ(signal.find_object_member("provider")
                ->find_object_member("ingress_depth")
                ->as_number(),
            64.0);
  auto mock_row = rows.as_array().front().as_object();
  for (auto &[field, value] : mock_row) {
    if (field == "update_mode")
      value = CanonicalValue("change_only");
    if (field == "freshness")
      value = CanonicalValue(CanonicalValue::Object{
          {"mode", CanonicalValue("provider_validity")}});
    if (field == "provider")
      value = CanonicalValue(
          CanonicalValue::Object{{"profile", CanonicalValue("mock")}});
  }
  auto mock_configuration = ordinary.as_object();
  mock_configuration.emplace_back("io_signals",
                                  CanonicalValue(CanonicalValue::Array{
                                      CanonicalValue(std::move(mock_row))}));
  const auto mock = validate_and_normalize_build_qualified_value(
      *schema, CanonicalValue(std::move(mock_configuration)));
  const auto *provider = mock.normalized_value()
                             .find_object_member("io_signals")
                             ->as_array()
                             .front()
                             .find_object_member("provider");
  ASSERT_NE(provider, nullptr);
  EXPECT_FALSE(provider->find_object_member("initial_value")->as_bool());
  EXPECT_EQ(provider->find_object_member("manual_only"), nullptr);
  EXPECT_EQ(provider->find_object_member("auto_answer_delay_ns"), nullptr);
  EXPECT_EQ(provider->find_object_member("ingress_depth")->as_number(), 64.0);
  for (const auto *field : {"auto_answer_delay_ns", "manual_only"}) {
    auto invalid_provider = provider->as_object();
    invalid_provider.emplace_back(
        field, std::string_view(field) == "auto_answer_delay_ns"
                   ? CanonicalValue(250000000.0)
                   : CanonicalValue(false));
    auto invalid_row = mock.normalized_value()
                           .find_object_member("io_signals")
                           ->as_array().front().as_object();
    for (auto &[name, value] : invalid_row) {
      if (name == "provider")
        value = CanonicalValue(std::move(invalid_provider));
    }
    auto invalid_configuration = ordinary.as_object();
    invalid_configuration.emplace_back(
        "io_signals", CanonicalValue(CanonicalValue::Array{
                          CanonicalValue(std::move(invalid_row))}));
    EXPECT_THROW((void)validate_and_normalize_build_qualified_value(
                     *schema, CanonicalValue(std::move(invalid_configuration))),
                 ClosedSchemaError) << field;
  }
  EXPECT_EQ(rows.as_array()
                .front()
                .find_object_member("provider")
                ->find_object_member("ingress_depth"),
            nullptr);
}

TEST(IoSignalCatalogTest,
     DirectProviderRequiresExactSealedSignalAndRuntimeOwnership) {
  const auto graph = io_graph();
  CapabilityProviderBindingDTO provider{
      CapabilityProviderBindingId("io.read"),
      CapabilityProviderBindingRevision("revision.io"),
      CapabilityId("io.gpio.get_input"),
      CapabilityDefinitionRevision("revision.io"),
      CapabilityScope::kTargetBinding,
      ComponentTargetCapabilityProviderTargetDTO{
          ControllableComponentId("device.io"),
          ComponentTypeRevision("revision.io"), CommandTargetId("gpio")},
      ProviderInstanceId("provider.io"),
      ProviderManifestId("provider.io"),
      ProviderManifestRevision("revision.io"),
      {{EndpointRoleId("io_signal_service"),
        CapabilityEndpointSourceKind::kRuntimeUnit, RuntimeUnitId("host.io"),
        EndpointRoleId("ros2_control_execution_status")}}};
  EXPECT_TRUE(manyforge::deployment::ValidateNonCoordinateProviderTarget(
                  graph, provider)
                  .ok());
  auto stale = provider;
  std::get<ComponentTargetCapabilityProviderTargetDTO>(stale.target)
      .component_type_revision = ComponentTypeRevision("stale");
  EXPECT_FALSE(
      manyforge::deployment::ValidateNonCoordinateProviderTarget(graph, stale)
          .ok());
  stale = provider;
  stale.endpoint_bindings.front().source_id = RuntimeUnitId("another.host");
  EXPECT_FALSE(
      manyforge::deployment::ValidateNonCoordinateProviderTarget(graph, stale)
          .ok());
  stale = provider;
  stale.endpoint_bindings.clear();
  EXPECT_FALSE(
      manyforge::deployment::ValidateNonCoordinateProviderTarget(graph, stale)
          .ok());
  stale = provider;
  std::get<ComponentTargetCapabilityProviderTargetDTO>(stale.target)
      .command_target_id = CommandTargetId("undeclared");
  EXPECT_FALSE(
      manyforge::deployment::ValidateNonCoordinateProviderTarget(graph, stale)
          .ok());
}

Object output_row(std::string name = "lamp",
                  std::string service = "/gpio/set_lamp") {
  return {{"controllable_component_id", JsonValueDTO{std::string("device.io")}},
          {"command_target_id", JsonValueDTO{std::string("gpio")}},
          {"resource_role_id", JsonValueDTO{name}},
          {"signal_name", JsonValueDTO{name}},
          {"source_id", JsonValueDTO{"source." + name}},
          {"value_type", JsonValueDTO{std::string("bool")}},
          {"direction", JsonValueDTO{std::string("output")}},
          {"provider",
           JsonValueDTO{
               Object{{"profile", JsonValueDTO{std::string("ros_set_bool")}},
                      {"service", JsonValueDTO{std::move(service)}}}}}};
}

TEST(IoSignalCatalogTest,
     OutputChannelIsDerivedAndIndependentOfRoleOrComponentAliases) {
  auto first = output_row();
  auto alias = output_row("alias");
  alias["controllable_component_id"] =
      JsonValueDTO{std::string("device.alias")};
  auto graph = io_graph({JsonValueDTO{first}, JsonValueDTO{alias}});
  auto component = graph.controllable_components.front();
  component.controllable_component_id = ControllableComponentId("device.alias");
  graph.controllable_components.push_back(component);
  const auto catalog = ResolveRetainedIoSignalCatalog(graph);
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  ASSERT_EQ(catalog.value().size(), 2U);
  EXPECT_EQ(catalog.value()[0].descriptor.output_resource,
            catalog.value()[1].descriptor.output_resource);
  EXPECT_EQ(catalog.value()[0].provider.service, "/gpio/set_lamp");
  EXPECT_FALSE(catalog.value()[0].provider.topic);
  EXPECT_EQ(catalog.value()[0].provider.ingress_depth, 0U);
  auto other = output_row("other", "/gpio/set_other");
  const auto distinct =
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{other}}));
  ASSERT_TRUE(distinct.ok());
  EXPECT_NE(catalog.value()[0].descriptor.output_resource,
            distinct.value()[0].descriptor.output_resource);
}

TEST(IoSignalCatalogTest,
     OutputRejectsRelativeAddressesAndAuthoredCompletionOrReservation) {
  for (const auto &service : {"", "relative", "~/private", "/", "/gpio//set",
                              "/gpio/", "/2gpio/set", "/gpio/{name}"}) {
    EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                     io_graph({JsonValueDTO{output_row("lamp", service)}}))
                     .ok())
        << service;
  }
  auto row = output_row();
  provider(row)["service"] = JsonValueDTO{std::string("/gpio/\0set", 10U)};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  for (const auto &field :
       {"output_completion", "readback_signal_id", "output_resource",
        "freshness", "update_mode", "transition_capability"}) {
    row = output_row();
    row[field] = JsonValueDTO{std::string("caller-selected")};
    EXPECT_FALSE(
        ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok())
        << field;
  }
  row = output_row();
  provider(row)["profile"] = JsonValueDTO{std::string("mock")};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
}

TEST(IoSignalCatalogTest,
     InputObservationCannotBeGrantedByAnOutputAcknowledgement) {
  const auto graph = io_graph({JsonValueDTO{output_row()}});
  CapabilityProviderBindingDTO binding{
      CapabilityProviderBindingId("io.write"),
      CapabilityProviderBindingRevision("revision.io"),
      CapabilityId("io.gpio.set_output"),
      CapabilityDefinitionRevision("revision.io"),
      CapabilityScope::kTargetBinding,
      ComponentTargetCapabilityProviderTargetDTO{
          ControllableComponentId("device.io"),
          ComponentTypeRevision("revision.io"), CommandTargetId("gpio")},
      ProviderInstanceId("provider.io"),
      ProviderManifestId("provider.io"),
      ProviderManifestRevision("revision.io"),
      {{EndpointRoleId("io_signal_service"),
        CapabilityEndpointSourceKind::kRuntimeUnit, RuntimeUnitId("host.io"),
        EndpointRoleId("ros2_control_execution_status")}}};
  EXPECT_TRUE(
      manyforge::deployment::ValidateNonCoordinateProviderTarget(graph, binding)
          .ok());
  binding.capability_id = CapabilityId("io.gpio.get_input");
  EXPECT_FALSE(
      manyforge::deployment::ValidateNonCoordinateProviderTarget(graph, binding)
          .ok());
  binding.capability_id = CapabilityId("io.gpio.wait_for_input");
  EXPECT_FALSE(
      manyforge::deployment::ValidateNonCoordinateProviderTarget(graph, binding)
          .ok());
}

TEST(IoSignalCatalogTest,
     OutputRuntimeSchemaHasNoInputDefaultsOrCompletionSelector) {
  using namespace manyforge::deployment;
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      SchemaId(std::string(
          detail::kRos2ControlExecutionRuntimeConfigurationSchemaId)),
      SchemaRevision(std::string(
          detail::kRos2ControlExecutionRuntimeConfigurationSchemaRevision)));
  ASSERT_NE(schema, nullptr);
  auto configuration =
      ros2_control_execution_runtime_contract::empty_host_configuration()
          .as_object();
  const auto row =
      parse_strict_json_model_yaml(R"({"controllable_component_id":"device.io",
    "command_target_id":"gpio","resource_role_id":"lamp","signal_name":"lamp",
    "source_id":"source.lamp","value_type":"bool","direction":"output",
    "provider":{"profile":"ros_set_bool","service":"/gpio/set_lamp"}})")
          .root();
  configuration.emplace_back("io_signals",
                             CanonicalValue(CanonicalValue::Array{row}));
  const auto normalized = validate_and_normalize_build_qualified_value(
      *schema, CanonicalValue(configuration));
  const auto &output = normalized.normalized_value()
                           .find_object_member("io_signals")
                           ->as_array()
                           .front();
  EXPECT_EQ(output, row);
  EXPECT_EQ(output.find_object_member("freshness"), nullptr);
  EXPECT_EQ(output.find_object_member("output_completion"), nullptr);
  auto invalid_row = row.as_object();
  invalid_row.emplace_back("output_completion",
                           CanonicalValue("source_acknowledged"));
  configuration.back().second =
      CanonicalValue(CanonicalValue::Array{CanonicalValue(invalid_row)});
  EXPECT_THROW((void)validate_and_normalize_build_qualified_value(
                   *schema, CanonicalValue(configuration)),
               std::exception);
}

TEST(IoSignalCatalogTest,
     RuntimeSchemaRejectsEmptyInputTopicsWithoutChangingInputDefaults) {
  using namespace manyforge::deployment;
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      SchemaId(std::string(
          detail::kRos2ControlExecutionRuntimeConfigurationSchemaId)),
      SchemaRevision(std::string(
          detail::kRos2ControlExecutionRuntimeConfigurationSchemaRevision)));
  ASSERT_NE(schema, nullptr);
  for (const auto *profile : {"ros_bool", "ros2_control_gpio"}) {
    auto configuration =
        ros2_control_execution_runtime_contract::empty_host_configuration()
            .as_object();
    auto row = parse_strict_json_model_yaml(
                   R"({"controllable_component_id":"device.io",
      "command_target_id":"gpio","resource_role_id":"ready","signal_name":"ready",
      "source_id":"source.io","value_type":"bool","direction":"input",
      "update_mode":"sampled","transition_capability":"sampled_only",
      "freshness":{"mode":"sample_age","maximum_age_ns":20},
      "provider":{"profile":"ros_bool","topic":""}})")
                   .root()
                   .as_object();
    for (auto &[field, value] : row) {
      if (field != "provider")
        continue;
      CanonicalValue::Object address{{"profile", CanonicalValue(profile)},
                                     {"topic", CanonicalValue("")}};
      if (std::string_view(profile) == "ros_bool")
        address.emplace_back("liveness",
                             CanonicalValue(CanonicalValue::Object{
                                 {"mode", CanonicalValue("periodic")}}));
      if (std::string_view(profile) == "ros2_control_gpio") {
        address.emplace_back("group", CanonicalValue("gpio"));
        address.emplace_back("interface_name", CanonicalValue("in"));
      }
      value = CanonicalValue(std::move(address));
    }
    configuration.emplace_back(
        "io_signals",
        CanonicalValue(CanonicalValue::Array{CanonicalValue(row)}));
    EXPECT_THROW((void)validate_and_normalize_build_qualified_value(
                     *schema, CanonicalValue(configuration)),
                 std::exception)
        << profile;
  }
}

TEST(IoSignalCatalogTest,
     MockOutputAliasesShareActualChannelWithoutInputAutomationSettings) {
  auto first = output_row();
  provider(first) = {{"profile", JsonValueDTO{std::string("mock_output")}},
                     {"channel", JsonValueDTO{std::string("lamp")}}};
  auto alias = first;
  alias["resource_role_id"] = JsonValueDTO{std::string("alias")};
  alias["signal_name"] = JsonValueDTO{std::string("alias")};
  const auto result = ResolveRetainedIoSignalCatalog(
      io_graph({JsonValueDTO{first}, JsonValueDTO{alias}}));
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().size(), 2U);
  EXPECT_EQ(result.value()[0].descriptor.output_resource,
            result.value()[1].descriptor.output_resource);
  EXPECT_EQ(result.value()[0].provider.channel, "lamp");
  EXPECT_FALSE(result.value()[0].provider.initial_value);
  EXPECT_FALSE(result.value()[0].provider.service);
  for (const auto *field :
       {"initial_value", "auto_answer_delay_ns", "manual_only"}) {
    auto invalid = first;
    provider(invalid)[field] = JsonValueDTO{false};
    EXPECT_FALSE(
        ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{invalid}})).ok())
        << field;
  }
}

Object device_row(double seconds) {
  return {{"controllable_component_id", JsonValueDTO{std::string("device.io")}},
          {"lifecycle_command_timeout_s", JsonValueDTO{seconds}}};
}

TEST(IoSignalCatalogTest,
     DeviceLifecycleLimitMaterializesDefaultAndKeepsExplicitOverride) {
  auto graph = io_graph();
  const auto absent =
      manyforge::deployment::ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(absent.ok());
  EXPECT_TRUE(
      absent.value().empty()); // This raw graph has not crossed normalization.
  ASSERT_TRUE(
      manyforge::deployment::MaterializeRetainedIoProviderDevices(graph).ok());
  const auto defaults =
      manyforge::deployment::ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(defaults.ok());
  ASSERT_EQ(defaults.value().size(), 1U);
  EXPECT_EQ(defaults.value().front().lifecycle_command_timeout_ns,
            10000000000LL);
  const auto frozen = graph.runtime_units.front().configuration;
  ASSERT_TRUE(
      manyforge::deployment::MaterializeRetainedIoProviderDevices(graph).ok());
  EXPECT_EQ(graph.runtime_units.front().configuration, frozen);
  graph.runtime_units.front().configuration["io_provider_devices"] =
      JsonValueDTO{Array{JsonValueDTO{device_row(0.25)}}};
  const auto configured =
      manyforge::deployment::ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(configured.ok()) << configured.error().message;
  ASSERT_EQ(configured.value().size(), 1U);
  EXPECT_EQ(configured.value().front().runtime_unit_id,
            RuntimeUnitId("host.io"));
  EXPECT_EQ(configured.value().front().controllable_component_id,
            ControllableComponentId("device.io"));
  EXPECT_EQ(configured.value().front().lifecycle_command_timeout_ns, 250000000);
  graph.runtime_units.front().configuration["io_provider_devices"] =
      JsonValueDTO{Array{JsonValueDTO{device_row(0.0000000001)}}};
  const auto sub_ns =
      manyforge::deployment::ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(sub_ns.ok());
  EXPECT_EQ(sub_ns.value().front().lifecycle_command_timeout_ns, 1);
}

TEST(IoSignalCatalogTest,
     DeviceLifecycleLimitRejectsInvalidDuplicateUnknownAndUnusedRows) {
  for (const auto seconds :
       {0.0, -1.0, 3600.0001, std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()}) {
    auto graph = io_graph();
    graph.runtime_units.front().configuration["io_provider_devices"] =
        JsonValueDTO{Array{JsonValueDTO{device_row(seconds)}}};
    EXPECT_FALSE(
        manyforge::deployment::ResolveRetainedIoProviderDevices(graph).ok());
  }
  auto graph = io_graph();
  graph.runtime_units.front().configuration["io_provider_devices"] =
      JsonValueDTO{
          Array{JsonValueDTO{device_row(1.0)}, JsonValueDTO{device_row(2.0)}}};
  EXPECT_FALSE(
      manyforge::deployment::ResolveRetainedIoProviderDevices(graph).ok());
  auto unknown = device_row(1.0);
  unknown["controllable_component_id"] = JsonValueDTO{std::string("unknown")};
  graph.runtime_units.front().configuration["io_provider_devices"] =
      JsonValueDTO{Array{JsonValueDTO{unknown}}};
  EXPECT_FALSE(
      manyforge::deployment::ResolveRetainedIoProviderDevices(graph).ok());
  graph = io_graph({});
  graph.runtime_units.front().configuration["io_provider_devices"] =
      JsonValueDTO{Array{JsonValueDTO{device_row(1.0)}}};
  EXPECT_FALSE(
      manyforge::deployment::ResolveRetainedIoProviderDevices(graph).ok());
}

TEST(IoSignalCatalogTest, DeviceDefaultsLeaveOrdinaryConfigurationsInert) {
  using manyforge::deployment::MaterializeRetainedIoProviderDevices;
  auto ordinary = io_graph({});
  const auto original = ordinary.runtime_units.front().configuration;
  ASSERT_TRUE(MaterializeRetainedIoProviderDevices(ordinary).ok());
  EXPECT_EQ(ordinary.runtime_units.front().configuration, original);
}

TEST(IoSignalCatalogTest,
     DeviceDefaultFillsOmittedFieldAndRefusesUnboundDeclarations) {
  using manyforge::deployment::MaterializeRetainedIoProviderDevices;
  auto graph = io_graph();
  graph.runtime_units.front().configuration["io_provider_devices"] =
      JsonValueDTO{Array{
          JsonValueDTO{Object{{"controllable_component_id",
                               JsonValueDTO{std::string("device.io")}}}}}};
  ASSERT_TRUE(MaterializeRetainedIoProviderDevices(graph).ok());
  const auto device =
      manyforge::deployment::ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(device.ok());
  EXPECT_EQ(device.value().front().lifecycle_command_timeout_ns, 10000000000LL);
  auto bad = device_row(1.0);
  bad["controllable_component_id"] = JsonValueDTO{std::string("not_owned")};
  graph.runtime_units.front().configuration["io_provider_devices"] =
      JsonValueDTO{Array{JsonValueDTO{bad}}};
  EXPECT_FALSE(MaterializeRetainedIoProviderDevices(graph).ok());
}

TEST(IoSignalCatalogTest,
     OutputSourceAckBudgetMaterializesOnlyForActualRosOutputs) {
  using namespace manyforge::deployment;
  auto synthetic = output_row("synthetic");
  provider(synthetic) = {{"profile", JsonValueDTO{std::string("mock_output")}},
                         {"channel", JsonValueDTO{std::string("synthetic")}}};
  auto mixed = io_graph({JsonValueDTO{output_row()}, JsonValueDTO{synthetic}});
  ASSERT_TRUE(MaterializeRetainedIoProviderDevices(mixed).ok());
  const auto devices = ResolveRetainedIoProviderDevices(mixed);
  ASSERT_TRUE(devices.ok()) << devices.error().message;
  ASSERT_EQ(devices.value().size(), 1U);
  EXPECT_EQ(devices.value().front().output_source_ack_timeout_ns, 500000000);
  EXPECT_EQ(devices.value().front().lifecycle_command_timeout_ns,
            10000000000LL);
  const auto frozen = mixed.runtime_units.front().configuration;
  ASSERT_TRUE(MaterializeRetainedIoProviderDevices(mixed).ok());
  EXPECT_EQ(mixed.runtime_units.front().configuration, frozen);
  for (auto graph : {io_graph(), io_graph({JsonValueDTO{synthetic}})}) {
    ASSERT_TRUE(MaterializeRetainedIoProviderDevices(graph).ok());
    const auto retained = ResolveRetainedIoProviderDevices(graph);
    ASSERT_TRUE(retained.ok());
    ASSERT_EQ(retained.value().size(), 1U);
    EXPECT_FALSE(retained.value().front().output_source_ack_timeout_ns);
    const auto &rows =
        std::get<Array>(graph.runtime_units.front()
                            .configuration.at("io_provider_devices")
                            .value);
    EXPECT_FALSE(std::get<Object>(rows.front().value)
                     .contains("output_source_ack_timeout_s"));
  }
}

TEST(IoSignalCatalogTest,
     OutputSourceAckBudgetIsSealedAndNeverDefaultedByReadback) {
  using namespace manyforge::deployment;
  auto graph = io_graph({JsonValueDTO{output_row()}});
  graph.runtime_units.front().configuration["io_provider_devices"] =
      JsonValueDTO{Array{JsonValueDTO{device_row(2.0)}}};
  EXPECT_FALSE(ResolveRetainedIoProviderDevices(graph).ok());
  ASSERT_TRUE(MaterializeRetainedIoProviderDevices(graph).ok());
  auto &rows = std::get<Array>(graph.runtime_units.front()
                                   .configuration.at("io_provider_devices")
                                   .value);
  auto &device = std::get<Object>(rows.front().value);
  device["output_source_ack_timeout_s"] = JsonValueDTO{0.0000000001};
  const auto tiny = ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(tiny.ok());
  EXPECT_EQ(tiny.value().front().output_source_ack_timeout_ns, 1);
  device["output_source_ack_timeout_s"] = JsonValueDTO{1.25};
  const auto configured = ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(configured.ok());
  EXPECT_EQ(configured.value().front().output_source_ack_timeout_ns,
            1250000000);
  EXPECT_EQ(configured.value().front().lifecycle_command_timeout_ns,
            2000000000);
  device["output_source_ack_timeout_s"] = JsonValueDTO{3600.0001};
  EXPECT_TRUE(ResolveRetainedIoProviderDevices(graph).ok());
  device["output_source_ack_timeout_s"] = JsonValueDTO{9223372036.0};
  const auto wide = ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(wide.ok());
  EXPECT_EQ(wide.value().front().output_source_ack_timeout_ns,
            9223372036000000000LL);
  for (const double invalid :
       {0.0, -1.0, 9223372037.0, std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()}) {
    device["output_source_ack_timeout_s"] = JsonValueDTO{invalid};
    EXPECT_FALSE(ResolveRetainedIoProviderDevices(graph).ok());
  }
}

TEST(IoSignalCatalogTest,
     OutputSourceAckBudgetCannotConfigureInputOrImmediateMockDevices) {
  using namespace manyforge::deployment;
  auto synthetic = output_row();
  provider(synthetic) = {{"profile", JsonValueDTO{std::string("mock_output")}},
                         {"channel", JsonValueDTO{std::string("lamp")}}};
  for (auto graph : {io_graph(), io_graph({JsonValueDTO{synthetic}})}) {
    auto device = device_row(30.0);
    device["output_source_ack_timeout_s"] = JsonValueDTO{0.5};
    graph.runtime_units.front().configuration["io_provider_devices"] =
        JsonValueDTO{Array{JsonValueDTO{device}}};
    EXPECT_FALSE(MaterializeRetainedIoProviderDevices(graph).ok());
    EXPECT_FALSE(ResolveRetainedIoProviderDevices(graph).ok());
  }
}

Object cyclic_input(std::string profile) {
  auto row = input_row();
  row["update_mode"] = JsonValueDTO{std::string("sampled")};
  row["freshness"] =
      JsonValueDTO{Object{{"mode", JsonValueDTO{std::string("sample_age")}},
                          {"maximum_age_ns", JsonValueDTO{20.0}}}};
  provider(row) = {{"profile", JsonValueDTO{profile}},
                   {"topic", JsonValueDTO{std::string("/device/io")}},
                   {"ingress_depth", JsonValueDTO{4.0}}};
  if (profile == "ros_bool")
    provider(row)["liveness"] =
        JsonValueDTO{Object{{"mode", JsonValueDTO{std::string("periodic")}}}};
  else if (profile == kAdapterProfile)
    provider(row)["pin"] = JsonValueDTO{17.0};
  else {
    provider(row)["group"] = JsonValueDTO{std::string("module/1")};
    provider(row)["interface_name"] =
        JsonValueDTO{std::string("digital.0/value")};
  }
  return row;
}

Object geometric_contact_input(std::string name = "contact",
                               std::string source = "source.contact",
                               double maximum_age_ns = 125.0) {
  auto row = input_row(std::move(name), std::move(source));
  row["update_mode"] = JsonValueDTO{std::string("sampled")};
  row["freshness"] =
      JsonValueDTO{Object{{"mode", JsonValueDTO{std::string("sample_age")}},
                          {"maximum_age_ns",
                           JsonValueDTO{maximum_age_ns}}}};
  provider(row) = {
      {"profile", JsonValueDTO{std::string("geometric_contact")}},
      {"ingress_depth", JsonValueDTO{4.0}},
      {"robot_component_id", JsonValueDTO{std::string("robot.arm")}},
      {"target_scene_resource_id",
       JsonValueDTO{std::string("fixture.target")}}};
  return row;
}

TEST(IoSignalCatalogTest,
     GeometricContactUsesIndependentStandardAgeAndSourceScopedSelectors) {
  auto first = geometric_contact_input();
  auto second = geometric_contact_input("contact_copy");
  auto result = ResolveRetainedIoSignalCatalog(
      io_graph({JsonValueDTO{first}, JsonValueDTO{second}}));
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().size(), 2U);
  for (const auto &signal : result.value()) {
    EXPECT_EQ(signal.descriptor.liveness, IoSignalLivenessMode::kPeriodic);
    EXPECT_EQ(signal.descriptor.update_mode, IoSignalUpdateMode::kSampled);
    EXPECT_EQ(signal.descriptor.freshness.mode,
              IoFreshnessMode::kSampleAge);
    EXPECT_EQ(signal.descriptor.freshness.maximum_age_ns, 125);
    EXPECT_EQ(signal.provider.robot_component_id,
              ControllableComponentId("robot.arm"));
    EXPECT_EQ(signal.provider.target_scene_resource_id,
              SceneResourceId("fixture.target"));
  }

  first = geometric_contact_input("contact", "source.contact", 17.0);
  second = geometric_contact_input("contact_copy", "source.contact", 42.0);
  result = ResolveRetainedIoSignalCatalog(
      io_graph({JsonValueDTO{first}, JsonValueDTO{second}}));
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().size(), 2U);
  EXPECT_EQ(result.value()[0].descriptor.freshness.maximum_age_ns, 17);
  EXPECT_EQ(result.value()[1].descriptor.freshness.maximum_age_ns, 42);
  const auto first_digest =
      ComputeIoSignalDescriptorDigest(result.value()[0].descriptor);
  const auto second_digest =
      ComputeIoSignalDescriptorDigest(result.value()[1].descriptor);
  ASSERT_TRUE(first_digest.ok());
  ASSERT_TRUE(second_digest.ok());
  EXPECT_NE(first_digest.value(), second_digest.value());

  // Body roles are derived by the provider; the input contract has no body
  // list or timing controls to configure.
  for (const auto *field : {"body_ids", "body_role_ids", "sample_period_ns",
                            "minimum_rate_hz", "timeout_ns"}) {
    auto malformed = geometric_contact_input();
    provider(malformed)[field] = JsonValueDTO{std::string("unapproved")};
    EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                     io_graph({JsonValueDTO{malformed}}))
                     .ok())
        << field;
  }

  auto malformed = geometric_contact_input();
  provider(malformed).erase("robot_component_id");
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                   io_graph({JsonValueDTO{malformed}}))
                   .ok());
  malformed = geometric_contact_input();
  provider(malformed)["target_scene_resource_id"] =
      JsonValueDTO{std::string("")};
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                   io_graph({JsonValueDTO{malformed}}))
                   .ok());
  malformed = geometric_contact_input();
  provider(malformed)["endpoint"] = JsonValueDTO{Object{
      {"controllable_component_id", JsonValueDTO{std::string("robot.arm")}},
      {"command_target_id", JsonValueDTO{std::string("arm")}},
      {"frame_role_id", JsonValueDTO{std::string("tool")}}}};
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                   io_graph({JsonValueDTO{malformed}}))
                   .ok());

  // The former fixed-endpoint profile is refused even without a new field.
  provider(malformed).erase("robot_component_id");
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                   io_graph({JsonValueDTO{malformed}}))
                   .ok());

  for (const double age : {0.0, -1.0}) {
    malformed = geometric_contact_input("contact", "source.contact", age);
    EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                     io_graph({JsonValueDTO{malformed}}))
                     .ok())
        << age;
  }

  auto endpoint_conflict = geometric_contact_input("contact_copy");
  provider(endpoint_conflict)["robot_component_id"] =
      JsonValueDTO{std::string("other_robot")};
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                   io_graph({JsonValueDTO{geometric_contact_input()},
                             JsonValueDTO{endpoint_conflict}}))
                   .ok());

  auto target_conflict = geometric_contact_input("contact_copy");
  provider(target_conflict)["target_scene_resource_id"] =
      JsonValueDTO{std::string("other.fixture")};
  EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                   io_graph({JsonValueDTO{geometric_contact_input()},
                             JsonValueDTO{target_conflict}}))
                   .ok());
}

Object typed_output(std::string profile, std::string name = "lamp") {
  auto row = output_row(std::move(name));
  if (profile == kAdapterProfile)
    provider(row) = {{"profile", JsonValueDTO{profile}},
                     {"service", JsonValueDTO{std::string("/device/set_io")}},
                     {"pin", JsonValueDTO{17.0}}};
  else
    provider(row) = {
        {"profile", JsonValueDTO{profile}},
        {"topic", JsonValueDTO{std::string("/device/commands")}},
        {"group", JsonValueDTO{std::string("module/1")}},
        {"interface_name", JsonValueDTO{std::string("digital.0/value")}}};
  return row;
}

TEST(IoSignalCatalogTest,
     SourceKindLivenessIsExplicitAndUsesOneExistingAgeBudget) {
  const auto mock = ResolveRetainedIoSignalCatalog(io_graph());
  ASSERT_TRUE(mock.ok());
  EXPECT_EQ(mock.value().front().descriptor.liveness,
            IoSignalLivenessMode::kInProcess);
  for (const auto *profile :
       {"ros_bool", kAdapterProfile, "ros2_control_gpio"}) {
    const auto result = ResolveRetainedIoSignalCatalog(
        io_graph({JsonValueDTO{cyclic_input(profile)}}));
    ASSERT_TRUE(result.ok()) << profile;
    const auto &signal = result.value().front();
    EXPECT_EQ(signal.descriptor.liveness, IoSignalLivenessMode::kPeriodic);
    EXPECT_EQ(signal.descriptor.freshness.maximum_age_ns, 20);
    if (std::string_view(profile) == kAdapterProfile) {
      EXPECT_EQ(signal.provider.adapter_fields.at("pin"), JsonValueDTO{17.0});
    }
    if (std::string_view(profile) == "ros2_control_gpio") {
      EXPECT_EQ(signal.provider.group, "module/1");
      EXPECT_EQ(signal.provider.interface_name, "digital.0/value");
    }
  }
  auto row = cyclic_input("ros_bool");
  provider(row)["liveness"] =
      JsonValueDTO{Object{{"mode", JsonValueDTO{std::string("change_only")}}}};
  row["freshness"] = JsonValueDTO{
      Object{{"mode", JsonValueDTO{std::string("provider_validity")}}}};
  const auto change_only =
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}}));
  ASSERT_TRUE(change_only.ok());
  EXPECT_EQ(change_only.value().front().descriptor.liveness,
            IoSignalLivenessMode::kChangeOnly);
  EXPECT_FALSE(change_only.value().front().descriptor.freshness.maximum_age_ns);
  auto descriptor = change_only.value().front().descriptor;
  descriptor.liveness.reset();
  EXPECT_FALSE(ComputeIoSignalDescriptorDigest(descriptor).ok());
  descriptor = change_only.value().front().descriptor;
  descriptor.liveness = IoSignalLivenessMode::kInProcess;
  EXPECT_NE(
      ComputeIoSignalDescriptorDigest(descriptor).value(),
      ComputeIoSignalDescriptorDigest(change_only.value().front().descriptor)
          .value());
}

TEST(IoSignalCatalogTest,
     ExistingSourcesAllowSelectorAliasesButKeepProviderAndDepthCoherent) {
  for (const auto *profile : {kAdapterProfile, "ros2_control_gpio"}) {
    auto first = cyclic_input(profile);
    auto second = cyclic_input(profile);
    second["resource_role_id"] = JsonValueDTO{std::string("other")};
    second["signal_name"] = JsonValueDTO{std::string("other")};
    second["source_id"] = JsonValueDTO{std::string("source.io")};
    if (std::string_view(profile) == kAdapterProfile) {
      provider(second)["pin"] = JsonValueDTO{16.0};
    } else {
      provider(second)["group"] = JsonValueDTO{std::string("module/2")};
      provider(second)["interface_name"] =
          JsonValueDTO{std::string("digital.1/value")};
    }
    auto result = ResolveRetainedIoSignalCatalog(
        io_graph({JsonValueDTO{first}, JsonValueDTO{second}}));
    ASSERT_TRUE(result.ok()) << profile << ": " << result.error().message;
    ASSERT_EQ(result.value().size(), 2U);
    EXPECT_EQ(result.value()[0].descriptor.source_id,
              result.value()[1].descriptor.source_id);

    provider(second)["ingress_depth"] = JsonValueDTO{8.0};
    EXPECT_FALSE(ResolveRetainedIoSignalCatalog(
                     io_graph({JsonValueDTO{first}, JsonValueDTO{second}}))
                     .ok())
        << profile;
  }
}

TEST(IoSignalCatalogTest,
     WrongLivenessBudgetSelectorsAndUnapprovedFieldsRefuse) {
  auto row = cyclic_input("ros_bool");
  provider(row).erase("liveness");
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  row = cyclic_input("ros_bool");
  auto &liveness = std::get<Object>(provider(row).at("liveness").value);
  liveness["min_rate_hz"] = JsonValueDTO{10.0};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  row = cyclic_input("ros_bool");
  provider(row)["liveness"] =
      JsonValueDTO{Object{{"mode", JsonValueDTO{std::string("change_only")}}}};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  row = cyclic_input("ros2_control_gpio");
  provider(row)["interface_name"] = JsonValueDTO{std::string("")};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  row = cyclic_input("ros_bool");
  row["freshness"] = JsonValueDTO{
      Object{{"mode", JsonValueDTO{std::string("provider_validity")}}}};
  EXPECT_FALSE(
      ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
}

TEST(IoSignalCatalogTest,
     TypedOutputCanonicalResourcesBindEveryActualChannelSelector) {
  for (const auto *profile : {kAdapterProfile, "ros2_control_gpio"}) {
    auto row = typed_output(profile);
    auto alias = typed_output(profile, "alias");
    const auto result = ResolveRetainedIoSignalCatalog(
        io_graph({JsonValueDTO{row}, JsonValueDTO{alias}}));
    ASSERT_TRUE(result.ok()) << profile;
    ASSERT_EQ(result.value().size(), 2U);
    EXPECT_EQ(result.value()[0].descriptor.output_resource,
              result.value()[1].descriptor.output_resource);
    EXPECT_FALSE(result.value().front().descriptor.liveness);
    if (std::string_view(profile) == kAdapterProfile)
      provider(alias)["pin"] = JsonValueDTO{16.0};
    else
      provider(alias)["interface_name"] = JsonValueDTO{std::string("other")};
    const auto other =
        ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{alias}}));
    ASSERT_TRUE(other.ok());
    EXPECT_NE(result.value().front().descriptor.output_resource,
              other.value().front().descriptor.output_resource);
    auto descriptor = other.value().front().descriptor;
    descriptor.liveness = IoSignalLivenessMode::kPeriodic;
    EXPECT_FALSE(ComputeIoSignalDescriptorDigest(descriptor).ok());
    provider(row)["ingress_depth"] = JsonValueDTO{4.0};
    EXPECT_FALSE(
        ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{row}})).ok());
  }
}

TEST(IoSignalCatalogTest,
     AdapterSourceAcknowledgementUsesDevicePolicyAndGpioHasNoAckBudget) {
  using namespace manyforge::deployment;
  auto graph = io_graph({JsonValueDTO{typed_output(kAdapterProfile)}});
  ASSERT_TRUE(MaterializeRetainedIoProviderDevices(graph).ok());
  auto devices = ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(devices.ok());
  ASSERT_EQ(devices.value().size(), 1U);
  EXPECT_EQ(devices.value().front().output_source_ack_timeout_ns, 500000000);
  auto &device = std::get<Object>(
      std::get<Array>(graph.runtime_units.front()
                          .configuration.at("io_provider_devices")
                          .value)
          .front()
          .value);
  device["output_source_ack_timeout_s"] = JsonValueDTO{0.75};
  devices = ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(devices.ok());
  EXPECT_EQ(devices.value().front().output_source_ack_timeout_ns, 750000000);
  auto gpio = io_graph({JsonValueDTO{typed_output("ros2_control_gpio")}});
  ASSERT_TRUE(MaterializeRetainedIoProviderDevices(gpio).ok());
  const auto gpio_devices = ResolveRetainedIoProviderDevices(gpio);
  ASSERT_TRUE(gpio_devices.ok());
  EXPECT_FALSE(gpio_devices.value().front().output_source_ack_timeout_ns);
  auto &gpio_device = std::get<Object>(
      std::get<Array>(gpio.runtime_units.front()
                          .configuration.at("io_provider_devices")
                          .value)
          .front()
          .value);
  gpio_device["output_source_ack_timeout_s"] = JsonValueDTO{0.5};
  EXPECT_FALSE(MaterializeRetainedIoProviderDevices(gpio).ok());
  EXPECT_FALSE(ResolveRetainedIoProviderDevices(gpio).ok());
}

TEST(IoSignalCatalogTest,
     RuntimeSchemaKeepsTypedSelectorsClosedAndDeclaresNoRateDefault) {
  using namespace manyforge::deployment;
  const auto &registry = build_qualified_closed_schema_registry();
  const auto *schema = registry.find_qualified_value_schema(
      SchemaId(std::string(
          detail::kRos2ControlExecutionRuntimeConfigurationSchemaId)),
      SchemaRevision(std::string(
          detail::kRos2ControlExecutionRuntimeConfigurationSchemaRevision)));
  ASSERT_NE(schema, nullptr);
  auto configuration =
      ros2_control_execution_runtime_contract::empty_host_configuration()
          .as_object();
  const auto signals = parse_strict_json_model_yaml(R"([
    {"controllable_component_id":"device.io","command_target_id":"gpio",
     "resource_role_id":"ready","signal_name":"ready","source_id":"source.input",
     "value_type":"bool","direction":"input","update_mode":"sampled",
     "transition_capability":"sampled_only","freshness":{"mode":"sample_age","maximum_age_ns":20},
     "provider":{"profile":"component_adapter","bank":"tool"}},
    {"controllable_component_id":"device.io","command_target_id":"gpio",
     "resource_role_id":"lamp","signal_name":"lamp","source_id":"source.output",
     "value_type":"bool","direction":"output",
     "provider":{"profile":"ros2_control_gpio","topic":"/device/commands",
                 "group":"module/1","interface_name":"digital.0/value"}},
    {"controllable_component_id":"device.io","command_target_id":"arm",
     "resource_role_id":"contact","signal_name":"contact","source_id":"source.contact",
     "value_type":"bool","direction":"input","update_mode":"sampled",
     "transition_capability":"sampled_only",
     "freshness":{"mode":"sample_age","maximum_age_ns":125},
     "provider":{"profile":"geometric_contact","ingress_depth":4,
       "robot_component_id":"robot.arm",
       "target_scene_resource_id":"fixture.target"}}
  ])")
                           .root();
  configuration.emplace_back("io_signals", signals);
  const auto normalized = validate_and_normalize_build_qualified_value(
      *schema, CanonicalValue(configuration));
  const auto &rows = normalized.normalized_value()
                         .find_object_member("io_signals")
                         ->as_array();
  ASSERT_EQ(rows.size(), 3U);
  const auto find_profile_index = [](const auto &items,
                                     std::string_view profile) {
    for (std::size_t index = 0; index < items.size(); ++index) {
      const auto *address = items[index].find_object_member("provider");
      const auto *profile_value =
          address == nullptr ? nullptr : address->find_object_member("profile");
      if (profile_value != nullptr && profile_value->as_string() == profile)
        return index;
    }
    return items.size();
  };
  bool input_seen = false;
  for (const auto &row : rows) {
    const auto &address = *row.find_object_member("provider");
    EXPECT_EQ(address.find_object_member("min_rate_hz"), nullptr);
    if (address.find_object_member("profile")->as_string() ==
        "component_adapter") {
      input_seen = true;
      EXPECT_EQ(address.find_object_member("bank")->as_string(), "tool");
      EXPECT_EQ(address.find_object_member("ingress_depth")->as_number(), 16.0);
      EXPECT_EQ(address.find_object_member("liveness"), nullptr);
    }
  }
  EXPECT_TRUE(input_seen);
  const auto geometric_index = find_profile_index(rows, "geometric_contact");
  ASSERT_LT(geometric_index, rows.size());
  const auto &geometric_provider =
      *rows[geometric_index].find_object_member("provider");
  EXPECT_EQ(geometric_provider.find_object_member("profile")->as_string(),
            "geometric_contact");
  EXPECT_EQ(geometric_provider.find_object_member("robot_component_id")
                ->as_string(),
            "robot.arm");
  auto invalid_signals = signals.as_array();
  auto invalid = invalid_signals.front().as_object();
  for (auto &[key, field] : invalid) {
    if (key != "provider")
      continue;
    auto address = field.as_object();
    address.emplace_back("function", CanonicalValue("analog"));
    field = CanonicalValue(std::move(address));
  }
  invalid_signals.front() = CanonicalValue(std::move(invalid));
  auto invalid_configuration = configuration;
  for (auto &[key, field] : invalid_configuration)
    if (key == "io_signals")
      field = CanonicalValue(std::move(invalid_signals));
  EXPECT_THROW((void)validate_and_normalize_build_qualified_value(
                   *schema, CanonicalValue(invalid_configuration)),
               std::exception);
  invalid_signals = signals.as_array();
  const auto invalid_geometric_index =
      find_profile_index(invalid_signals, "geometric_contact");
  ASSERT_LT(invalid_geometric_index, invalid_signals.size());
  auto invalid_geometric = invalid_signals[invalid_geometric_index].as_object();
  for (auto &[key, field] : invalid_geometric) {
    if (key != "provider")
      continue;
    auto address = field.as_object();
    address.emplace_back("body_ids", CanonicalValue(CanonicalValue::Array{}));
    field = CanonicalValue(std::move(address));
  }
  invalid_signals[invalid_geometric_index] =
      CanonicalValue(std::move(invalid_geometric));
  invalid_configuration = configuration;
  for (auto &[key, field] : invalid_configuration)
    if (key == "io_signals")
      field = CanonicalValue(std::move(invalid_signals));
  EXPECT_THROW((void)validate_and_normalize_build_qualified_value(
                   *schema, CanonicalValue(invalid_configuration)),
               std::exception);
}

TEST(IoSignalCatalogTest, PoseStreamIdsAreObjectLinksOrOverlays) {
  using manyforge::deployment::PoseSourceStreamId;
  EXPECT_EQ(PoseSourceStreamId("cheese_box", "scene"), "poses/cheese_box");
  EXPECT_EQ(PoseSourceStreamId("robot", "viewer.measured_tcp"),
            "poses/robot/viewer.measured_tcp");
}

// A machine's I/O bank on the real lane (the rows the
// compiler writes for an I/O member): periodic inputs with a sample-age
// budget, outputs with read-back, one channel per output, no
// source-acknowledgement budget.
TEST(IoSignalCatalogTest, MachineIoBankRowsReadBackWithOneChannelPerOutput) {
  using namespace manyforge::deployment;
  auto input = input_row("CI0", "robot_controller_io.CI0");
  input["controllable_component_id"] = JsonValueDTO{std::string("device.io")};
  input["update_mode"] = JsonValueDTO{std::string("sampled")};
  input["freshness"] = JsonValueDTO{Object{
      {"mode", JsonValueDTO{std::string("sample_age")}},
      {"maximum_age_ns", JsonValueDTO{100'000'000.0}}}};
  provider(input) = {{"profile", JsonValueDTO{std::string("component_adapter")}},
                     {"bank", JsonValueDTO{std::string("controller")}},
                     {"ingress_depth", JsonValueDTO{16.0}}};
  // The stream is periodic: an input without a sample-age budget is refused.
  {
    auto unbudgeted = input;
    unbudgeted["freshness"] = JsonValueDTO{
        Object{{"mode", JsonValueDTO{std::string("provider_validity")}}}};
    const auto refused =
        ResolveRetainedIoSignalCatalog(io_graph({JsonValueDTO{unbudgeted}}));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"),
              "io_signal_liveness_freshness_mismatch");
  }
  const auto output = [&](std::string name) {
    auto row = output_row(name);
    row["source_id"] = JsonValueDTO{"robot_controller_io." + name};
    provider(row) = {{"profile", JsonValueDTO{std::string("component_adapter")}},
                     {"bank", JsonValueDTO{std::string("controller")}}};
    return row;
  };
  auto graph = io_graph({JsonValueDTO{input}, JsonValueDTO{output("CO0")},
                         JsonValueDTO{output("CO1")}});
  ASSERT_TRUE(MaterializeRetainedIoProviderDevices(graph).ok());
  const auto signals = ResolveRetainedIoSignalCatalog(graph);
  ASSERT_TRUE(signals.ok()) << signals.error().message;
  ASSERT_EQ(signals.value().size(), 3U);
  std::set<std::string> resources;
  for (const auto &row : signals.value()) {
    EXPECT_EQ(row.provider.adapter_fields.at("bank"),
              JsonValueDTO{std::string("controller")});
    if (row.descriptor.direction == IoSignalDirection::kInput) {
      EXPECT_EQ(row.descriptor.liveness, IoSignalLivenessMode::kPeriodic);
      EXPECT_EQ(row.descriptor.freshness.maximum_age_ns,
                std::optional<Duration>(100'000'000));
      EXPECT_FALSE(row.descriptor.readback);
    } else {
      EXPECT_TRUE(row.descriptor.readback);
      EXPECT_TRUE(HoldsChangedOutside(row.descriptor));
      ASSERT_TRUE(row.descriptor.output_resource.has_value());
      resources.insert(row.descriptor.output_resource->value());
    }
  }
  EXPECT_EQ(resources.size(), 2U);
  const auto devices = ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(devices.ok());
  ASSERT_EQ(devices.value().size(), 1U);
  EXPECT_FALSE(devices.value().front().output_source_ack_timeout_ns);
}

// An I/O member's device row names its owner and role.
TEST(IoSignalCatalogTest, MemberDeviceRowsCarryOwnerAndRole) {
  using namespace manyforge::deployment;
  auto graph = io_graph();
  graph.runtime_units.front().configuration["io_provider_devices"] =
      JsonValueDTO{Array{JsonValueDTO{Object{
          {"controllable_component_id", JsonValueDTO{std::string("device.io")}},
          {"owner_component_id", JsonValueDTO{std::string("lite6")}},
          {"member_role", JsonValueDTO{std::string("controller_io")}}}}}};
  ASSERT_TRUE(MaterializeRetainedIoProviderDevices(graph).ok());
  const auto devices = ResolveRetainedIoProviderDevices(graph);
  ASSERT_TRUE(devices.ok()) << devices.error().message;
  ASSERT_EQ(devices.value().size(), 1U);
  EXPECT_EQ(devices.value().front().owner_component_id, "lite6");
  EXPECT_EQ(devices.value().front().member_role, "controller_io");
}

} // namespace
