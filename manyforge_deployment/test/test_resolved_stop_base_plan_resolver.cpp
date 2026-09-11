#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <manyforge_core/controller_lifecycle_evaluator.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_deployment/resolved_stop_base_plan_resolver.hpp>
#include <manyforge_deployment/retained_runtime_policy.hpp>
#include <manyforge_deployment/route_configuration_publication.hpp>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unistd.h>

#include "build_manifest_internal.hpp"
#include "content_identity_internal.hpp"
#include "lite6_retained_consumer_fixture.hpp"
#include "stop_timing_resolution_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

class ScopedTrustedRoot final {
public:
  ScopedTrustedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-resolved-stop-plan-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::logic_error("test could not create its trusted root");
    }
  }

  ~ScopedTrustedRoot() {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  inline static std::atomic<std::uint64_t> sequence_{0U};
  std::filesystem::path path_;
};

template <typename Range, typename Predicate>
[[nodiscard]] const typename Range::value_type &
require_one(const Range &values, Predicate predicate,
            std::string_view description) {
  const auto found = std::find_if(values.begin(), values.end(), predicate);
  if (found == values.end() ||
      std::find_if(std::next(found), values.end(), predicate) != values.end()) {
    throw std::logic_error("fixture has no sole " + std::string(description));
  }
  return *found;
}

struct RuntimeContext final {
  core::RuntimeParticipantBinding binding;
  core::ActiveControlConfigurationDTO active_configuration;
  core::DesiredControlConfigurationDTO desired_configuration;
  core::RuntimeUnitId source_runtime_unit_id;
  core::RuntimeEpoch source_runtime_epoch;
};

[[nodiscard]] RuntimeContext
make_runtime_context(const RetainedBundle &retained_bundle) {
  const auto &graph = retained_bundle.value().cell_graph;
  const auto &control_binding = require_one(
      graph.control_bindings,
      [](const auto &candidate) {
        return candidate.capability_id ==
               core::CapabilityId("execution.trajectory.execute");
      },
      "trajectory control binding");
  const auto &component = require_one(
      graph.controllable_components,
      [&](const auto &candidate) {
        return candidate.controllable_component_id ==
               control_binding.controllable_component_id;
      },
      "controllable component");
  const auto &domain = require_one(
      graph.control_domains,
      [&](const auto &candidate) {
        return candidate.control_domain_id == control_binding.control_domain_id;
      },
      "control domain");
  const auto &desired = require_one(
      retained_bundle.value().desired_control_configurations,
      [&](const auto &candidate) {
        return candidate.control_domain_id == domain.control_domain_id;
      },
      "desired control configuration");
  const auto &route = require_one(
      desired.binding_routes,
      [&](const auto &candidate) {
        return candidate.control_binding_id ==
               control_binding.control_binding_id;
      },
      "desired binding route");

  std::vector<core::ProgramModelRoleBindingDTO> models;
  for (const auto &row : component.model_role_bindings) {
    const auto &model = require_one(
        graph.model_instances,
        [&](const auto &candidate) {
          return candidate.model_instance_id == row.model_instance_id;
        },
        "model instance");
    models.push_back(
        {row.model_role_id, row.model_instance_id, model.model_revision});
  }
  if (models.empty()) {
    throw std::logic_error("fixture component has no model binding");
  }
  std::vector<core::RuntimeResourceBindingDTO> resources;
  for (std::size_t index = 0U; index < control_binding.resource_bindings.size();
       ++index) {
    const auto &row = control_binding.resource_bindings[index];
    const auto symbol = "joint_" + std::to_string(index + 1U);
    resources.push_back({row.resource_role_id, row.resource_id, symbol, symbol,
                         core::CoordinateKind::kAngular});
  }
  std::sort(resources.begin(), resources.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.resource_id < rhs.resource_id;
            });

  const core::RuntimeEpoch runtime_epoch(
      "epoch." + control_binding.provider_runtime_unit_id.value());
  const core::RuntimeEpoch provider_epoch("provider.epoch.1");
  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("resolved.stop.plan.program"),
      core::ProgramRevision(std::string(kZeroDigest)),
      core::ProgramBindingId("resolved.stop.plan.binding"),
      retained_bundle.retained_bundle_id(),
      graph.graph_revision,
      core::ComponentRoleId("robot"),
      core::TargetRoleId("arm"),
      component.controllable_component_id,
      component.component_type_id,
      component.component_type_revision,
      control_binding.command_target_id,
      control_binding.capability_id,
      control_binding.capability_definition_revision,
      std::nullopt,
      std::move(models),
      {},
      {core::ProgramFrameRoleBindingDTO{
          core::FrameRoleId("base"),
          component.model_role_bindings.front().model_instance_id,
          require_one(
              graph.model_instances,
              [&](const auto &candidate) {
                return candidate.model_instance_id ==
                       component.model_role_bindings.front().model_instance_id;
              },
              "frame model")
              .model_revision,
          "base_link"}},
      resources,
      {},
      core::RuntimeControlPathDTO{
          domain.control_domain_id,
          domain.control_domain_revision,
          control_binding.control_binding_id,
          control_binding.control_binding_revision,
          control_binding.control_interface_contract_id,
          control_binding.control_interface_revision,
          control_binding.controller_topology_id,
          control_binding.topology_revision,
          route.control_route_id,
          desired.activation_set_id,
          core::ActiveControlConfigurationId("active.resolved.stop.plan"),
          core::ActiveControlConfigurationGeneration(4U),
          control_binding.provider_instance_id,
          control_binding.provider_manifest_id,
          control_binding.provider_manifest_revision,
          control_binding.provider_runtime_unit_id,
          provider_epoch,
          control_binding.state_authority_id,
          control_binding.state_authority_class,
          control_binding.stop_evidence_policy_id,
          control_binding.stop_evidence_policy_revision,
          control_binding.containment_granularity,
          std::nullopt},
      std::nullopt,
      {},
      {{control_binding.provider_runtime_unit_id, runtime_epoch}},
      {}};
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }

  std::vector<core::ActiveControlConfigurationBindingRouteDTO> active_routes;
  for (const auto &row : desired.binding_routes) {
    active_routes.push_back({row.control_binding_id,
                             row.control_binding_revision,
                             row.control_route_id});
  }
  std::sort(active_routes.begin(), active_routes.end(),
            core::CanonicalActiveControlConfigurationBindingRouteLess);
  std::vector<core::KinematicResourceSourceProjectionDTO> projections;
  const auto publication_source = [&] {
    switch (control_binding.state_authority_class) {
    case core::StateAuthorityClass::kMeasured:
      return core::ObservedStateSourceKind::kJointState;
    case core::StateAuthorityClass::kCommanded:
      return core::ObservedStateSourceKind::kCommandedState;
    case core::StateAuthorityClass::kPredicted:
      return core::ObservedStateSourceKind::kPredictedState;
    }
    throw std::logic_error("fixture has an unknown state-authority class");
  }();
  for (const auto &resource : resources) {
    projections.push_back({
        resource.resource_id,
        {{control_binding.control_binding_id,
          control_binding.control_binding_revision}},
        control_binding.provider_instance_id,
        control_binding.provider_manifest_id,
        control_binding.provider_manifest_revision,
        control_binding.state_authority_id,
        control_binding.state_authority_class,
        publication_source,
    });
  }
  const core::RuntimeUnitId source_runtime_unit_id =
      control_binding.provider_runtime_unit_id;
  const core::RuntimeEpoch source_runtime_epoch = runtime_epoch;
  core::ActiveControlConfigurationDTO active{
      core::ActiveControlConfigurationId("active.resolved.stop.plan"),
      domain.control_domain_id,
      domain.control_domain_revision,
      std::move(active_routes),
      desired.activation_set_id,
      domain.controller_topology_id,
      domain.topology_revision,
      source_runtime_unit_id,
      source_runtime_epoch,
      7U,
      std::move(projections),
      core::ActiveControlConfigurationGeneration(4U),
      core::ActiveControlConfigurationStateDTO::kReconciled};
  if (!active.locally_valid()) {
    throw std::logic_error("test active configuration is invalid");
  }
  return {std::move(sealed.value()), std::move(active), desired,
          source_runtime_unit_id, source_runtime_epoch};
}

class MemoryPlanRepository final
    : public core::IResolvedStopBasePlanRepository {
public:
  [[nodiscard]] core::Result<core::ResolvedStopBasePlanId>
  PutIfAbsent(core::ResolvedStopBasePlanDTO plan) override {
    auto made = core::MakeResolvedStopBasePlan(std::move(plan));
    if (!made.ok()) {
      return core::Result<core::ResolvedStopBasePlanId>::Err(made.error());
    }
    value_ = std::move(made.value());
    return core::Result<core::ResolvedStopBasePlanId>::Ok(
        value_->resolved_stop_base_plan_id);
  }

  [[nodiscard]] core::Result<core::ResolvedStopBasePlanDTO>
  LoadVerified(const core::ResolvedStopBasePlanId &id) const override {
    if (!value_.has_value() || value_->resolved_stop_base_plan_id != id) {
      return core::Result<core::ResolvedStopBasePlanDTO>::Err(
          core::ErrorDTO{"missing_reference", "plan is absent", {}});
    }
    auto result = *value_;
    if (corrupt_readback_) {
      result.source_sequence += 1U;
    }
    return core::Result<core::ResolvedStopBasePlanDTO>::Ok(std::move(result));
  }

  bool corrupt_readback_{};

private:
  std::optional<core::ResolvedStopBasePlanDTO> value_;
};

[[nodiscard]] core::RouteSelectionAcceptedDTO make_route_acceptance(
    const RuntimeContext &runtime,
    const VerifiedControllerLifecycleExpectedDomain &expected) {
  const auto &binding = runtime.binding.value();
  std::vector<core::RouteSelectionComponentExpectationDTO> components{
      {binding.controllable_component_id,
       core::ComponentLifecycleGeneration(11U),
       core::ComponentLifecycleStateDTO::kActive}};
  std::vector<core::ConfigurationFenceBindingScopeDTO> scoped_bindings;
  scoped_bindings.reserve(runtime.desired_configuration.binding_routes.size());
  for (const auto &route : runtime.desired_configuration.binding_routes) {
    scoped_bindings.push_back({route.control_binding_id,
                               route.control_binding_revision,
                               route.control_binding_revision});
  }
  std::vector<core::ResourceId> resources;
  resources.reserve(binding.resources.size());
  for (const auto &resource : binding.resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());
  core::ConfigurationFenceScopeDTO scope{
      {binding.controllable_component_id},
      {{runtime.desired_configuration.control_domain_id,
        runtime.desired_configuration.control_domain_revision,
        runtime.desired_configuration.control_domain_revision}},
      std::move(scoped_bindings),
      std::move(resources)};
  core::RouteSelectionAcceptedDTO result{
      core::RequestId("request.route.publication"),
      core::RequestReplayStatusDTO::kAcceptedNew,
      core::RouteSelectionTransactionId("route.transaction.publication"),
      core::RouteSelectionTransactionGeneration(1U),
      binding.retained_bundle_id,
      binding.graph_revision,
      runtime.desired_configuration,
      std::move(components),
      runtime.active_configuration,
      std::move(scope),
      core::MakeControllerLifecycleObservationQuery(expected.value()),
      core::ActiveControlConfigurationId("active.route.publication"),
      core::ActiveControlConfigurationGeneration(5U),
      50,
      1'000'000'000};
  if (!result.locally_valid()) {
    throw std::logic_error("test route acceptance is invalid");
  }
  return result;
}

[[nodiscard]] core::ActiveControlConfigurationDTO
make_route_configuration(const RuntimeContext &runtime) {
  auto result = runtime.active_configuration;
  result.active_control_configuration_id =
      core::ActiveControlConfigurationId("active.route.publication");
  result.active_control_configuration_generation =
      core::ActiveControlConfigurationGeneration(5U);
  if (!result.locally_valid()) {
    throw std::logic_error("test route configuration is invalid");
  }
  return result;
}

[[nodiscard]] core::ControllerLifecycleObservationDTO
make_desired_controller_observation(
    const VerifiedControllerLifecycleExpectedDomain &expected) {
  std::vector<core::ActiveControlConfigurationBindingRouteDTO> routes;
  routes.reserve(expected.value().desired_configuration.binding_routes.size());
  for (const auto &route :
       expected.value().desired_configuration.binding_routes) {
    routes.push_back({route.control_binding_id, route.control_binding_revision,
                      route.control_route_id});
  }
  std::vector<core::ControllerLifecycleObservedControllerDTO> controllers;
  controllers.reserve(expected.value().expected_controllers.size());
  for (const auto &controller : expected.value().expected_controllers) {
    controllers.push_back(
        {controller, core::ControllerLifecycleActivityDTO::kActive});
  }
  std::vector<core::ControllerLifecycleObservedEndpointDTO> endpoints;
  endpoints.reserve(expected.value().expected_endpoints.size());
  for (const auto &endpoint : expected.value().expected_endpoints) {
    endpoints.push_back({endpoint, true});
  }
  return {core::MakeControllerLifecycleObservationQuery(expected.value()),
          1U,
          100,
          101,
          1'000,
          core::ControllerLifecycleObservedConfigurationDTO{
              expected.value().desired_configuration.activation_set_id,
              std::move(routes)},
          std::move(controllers),
          {},
          std::move(endpoints)};
}

class MemoryRouteConfigurationKernelPublicationOwner final
    : public IRouteConfigurationKernelPublicationOwner {
public:
  explicit MemoryRouteConfigurationKernelPublicationOwner(
      std::vector<core::RuntimeParticipantBinding> current)
      : current_(std::move(current)) {}

  [[nodiscard]] core::Result<std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO &) const override {
    ++read_count_;
    return core::Result<std::vector<core::RuntimeParticipantBinding>>::Ok(
        current_);
  }

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO &configuration,
      std::vector<VerifiedRouteReadinessParticipant> participants) override {
    ++publish_count_;
    published_participants_ = participants;
    std::vector<core::RouteReadinessPublicationRowDTO> rows;
    rows.reserve(participants.size());
    for (const auto &participant : participants) {
      rows.push_back(participant.readiness_row);
    }
    auto readback_configuration = configuration;
    if (mismatch_configuration_) {
      readback_configuration.active_control_configuration_id =
          core::ActiveControlConfigurationId("active.route.drifted");
    }
    const bool expected_open =
        configuration.state ==
        core::ActiveControlConfigurationStateDTO::kReconciled;
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Ok(
        {std::move(readback_configuration), std::move(rows),
         mismatch_admission_ ? !expected_open : expected_open});
  }

  mutable std::size_t read_count_{};
  std::size_t publish_count_{};
  bool mismatch_configuration_{};
  bool mismatch_admission_{};
  std::vector<VerifiedRouteReadinessParticipant> published_participants_;

private:
  std::vector<core::RuntimeParticipantBinding> current_;
};

TEST(ResolvedStopBasePlanResolverTest,
     SavedTimingSurvivesCompilationDurableReloadAndReadiness) {
  ScopedTrustedRoot root;
  const core::StopTimingParametersDTO timing{250'000'000, 750'000'000,
                                             3'000'000'000};
  const auto retained = test::materialize_lite6_retained_consumer_fixture_from(
      root.path(),
      test::make_lite6_stop_qualified_native_motion_retention_fixture(
          {timing}));
  ASSERT_EQ(retained.bundle->value().stop_timing_parameters.size(), 1U);
  EXPECT_EQ(retained.bundle->value().stop_timing_parameters.front().timing,
            timing);
  FilesystemVerifiedRetainedProductArtifactSource source(root.path());
  const auto runtime = make_runtime_context(*retained.bundle);
  const auto expected = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, runtime.desired_configuration,
      runtime.source_runtime_unit_id, runtime.source_runtime_epoch);
  const auto plan = resolve_verified_stop_base_plan(
      *retained.bundle, source, expected, runtime.active_configuration,
      runtime.binding);
  EXPECT_EQ(plan.deadline_budget.phases.front().deadline_offset_ns,
            250'000'000);
  EXPECT_EQ(plan.deadline_budget.phases[1].deadline_offset_ns, 3'000'000'000);
  FilesystemCatalogArtifactStore catalogs(root.path());
  const auto envelope =
      resolve_retained_stop_deadline_budget(*retained.bundle, catalogs);
  ASSERT_TRUE(envelope);
  EXPECT_EQ(envelope->outer_duration_ns,
            plan.deadline_budget.outer_duration_ns);
  MemoryPlanRepository repository;
  const auto readiness = store_and_verify_runtime_participant_stop_readiness(
      *retained.bundle, plan, core::ComponentLifecycleGeneration(11U), expected,
      runtime.binding, repository);
  ASSERT_TRUE(readiness.ok()) << readiness.error().message;
  EXPECT_EQ(readiness.value().value().maximum_controlled_stop_duration_ns,
            2'000'000'000);
  const auto different_settling =
      test::materialize_lite6_retained_consumer_fixture_from(
          root.path(),
          test::make_lite6_stop_qualified_native_motion_retention_fixture(
              {{250'000'000, 500'000'000, 3'000'000'000}}));
  EXPECT_NE(different_settling.bundle->retained_bundle_id(),
            retained.bundle->retained_bundle_id());
  EXPECT_FALSE(store_and_verify_runtime_participant_stop_readiness(
                   *different_settling.bundle, plan,
                   core::ComponentLifecycleGeneration(11U), expected,
                   runtime.binding, repository)
                   .ok());
  EXPECT_THROW(test::make_lite6_stop_qualified_native_motion_retention_fixture(
                   {{250'000'000, 750'000'000, 1'999'999'999}}),
               DeploymentApplicabilityCodecError);
}

TEST(ResolvedStopBasePlanResolverTest,
     ResolvesExactActiveProductAndPublishesOnlyAfterIndependentReadback) {
  ScopedTrustedRoot root;
  const auto retained = test::
      materialize_lite6_stop_capable_native_motion_retained_consumer_fixture(
          root.path());
  const FilesystemVerifiedRetainedProductArtifactSource source(root.path());
  const auto runtime = make_runtime_context(*retained.bundle);
  const auto expected = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, runtime.desired_configuration,
      runtime.source_runtime_unit_id, runtime.source_runtime_epoch);

  const auto plan = resolve_verified_stop_base_plan(
      *retained.bundle, source, expected, runtime.active_configuration,
      runtime.binding);
  ASSERT_TRUE(core::VerifyResolvedStopBasePlan(plan).ok());
  EXPECT_EQ(plan.retained_bundle_id, retained.bundle->retained_bundle_id());
  EXPECT_EQ(plan.active_control_configuration_id,
            runtime.active_configuration.active_control_configuration_id);
  EXPECT_EQ(plan.source_sequence, 7U);
  ASSERT_EQ(plan.escalation_stages.size(), 2U);
  EXPECT_EQ(plan.escalation_stages.front().stage_kind,
            core::StopEscalationStageKind::kNominal);
  EXPECT_TRUE(
      plan.escalation_stages.front()
          .command_qualification.controlled_substitution_contract.has_value());
  EXPECT_EQ(plan.escalation_stages.back().stage_kind,
            core::StopEscalationStageKind::kSoftwareHard);
  EXPECT_EQ(
      plan.escalation_stages.back().command_qualification.stop_action_class,
      core::StopActionClass::kSoftwareHard);

  MemoryPlanRepository repository;
  auto readiness = store_and_verify_runtime_participant_stop_readiness(
      *retained.bundle, plan, core::ComponentLifecycleGeneration(11U), expected,
      runtime.binding, repository);
  ASSERT_TRUE(readiness.ok()) << readiness.error().message;
  EXPECT_EQ(readiness.value().value().resolved_stop_base_plan, plan);
  EXPECT_FALSE(readiness.value().value().maximum_controlled_stop_duration_ns);

  repository.corrupt_readback_ = true;
  readiness = store_and_verify_runtime_participant_stop_readiness(
      *retained.bundle, plan, core::ComponentLifecycleGeneration(11U), expected,
      runtime.binding, repository);
  ASSERT_FALSE(readiness.ok());
  EXPECT_EQ(readiness.error().details.at("reason"),
            "resolved_stop_base_plan_repository_readback_mismatch");
}

TEST(ResolvedStopBasePlanResolverTest,
     IndependentDomainCutoffsSurviveOneRetainedEnvelope) {
  ScopedTrustedRoot root;
  auto fixture = test::make_lite6_controllable_retention_fixture(
      false, false, 2U, false, false, {}, false, true, "resource", false, {},
      {}, false, false,
      {{250'000'000, 750'000'000, 3'000'000'000},
       {200'000'000, 500'000'000, 4'000'000'000}});
  const auto &stops =
      fixture.qualified.compiler_ir().applicability().stop_applicability();
  ASSERT_EQ(stops.size(), 2U);
  const auto &first = std::get<LogicalStoppableApplicabilityTemplate>(stops[0]);
  const auto &second =
      std::get<LogicalStoppableApplicabilityTemplate>(stops[1]);
  EXPECT_EQ(first.deadline_budget.outer_duration_ns,
            second.deadline_budget.outer_duration_ns);
  std::set<std::int64_t> cutoffs{
      first.deadline_budget.phases[1].deadline_offset_ns,
      second.deadline_budget.phases[1].deadline_offset_ns};
  EXPECT_EQ(cutoffs, (std::set<std::int64_t>{3'000'000'000, 4'000'000'000}));
  const auto &applicability = fixture.qualified.compiler_ir().applicability();
  const auto &bindings =
      fixture.qualified.deployment().graph().graph().control_bindings;
  auto selections =
      applicability.deployment_inputs().value().stop_timing_parameters;
  ASSERT_EQ(selections.size(), 2U);
  const auto omitted_id = selections.back().control_binding_id;
  selections.pop_back();
  const auto mixed = detail::resolve_binding_stop_timing(
      bindings, applicability.stop_evidence_policies(), selections);
  ASSERT_TRUE(mixed.ok()) << mixed.error().message;
  EXPECT_EQ(mixed.value().at(omitted_id).phases[1].deadline_offset_ns,
            300'000'000);
  EXPECT_GT(mixed.value().at(omitted_id).outer_duration_ns, 3'000'000'000);
  selections.front().control_binding_revision =
      core::ControlBindingRevision(std::string(kZeroDigest));
  EXPECT_FALSE(detail::resolve_binding_stop_timing(
                   bindings, applicability.stop_evidence_policies(), selections)
                   .ok());
  const auto retained = test::materialize_lite6_retained_consumer_fixture_from(
      root.path(), std::move(fixture));
  ASSERT_EQ(retained.bundle->value().cell_graph.control_domains.size(), 2U);
  ASSERT_EQ(retained.bundle->value().stop_timing_parameters.size(), 2U);
  FilesystemCatalogArtifactStore catalogs(root.path());
  const auto envelope =
      resolve_retained_stop_deadline_budget(*retained.bundle, catalogs);
  ASSERT_TRUE(envelope);
  EXPECT_GT(envelope->outer_duration_ns, 4'000'000'000);
}

TEST(ResolvedStopBasePlanResolverTest,
     RejectsExternalConfigurationOrRuntimeAuthorityDrift) {
  ScopedTrustedRoot root;
  const auto retained = test::
      materialize_lite6_stop_capable_native_motion_retained_consumer_fixture(
          root.path());
  const FilesystemVerifiedRetainedProductArtifactSource source(root.path());
  const auto runtime = make_runtime_context(*retained.bundle);
  const auto expected = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, runtime.desired_configuration,
      runtime.source_runtime_unit_id, runtime.source_runtime_epoch);

  auto unreconciled = runtime.active_configuration;
  unreconciled.state =
      core::ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  EXPECT_THROW(
      static_cast<void>(resolve_verified_stop_base_plan(
          *retained.bundle, source, expected, unreconciled, runtime.binding)),
      RetainedBundleError);

  auto changed_authority = runtime.active_configuration;
  changed_authority.kinematic_resource_source_projections.front()
      .state_authority_id = core::StateAuthorityId("state.substituted");
  EXPECT_THROW(static_cast<void>(resolve_verified_stop_base_plan(
                   *retained.bundle, source, expected, changed_authority,
                   runtime.binding)),
               RetainedBundleError);
}

TEST(ResolvedStopBasePlanResolverTest,
     RebindsAndPublishesOneCompleteSelectedBindingBatch) {
  ScopedTrustedRoot root;
  const auto retained = test::
      materialize_lite6_stop_capable_native_motion_retained_consumer_fixture(
          root.path());
  const FilesystemVerifiedRetainedProductArtifactSource source(root.path());
  const auto runtime = make_runtime_context(*retained.bundle);
  const auto expected = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, runtime.desired_configuration,
      runtime.source_runtime_unit_id, runtime.source_runtime_epoch);

  auto active = runtime.active_configuration;
  active.active_control_configuration_id =
      core::ActiveControlConfigurationId("active.route.publication");
  active.active_control_configuration_generation =
      core::ActiveControlConfigurationGeneration(5U);
  const std::vector<core::RouteSelectionComponentExpectationDTO> components{
      {runtime.binding.value().controllable_component_id,
       core::ComponentLifecycleGeneration(11U),
       core::ComponentLifecycleStateDTO::kActive}};
  const std::vector<core::RuntimeParticipantBinding> participants{
      runtime.binding};
  MemoryPlanRepository repository;

  auto batch = resolve_store_and_verify_route_readiness_batch(
      *retained.bundle, source, expected, active, components, participants,
      repository);
  ASSERT_TRUE(batch.ok()) << batch.error().message;
  ASSERT_EQ(batch.value().size(), 1U);
  const auto &published = batch.value().front();
  ASSERT_TRUE(published.participant_binding.value().control_path.has_value());
  EXPECT_EQ(published.participant_binding.value()
                .control_path->active_control_configuration_id,
            active.active_control_configuration_id);
  EXPECT_EQ(published.participant_binding.value()
                .control_path->active_control_configuration_generation,
            active.active_control_configuration_generation);
  EXPECT_EQ(published.stop_readiness.value().component_lifecycle_generation,
            core::ComponentLifecycleGeneration(11U));
  EXPECT_EQ(published.stop_readiness.value()
                .resolved_stop_base_plan.active_control_configuration_id,
            active.active_control_configuration_id);
  EXPECT_EQ(published.readiness_row.controllable_component_id,
            runtime.binding.value().controllable_component_id);
  EXPECT_EQ(published.readiness_row.resolved_stop_base_plan_id,
            published.stop_readiness.value()
                .resolved_stop_base_plan.resolved_stop_base_plan_id);

  const std::vector<core::RuntimeParticipantBinding> missing;
  batch = resolve_store_and_verify_route_readiness_batch(
      *retained.bundle, source, expected, active, components, missing,
      repository);
  ASSERT_FALSE(batch.ok());
  EXPECT_EQ(batch.error().details.at("reason"),
            "route_readiness_batch_shape_invalid");

  active.state =
      core::ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  batch = resolve_store_and_verify_route_readiness_batch(
      *retained.bundle, source, expected, active, components, participants,
      repository);
  ASSERT_FALSE(batch.ok());
  EXPECT_EQ(batch.error().details.at("reason"),
            "route_readiness_batch_shape_invalid");
}

TEST(ResolvedStopBasePlanResolverTest,
     PublishesExactRouteReadinessAndRejectsKernelReadbackDrift) {
  ScopedTrustedRoot root;
  const auto retained = test::
      materialize_lite6_stop_capable_native_motion_retained_consumer_fixture(
          root.path());
  const FilesystemVerifiedRetainedProductArtifactSource source(root.path());
  const auto runtime = make_runtime_context(*retained.bundle);
  const auto expected = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, runtime.desired_configuration,
      runtime.source_runtime_unit_id, runtime.source_runtime_epoch);
  const auto acceptance = make_route_acceptance(runtime, expected);
  const auto configuration = make_route_configuration(runtime);
  MemoryPlanRepository repository;
  MemoryRouteConfigurationKernelPublicationOwner owner({runtime.binding});
  VerifiedRouteConfigurationPublicationPort publisher(
      *retained.bundle, source, repository, owner,
      runtime.active_configuration.kinematic_resource_source_projections,
      [] { return 75; });

  const auto first = publisher.PublishAndReadBack(acceptance, expected.sealed(),
                                                  configuration);
  ASSERT_TRUE(first.ok()) << first.error().message;
  ASSERT_TRUE(first.value().locally_valid());
  EXPECT_TRUE(first.value().operation_admission_open);
  ASSERT_EQ(first.value().readiness_rows.size(), 1U);
  EXPECT_EQ(first.value().readiness_rows.front().controllable_component_id,
            runtime.binding.value().controllable_component_id);
  ASSERT_EQ(owner.published_participants_.size(), 1U);
  EXPECT_EQ(owner.published_participants_.front()
                .participant_binding.value()
                .control_path->active_control_configuration_id,
            configuration.active_control_configuration_id);
  EXPECT_EQ(owner.published_participants_.front()
                .stop_readiness.value()
                .resolved_stop_base_plan.active_control_configuration_id,
            configuration.active_control_configuration_id);

  const auto replay = publisher.PublishAndReadBack(
      acceptance, expected.sealed(), configuration);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value(), first.value());
  EXPECT_EQ(owner.read_count_, 2U);
  EXPECT_EQ(owner.publish_count_, 2U);

  owner.mismatch_configuration_ = true;
  const auto drifted = publisher.PublishAndReadBack(
      acceptance, expected.sealed(), configuration);
  ASSERT_FALSE(drifted.ok());
  EXPECT_EQ(drifted.error().details.at("reason"),
            "route_publication_kernel_readback_mismatch");
  owner.mismatch_configuration_ = false;
  owner.mismatch_admission_ = true;
  const auto reopened = publisher.PublishAndReadBack(
      acceptance, expected.sealed(), configuration);
  ASSERT_FALSE(reopened.ok());
  EXPECT_EQ(reopened.error().details.at("reason"),
            "route_publication_kernel_readback_mismatch");
}

TEST(ResolvedStopBasePlanResolverTest,
     RouteResolutionSelectsOnlyItsDomainSourceProjections) {
  ScopedTrustedRoot root;
  const auto retained = test::
      materialize_lite6_stop_capable_native_motion_retained_consumer_fixture(
          root.path());
  const FilesystemVerifiedRetainedProductArtifactSource source(root.path());
  const auto runtime = make_runtime_context(*retained.bundle);
  const auto expected = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, runtime.desired_configuration,
      runtime.source_runtime_unit_id, runtime.source_runtime_epoch);
  const auto acceptance = make_route_acceptance(runtime, expected);
  const auto observation = make_desired_controller_observation(expected);
  const auto reconciliation = core::ReconcileControllerLifecycle(
      expected.value(), observation, observation.received_at_ns);
  ASSERT_TRUE(reconciliation.ok()) << reconciliation.error().message;
  ASSERT_FALSE(runtime.active_configuration
                   .kinematic_resource_source_projections.empty());

  auto foreign = runtime.active_configuration
                     .kinematic_resource_source_projections.front();
  foreign.resource_id = core::ResourceId("resource.foreign.domain");
  foreign.contributing_control_bindings.front().control_binding_id =
      core::ControlBindingId("binding.foreign.domain");
  foreign.contributing_control_bindings.front().control_binding_revision =
      core::ControlBindingRevision("revision.foreign.domain");
  auto cell_projections =
      runtime.active_configuration.kinematic_resource_source_projections;
  cell_projections.push_back(std::move(foreign));

  MemoryPlanRepository repository;
  MemoryRouteConfigurationKernelPublicationOwner owner({runtime.binding});
  VerifiedRouteConfigurationPublicationPort publisher(
      *retained.bundle, source, repository, owner, std::move(cell_projections),
      [] { return 90; });
  const auto resolved = publisher.ResolveResultingActiveControlConfiguration(
      acceptance, expected.sealed(), observation, reconciliation.value(),
      core::RouteSelectionEffectDTO::kKnownNoEffect);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_EQ(resolved.value().kinematic_resource_source_projections,
            runtime.active_configuration.kinematic_resource_source_projections);

  auto mixed =
      runtime.active_configuration.kinematic_resource_source_projections;
  auto foreign_source = mixed.front().contributing_control_bindings.front();
  foreign_source.control_binding_id =
      core::ControlBindingId("binding.foreign.domain");
  foreign_source.control_binding_revision =
      core::ControlBindingRevision("revision.foreign.domain");
  mixed.front().contributing_control_bindings.push_back(
      std::move(foreign_source));
  std::sort(
      mixed.front().contributing_control_bindings.begin(),
      mixed.front().contributing_control_bindings.end(),
      [](const auto &lhs, const auto &rhs) {
        return std::tie(lhs.control_binding_id, lhs.control_binding_revision) <
               std::tie(rhs.control_binding_id, rhs.control_binding_revision);
      });
  VerifiedRouteConfigurationPublicationPort mixed_publisher(
      *retained.bundle, source, repository, owner, std::move(mixed),
      [] { return 91; });
  const auto rejected =
      mixed_publisher.ResolveResultingActiveControlConfiguration(
          acceptance, expected.sealed(), observation, reconciliation.value(),
          core::RouteSelectionEffectDTO::kKnownNoEffect);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "route_publication_cross_domain_source_projection");
}

TEST(ResolvedStopBasePlanResolverTest,
     PublishesUnreconciledTruthClosedAndRejectsUnsafeReceiptTime) {
  ScopedTrustedRoot root;
  const auto retained = test::
      materialize_lite6_stop_capable_native_motion_retained_consumer_fixture(
          root.path());
  const FilesystemVerifiedRetainedProductArtifactSource source(root.path());
  const auto runtime = make_runtime_context(*retained.bundle);
  const auto expected = load_verified_controller_lifecycle_expected_domain(
      *retained.bundle, source, runtime.desired_configuration,
      runtime.source_runtime_unit_id, runtime.source_runtime_epoch);
  const auto acceptance = make_route_acceptance(runtime, expected);
  auto configuration = make_route_configuration(runtime);
  configuration.state =
      core::ActiveControlConfigurationStateDTO::kExternalOrUnknownUnreconciled;
  MemoryPlanRepository repository;
  MemoryRouteConfigurationKernelPublicationOwner owner({runtime.binding});
  VerifiedRouteConfigurationPublicationPort publisher(
      *retained.bundle, source, repository, owner,
      runtime.active_configuration.kinematic_resource_source_projections,
      [] { return 80; });

  const auto result = publisher.PublishAndReadBack(
      acceptance, expected.sealed(), configuration);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_TRUE(result.value().readiness_rows.empty());
  EXPECT_FALSE(result.value().operation_admission_open);
  EXPECT_EQ(owner.read_count_, 0U);
  ASSERT_EQ(owner.publish_count_, 1U);
  EXPECT_TRUE(owner.published_participants_.empty());

  VerifiedRouteConfigurationPublicationPort unsafe_clock(
      *retained.bundle, source, repository, owner,
      runtime.active_configuration.kinematic_resource_source_projections,
      [] { return 9'007'199'254'740'992LL; });
  const auto unsafe = unsafe_clock.PublishAndReadBack(
      acceptance, expected.sealed(), configuration);
  ASSERT_FALSE(unsafe.ok());
  EXPECT_EQ(unsafe.error().details.at("reason"),
            "route_publication_time_invalid");
}

} // namespace
} // namespace manyforge::deployment
