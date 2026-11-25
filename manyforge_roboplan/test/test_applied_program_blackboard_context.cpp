#include "applied_program_owner_reader.hpp"
#include "selected_program_products.hpp"
#include "ros_host_composition.hpp"
#include "content_identity_internal.hpp"
#include "canonical_empty_admission_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"

#include "../../manyforge_deployment/test/io_retained_fixture.hpp"
#include <gtest/gtest.h>
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
ScalarProgram(const d::RetainedBundle &bundle,
              const std::optional<d::VerifiedRetainedModelMap> &map,
              const d::ICatalogArtifactSource &catalog) {
  auto root = d::parse_strict_json_model_yaml(R"({
    "api_version":"manyforge.program.v4",
    "program_revision":"sha256:0000000000000000000000000000000000000000000000000000000000000000",
    "program_profile":"manyforge-cell-v1",
    "catalog":{"api_version":"manyforge.node_catalog.v3","content_version":"0.0.0"},
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
                     root, {"program-revision", "manyforge.program.v4",
                            "program_revision"})));
  return d::verify_program_binding_contract(d::canonical_json(root), bundle, map, catalog);
}

class AppliedProgramBlackboardContextTest : public ::testing::Test {
protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("manyforge-applied-context-" + std::to_string(::getpid()) + "-" +
            std::to_string(next.fetch_add(1U)));
    package = root / "package";
    ASSERT_TRUE(std::filesystem::create_directories(package));
    bundle = d::test::materialize_boolean_io_retained_fixture(package).bundle;
    d::FilesystemVerifiedRetainedModelMapSource maps(package);
    const auto map =
        maps.get_verified({bundle->retained_bundle_id(),
                           bundle->value()
                               .compiled_products.symbol_role_maps.front()
                               .product_id});
    ASSERT_TRUE(map);
    d::FilesystemCatalogArtifactStore catalog(package);
    program = ScalarProgram(*bundle, map, catalog);
    d::FilesystemRawArtifactStore raw(package);
    auto prepared = d::assemble_prepared_activation(
        {d::PreparedScenarioOrigin{"context.scenario", kDigestA,
                                   "/fixtures/context.scenario"},
         kDigestB, "optional"},
        *bundle, *map, *program, catalog, raw);
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

  // The installed product's Applied Program as the host holds it in memory.
  AppliedProgramOwnerSnapshot Applied() const {
    return {c::ActiveBundlePublicationGeneration(2U),
            program->value().program_revision,
            binding->program_binding_id()};
  }

  static inline std::atomic<std::uint64_t> next{};
  std::filesystem::path root, package;
  std::shared_ptr<const d::RetainedBundle> bundle;
  std::shared_ptr<const d::ProgramBindingContract> program;
  std::shared_ptr<const d::ProgramBinding> binding;
  std::shared_ptr<const d::PreparedActivationInput> selected;
};

TEST_F(
    AppliedProgramBlackboardContextTest,
    AppliedProgramSnapshotProducesExactEmptyReferenceContext) {
  const auto applied = Applied();
  const auto context = CompileAppliedProgramBlackboardContext(
      applied, program, binding, *bundle);
  ASSERT_TRUE(context.ok()) << context.error().message;
  EXPECT_EQ(context.value().active_publication_generation,
            applied.publication_generation);
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
  auto mismatched = applied;
  mismatched.program_binding_id = c::ProgramBindingId(kDigestA.value());
  EXPECT_FALSE(CompileAppliedProgramBlackboardContext(mismatched, program,
                                                      binding, *bundle)
                   .ok());
  mismatched = applied;
  mismatched.program_revision = c::ProgramRevision(kDigestA.value());
  EXPECT_FALSE(CompileAppliedProgramBlackboardContext(mismatched, program,
                                                      binding, *bundle)
                   .ok());
}

TEST_F(
    AppliedProgramBlackboardContextTest,
    RequestProgramLoadsRealProgramProductsWithoutManagerStores) {
  d::FilesystemVerifiedRetainedModelMapSource maps(package);
  const auto map = maps.get_verified(
      {bundle->retained_bundle_id(),
       bundle->value().compiled_products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(map);
  d::FilesystemProgramRepository programs(package);
  d::FilesystemProgramBindingRepository bindings(package);
  static_cast<void>(programs.put_if_absent(program, *bundle));
  d::FilesystemVerifiedCatalogArtifactSource catalog(package);
  static_cast<void>(bindings.put_if_absent(binding, *bundle, *map, catalog));
  // No Manager store exists: the Apply request's facts alone select the products.
  const std::optional<m::HostApplyProgramSelection> selection{
      m::HostApplyProgramSelection{program->value().program_revision,
                                   binding->program_binding_id()}};
  const auto loaded =
      LoadSelectedProgramProducts(selection, *bundle, *map, package, {});
  ASSERT_TRUE(loaded.ok()) << loaded.error().message;
  ASSERT_NE(loaded.value().program, nullptr);
  ASSERT_NE(loaded.value().binding, nullptr);
  EXPECT_EQ(loaded.value().binding->program_binding_id(), binding->program_binding_id());
  const auto no_map = LoadSelectedProgramProducts(
      selection, *bundle, static_cast<const d::VerifiedRetainedModelMap *>(nullptr),
      package, {});
  ASSERT_FALSE(no_map.ok());
  EXPECT_EQ(no_map.error().details.at("reason"), "program_selected_products_unavailable");
  EXPECT_EQ(loaded.value().program->canonical_program_bytes(),
            program->canonical_program_bytes());
  EXPECT_EQ(loaded.value().binding->canonical_bytes(),
            binding->canonical_bytes());
  const auto unbound = LoadSelectedProgramProducts(std::nullopt, *bundle,
      static_cast<const d::VerifiedRetainedModelMap *>(nullptr), package, {});
  ASSERT_TRUE(unbound.ok()) << unbound.error().message;
  EXPECT_EQ(unbound.value().program, nullptr);
  EXPECT_EQ(unbound.value().binding, nullptr);
  const auto primary = root / "empty-program-root";
  ASSERT_TRUE(std::filesystem::create_directory(primary));
  const auto artifact =
      LoadSelectedProgramProducts(selection, *bundle, *map, primary, package);
  ASSERT_TRUE(artifact.ok()) << artifact.error().message;
  EXPECT_EQ(artifact.value().binding->canonical_bytes(),
            binding->canonical_bytes());
  EXPECT_FALSE(
      LoadSelectedProgramProducts(selection, *bundle, *map, primary, {}).ok());
  auto foreign_revision = selection;
  foreign_revision->program_revision = c::ProgramRevision(kDigestA.value());
  const auto foreign = LoadSelectedProgramProducts(foreign_revision, *bundle, *map, package, {});
  ASSERT_FALSE(foreign.ok());
  EXPECT_EQ(foreign.error().details.at("reason"), "program_selected_products_unavailable");
}

} // namespace
} // namespace manyforge::roboplan_backend::detail
