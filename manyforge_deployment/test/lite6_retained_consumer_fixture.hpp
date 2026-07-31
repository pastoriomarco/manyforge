#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/retained_bundle.hpp>

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
materialize_lite6_retained_consumer_fixture(
    const std::filesystem::path& package_root) {
  auto controllable = make_lite6_controllable_retention_fixture();
  auto qualified = controllable.qualified;
  const Lite6ControllableCompilerInputArtifactSource input_source(
      controllable);
  FilesystemBuildManifestRepository manifest_repository(package_root);
  FilesystemCatalogArtifactStore catalog_store(package_root);
  FilesystemRawArtifactStore raw_store(package_root);
  FilesystemRetainedBundleRepository bundle_repository(package_root);
  auto bundle =
      detail::RetainedBundleMaterializer::
          materialize_store_and_reload_production(
              qualified, manifest_repository, input_source,
              controllable.structural_preparations(), catalog_store,
              raw_store,
              bundle_repository);
  if (bundle == nullptr) {
    throw std::logic_error(
        "non-empty Lite6 materializer returned no durable bundle");
  }
  return {
      std::move(controllable.structural),
      std::move(bundle),
  };
}

}  // namespace manyforge::deployment::test
