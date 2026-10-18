#include "applied_program_owner_reader.hpp"
#include "selected_program_products.hpp"
#include "ros_host_composition.hpp"
#include "content_identity_internal.hpp"
#include "canonical_empty_admission_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"

#include "../../manyforge_deployment/test/io_retained_fixture.hpp"
#include <gtest/gtest.h>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_deployment/prepared_activation_repository.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <atomic>
#include <filesystem>
#include <map>
#include <unistd.h>

namespace manyforge::roboplan_backend::detail {
namespace {
namespace c = core;
namespace d = deployment;
namespace m = component_manager;
using V = d::CanonicalValue;
const c::ContentDigest kDigestA("sha256:" + std::string(64U, 'a'));
const c::ContentDigest kDigestB("sha256:" + std::string(64U, 'b'));

V replace(const V &root, std::string_view field, V value) {
  auto object = root.as_object();
  for (auto &[name, item] : object)
    if (name == field) {
      item = std::move(value);
      return V(std::move(object));
    }
  throw std::logic_error("fixture field is absent");
}

std::shared_ptr<const d::ProgramBindingContract>
ScalarProgram(const d::RetainedBundle &bundle) {
  auto root = d::parse_strict_json_model_yaml(R"({
    "api_version":"manyforge.program.v2","program_id":"applied_context_fixture",
    "program_revision":"sha256:0000000000000000000000000000000000000000000000000000000000000000",
    "program_profile":"composable-cell-v1","name":"Applied context fixture",
    "catalog":{"api_version":"manyforge.node_catalog.v2","content_version":"0.0.0"},
    "component_roles":{},"target_roles":{},
    "blackboard":{"namespaces":[],"keys":[
      {"key":"count","type":"int64","access":"read_write","persistence":"run"},
      {"key":"label","type":"string","access":"read_write","persistence":"run"},
      {"key":"ratio","type":"float64","access":"read_write","persistence":"run"},
      {"key":"ready","type":"bool","access":"read_write","persistence":"run"}]},
    "tree":{"node_id":"sequence","instance_id":"root","parameters":{},"children":[]}})")
                  .root();
  root = replace(
      root, "catalog",
      replace(*root.find_object_member("catalog"), "content_version",
              V(bundle.value().catalogs.node_environment.content_version)));
  root = replace(root, "program_revision",
                 V(d::detail::compute_content_identity(
                     root, {"program-revision", "manyforge.program.v2",
                            "program_revision"})));
  return d::verify_program_binding_contract(d::canonical_json(root), bundle);
}

class AppliedProgramBlackboardContextTest : public ::testing::Test {
protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("manyforge-applied-context-" + std::to_string(::getpid()) + "-" +
            std::to_string(next.fetch_add(1U)));
    package = root / "package";
    manager = root / "manager";
    ASSERT_TRUE(std::filesystem::create_directories(package));
    ASSERT_TRUE(std::filesystem::create_directory(manager));
    bundle = d::test::materialize_boolean_io_retained_fixture(package).bundle;
    program = ScalarProgram(*bundle);
    d::FilesystemVerifiedRetainedModelMapSource maps(package);
    const auto map =
        maps.get_verified({bundle->retained_bundle_id(),
                           bundle->value()
                               .compiled_products.symbol_role_maps.front()
                               .product_id});
    ASSERT_TRUE(map);
    d::FilesystemCatalogArtifactStore catalog(package);
    d::FilesystemRawArtifactStore raw(package);
    const auto roles = d::parse_strict_json_model_yaml(
                           R"({"component_roles":[],"target_roles":[]})")
                           .root();
    auto prepared = d::assemble_prepared_activation(
        {d::PreparedScenarioOrigin{"context.scenario", kDigestA}, kDigestB,
         "optional"},
        *bundle, *map, *program, roles, catalog, raw);
    d::FilesystemPreparedActivationRepository repository(package);
    static_cast<void>(repository.put_if_absent(prepared));
    selected = repository.get_verified(prepared.prepared_activation_id());
    ASSERT_NE(selected, nullptr);
    binding = selected->closure().program_binding;
    ASSERT_NE(binding, nullptr);
    ASSERT_EQ(selected->closure().program->value(), program->value());
  }
  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  c::ActiveBundlePublicationReceiptDTO WriteNamedSelection(bool complete) {
    const auto cell = bundle->value().cell_graph.cell_id;
    const c::ProjectionRevisionDTO projection{
        bundle->retained_bundle_id(),
        bundle->value().cell_graph.graph_revision,
        {c::SceneId(kDigestA.value()), 1U, kDigestB}};
    m::SqliteActiveBundlePublicationStore publications(
        manager / std::string(m::kActiveBundlePublicationStoreFilename), cell);
    const auto predecessor = publications.PublishActiveBundle(
        {c::BootstrapPublicationTransactionDTO{
             c::BootstrapId("context.bootstrap"), c::BootstrapGeneration(1U)},
         projection, projection.retained_bundle_id, projection.graph_revision,
         std::nullopt, kDigestA});
    // As in the public preparer, the real stored prepared value is named by
    // its exact compact receipt; the activation payload is not copied twice.
    V::Object receipt;
    for (const auto &[name, value] : selected->normalized_root().as_object())
      if (name != "activation_payload")
        receipt.emplace_back(name, value);
    const m::ManagerApplyIntentDTO intent{
        {c::RequestId("context.apply.request"),
         c::RequestOwnerEpoch("context.manager.owner"), "writer-reader-test",
         std::nullopt},
        c::ApplyId("context.apply"),
        c::ApplyGeneration(1U),
        cell,
        projection,
        projection.retained_bundle_id,
        projection.graph_revision,
        {selected->captured_input_digest(), selected->deployment_artifact_id(),
         selected->deployment_revision(), selected->program_binding_id(),
         selected->prepared_activation_id(),
         d::canonical_json(V(std::move(receipt)))},
        c::AdmissionGeneration(1U),
        std::nullopt};
    m::SqliteManagerApplyJournal journal(
        manager / std::string(m::kManagerApplyJournalFilename), cell);
    const auto accepted = journal.EnsureIntent(intent);
    // These typed observations are journal fixture inputs. They are not a
    // claim that a Manager process or Kernel performed operational Apply.
    // The candidate graph is unchanged; this selection has no runtime delta.
    const c::ApplyConfigurationFenceIntentDTO fence{
        intent.request_context,
        {intent.apply_id, intent.apply_generation},
        {},
        c::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
        projection.retained_bundle_id,
        projection.graph_revision,
        0U,
        c::ConfigurationFenceGeneration(0U),
        {},
        "Program selection fixture"};
    const auto affected = journal.EnsureAffectedScope(
        accepted.record.intent_digest, {intent.apply_id,
                                        intent.apply_generation,
                                        fence.scope,
                                        fence.busy_disposition,
                                        {}});
    const auto attempt = journal.EnsureFenceAttempt(
        accepted.record.intent_digest, affected.record.affected_scope_digest,
        fence);
    const auto held = journal.EnsureFenceAcceptance(
        intent.apply_id, intent.apply_generation,
        attempt.record.fence_attempt_digest,
        {c::ConfigurationFenceId("context.fence"),
         c::ConfigurationFenceGeneration(1U), fence.scope, 1LL,
         c::RequestReplayStatusDTO::kAcceptedNew});
    const auto convergence = journal.EnsureRuntimeConvergenceIntent(
        accepted.record.intent_digest,
        {intent.apply_id,
         intent.apply_generation,
         projection.retained_bundle_id,
         projection.graph_revision,
         held.record.acceptance_digest,
         predecessor.publication_generation,
         predecessor.durable_public_pointer_content_digest,
         {}});
    static_cast<void>(journal.EnsureRuntimeStagedReady(
        {intent.apply_id,
         intent.apply_generation,
         1U,
         convergence.record.runtime_convergence_intent_digest,
         predecessor.publication_generation,
         predecessor.durable_public_pointer_content_digest,
         2U,
         {}}));
    const auto materialization = journal.EnsurePrivateMaterialization(
        {projection.scene_revision.content_hash, intent.apply_id,
         intent.apply_generation, projection,
         c::ContentDigest(d::sha256_digest(bundle->canonical_bytes())),
         static_cast<std::uint64_t>(bundle->canonical_bytes().size())});
    const auto publication = publications.PublishActiveBundle(
        {c::NamedApplyPublicationTransactionDTO{intent.apply_id,
                                                intent.apply_generation},
         projection, projection.retained_bundle_id, projection.graph_revision,
         predecessor.publication_generation,
         materialization.record.private_materialization_content_digest});
    const auto published = journal.EnsurePublicationReceipt(
        {materialization.record.private_materialization_content_digest,
         publication});
    if (!complete)
      return publication;
    const c::ConfigurationFenceReconciliationEvidenceDTO evidence{
        {c::RuntimeUnitId("context.kernel"), c::RuntimeEpoch("context.runtime"),
         c::RequestOwnerEpoch("context.kernel.owner"), kDigestA,
         publication.publication_generation},
        {intent.apply_id, intent.apply_generation},
        held.record.accepted_fence.configuration_fence_id,
        held.record.accepted_fence.configuration_fence_generation,
        fence.scope,
        0U,
        projection.graph_revision,
        projection,
        publication.publication_generation,
        publication.durable_public_pointer_content_digest,
        {},
        0U,
        0U,
        kDigestB};
    const c::ConfigurationFenceReleaseDTO release{
        evidence, c::RuntimeEpoch("context.manager")};
    const auto releasing = journal.EnsureFenceReleaseIntent(
        {published.record.publication_receipt_digest,
         held.record.acceptance_digest, release});
    const auto released = journal.EnsureFenceReleased(
        {releasing.record.fence_release_intent_digest,
         {release, evidence.execution_host_epoch, 3LL,
          c::RequestReplayStatusDTO::kAcceptedNew, kDigestA}});
    static_cast<void>(journal.EnsureTerminalOutcome(
        {intent.apply_id, intent.apply_generation, projection,
         published.record.publication_receipt_digest,
         published.record.publication_receipt_digest,
         released.record.fence_released_digest,
         m::ManagerApplyTerminalOutcomeState::kSucceeded, std::nullopt}));
    static_cast<void>(journal.VerifyIntegrity());
    static_cast<void>(publications.VerifyIntegrity());
    return publication;
  }

  static inline std::atomic<std::uint64_t> next{};
  std::filesystem::path root, package, manager;
  std::shared_ptr<const d::RetainedBundle> bundle;
  std::shared_ptr<const d::ProgramBindingContract> program;
  std::shared_ptr<const d::ProgramBinding> binding;
  std::shared_ptr<const d::PreparedActivationInput> selected;
};

TEST_F(
    AppliedProgramBlackboardContextTest,
    RealJournalWriterAndPublicPreparedReceiptProduceExactEmptyReferenceContext) {
  const auto publication = WriteNamedSelection(true);
  const auto observed = ReadAppliedProgramOwnerReadOnly(
      manager, bundle->value().cell_graph.cell_id,
      publication.publication_generation);
  ASSERT_TRUE(observed.ok()) << observed.error().message;
  const auto applied = ReadAppliedOwnerReadOnly(manager,
      bundle->value().cell_graph.cell_id, publication.publication_generation);
  ASSERT_TRUE(applied.ok()) << applied.error().message;
  EXPECT_EQ(applied.value().publication, observed.value().publication);
  EXPECT_EQ(applied.value().apply_intent_digest, observed.value().apply_intent_digest);
  EXPECT_EQ(applied.value().terminal_outcome_digest, observed.value().terminal_outcome_digest);
  EXPECT_EQ(observed.value().publication, publication);
  EXPECT_EQ(observed.value().program_binding_id, binding->program_binding_id());
  EXPECT_EQ(observed.value().program_id, program->value().program_id);
  EXPECT_EQ(observed.value().program_revision,
            program->value().program_revision);
  const c::RequestOwnerEpoch owner("context.kernel.owner");
  const auto context = CompileAppliedProgramBlackboardContext(
      observed.value(), owner, program, binding, *bundle);
  ASSERT_TRUE(context.ok()) << context.error().message;
  EXPECT_EQ(context.value().request_owner_epoch, owner);
  EXPECT_EQ(context.value().active_publication_generation,
            publication.publication_generation);
  EXPECT_EQ(context.value().program_binding.get(), &binding->value());
  EXPECT_EQ(context.value().blackboard_key_declarations.get(),
            &program->blackboard_key_declarations());
  std::map<std::string, std::string> declarations;
  for (const auto &key : *context.value().blackboard_key_declarations)
    declarations.emplace(key.name, key.declared_type);
  EXPECT_EQ(declarations.at("count"), "int64");
  EXPECT_EQ(declarations.at("label"), "string");
  EXPECT_EQ(declarations.at("ratio"), "float64");
  EXPECT_EQ(declarations.at("ready"), "bool");
  EXPECT_TRUE(context.value().required_keys.empty());
  ASSERT_NE(context.value().io_conditions, nullptr);
  EXPECT_TRUE(context.value().io_conditions->program_node_conditions.empty());
  EXPECT_EQ(context.value().maximum_value_bytes,
            c::kMaximumCellAdmissionJsonRequestBytes);
  auto mismatched = observed.value();
  mismatched.program_binding_id = c::ProgramBindingId(kDigestA.value());
  EXPECT_FALSE(CompileAppliedProgramBlackboardContext(mismatched, owner,
                                                      program, binding, *bundle)
                   .ok());
  mismatched = observed.value();
  mismatched.program_revision = c::ProgramRevision(kDigestA.value());
  EXPECT_FALSE(CompileAppliedProgramBlackboardContext(mismatched, owner,
                                                      program, binding, *bundle)
                   .ok());
  EXPECT_FALSE(ReadAppliedProgramOwnerReadOnly(
                   manager, bundle->value().cell_graph.cell_id,
                   c::ActiveBundlePublicationGeneration(
                       publication.publication_generation.value() + 1U))
                   .ok());
}

TEST_F(
    AppliedProgramBlackboardContextTest,
    ProspectiveNamedSelectionLoadsRealProgramProductsWithoutClaimingSuccess) {
  d::FilesystemVerifiedRetainedModelMapSource maps(package);
  const auto map = maps.get_verified(
      {bundle->retained_bundle_id(),
       bundle->value().compiled_products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(map);
  d::FilesystemProgramRepository programs(package);
  d::FilesystemProgramBindingRepository bindings(package);
  static_cast<void>(programs.put_if_absent(program, *bundle));
  static_cast<void>(bindings.put_if_absent(binding, *bundle, *map));
  const auto publication = WriteNamedSelection(false);
  const auto &origin = std::get<c::NamedApplyPublicationTransactionDTO>(
      publication.request.transaction_ref);
  const auto loaded =
      ReadSelectedProgramProducts(manager, bundle->value().cell_graph.cell_id,
                                  origin, *bundle, *map, package, {});
  ASSERT_TRUE(loaded.ok()) << loaded.error().message;
  ASSERT_NE(loaded.value().program, nullptr);
  ASSERT_NE(loaded.value().binding, nullptr);
  EXPECT_EQ(loaded.value().outcome.program_binding_id, binding->program_binding_id());
  EXPECT_FALSE(loaded.value().outcome.non_executable);
  const auto no_map = ReadSelectedProgramProducts(manager,
      bundle->value().cell_graph.cell_id, origin, *bundle,
      static_cast<const d::VerifiedRetainedModelMap *>(nullptr), package, {});
  ASSERT_FALSE(no_map.ok());
  EXPECT_EQ(no_map.error().details.at("reason"), "program_selected_products_unavailable");
  EXPECT_EQ(loaded.value().program->canonical_program_bytes(),
            program->canonical_program_bytes());
  EXPECT_EQ(loaded.value().binding->canonical_bytes(),
            binding->canonical_bytes());
  EXPECT_FALSE(ReadAppliedOwnerReadOnly(manager,
                                        bundle->value().cell_graph.cell_id,
                                        publication.publication_generation)
                   .ok());
  const auto foreign_cell = ReadSelectedProgramProducts(manager, c::CellId("foreign.cell"),
      origin, *bundle, *map, package, {});
  ASSERT_FALSE(foreign_cell.ok());
  EXPECT_EQ(foreign_cell.error().details.at("reason"), "program_selected_product_mismatch");
  const auto primary = root / "empty-program-root";
  ASSERT_TRUE(std::filesystem::create_directory(primary));
  const auto artifact =
      ReadSelectedProgramProducts(manager, bundle->value().cell_graph.cell_id,
                                  origin, *bundle, *map, primary, package);
  ASSERT_TRUE(artifact.ok()) << artifact.error().message;
  EXPECT_EQ(artifact.value().binding->canonical_bytes(),
            binding->canonical_bytes());
  EXPECT_FALSE(ReadSelectedProgramProducts(manager,
                                           bundle->value().cell_graph.cell_id,
                                           origin, *bundle, *map, primary, {})
                   .ok());
  EXPECT_FALSE(ReadSelectedProgramProducts(
                   manager, bundle->value().cell_graph.cell_id,
                   {origin.apply_id,
                    c::ApplyGeneration(origin.apply_generation.value() + 1U)},
                   *bundle, *map, package, {})
                   .ok());
}

TEST_F(
    AppliedProgramBlackboardContextTest,
    PublishedNamedSelectionCannotAttestSucceededOwnerWithoutReleaseAndTerminal) {
  const auto publication = WriteNamedSelection(false);
  const auto observed = ReadAppliedProgramOwnerReadOnly(
      manager, bundle->value().cell_graph.cell_id,
      publication.publication_generation);
  ASSERT_FALSE(observed.ok());
  const auto applied = ReadAppliedOwnerReadOnly(manager,
      bundle->value().cell_graph.cell_id, publication.publication_generation);
  ASSERT_FALSE(applied.ok());
  EXPECT_EQ(applied.error(), observed.error());
  EXPECT_EQ(observed.error().details.at("reason"),
            "program_bb_applied_record_unavailable");
}
TEST_F(AppliedProgramBlackboardContextTest,
       GenuineCanonicalEmptySucceededApplyNeedsNoExecutableProgram) {
  // Reuse the actual public canonical-empty admission/preparation path, then
  // the same complete SQLite writer chain. No fake prepared receipt/owner.
  const auto physical_bundle = bundle;
  const auto empty_package = root / "empty-package";
  ASSERT_TRUE(std::filesystem::create_directory(empty_package));
  const auto admission = d::detail::generate_ros_empty_package(
      empty_package, c::CellId("empty.applied.context"),
      d::detail::ros2_control_execution_runtime_profile_document());
  const d::FilesystemVerifiedCatalogArtifactSource catalog(empty_package);
  const d::FilesystemVerifiedRawArtifactSource raw(empty_package);
  const auto prepared = d::assemble_sealed_canonical_empty_activation(admission, catalog, raw);
  d::FilesystemPreparedActivationRepository repository(empty_package);
  static_cast<void>(repository.put_if_absent(prepared));
  selected = repository.get_verified(prepared.prepared_activation_id(), &admission);
  ASSERT_NE(selected, nullptr);
  ASSERT_FALSE(selected->program_id());
  ASSERT_FALSE(selected->program_binding_id());
  bundle = selected->closure().retained_bundle;
  program.reset();
  binding.reset();
  const auto publication = WriteNamedSelection(true);
  const auto actual = ReadAppliedOwnerReadOnly(manager,
      bundle->value().cell_graph.cell_id, publication.publication_generation);
  ASSERT_TRUE(actual.ok()) << actual.error().message;
  EXPECT_EQ(actual.value().publication, publication);
  const auto selected = ReadSelectedProgramProducts(manager,
      bundle->value().cell_graph.cell_id,
      std::get<c::NamedApplyPublicationTransactionDTO>(publication.request.transaction_ref),
      *bundle, static_cast<const d::VerifiedRetainedModelMap *>(nullptr), package, {});
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  EXPECT_FALSE(selected.value().program);
  EXPECT_FALSE(selected.value().binding);
  EXPECT_FALSE(selected.value().outcome.program_binding_id);
  EXPECT_FALSE(selected.value().outcome.non_executable);
  // The real zero-component selected product needs no invented Composition.
  host_detail::ExecutionCompositionAccess access;
  auto logical = std::make_shared<host_detail::ExecutionProduct>(bundle,
      selected.value().program, selected.value().binding,
      publication.request.candidate_projection_revision, publication.publication_generation);
  logical->selected_program_outcome = selected.value().outcome;
  logical->origin_named_apply = std::get<c::NamedApplyPublicationTransactionDTO>(
      publication.request.transaction_ref);
  {
    std::unique_lock lock(access.mutex);
    access.replace_logical_product(logical);
    EXPECT_EQ(access.current, nullptr);
    EXPECT_EQ(access.product(), logical.get());
    EXPECT_EQ(access.product()->bundle, bundle);
    EXPECT_EQ(access.product()->origin_named_apply, logical->origin_named_apply);
    auto invalid = std::make_shared<host_detail::ExecutionProduct>(physical_bundle,
        nullptr, nullptr, publication.request.candidate_projection_revision,
        publication.publication_generation);
    EXPECT_THROW(access.replace_logical_product(invalid), std::exception);
    EXPECT_EQ(access.product(), logical.get());
    access.replace_logical_product(nullptr);
    EXPECT_EQ(access.product(), nullptr);
  }
  const auto program_only = ReadAppliedProgramOwnerReadOnly(manager,
      bundle->value().cell_graph.cell_id, publication.publication_generation);
  ASSERT_FALSE(program_only.ok());
  EXPECT_EQ(program_only.error().details.at("reason"), "program_bb_applied_program_unbound");
}

} // namespace
} // namespace manyforge::roboplan_backend::detail
