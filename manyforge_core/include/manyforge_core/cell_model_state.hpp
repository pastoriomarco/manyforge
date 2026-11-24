#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <manyforge_core/component_stream_health.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

/// One component's observed model coordinates: its wait-free publication,
/// whose canonical joint names are compiled joint names of the cell model.
struct CellModelStateSource final {
  ControllableComponentId component_id;
  std::shared_ptr<const ObservedRobotStatePublication> publication;
  /// The component's stream-stale condition (the one that faults it). Empty:
  /// the component declares no stream, and its state is never stale.
  std::shared_ptr<const ComponentStreamStaleFlag> stream_stale{};
};

/// Every component of the cell that publishes model coordinates (robots and
/// model-backed grippers). One list serves every robot: a consumer binds it
/// to its own coordinate vector and its own coordinates drop out.
using CellModelStateSources = std::vector<CellModelStateSource>;

/// A robot's reference to the cell's current source list. The list is
/// immutable and replaced as a whole when the cell's components change.
/// Read at planning, validation and dispatch; never per collision check.
class CellModelStateHandle final {
 public:
  void Replace(std::shared_ptr<const CellModelStateSources> sources) noexcept {
    sources_.store(std::move(sources), std::memory_order_release);
  }
  [[nodiscard]] std::shared_ptr<const CellModelStateSources> Get() const noexcept {
    return sources_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<std::shared_ptr<const CellModelStateSources>> sources_;
};

/// One source bound to a named coordinate vector.
struct BoundCellModelStateSource final {
  static constexpr std::size_t kNotBound = std::numeric_limits<std::size_t>::max();

  ControllableComponentId component_id;
  std::shared_ptr<const ObservedRobotStatePublication> publication;
  /// Per coordinate of the publication: its index in the bound vector, or
  /// kNotBound when the vector has no such coordinate.
  std::vector<std::size_t> target_indices;
  std::shared_ptr<const ComponentStreamStaleFlag> stream_stale{};
};

/// Binds the sources to `coordinate_names`. A source that publishes any of
/// `own_coordinate_names` is the caller's own and is left out, and so is a
/// source none of whose coordinates the vector names.
[[nodiscard]] std::vector<BoundCellModelStateSource> bind_cell_model_state(
    const CellModelStateSources& sources, std::span<const std::string> coordinate_names,
    std::span<const std::string> own_coordinate_names);

/// True when `index` of the bound vector is written by one of `sources`.
[[nodiscard]] bool cell_model_state_binds(std::span<const BoundCellModelStateSource> sources,
                                          std::size_t index) noexcept;

/// What a consumer does with another component's latest sample.
enum class PeerStateVerdict : std::uint8_t {
  /// Write the sample's positions.
  kUse,
  /// The component is in its stream-stale condition: the one that faults it.
  kStale,
  /// Nothing usable was published.
  kAbsent,
};

/// The one decision on another component's observed state; planning,
/// validation, runtime collision checks and look-ahead all pass through it
/// (OD-343). It has no rule of its own: the state is stale exactly while the
/// stream registry classifies a `fault` stream of that component stale
/// (`stream_stale`, null when the component declares none), and absent when
/// `sample` is empty or unusable.
[[nodiscard]] PeerStateVerdict peer_state_verdict(
    const ObservedRobotStatePublication::ReadView& sample, std::size_t expected_coordinates,
    const ComponentStreamStaleFlag* stream_stale) noexcept;

/// The first source whose sample was not usable; empty when all were used.
struct PeerStateUnavailable final {
  const BoundCellModelStateSource* source{};
  PeerStateVerdict verdict{PeerStateVerdict::kUse};

  explicit operator bool() const noexcept { return source != nullptr; }
};

/// The refusal of a move that needs the state of `unavailable`'s component.
/// `code` is the refusing path's own code (planning, IK, validation); the
/// details carry `reason` = that component's stream condition
/// (`component_stream_stale` or `component_stream_absent`) and
/// `peer_component_id`.
[[nodiscard]] ErrorDTO peer_state_unavailable_error(const PeerStateUnavailable& unavailable,
                                                    std::string_view code);

/// Writes every bound source's latest observed positions into `coordinates`
/// and stops at the first source that is stale or absent. Wait-free and
/// allocation-free; each sample is released before the next is read, and
/// none is held after return.
[[nodiscard]] PeerStateUnavailable overlay_cell_model_state(
    std::span<const BoundCellModelStateSource> sources, std::int64_t now_ns,
    std::span<double> coordinates) noexcept;

/// Reads every bound source once and writes its positions into every waypoint
/// of `trajectory`, whose joint names the sources were bound to.
[[nodiscard]] PeerStateUnavailable overlay_cell_model_state(
    std::span<const BoundCellModelStateSource> sources, std::int64_t now_ns,
    TrajectoryDTO& trajectory);

}  // namespace manyforge::core
