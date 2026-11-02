#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_component_manager/runtime_observation.hpp>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "manyforge_headless/owner_publication.hpp"
#include "assistant_authoring_fixture.hpp"

namespace manyforge::headless {
namespace {

namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kSceneId =
    "sha256:abababababababababababababababababababababababababababababababab";

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-headless-owner-publication-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("owner-publication test mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

deployment::CanonicalEmptyRetainedBundleAdmission LoadAdmission() {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  return source.load_verified();
}

core::SceneRevision SceneRevision() {
  return {
      core::SceneId(std::string(kSceneId)),
      1U,
      core::ContentDigest(std::string(kDigestOne)),
  };
}

core::ProjectionRevisionDTO ProjectionRevision(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  return {
      admission.retained_bundle_id(),
      admission.graph_revision(),
      SceneRevision(),
  };
}

core::ActiveBundlePublicationReceiptDTO Publication(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  const auto projection = ProjectionRevision(admission);
  return {
      core::ActiveBundlePublicationRequestDTO{
          core::BootstrapPublicationTransactionDTO{
              core::BootstrapId("bootstrap-1"),
              core::BootstrapGeneration(1U),
          },
          projection,
          projection.retained_bundle_id,
          projection.graph_revision,
          std::nullopt,
          core::ContentDigest(std::string(kDigestOne)),
      },
      core::ActiveBundlePublicationGeneration(1U),
      core::ContentDigest(std::string(kDigestTwo)),
  };
}

component_manager::ManagerActiveRuntimeConvergenceResult
Runtime(const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  component_manager::RuntimeObservationReducer reducer([] { return 42U; }, {});
  return {
      component_manager::ManagerActiveRuntimePlanDTO{
          Publication(admission),
          std::nullopt,
          {},
      },
      reducer.Evaluate({}),
      component_manager::ManagerActiveRuntimeConvergenceState::kConverged,
  };
}

component_manager::RuntimeUnitConvergencePlanRowDTO RuntimeRow() {
  const auto requirement = core::MakeRuntimeContractRequirement(
      core::RuntimeImplementationFamilyId("runtime-publication-family"), {}, {});
  if (!requirement.ok()) {
    throw std::runtime_error("owner-publication test requirement is invalid");
  }
  return {
      core::RuntimeUnitId("runtime.publication"),
      core::RuntimeProfileId("profile.reference"),
      core::RuntimeProfileRevision("profile-revision-1"),
      core::RuntimeLifecycleControl::kManagerManaged,
      component_manager::RuntimeUnitCandidateDisposition::kAdded,
      core::ProductId("runtime-publication-product"),
      core::ProductContractId("runtime-publication-contract"),
      core::ProductContractRevision("runtime-publication-contract-revision"),
      core::ContentDigest(std::string(kDigestOne)),
      core::ContentDigest(std::string(kDigestTwo)),
      {},
      {core::RuntimeRoleId("status")},
      core::EndpointRoleId("status"),
      "manyforge.runtime-publication.status",
      "status-revision-1",
      "private-controller",
      "manyforge.posix_sh.retained_script",
      "adapter-revision-1",
      "retained-raw-artifact",
      "retrieval-revision-1",
      core::ContentDigest(std::string(kDigestTwo)),
      1U,
      "linux",
      "posix.1-2017",
      requirement.value(),
      core::RuntimeContractOfferDigest(std::string(kDigestOne)),
  };
}

component_manager::RuntimeUnitExpectation RuntimeExpectation() {
  return {
      "runtime.publication",
      component_manager::RuntimeLifecycleControl::kManagerManaged,
      std::string(kDigestOne),
      std::string(kDigestTwo),
      100U,
      100U,
      std::nullopt,
      {},
      {"status"},
      component_manager::ManagedCommandSurfaceState::kClosedNoActiveEffect,
      std::string(kDigestOne),
  };
}

component_manager::RuntimeObservationSnapshot ObservationWithoutDigest() {
  component_manager::RuntimeObservationReducer reducer(
      [] { return 42U; }, {"runtime.publication"});
  return reducer.Evaluate({RuntimeExpectation()});
}

component_manager::RuntimeObservationSnapshot ObservationWithDigest(
    const std::optional<std::uint64_t> dropped_lines = std::nullopt,
    const std::optional<std::uint64_t> dropped_bytes = std::nullopt) {
  component_manager::RuntimeObservationReducer reducer(
      [] { return 42U; }, {"runtime.publication"});
  if (!reducer.ObserveProcess(component_manager::ProcessPresenceObservation(
          "runtime.publication", "owner-publication-observer", 1U,
          component_manager::ProcessPresenceState::kPresent,
          "runtime-publication-epoch")) ||
      !reducer.IngestSourceReport(component_manager::RuntimeUnitSourceReport(
          "runtime.publication", "runtime-publication-epoch", 1U, 1U,
          component_manager::RuntimeSourcePhase::kServing,
          component_manager::RuntimeSourceHealth::kHealthy,
          std::string(kDigestOne), std::string(kDigestTwo), {"status"},
          component_manager::ManagedCommandSurfaceState::kClosedNoActiveEffect,
          {}, std::string(kDigestOne), dropped_lines, dropped_bytes))) {
    throw std::runtime_error("owner-publication test observation was rejected");
  }
  return reducer.Evaluate({RuntimeExpectation()});
}

component_manager::ManagerActiveRuntimeConvergenceResult RuntimeWithRow(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    component_manager::RuntimeObservationSnapshot observations,
    const component_manager::ManagerActiveRuntimeConvergenceState state) {
  return {
      component_manager::ManagerActiveRuntimePlanDTO{
          Publication(admission),
          core::ContentDigest(std::string(kDigestTwo)),
          {RuntimeRow()},
      },
      std::move(observations),
      state,
  };
}

component_manager::ManagerApplyIntentDTO ApplyIntent(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  return {
      core::PublicRequestContextDTO{
          core::RequestId("request-1"),
          core::RequestOwnerEpoch("manager-client-epoch-1"),
          "owner-publication-test",
          std::nullopt,
      },
      core::ApplyId("apply-1"),
      core::ApplyGeneration(1U),
      admission.cell_id(),
      ProjectionRevision(admission),
      admission.retained_bundle_id(),
      admission.graph_revision(),
      component_manager::ManagerApplyPreparedActivationDTO{
          core::ContentDigest(std::string(kDigestOne)),
          core::DeploymentArtifactId("owner-publication-deployment"),
          core::DeploymentRevision(std::string(kDigestTwo)),
          core::ProgramBindingId(std::string(kDigestOne)),
          core::ContentDigest(std::string(kDigestTwo)),
          R"({"schema_version":"test.prepared_activation_receipt.v1"})",
      },
      core::AdmissionGeneration(7U),
      std::nullopt,
  };
}

core::LogicalSceneSnapshotRoot SceneSnapshot(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  core::CollisionPolicySnapshot policy;
  policy.policy_id = "free_space";
  policy.revision = {
      1U,
      std::string(core::kCanonicalEmptyCollisionPolicyDigest),
  };
  return core::LogicalSceneSnapshotRoot(
      std::make_shared<const core::LogicalSceneSnapshotDTO>(
          core::LogicalSceneSnapshotDTO{
              SceneRevision(),
              admission.cell_id(),
              1U,
              1U,
              1U,
              std::move(policy),
              {},
              {},
          }));
}

core::LogicalSceneSnapshotRoot SceneSnapshotWithSupport(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  auto policy = SceneSnapshot(admission).shared()->configured_collision_policy;
  const auto fixed_placement = core::CellFixedSceneObjectPlacementDTO{
      admission.cell_id(),
      core::Pose3DTO{
          core::Vector3DTO{0.0, 0.0, 0.0},
          core::QuaternionDTO{0.0, 0.0, 0.0, 1.0},
      },
  };
  return core::LogicalSceneSnapshotRoot(
      std::make_shared<const core::LogicalSceneSnapshotDTO>(
          core::LogicalSceneSnapshotDTO{
              SceneRevision(),
              admission.cell_id(),
              2U,
              2U,
              1U,
              std::move(policy),
              {
                  core::SceneObjectSnapshotDTO{
                      core::SceneResourceId("part"),
                      {core::SphereShapeDTO{0.01}},
                      fixed_placement,
                      std::nullopt,
                      {core::SupportContactDTO{.support_object_id = "table"}},
                  },
                  core::SceneObjectSnapshotDTO{
                      core::SceneResourceId("table"),
                      {core::BoxShapeDTO{core::Vector3DTO{1.0, 1.0, 0.1}}},
                      fixed_placement,
                      std::nullopt,
                      {},
                  },
              },
              {},
          }));
}

TEST(ManagerSourcePublication,
     ProjectsVerifiedActiveGraphAndExplicitKernelUnavailability) {
  const auto admission = LoadAdmission();
  TemporaryDirectory directory;
  component_manager::SqliteManagerApplyJournal journal(
      directory.path() / "apply.sqlite3", admission.cell_id());

  const auto payload = BuildManagerSourcePayload(
      admission.retained_bundle(), admission, Runtime(admission), journal,
      ManagerApplyMutationAvailability::kKernelOwnerUnavailable);
  const auto bytes = content_identity::canonical_json(payload);
  EXPECT_NE(bytes.find("\"api_version\":\"manyforge.manager_source.v1\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"cell_graph\":"), std::string::npos);
  const auto *const active = payload.find_object_member("active");
  ASSERT_NE(active, nullptr);
  const auto *const canonical_empty = active->find_object_member("canonical_empty");
  ASSERT_NE(canonical_empty, nullptr);
  ASSERT_TRUE(canonical_empty->is_bool());
  EXPECT_TRUE(canonical_empty->as_bool());
  const auto *const dependencies =
      active->find_object_member("component_simulator_dependencies");
  ASSERT_NE(dependencies, nullptr);
  ASSERT_TRUE(dependencies->is_array());
  EXPECT_TRUE(dependencies->as_array().empty());
  EXPECT_NE(bytes.find("\"manager_ready\":true"), std::string::npos);
  EXPECT_NE(bytes.find("\"mutation_available\":false"), std::string::npos);
  EXPECT_NE(
      bytes.find("\"unavailable_reason_code\":\"kernel_owner_unavailable\""),
      std::string::npos);
  EXPECT_NE(bytes.find("\"records\":[]"), std::string::npos);
}

TEST(ManagerSourcePublication, NamedClosePublicationIsCanonicalEmptyByAdmissionIdentity) {
  const auto admission = LoadAdmission();
  TemporaryDirectory directory;
  component_manager::SqliteManagerApplyJournal journal(
      directory.path() / "apply.sqlite3", admission.cell_id());
  auto runtime = Runtime(admission);
  runtime.plan.active_publication.request.transaction_ref =
      core::NamedApplyPublicationTransactionDTO{core::ApplyId("apply.close"), core::ApplyGeneration(1U)};
  runtime.plan.active_publication.request.expected_previous_publication_generation =
      core::ActiveBundlePublicationGeneration(1U);
  runtime.plan.active_publication.publication_generation = core::ActiveBundlePublicationGeneration(2U);
  runtime.plan.runtime_convergence_intent_digest = core::ContentDigest(std::string(kDigestOne));
  ASSERT_TRUE(runtime.plan.valid());
  const auto payload = BuildManagerSourcePayload(admission.retained_bundle(), admission,
      runtime, journal, ManagerApplyMutationAvailability::kAvailable);
  const auto *const active = payload.find_object_member("active");
  ASSERT_NE(active, nullptr);
  const auto *const canonical_empty = active->find_object_member("canonical_empty");
  ASSERT_NE(canonical_empty, nullptr);
  ASSERT_TRUE(canonical_empty->is_bool());
  EXPECT_TRUE(canonical_empty->as_bool());
  EXPECT_NE(content_identity::canonical_json(*active).find("\"apply_id\":\"apply.close\""), std::string::npos);
}

TEST(ManagerSourcePublication, VerifiedNonemptyProductIsNotCanonicalEmpty) {
  const auto admission = LoadAdmission();
  const auto bundle = test::LoadAssistantAuthoringBundle();
  ASSERT_NE(bundle, nullptr);
  ASSERT_TRUE(deployment::verify_retained_bundle_identity(*bundle));
  ASSERT_NE(bundle->retained_bundle_id(), admission.retained_bundle_id());
  const auto *const graph = bundle->normalized_root().find_object_member("cell_graph");
  ASSERT_NE(graph, nullptr);
  // The existing independently verified assistant fixture actually contains
  // configured components; the production predicate does not count them.
  const auto *const components = graph->find_object_member("controllable_components");
  ASSERT_NE(components, nullptr);
  ASSERT_TRUE(components->is_array());
  ASSERT_FALSE(components->as_array().empty());
  TemporaryDirectory directory;
  component_manager::SqliteManagerApplyJournal journal(
      directory.path() / "apply.sqlite3", bundle->value().cell_graph.cell_id);
  auto runtime = Runtime(admission);
  const core::ProjectionRevisionDTO projection{
      bundle->retained_bundle_id(), bundle->value().cell_graph.graph_revision, SceneRevision()};
  runtime.plan.active_publication.request.candidate_projection_revision = projection;
  runtime.plan.active_publication.request.candidate_retained_bundle_id = projection.retained_bundle_id;
  runtime.plan.active_publication.request.candidate_graph_revision = projection.graph_revision;
  runtime.plan.active_publication.request.transaction_ref =
      core::NamedApplyPublicationTransactionDTO{core::ApplyId("apply.nonempty"), core::ApplyGeneration(1U)};
  runtime.plan.active_publication.request.expected_previous_publication_generation =
      core::ActiveBundlePublicationGeneration(1U);
  runtime.plan.active_publication.publication_generation = core::ActiveBundlePublicationGeneration(2U);
  runtime.plan.runtime_convergence_intent_digest = core::ContentDigest(std::string(kDigestOne));
  ASSERT_TRUE(runtime.plan.valid());
  const auto payload = BuildManagerSourcePayload(*bundle, admission, runtime, journal,
      ManagerApplyMutationAvailability::kAvailable);
  const auto *const canonical_empty = payload.find_object_member("active")->find_object_member("canonical_empty");
  ASSERT_NE(canonical_empty, nullptr);
  ASSERT_TRUE(canonical_empty->is_bool());
  EXPECT_FALSE(canonical_empty->as_bool());
  // CP8 step 1: the deployment-derived simulator row of each ROS-hosted
  // component travels with the active graph without changing its identity.
  const auto *const dependencies = payload.find_object_member("active")->find_object_member(
      "component_simulator_dependencies");
  ASSERT_NE(dependencies, nullptr);
  ASSERT_TRUE(dependencies->is_array());
  ASSERT_FALSE(dependencies->as_array().empty());
  EXPECT_EQ(*dependencies, deployment::ros2_control_execution_runtime_contract::
                               component_simulator_dependencies(*bundle));
  for (const auto &row : dependencies->as_array()) {
    ASSERT_TRUE(row.is_object());
    EXPECT_EQ(row.as_object().size(), 2U);
    ASSERT_NE(row.find_object_member("controllable_component_id"), nullptr);
    const auto *const simulator = row.find_object_member("simulator");
    ASSERT_NE(simulator, nullptr);
    EXPECT_TRUE(*simulator == content_identity::CanonicalValue("none") ||
                *simulator == content_identity::CanonicalValue("isaac"));
  }
  EXPECT_EQ(*payload.find_object_member("active")->find_object_member("cell_graph"),
            *graph);
}

TEST(ManagerSourcePublication,
     ProjectsOneDurableIncompleteApplyWithoutInventingKernelState) {
  const auto admission = LoadAdmission();
  TemporaryDirectory directory;
  component_manager::SqliteManagerApplyJournal journal(
      directory.path() / "apply.sqlite3", admission.cell_id());
  const auto intent = journal.EnsureIntent(ApplyIntent(admission)).record;
  static_cast<void>(journal.EnsureAffectedScope(
      intent.intent_digest,
      component_manager::ManagerApplyAffectedScopeDTO{
          intent.intent.apply_id,
          intent.intent.apply_generation,
          {},
          core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
          deployment::derive_apply_component_delta(
              admission.retained_bundle(), admission.retained_bundle(), deployment::SiteFacts{}),
      }));

  const auto bytes = content_identity::canonical_json(BuildManagerSourcePayload(
      admission.retained_bundle(), admission, Runtime(admission), journal,
      ManagerApplyMutationAvailability::kKernelOwnerUnavailable));
  EXPECT_NE(bytes.find("\"apply_id\":\"apply-1\""), std::string::npos);
  EXPECT_NE(bytes.find("\"phase\":\"scope_persisted\""), std::string::npos);
  EXPECT_NE(bytes.find("\"state\":\"scope_persisted\""), std::string::npos);
  EXPECT_NE(bytes.find("\"incomplete\":true"), std::string::npos);
  EXPECT_NE(bytes.find("\"manager_ready\":false"), std::string::npos);
  EXPECT_NE(bytes.find("\"converged\":false"), std::string::npos);
  EXPECT_EQ(bytes.find("\"configuration_fence_id\":\""), std::string::npos);
}

TEST(ManagerSourcePublication,
     RefusesARuntimeThatPredatesACommittedApplyPublication) {
  // The live race: the Apply progression lane published a transition value
  // from the last runtime observed before the Apply (active publication N)
  // beside a journal that already held the Apply's receipt for publication
  // N+1. Readers then saw a committed Apply that no publication accounted
  // for (program_activation_history_incomplete). Such a pair is one
  // generation apart and is never projected.
  const auto admission = LoadAdmission();
  TemporaryDirectory directory;
  component_manager::SqliteManagerApplyJournal journal(
      directory.path() / "apply.sqlite3", admission.cell_id());
  static_cast<void>(journal.EnsureIntent(ApplyIntent(admission)));
  const auto runtime = Runtime(admission);
  auto inventory = journal.ReadStatusInventory();
  ASSERT_EQ(inventory.size(), 1U);
  // Not yet published: the active publication accounts for it trivially.
  EXPECT_TRUE(ManagerRuntimeAccountsForCommittedApplies(runtime, inventory));

  auto committed = runtime.plan.active_publication;
  committed.publication_generation = core::ActiveBundlePublicationGeneration(
      runtime.plan.active_publication.publication_generation.value() + 1U);
  committed.durable_public_pointer_content_digest =
      core::ContentDigest(std::string(kDigestOne));
  inventory.front().publication_receipt =
      component_manager::PersistedManagerApplyPublicationReceipt{
          component_manager::ManagerApplyPublicationReceiptDTO{
              core::ContentDigest(std::string(kDigestOne)), committed},
          core::ContentDigest(std::string(kDigestTwo)),
      };
  EXPECT_FALSE(ManagerRuntimeAccountsForCommittedApplies(runtime, inventory));
  EXPECT_THROW(static_cast<void>(BuildManagerSourcePayload(
                   admission.retained_bundle(), admission, runtime, journal, inventory,
                   ManagerApplyMutationAvailability::kAvailable)),
               std::invalid_argument);

  // Once the observed runtime holds that publication, the pair is current.
  auto current = runtime;
  current.plan.active_publication = committed;
  EXPECT_TRUE(ManagerRuntimeAccountsForCommittedApplies(current, inventory));

  // A receipt at the active generation must be that exact publication.
  auto foreign = committed;
  foreign.durable_public_pointer_content_digest =
      core::ContentDigest(std::string(kDigestTwo));
  inventory.front().publication_receipt->publication.publication_receipt =
      foreign;
  EXPECT_FALSE(ManagerRuntimeAccountsForCommittedApplies(current, inventory));
}

TEST(ManagerSourcePublication,
     PackageGenerationFailureClosesReadinessWithTypedReason) {
  const auto admission = LoadAdmission();
  TemporaryDirectory directory;
  component_manager::SqliteManagerApplyJournal journal(
      directory.path() / "apply.sqlite3", admission.cell_id());

  const auto bytes = content_identity::canonical_json(BuildManagerSourcePayload(
      admission.retained_bundle(), admission, Runtime(admission), journal,
      ManagerApplyMutationAvailability::kFullStackRestartRequired));
  EXPECT_NE(bytes.find("\"manager_ready\":false"), std::string::npos);
  EXPECT_NE(bytes.find("\"converged\":false"), std::string::npos);
  EXPECT_NE(bytes.find("\"mutation_available\":false"), std::string::npos);
  EXPECT_NE(bytes.find(
                "\"unavailable_reason_code\":\"full_stack_restart_required\""),
            std::string::npos);
}

TEST(ManagerSourcePublication, ProjectsSelectedAndObservedOfferDigests) {
  const auto admission = LoadAdmission();
  TemporaryDirectory directory;
  component_manager::SqliteManagerApplyJournal journal(
      directory.path() / "apply.sqlite3", admission.cell_id());

  const auto bytes = content_identity::canonical_json(BuildManagerSourcePayload(
      admission.retained_bundle(), admission,
      RuntimeWithRow(admission, ObservationWithDigest(),
                     component_manager::ManagerActiveRuntimeConvergenceState::
                         kConverged),
      journal, ManagerApplyMutationAvailability::kKernelOwnerUnavailable));
  EXPECT_NE(bytes.find("\"selected_runtime_contract_offer_digest\":\"" +
                       std::string(kDigestOne) + "\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"observed_runtime_contract_offer_digest\":\"" +
                       std::string(kDigestOne) + "\""),
            std::string::npos);
}

TEST(ManagerSourcePublication, HostNoticeCountsPreserveFullWidthAndReadiness) {
  const auto admission = LoadAdmission();
  TemporaryDirectory directory;
  component_manager::SqliteManagerApplyJournal journal(
      directory.path() / "apply.sqlite3", admission.cell_id());
  const auto publish = [&](const std::optional<std::uint64_t> lines,
                           const std::optional<std::uint64_t> bytes) {
    return content_identity::canonical_json(BuildManagerSourcePayload(
        admission.retained_bundle(), admission,
        RuntimeWithRow(admission, ObservationWithDigest(lines, bytes),
                       component_manager::ManagerActiveRuntimeConvergenceState::
                           kConverged),
        journal, ManagerApplyMutationAvailability::kKernelOwnerUnavailable));
  };
  const auto absent = publish(std::nullopt, std::nullopt);
  EXPECT_EQ(absent.find("dropped_host_notice_"), std::string::npos);
  const auto zero = publish(0U, 0U);
  EXPECT_NE(zero.find("\"dropped_host_notice_lines\":0"), std::string::npos);
  const auto boundary = publish(9007199254740991ULL, 9007199254740992ULL);
  EXPECT_NE(boundary.find("\"dropped_host_notice_lines\":9007199254740991"),
            std::string::npos);
  EXPECT_NE(boundary.find("\"dropped_host_notice_bytes\":\"9007199254740992\""),
            std::string::npos);
  const auto full = publish(18446744073709551615ULL, 18446744073709551615ULL);
  EXPECT_NE(full.find("\"dropped_host_notice_lines\":\"18446744073709551615\""),
            std::string::npos);
  EXPECT_NE(full.find("\"dropped_host_notice_bytes\":\"18446744073709551615\""),
            std::string::npos);
  for (const auto &payload : {absent, zero, boundary, full}) {
    EXPECT_NE(payload.find("\"manager_ready\":true"), std::string::npos);
    EXPECT_NE(payload.find("\"source_health\":\"healthy\""), std::string::npos);
    EXPECT_NE(payload.find("\"source_reason_codes\":[]"), std::string::npos);
  }
}

TEST(ManagerSourcePublication, ProjectsMissingObservedOfferDigestAsNull) {
  const auto admission = LoadAdmission();
  TemporaryDirectory directory;
  component_manager::SqliteManagerApplyJournal journal(
      directory.path() / "apply.sqlite3", admission.cell_id());

  const auto bytes = content_identity::canonical_json(BuildManagerSourcePayload(
      admission.retained_bundle(), admission,
      RuntimeWithRow(admission, ObservationWithoutDigest(),
                     component_manager::ManagerActiveRuntimeConvergenceState::
                         kPending),
      journal, ManagerApplyMutationAvailability::kKernelOwnerUnavailable));
  EXPECT_NE(bytes.find("\"selected_runtime_contract_offer_digest\":\"" +
                       std::string(kDigestOne) + "\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"observed_runtime_contract_offer_digest\":null"),
            std::string::npos);
}

TEST(LogicalSceneSourcePublication,
     DistinguishesUnavailableBootstrapFromAnActiveImmutableSnapshot) {
  const auto admission = LoadAdmission();
  const auto unavailable = BuildLogicalSceneSourcePayload(
      manyforge::scene::CanonicalEmptySceneAccessStatus{
          manyforge::scene::CanonicalEmptySceneBootstrapPhase::
              kInitialCommandFenced,
          false,
          false,
          std::nullopt,
      },
      std::nullopt);
  const auto unavailable_bytes = content_identity::canonical_json(unavailable);
  EXPECT_NE(unavailable_bytes.find("\"phase\":\"initial_command_fenced\""),
            std::string::npos);
  EXPECT_NE(unavailable_bytes.find("\"snapshot\":null"), std::string::npos);

  const auto projection = ProjectionRevision(admission);
  const auto snapshot = SceneSnapshot(admission);
  const auto active = BuildLogicalSceneSourcePayload(
      manyforge::scene::CanonicalEmptySceneAccessStatus{
          manyforge::scene::CanonicalEmptySceneBootstrapPhase::kFinalized,
          true,
          true,
          projection,
      },
      snapshot);
  const auto active_bytes = content_identity::canonical_json(active);
  EXPECT_NE(active_bytes.find("\"phase\":\"finalized\""), std::string::npos);
  EXPECT_NE(active_bytes.find("\"scene_writes_open\":true"), std::string::npos);
  EXPECT_NE(active_bytes.find("\"policy_id\":\"free_space\""),
            std::string::npos);
  EXPECT_NE(active_bytes.find("\"objects\":[]"), std::string::npos);
}

TEST(LogicalSceneSourcePublication, RejectsCrossOwnerRevisionMismatch) {
  const auto admission = LoadAdmission();
  auto projection = ProjectionRevision(admission);
  projection.scene_revision.revision_id = 2U;
  EXPECT_THROW(
      BuildLogicalSceneSourcePayload(
          manyforge::scene::CanonicalEmptySceneAccessStatus{
              manyforge::scene::CanonicalEmptySceneBootstrapPhase::kFinalized,
              true,
              true,
              projection,
          },
          SceneSnapshot(admission)),
      std::invalid_argument);
}

TEST(LogicalSceneSourcePublication,
     PublishesPortableSupportDeclarationWithoutNativeBodyNames) {
  const auto admission = LoadAdmission();
  const auto projection = ProjectionRevision(admission);
  const auto bytes =
      content_identity::canonical_json(BuildLogicalSceneSourcePayload(
          manyforge::scene::CanonicalEmptySceneAccessStatus{
              manyforge::scene::CanonicalEmptySceneBootstrapPhase::kFinalized,
              true,
              true,
              projection,
          },
          SceneSnapshotWithSupport(admission)));

  EXPECT_NE(bytes.find("\"support_object_id\":\"table\""), std::string::npos);
  EXPECT_NE(bytes.find("\"mode\":\"contact_release\""), std::string::npos);
  EXPECT_EQ(bytes.find("scene/part/shape/0"), std::string::npos);
}

TEST(SceneRuntimeSourcePublication,
     WrapsTheSeparateInitializationAndPoseLifetimeOwner) {
  std::int64_t now = 10;
  SceneRuntimeStateOwner owner([&] { return now; }, [] { return "token-1"; });
  owner.EstablishOpenRuntime(
      SceneRuntimeIdentity{"bundle-1", "graph-1", 1U, 0U, 1U}, 1U, "bootstrap");
  const auto bytes = content_identity::canonical_json(
      BuildSceneRuntimeSourcePayload("cell-a", owner.Snapshot()));
  EXPECT_NE(bytes.find("\"api_version\":\"manyforge.scene_runtime_source.v1\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"cell_id\":\"cell-a\""), std::string::npos);
  EXPECT_NE(bytes.find("\"state\":\"open\""), std::string::npos);
  EXPECT_THROW(BuildSceneRuntimeSourcePayload("", owner.Snapshot()),
               std::invalid_argument);
}

} // namespace
} // namespace manyforge::headless
