#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iterator>
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
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/retained_runtime_policy.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/retained_native_cclc_runtime.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

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
make_runtime_context(const deployment::RetainedBundle &bundle) {
  const auto &graph = bundle.value().cell_graph;
  const core::CapabilityId trajectory_execute_capability_id(
      "execution.trajectory.execute");
  const auto &binding = require_one(
      graph.control_bindings, trajectory_execute_capability_id,
      [](const auto &value) { return value.capability_id; },
      "trajectory control binding");
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
                         symbol, symbol, core::CoordinateKind::kAngular});
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
  std::vector<core::RuntimeUnitEpochBindingDTO> runtime_unit_epochs{
      {binding.provider_runtime_unit_id, runtime_epoch}};
  if (*planner_runtime_units.begin() != binding.provider_runtime_unit_id) {
    runtime_unit_epochs.push_back(
        {*planner_runtime_units.begin(), runtime_epoch});
    std::sort(runtime_unit_epochs.begin(), runtime_unit_epochs.end(),
              [](const auto &lhs, const auto &rhs) {
                return lhs.runtime_unit_id < rhs.runtime_unit_id;
              });
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
      {planner_path},
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

[[nodiscard]] core::BrandLifecyclePlanCatalogDTO
make_prepare_catalog(const core::ControlBindingDTO &binding) {
  const core::LifecyclePredicateDTO declared{
      {core::ComponentLifecycleStateDTO::kDeclared}, {}};
  const core::LifecyclePredicateDTO prepared{
      {core::ComponentLifecycleStateDTO::kPrepared}, {}};
  auto result = core::MakeBrandLifecyclePlanCatalog(
      binding.provider_manifest_id, binding.provider_manifest_revision,
      {{core::ComponentLifecycleIntentKindDTO::kPrepare,
        core::ComponentLifecycleStateDTO::kPrepared,
        std::nullopt,
        prepared,
        1'000'000'000LL,
        {{core::LifecyclePrimitiveId("prepare"), declared, prepared}}}});
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
    const auto state = prepared_.load()
                           ? core::ComponentLifecycleStateDTO::kPrepared
                           : core::ComponentLifecycleStateDTO::kDeclared;
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
    if (cancellation.stop_requested() ||
        intent.lifecycle_primitive_id !=
            core::LifecyclePrimitiveId("prepare")) {
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
    prepared_.store(true);
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
  std::atomic<bool> prepared_{false};
};

class KernelPorts final : public core::ICCLCKernelDisableHandoffPort,
                          public core::ICCLCKernelRouteConfigurationFencePort,
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
  [[nodiscard]] core::Result<core::DisableHandoffAdmissionSnapshotDTO>
  ObserveDisableHandoffAdmission(
      const core::DisableHandoffAdmissionQueryDTO &) const override {
    return unused<core::DisableHandoffAdmissionSnapshotDTO>();
  }
  [[nodiscard]] core::Result<core::DisableHandoffAcceptedDTO>
  BeginDisableHandoff(const core::DisableHandoffRequestDTO &) override {
    return unused<core::DisableHandoffAcceptedDTO>();
  }
  [[nodiscard]] core::Result<core::DisableHandoffStatusDTO>
  DisableHandoffStatus(const core::LifecycleTransactionId &,
                       core::LifecycleTransactionGeneration) override {
    return unused<core::DisableHandoffStatusDTO>();
  }
  [[nodiscard]] core::Result<core::DisableHandoffAttemptReconciliationDTO>
  ReconcileDisableHandoffAttempt(
      const core::DisableHandoffRequestDTO &) override {
    return unused<core::DisableHandoffAttemptReconciliationDTO>();
  }
  [[nodiscard]] core::Result<core::DisableFenceReleasedDTO>
  ReleaseDisableFence(const core::DisableFenceReleaseRequestDTO &) override {
    return unused<core::DisableFenceReleasedDTO>();
  }
  [[nodiscard]]
  core::Result<core::DisableFenceReleaseAttemptReconciliationDTO>
  ReconcileDisableFenceReleaseAttempt(
      const core::DisableFenceReleaseRequestDTO &) override {
    return unused<core::DisableFenceReleaseAttemptReconciliationDTO>();
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

class AdmissionEngine final
    : public core::IResolvedParticipantExecutionEngine,
      public core::IResolvedPreparedParticipantExecutionEngine,
      public core::IControlDomainExecutionPort {
public:
  [[nodiscard]] core::Result<std::uint64_t>
  execute(const core::ExecuteRequestDTO &) override {
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
  PrepareResumeMotion(std::uint64_t,
                      core::ResolvedPreparedMotionResumeRequestDTO) override {
    return Unsupported<void>();
  }

  [[nodiscard]] core::Result<void>
  DispatchPreparedResumeMotion(std::uint64_t,
                               const core::ContentDigest &) override {
    return Unsupported<void>();
  }

  [[nodiscard]] core::Result<void>
  CancelPreparedResumeMotion(std::uint64_t) override {
    return Unsupported<void>();
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

  [[nodiscard]] core::PauseResultDTO pause() override {
    return {true, std::nullopt};
  }
  [[nodiscard]] core::ResumeResultDTO
  resume(std::optional<std::uint64_t>) override {
    return {true, std::nullopt};
  }
  [[nodiscard]] core::ResetResultDTO
  reset(std::optional<std::uint64_t>) override {
    return {true, std::nullopt};
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
        {authority.runtime_policy.value().source_artifact,
         authority.runtime_policy.value().selected_motion_speed_limit},
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
        &authority.runtime_policy, authority.retained_default_stop_limits,
        authority.stop_node_category);
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
      core::FakeClock &clock, core::RuntimePolicyDTO runtime_policy,
      core::RuntimePolicyCatalogRecordRefDTO runtime_policy_source_artifact)
      : binding_(std::move(binding)), journal_path_(std::move(journal_path)),
        cell_id_(std::move(cell_id)), clock_(clock),
        runtime_policy_(std::move(runtime_policy)),
        runtime_policy_source_artifact_(
            std::move(runtime_policy_source_artifact)) {}

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
    auto runtime_policy = core::verify_runtime_participant_policy_selection(
        runtime_policy_source_artifact_, runtime_policy_,
        participants.front().participant_binding);
    if (!runtime_policy.ok()) {
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          runtime_policy.error());
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
        participants.front().participant_binding,
        participants.front().stop_readiness,
        std::move(hard_limits),
        core::MotionInterruptNodeCategory::kGeneric,
        &engine_,
        &engine_,
        std::move(runtime_policy.value()),
        &engine_,
        std::move(operator_profile.value()),
        std::move(fault_profile)};
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
              participants.front().participant_binding.value().graph_revision,
              participants.front()
                  .stop_readiness.value()
                  .resolved_stop_base_plan.deadline_budget});
    }
    auto published = kernel_->publish_runtime_route_configuration(
        configuration, {std::move(registration)});
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
  core::RuntimePolicyDTO runtime_policy_;
  core::RuntimePolicyCatalogRecordRefDTO runtime_policy_source_artifact_;
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
    bool expected_natural_completion) {
  EXPECT_EQ(plan.participant_stop_profile_purpose, expected_purpose);
  ASSERT_TRUE(plan.participant_stop_profile.has_value());
  const auto &profile = *plan.participant_stop_profile;
  EXPECT_EQ(profile.profile_id, expected_profile_id);
  EXPECT_EQ(profile.stop_mode, expected_mode);
  ASSERT_TRUE(profile.deceleration_time_s.has_value());
  EXPECT_DOUBLE_EQ(*profile.deceleration_time_s, 1.0);
  EXPECT_EQ(profile.allow_natural_completion_on_short_path,
            expected_natural_completion);
  EXPECT_FALSE(profile.recoverable_substitution_mode.has_value());
  EXPECT_TRUE(profile.require_backend_confirmation);
  ASSERT_EQ(profile.per_joint_max_deceleration.size(), 6U);
  for (const auto &[joint, limit] : profile.per_joint_max_deceleration) {
    (void)joint;
    const auto *angular = std::get_if<core::AngularAccelerationDTO>(&limit);
    ASSERT_NE(angular, nullptr);
    EXPECT_DOUBLE_EQ(angular->value_rad_s2, 4.0);
  }

  ASSERT_EQ(plan.deadline_budget.outer_duration_ns, 3'000'000'000);
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

TEST(RetainedNativeCCLCRuntime,
     RetainedProductPublishesRepositoryReadBackReadiness) {
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
       core::ContentDigest(Sha('7')), core::ContentDigest(Sha('8'))});
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
       {owner, 128U, 2U, 2U, 1'000'000LL, 1'000'000'000LL, 1'000'000'000LL}},
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
  const deployment::FilesystemCatalogArtifactStore policy_artifacts(
      fixture_root / "retained-package");
  const auto runtime_policy =
      deployment::resolve_retained_runtime_policy(*bundle, policy_artifacts);
  ASSERT_TRUE(runtime_policy.has_value());
  core::FakeClock clock;
  clock.set(1'000);
  PublicationOwner publication_owner(
      runtime.binding, state.path() / "operation-journal.sqlite3",
      bundle->value().cell_graph.cell_id, clock, runtime_policy->value(),
      runtime_policy->reference());
  core::ManualScheduler scheduler;

  auto composition = CreateRetainedNativeCCLCRuntime(
      config, bundle, publication_store, publication.value(), artifacts,
      catalogs, {{occurrence, brand_port}}, {runtime.binding}, kernel_ports,
      kernel_ports, kernel_ports, publication_owner, clock, scheduler);
  ASSERT_TRUE(composition.ok()) << composition.error().message;
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

  core::RouteSelectionIntentDTO route{
      {core::RequestId("route.native.readiness"), current_request_owner, "test",
       std::nullopt},
      bundle->retained_bundle_id(),
      bundle->value().cell_graph.graph_revision,
      *runtime.desired,
      {core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
       std::nullopt, std::nullopt},
      {{runtime.component->controllable_component_id,
        lifecycle_status.value()->acceptance.component_lifecycle_generation,
        core::ComponentLifecycleStateDTO::kPrepared}},
      {core::RequestTransportDTO::kInternal,
       core::RequestAssuranceDTO::kUnverified, std::nullopt},
      kernel_ports.GetAuthoringAdmission().admission_generation};
  const auto route_result =
      composition.value()->coordinator().SubmitRouteSelection(route);
  ASSERT_TRUE(route_result.ok()) << route_result.error().message;
  ASSERT_TRUE(route_result.value().acceptance.has_value());
  EXPECT_TRUE(
      composition.value()->coordinator().DrainFor(std::chrono::seconds(5)));
  const auto route_status =
      composition.value()->coordinator().LoadRouteSelectionStatus(
          route_result.value().acceptance->route_selection_transaction_id);
  ASSERT_TRUE(route_status.ok()) << route_status.error().message;
  ASSERT_TRUE(route_status.value().has_value());
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
      true);
  ASSERT_TRUE(prepared->fault_stop_plan.has_value());
  ExpectExactControlledStopPlan(
      *prepared->fault_stop_plan, runtime.binding.value().resources,
      core::ParticipantStopProfilePurposeDTO::kFault, "fault_spring_back_hold",
      core::MotionInterruptStopMode::kSpringBackHold, false);
}

} // namespace
} // namespace manyforge::roboplan_backend
