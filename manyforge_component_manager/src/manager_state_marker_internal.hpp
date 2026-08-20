#pragma once

#include <filesystem>
#include <string>

namespace manyforge::component_manager {

struct ManagerStateMarkerIdentity final {
  std::string admission_digest;
  std::string cell_id;
  std::string retained_bundle_id;
  std::string graph_revision;

  friend bool operator==(
      const ManagerStateMarkerIdentity&,
      const ManagerStateMarkerIdentity&) = default;
};

enum class ManagerStateMarkerPreflightResult {
  kClean,
  kValid,
};

[[nodiscard]] ManagerStateMarkerPreflightResult
PreflightManagerStateMarker(
    const std::filesystem::path& stable_state_directory,
    const ManagerStateMarkerIdentity& expected);

void WriteManagerStateMarker(
    const std::filesystem::path& stable_state_directory,
    const ManagerStateMarkerIdentity& identity);

}  // namespace manyforge::component_manager
