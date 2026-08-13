#include <gtest/gtest.h>

#include <algorithm>
#include <manyforge_core/controller_lifecycle_evaluator.hpp>
#include <manyforge_core/ports/icontroller_lifecycle_port.hpp>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

[[nodiscard]] ContentDigest Digest(std::string suffix) {
  return ContentDigest("sha256:" + std::move(suffix));
}

[[nodiscard]] ResourceInterfaceClaimDTO PositionClaim() {
  return {ResourceRoleId("arm_joints"), "position"};
}

[[nodiscard]] ControllerTopologyControllerDTO TopologyController(std::string role) {
  return {
      ControllerRoleId(std::move(role)),
      {},
      {PositionClaim()},
      {},
  };
}

[[nodiscard]] ControllerChainEdgeDTO Edge(std::string upstream, std::string downstream) {
  return {
      ControllerRoleId(std::move(upstream)),
      ControllerRoleId(std::move(downstream)),
      {PositionClaim()},
      {},
  };
}

[[nodiscard]] ControllerChainEdgeRefDTO EdgeRef(std::string upstream, std::string downstream) {
  return {ControllerRoleId(std::move(upstream)), ControllerRoleId(std::move(downstream))};
}

[[nodiscard]] ControllerLifecycleExpectedControllerDTO ExpectedController(std::string role) {
  const auto stable_role = role;
  return {
      ControllerRoleId(std::move(role)),
      ControllerImplementationId("implementation." + stable_role),
      ControllerImplementationRevision("sha256:implementation." + stable_role),
      "controller_interface::ControllerInterface",
      Digest("configuration." + stable_role),
      {},
      {},
      {},
      {},
  };
}

[[nodiscard]] ControllerTopologyDTO Topology() {
  return {
      ControllerTopologyId("topology.arm"),
      TopologyRevision("sha256:topology.arm"),
      {TopologyController("outer"), TopologyController("middle"), TopologyController("hardware")},
      {Edge("outer", "middle"), Edge("middle", "hardware")},
      {
          {ActivationSetId("activation.direct"), {ControllerRoleId("hardware")}, {}},
          {ActivationSetId("activation.chain"),
           {ControllerRoleId("hardware"), ControllerRoleId("middle"), ControllerRoleId("outer")},
           {EdgeRef("middle", "hardware"), EdgeRef("outer", "middle")}},
      },
      {
          {ControlRouteId("route.direct"),
           ControlInterfaceContractId("contract.position"),
           ControlInterfaceRevision("sha256:contract.position"),
           {ControllerRoleId("hardware")},
           {ActivationSetId("activation.direct")}},
          {ControlRouteId("route.chain"),
           ControlInterfaceContractId("contract.position"),
           ControlInterfaceRevision("sha256:contract.position"),
           {ControllerRoleId("outer"), ControllerRoleId("middle"), ControllerRoleId("hardware")},
           {ActivationSetId("activation.chain")}},
      },
  };
}

[[nodiscard]] ControllerLifecycleExpectedDomainDTO Expected(bool chained) {
  const auto route = chained ? "route.chain" : "route.direct";
  const auto activation = chained ? "activation.chain" : "activation.direct";
  const DesiredControlBindingRouteDTO binding_route{ControlBindingId("binding.arm"),
                                                    ControlBindingRevision("sha256:binding.arm"),
                                                    ControlRouteId(route)};
  const std::vector<ControllerLifecycleExpectedEndpointDTO> endpoints{
      {binding_route.control_binding_id, binding_route.control_binding_revision,
       ControlRouteId("route.chain"), EndpointRoleId("endpoint.command"),
       "/cell/arm/chain_controller_command", true, false},
      {binding_route.control_binding_id, binding_route.control_binding_revision,
       ControlRouteId("route.direct"), EndpointRoleId("endpoint.command"),
       "/cell/arm/direct_controller_command", true, false},
  };
  return {
      ControlDomainId("domain.arm"),
      ControlDomainRevision("sha256:domain.arm"),
      ControllerTopologyId("topology.arm"),
      TopologyRevision("sha256:topology.arm"),
      ProductId("product.control.arm"),
      ProductContractId("manyforge.control-domain-control.v2"),
      ProductContractRevision("sha256:control-product-contract"),
      Digest("control-artifact"),
      Digest("source-conformance"),
      RuntimeUnitId("runtime.arm"),
      RuntimeEpoch("runtime-epoch.1"),
      Topology(),
      {ControlDomainId("domain.arm"),
       ControlDomainRevision("sha256:domain.arm"),
       ControllerTopologyId("topology.arm"),
       TopologyRevision("sha256:topology.arm"),
       ActivationSetId(activation),
       {binding_route}},
      {{binding_route.control_binding_id,
        binding_route.control_binding_revision,
        ControlInterfaceContractId("contract.position"),
        ControlInterfaceRevision("sha256:contract.position"),
        {{ResourceId("joint.1"), "position"}},
        {{ResourceId("joint.1"), "position"}}}},
      {ExpectedController("hardware"), ExpectedController("middle"), ExpectedController("outer")},
      endpoints,
  };
}

[[nodiscard]] std::optional<ControllerLifecycleObservedConfigurationDTO>
StableObservedConfiguration(const ControllerLifecycleExpectedDomainDTO& expected,
                            const std::map<std::string, ControllerLifecycleActivityDTO>& activities,
                            const std::vector<ControllerChainEdgeRefDTO>& active_edges) {
  for (const auto& activation : expected.controller_topology.activation_sets) {
    std::vector<ControllerRoleId> active_roles;
    for (const auto& [role, activity] : activities) {
      if (activity == ControllerLifecycleActivityDTO::kActive) {
        active_roles.emplace_back(role);
      } else if (activity != ControllerLifecycleActivityDTO::kInactive) {
        active_roles.clear();
        break;
      }
    }
    std::sort(active_roles.begin(), active_roles.end());
    if (active_roles != activation.active_controller_role_ids ||
        active_edges != activation.active_chain_edges) {
      continue;
    }
    std::vector<ActiveControlConfigurationBindingRouteDTO> routes;
    for (const auto& binding : expected.expected_bindings) {
      const auto legal = std::find_if(
          expected.controller_topology.legal_routes.begin(),
          expected.controller_topology.legal_routes.end(), [&](const auto& route) {
            return route.control_interface_contract_id == binding.control_interface_contract_id &&
                   route.control_interface_revision == binding.control_interface_revision &&
                   std::find(route.compatible_activation_set_ids.begin(),
                             route.compatible_activation_set_ids.end(),
                             activation.activation_set_id) !=
                       route.compatible_activation_set_ids.end() &&
                   std::any_of(expected.expected_endpoints.begin(),
                               expected.expected_endpoints.end(), [&](const auto& endpoint) {
                                 return endpoint.control_binding_id == binding.control_binding_id &&
                                        endpoint.control_binding_revision ==
                                            binding.control_binding_revision &&
                                        endpoint.control_route_id == route.control_route_id;
                               });
          });
      if (legal == expected.controller_topology.legal_routes.end()) {
        return std::nullopt;
      }
      routes.push_back(
          {binding.control_binding_id, binding.control_binding_revision, legal->control_route_id});
    }
    return ControllerLifecycleObservedConfigurationDTO{activation.activation_set_id,
                                                       std::move(routes)};
  }
  return std::nullopt;
}

[[nodiscard]] ControllerLifecycleObservationDTO Observation(
    const ControllerLifecycleExpectedDomainDTO& expected,
    const std::map<std::string, ControllerLifecycleActivityDTO>& activities,
    std::vector<ControllerChainEdgeRefDTO> active_edges, bool endpoint_ready = true) {
  std::vector<ControllerLifecycleObservedControllerDTO> controllers;
  controllers.reserve(expected.expected_controllers.size());
  for (const auto& controller : expected.expected_controllers) {
    controllers.push_back({controller, activities.at(controller.controller_role_id.value())});
  }
  std::vector<ControllerLifecycleObservedEndpointDTO> endpoints;
  endpoints.reserve(expected.expected_endpoints.size());
  for (const auto& endpoint : expected.expected_endpoints) {
    const bool selected =
        std::any_of(expected.desired_configuration.binding_routes.begin(),
                    expected.desired_configuration.binding_routes.end(), [&](const auto& route) {
                      return route.control_binding_id == endpoint.control_binding_id &&
                             route.control_binding_revision == endpoint.control_binding_revision &&
                             route.control_route_id == endpoint.control_route_id;
                    });
    endpoints.push_back({endpoint, !selected || endpoint_ready});
  }
  auto active_configuration = StableObservedConfiguration(expected, activities, active_edges);
  return {
      MakeControllerLifecycleObservationQuery(expected),
      7U,
      10,
      20,
      100,
      std::move(active_configuration),
      std::move(controllers),
      std::move(active_edges),
      std::move(endpoints),
  };
}

[[nodiscard]] std::map<std::string, ControllerLifecycleActivityDTO> DirectActivities() {
  return {{"hardware", ControllerLifecycleActivityDTO::kActive},
          {"middle", ControllerLifecycleActivityDTO::kInactive},
          {"outer", ControllerLifecycleActivityDTO::kInactive}};
}

[[nodiscard]] std::map<std::string, ControllerLifecycleActivityDTO> ChainActivities() {
  return {{"hardware", ControllerLifecycleActivityDTO::kActive},
          {"middle", ControllerLifecycleActivityDTO::kActive},
          {"outer", ControllerLifecycleActivityDTO::kActive}};
}

[[nodiscard]] std::vector<ControllerChainEdgeRefDTO> ChainEdges() {
  return {EdgeRef("middle", "hardware"), EdgeRef("outer", "middle")};
}

TEST(ControllerLifecycleEvaluator, ExactFreshDesiredStateReconcilesWithEmptyPlan) {
  const auto expected = Expected(true);
  ASSERT_TRUE(expected.locally_valid());
  const auto observation = Observation(expected, ChainActivities(), ChainEdges());
  ASSERT_TRUE(observation.locally_valid());

  const auto reconciled = ReconcileControllerLifecycle(expected, observation, 50);
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  EXPECT_EQ(reconciled.value().disposition,
            ControllerLifecycleReconciliationDispositionDTO::kReconciled);
  EXPECT_TRUE(reconciled.value().reason_codes.empty());

  const auto plan = BuildControllerLifecycleTransitionPlan(expected, observation, 50);
  ASSERT_TRUE(plan.ok()) << plan.error().message;
  EXPECT_TRUE(plan.value().steps.empty());
  EXPECT_TRUE(plan.value().locally_valid());
}

TEST(ControllerLifecycleEvaluator, DivergenceIsReportedWithoutPassiveRepair) {
  const auto expected = Expected(true);
  const auto observation = Observation(expected, DirectActivities(), {}, false);
  ASSERT_TRUE(observation.active_configuration.has_value());
  EXPECT_EQ(observation.active_configuration->activation_set_id,
            ActivationSetId("activation.direct"));
  ASSERT_EQ(observation.active_configuration->binding_routes.size(), 1U);
  EXPECT_EQ(observation.active_configuration->binding_routes.front().control_route_id,
            ControlRouteId("route.direct"));

  const auto reconciled = ReconcileControllerLifecycle(expected, observation, 50);
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  EXPECT_EQ(reconciled.value().disposition,
            ControllerLifecycleReconciliationDispositionDTO::kExternalOrUnknownUnreconciled);
  EXPECT_EQ(
      reconciled.value().reason_codes,
      (std::vector<std::string>{"active_chain_edges_mismatch", "active_configuration_mismatch",
                                "controller_activity_mismatch", "endpoint_not_ready"}));
}

TEST(ControllerLifecycleEvaluator, MissingOrForeignSelectedConfigurationNeverReconciles) {
  const auto expected = Expected(true);
  auto missing = Observation(expected, ChainActivities(), ChainEdges());
  missing.active_configuration.reset();
  const auto missing_result = ReconcileControllerLifecycle(expected, missing, 50);
  ASSERT_TRUE(missing_result.ok()) << missing_result.error().message;
  EXPECT_EQ(missing_result.value().disposition,
            ControllerLifecycleReconciliationDispositionDTO::kExternalOrUnknownUnreconciled);
  EXPECT_EQ(missing_result.value().reason_codes,
            (std::vector<std::string>{"active_configuration_unavailable"}));

  auto foreign = Observation(expected, ChainActivities(), ChainEdges());
  ASSERT_TRUE(foreign.active_configuration.has_value());
  foreign.active_configuration->binding_routes.front().control_route_id =
      ControlRouteId("route.foreign");
  const auto foreign_result = ReconcileControllerLifecycle(expected, foreign, 50);
  ASSERT_FALSE(foreign_result.ok());
  EXPECT_EQ(foreign_result.error().details.at("reason"), "observed_configuration_invalid");
}

TEST(ControllerLifecycleEvaluator, ActivationRunsHardwareToUpstreamAndPreservesHardware) {
  const auto expected = Expected(true);
  const auto observation = Observation(expected, DirectActivities(), {});

  const auto result = BuildControllerLifecycleTransitionPlan(expected, observation, 50);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().steps.size(), 2U);
  EXPECT_EQ(result.value().steps[0],
            (ControllerLifecycleTransitionStepDTO{ControllerLifecycleTransitionKindDTO::kActivate,
                                                  {ControllerRoleId("middle")}}));
  EXPECT_EQ(result.value().steps[1],
            (ControllerLifecycleTransitionStepDTO{ControllerLifecycleTransitionKindDTO::kActivate,
                                                  {ControllerRoleId("outer")}}));
}

TEST(ControllerLifecycleEvaluator, DeactivationRunsUpstreamToHardwareAndPreservesHardware) {
  const auto expected = Expected(false);
  const auto observation = Observation(expected, ChainActivities(), ChainEdges());

  const auto result = BuildControllerLifecycleTransitionPlan(expected, observation, 50);
  ASSERT_TRUE(result.ok()) << result.error().message;
  ASSERT_EQ(result.value().steps.size(), 2U);
  EXPECT_EQ(result.value().steps[0],
            (ControllerLifecycleTransitionStepDTO{ControllerLifecycleTransitionKindDTO::kDeactivate,
                                                  {ControllerRoleId("outer")}}));
  EXPECT_EQ(result.value().steps[1],
            (ControllerLifecycleTransitionStepDTO{ControllerLifecycleTransitionKindDTO::kDeactivate,
                                                  {ControllerRoleId("middle")}}));
  EXPECT_EQ(result.value().desired_active_controller_role_ids,
            (std::vector<ControllerRoleId>{ControllerRoleId("hardware")}));
}

TEST(ControllerLifecycleEvaluator, ExactIdentityCompletenessAndFreshnessFailClosed) {
  const auto expected = Expected(true);

  auto wrong_epoch = Observation(expected, ChainActivities(), ChainEdges());
  wrong_epoch.query.source_runtime_epoch = RuntimeEpoch("runtime-epoch.2");
  const auto wrong_epoch_result = ReconcileControllerLifecycle(expected, wrong_epoch, 50);
  ASSERT_FALSE(wrong_epoch_result.ok());
  EXPECT_EQ(wrong_epoch_result.error().details.at("reason"), "observation_identity_mismatch");

  auto partial = Observation(expected, ChainActivities(), ChainEdges());
  partial.controllers.pop_back();
  const auto partial_result = ReconcileControllerLifecycle(expected, partial, 50);
  ASSERT_FALSE(partial_result.ok());
  EXPECT_EQ(partial_result.error().details.at("reason"), "observation_incomplete");

  const auto stale = Observation(expected, ChainActivities(), ChainEdges());
  const auto stale_result = ReconcileControllerLifecycle(expected, stale, 100);
  ASSERT_FALSE(stale_result.ok());
  EXPECT_EQ(stale_result.error().details.at("reason"), "observation_not_fresh");
}

TEST(ControllerLifecycleEvaluator, ActiveEdgeRequiresBothObservedEndpointsActive) {
  const auto expected = Expected(true);
  const auto observation = Observation(expected, DirectActivities(), ChainEdges());

  const auto result = ReconcileControllerLifecycle(expected, observation, 50);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "observed_active_edge_invalid");
}

TEST(ControllerLifecycleEvaluator, TruthfulUnknownActivityReconcilesExternalButCannotPlan) {
  const auto expected = Expected(true);
  auto activities = ChainActivities();
  activities["outer"] = ControllerLifecycleActivityDTO::kUnknown;
  const auto observation = Observation(expected, activities, {EdgeRef("middle", "hardware")});

  const auto reconciliation = ReconcileControllerLifecycle(expected, observation, 50);
  ASSERT_TRUE(reconciliation.ok()) << reconciliation.error().message;
  EXPECT_EQ(reconciliation.value().disposition,
            ControllerLifecycleReconciliationDispositionDTO::kExternalOrUnknownUnreconciled);

  const auto plan = BuildControllerLifecycleTransitionPlan(expected, observation, 50);
  ASSERT_FALSE(plan.ok());
  EXPECT_EQ(plan.error().details.at("reason"), "controller_activity_not_plannable");
}

TEST(ControllerLifecycleEvaluator, TopologyCycleRejectsBeforeAnyTransitionPlan) {
  auto expected = Expected(true);
  expected.controller_topology.chain_edges.push_back(Edge("hardware", "outer"));
  const auto observation = Observation(expected, ChainActivities(), ChainEdges());

  const auto result = BuildControllerLifecycleTransitionPlan(expected, observation, 50);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "controller_topology_cycle");
}

TEST(ControllerLifecycleContracts, TransitionDispatchPresenceRulesAreClosed) {
  const auto expected = Expected(false);
  const auto observation = Observation(expected, DirectActivities(), {});
  const ControllerLifecycleTransitionIntentDTO intent{
      observation.query,
      observation.source_sequence,
      0U,
      {ControllerLifecycleTransitionKindDTO::kActivate, {ControllerRoleId("hardware")}},
      1'000'000,
  };
  ASSERT_TRUE(intent.locally_valid());

  EXPECT_TRUE((ControllerLifecycleTransitionResultDTO{
                   intent, ControllerLifecycleTransitionDispatchStateDTO::kDispatchedKnown,
                   ControllerLifecycleTransitionKnownResultDTO::kSucceeded, std::nullopt})
                  .locally_valid());
  EXPECT_FALSE((ControllerLifecycleTransitionResultDTO{
                    intent, ControllerLifecycleTransitionDispatchStateDTO::kDispatchedUncertain,
                    ControllerLifecycleTransitionKnownResultDTO::kSucceeded,
                    ErrorDTO{"controller_transition_uncertain", "uncertain", {}}})
                   .locally_valid());
  EXPECT_TRUE((ControllerLifecycleTransitionResultDTO{
                   intent, ControllerLifecycleTransitionDispatchStateDTO::kDispatchedUncertain,
                   std::nullopt, ErrorDTO{"controller_transition_uncertain", "uncertain", {}}})
                  .locally_valid());
}

TEST(ControllerLifecycleContracts, InterfaceInstancesUsePortableV1TokenGrammar) {
  EXPECT_TRUE((ControllerInterfaceInstanceDTO{ResourceId("joint.1"), "position"}).locally_valid());
  EXPECT_FALSE(
      (ControllerInterfaceInstanceDTO{ResourceId("joint.1"), "joint/position"}).locally_valid());
  EXPECT_FALSE(
      (ControllerInterfaceInstanceDTO{ResourceId("joint.1"), "-position"}).locally_valid());
}

}  // namespace
}  // namespace manyforge::core
