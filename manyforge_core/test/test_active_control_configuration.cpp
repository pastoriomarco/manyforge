#include <gtest/gtest.h>

#include <algorithm>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

static_assert(!std::is_same_v<LifecycleTransactionId, RouteSelectionTransactionId>);
static_assert(!std::is_same_v<LifecycleTransactionId, StopCertificateId>);
static_assert(!std::is_same_v<LifecycleTransactionGeneration,
                             RouteSelectionTransactionGeneration>);
static_assert(!std::is_same_v<ComponentLifecycleGeneration,
                             ActiveControlConfigurationGeneration>);
static_assert(!std::is_same_v<StopCertificateRevision, StopGeneration>);
static_assert(std::is_same_v<decltype(ActiveControlConfigurationDTO::control_domain_id),
                             ControlDomainId>);
static_assert(
    std::is_same_v<decltype(ActiveControlConfigurationDTO::binding_routes),
                   std::vector<ActiveControlConfigurationBindingRouteDTO>>);
static_assert(
    std::is_same_v<decltype(ActiveControlConfigurationDTO::source_runtime_unit_id),
                   RuntimeUnitId>);
static_assert(
    std::is_same_v<decltype(ActiveControlConfigurationDTO::source_runtime_epoch), RuntimeEpoch>);
static_assert(std::is_same_v<
              decltype(ActiveControlConfigurationDTO::kinematic_resource_source_projections),
              std::vector<KinematicResourceSourceProjectionDTO>>);

[[nodiscard]] ActiveControlConfigurationBindingRouteDTO BindingRoute(
    std::string binding_id, std::string binding_revision, std::string route_id) {
  return {ControlBindingId(std::move(binding_id)),
          ControlBindingRevision(std::move(binding_revision)),
          ControlRouteId(std::move(route_id))};
}

[[nodiscard]] KinematicResourceSourceProjectionDTO SourceProjection(
    std::string resource_id, std::vector<KinematicControlBindingSourceDTO> contributing_bindings,
    StateAuthorityClass authority_class = StateAuthorityClass::kMeasured,
    ObservedStateSourceKind publication_source = ObservedStateSourceKind::kJointState) {
  return {ResourceId(std::move(resource_id)),
          std::move(contributing_bindings),
          ProviderInstanceId("provider.instance"),
          ProviderManifestId("provider.manifest"),
          ProviderManifestRevision("sha256:provider-manifest"),
          StateAuthorityId("state.authority"),
          authority_class,
          publication_source};
}

[[nodiscard]] ActiveControlConfigurationDTO Configuration() {
  const auto binding_a = BindingRoute("binding.a", "sha256:binding-a", "route.a");
  const auto binding_b = BindingRoute("binding.b", "sha256:binding-b", "route.b");
  return {
      ActiveControlConfigurationId("active.configuration"),
      ControlDomainId("domain.motion"),
      ControlDomainRevision("sha256:domain"),
      {binding_a, binding_b},
      ActivationSetId("activation.trajectory"),
      ControllerTopologyId("topology.standard"),
      TopologyRevision("sha256:topology"),
      RuntimeUnitId("controller.observer"),
      RuntimeEpoch("controller-observer.epoch"),
      17U,
      {SourceProjection("resource.a",
                        {{binding_a.control_binding_id, binding_a.control_binding_revision}}),
       SourceProjection("resource.shared",
                        {{binding_a.control_binding_id, binding_a.control_binding_revision},
                         {binding_b.control_binding_id, binding_b.control_binding_revision}})},
      ActiveControlConfigurationGeneration(9U),
      ActiveControlConfigurationStateDTO::kReconciled,
  };
}

TEST(ActiveControlConfigurationContract, StrongLifecycleAndStopCertificateTypesStayDistinct) {
  const LifecycleTransactionId lifecycle("same");
  const RouteSelectionTransactionId route("same");
  const StopCertificateId certificate("same");

  EXPECT_EQ(lifecycle.value(), route.value());
  EXPECT_EQ(route.value(), certificate.value());
  EXPECT_EQ(LifecycleTransactionGeneration(4U).value(), 4U);
  EXPECT_EQ(ComponentLifecycleGeneration(5U).value(), 5U);
  EXPECT_EQ(RouteSelectionTransactionGeneration(6U).value(), 6U);
  EXPECT_EQ(StopCertificateRevision(7U).value(), 7U);
}

TEST(ActiveControlConfigurationContract, AcceptsCanonicalReconciledAndExternalFacts) {
  auto configuration = Configuration();
  EXPECT_TRUE(configuration.locally_valid());
  EXPECT_EQ(to_string(configuration.state), "reconciled");

  configuration.state =
      ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  EXPECT_TRUE(configuration.locally_valid());
  EXPECT_EQ(to_string(configuration.state), "external_or_unknown_unreconciled");
}

TEST(ActiveControlConfigurationContract, RejectsMissingIdentityAndInvalidCounters) {
  auto configuration = Configuration();
  configuration.active_control_configuration_id = ActiveControlConfigurationId("");
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.source_runtime_unit_id = RuntimeUnitId("");
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.source_runtime_epoch = RuntimeEpoch("");
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.active_control_configuration_generation =
      ActiveControlConfigurationGeneration(0U);
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.state = static_cast<ActiveControlConfigurationStateDTO>(255U);
  EXPECT_FALSE(configuration.locally_valid());
}

TEST(ActiveControlConfigurationContract, RejectsNonCanonicalOrDuplicateBindingRoutes) {
  auto configuration = Configuration();
  configuration.binding_routes.clear();
  configuration.kinematic_resource_source_projections.clear();
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  std::reverse(configuration.binding_routes.begin(), configuration.binding_routes.end());
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.binding_routes = {
      BindingRoute("binding.a", "sha256:binding-a", "route.a"),
      BindingRoute("binding.a", "sha256:binding-a-new", "route.b"),
  };
  configuration.kinematic_resource_source_projections.clear();
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.binding_routes.front().control_route_id = ControlRouteId("");
  EXPECT_FALSE(configuration.locally_valid());
}

TEST(ActiveControlConfigurationContract, RejectsNonCanonicalOrUnselectedProjectionSources) {
  auto configuration = Configuration();
  std::reverse(configuration.kinematic_resource_source_projections.begin(),
               configuration.kinematic_resource_source_projections.end());
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.kinematic_resource_source_projections.front()
      .contributing_control_bindings.clear();
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.kinematic_resource_source_projections.front()
      .contributing_control_bindings.front()
      .control_binding_id = ControlBindingId("binding.unknown");
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.kinematic_resource_source_projections.front()
      .contributing_control_bindings.front()
      .control_binding_revision = ControlBindingRevision("sha256:stale");
  EXPECT_FALSE(configuration.locally_valid());
}

TEST(ActiveControlConfigurationContract, RejectsDuplicateResourcesAndAuthoritySourceMismatch) {
  auto configuration = Configuration();
  configuration.kinematic_resource_source_projections[1].resource_id = ResourceId("resource.a");
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.kinematic_resource_source_projections.front().provider_instance_id =
      ProviderInstanceId("");
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.kinematic_resource_source_projections.front().state_authority_class =
      StateAuthorityClass::kCommanded;
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.kinematic_resource_source_projections.front().state_authority_class =
      StateAuthorityClass::kPredicted;
  configuration.kinematic_resource_source_projections.front().publication_source =
      ObservedStateSourceKind::kPredictedState;
  EXPECT_TRUE(configuration.locally_valid());
}

TEST(ActiveControlConfigurationContract, EnforcesCoreLocalCollectionBounds) {
  auto configuration = Configuration();
  configuration.binding_routes.assign(kMaxActiveControlConfigurationBindingRoutes + 1U,
                                      configuration.binding_routes.front());
  EXPECT_FALSE(configuration.locally_valid());

  configuration = Configuration();
  configuration.kinematic_resource_source_projections.assign(
      kMaxActiveControlConfigurationResourceSourceProjections + 1U,
      configuration.kinematic_resource_source_projections.front());
  EXPECT_FALSE(configuration.locally_valid());
}

}  // namespace
}  // namespace manyforge::core
