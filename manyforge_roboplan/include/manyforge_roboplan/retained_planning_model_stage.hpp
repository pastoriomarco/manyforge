#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

class RetainedPlanningStageError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// One already-verified retained planning resource copied across the
/// deployment/RoboPlan boundary.
///
/// This deliberately contains values rather than a deployment-layer type.
/// The deployment reader remains responsible for sealing the original
/// retained-product proof; this adapter defensively rechecks the complete
/// resource identity immediately before exposing bytes to a file-only
/// third-party mesh loader.
struct RetainedPlanningResourceInput final {
  std::string logical_uri;
  std::string media_type;
  std::uint64_t byte_size;
  std::string content_digest;
  std::string content_addressed_store_key;
  std::string bytes;
};

/// Exact retained planning-model inputs needed by RoboPlan.
struct RetainedPlanningModelInput final {
  std::string urdf_bytes;
  std::string srdf_bytes;
  std::vector<RetainedPlanningResourceInput> resources;
};

/// Caller-selectable lower bounds for focused tests and constrained
/// deployments. A caller may tighten but never raise the hard production
/// ceilings.
struct RetainedPlanningStageLimits final {
  std::uint64_t maximum_resource_count = 4096U;
  std::uint64_t maximum_xml_bytes = 16ULL * 1024ULL * 1024ULL;
  std::uint64_t maximum_resource_bytes = 256ULL * 1024ULL * 1024ULL;
  std::uint64_t maximum_total_bytes = 512ULL * 1024ULL * 1024ULL;
};

namespace detail {

template <typename Value>
[[nodiscard]] std::string retained_planning_text(Value&& value) {
  if constexpr (requires { std::forward<Value>(value).value(); }) {
    return std::string(std::forward<Value>(value).value());
  } else {
    return std::string(std::forward<Value>(value));
  }
}

template <typename Value>
[[nodiscard]] std::uint64_t retained_planning_size(Value&& value) {
  if constexpr (requires { std::forward<Value>(value).value(); }) {
    return static_cast<std::uint64_t>(
        std::forward<Value>(value).value());
  } else {
    return static_cast<std::uint64_t>(std::forward<Value>(value));
  }
}

}  // namespace detail

/// Private, per-instance retained-resource staging lease for RoboPlan.
///
/// Pinocchio's URDF geometry loader accepts package/file paths but does not
/// understand ManyForge's closure-local `asset://` scheme. This adapter:
///
/// - validates every retained resource again;
/// - writes it below one private, uniquely owned directory;
/// - derives a loader-only URDF whose exact `filename="asset://..."` values
///   become `filename="package://..."`; and
/// - exposes exactly one package search root, with no ambient ROS, current
///   working-directory, or source-package fallback.
///
/// Keep this object alive for at least as long as the RoboPlan Scene built
/// from its loader inputs. Destruction removes the private staging tree.
class RetainedPlanningModelStage final {
 public:
  static constexpr std::uint64_t kHardMaximumResourceCount = 4096U;
  static constexpr std::uint64_t kHardMaximumXmlBytes =
      16ULL * 1024ULL * 1024ULL;
  static constexpr std::uint64_t kHardMaximumResourceBytes =
      256ULL * 1024ULL * 1024ULL;
  static constexpr std::uint64_t kHardMaximumTotalBytes =
      512ULL * 1024ULL * 1024ULL;

  /// Stages one copied, already-verified planning model below an existing
  /// trusted parent directory.
  [[nodiscard]] static RetainedPlanningModelStage create(
      RetainedPlanningModelInput input,
      const std::filesystem::path& staging_parent,
      RetainedPlanningStageLimits limits = {});

  /// Convenience bridge for the planned deployment-side sealed value:
  ///
  ///   model.urdf_bytes(), model.srdf_bytes(), model.resources()
  ///
  /// and, for every resource:
  ///
  ///   logical_uri(), media_type(), byte_size(), content_digest(),
  ///   content_addressed_store_key(), bytes().
  ///
  /// No deployment header or compiler target enters the RoboPlan library.
  template <typename VerifiedRetainedPlanningModel>
  [[nodiscard]] static RetainedPlanningModelStage create_verified(
      const VerifiedRetainedPlanningModel& model,
      const std::filesystem::path& staging_parent,
      RetainedPlanningStageLimits limits = {}) {
    RetainedPlanningModelInput input;
    input.urdf_bytes =
        detail::retained_planning_text(model.urdf_bytes());
    input.srdf_bytes =
        detail::retained_planning_text(model.srdf_bytes());
    for (const auto& resource : model.resources()) {
      input.resources.push_back(RetainedPlanningResourceInput{
          detail::retained_planning_text(resource.logical_uri()),
          detail::retained_planning_text(resource.media_type()),
          detail::retained_planning_size(resource.byte_size()),
          detail::retained_planning_text(resource.content_digest()),
          detail::retained_planning_text(
              resource.content_addressed_store_key()),
          detail::retained_planning_text(resource.bytes()),
      });
    }
    return create(std::move(input), staging_parent, limits);
  }

  ~RetainedPlanningModelStage();

  RetainedPlanningModelStage(const RetainedPlanningModelStage&) = delete;
  RetainedPlanningModelStage& operator=(
      const RetainedPlanningModelStage&) = delete;
  RetainedPlanningModelStage(RetainedPlanningModelStage&& other) noexcept;
  RetainedPlanningModelStage& operator=(
      RetainedPlanningModelStage&& other) noexcept;

  /// Derived loader-only URDF. The retained canonical URDF is never changed.
  [[nodiscard]] std::string_view loader_urdf_bytes() const noexcept {
    return loader_urdf_bytes_;
  }

  /// Exact retained SRDF bytes.
  [[nodiscard]] std::string_view srdf_bytes() const noexcept {
    return srdf_bytes_;
  }

  /// Private loader files consumed by path-only third-party adapters.
  ///
  /// These are derived runtime files under `staging_root()`, not retained
  /// product identities. Their bytes are published and read back before the
  /// stage's ready marker becomes visible.
  [[nodiscard]] std::filesystem::path loader_urdf_path() const {
    return staging_root_ / "retained-model.urdf";
  }
  [[nodiscard]] std::filesystem::path srdf_path() const {
    return staging_root_ / "retained-model.srdf";
  }

  /// Exactly one private package-search root for RoboPlan/Pinocchio.
  [[nodiscard]] const std::vector<std::filesystem::path>& package_paths()
      const noexcept {
    return package_paths_;
  }

  /// Exposed for lifecycle wiring, diagnostics, and focused tests. It is not a
  /// stable product identity or a path that may be persisted in a bundle.
  [[nodiscard]] const std::filesystem::path& staging_root() const noexcept {
    return staging_root_;
  }

 private:
  RetainedPlanningModelStage(
      std::filesystem::path staging_root, std::string loader_urdf_bytes,
      std::string srdf_bytes);

  void cleanup() noexcept;

  std::filesystem::path staging_root_;
  std::string loader_urdf_bytes_;
  std::string srdf_bytes_;
  std::vector<std::filesystem::path> package_paths_;
};

}  // namespace manyforge::roboplan_backend
