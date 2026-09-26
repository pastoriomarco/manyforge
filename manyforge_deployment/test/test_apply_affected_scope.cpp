#include "apply_affected_scope_internal.hpp"
#include "native_execution_runtime_unit_internal.hpp"

#include <gtest/gtest.h>

#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/native_domain_topology.hpp>

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment::detail {
namespace {

namespace core = manyforge::core;

[[nodiscard]] core::RetainedProductDTO
Product(std::string id, const core::RetainedProductKind kind) {
  return {
      core::ProductId(std::move(id)),
      kind,
      core::ProductContractId("contract"),
      core::ProductContractRevision("contract-r1"),
      {},
  };
}

[[nodiscard]] core::ControllableComponentDTO
Component(std::string id, std::vector<core::RuntimeUnitId> runtime_units = {}) {
  return {
      core::ControllableComponentId(std::move(id)),
      core::ComponentTypeId("component-type"),
      core::ComponentTypeRevision("component-type-r1"),
      {},
      std::move(runtime_units),
      std::nullopt,
  };
}

[[nodiscard]] core::RuntimeUnitDTO
RuntimeUnit(std::string id,
            std::vector<core::RuntimeUnitId> dependencies = {}) {
  return {
      core::RuntimeUnitId(std::move(id)),
      core::RuntimeProfileId("profile"),
      core::RuntimeProfileRevision("profile-r1"),
      std::move(dependencies),
      {},
  };
}

[[nodiscard]] core::ControlDomainDTO
Domain(std::string id, std::vector<core::RuntimeUnitId> runtime_units = {}) {
  return {
      core::ControlDomainId(std::move(id)),
      core::ControlDomainRevision("domain-r1"),
      std::move(runtime_units),
      core::EndpointRoleId("coordinator"),
      core::ControllerTopologyId("topology"),
      core::TopologyRevision("topology-r1"),
      core::ContainmentGranularity::kResource,
  };
}

[[nodiscard]] core::ControlBindingDTO
Binding(std::string id, std::string revision, std::string component,
        std::string domain, std::vector<std::string> resources) {
  std::vector<core::ResourceBindingDTO> resource_bindings;
  for (std::size_t index = 0U; index < resources.size(); ++index) {
    resource_bindings.push_back({
        core::ResourceRoleId("role-" + std::to_string(index)),
        core::ResourceId(std::move(resources[index])),
    });
  }
  return {
      core::ControlBindingId(std::move(id)),
      core::ControlBindingRevision(std::move(revision)),
      core::ControllableComponentId(std::move(component)),
      core::CommandTargetId("target"),
      core::CapabilityId("capability"),
      core::CapabilityDefinitionRevision("capability-r1"),
      core::ControlDomainId(std::move(domain)),
      std::move(resource_bindings),
      core::ControlInterfaceContractId("interface"),
      core::ControlInterfaceRevision("interface-r1"),
      core::ControllerTopologyId("topology"),
      core::TopologyRevision("topology-r1"),
      {core::ControlRouteId("route")},
      {},
      core::ProviderInstanceId("provider"),
      core::RuntimeUnitId("runtime"),
      core::ProviderManifestId("manifest"),
      core::ProviderManifestRevision("manifest-r1"),
      core::StateAuthorityId("state-authority"),
      core::StateAuthorityClass::kMeasured,
      core::StopEvidencePolicyId("stop-policy"),
      core::StopEvidencePolicyRevision("stop-policy-r1"),
      core::ContainmentGranularity::kResource,
      {},
  };
}

[[nodiscard]] core::RetainedBundleDTO
Bundle(core::CellGraphDTO graph, core::RetainedCompiledProductsDTO products) {
  return {
      "manyforge.retained_bundle.v1",
      core::RetainedBundleId("bundle"),
      core::DeploymentArtifactId("deployment"),
      core::DeploymentRevision("deployment-r1"),
      std::move(graph),
      core::RetainedCatalogDTO{
          "catalog-dialect",
          core::ContentDigest("bootstrap-digest"),
          core::ContentDigest("registry-digest"),
          {},
          {},
          core::RetainedNodeEnvironmentDTO{
              "catalog", "v1", core::ContentDigest("catalog-digest"),
              core::ContentDigest("environment-digest")},
      },
      {},
      {},
      {},
      std::move(products),
      core::RetainedBundleProvenanceDTO{
          core::ContentDigest("compiler-contract"),
          core::ContentDigest("compiler-build"),
          {},
          {},
          {},
      },
      std::nullopt,
      {},
      std::nullopt,
      {},
  };
}

[[nodiscard]] core::CellGraphDTO
Graph(std::vector<core::RuntimeUnitDTO> runtime_units,
      std::vector<core::ControllableComponentDTO> components,
      std::vector<core::ControlDomainDTO> domains,
      std::vector<core::ControlBindingDTO> bindings) {
  return {
      "manyforge.cell_graph.v1",
      core::GraphRevision("graph-r1"),
      core::CellId("cell"),
      std::move(runtime_units),
      {},
      std::move(components),
      std::move(domains),
      std::move(bindings),
      {},
  };
}

[[nodiscard]] core::RetainedCompiledProductsDTO
Products(std::string planning_id = "planning",
         std::optional<std::string> viewer_id = std::nullopt) {
  return {
      Product(std::move(planning_id),
              core::RetainedProductKind::kPlanningCollisionModel),
      viewer_id.has_value()
          ? std::optional(Product(std::move(*viewer_id),
                                  core::RetainedProductKind::kViewerModel))
          : std::nullopt,
      {},
      {},
      {},
  };
}

[[nodiscard]] core::RuntimeUnitId NativeHostId() {
  return core::RuntimeUnitId(
      std::string(native_execution_runtime_contract::kCellHostRuntimeUnitId));
}

[[nodiscard]] core::JsonValueDTO MotionLimit(const std::string &component,
                                             const std::string &target,
                                             const double maximum = 0.5) {
  return {core::JsonObjectDTO{
      {"controllable_component_id", core::JsonValueDTO{component}},
      {"command_target_id", core::JsonValueDTO{target}},
      {"maximum_tcp_linear_speed_mps", core::JsonValueDTO{maximum}},
      {"ordinary_motion_jerk",
       core::JsonValueDTO{core::JsonObjectDTO{
           {"enabled", core::JsonValueDTO{false}},
           {"preset_reference_time_s", core::JsonValueDTO{1.0}},
           {"joint_limits", core::JsonValueDTO{core::JsonValueDTO::Array{}}},
       }}},
  }};
}

[[nodiscard]] core::CellGraphDTO NativeGraph(const bool include_b = true) {
  auto runtime = RuntimeUnit(NativeHostId().value());
  runtime.runtime_profile_id =
      core::RuntimeProfileId(std::string(kNativeExecutionRuntimeProfileId));
  runtime.runtime_profile_revision =
      core::RuntimeProfileRevision(native_execution_runtime_profile_revision());
  auto positions = core::JsonValueDTO::Array{core::JsonValueDTO{0.1},
                                             core::JsonValueDTO{0.2}};
  auto limits =
      core::JsonValueDTO::Array{MotionLimit("component-a", "target-a")};
  if (include_b) {
    positions.push_back(core::JsonValueDTO{0.3});
    limits.push_back(MotionLimit("component-b", "target-b"));
  }
  runtime.configuration = {
      {"mode", core::JsonValueDTO{std::string("simulated")}},
      {"planner_rng_seed", core::JsonValueDTO{1.0}},
      {"execution_tick_hz", core::JsonValueDTO{100.0}},
      {"collision_monitor_period_ms", core::JsonValueDTO{10.0}},
      {"initial_joint_positions_rad", core::JsonValueDTO{std::move(positions)}},
      {"motion_target_limits", core::JsonValueDTO{std::move(limits)}},
      {"direct_control_joint_limits",
       core::JsonValueDTO{core::JsonValueDTO::Array{}}},
      {"gripper_target_configurations",
       core::JsonValueDTO{core::JsonValueDTO::Array{}}},
  };
  const auto motion_binding = [](const std::string &suffix,
                                 std::vector<std::string> resources) {
    auto binding = Binding("binding-" + suffix, "binding-" + suffix + "-r1",
                           "component-" + suffix, "domain-" + suffix,
                           std::move(resources));
    binding.command_target_id = core::CommandTargetId("target-" + suffix);
    binding.capability_id = core::CapabilityId("execution.trajectory.execute");
    binding.provider_runtime_unit_id = NativeHostId();
    binding.provider_manifest_id =
        core::ProviderManifestId(std::string(kNativeControlProviderManifestId));
    return binding;
  };
  auto components = std::vector{Component("component-a", {NativeHostId()})};
  auto domains = std::vector{Domain("domain-a", {NativeHostId()})};
  auto bindings =
      std::vector{motion_binding("a", {"resource-a-1", "resource-a-2"})};
  if (include_b) {
    components.push_back(Component("component-b", {NativeHostId()}));
    domains.push_back(Domain("domain-b", {NativeHostId()}));
    bindings.push_back(motion_binding("b", {"resource-b"}));
  }
  return Graph({std::move(runtime)}, std::move(components), std::move(domains),
               std::move(bindings));
}

[[nodiscard]] core::CellGraphDTO NativeGraphWithDependentGenericRuntime() {
  auto graph = NativeGraph(false);
  const auto dependent_runtime = core::RuntimeUnitId("runtime");
  // Exercise the verified-values algorithm contract: the reference generic
  // runtime profile does not currently admit this dependency in production.
  graph.runtime_units.push_back(
      RuntimeUnit(dependent_runtime.value(), {NativeHostId()}));
  graph.controllable_components.push_back(
      Component("component-b", {dependent_runtime}));
  graph.control_domains.push_back(Domain("domain-b"));
  graph.control_bindings.push_back(
      Binding("binding-b", "binding-b-r1", "component-b", "domain-b",
              {"resource-b"}));
  return graph;
}

[[nodiscard]] core::RetainedCompiledProductsDTO NativeProducts() {
  auto products = Products();
  auto runtime_product =
      Product(native_execution_runtime_product_id(NativeHostId()),
              core::RetainedProductKind::kRuntimeUnitConfiguration);
  runtime_product.product_contract_id = core::ProductContractId(
      std::string(kNativeExecutionRuntimeProductContractId));
  products.runtime_unit_products = {
      {NativeHostId(), {std::move(runtime_product)}}};
  return products;
}

[[nodiscard]] core::RetainedRuntimeContractRequirementDTO
RuntimeRequirement(const core::RuntimeUnitId &runtime_unit_id) {
  return {
      runtime_unit_id,
      {core::RuntimeContractRequirementDigest("sha256:" +
                                              std::string(64U, 'a')),
       core::RuntimeImplementationFamilyId(
           std::string(native_execution_runtime_contract::kImplementationId)),
       {core::ApiContractId(std::string(
           native_execution_runtime_contract::kRuntimeRoleApiContractId))},
       {{core::CapabilityId("execution.trajectory.execute"),
         core::CapabilityDefinitionRevision("capability-r1")}}},
  };
}

void ExpectOnlyComponentB(const core::ConfigurationFenceScopeDTO &scope) {
  ASSERT_TRUE(scope.valid());
  EXPECT_EQ(scope.controllable_component_ids,
            (std::vector{core::ControllableComponentId("component-b")}));
  EXPECT_EQ(scope.resources, (std::vector{core::ResourceId("resource-b")}));
  ASSERT_EQ(scope.bindings.size(), 1U);
  EXPECT_EQ(scope.bindings.front().control_binding_id,
            core::ControlBindingId("binding-b"));
  ASSERT_EQ(scope.domains.size(), 1U);
  EXPECT_EQ(scope.domains.front().control_domain_id,
            core::ControlDomainId("domain-b"));
}

void ExpectBothComponents(const core::ConfigurationFenceScopeDTO &scope) {
  ASSERT_TRUE(scope.valid());
  EXPECT_EQ(scope.controllable_component_ids,
            (std::vector{core::ControllableComponentId("component-a"),
                         core::ControllableComponentId("component-b")}));
  EXPECT_EQ(scope.resources, (std::vector{core::ResourceId("resource-a-1"),
                                          core::ResourceId("resource-a-2"),
                                          core::ResourceId("resource-b")}));
  EXPECT_EQ(scope.bindings.size(), 2U);
  EXPECT_EQ(scope.domains.size(), 2U);
}

TEST(ApplyAffectedScope, NativeInitialPositionChangeFencesOnlyItsComponent) {
  const auto predecessor = NativeGraph();
  auto candidate = predecessor;
  std::get<core::JsonValueDTO::Array>(
      candidate.runtime_units.front()
          .configuration.at("initial_joint_positions_rad")
          .value)
      .back() = core::JsonValueDTO{0.4};

  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      Bundle(predecessor, NativeProducts()),
      Bundle(candidate, NativeProducts()));

  ExpectOnlyComponentB(scope);
}

TEST(ApplyAffectedScope, NativeMotionLimitChangeFencesOnlyItsComponent) {
  const auto predecessor = NativeGraph();
  auto candidate = predecessor;
  std::get<core::JsonValueDTO::Array>(
      candidate.runtime_units.front()
          .configuration.at("motion_target_limits")
          .value)
      .back() = MotionLimit("component-b", "target-b", 0.75);

  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      Bundle(predecessor, NativeProducts()),
      Bundle(candidate, NativeProducts()));

  ExpectOnlyComponentB(scope);
}

TEST(ApplyAffectedScope,
     NativeRemovalKeepsOnlyRemovedComponentPredecessorScope) {
  // The collision product/model tables deliberately stay equal: this isolates
  // removal's aggregate controller settings from genuine shared dependencies.
  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      Bundle(NativeGraph(), NativeProducts()),
      Bundle(NativeGraph(false), NativeProducts()));

  ExpectOnlyComponentB(scope);
  ASSERT_EQ(scope.bindings.size(), 1U);
  EXPECT_EQ(scope.bindings.front().predecessor_control_binding_revision,
            core::ControlBindingRevision("binding-b-r1"));
  EXPECT_FALSE(scope.bindings.front().candidate_control_binding_revision);
  ASSERT_EQ(scope.domains.size(), 1U);
  EXPECT_EQ(scope.domains.front().predecessor_control_domain_revision,
            core::ControlDomainRevision("domain-r1"));
  EXPECT_FALSE(scope.domains.front().candidate_control_domain_revision);
}

TEST(ApplyAffectedScope, SharedNativeScalarsFenceEveryHostedComponent) {
  const auto predecessor = NativeGraph();
  for (const auto *field : {"planner_rng_seed", "execution_tick_hz",
                            "collision_monitor_period_ms"}) {
    SCOPED_TRACE(field);
    auto candidate = predecessor;
    auto &value = candidate.runtime_units.front().configuration.at(field).value;
    value = std::get<double>(value) + 1.0;

    const auto scope = derive_apply_configuration_fence_scope_verified_values(
        Bundle(predecessor, NativeProducts()),
        Bundle(candidate, NativeProducts()));

    ExpectBothComponents(scope);
  }
}

TEST(ApplyAffectedScope, SharedNativeScalarsFenceDependentGenericRuntime) {
  const auto predecessor = NativeGraphWithDependentGenericRuntime();
  for (const auto *field : {"planner_rng_seed", "execution_tick_hz",
                            "collision_monitor_period_ms"}) {
    SCOPED_TRACE(field);
    auto candidate = predecessor;
    auto &value = candidate.runtime_units.front().configuration.at(field).value;
    value = std::get<double>(value) + 1.0;

    const auto scope = derive_apply_configuration_fence_scope_verified_values(
        Bundle(predecessor, NativeProducts()),
        Bundle(candidate, NativeProducts()));

    ExpectBothComponents(scope);
  }
}

TEST(ApplyAffectedScope,
     NativeLocalConfigurationDoesNotFenceDependentGenericRuntime) {
  const auto predecessor = NativeGraphWithDependentGenericRuntime();
  for (const auto *field : {"initial_joint_positions_rad",
                            "motion_target_limits"}) {
    SCOPED_TRACE(field);
    auto candidate = predecessor;
    auto &values = std::get<core::JsonValueDTO::Array>(
        candidate.runtime_units.front().configuration.at(field).value);
    values.front() = std::string(field) == "initial_joint_positions_rad"
                         ? core::JsonValueDTO{0.4}
                         : MotionLimit("component-a", "target-a", 0.75);

    const auto scope = derive_apply_configuration_fence_scope_verified_values(
        Bundle(predecessor, NativeProducts()),
        Bundle(candidate, NativeProducts()));

    ASSERT_TRUE(scope.valid());
    EXPECT_EQ(scope.controllable_component_ids,
              (std::vector{core::ControllableComponentId("component-a")}));
    EXPECT_EQ(scope.resources,
              (std::vector{core::ResourceId("resource-a-1"),
                           core::ResourceId("resource-a-2")}));
    ASSERT_EQ(scope.bindings.size(), 1U);
    EXPECT_EQ(scope.bindings.front().control_binding_id,
              core::ControlBindingId("binding-a"));
    ASSERT_EQ(scope.domains.size(), 1U);
    EXPECT_EQ(scope.domains.front().control_domain_id,
              core::ControlDomainId("domain-a"));
  }
}

TEST(ApplyAffectedScope, NativeCollisionProductChangeFencesEveryComponent) {
  const auto graph = NativeGraph();
  auto candidate_products = NativeProducts();
  candidate_products.planning_collision_model.product_id =
      core::ProductId("changed-planning");

  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      Bundle(graph, NativeProducts()), Bundle(graph, candidate_products));

  ExpectBothComponents(scope);
}

TEST(ApplyAffectedScope, NativeHostProfileChangeFencesEveryHostedComponent) {
  const auto predecessor = NativeGraph();
  auto candidate = predecessor;
  candidate.runtime_units.front().runtime_profile_revision =
      core::RuntimeProfileRevision("changed-profile-revision");

  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      Bundle(predecessor, NativeProducts()),
      Bundle(candidate, NativeProducts()));

  ExpectBothComponents(scope);
}

TEST(ApplyAffectedScope, NativeHostProductChangeFencesEveryHostedComponent) {
  const auto graph = NativeGraph();
  auto candidate_products = NativeProducts();
  candidate_products.runtime_unit_products.front().products.front().product_id =
      core::ProductId("changed-native-host-product");

  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      Bundle(graph, NativeProducts()), Bundle(graph, candidate_products));

  ExpectBothComponents(scope);
}

TEST(ApplyAffectedScope,
     NativeAggregateRequirementChangeDoesNotFenceUnchangedComponent) {
  auto predecessor = Bundle(NativeGraph(), NativeProducts());
  predecessor.runtime_contract_requirements = {
      RuntimeRequirement(NativeHostId())};
  auto candidate = predecessor;
  candidate.cell_graph.control_bindings.back().control_binding_revision =
      core::ControlBindingRevision("binding-b-r2");
  std::get<core::JsonValueDTO::Array>(
      candidate.cell_graph.runtime_units.front()
          .configuration.at("initial_joint_positions_rad")
          .value)
      .back() = core::JsonValueDTO{0.4};
  candidate.runtime_contract_requirements.front()
      .requirement.requirement_digest =
      core::RuntimeContractRequirementDigest("sha256:" + std::string(64U, 'b'));

  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      predecessor, candidate);

  ExpectOnlyComponentB(scope);
  ASSERT_EQ(scope.bindings.size(), 1U);
  EXPECT_EQ(scope.bindings.front().candidate_control_binding_revision,
            core::ControlBindingRevision("binding-b-r2"));
}

TEST(ApplyAffectedScope, GenericRequirementChangeStillFencesWholeRuntime) {
  const auto runtime_id = core::RuntimeUnitId("runtime");
  auto predecessor =
      Bundle(Graph({RuntimeUnit(runtime_id.value())},
                   {Component("component-a", {runtime_id}),
                    Component("component-b", {runtime_id})},
                   {Domain("domain-a"), Domain("domain-b")},
                   {Binding("binding-a", "binding-a-r1", "component-a",
                            "domain-a", {"resource-a-1", "resource-a-2"}),
                    Binding("binding-b", "binding-b-r1", "component-b",
                            "domain-b", {"resource-b"})}),
             Products());
  predecessor.runtime_contract_requirements = {RuntimeRequirement(runtime_id)};
  auto candidate = predecessor;
  candidate.runtime_contract_requirements.front()
      .requirement.requirement_digest =
      core::RuntimeContractRequirementDigest("sha256:" + std::string(64U, 'b'));

  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      predecessor, candidate);

  ExpectBothComponents(scope);
}

TEST(ApplyAffectedScope, GenericConfigurationChangeStillFencesWholeRuntime) {
  const auto runtime_id = core::RuntimeUnitId("runtime");
  const auto predecessor =
      Bundle(Graph({RuntimeUnit(runtime_id.value())},
                   {Component("component-a", {runtime_id}),
                    Component("component-b", {runtime_id})},
                   {Domain("domain-a"), Domain("domain-b")},
                   {Binding("binding-a", "binding-a-r1", "component-a",
                            "domain-a", {"resource-a-1", "resource-a-2"}),
                    Binding("binding-b", "binding-b-r1", "component-b",
                            "domain-b", {"resource-b"})}),
             Products());
  auto candidate = predecessor;
  candidate.cell_graph.runtime_units.front().configuration.emplace(
      "changed", core::JsonValueDTO{true});

  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      predecessor, candidate);

  ExpectBothComponents(scope);
}

TEST(ApplyAffectedScope,
     MultipleNativeMotionTargetsRejectBeforeScopeProduction) {
  const auto predecessor = NativeGraph();
  auto candidate = predecessor;
  auto second_motion = candidate.control_bindings.back();
  second_motion.control_binding_id =
      core::ControlBindingId("binding-b-secondary");
  second_motion.command_target_id = core::CommandTargetId("target-b-secondary");
  second_motion.resource_bindings.front().resource_id =
      core::ResourceId("resource-b-secondary");
  candidate.control_bindings.push_back(std::move(second_motion));
  auto &configuration = candidate.runtime_units.front().configuration;
  std::get<core::JsonValueDTO::Array>(
      configuration.at("initial_joint_positions_rad").value)
      .push_back(core::JsonValueDTO{0.5});
  std::get<core::JsonValueDTO::Array>(
      configuration.at("motion_target_limits").value)
      .push_back(MotionLimit("component-b", "target-b-secondary"));
  const auto decoded = decode_native_execution_runtime_configuration(
      configuration, candidate, NativeHostId());
  try {
    static_cast<void>(select_native_component_configuration(
        decoded, candidate, NativeHostId(),
        core::ControllableComponentId("component-b")));
    FAIL() << "ambiguous native motion ownership returned a configuration";
  } catch (const RetainedBundleError &error) {
    EXPECT_EQ(
        std::string(error.what()),
        "native runtime supports at most one motion target per component: "
        "component-b");
  }

  EXPECT_THROW(
      static_cast<void>(derive_apply_configuration_fence_scope_verified_values(
          Bundle(predecessor, NativeProducts()),
          Bundle(candidate, NativeProducts()))),
      RetainedBundleError);
}

TEST(ApplyAffectedScope, RuntimeOnlyAndViewerOnlyChangesStayEmpty) {
  auto old_graph = Graph({{core::RuntimeUnitId("runtime"),
                           core::RuntimeProfileId("profile"),
                           core::RuntimeProfileRevision("profile-r1"),
                           {},
                           {}}},
                         {}, {}, {});
  auto new_graph = old_graph;
  new_graph.runtime_units.front().configuration.emplace("changed", true);
  const auto runtime_scope =
      derive_apply_configuration_fence_scope_verified_values(
          Bundle(old_graph, Products()), Bundle(new_graph, Products()));
  EXPECT_TRUE(runtime_scope.valid());
  EXPECT_TRUE(runtime_scope.resources.empty());

  const auto graph = Graph({}, {}, {}, {});
  const auto viewer_scope =
      derive_apply_configuration_fence_scope_verified_values(
          Bundle(graph, Products("planning", "viewer-a")),
          Bundle(graph, Products("planning", "viewer-b")));
  EXPECT_TRUE(viewer_scope.valid());
  EXPECT_TRUE(viewer_scope.resources.empty());
}

TEST(ApplyAffectedScope, SharedResourceExpandsToOneAtomicFlatClosure) {
  const auto components =
      std::vector{Component("component-a"), Component("component-b")};
  const auto domains = std::vector{Domain("domain-a"), Domain("domain-b")};
  const auto binding_a = Binding("binding-a", "binding-a-r1", "component-a",
                                 "domain-a", {"resource-a", "resource-shared"});
  const auto binding_b = Binding("binding-b", "binding-b-r1", "component-b",
                                 "domain-b", {"resource-b", "resource-shared"});
  auto changed_binding_b = binding_b;
  changed_binding_b.control_binding_revision =
      core::ControlBindingRevision("binding-b-r2");

  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      Bundle(Graph({}, components, domains, {binding_a, binding_b}),
             Products()),
      Bundle(Graph({}, components, domains, {binding_a, changed_binding_b}),
             Products()));

  ASSERT_TRUE(scope.valid());
  EXPECT_EQ(scope.controllable_component_ids,
            (std::vector<core::ControllableComponentId>{
                core::ControllableComponentId("component-a"),
                core::ControllableComponentId("component-b")}));
  ASSERT_EQ(scope.bindings.size(), 2U);
  EXPECT_EQ(scope.resources,
            (std::vector<core::ResourceId>{
                core::ResourceId("resource-a"), core::ResourceId("resource-b"),
                core::ResourceId("resource-shared")}));
  EXPECT_EQ(scope.bindings[1].predecessor_control_binding_revision,
            core::ControlBindingRevision("binding-b-r1"));
  EXPECT_EQ(scope.bindings[1].candidate_control_binding_revision,
            core::ControlBindingRevision("binding-b-r2"));
}

TEST(ApplyAffectedScope, PlanningProductChangeClosesEveryResource) {
  const auto graph = Graph(
      {}, {Component("component")}, {Domain("domain")},
      {Binding("binding", "binding-r1", "component", "domain", {"resource"})});
  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      Bundle(graph, Products("planning-a")),
      Bundle(graph, Products("planning-b")));
  ASSERT_TRUE(scope.valid());
  EXPECT_EQ(scope.resources, (std::vector{core::ResourceId("resource")}));
}

TEST(ApplyAffectedScope, RemovedBindingRetainsItsPredecessorOnlyTransition) {
  const auto components = std::vector{Component("component")};
  const auto domains = std::vector{Domain("domain")};
  const auto binding = Binding("binding", "binding-r1", "component", "domain",
                               {"resource"});

  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      Bundle(Graph({}, components, domains, {binding}), Products()),
      Bundle(Graph({}, components, domains, {}), Products()));

  ASSERT_TRUE(scope.valid());
  ASSERT_EQ(scope.bindings.size(), 1U);
  EXPECT_EQ(scope.bindings.front().control_binding_id,
            core::ControlBindingId("binding"));
  EXPECT_EQ(scope.bindings.front().predecessor_control_binding_revision,
            core::ControlBindingRevision("binding-r1"));
  EXPECT_FALSE(
      scope.bindings.front().candidate_control_binding_revision.has_value());
  EXPECT_EQ(scope.resources, (std::vector{core::ResourceId("resource")}));
}

TEST(ApplyAffectedScope,
     RuntimeDependencyAndProductChangesReachComponentAndDomainConsumers) {
  const auto runtime_a = RuntimeUnit("runtime-a");
  const auto runtime_b =
      RuntimeUnit("runtime-b", {core::RuntimeUnitId("runtime-a")});
  const auto components = std::vector{
      Component("component-a", {core::RuntimeUnitId("runtime-b")}),
      Component("component-b")};
  const auto domains =
      std::vector{Domain("domain-a"),
                  Domain("domain-b", {core::RuntimeUnitId("runtime-a")})};
  const auto bindings = std::vector{
      Binding("binding-a", "binding-a-r1", "component-a", "domain-a",
              {"resource-a"}),
      Binding("binding-b", "binding-b-r1", "component-b", "domain-b",
              {"resource-b"})};
  auto predecessor_products = Products();
  predecessor_products.runtime_unit_products = {
      {core::RuntimeUnitId("runtime-a"),
       {Product("runtime-product-a",
                core::RetainedProductKind::kRuntimeUnitConfiguration)}}};
  auto candidate_products = predecessor_products;
  candidate_products.runtime_unit_products.front().products.front().product_id =
      core::ProductId("runtime-product-b");

  const auto scope = derive_apply_configuration_fence_scope_verified_values(
      Bundle(Graph({runtime_a, runtime_b}, components, domains, bindings),
             predecessor_products),
      Bundle(Graph({runtime_a, runtime_b}, components, domains, bindings),
             candidate_products));

  ASSERT_TRUE(scope.valid());
  EXPECT_EQ(scope.controllable_component_ids,
            (std::vector<core::ControllableComponentId>{
                core::ControllableComponentId("component-a"),
                core::ControllableComponentId("component-b")}));
  EXPECT_EQ(scope.resources,
            (std::vector<core::ResourceId>{core::ResourceId("resource-a"),
                                           core::ResourceId("resource-b")}));
}

TEST(ApplyAffectedScope, CrossCellValuesRejectBeforeScopeProduction) {
  auto predecessor = Bundle(Graph({}, {}, {}, {}), Products());
  auto candidate = predecessor;
  candidate.cell_graph.cell_id = core::CellId("other-cell");
  EXPECT_THROW(
      static_cast<void>(derive_apply_configuration_fence_scope_verified_values(
          predecessor, candidate)),
      ApplyAffectedScopeError);
}

TEST(NativeDomainTopology, RefusesSharedNativeDomainsButNotOtherProviders) {
  const auto native = [](const std::string &id, const std::string &component,
                         const std::string &domain, const std::string &resource) {
    auto binding = Binding(id, id + "-r1", component, domain, {resource});
    binding.provider_manifest_id =
        core::ProviderManifestId(std::string(kNativeControlProviderManifestId));
    return binding;
  };
  const auto components =
      std::vector{Component("component-a"), Component("component-b")};
  const auto one_each =
      Graph({}, components, {Domain("domain-a"), Domain("domain-b")},
            {native("binding-a", "component-a", "domain-a", "resource-a"),
             native("binding-b", "component-b", "domain-b", "resource-b")});
  EXPECT_TRUE(VerifySupportedNativeDomainTopology(one_each).ok());

  const auto shared =
      Graph({}, components, {Domain("domain-a")},
            {native("binding-a", "component-a", "domain-a", "resource-a"),
             native("binding-b", "component-b", "domain-a", "resource-b")});
  const auto refused = VerifySupportedNativeDomainTopology(shared);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "native_shared_domain_unsupported");
  EXPECT_EQ(refused.error().details.at("control_domain_id"), "domain-a");
  EXPECT_EQ(refused.error().details.at("controllable_component_ids"),
            "component-a,component-b");

  // The generic graph contract still allows a shared non-native domain.
  const auto generic = Graph(
      {}, components, {Domain("domain-a")},
      {Binding("binding-a", "binding-a-r1", "component-a", "domain-a",
               {"resource-a"}),
       Binding("binding-b", "binding-b-r1", "component-b", "domain-a",
               {"resource-b"})});
  EXPECT_TRUE(VerifySupportedNativeDomainTopology(generic).ok());
}

} // namespace
} // namespace manyforge::deployment::detail
