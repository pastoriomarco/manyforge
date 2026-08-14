#pragma once

#include <filesystem>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "build_manifest_internal.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "retained_bundle_internal.hpp"

namespace manyforge::deployment::test {

/// Exact compiler/materializer product shared by the I3 consumer
/// qualifications.
///
/// This helper is test-only and deployment-owned. The generated consumer
/// handoff obtains the planning model and sealed typed model map exclusively
/// through
/// durable exact point-read capabilities, binds both to one RetainedBundleId,
/// and selects control coordinates by logical component/command target. No
/// compiler-memory map or raw CanonicalValue enters the consumer boundary.
struct DurableLite6RetainedConsumerFixture final {
  Lite6StructuralRetentionFixture input;
  std::shared_ptr<const RetainedBundle> bundle;
};

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_retained_consumer_fixture_from(const std::filesystem::path& package_root,
                                                 Lite6ControllableRetentionFixture controllable,
                                                 std::span<const detail::StopQualificationEvidenceCandidate>
                                                     stop_evidence_candidates = {},
                                                 std::optional<std::string_view>
                                                     test_stop_build_identity = std::nullopt) {
  auto qualified = controllable.qualified;
  const Lite6ControllableCompilerInputArtifactSource input_source(controllable);
  FilesystemBuildManifestRepository manifest_repository(package_root);
  FilesystemCatalogArtifactStore catalog_store(package_root);
  FilesystemRawArtifactStore raw_store(package_root);
  FilesystemRetainedBundleRepository bundle_repository(package_root);
  std::shared_ptr<const RetainedBundle> bundle;
  if (test_stop_build_identity.has_value()) {
    if (!stop_evidence_candidates.empty()) {
      throw std::logic_error(
          "test build-identity fixture cannot also claim Stop qualification");
    }
    const auto context = detail::BuildManifestTestAccess::
        qualify_persisted_with_test_stop_build_identity(
            manifest_repository, *test_stop_build_identity);
    bundle = detail::RetainedBundleMaterializer::materialize(
        qualified, context, input_source,
        controllable.structural_preparations(), catalog_store, raw_store);
    (void)bundle_repository.put_if_absent(bundle);
    bundle = bundle_repository.get_verified_envelope(
        bundle->retained_bundle_id());
  } else {
    bundle = detail::RetainedBundleMaterializer::
        materialize_store_and_reload_production(
            qualified, manifest_repository, input_source,
            controllable.structural_preparations(), catalog_store, raw_store,
            bundle_repository, stop_evidence_candidates);
  }
  if (bundle == nullptr) {
    throw std::logic_error("non-empty Lite6 materializer returned no durable bundle");
  }
  return {
      std::move(controllable.structural),
      std::move(bundle),
  };
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_retained_consumer_fixture(const std::filesystem::path& package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_controllable_retention_fixture());
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_composer_assistant_retained_consumer_fixture(
    const std::filesystem::path& package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_composer_assistant_retention_fixture());
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_arm_gripper_aggregate_retained_consumer_fixture(
    const std::filesystem::path& package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_arm_gripper_aggregate_retention_fixture());
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_native_motion_retained_consumer_fixture(
    const std::filesystem::path& package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_native_motion_retention_fixture());
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
    const std::filesystem::path& package_root, std::string_view cell_id) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root,
      make_lite6_native_motion_retention_fixture_for_cell(cell_id));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_two_lite6_same_type_native_motion_retained_consumer_fixture_for_cell(
    const std::filesystem::path& package_root, std::string_view cell_id) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root,
      make_two_lite6_same_type_native_motion_retention_fixture_for_cell(
          cell_id));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_native_motion_arm_gripper_retained_consumer_fixture(
    const std::filesystem::path& package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_native_motion_arm_gripper_aggregate_retention_fixture());
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_test_build_identified_native_motion_arm_gripper_retained_consumer_fixture(
    const std::filesystem::path& package_root,
    std::string_view test_stop_build_identity) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root,
      make_lite6_native_motion_arm_gripper_aggregate_retention_fixture(), {},
      test_stop_build_identity);
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_stop_qualified_native_motion_retained_consumer_fixture(
    const std::filesystem::path& package_root,
    std::span<const detail::StopQualificationEvidenceCandidate>
        stop_evidence_candidates) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root,
      make_lite6_stop_qualified_native_motion_retention_fixture(),
      stop_evidence_candidates);
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_native_cclc_product_retained_consumer_fixture(
    const std::filesystem::path &package_root,
    std::span<const detail::StopQualificationEvidenceCandidate>
        stop_evidence_candidates,
    std::string_view arm_lifecycle_semantic_digest,
    std::string_view gripper_lifecycle_semantic_digest) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root,
      make_lite6_native_cclc_product_retention_fixture(
          arm_lifecycle_semantic_digest,
          gripper_lifecycle_semantic_digest),
      stop_evidence_candidates);
}

}  // namespace manyforge::deployment::test
