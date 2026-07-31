#include <gtest/gtest.h>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "retained_bundle_internal.hpp"
#include "retention_store_fixtures.hpp"
#include "runtime_only_retention_fixture.hpp"

namespace manyforge::deployment::detail {

/// Uninstalled test-only access to the deliberately producer-less context.
///
/// These values prove projection/materialization mechanics only. They are not
/// a production node-environment or compiler-build qualification, and no
/// equivalent callable factory is linked into the library.
class RetentionMaterializerTestAccess final {
 public:
  [[nodiscard]] static BuildQualifiedRetentionContext make_context() {
    return BuildQualifiedRetentionContext(
        core::RetainedNodeEnvironmentDTO{
            "manyforge_i3_materializer_test",
            "1.0.0",
            core::ContentDigest(
                "sha256:2222222222222222222222222222222222222222222222222222222222222222"),
            core::ContentDigest(
                "sha256:3333333333333333333333333333333333333333333333333333333333333333"),
        },
        core::ContentDigest(
            "sha256:4444444444444444444444444444444444444444444444444444444444444444"),
        core::ContentDigest(
            "sha256:5555555555555555555555555555555555555555555555555555555555555555"));
  }
};

}  // namespace manyforge::deployment::detail

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

static_assert(
    !std::is_aggregate_v<detail::BuildQualifiedRetentionContext>);
static_assert(
    !std::is_default_constructible_v<detail::BuildQualifiedRetentionContext>);
static_assert(!std::is_constructible_v<
              detail::BuildQualifiedRetentionContext,
              core::RetainedNodeEnvironmentDTO, core::ContentDigest,
              core::ContentDigest>);

[[nodiscard]] const CanonicalValue& member(const CanonicalValue& value,
                                           std::string_view name) {
  const CanonicalValue* found = value.find_object_member(name);
  if (found == nullptr) {
    throw std::logic_error("test fixture is missing member " +
                           std::string(name));
  }
  return *found;
}

[[nodiscard]] const CanonicalValue::Array& array_member(
    const CanonicalValue& value, std::string_view name) {
  return std::get<CanonicalValue::Array>(member(value, name).storage());
}

[[nodiscard]] const std::string& string_member(
    const CanonicalValue& value, std::string_view name) {
  return std::get<std::string>(member(value, name).storage());
}

[[nodiscard]] std::string empty_graph() {
  return "{\"runtime_units\":[],\"model_instances\":[],"
         "\"controllable_components\":[],\"control_domains\":[],"
         "\"control_bindings\":[],\"component_attachments\":[]}";
}

[[nodiscard]] std::string deployment_document(std::string_view cell_id) {
  return "{\"api_version\":\"manyforge.deployment.v1\","
         "\"deployment_artifact_id\":\"i3_canonical_empty\","
         "\"deployment_revision\":\"" +
         std::string(kZeroDigest) +
         "\",\"cell_id\":\"" + std::string(cell_id) +
         "\",\"name\":\"I3 canonical empty\","
         "\"catalogs\":{\"records\":[]},"
         "\"graph\":" + empty_graph() +
         ",\"desired_control_configurations\":[],"
         "\"capability_provider_bindings\":[]}";
}

[[nodiscard]] RetentionQualifiedDeployment qualify_empty(
    std::string_view cell_id) {
  const auto deployment = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(deployment_document(cell_id)),
      std::span<const QualifiedRuntimeProfile>{});
  const auto composition = compose_cell_occurrences(
      deployment.graph(), std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const auto selected_overlay = select_site_overlay_for_deployment(
      deployment.applicability_inputs().value().site_overlay_ref,
      deployment.applicability_inputs().value().external_resources, nullptr);
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), {}, {}, {}, {}, {},
      {}, selected_overlay);
  return qualify_deployment_for_retention(
      deployment, compile_cell_ir(applicability));
}

[[nodiscard]] std::shared_ptr<const RetainedBundle> materialize_empty(
    std::string_view cell_id, IRawArtifactStore& store) {
  const auto qualified = qualify_empty(cell_id);
  return detail::RetainedBundleMaterializer::materialize(
      qualified, detail::RetentionMaterializerTestAccess::make_context(),
      store);
}

class CountingRawArtifactStore final : public IRawArtifactStore {
 public:
  [[nodiscard]] StoredRawArtifact put_if_absent(
      std::string_view bytes) override {
    ++put_calls_;
    return {
        core::ContentDigest(sha256_digest(bytes)),
        core::ContentAddressedStoreKey(
            "sha256/" + sha256_digest(bytes).substr(7U)),
        static_cast<std::uint64_t>(bytes.size()),
        true,
    };
  }

  [[nodiscard]] std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey&, const core::ContentDigest&,
      std::uint64_t) const override {
    ++get_calls_;
    return std::nullopt;
  }

  [[nodiscard]] std::size_t put_calls() const noexcept { return put_calls_; }
  [[nodiscard]] std::size_t get_calls() const noexcept { return get_calls_; }

 private:
  std::size_t put_calls_{0U};
  mutable std::size_t get_calls_{0U};
};

TEST(RetainedBundleMaterializer,
     CanonicalEmptyUsesOrdinaryCompilerUrdfSrdfAndClosedEnvelope) {
  const auto qualified = qualify_empty("i3_empty_cell");
  const auto& composition =
      qualified.compiler_ir().applicability().composition();
  const std::string expected_urdf =
      emit_canonical_urdf_document(composition.flattened_urdf());
  const std::string expected_srdf =
      emit_canonical_srdf_document(composition.flattened_srdf());
  ASSERT_EQ(composition.flattened_urdf().links.size(), 1U);
  EXPECT_EQ(composition.flattened_urdf().links.front().name,
            composition.cell_frame().compiled_frame_name);
  EXPECT_NE(expected_urdf.find("<link"), std::string::npos);
  EXPECT_EQ(expected_srdf.find("<group"), std::string::npos);

  test::InMemoryRawArtifactStoreFixture store;
  const auto bundle = detail::RetainedBundleMaterializer::materialize(
      qualified, detail::RetentionMaterializerTestAccess::make_context(),
      store);

  ASSERT_NE(bundle, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*bundle));
  EXPECT_EQ(bundle->canonical_bytes(), canonical_json(bundle->normalized_root()));
  EXPECT_EQ(store.size(), 2U);

  const CanonicalValue& root = bundle->normalized_root();
  EXPECT_EQ(root.as_object().size(), 10U);
  EXPECT_EQ(string_member(root, "api_version"),
            core::kRetainedBundleApiVersion);
  EXPECT_EQ(string_member(root, "deployment_artifact_id"),
            "i3_canonical_empty");
  EXPECT_EQ(string_member(root, "retained_bundle_id"),
            bundle->retained_bundle_id().value());
  EXPECT_EQ(bundle->value().api_version, core::kRetainedBundleApiVersion);
  EXPECT_EQ(bundle->value().retained_bundle_id,
            bundle->retained_bundle_id());
  EXPECT_EQ(bundle->value().cell_graph,
            qualified.compiler_ir().applicability().composition().graph());
  EXPECT_TRUE(bundle->value().catalogs.records.empty());
  EXPECT_TRUE(bundle->value().catalogs.schema_artifacts.empty());
  EXPECT_FALSE(bundle->value().compiled_products.viewer_model.has_value());
  EXPECT_EQ(bundle->value().provenance.output_records.size(), 1U);

  const CanonicalValue& catalogs = member(root, "catalogs");
  EXPECT_TRUE(array_member(catalogs, "records").empty());
  EXPECT_TRUE(array_member(catalogs, "schema_artifacts").empty());
  EXPECT_TRUE(
      array_member(root, "desired_control_configurations").empty());
  EXPECT_TRUE(array_member(root, "capability_provider_bindings").empty());

  const CanonicalValue& products = member(root, "compiled_products");
  const CanonicalValue& planning =
      member(products, "planning_collision_model");
  const auto& artifacts = array_member(planning, "artifact_refs");
  ASSERT_EQ(artifacts.size(), 2U);
  EXPECT_EQ(string_member(artifacts[0], "artifact_role_id"), "srdf");
  EXPECT_EQ(string_member(artifacts[1], "artifact_role_id"), "urdf");

  const std::string srdf_digest = sha256_digest(expected_srdf);
  const std::string urdf_digest = sha256_digest(expected_urdf);
  EXPECT_EQ(
      store.get_verified(
          core::ContentAddressedStoreKey("sha256/" + srdf_digest.substr(7U)),
          core::ContentDigest(srdf_digest), expected_srdf.size()),
      expected_srdf);
  EXPECT_EQ(
      store.get_verified(
          core::ContentAddressedStoreKey("sha256/" + urdf_digest.substr(7U)),
          core::ContentDigest(urdf_digest), expected_urdf.size()),
      expected_urdf);

  const CanonicalValue& provenance = member(root, "provenance");
  EXPECT_TRUE(array_member(provenance, "source_records").empty());
  EXPECT_TRUE(array_member(provenance, "generator_records").empty());
  EXPECT_EQ(array_member(provenance, "output_records").size(), 1U);
}

TEST(RetainedBundleMaterializer,
     ValidButUnsupportedNonEmptyDeploymentRejectsBeforeAnyStoreEffect) {
  const auto fixture = test::make_runtime_only_retention_fixture();
  ASSERT_EQ(fixture.qualified.deployment().graph().graph().runtime_units.size(),
            1U);
  ASSERT_EQ(fixture.qualified.compiler_ir().catalog_record_refs().size(), 1U);

  CountingRawArtifactStore store;
  EXPECT_THROW(
      static_cast<void>(detail::RetainedBundleMaterializer::materialize(
          fixture.qualified,
          detail::RetentionMaterializerTestAccess::make_context(), store)),
      RetainedBundleError);
  EXPECT_EQ(store.put_calls(), 0U);
  EXPECT_EQ(store.get_calls(), 0U);
}

TEST(RetainedBundleMaterializer,
     RegenerationIsByteExactAndRepositoryRoundTripReverifiesWholeEnvelope) {
  test::InMemoryRawArtifactStoreFixture first_store;
  test::InMemoryRawArtifactStoreFixture second_store;
  const auto first = materialize_empty("i3_empty_cell", first_store);
  const auto second = materialize_empty("i3_empty_cell", second_store);

  EXPECT_EQ(first->retained_bundle_id(), second->retained_bundle_id());
  EXPECT_EQ(first->normalized_root(), second->normalized_root());
  EXPECT_EQ(first->canonical_bytes(), second->canonical_bytes());

  test::InMemoryRetainedBundleRepositoryFixture repository;
  EXPECT_TRUE(repository.put_if_absent(first));
  EXPECT_FALSE(repository.put_if_absent(second));
  EXPECT_EQ(repository.size(), 1U);
  EXPECT_EQ(repository.retained_bytes(), first->canonical_bytes().size());

  const auto loaded =
      repository.get_verified_envelope(first->retained_bundle_id());
  ASSERT_NE(loaded, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*loaded));
  EXPECT_EQ(loaded->retained_bundle_id(), first->retained_bundle_id());
  EXPECT_EQ(loaded->value(), first->value());
  EXPECT_EQ(loaded->normalized_root(), first->normalized_root());
  EXPECT_EQ(loaded->canonical_bytes(), first->canonical_bytes());
}

TEST(RetainedBundleMaterializer,
     SealedRepositoryEnforcesActualBundleCountAndByteBounds) {
  test::InMemoryRawArtifactStoreFixture first_store;
  test::InMemoryRawArtifactStoreFixture second_store;
  const auto first = materialize_empty("i3_bound_cell_a", first_store);
  const auto second = materialize_empty("i3_bound_cell_b", second_store);

  ASSERT_GT(first->canonical_bytes().size(), 1U);
  test::InMemoryRetainedBundleRepositoryFixture byte_bounded(
      2U, first->canonical_bytes().size() - 1U);
  EXPECT_THROW(static_cast<void>(byte_bounded.put_if_absent(first)),
               RetainedBundleError);
  EXPECT_EQ(byte_bounded.size(), 0U);
  EXPECT_EQ(byte_bounded.retained_bytes(), 0U);

  const std::uint64_t combined_bytes =
      static_cast<std::uint64_t>(first->canonical_bytes().size()) +
      static_cast<std::uint64_t>(second->canonical_bytes().size());
  test::InMemoryRetainedBundleRepositoryFixture count_bounded(
      1U, combined_bytes);
  EXPECT_TRUE(count_bounded.put_if_absent(first));
  EXPECT_THROW(static_cast<void>(count_bounded.put_if_absent(second)),
               RetainedBundleError);
  EXPECT_EQ(count_bounded.size(), 1U);
  EXPECT_EQ(count_bounded.retained_bytes(), first->canonical_bytes().size());
}

TEST(RetainedBundleMaterializer,
     ValidCellIdentityChangesRemainOrdinaryCellSpecificProducts) {
  test::InMemoryRawArtifactStoreFixture first_store;
  test::InMemoryRawArtifactStoreFixture second_store;
  const auto first = materialize_empty("i3_empty_cell_a", first_store);
  const auto second = materialize_empty("i3_empty_cell_b", second_store);

  EXPECT_NE(first->retained_bundle_id(), second->retained_bundle_id());
  EXPECT_NE(first->canonical_bytes(), second->canonical_bytes());
  EXPECT_NE(
      string_member(member(first->normalized_root(), "cell_graph"), "cell_id"),
      string_member(member(second->normalized_root(), "cell_graph"),
                    "cell_id"));
  const auto& first_artifacts =
      first->value()
          .compiled_products.planning_collision_model.artifact_refs;
  const auto& second_artifacts =
      second->value()
          .compiled_products.planning_collision_model.artifact_refs;
  ASSERT_EQ(first_artifacts.size(), 2U);
  ASSERT_EQ(second_artifacts.size(), first_artifacts.size());
  for (std::size_t index = 0U; index < first_artifacts.size(); ++index) {
    EXPECT_EQ(first_artifacts[index].artifact_role_id,
              second_artifacts[index].artifact_role_id);
    EXPECT_NE(first_artifacts[index].content_digest,
              second_artifacts[index].content_digest);
  }
}

}  // namespace
}  // namespace manyforge::deployment
