#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_headless/source_snapshot.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::headless {

inline constexpr std::string_view kHeadlessRuntimeSnapshotSchemaVersion =
    "manyforge.headless_runtime_snapshot.v1";
inline constexpr std::string_view kHeadlessRuntimeSnapshotIdentityDomain =
    "manyforge-headless-runtime-snapshot";

class RuntimeSnapshotError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct RuntimeSnapshotFreshnessPolicy final {
  std::int64_t manager_maximum_age_ns;
  std::int64_t logical_scene_maximum_age_ns;
  std::int64_t scene_runtime_maximum_age_ns;
};

struct HeadlessRuntimeSnapshot final {
  std::uint64_t projection_generation;
  std::int64_t projected_at_ns;
  std::string semantic_content_digest;
  manyforge::content_identity::CanonicalValue root;
};

/// Bounded verified source-envelope observation for the cold control watcher.
/// The fixed-size hint ignores only presentation-irrelevant source pulses;
/// it neither builds a projection nor grants command/owner authority.
[[nodiscard]] std::optional<std::pair<std::string, std::int64_t>> ReadControlSourceFileHint(
    const std::filesystem::path& path);

/// Verified Scene source for the demand-scoped viewer reader.
[[nodiscard]] std::optional<std::tuple<std::string, std::string, std::int64_t>>
ReadViewerSceneSource(const std::filesystem::path &path, const std::string &cell_id);
[[nodiscard]] std::optional<std::pair<std::string, std::string>>
ReadViewerPoseSessions(const std::filesystem::path &path, const std::string &cell_id);

/// Independently restartable, read-only projection over immutable Manager and
/// logical-scene source publications. It measures freshness from the exact
/// publication inode, retains stale source identity without relabelling it as
/// current truth, and never invokes either source owner.
class RuntimeSnapshotAggregator final {
public:
  using Clock = std::function<std::int64_t()>;

  RuntimeSnapshotAggregator(std::filesystem::path publication_root,
                            std::string cell_id,
                            std::string projection_owner_id,
                            std::string projection_owner_epoch,
                            RuntimeSnapshotFreshnessPolicy freshness_policy,
                            Clock wall_clock_ns, Clock projection_clock_ns);

  [[nodiscard]] HeadlessRuntimeSnapshot Evaluate();

private:
  AtomicSourceSnapshotChannel manager_source_;
  AtomicSourceSnapshotChannel logical_scene_source_;
  AtomicSourceSnapshotChannel scene_runtime_source_;
  std::string cell_id_;
  std::string projection_owner_id_;
  std::string projection_owner_epoch_;
  RuntimeSnapshotFreshnessPolicy freshness_policy_;
  Clock wall_clock_ns_;
  Clock projection_clock_ns_;
  std::mutex mutex_;
  std::uint64_t projection_generation_{};
  std::string previous_semantic_digest_;
  std::optional<SourceSnapshotFileObservation> last_manager_source_;
  std::optional<SourceSnapshotFileObservation> last_logical_scene_source_;
  std::optional<SourceSnapshotFileObservation> last_scene_runtime_source_;
};

} // namespace manyforge::headless
