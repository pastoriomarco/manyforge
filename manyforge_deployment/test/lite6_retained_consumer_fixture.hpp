#pragma once

#include <filesystem>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <memory>
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
                                                 bool retain_empty_model_map = false) {
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
          bundle_repository, retain_empty_model_map);
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
materialize_lite6_stop_capable_ros_motion_retained_consumer_fixture(
    const std::filesystem::path &package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_stop_qualified_ros_motion_retention_fixture());
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_ros2_control_retained_consumer_fixture(
    const std::filesystem::path& package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_ros2_control_shared_consumer_retention_fixture());
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
    const std::filesystem::path& package_root, std::string_view cell_id) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_ros2_control_shared_consumer_retention_fixture(cell_id, 1U, false));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_two_lite6_same_type_ros2_control_motion_retained_consumer_fixture_for_cell(
    const std::filesystem::path& package_root, std::string_view cell_id) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_ros2_control_shared_consumer_retention_fixture(cell_id, 2U, false));
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_ros2_control_observed_retained_consumer_fixture(
    const std::filesystem::path &package_root) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, make_lite6_ros2_control_observed_consumer_retention_fixture());
}

[[nodiscard]] inline DurableLite6RetainedConsumerFixture
materialize_lite6_ros2_control_observed_endpoint_retained_consumer_fixture(
    const std::filesystem::path &package_root,
    bool shared_arm_endpoint_bodies = false) {
  return materialize_lite6_retained_consumer_fixture_from(
      package_root,
      make_lite6_ros2_control_observed_endpoint_consumer_retention_fixture(
          shared_arm_endpoint_bodies));
}

}  // namespace manyforge::deployment::test
