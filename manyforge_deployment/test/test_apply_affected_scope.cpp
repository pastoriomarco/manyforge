#include "apply_affected_scope_internal.hpp"

#include <gtest/gtest.h>

#include <manyforge_deployment/apply_affected_scope.hpp>

#include <optional>
#include <string>
#include <utility>
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
      core::ProviderManifestId("manifest"),
      core::ProviderManifestRevision("manifest-r1"),
      core::StateAuthorityId("state-authority"),
      core::StateAuthorityClass::kMeasured,
      core::StopEvidencePolicyId("stop-policy"),
      core::StopEvidencePolicyRevision("stop-policy-r1"),
      core::ContainmentGranularity::kResource,
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
      std::move(products),
      core::RetainedBundleProvenanceDTO{
          core::ContentDigest("compiler-contract"),
          core::ContentDigest("compiler-build"),
          {},
          {},
          {},
      },
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

} // namespace
} // namespace manyforge::deployment::detail
