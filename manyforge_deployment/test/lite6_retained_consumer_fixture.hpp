#pragma once

#include <filesystem>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <memory>
#include <optional>
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
                                                 Lite6ControllableRetentionFixture controllable) {
  auto qualified = controllable.qualified;
  const Lite6ControllableCompilerInputArtifactSource input_source(controllable);
  FilesystemBuildManifestRepository manifest_repository(package_root);
  FilesystemCatalogArtifactStore catalog_store(package_root);
  FilesystemRawArtifactStore raw_store(package_root);
  FilesystemRetainedBundleRepository bundle_repository(package_root);
  auto bundle = detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(
          qualified, manifest_repository, input_source,
          controllable.structural_preparations(), catalog_store, raw_store,
          bundle_repository);
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
      package_root, make_lite6_ros2_control_shared_consumer_retention_fixture());
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
materialize_lite6_stop_capable_native_motion_retained_consumer_fixture(
    const std::filesystem::path& package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root,
      make_lite6_stop_qualified_native_motion_retention_fixture());
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_native_arm_only_cclc_product_retained_consumer_fixture(
    const std::filesystem::path& package_root,
    std::string_view arm_lifecycle_semantic_digest) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_native_arm_only_cclc_product_retention_fixture(
                        arm_lifecycle_semantic_digest));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_ros2_control_arm_only_cclc_product_retained_consumer_fixture(
    const std::filesystem::path& package_root,
    std::string_view lifecycle_semantic_digest) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_ros2_control_arm_only_cclc_product_retention_fixture(
                        lifecycle_semantic_digest));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_ros2_control_cclc_product_retained_consumer_fixture(
    const std::filesystem::path& package_root,
    std::string_view lifecycle_semantic_digest,
    bool gripper_feedback = false,
    std::optional<double> initial_joint1_position_rad = std::nullopt) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_ros2_control_cclc_product_retention_fixture(
                        lifecycle_semantic_digest, gripper_feedback,
                        initial_joint1_position_rad));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_native_cclc_product_retained_consumer_fixture(
    const std::filesystem::path& package_root, std::string_view arm_lifecycle_semantic_digest,
    std::string_view gripper_lifecycle_semantic_digest, bool gripper_feedback = false,
    std::optional<double> initial_joint1_position_rad = std::nullopt) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root,
      make_lite6_native_cclc_product_retention_fixture(
          arm_lifecycle_semantic_digest, gripper_lifecycle_semantic_digest,
          gripper_feedback, initial_joint1_position_rad));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_native_gripper_only_cclc_product_retained_consumer_fixture(
    const std::filesystem::path& package_root, std::string_view arm_lifecycle_semantic_digest,
    std::string_view gripper_lifecycle_semantic_digest) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_native_gripper_only_cclc_product_retention_fixture(
                        arm_lifecycle_semantic_digest, gripper_lifecycle_semantic_digest));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_ros2_control_retained_consumer_fixture(
    const std::filesystem::path& package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_ros2_control_shared_consumer_retention_fixture());
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_ros2_control_arm_gripper_aggregate_retained_consumer_fixture(
    const std::filesystem::path& package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_ros2_control_shared_consumer_retention_fixture({}, 1U, true));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_ros2_control_motion_arm_gripper_retained_consumer_fixture(
    const std::filesystem::path& package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_ros2_control_shared_consumer_retention_fixture({}, 1U, true, true));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
    const std::filesystem::path& package_root, std::string_view cell_id) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_ros2_control_shared_consumer_retention_fixture(cell_id, 1U, false, false));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_two_lite6_same_type_ros2_control_motion_retained_consumer_fixture_for_cell(
    const std::filesystem::path& package_root, std::string_view cell_id) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_ros2_control_shared_consumer_retention_fixture(cell_id, 2U, false, false));
}

}  // namespace manyforge::deployment::test
