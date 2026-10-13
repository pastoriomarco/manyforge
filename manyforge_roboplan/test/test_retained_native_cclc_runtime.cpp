#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iterator>
#include <limits>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_core/build_owned_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/retained_native_cclc_runtime.hpp>
#include <manyforge_roboplan/retained_native_cell_command_readiness_port.hpp>
#include <manyforge_roboplan/retained_native_cell_runtime.hpp>
#include <manyforge_roboplan/retained_native_brand_lifecycle_port.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <stop_token>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

[[nodiscard]] std::string Sha(const char value);

TEST(RetainedNativeCCLCRuntime, NativeAdapterOwnsProviderReadinessButNeverCommandPermission) {
  using State = core::ComponentLifecycleStateDTO;
  using Intent = core::ComponentLifecycleIntentKindDTO;
  const core::ProviderManifestId manifest_id("manyforge.provider.native_control");
  const core::ProviderManifestRevision manifest_revision(Sha('1'));
  const core::LifecyclePredicateDTO active{
      {State::kActive},
      {{core::LifecycleFactId("native.test.ready"),
        {core::LifecycleFactValueKindDTO::kBoolean, true, std::nullopt, std::nullopt}}}};
  const core::LifecyclePredicateDTO prepared{{State::kPrepared}, {}};
  const core::LifecyclePredicateDTO disabled{{State::kDisabled}, {}};
  const auto catalog = core::MakeBrandLifecyclePlanCatalog(
      manifest_id, manifest_revision,
      {{Intent::kPrepare, State::kPrepared, std::nullopt, prepared, 10'000,
        {{core::LifecyclePrimitiveId("native.control.prepare"), disabled, prepared}}},
       {Intent::kEnable, State::kActive, std::nullopt, active, 10'000,
        {{core::LifecyclePrimitiveId("native.control.enable"), prepared, active}}},
       {Intent::kDisable, State::kDisabled, std::nullopt, disabled, 10'000,
        {{core::LifecyclePrimitiveId("native.control.disable"), active, disabled}}}});
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  const core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
      core::ControlDomainId("domain.1"), core::ControlDomainRevision("domain.revision.1"),
      core::ProviderInstanceId("provider.1"), manifest_id, manifest_revision,
      core::RuntimeUnitId("runtime.1"), core::RuntimeEpoch("epoch.1"),
      catalog.value().catalog_digest};
  core::FakeClock clock;
  clock.set(100);
  // Route readiness is observed only; the adapter has no command-gate seam.
  bool route_open = true;
  bool engine_available = true;
  const auto port = RetainedNativeBrandLifecyclePort::Create(
      occurrence, catalog.value(), clock, 1000,
      [&] {
        return engine_available ? core::Result<void>::Ok()
                                : core::Result<void>::Err(
                                      {"native_engine_unavailable", "test source missing", {}});
      },
      [&] { return core::Result<bool>::Ok(route_open); }, State::kActive);
  ASSERT_TRUE(port.ok()) << port.error().message;
  const core::BrandLifecycleObservationQueryDTO query{
      core::LifecycleTransactionId("lifecycle.1"), core::LifecycleTransactionGeneration(1U),
      core::ControllableComponentId("component.1"), occurrence.control_domain_id,
      occurrence.control_domain_revision, occurrence.provider_instance_id,
      occurrence.provider_manifest_id, occurrence.provider_manifest_revision,
      occurrence.provider_runtime_unit_id, occurrence.provider_runtime_epoch};
  const auto dispatch = [&](const char* primitive, std::uint64_t generation) {
    const core::BrandLifecyclePrimitiveIntentDTO request{
        query.lifecycle_transaction_id, query.lifecycle_transaction_generation,
        query.controllable_component_id, query.control_domain_id,
        query.control_domain_revision, query.provider_instance_id,
        query.provider_manifest_id, query.provider_manifest_revision,
        query.provider_runtime_unit_id, query.provider_runtime_epoch,
        core::LifecyclePrimitiveId(primitive),
        core::LifecyclePrimitiveAttemptId("attempt.1"), generation, 10'000};
    return port.value()->Dispatch(request, std::stop_token{});
  };

  for (int read = 0; read < 3; ++read) {
    const auto health = port.value()->ReadLifecycleHealth(occurrence.control_domain_id, clock.now());
    ASSERT_TRUE(health.ok()) << health.error().message;
    EXPECT_EQ(health.value().observed_state, State::kActive);
    ASSERT_EQ(health.value().facts.size(), 1U);
    EXPECT_EQ(health.value().facts.front().lifecycle_fact_id,
              core::LifecycleFactId("native.test.ready"));
    EXPECT_EQ(health.value().facts.front().value.boolean_value, true);
  }
  EXPECT_FALSE(port.value()->ReadLifecycleHealth(core::ControlDomainId("foreign.domain"),
                                                clock.now()).ok());
  EXPECT_FALSE(port.value()->ReadLifecycleHealth(occurrence.control_domain_id, -1).ok());
  const auto first = port.value()->Observe(query);
  ASSERT_TRUE(first.ok()) << first.error().message;
  EXPECT_EQ(first.value().observed_state, State::kActive);
  EXPECT_EQ(first.value().source_sequence, 1U);
  const auto pure = port.value()->ReadLifecycleHealth(occurrence.control_domain_id, clock.now());
  ASSERT_TRUE(pure.ok());
  EXPECT_EQ(pure.value().observed_state, first.value().observed_state);
  EXPECT_EQ(pure.value().facts, first.value().facts);
  const auto after_reads = port.value()->Observe(query);
  ASSERT_TRUE(after_reads.ok());
  EXPECT_EQ(after_reads.value().source_sequence, first.value().source_sequence + 1U);
  EXPECT_EQ(after_reads.value().observation_generation, first.value().observation_generation);

  engine_available = false;
  const auto missing_engine = port.value()->ReadLifecycleHealth(occurrence.control_domain_id,
                                                               clock.now());
  ASSERT_FALSE(missing_engine.ok());
  EXPECT_EQ(missing_engine.error().code, "native_engine_unavailable");
  EXPECT_FALSE(port.value()->Observe(query).ok());
  engine_available = true;
  const auto stale = dispatch("native.control.disable", 2U);
  EXPECT_EQ(stale.dispatch_state, core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);

  // Disable and Prepare change only the provider's readiness state; the
  // route stays installed and its readiness is untouched.
  const auto off = dispatch("native.control.disable", first.value().observation_generation);
  EXPECT_EQ(off.dispatch_state, core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown);
  EXPECT_TRUE(route_open);
  const auto disabled_observation = port.value()->Observe(query);
  ASSERT_TRUE(disabled_observation.ok()) << disabled_observation.error().message;
  EXPECT_EQ(disabled_observation.value().observed_state, State::kDisabled);

  const auto prepared_result = dispatch("native.control.prepare",
                                        disabled_observation.value().observation_generation);
  EXPECT_EQ(prepared_result.dispatch_state,
            core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown);
  const auto prepared_observation = port.value()->Observe(query);
  ASSERT_TRUE(prepared_observation.ok()) << prepared_observation.error().message;
  EXPECT_EQ(prepared_observation.value().observed_state, State::kPrepared);

  // Enable requires a current native route and reports honestly otherwise.
  route_open = false;
  const auto refused = dispatch("native.control.enable",
                                prepared_observation.value().observation_generation);
  EXPECT_EQ(refused.dispatch_state, core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  EXPECT_EQ(refused.effect, core::LifecyclePrimitiveEffectDTO::kNone);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->code, "native_lifecycle_route_not_current");
  const auto still_prepared = port.value()->Observe(query);
  ASSERT_TRUE(still_prepared.ok()) << still_prepared.error().message;
  EXPECT_EQ(still_prepared.value().observed_state, State::kPrepared);
  EXPECT_EQ(still_prepared.value().observation_generation,
            prepared_observation.value().observation_generation);

  route_open = true;
  const auto on =
      dispatch("native.control.enable", still_prepared.value().observation_generation);
  EXPECT_EQ(on.dispatch_state, core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown);
  const auto active_observation = port.value()->Observe(query);
  ASSERT_TRUE(active_observation.ok()) << active_observation.error().message;
  EXPECT_EQ(active_observation.value().observed_state, State::kActive);

  // An Active provider without a current route is not an observation.
  route_open = false;
  EXPECT_FALSE(port.value()->Observe(query).ok());
  const auto no_route = port.value()->ReadLifecycleHealth(occurrence.control_domain_id, clock.now());
  ASSERT_FALSE(no_route.ok());
  EXPECT_EQ(no_route.error().code, "native_brand_lifecycle_route_state_changed");

  const auto strict_restart = RetainedNativeBrandLifecyclePort::Create(
      occurrence, catalog.value(), clock, 1000,
      [] { return core::Result<void>::Ok(); },
      [&] { return core::Result<bool>::Ok(route_open); }, State::kActive);
  EXPECT_FALSE(strict_restart.ok());
  const auto recovering = RetainedNativeBrandLifecyclePort::Create(
      occurrence, catalog.value(), clock, 1000,
      [] { return core::Result<void>::Ok(); },
      [&] { return core::Result<bool>::Ok(route_open); }, State::kActive, true);
  ASSERT_TRUE(recovering.ok()) << recovering.error().message;
  // Construction during closed route recovery is not an Active observation.
  EXPECT_FALSE(recovering.value()->Observe(query).ok());
  route_open = true;
  const auto recovered = recovering.value()->Observe(query);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_EQ(recovered.value().observed_state, State::kActive);
}

[[nodiscard]] std::string Sha(const char value) {
  return std::string("sha256:") + std::string(64U, value);
}

class ScopedTrustedRoot final {
public:
  ScopedTrustedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-native-cclc-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::runtime_error("cannot create native CCLC test root");
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

template <typename Range, typename Identity, typename Projection>
[[nodiscard]] const typename Range::value_type &
require_one(const Range &values, const Identity &identity,
            Projection projection, std::string_view description) {
  const auto found =
      std::find_if(values.begin(), values.end(), [&](const auto &value) {
        return projection(value) == identity;
      });
  if (found == values.end() ||
      std::find_if(std::next(found), values.end(), [&](const auto &value) {
        return projection(value) == identity;
      }) != values.end()) {
    throw std::runtime_error("fixture has no sole " + std::string(description));
  }
  return *found;
}

[[nodiscard]] std::shared_ptr<const deployment::RetainedBundle>
load_fixture_bundle(const std::filesystem::path &fixture_root) {
  const auto values_root =
      fixture_root / "retained-package" / "retained-bundles" / "sha256";
  std::optional<core::RetainedBundleId> id;
  for (const auto &entry : std::filesystem::directory_iterator(values_root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") {
      continue;
    }
    if (id.has_value()) {
      throw std::runtime_error("fixture has multiple retained bundles");
    }
    id = core::RetainedBundleId("sha256:" + entry.path().stem().string());
  }
  if (!id.has_value()) {
    throw std::runtime_error("fixture has no retained bundle");
  }
  const deployment::FilesystemVerifiedRetainedBundleSource source(
      fixture_root / "retained-package");
  auto bundle = source.get_verified(*id);
  if (bundle == nullptr) {
    throw std::runtime_error("fixture retained bundle failed verification");
  }
  return bundle;
}

struct RuntimeContext final {
  core::RuntimeParticipantBinding binding;
  const core::ControllableComponentDTO *component;
  const core::ControlDomainDTO *domain;
  const core::ControlBindingDTO *control_binding;
  const core::DesiredControlConfigurationDTO *desired;
  core::RuntimeEpoch provider_epoch;
};

[[nodiscard]] RuntimeContext
make_runtime_context(
    const deployment::RetainedBundle &bundle,
    const core::CapabilityId &capability =
        core::CapabilityId("execution.trajectory.execute")) {
  const auto &graph = bundle.value().cell_graph;
  const bool motion = capability == core::CapabilityId("execution.trajectory.execute");
  const auto &binding = require_one(
      graph.control_bindings, capability,
      [](const auto &value) { return value.capability_id; },
      "selected control binding");
  const auto &component = require_one(
      graph.controllable_components, binding.controllable_component_id,
      [](const auto &value) { return value.controllable_component_id; },
      "controllable component");
  const auto &domain = require_one(
      graph.control_domains, binding.control_domain_id,
      [](const auto &value) { return value.control_domain_id; },
      "control domain");
  const auto &desired = require_one(
      bundle.value().desired_control_configurations, binding.control_domain_id,
      [](const auto &value) { return value.control_domain_id; },
      "desired control configuration");
  const auto &route = require_one(
      desired.binding_routes, binding.control_binding_id,
      [](const auto &value) { return value.control_binding_id; },
      "desired binding route");

  std::vector<core::ProgramModelRoleBindingDTO> models;
  for (const auto &row : component.model_role_bindings) {
    const auto &model = require_one(
        graph.model_instances, row.model_instance_id,
        [](const auto &value) { return value.model_instance_id; },
        "model instance");
    models.push_back(
        {row.model_role_id, row.model_instance_id, model.model_revision});
  }
  if (models.empty()) {
    throw std::runtime_error("fixture component has no model binding");
  }
  std::vector<core::RuntimeResourceBindingDTO> resources;
  resources.reserve(binding.resource_bindings.size());
  for (std::size_t index = 0U; index < binding.resource_bindings.size();
       ++index) {
    const auto &resource = binding.resource_bindings[index];
    const auto symbol = "joint_" + std::to_string(index + 1U);
    resources.push_back({resource.resource_role_id, resource.resource_id,
                         symbol, symbol, motion ? core::CoordinateKind::kAngular
                                               : core::CoordinateKind::kLinear});
  }
  std::sort(resources.begin(), resources.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.resource_id < rhs.resource_id;
            });
  const auto &primary_model = require_one(
      graph.model_instances,
      component.model_role_bindings.front().model_instance_id,
      [](const auto &value) { return value.model_instance_id; },
      "primary model instance");
  std::vector<std::string> expanded_joints;
  expanded_joints.reserve(resources.size());
  for (const auto &resource : resources) {
    expanded_joints.push_back(resource.compiled_resource_name);
  }
  std::vector<core::RuntimeModelGroupBindingDTO> model_groups{
      {core::ModelGroupRoleId("arm"), primary_model.model_instance_id,
       "native_cclc_arm", std::move(expanded_joints)}};
  std::vector<core::ProgramFrameRoleBindingDTO> frames{
      {core::FrameRoleId("base"), primary_model.model_instance_id,
       primary_model.model_revision, "base_link"},
      {core::FrameRoleId("tcp"), primary_model.model_instance_id,
       primary_model.model_revision, "tool0"}};

  const core::RuntimeEpoch runtime_epoch(
      "epoch." + binding.provider_runtime_unit_id.value());
  const core::RuntimeEpoch provider_epoch("provider.epoch.native-cclc.1");
  std::vector<core::RuntimeUnitEpochBindingDTO> runtime_unit_epochs{
      {binding.provider_runtime_unit_id, runtime_epoch}};
  std::vector<core::RuntimeCapabilityProviderPathDTO> planner_paths;
  if (motion) {
    const core::CapabilityId joint_planning_capability_id(
        "planning.motion.joint_goal");
    const auto &planner = require_one(
        bundle.value().capability_provider_bindings, joint_planning_capability_id,
        [](const auto &value) { return value.capability_id; },
        "joint planner provider");
    std::set<core::RuntimeUnitId> planner_runtime_units;
    for (const auto &endpoint : planner.endpoint_bindings) {
      if (endpoint.source_kind !=
          core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *runtime_unit =
          std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
      if (runtime_unit == nullptr) {
        throw std::runtime_error("planner runtime endpoint is malformed");
      }
      planner_runtime_units.insert(*runtime_unit);
    }
    if (planner_runtime_units.size() != 1U) {
      throw std::runtime_error("planner has no sole runtime unit");
    }
    const core::RuntimeCapabilityProviderPathDTO planner_path{
        planner.capability_id,
        planner.capability_definition_revision,
        planner.capability_provider_binding_id,
        planner.capability_provider_binding_revision,
        planner.provider_instance_id,
        planner.provider_manifest_id,
        planner.provider_manifest_revision,
        *planner_runtime_units.begin(),
        core::RuntimeEpoch("provider.epoch.native-planner.1")};
    planner_paths.push_back(planner_path);
    if (*planner_runtime_units.begin() != binding.provider_runtime_unit_id) {
      runtime_unit_epochs.push_back(
          {*planner_runtime_units.begin(), runtime_epoch});
      std::sort(runtime_unit_epochs.begin(), runtime_unit_epochs.end(),
                [](const auto &lhs, const auto &rhs) {
                  return lhs.runtime_unit_id < rhs.runtime_unit_id;
                });
    }
  }
  core::RuntimeParticipantBindingDTO candidate{
      core::ProgramId("native.cclc.readiness.program"),
      core::ProgramRevision(Sha('0')),
      core::ProgramBindingId(Sha('b')),
      bundle.retained_bundle_id(),
      graph.graph_revision,
      core::ComponentRoleId("robot"),
      core::TargetRoleId("arm"),
      component.controllable_component_id,
      component.component_type_id,
      component.component_type_revision,
      binding.command_target_id,
      binding.capability_id,
      binding.capability_definition_revision,
      std::nullopt,
      std::move(models),
      std::move(model_groups),
      std::move(frames),
      std::move(resources),
      {},
      core::RuntimeControlPathDTO{
          domain.control_domain_id,
          domain.control_domain_revision,
          binding.control_binding_id,
          binding.control_binding_revision,
          binding.control_interface_contract_id,
          binding.control_interface_revision,
          binding.controller_topology_id,
          binding.topology_revision,
          route.control_route_id,
          desired.activation_set_id,
          core::ActiveControlConfigurationId("active.native.bootstrap"),
          core::ActiveControlConfigurationGeneration(1U),
          binding.provider_instance_id,
          binding.provider_manifest_id,
          binding.provider_manifest_revision,
          binding.provider_runtime_unit_id,
          provider_epoch,
          binding.state_authority_id,
          binding.state_authority_class,
          binding.stop_evidence_policy_id,
          binding.stop_evidence_policy_revision,
          binding.containment_granularity,
          std::nullopt},
      std::nullopt,
      std::move(planner_paths),
      std::move(runtime_unit_epochs),
      {}};
  auto sealed = core::verify_runtime_participant_binding(std::move(candidate));
  if (!sealed.ok()) {
    throw std::runtime_error(sealed.error().message);
  }
  return {std::move(sealed.value()),
          &component,
          &domain,
          &binding,
          &desired,
          provider_epoch};
}

[[nodiscard]] core::RuntimeRouteConfigurationReadbackDTO
make_retained_plan_readback(const RuntimeContext &runtime) {
  const auto &desired = *runtime.desired;
  const auto &path = *runtime.binding.value().control_path;
  core::ActiveControlConfigurationDTO active{
      core::ActiveControlConfigurationId("existing.retained.route"),
      desired.control_domain_id, desired.control_domain_revision, {},
      desired.activation_set_id, desired.controller_topology_id,
      desired.topology_revision, path.provider_runtime_unit_id,
      path.provider_runtime_epoch, 9U, {},
      core::ActiveControlConfigurationGeneration(17U),
      core::ActiveControlConfigurationStateDTO::kReconciled};
  for (const auto &route : desired.binding_routes)
    active.binding_routes.push_back({route.control_binding_id,
                                    route.control_binding_revision,
                                    route.control_route_id});
  return {std::move(active), {}, true};
}

TEST(RetainedNativeCCLCRuntime,
     VerifiedEmptyCellRoutePortRefusesAnActualPhysicalConfiguration) {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource empty_source(
      std::filesystem::path(MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE_ROOT));
  const auto empty = empty_source.load_verified();
  auto port = MakeVerifiedEmptyCellRoutePublicationOwner(empty.retained_bundle());
  ASSERT_TRUE(port.ok()) << port.error().message;

  const auto populated = load_fixture_bundle(MANYFORGE_CCLC_READINESS_FIXTURE_ROOT);
  const auto runtime = make_runtime_context(*populated);
  const auto physical = make_retained_plan_readback(runtime).active_configuration;
  const auto read = port.value()->ReadCurrentPhysicalParticipants(physical);
  ASSERT_FALSE(read.ok());
  EXPECT_EQ(read.error().code, "empty_cell_physical_route_unavailable");
  EXPECT_EQ(read.error().details.at("effect"), "not_started");
  const auto publish = port.value()->PublishAndReadBackRouteConfiguration(physical, {});
  ASSERT_FALSE(publish.ok());
  EXPECT_EQ(publish.error().code, "empty_cell_physical_route_unavailable");
  EXPECT_FALSE(MakeVerifiedEmptyCellRoutePublicationOwner(*populated).ok());
}

TEST(RetainedNativeApplyPlanning, RetainedRouteIsExactCurrentFactBeforeAnyEffects) {
  const auto bundle = load_fixture_bundle(MANYFORGE_CCLC_READINESS_FIXTURE_ROOT);
  const auto runtime = make_runtime_context(*bundle);
  const auto delta = deployment::derive_apply_component_delta(*bundle, *bundle);
  const std::vector<core::RuntimeParticipantBinding> bindings{runtime.binding};
  const std::vector<core::RuntimeRouteConfigurationReadbackDTO> current{
      make_retained_plan_readback(runtime)};
  const core::NamedApplyPublicationTransactionDTO apply{
      core::ApplyId("pre-effect.retained"), core::ApplyGeneration(3U)};
  const auto result = DeriveRetainedNativeApplyPlanRequest(
      apply, *bundle, bundle.get(), delta, bindings, current);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().apply, apply);
  EXPECT_EQ(result.value().candidate_retained_bundle_id, bundle->retained_bundle_id());
  EXPECT_EQ(result.value().candidate_graph_revision, bundle->value().cell_graph.graph_revision);
  EXPECT_EQ(result.value().components,
            std::vector<core::ControllableComponentId>{runtime.component->controllable_component_id});
  EXPECT_EQ(result.value().retained_components, result.value().components);
  EXPECT_TRUE(result.value().removed_components.empty());
  EXPECT_TRUE(result.value().removed_domains.empty());
  ASSERT_EQ(result.value().compiled_routes.size(), 1U);
  EXPECT_EQ(result.value().compiled_routes.front().control_domain_id, runtime.domain->control_domain_id);
  EXPECT_EQ(result.value().compiled_routes.front().generation,
            runtime.binding.value().control_path->active_control_configuration_generation);
  EXPECT_EQ(result.value().retained_routes,
            std::vector<core::ActiveControlConfigurationDTO>{current.front().active_configuration});
  EXPECT_NE(result.value().retained_routes.front().active_control_configuration_generation,
            result.value().compiled_routes.front().generation);
  EXPECT_EQ(result.value().changed_component_state, core::ComponentLifecycleStateDTO::kActive);
  // The helper has no Kernel, journal, controller, readiness or install port.
  EXPECT_EQ(bindings.front().value(), runtime.binding.value());
  EXPECT_EQ(current.front(), make_retained_plan_readback(runtime));
}

TEST(RetainedNativeApplyPlanning, InitialAdditionContainsOnlyPlannedCandidateValues) {
  const auto bundle = load_fixture_bundle(MANYFORGE_CCLC_READINESS_FIXTURE_ROOT);
  const auto runtime = make_runtime_context(*bundle);
  auto delta = deployment::derive_apply_component_delta(*bundle, *bundle);
  for (auto &row : delta.components) {
    row.disposition = deployment::ApplyComponentDisposition::kAdd;
    row.predecessor_runtime_identity.reset();
    row.predecessor_domains.clear();
  }
  ASSERT_TRUE(delta.valid());
  const std::vector<core::RuntimeParticipantBinding> bindings{runtime.binding};
  const auto result = DeriveRetainedNativeApplyPlanRequest(
      {core::ApplyId("pre-effect.add"), core::ApplyGeneration(4U)},
      *bundle, nullptr, delta, bindings, {}, core::ComponentLifecycleStateDTO::kDeclared);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().components,
            std::vector<core::ControllableComponentId>{runtime.component->controllable_component_id});
  EXPECT_TRUE(result.value().retained_components.empty());
  EXPECT_TRUE(result.value().retained_routes.empty());
  EXPECT_TRUE(result.value().removed_components.empty());
  EXPECT_TRUE(result.value().removed_domains.empty());
  ASSERT_EQ(result.value().compiled_routes.size(), 1U);
  EXPECT_EQ(result.value().compiled_routes.front().generation,
            runtime.binding.value().control_path->active_control_configuration_generation);
  EXPECT_EQ(result.value().changed_component_state, core::ComponentLifecycleStateDTO::kDeclared);
}

TEST(RetainedNativeApplyPlanning, ReplaceMembershipPrecedesOutgoingAvailabilityLoss) {
  const auto predecessor = load_fixture_bundle(MANYFORGE_I6_NATIVE_FIXTURE_ROOT);
  const auto candidate = load_fixture_bundle(MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT);
  ASSERT_EQ(predecessor->value().cell_graph.cell_id, candidate->value().cell_graph.cell_id);
  ASSERT_TRUE(deployment::verify_retained_bundle_identity(*predecessor));
  ASSERT_TRUE(deployment::verify_retained_bundle_identity(*candidate));
  const auto arm = make_runtime_context(*candidate);
  const auto gripper = make_runtime_context(*candidate, core::CapabilityId("tool.gripper.command"));
  const auto delta = deployment::derive_apply_component_delta(*predecessor, *candidate);
  std::vector<core::ControllableComponentId> replaced;
  for (const auto &row : delta.components) {
    ASSERT_NE(row.disposition, deployment::ApplyComponentDisposition::kRemove);
    if (row.disposition == deployment::ApplyComponentDisposition::kReplace)
      replaced.push_back(row.controllable_component_id);
  }
  ASSERT_EQ(replaced, std::vector<core::ControllableComponentId>{arm.component->controllable_component_id});
  const std::vector<core::RuntimeParticipantBinding> bindings{arm.binding, gripper.binding};
  const auto result = DeriveRetainedNativeApplyPlanRequest(
      {core::ApplyId("pre-effect.replace"), core::ApplyGeneration(5U)},
      *candidate, predecessor.get(), delta, bindings, {},
      core::ComponentLifecycleStateDTO::kDeclared);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().components, replaced);
  EXPECT_TRUE(result.value().removed_components.empty());
  EXPECT_TRUE(result.value().removed_domains.empty());
  EXPECT_TRUE(result.value().retained_components.empty());
  EXPECT_TRUE(result.value().retained_routes.empty());
  ASSERT_EQ(result.value().compiled_routes.size(), 2U);
  for (const auto &route : result.value().compiled_routes) {
    const auto physical = std::find_if(bindings.begin(), bindings.end(), [&](const auto &binding) {
      return binding.value().control_path->control_domain_id == route.control_domain_id;
    });
    ASSERT_NE(physical, bindings.end());
    EXPECT_EQ(route.generation, physical->value().control_path->active_control_configuration_generation);
  }
  EXPECT_EQ(result.value().candidate_retained_bundle_id, candidate->retained_bundle_id());
  EXPECT_EQ(result.value().changed_component_state, core::ComponentLifecycleStateDTO::kDeclared);
  // No installed composition or retiring controller is needed to make this
  // exact replacement membership visible. This pair has no removal coverage.
}

TEST(RetainedNativeApplyPlanning, ForeignCandidateDomainGenerationAndReadbackJoinsRefuse) {
  const auto bundle = load_fixture_bundle(MANYFORGE_CCLC_READINESS_FIXTURE_ROOT);
  const auto runtime = make_runtime_context(*bundle);
  const auto delta = deployment::derive_apply_component_delta(*bundle, *bundle);
  const core::NamedApplyPublicationTransactionDTO apply{
      core::ApplyId("pre-effect.foreign"), core::ApplyGeneration(6U)};
  const std::vector<core::RuntimeParticipantBinding> bindings{runtime.binding};
  const std::vector<core::RuntimeRouteConfigurationReadbackDTO> current{
      make_retained_plan_readback(runtime)};
  const auto reject_binding = [&](const auto &mutate) {
    auto value = runtime.binding.value();
    mutate(value);
    auto verified = core::verify_runtime_participant_binding(std::move(value));
    ASSERT_TRUE(verified.ok()) << verified.error().message;
    const std::vector<core::RuntimeParticipantBinding> foreign{std::move(verified.value())};
    EXPECT_FALSE(DeriveRetainedNativeApplyPlanRequest(apply, *bundle, bundle.get(),
                                                     delta, foreign, current).ok());
  };
  reject_binding([](auto &value) { value.retained_bundle_id = core::RetainedBundleId(Sha('1')); });
  reject_binding([](auto &value) { value.graph_revision = core::GraphRevision(Sha('2')); });
  reject_binding([](auto &value) { value.control_path->control_domain_revision = core::ControlDomainRevision(Sha('3')); });
  reject_binding([](auto &value) {
    value.control_path->active_control_configuration_generation =
        core::ActiveControlConfigurationGeneration(std::numeric_limits<std::uint64_t>::max());
  });
  EXPECT_FALSE(DeriveRetainedNativeApplyPlanRequest(apply, *bundle, bundle.get(),
                                                   delta, {}, current).ok());
  auto duplicate = bindings;
  duplicate.push_back(runtime.binding);
  EXPECT_FALSE(DeriveRetainedNativeApplyPlanRequest(apply, *bundle, bundle.get(),
                                                   delta, duplicate, current).ok());
  const auto reject_current = [&](const auto &mutate) {
    auto foreign = current;
    mutate(foreign.front());
    EXPECT_FALSE(DeriveRetainedNativeApplyPlanRequest(apply, *bundle, bundle.get(),
                                                     delta, bindings, foreign).ok());
  };
  reject_current([](auto &value) { value.operation_admission_open = false; });
  reject_current([](auto &value) { value.active_configuration.control_domain_id = core::ControlDomainId("foreign-domain"); });
  reject_current([](auto &value) { value.active_configuration.binding_routes.front().control_route_id = core::ControlRouteId("foreign-route"); });
  reject_current([](auto &value) { value.active_configuration.active_control_configuration_generation = core::ActiveControlConfigurationGeneration(0U); });
  auto foreign_delta = delta;
  foreign_delta.shared_scene_projection_changed = !foreign_delta.shared_scene_projection_changed;
  EXPECT_FALSE(DeriveRetainedNativeApplyPlanRequest(apply, *bundle, bundle.get(),
                                                   foreign_delta, bindings, current).ok());
  EXPECT_FALSE(DeriveRetainedNativeApplyPlanRequest(apply, *bundle, bundle.get(),
      delta, bindings, current, core::ComponentLifecycleStateDTO::kDisabled).ok());
}

[[nodiscard]] core::BrandLifecyclePlanCatalogDTO
make_prepare_catalog(const core::ControlBindingDTO &binding) {
  const core::LifecyclePredicateDTO declared{
      {core::ComponentLifecycleStateDTO::kDeclared}, {}};
  const core::LifecyclePredicateDTO prepared{
      {core::ComponentLifecycleStateDTO::kPrepared}, {}};
  const core::LifecyclePredicateDTO active{
      {core::ComponentLifecycleStateDTO::kActive}, {}};
  auto result = core::MakeBrandLifecyclePlanCatalog(
      binding.provider_manifest_id, binding.provider_manifest_revision,
      {{core::ComponentLifecycleIntentKindDTO::kPrepare,
        core::ComponentLifecycleStateDTO::kPrepared,
        std::nullopt,
        prepared,
        1'000'000'000LL,
        {{core::LifecyclePrimitiveId("prepare"), declared, prepared}}},
       {core::ComponentLifecycleIntentKindDTO::kEnable,
        core::ComponentLifecycleStateDTO::kActive,
        std::nullopt,
        active,
        1'000'000'000LL,
        {{core::LifecyclePrimitiveId("enable"), prepared, active}}}});
  if (!result.ok()) {
    throw std::runtime_error(result.error().message);
  }
  return std::move(result.value());
}

class FixedCatalogSource final : public core::ICCLCLifecyclePlanCatalogSource {
public:
  explicit FixedCatalogSource(core::BrandLifecyclePlanCatalogDTO catalog)
      : catalog_(std::move(catalog)) {}

  [[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
  LoadExactLifecyclePlanCatalog(
      const core::ProviderManifestId &manifest_id,
      const core::ProviderManifestRevision &manifest_revision,
      const core::ContentDigest &digest) const override {
    if (manifest_id != catalog_.provider_manifest_id ||
        manifest_revision != catalog_.provider_manifest_revision ||
        digest != catalog_.catalog_digest) {
      return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
          {"catalog_mismatch", "catalog key mismatch", {}});
    }
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Ok(catalog_);
  }

private:
  core::BrandLifecyclePlanCatalogDTO catalog_;
};

class PreparingBrandPort final : public core::IBrandLifecyclePort {
public:
  [[nodiscard]] core::Result<core::BrandLifecycleObservationDTO>
  Observe(const core::BrandLifecycleObservationQueryDTO &query) override {
    const auto sequence = next_sequence_.fetch_add(1U);
    const auto state = state_.load();
    return core::Result<core::BrandLifecycleObservationDTO>::Ok(
        {query.lifecycle_transaction_id,
         query.lifecycle_transaction_generation,
         query.controllable_component_id,
         query.control_domain_id,
         query.control_domain_revision,
         query.provider_instance_id,
         query.provider_manifest_id,
         query.provider_manifest_revision,
         query.provider_runtime_unit_id,
         query.provider_runtime_epoch,
         sequence,
         sequence,
         1'000,
         1'000,
         1'000'000'000,
         state,
         {}});
  }

  [[nodiscard]] core::BrandLifecyclePrimitiveResultDTO
  Dispatch(const core::BrandLifecyclePrimitiveIntentDTO &intent,
           std::stop_token cancellation) override {
    const bool prepare = intent.lifecycle_primitive_id ==
                         core::LifecyclePrimitiveId("prepare");
    const bool enable = intent.lifecycle_primitive_id ==
                        core::LifecyclePrimitiveId("enable");
    if (cancellation.stop_requested() ||
        (!prepare && !enable) ||
        (prepare && state_.load() != core::ComponentLifecycleStateDTO::kDeclared) ||
        (enable && state_.load() != core::ComponentLifecycleStateDTO::kPrepared)) {
      return {intent.lifecycle_transaction_id,
              intent.lifecycle_transaction_generation,
              intent.controllable_component_id,
              intent.control_domain_id,
              intent.control_domain_revision,
              intent.provider_instance_id,
              intent.provider_manifest_id,
              intent.provider_manifest_revision,
              intent.provider_runtime_unit_id,
              intent.provider_runtime_epoch,
              intent.lifecycle_primitive_id,
              intent.lifecycle_primitive_attempt_id,
              intent.expected_observation_generation,
              core::LifecyclePrimitiveDispatchStateDTO::kNotDispatched,
              core::LifecyclePrimitiveEffectDTO::kNone,
              std::nullopt,
              core::ErrorDTO{"prepare_cancelled", "prepare was cancelled", {}}};
    }
    state_.store(prepare ? core::ComponentLifecycleStateDTO::kPrepared
                         : core::ComponentLifecycleStateDTO::kActive);
    dispatch_count_.fetch_add(1U);
    return {intent.lifecycle_transaction_id,
            intent.lifecycle_transaction_generation,
            intent.controllable_component_id,
            intent.control_domain_id,
            intent.control_domain_revision,
            intent.provider_instance_id,
            intent.provider_manifest_id,
            intent.provider_manifest_revision,
            intent.provider_runtime_unit_id,
            intent.provider_runtime_epoch,
            intent.lifecycle_primitive_id,
            intent.lifecycle_primitive_attempt_id,
            intent.expected_observation_generation,
            core::LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown,
            core::LifecyclePrimitiveEffectDTO::kObserved,
            core::LifecyclePrimitiveKnownResultDTO::kSucceeded,
            std::nullopt};
  }

  [[nodiscard]] std::size_t dispatch_count() const noexcept {
    return dispatch_count_.load();
  }

private:
  std::atomic<std::uint64_t> next_sequence_{1U};
  std::atomic<std::size_t> dispatch_count_{0U};
  std::atomic<core::ComponentLifecycleStateDTO> state_{
      core::ComponentLifecycleStateDTO::kDeclared};
};

class KernelPorts final : public core::ICCLCKernelRouteConfigurationFencePort,
                          public core::IKernelCellAdmissionPort {
public:
  [[nodiscard]] core::AuthoringAdmissionDTO
  GetAuthoringAdmission() const override {
    return {true, core::AdmissionGeneration(7U)};
  }
  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  UpdateCellAdmissionReadiness(
      const core::CellAdmissionReadinessSnapshotDTO &) override {
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }
  [[nodiscard]] core::Result<core::CellAdmissionClaimDTO>
  AcquireCellAdmissionClaim(
      const core::CellAdmissionClaimIntentDTO &intent) override {
    claim_ = core::CellAdmissionClaimDTO{
        intent, core::CellAdmissionClaimGeneration(1U),
        intent.expected_admission_generation, 1'000};
    last_claim_ = claim_;
    return core::Result<core::CellAdmissionClaimDTO>::Ok(*claim_);
  }
  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  ReleaseCellAdmissionClaim(
      const core::CellAdmissionClaimReleaseDTO &release) override {
    release_ = release;
    claim_.reset();
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }
  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  ReconcileCellAdmissionClaims(
      const core::CellAdmissionReconcileDTO &) override {
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }
  [[nodiscard]] core::Result<core::CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const core::CellAdmissionRecoveryReceiptQueryDTO &) const override {
    return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Ok(
        {release_.has_value()
             ? core::CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact
             : (claim_.has_value()
                    ? core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact
                    : core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown),
         core::RequestOwnerEpoch("kernel.1"),
         core::AdmissionGeneration(7U),
         last_claim_,
         release_,
         std::nullopt,
         claim_.has_value() ? std::vector{*claim_}
                            : std::vector<core::CellAdmissionClaimDTO>{},
         {}});
  }

  [[nodiscard]]
  core::Result<core::RouteConfigurationFenceAdmissionSnapshotDTO>
  ObserveRouteConfigurationFenceAdmission(
      const core::RouteConfigurationFenceAdmissionQueryDTO &query)
      const override {
    return core::Result<core::RouteConfigurationFenceAdmissionSnapshotDTO>::Ok(
        {query, host_epoch(),
         projection(query.expected_retained_bundle_id,
                    query.expected_graph_revision),
         core::ActiveBundlePublicationGeneration(1U),
         core::ContentDigest(Sha('a')), 1U,
         core::ConfigurationFenceGeneration(1U),
         query.expected_active_control_configuration, 0U, 0U,
         core::ContentDigest(Sha('b'))});
  }

  [[nodiscard]] core::Result<core::RouteConfigurationFenceAcceptedDTO>
  AcquireRouteConfigurationFence(
      const core::RouteConfigurationFenceIntentDTO &intent) override {
    std::lock_guard lock(mutex_);
    accepted_ = core::RouteConfigurationFenceAcceptedDTO{
        intent.owner_request_context.request_id,
        core::RequestReplayStatusDTO::kAcceptedNew,
        intent.route_selection_transaction_id,
        intent.route_selection_transaction_generation,
        core::ConfigurationFenceId("native-cclc-route-fence"),
        core::ConfigurationFenceGeneration(
            intent.expected_fence_generation.value() + 1U),
        intent.scope,
        intent.candidate_active_control_configuration_id,
        intent.candidate_active_control_configuration_generation,
        1'000,
        core::ContentDigest(Sha('c'))};
    return core::Result<core::RouteConfigurationFenceAcceptedDTO>::Ok(
        *accepted_);
  }

  [[nodiscard]] core::Result<
      core::RouteConfigurationFenceAcquireAttemptReconciliationDTO>
  ReconcileRouteConfigurationFenceAcquireAttempt(
      const core::RouteConfigurationFenceIntentDTO &) const override {
    return unused<
        core::RouteConfigurationFenceAcquireAttemptReconciliationDTO>();
  }

  [[nodiscard]] core::Result<core::RouteConfigurationFenceStatusDTO>
  ObserveRouteConfigurationFenceStatus(
      const core::RouteConfigurationFenceStatusQueryDTO &query) const override {
    std::lock_guard lock(mutex_);
    if (!accepted_.has_value()) {
      return unused<core::RouteConfigurationFenceStatusDTO>();
    }
    const core::ExpectedActiveControlConfigurationDTO current{
        core::ExpectedActiveControlConfigurationPresenceDTO::kPresent,
        accepted_->candidate_active_control_configuration_id,
        accepted_->candidate_active_control_configuration_generation};
    core::RouteConfigurationFenceReconciliationEvidenceDTO evidence{
        host_epoch(),
        query.route_selection_transaction_id,
        query.route_selection_transaction_generation,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        1U,
        active_bundle_id_,
        active_graph_revision_,
        projection(active_bundle_id_, active_graph_revision_),
        core::ActiveBundlePublicationGeneration(1U),
        core::ContentDigest(Sha('a')),
        current,
        0U,
        0U,
        core::ContentDigest(Sha('d'))};
    return core::Result<core::RouteConfigurationFenceStatusDTO>::Ok(
        {query,
         std::move(evidence),
         core::RouteConfigurationFenceStateDTO::kHeld,
         true,
         std::nullopt,
         {},
         core::ContentDigest(Sha('e')),
         std::nullopt});
  }

  [[nodiscard]] core::Result<core::RouteConfigurationFenceReleasedDTO>
  ReleaseRouteConfigurationFence(
      const core::RouteConfigurationFenceReleaseRequestDTO &request) override {
    core::RouteConfigurationFenceReleasedDTO receipt{
        request,
        core::RequestReplayStatusDTO::kAcceptedNew,
        1'000,
        request.accepted_fence.configuration_fence_id,
        request.accepted_fence.configuration_fence_generation,
        core::ContentDigest(Sha('f')),
        core::ContentDigest(Sha('1'))};
    return core::Result<core::RouteConfigurationFenceReleasedDTO>::Ok(
        std::move(receipt));
  }

  [[nodiscard]] core::Result<
      core::RouteConfigurationFenceReleaseAttemptReconciliationDTO>
  ReconcileRouteConfigurationFenceReleaseAttempt(
      const core::RouteConfigurationFenceReleaseRequestDTO &) const override {
    return unused<
        core::RouteConfigurationFenceReleaseAttemptReconciliationDTO>();
  }

  void set_active(const core::RetainedBundleId &bundle_id,
                  const core::GraphRevision &graph_revision) {
    active_bundle_id_ = bundle_id;
    active_graph_revision_ = graph_revision;
  }

private:
  template <typename Value> [[nodiscard]] static core::Result<Value> unused() {
    return core::Result<Value>::Err(
        {"unused_test_port", "test path was not expected", {}});
  }

  [[nodiscard]] static core::ExecutionHostEpochDTO host_epoch() {
    return {core::RuntimeUnitId("runtime.native"),
            core::RuntimeEpoch("kernel.epoch.1"),
            core::RequestOwnerEpoch("kernel.owner.1"),
            core::ContentDigest(Sha('2')),
            core::ActiveBundlePublicationGeneration(1U)};
  }

  [[nodiscard]] static core::ProjectionRevisionDTO
  projection(const core::RetainedBundleId &bundle_id,
             const core::GraphRevision &graph_revision) {
    return {bundle_id,
            graph_revision,
            {core::SceneId(Sha('3')), 1U, core::ContentDigest(Sha('4'))}};
  }

  mutable std::mutex mutex_;
  std::optional<core::RouteConfigurationFenceAcceptedDTO> accepted_;
  mutable std::optional<core::CellAdmissionClaimDTO> claim_;
  mutable std::optional<core::CellAdmissionClaimDTO> last_claim_;
  mutable std::optional<core::CellAdmissionClaimReleaseDTO> release_;
  core::RetainedBundleId active_bundle_id_{"unset"};
  core::GraphRevision active_graph_revision_{"unset"};
};

class CountingActivePublicationReader final
    : public core::IActiveBundlePublicationReader {
public:
  [[nodiscard]] core::Result<core::ActiveBundlePublicationReceiptDTO>
  ReadActiveBundlePublication(
      core::ActiveBundlePublicationGeneration) const override {
    ++read_calls;
    return core::Result<core::ActiveBundlePublicationReceiptDTO>::Err(
        {"unexpected_publication_read", "stale-binding preflight read the Manager",
         {}});
  }

  mutable std::size_t read_calls{};
};

class CountingRoutePublicationOwner final
    : public deployment::IRouteConfigurationKernelPublicationOwner {
public:
  [[nodiscard]] core::Result<std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO &) const override {
    ++physical_reads;
    return core::Result<std::vector<core::RuntimeParticipantBinding>>::Err(
        {"unexpected_physical_read", "stale-binding preflight read routes", {}});
  }

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO &,
      std::vector<deployment::VerifiedRouteReadinessParticipant>) override {
    ++publications;
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
        {"unexpected_route_publication",
         "stale-binding preflight published a route", {}});
  }

  mutable std::size_t physical_reads{};
  std::size_t publications{};
};

class AdmissionEngine final
    : public core::IResolvedParticipantExecutionEngine,
      public core::IResolvedPreparedParticipantExecutionEngine,
      public core::IControlDomainExecutionPort {
public:
  [[nodiscard]] core::Result<std::uint64_t>
  execute(const core::ExecuteRequestDTO &) {
    ++execute_count_;
    status_.state = core::ExecutionState::kPlanning;
    status_.execution_id = 1U;
    return core::Result<std::uint64_t>::Ok(1U);
  }

  [[nodiscard]] core::Result<std::uint64_t> PrepareMotion(
      core::ResolvedPreparedMotionExecutionRequestDTO request) override {
    auto participant = MakePreparedMotionParticipant(request);
    if (!participant.ok()) {
      return core::Result<std::uint64_t>::Err(participant.error());
    }
    auto retained =
        std::make_shared<const core::PreparedOperationParticipantDTO>(
            std::move(participant.value()));
    auto backend_prepare = core::MakePrepareParticipantResult(
        *retained, core::PrepareParticipantResultStateDTO::kPrepared);
    if (!backend_prepare.ok()) {
      return core::Result<std::uint64_t>::Err(backend_prepare.error());
    }
    prepared_request_ = std::move(request);
    prepared_snapshot_ = core::ResolvedPreparedMotionExecutionSnapshotDTO{
        1U, core::ResolvedPreparedMotionExecutionStateDTO::kPrepared, retained,
        std::move(backend_prepare.value()), std::nullopt};
    const core::KernelEvent event{
        core::KernelEventType::kParticipantPrepared,
        1U,
        0U,
        {},
        {},
        std::nullopt,
        {{"prepared_contract_digest",
          retained->prepared_contract_digest.value()}}};
    const auto callbacks = callbacks_;
    for (const auto &[id, callback] : callbacks) {
      (void)id;
      callback(event);
    }
    return core::Result<std::uint64_t>::Ok(1U);
  }

  [[nodiscard]] core::Result<core::ResolvedPreparedMotionExecutionSnapshotDTO>
  ObservePreparedMotion(std::uint64_t execution_id) const override {
    if (!prepared_snapshot_.has_value() ||
        prepared_snapshot_->execution_id != execution_id) {
      return core::Result<core::ResolvedPreparedMotionExecutionSnapshotDTO>::
          Err({"prepared_motion_missing",
               "test prepared motion is unavailable",
               {}});
    }
    return core::Result<core::ResolvedPreparedMotionExecutionSnapshotDTO>::Ok(
        *prepared_snapshot_);
  }

  [[nodiscard]] core::Result<void> DispatchPreparedMotion(
      std::uint64_t execution_id,
      const core::ContentDigest &prepared_contract_digest) override {
    if (!prepared_request_.has_value() || !prepared_snapshot_.has_value() ||
        !prepared_snapshot_->participant ||
        prepared_snapshot_->execution_id != execution_id ||
        prepared_snapshot_->participant->prepared_contract_digest !=
            prepared_contract_digest) {
      return core::Result<void>::Err(
          {"prepared_motion_dispatch_mismatch",
           "test prepared motion dispatch identity differs",
           {}});
    }
    prepared_snapshot_->state =
        core::ResolvedPreparedMotionExecutionStateDTO::kDispatched;
    auto dispatched = execute(prepared_request_->execution_request);
    return dispatched.ok() ? core::Result<void>::Ok()
                           : core::Result<void>::Err(dispatched.error());
  }

  [[nodiscard]] core::Result<void>
  CancelPreparedMotion(std::uint64_t execution_id) override {
    if (!prepared_snapshot_.has_value() ||
        prepared_snapshot_->execution_id != execution_id) {
      return core::Result<void>::Err({"prepared_motion_missing",
                                      "test prepared motion is unavailable",
                                      {}});
    }
    prepared_snapshot_->state =
        core::ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
    return core::Result<void>::Ok();
  }

  [[nodiscard]] core::Result<std::uint64_t> PrepareCapabilityCommand(
      core::ResolvedPreparedCapabilityCommandExecutionRequestDTO) override {
    return Unsupported<std::uint64_t>();
  }

  [[nodiscard]]
  core::Result<core::ResolvedPreparedCapabilityCommandSnapshotDTO>
  ObservePreparedCapabilityCommand(std::uint64_t) const override {
    return Unsupported<core::ResolvedPreparedCapabilityCommandSnapshotDTO>();
  }

  [[nodiscard]] core::Result<core::GripperCommandResultDTO>
  DispatchPreparedCapabilityCommand(std::uint64_t, const core::ContentDigest &,
                                    const core::ControllerGoalId &,
                                    core::ControllerGoalGeneration) override {
    return Unsupported<core::GripperCommandResultDTO>();
  }

  [[nodiscard]] core::Result<void>
  CancelPreparedCapabilityCommand(std::uint64_t) override {
    return Unsupported<void>();
  }

  [[nodiscard]] core::ResetResultDTO
  reset_after_certified_containment(std::uint64_t,
                                    std::optional<std::uint64_t>) override {
    return {true, std::nullopt};
  }
  [[nodiscard]] core::AcknowledgeCompletedResultDTO
  acknowledge_completed(std::uint64_t, std::optional<std::uint64_t>) override {
    return {true, std::nullopt};
  }

  [[nodiscard]] core::Result<core::ResolvedRuntimeScaleAcceptedDTO>
  set_runtime_scale(const core::ResolvedRuntimeScaleCommandDTO &) override {
    return Unsupported<core::ResolvedRuntimeScaleAcceptedDTO>();
  }
  [[nodiscard]] core::Result<core::ResolvedRuntimeScaleStatusDTO>
  runtime_scale_status(const core::OperationId &,
                       const core::ControlBindingId &) const override {
    return Unsupported<core::ResolvedRuntimeScaleStatusDTO>();
  }
  [[nodiscard]] core::GripperCommandResultDTO
  command_gripper(const core::GripperCommandRequestDTO &) override {
    return {false, 0U, 0U,
            core::ErrorDTO{
                "unsupported_capability", "test engine has no gripper", {}}};
  }
  [[nodiscard]] core::Result<void> validate_gripper_command(
      const core::GripperCommandRequestDTO &) const override {
    return Unsupported<void>();
  }
  [[nodiscard]] core::GripperCommandStatusDTO
  get_gripper_status(const std::string &) const override {
    return {};
  }
  [[nodiscard]] core::Result<std::uint64_t>
  stop_gripper(const std::string &, std::uint64_t, std::uint64_t) override {
    return Unsupported<std::uint64_t>();
  }
  [[nodiscard]] core::Result<void>
  reset_gripper(const std::string &, std::uint64_t, std::uint64_t) override {
    return Unsupported<void>();
  }
  [[nodiscard]] core::ExecutionStatus get_status() const override {
    return status_;
  }
  [[nodiscard]] core::SubscriptionHandle
  subscribe_events(core::EventCallback callback) override {
    const auto id = next_subscription_id_++;
    callbacks_.emplace(id, std::move(callback));
    return {id};
  }
  void unsubscribe_events(const core::SubscriptionHandle handle) override {
    callbacks_.erase(handle.id);
  }
  [[nodiscard]] core::ParticipantStopStageResultDTO
  TranslateStopStage(const core::ResolvedParticipantStopStageDTO &,
                     const core::RemainingStopBudgetDTO &,
                     std::stop_token) override {
    throw std::logic_error("test did not request Stop-stage translation");
  }

  [[nodiscard]] std::size_t execute_count() const noexcept {
    return execute_count_;
  }

  [[nodiscard]] std::shared_ptr<const core::PreparedOperationParticipantDTO>
  prepared_participant() const noexcept {
    return prepared_snapshot_.has_value() ? prepared_snapshot_->participant
                                          : nullptr;
  }

private:
  template <typename Value>
  [[nodiscard]] static core::Result<Value> Unsupported() {
    return core::Result<Value>::Err(
        {"unsupported_capability", "test engine path is unsupported", {}});
  }

  [[nodiscard]] static core::Result<core::PreparedOperationParticipantDTO>
  MakePreparedMotionParticipant(
      const core::ResolvedPreparedMotionExecutionRequestDTO &authority) {
    using namespace manyforge::core;
    const auto &request = authority.execution_request;
    const auto &binding = authority.binding.value();
    const auto &control = *binding.control_path;
    std::vector<CoordinatePositionDTO> positions;
    positions.reserve(binding.resources.size());
    for (const auto &resource : binding.resources) {
      positions.emplace_back(
          resource.coordinate_kind == CoordinateKind::kAngular
              ? CoordinatePositionDTO{AngularPositionDTO{0.0}}
              : CoordinatePositionDTO{LinearPositionDTO{0.0}});
    }
    ControlledStateDTO start{
        binding.controllable_component_id,
        binding.command_target_id,
        control.control_domain_id,
        control.control_domain_revision,
        control.control_binding_id,
        control.control_binding_revision,
        control.controller_topology_id,
        control.topology_revision,
        control.control_route_id,
        control.active_control_configuration_id,
        control.active_control_configuration_generation,
        request.binding.resources,
        request.binding.model_group_binding.compiled_joint_names,
        std::move(positions),
        std::nullopt};
    ProjectionRevisionDTO projection{
        binding.retained_bundle_id, binding.graph_revision,
        SceneRevision{SceneId(Sha('a')), 1U, ContentDigest(Sha('b'))}};
    PlanRequestDTO plan_request{
        request.binding.operation_id,
        request.binding.participant_id,
        request.binding.program_id,
        request.binding.program_revision,
        request.binding.program_binding_id,
        request.binding.program_run_id,
        MoveId(request.move_id),
        request.binding.controllable_component_id,
        request.binding.component_type_id,
        request.binding.component_type_revision,
        request.binding.command_target_id,
        request.binding.capability_id,
        request.binding.capability_definition_revision,
        request.binding.planner_capability_binding,
        request.binding.model_bindings,
        request.binding.model_group_binding,
        request.binding.control_domain_id,
        request.binding.control_domain_revision,
        request.binding.control_binding_id,
        request.binding.control_binding_revision,
        request.binding.control_interface_contract_id,
        request.binding.control_interface_revision,
        request.binding.controller_topology_id,
        request.binding.topology_revision,
        request.binding.control_route_id,
        request.binding.activation_set_id,
        request.binding.active_control_configuration_id,
        request.binding.active_control_configuration_generation,
        request.binding.provider_instance_id,
        request.binding.provider_manifest_id,
        request.binding.provider_manifest_revision,
        request.binding.provider_runtime_epoch,
        request.binding.runtime_unit_bindings,
        request.binding.resources,
        request.action_span,
        request.binding.base_frame,
        request.binding.tcp_frame,
        start,
        request.motion,
        request.tolerances,
        request.speed_profile,
        request.planning_profile,
        request.contact_policy_id.empty()
            ? std::nullopt
            : std::optional<std::string>{request.contact_policy_id},
        request.per_resource_dynamics_limits,
        request.timeouts.planning_timeout_s,
        0U,
        ProjectionLeaseBindingDTO{projection,
                                  ProjectionLeaseId("lease.native.readiness")},
        request.binding.component_attachment_revisions,
        request.binding.carried_object_attachment_revisions,
        {},
        {}};
    std::vector<double> zeros(request.binding.resources.size(), 0.0);
    TrajectoryDTO trajectory{
        request.binding.resources,
        request.binding.model_group_binding.compiled_joint_names,
        {{0.0, zeros, {}, {}}, {1.0, zeros, {}, {}}}};
    PlanResultDTO planning_result{trajectory, std::nullopt, projection};
    CollisionWorldRevision world{1U, Sha('c')};
    CollisionDecision decision;
    decision.accepted = true;
    decision.severity = CollisionDecisionSeverity::kNone;
    decision.action = CollisionDecisionAction::kDispatch;
    decision.collision_world_revision = world;

    std::vector<PreparedMotionInterruptScopeDTO> scopes;
    scopes.reserve(request.motion_interrupt_scopes.size());
    for (const auto &scope : request.motion_interrupt_scopes) {
      if (!scope.stop_profile) {
        return core::Result<core::PreparedOperationParticipantDTO>::Err(
            {"stop_profile_missing",
             "test prepared scope has no Stop profile",
             {}});
      }
      PreparedParticipantStopPlanContextDTO context{
          request.binding.operation_id,
          authority.operation_revision,
          request.binding.participant_id,
          request.binding.program_id,
          request.binding.program_revision,
          request.binding.program_binding_id,
          request.binding.program_run_id,
          scope.stop_profile,
          ParticipantStopProfilePurposeDTO::kMotionInterruptScope,
          scope.scope_id,
          scope.node_category};
      auto stop = MakePreparedParticipantStopPlan(
          authority.stop_readiness.value().resolved_stop_base_plan,
          std::move(context), binding.resources,
          authority.retained_default_stop_limits);
      if (!stop.ok()) {
        return core::Result<core::PreparedOperationParticipantDTO>::Err(
            stop.error());
      }
      scopes.push_back({MotionInterruptScopeId(scope.scope_id),
                        InterruptPolicyId(scope.policy_id),
                        scope.activation_mode, scope.arm_timeout_ns,
                        scope.result_binding_key, scope.required,
                        *scope.stop_profile, scope.node_category,
                        scope.metadata, std::move(stop.value())});
    }
    std::sort(scopes.begin(), scopes.end(),
              [](const auto &left, const auto &right) {
                return left.scope_id < right.scope_id;
              });

    std::optional<PreparedContactPolicyRefDTO> contact_policy;
    if (request.resolved_contact_grasp) {
      contact_policy = PreparedContactPolicyRefDTO{
          request.resolved_contact_grasp->contact_policy_id,
          request.resolved_contact_grasp->contact_policy_revision};
    }
    PreparedMotionExecutionDTO payload{
        MoveId(request.move_id),
        plan_request,
        trajectory,
        start,
        {std::move(planning_result), 0U,
         request.binding.planner_capability_binding},
        {ValidationRevision("validation.native.readiness"),
         ValidationResultDTO{
             true, std::nullopt,
             TrajectoryValidationMetricsDTO{
                 trajectory.waypoints.back().time_from_start_s,
                 std::vector<double>(trajectory.joint_names.size(), 0.5),
                 std::vector<double>(trajectory.joint_names.size(), 0.0), 0.1}},
         world, CollisionPolicyRevision{1U, Sha('d')}, decision},
        std::move(contact_policy),
        request.speed_profile.max_tcp_linear_speed_mps,
        authority.deployment_maximum_tcp_linear_speed_mps,
        std::move(scopes),
        request.timeouts.execution_timeout_s};

    std::vector<ResourceId> resources = request.binding.resources;
    std::sort(resources.begin(), resources.end());
    std::optional<PreparedCapabilityProviderRuntimeOccurrenceDTO>
        capability_provider;
    if (binding.capability_provider_path) {
      const auto &provider = *binding.capability_provider_path;
      capability_provider = PreparedCapabilityProviderRuntimeOccurrenceDTO{
          provider.capability_provider_binding_id,
          provider.capability_provider_binding_revision,
          {provider.provider_instance_id, provider.provider_manifest_id,
           provider.provider_manifest_revision,
           provider.provider_runtime_unit_id, provider.provider_runtime_epoch}};
    }
    PreparedOperationParticipantDTO candidate{
        ContentDigest("pending"),
        request.binding.operation_id,
        authority.operation_revision,
        request.binding.participant_id,
        binding.program_id,
        binding.program_revision,
        binding.program_binding_id,
        request.binding.program_run_id,
        binding.retained_bundle_id,
        binding.graph_revision,
        binding.controllable_component_id,
        authority.stop_readiness.value().component_lifecycle_generation,
        binding.component_type_id,
        binding.component_type_revision,
        binding.command_target_id,
        binding.capability_id,
        binding.capability_definition_revision,
        binding.model_bindings,
        binding.frame_bindings,
        control.control_domain_id,
        control.control_domain_revision,
        control.control_binding_id,
        control.control_binding_revision,
        control.control_interface_contract_id,
        control.control_interface_revision,
        control.controller_topology_id,
        control.topology_revision,
        control.control_route_id,
        control.activation_set_id,
        {control.provider_instance_id, control.provider_manifest_id,
         control.provider_manifest_revision, control.provider_runtime_unit_id,
         control.provider_runtime_epoch},
        std::move(capability_provider),
        std::move(resources),
        PreparedParticipantFocusedPortDTO{control.provider_runtime_unit_id},
        {},
        {},
        control.active_control_configuration_id,
        control.active_control_configuration_generation,
        binding.runtime_unit_epochs,
        control.stop_evidence_policy_id,
        control.stop_evidence_policy_revision,
        *authority.ordinary_stop_plan,
        std::move(payload),
        authority.fault_stop_plan
            ? std::optional<
                  PreparedParticipantStopPlanDTO>{*authority.fault_stop_plan}
            : std::nullopt};
    return MakePreparedOperationParticipant(
        std::move(candidate), authority.binding, authority.stop_readiness,
        authority.retained_default_stop_limits, authority.stop_node_category);
  }

  std::size_t execute_count_{};
  std::uint64_t next_subscription_id_{1U};
  core::ExecutionStatus status_{};
  std::map<std::uint64_t, core::EventCallback> callbacks_;
  std::optional<core::ResolvedPreparedMotionExecutionRequestDTO>
      prepared_request_;
  std::optional<core::ResolvedPreparedMotionExecutionSnapshotDTO>
      prepared_snapshot_;
};

class PublicationOwner final
    : public deployment::IRouteConfigurationKernelPublicationOwner {
public:
  PublicationOwner(
      core::RuntimeParticipantBinding binding,
      std::filesystem::path journal_path, core::CellId cell_id,
      core::FakeClock &clock)
      : binding_(std::move(binding)), journal_path_(std::move(journal_path)),
        cell_id_(std::move(cell_id)), clock_(clock) {}

  [[nodiscard]] core::Result<std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO &) const override {
    return core::Result<std::vector<core::RuntimeParticipantBinding>>::Ok(
        {binding_});
  }

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO &configuration,
      std::vector<deployment::VerifiedRouteReadinessParticipant> participants)
      override {
    if (participants.size() != 1U ||
        participants.front().participant_binding.value().control_path ==
            std::nullopt) {
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          {"invalid_request", "test publication has no sole participant", {}});
    }
    std::vector<core::HardStopResourceLimitDTO> hard_limits;
    for (const auto &resource :
         participants.front().participant_binding.value().resources) {
      if (resource.coordinate_kind == core::CoordinateKind::kAngular) {
        hard_limits.push_back(
            {resource.resource_id,
             core::CoordinateVelocityDTO{core::AngularVelocityDTO{0.02}}});
      } else {
        hard_limits.push_back(
            {resource.resource_id,
             core::CoordinateVelocityDTO{core::LinearVelocityDTO{0.02}}});
      }
    }
    const auto &base_plan =
        participants.front().stop_readiness.value().resolved_stop_base_plan;
    const auto controlled = std::find_if(
        base_plan.escalation_stages.begin(), base_plan.escalation_stages.end(),
        [](const auto &stage) {
          return stage.containment_command.command_kind ==
                 core::StopContainmentCommandKind::kMotionControlledStop;
        });
    if (controlled == base_plan.escalation_stages.end() ||
        !controlled->default_contract_parameters.has_value()) {
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          {"invalid_fixture",
           "retained CCLC fixture has no exact controlled Stop profile",
           {}});
    }
    auto operator_profile =
        core::map_motion_controlled_stop_contract_parameters_to_profile(
            *controlled->default_contract_parameters,
            participants.front().participant_binding.value().resources,
            "operator_soft_on_path");
    if (!operator_profile.ok()) {
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          operator_profile.error());
    }
    auto fault_profile = operator_profile.value();
    fault_profile.profile_id = "fault_spring_back_hold";
    fault_profile.stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
    fault_profile.allow_natural_completion_on_short_path = false;
    fault_profile.recoverable_substitution_mode.reset();
    core::RuntimeParticipantExecutionRegistration registration{
        .binding = participants.front().participant_binding,
        .stop_readiness = participants.front().stop_readiness,
        .retained_default_stop_limits = std::move(hard_limits),
        .stop_node_category = core::MotionInterruptNodeCategory::kGeneric,
        .engine = &engine_,
        .stop_execution_port = &engine_,
        .participant_policy = core::select_runtime_participant_policy(
            participants.front().participant_binding),
        .prepared_motion_engine = &engine_,
        .default_operator_stop_profile = std::move(operator_profile.value()),
        .default_fault_stop_profile = std::move(fault_profile),
        .deployment_maximum_tcp_linear_speed_mps = 0.15};
    if (kernel_ == nullptr) {
      journal_ = std::make_unique<core::SqliteOperationJournal>(journal_path_,
                                                                cell_id_);
      kernel_ = std::make_unique<core::ResourceAddressedExecutionKernel>(
          std::vector<core::RuntimeParticipantExecutionRegistration>{
              registration},
          clock_, scheduler_, *journal_,
          core::ResourceAddressedExecutionKernelConfig{
              core::RequestOwnerEpoch("kernel.native.readiness.1"),
              cell_id_,
              64U,
              {},
              nullptr,
              participants.front().participant_binding.value().graph_revision});
    }
    auto published = kernel_->publish_runtime_route_configuration(
        configuration, {std::move(registration)}, {});
    if (!published.ok()) {
      return published;
    }
    auto readback = kernel_->runtime_route_configuration_readback(
        configuration.control_domain_id,
        configuration.active_control_configuration_id,
        configuration.active_control_configuration_generation);
    if (!readback.ok() || readback.value() != published.value()) {
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          {"invariant_violation", "Kernel publication readback differs", {}});
    }
    {
      std::lock_guard lock(mutex_);
      participants_ = std::move(participants);
    }
    return readback;
  }

  [[nodiscard]] std::vector<deployment::VerifiedRouteReadinessParticipant>
  participants() const {
    std::lock_guard lock(mutex_);
    return participants_;
  }

  [[nodiscard]] core::ResourceAddressedExecutionKernel &kernel() {
    if (kernel_ == nullptr) {
      throw std::logic_error("Kernel publication has not occurred");
    }
    return *kernel_;
  }

  [[nodiscard]] std::size_t execute_count() const noexcept {
    return engine_.execute_count();
  }

  [[nodiscard]] std::shared_ptr<const core::PreparedOperationParticipantDTO>
  prepared_participant() const noexcept {
    return engine_.prepared_participant();
  }

private:
  core::RuntimeParticipantBinding binding_;
  std::filesystem::path journal_path_;
  core::CellId cell_id_;
  core::FakeClock &clock_;
  core::ManualScheduler scheduler_;
  mutable std::mutex mutex_;
  std::vector<deployment::VerifiedRouteReadinessParticipant> participants_;
  AdmissionEngine engine_;
  std::unique_ptr<core::SqliteOperationJournal> journal_;
  std::unique_ptr<core::ResourceAddressedExecutionKernel> kernel_;
};

[[nodiscard]] core::ExpectedControlDomainRevisionDTO
expected_domain(const RuntimeContext &runtime) {
  std::vector<core::ExpectedControlBindingRevisionDTO> bindings;
  for (const auto &route : runtime.desired->binding_routes) {
    bindings.push_back({route.control_binding_id,
                        route.control_binding_revision,
                        route.control_route_id});
  }
  std::sort(bindings.begin(), bindings.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.control_binding_id < rhs.control_binding_id;
            });
  return {runtime.domain->control_domain_id,
          runtime.domain->control_domain_revision,
          runtime.domain->controller_topology_id,
          runtime.domain->topology_revision,
          {core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
           std::nullopt, std::nullopt},
          std::move(bindings)};
}

[[nodiscard]] core::OperationIntentDTO
motion_operation(const core::RuntimeParticipantBinding &binding) {
  const auto &value = binding.value();
  std::vector<std::string> source_joint_names;
  std::vector<double> positions;
  std::map<std::string, core::JointDynamicsLimitDTO> dynamics;
  for (const auto &resource : value.resources) {
    if (!resource.source_joint_name.has_value()) {
      throw std::logic_error("motion resource has no source joint");
    }
    source_joint_names.push_back(*resource.source_joint_name);
    positions.push_back(0.01);
    dynamics.emplace(*resource.source_joint_name,
                     core::JointDynamicsLimitDTO{1.0, 2.0, std::nullopt});
  }
  const core::JointGoalDTO joint_goal{std::move(source_joint_names),
                                      std::move(positions)};
  const core::MotionParticipantIntentDTO motion{
      core::MoveId("move.native.readiness.1"),
      core::FrameRoleId("base"),
      core::FrameRoleId("tcp"),
      core::TolerancesDTO{0.01, 0.001, 0.01},
      core::SpeedProfileDTO{},
      core::TimeoutsDTO{},
      core::CacheMode::kDefault,
      core::PlanningProfileDTO::kBalanced,
      core::MotionTargetModeDTO::kAbsolute,
      std::nullopt,
      std::nullopt,
      joint_goal,
      std::nullopt,
      std::move(dynamics),
      {},
      {}};
  const core::OperationParticipantIntentDTO participant{
      value.component_role_id, value.target_role_id, value.capability_id,
      std::nullopt, motion};
  std::vector<core::OperationParticipantIntentDTO> participants{participant};
  return {{core::RequestId("execute.native.readiness"),
           core::RequestOwnerEpoch("kernel.native.readiness.1"), "test",
           std::nullopt},
          value.program_id,
          value.program_revision,
          value.program_binding_id,
          core::ProgramRunId("run.native.readiness.1"),
          value.graph_revision,
          value.retained_bundle_id,
          std::nullopt,
          std::move(participants),
          core::OperationCoordinationPolicy::kIndependent};
}

void ExpectExactControlledStopPlan(
    const core::PreparedParticipantStopPlanDTO &plan,
    std::span<const core::RuntimeResourceBindingDTO> resources,
    core::ParticipantStopProfilePurposeDTO expected_purpose,
    std::string_view expected_profile_id,
    core::MotionInterruptStopMode expected_mode,
    bool expected_natural_completion,
    std::optional<core::MotionInterruptStopMode> expected_recoverable_mode) {
  EXPECT_EQ(plan.participant_stop_profile_purpose, expected_purpose);
  ASSERT_TRUE(plan.participant_stop_profile.has_value());
  const auto &profile = *plan.participant_stop_profile;
  EXPECT_EQ(profile.profile_id, expected_profile_id);
  EXPECT_EQ(profile.stop_mode, expected_mode);
  ASSERT_TRUE(profile.deceleration_time_s.has_value());
  EXPECT_DOUBLE_EQ(*profile.deceleration_time_s, 1.0);
  EXPECT_EQ(profile.allow_natural_completion_on_short_path,
            expected_natural_completion);
  ASSERT_EQ(profile.recoverable_substitution_mode.has_value(),
            expected_recoverable_mode.has_value());
  if (expected_recoverable_mode.has_value()) {
    EXPECT_EQ(*profile.recoverable_substitution_mode, *expected_recoverable_mode);
  }
  EXPECT_TRUE(profile.require_backend_confirmation);
  ASSERT_EQ(profile.per_joint_max_deceleration.size(), 6U);
  for (const auto &[joint, limit] : profile.per_joint_max_deceleration) {
    (void)joint;
    const auto *angular = std::get_if<core::AngularAccelerationDTO>(&limit);
    ASSERT_NE(angular, nullptr);
    EXPECT_DOUBLE_EQ(angular->value_rad_s2, 4.0);
  }

  ASSERT_EQ(plan.stage_commands.size(), 2U);
  const auto &nominal = plan.stage_commands.front();
  EXPECT_EQ(nominal.resolved_stage.stage_kind,
            core::StopEscalationStageKind::kNominal);
  EXPECT_EQ(nominal.resolved_stage.containment_command.command_kind,
            core::StopContainmentCommandKind::kMotionControlledStop);
  EXPECT_EQ(nominal.parameter_source,
            core::StopStageParameterSourceDTO::kParticipantProfileMapping);
  ASSERT_TRUE(nominal.validated_contract_parameters.has_value());
  const auto decoded =
      core::map_motion_controlled_stop_contract_parameters_to_profile(
          *nominal.validated_contract_parameters, resources,
          expected_profile_id);
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value(), profile);

  const auto &hard = plan.stage_commands.back();
  EXPECT_EQ(hard.resolved_stage.stage_kind,
            core::StopEscalationStageKind::kSoftwareHard);
  EXPECT_EQ(hard.resolved_stage.containment_command.command_kind,
            core::StopContainmentCommandKind::kRegistered);
  ASSERT_TRUE(hard.resolved_stage.containment_command.registered_command_kind_id
                  .has_value());
  EXPECT_EQ(hard.resolved_stage.containment_command.registered_command_kind_id
                ->value(),
            "manyforge.stop.software_hard");
}

class RestartReadinessPort final : public core::ICCLCComponentCommandReadinessPort {
public:
  explicit RestartReadinessPort(core::ResourceAddressedExecutionKernel &kernel,
                                bool fail)
      : kernel_(kernel), fail_(fail) {}

  core::Result<core::ComponentCommandReadinessDTO> CloseComponentCommandEligibility(
      const core::LifecycleTransactionId &,
      const core::ControllableComponentId &) override {
    ADD_FAILURE() << "Restart must not dispatch a lifecycle close";
    return core::Result<core::ComponentCommandReadinessDTO>::Err(
        {"unexpected_close", "unexpected lifecycle effect", {}});
  }
  core::Result<core::ComponentCommandReadinessDTO> PublishComponentCommandReadiness(
      const core::ComponentCommandReadinessPublicationDTO &publication) override {
    ++publications;
    EXPECT_EQ(publication.authority,
              core::ComponentReadinessAuthorityDTO::kRestartExclusion);
    if (fail_) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          {"injected_readiness_failure", "restart readback unavailable",
           {{"reason", "injected_readiness_failure"}}});
    }
    return kernel_.publish_component_command_readiness(publication);
  }
  core::Result<core::ComponentCommandReadinessDTO> ReadComponentCommandReadiness(
      const core::ControllableComponentId &component) const override {
    return kernel_.component_command_readiness(component);
  }
  std::size_t publications{};

private:
  core::ResourceAddressedExecutionKernel &kernel_;
  bool fail_;
};

enum class ReadinessCase { kPublication, kFailedRestart, kRepeatedReconciliation };

void CheckRetainedProductReadiness(ReadinessCase test_case) {
  const std::filesystem::path fixture_root =
      MANYFORGE_CCLC_READINESS_FIXTURE_ROOT;
  auto bundle = load_fixture_bundle(fixture_root);
  auto runtime = make_runtime_context(*bundle);
  const auto catalog = make_prepare_catalog(*runtime.control_binding);
  FixedCatalogSource catalogs(catalog);
  auto brand_port = std::make_shared<PreparingBrandPort>();

  ScopedTrustedRoot state;
  const auto manager_root = state.path() / "manager";
  const auto state_root = state.path() / "state";
  const auto plan_root = state.path() / "plans";
  ASSERT_TRUE(std::filesystem::create_directory(manager_root));
  ASSERT_TRUE(std::filesystem::create_directory(state_root));
  ASSERT_TRUE(std::filesystem::create_directory(plan_root));
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      manager_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      bundle->value().cell_graph.cell_id);
  const core::ProjectionRevisionDTO active_projection{
      bundle->retained_bundle_id(),
      bundle->value().cell_graph.graph_revision,
      {core::SceneId(Sha('5')), 1U, core::ContentDigest(Sha('6'))}};
  const auto published_bundle = publication_store.PublishActiveBundle(
      {core::BootstrapPublicationTransactionDTO{
           core::BootstrapId("native-cclc-bootstrap"),
           core::BootstrapGeneration(1U)},
       active_projection, bundle->retained_bundle_id(),
       bundle->value().cell_graph.graph_revision, std::nullopt,
       core::ContentDigest(Sha('7'))});
  const auto integrity = publication_store.VerifyIntegrity();
  ASSERT_EQ(integrity.current_generation,
            published_bundle.publication_generation);
  const auto publication = publication_store.ReadActiveBundlePublication(
      integrity.current_generation);
  ASSERT_TRUE(publication.ok()) << publication.error().message;

  const core::RequestOwnerEpoch owner("cclc.native.readiness.1");
  RetainedNativeCCLCRuntimeConfig config{
      {state_root,
       plan_root,
       bundle->value().cell_graph.cell_id,
       {owner, 128U, 2U, 1'000'000LL, 1'000'000'000LL}},
      1'000'000'000LL};

  const core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
      runtime.domain->control_domain_id,
      runtime.domain->control_domain_revision,
      runtime.control_binding->provider_instance_id,
      runtime.control_binding->provider_manifest_id,
      runtime.control_binding->provider_manifest_revision,
      runtime.control_binding->provider_runtime_unit_id,
      runtime.provider_epoch,
      catalog.catalog_digest};
  KernelPorts kernel_ports;
  kernel_ports.set_active(bundle->retained_bundle_id(),
                          bundle->value().cell_graph.graph_revision);
  const deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(
      fixture_root / "retained-package");
  core::FakeClock clock;
  clock.set(1'000);
  PublicationOwner publication_owner(
      runtime.binding, state.path() / "operation-journal.sqlite3",
      bundle->value().cell_graph.cell_id, clock);
  core::ManualScheduler scheduler;

  auto composition = CreateRetainedNativeCCLCRuntime(
      config, bundle, publication_store, publication.value(), artifacts,
      catalogs, {{occurrence, brand_port}}, {runtime.binding}, kernel_ports,
      kernel_ports, publication_owner, clock, scheduler);
  ASSERT_TRUE(composition.ok()) << composition.error().message;
  ASSERT_TRUE(composition.value()->Start().ok());
  const auto current_request_owner =
      composition.value()->coordinator().request_owner_epoch();

  core::ComponentLifecycleIntentDTO prepare{
      {core::RequestId("prepare.native.readiness"), current_request_owner,
       "test", std::nullopt},
      runtime.component->controllable_component_id,
      core::ComponentLifecycleIntentKindDTO::kPrepare,
      bundle->value().cell_graph.graph_revision,
      core::ComponentLifecycleGeneration(1U),
      {expected_domain(runtime)},
      std::nullopt,
      {core::RequestTransportDTO::kInternal,
       core::RequestAssuranceDTO::kUnverified, std::nullopt}};
  prepare.expected_admission_generation =
      kernel_ports.GetAuthoringAdmission().admission_generation;
  const auto lifecycle =
      composition.value()->coordinator().SubmitLifecycle(prepare);
  ASSERT_TRUE(lifecycle.ok()) << lifecycle.error().message;
  ASSERT_TRUE(lifecycle.value().acceptance.has_value());
  EXPECT_TRUE(
      composition.value()->coordinator().DrainFor(std::chrono::seconds(5)));
  const auto lifecycle_status =
      composition.value()->coordinator().LoadLifecycleStatus(
          lifecycle.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(lifecycle_status.ok()) << lifecycle_status.error().message;
  ASSERT_TRUE(lifecycle_status.value().has_value());
  ASSERT_TRUE(lifecycle_status.value()->terminal_result.has_value());
  EXPECT_EQ(*lifecycle_status.value()->terminal_result,
            core::ComponentLifecycleTerminalResultDTO::kSucceeded);
  ASSERT_TRUE(lifecycle_status.value()->current_state.has_value());
  EXPECT_EQ(*lifecycle_status.value()->current_state,
            core::ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(brand_port->dispatch_count(), 1U);

  // This synthetic adapter fixture has no Manager Apply or native lifecycle
  // product. Its fake provider reaches Active before route selection so the
  // publication test can exercise the admitted-route case. Production native
  // Enable still requires a previously installed, command-closed route.
  auto enable = prepare;
  enable.request_context.request_id = core::RequestId("enable.native.readiness");
  enable.intent = core::ComponentLifecycleIntentKindDTO::kEnable;
  enable.expected_component_lifecycle_generation =
      lifecycle_status.value()->acceptance.component_lifecycle_generation;
  const auto enabled = composition.value()->coordinator().SubmitLifecycle(enable);
  ASSERT_TRUE(enabled.ok()) << enabled.error().message;
  ASSERT_TRUE(enabled.value().acceptance.has_value());
  EXPECT_TRUE(composition.value()->coordinator().DrainFor(std::chrono::seconds(5)));
  const auto enabled_status = composition.value()->coordinator().LoadLifecycleStatus(
      enabled.value().acceptance->lifecycle_transaction_id);
  ASSERT_TRUE(enabled_status.ok()) << enabled_status.error().message;
  ASSERT_TRUE(enabled_status.value().has_value());
  ASSERT_TRUE(enabled_status.value()->terminal_result.has_value());
  EXPECT_EQ(*enabled_status.value()->terminal_result,
            core::ComponentLifecycleTerminalResultDTO::kSucceeded);
  EXPECT_EQ(enabled_status.value()->current_state,
            core::ComponentLifecycleStateDTO::kActive);
  EXPECT_EQ(brand_port->dispatch_count(), 2U);

  core::RouteSelectionIntentDTO route{
      {core::RequestId("route.native.readiness"), current_request_owner, "test",
       std::nullopt},
      bundle->retained_bundle_id(),
      bundle->value().cell_graph.graph_revision,
      *runtime.desired,
      {core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
       std::nullopt, std::nullopt},
      {{runtime.component->controllable_component_id,
        enabled_status.value()->acceptance.component_lifecycle_generation,
        core::ComponentLifecycleStateDTO::kActive}},
      {core::RequestTransportDTO::kInternal,
       core::RequestAssuranceDTO::kUnverified, std::nullopt},
      kernel_ports.GetAuthoringAdmission().admission_generation};
  const auto route_result =
      composition.value()->coordinator().SubmitRouteSelection(route);
  ASSERT_TRUE(route_result.ok()) << route_result.error().message;
  ASSERT_TRUE(route_result.value().acceptance.has_value());
  const bool route_drained =
      composition.value()->coordinator().DrainFor(std::chrono::seconds(5));
  const auto route_status =
      composition.value()->coordinator().LoadRouteSelectionStatus(
          route_result.value().acceptance->route_selection_transaction_id);
  ASSERT_TRUE(route_status.ok()) << route_status.error().message;
  ASSERT_TRUE(route_status.value().has_value());
  EXPECT_TRUE(route_drained)
      << "route phase=" << static_cast<int>(route_status.value()->phase)
      << " sequence=" << route_status.value()->status_sequence;
  ASSERT_TRUE(route_status.value()->terminal_result.has_value());
  EXPECT_EQ(*route_status.value()->terminal_result,
            core::RouteSelectionTerminalResultDTO::kSucceeded);
  ASSERT_TRUE(
      route_status.value()->resulting_active_control_configuration.has_value());
  EXPECT_EQ(route_status.value()->resulting_active_control_configuration->state,
            core::ActiveControlConfigurationStateDTO::kReconciled);

  const auto published = publication_owner.participants();
  ASSERT_EQ(published.size(), 1U);
  EXPECT_EQ(published.front().stop_readiness.value().controllable_component_id,
            runtime.component->controllable_component_id);
  EXPECT_EQ(published.front()
                .stop_readiness.value()
                .resolved_stop_base_plan.active_control_configuration_id,
            route_status.value()
                ->resulting_active_control_configuration
                ->active_control_configuration_id);
  EXPECT_TRUE(std::filesystem::exists(
      plan_root / "resolved-stop-base-plans" / "sha256" /
      (published.front()
           .stop_readiness.value()
           .resolved_stop_base_plan.resolved_stop_base_plan_id.value()
           .substr(7U) +
       ".json")));

  // Route selection installed route and Stop readiness only: it never
  // enables the component, even though CCLC recorded it Active.
  const auto route_only =
      publication_owner.kernel().execute(motion_operation(runtime.binding));
  ASSERT_FALSE(route_only.ok());
  EXPECT_EQ(route_only.error().details.at("reason"),
            "component_command_not_eligible");
  EXPECT_EQ(publication_owner.execute_count(), 0U);

  if (test_case != ReadinessCase::kPublication) {
    // Close and reopen the real durable CCLC owner while retaining the route
    // and engine. Inject only the readiness port result, not recovery logic.
    composition.value().reset();
    ASSERT_TRUE(publication_owner.kernel().SetRouteRecoveryPending(true).ok());
    const auto held_generation =
        publication_owner.kernel().GetAuthoringAdmission().admission_generation;
    RestartReadinessPort readiness(
        publication_owner.kernel(), test_case == ReadinessCase::kFailedRestart);
    config.composition.coordinator.request_owner_epoch =
        core::RequestOwnerEpoch("cclc.native.readiness.restarted");
    std::function<core::Result<void>()> reconcile;
    auto recovered = CreateRecoveredRetainedNativeCellCCLCRuntime(
        config, bundle, publication_store, publication.value(), artifacts,
        catalogs, {{occurrence, brand_port}}, {runtime.binding}, kernel_ports,
        kernel_ports, publication_owner, readiness,
        publication_owner.kernel(), clock, scheduler,
        [](const core::ActiveControlConfigurationDTO &) {
          ADD_FAILURE() << "Exact installed route must not be republished";
          return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
              {"unexpected_adoption", "route should already be current", {}});
        }, [] { return core::Result<void>::Ok(); }, reconcile);
    EXPECT_EQ(readiness.publications, 1U);
    EXPECT_EQ(brand_port->dispatch_count(), 2U);
    if (test_case == ReadinessCase::kFailedRestart) {
      ASSERT_FALSE(recovered.ok());
      EXPECT_EQ(recovered.error().code, "injected_readiness_failure");
      EXPECT_FALSE(reconcile);
      EXPECT_FALSE(publication_owner.kernel().GetAuthoringAdmission().authoring_allowed);
      EXPECT_EQ(publication_owner.kernel().GetAuthoringAdmission().admission_generation,
                held_generation);
      // Prove the restart exclusion itself is still held, not merely that
      // another missing readiness fact happens to block commands.
      const auto under_exclusion = publication_owner.kernel().publish_component_command_readiness(
          {core::ComponentReadinessAuthorityDTO::kRestartExclusion, std::nullopt,
           std::nullopt, runtime.component->controllable_component_id,
           enabled_status.value()->acceptance.component_lifecycle_generation, false});
      ASSERT_TRUE(under_exclusion.ok()) << under_exclusion.error().message;
      EXPECT_FALSE(publication_owner.kernel().execute(motion_operation(runtime.binding)).ok());
      EXPECT_EQ(publication_owner.execute_count(), 0U);
    } else {
      ASSERT_TRUE(recovered.ok()) << recovered.error().message;
      ASSERT_TRUE(reconcile);
      const auto before = readiness.ReadComponentCommandReadiness(
          runtime.component->controllable_component_id);
      ASSERT_TRUE(before.ok());
      EXPECT_TRUE(before.value().command_eligible);
      const auto released_generation =
          publication_owner.kernel().GetAuthoringAdmission().admission_generation;
      for (int attempt = 0; attempt < 3; ++attempt) {
        const auto result = reconcile();
        ASSERT_TRUE(result.ok()) << result.error().message;
      }
      EXPECT_EQ(readiness.publications, 1U);
      EXPECT_EQ(brand_port->dispatch_count(), 2U);
      EXPECT_EQ(publication_owner.kernel().GetAuthoringAdmission().admission_generation,
                released_generation);
      const auto after = readiness.ReadComponentCommandReadiness(
          runtime.component->controllable_component_id);
      ASSERT_TRUE(after.ok());
      EXPECT_EQ(after.value(), before.value());
      EXPECT_TRUE(publication_owner.kernel().execute(motion_operation(runtime.binding)).ok());
      EXPECT_EQ(publication_owner.execute_count(), 1U);
    }
    return;
  }

  // Publish the committed Active result as owner reconstruction would, under
  // the Kernel's restart exclusion and at the lifecycle generation.
  ASSERT_TRUE(publication_owner.kernel().SetRouteRecoveryPending(true).ok());
  const auto published_readiness =
      publication_owner.kernel().publish_component_command_readiness(
      {core::ComponentReadinessAuthorityDTO::kRestartExclusion, std::nullopt, std::nullopt,
       runtime.component->controllable_component_id,
       enabled_status.value()->acceptance.component_lifecycle_generation, true});
  ASSERT_TRUE(publication_owner.kernel().SetRouteRecoveryPending(false).ok());
  ASSERT_TRUE(published_readiness.ok()) << published_readiness.error().message;

  const auto accepted =
      publication_owner.kernel().execute(motion_operation(runtime.binding));
  ASSERT_TRUE(accepted.ok())
      << accepted.error().message
      << "; reason=" << accepted.error().details.at("reason");
  EXPECT_EQ(publication_owner.execute_count(), 1U);
  const auto prepared = publication_owner.prepared_participant();
  ASSERT_NE(prepared, nullptr);
  EXPECT_EQ(prepared->stop_evidence_policy_id.value(),
            "lite6_motion_controlled_stop");
  EXPECT_EQ(prepared->resources.size(), 6U);
  ExpectExactControlledStopPlan(
      prepared->ordinary_stop_plan, runtime.binding.value().resources,
      core::ParticipantStopProfilePurposeDTO::kOperator,
      "operator_soft_on_path", core::MotionInterruptStopMode::kSoftOnPath,
      true, core::MotionInterruptStopMode::kSpringBackHold);
  ASSERT_TRUE(prepared->fault_stop_plan.has_value());
  ExpectExactControlledStopPlan(
      *prepared->fault_stop_plan, runtime.binding.value().resources,
      core::ParticipantStopProfilePurposeDTO::kFault, "fault_spring_back_hold",
      core::MotionInterruptStopMode::kSpringBackHold, false, std::nullopt);
}

TEST(RetainedNativeCCLCRuntime, RetainedProductPublishesRepositoryReadBackReadiness) {
  CheckRetainedProductReadiness(ReadinessCase::kPublication);
}

TEST(RetainedNativeCCLCRuntime, FailedRestartReadinessKeepsAdmissionAndCommandsClosed) {
  CheckRetainedProductReadiness(ReadinessCase::kFailedRestart);
}

TEST(RetainedNativeCCLCRuntime, RepeatedReconciliationAfterReleaseDoesNotReconstructOrDispatch) {
  CheckRetainedProductReadiness(ReadinessCase::kRepeatedReconciliation);
}

TEST(RetainedNativeCCLCRuntime,
     RecoveredCellRejectsStalePhysicalBindingBeforeReadingOrPublishingRoutes) {
  const std::filesystem::path fixture_root =
      MANYFORGE_CCLC_READINESS_FIXTURE_ROOT;
  auto bundle = load_fixture_bundle(fixture_root);
  auto runtime = make_runtime_context(*bundle);
  auto stale_value = runtime.binding.value();
  stale_value.graph_revision = core::GraphRevision(Sha('8'));
  auto stale_binding =
      core::verify_runtime_participant_binding(std::move(stale_value));
  ASSERT_TRUE(stale_binding.ok()) << stale_binding.error().message;
  ASSERT_NE(stale_binding.value().value().graph_revision,
            bundle->value().cell_graph.graph_revision);

  ScopedTrustedRoot state;
  const auto state_root = state.path() / "state";
  const auto run_root = state.path() / "runs";
  ASSERT_TRUE(std::filesystem::create_directory(state_root));
  ASSERT_TRUE(std::filesystem::create_directory(run_root));
  const core::RequestOwnerEpoch owner("cclc.native.stale-binding");
  RetainedNativeCCLCRuntimeConfig config{
      {state_root,
       run_root,
       bundle->value().cell_graph.cell_id,
       {owner, 128U, 2U, 1'000'000LL, 1'000'000'000LL}},
      1'000'000'000LL};
  const deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(
      fixture_root / "retained-package");
  CountingActivePublicationReader publication_reader;
  FixedCatalogSource catalogs(make_prepare_catalog(*runtime.control_binding));
  CountingRoutePublicationOwner route_owner;
  KernelPorts kernel_ports;
  core::FakeClock clock;
  clock.set(100);
  core::ManualScheduler scheduler;

  const auto empty_graph = core::GraphRevision("graph.empty.stale-binding-test");
  core::ResourceAddressedExecutionKernelConfig kernel_config{
      core::RequestOwnerEpoch("kernel.native.stale-binding"),
      bundle->value().cell_graph.cell_id, 32U, {}, nullptr, empty_graph};
  kernel_config.empty_runtime_retained_bundle_id =
      core::RetainedBundleId("bundle.empty.stale-binding-test");
  kernel_config.empty_runtime_graph_revision = empty_graph;
  RetainedNativeCellRuntimeInput cell_input{
      .clock = std::make_shared<WallClock>(),
      .scheduler = std::make_shared<SimpleScheduler>(),
      .operation_journal_path = state_root / "cell-operation-journal.sqlite3",
      .participant_registrations = {},
      .kernel_config = std::move(kernel_config),
      .stop_sources = {}};
  {
    RetainedNativeCellRuntime cell(std::move(cell_input));
    RetainedNativeCellCommandReadinessPort readiness_owner(
        cell, [](const core::ComponentCommandReadinessPublicationDTO &,
                 const std::vector<
                     core::RuntimeParticipantExecutionRegistration> &current) {
          RetainedNativeCellReadinessCandidate candidate;
          candidate.registrations = current;
          candidate.adopt_component_owners = []() noexcept {};
          return core::Result<RetainedNativeCellReadinessCandidate>::Ok(
              std::move(candidate));
        });
    std::function<core::Result<void>()> reconcile_after_start;
    std::size_t predecessor_adoption_calls = 0U;
    const core::ProjectionRevisionDTO projection{
        bundle->retained_bundle_id(),
        bundle->value().cell_graph.graph_revision,
        {core::SceneId("scene.native.stale-binding"), 1U,
         core::ContentDigest(Sha('7'))}};
    const core::ActiveBundlePublicationReceiptDTO expected_publication{
        {core::BootstrapPublicationTransactionDTO{
             core::BootstrapId("bootstrap.native.stale-binding"),
             core::BootstrapGeneration(1U)},
         projection, bundle->retained_bundle_id(),
         bundle->value().cell_graph.graph_revision, std::nullopt,
         core::ContentDigest(Sha('6'))},
        core::ActiveBundlePublicationGeneration(1U),
        core::ContentDigest(Sha('5'))};
    auto recovered = CreateRecoveredRetainedNativeCellCCLCRuntime(
        std::move(config), bundle, publication_reader,
        expected_publication, artifacts, catalogs, {},
        {std::move(stale_binding.value())}, kernel_ports, kernel_ports,
        route_owner, readiness_owner, cell.addressed_kernel(),
        clock, scheduler,
        [&predecessor_adoption_calls](
            const core::ActiveControlConfigurationDTO &) {
          ++predecessor_adoption_calls;
          return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
              {"unexpected_predecessor_adoption",
               "stale-binding preflight adopted a route", {}});
        },
        [] { return core::Result<void>::Ok(); }, reconcile_after_start);

    ASSERT_FALSE(recovered.ok());
    EXPECT_EQ(recovered.error().details.at("reason"),
              "native_cell_restart_binding_invalid");
    EXPECT_EQ(publication_reader.read_calls, 0U);
    EXPECT_EQ(route_owner.physical_reads, 0U);
    EXPECT_EQ(route_owner.publications, 0U);
    EXPECT_EQ(predecessor_adoption_calls, 0U);
    EXPECT_FALSE(reconcile_after_start);
  }
}

} // namespace
} // namespace manyforge::roboplan_backend
