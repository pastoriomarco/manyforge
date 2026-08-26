#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::headless {

inline constexpr std::string_view kNodeEnvironmentProjectionApiVersion =
    "manyforge.node_environment_projection.v2";

class NodeEnvironmentProjectionError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Read-only projection of the immutable NodeEnvironment selected by the
/// Manager's active retained bundle.
///
/// The projection independently reopens the build-qualified environment
/// manifest from the deployment package, verifies the packaged v2 catalog
/// against that manifest and the active bundle, and adds only dynamic
/// availability facts. It owns no catalog meaning, implementation factory,
/// Program Binding, or execution admission.
class ActiveNodeEnvironmentProjection final {
public:
  ActiveNodeEnvironmentProjection(std::filesystem::path package_root,
                                  std::filesystem::path node_catalog_path,
                                  std::filesystem::path node_environment_manifest_path);
  ~ActiveNodeEnvironmentProjection();

  ActiveNodeEnvironmentProjection(const ActiveNodeEnvironmentProjection &) =
      delete;
  ActiveNodeEnvironmentProjection &
  operator=(const ActiveNodeEnvironmentProjection &) = delete;
  ActiveNodeEnvironmentProjection(ActiveNodeEnvironmentProjection &&) = delete;
  ActiveNodeEnvironmentProjection &
  operator=(ActiveNodeEnvironmentProjection &&) = delete;

  [[nodiscard]] manyforge::content_identity::CanonicalValue
  Project(const manyforge::deployment::RetainedBundle &active_bundle,
          bool manager_runtime_converged) const;

private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
};

} // namespace manyforge::headless
