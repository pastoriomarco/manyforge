#include <algorithm>
#include <cmath>
#include <manyforge_core/cell_model_state.hpp>
#include <manyforge_core/component_stream_health.hpp>
#include <string>

namespace manyforge::core {

std::vector<BoundCellModelStateSource> bind_cell_model_state(
    const CellModelStateSources& sources, const std::span<const std::string> coordinate_names,
    const std::span<const std::string> own_coordinate_names) {
  std::vector<BoundCellModelStateSource> bound;
  bound.reserve(sources.size());
  for (const auto& source : sources) {
    if (!source.publication) {
      continue;
    }
    const auto names = source.publication->canonical_joint_names();
    const bool own = std::any_of(names.begin(), names.end(), [&](const std::string& name) {
      return std::find(own_coordinate_names.begin(), own_coordinate_names.end(), name) !=
             own_coordinate_names.end();
    });
    if (own) {
      continue;
    }
    BoundCellModelStateSource candidate{source.component_id, source.publication, {},
                                        source.stream_stale};
    candidate.target_indices.reserve(names.size());
    bool any = false;
    for (const auto& name : names) {
      const auto found = std::find(coordinate_names.begin(), coordinate_names.end(), name);
      if (found == coordinate_names.end()) {
        candidate.target_indices.push_back(BoundCellModelStateSource::kNotBound);
        continue;
      }
      candidate.target_indices.push_back(
          static_cast<std::size_t>(std::distance(coordinate_names.begin(), found)));
      any = true;
    }
    if (any) {
      bound.push_back(std::move(candidate));
    }
  }
  return bound;
}

bool cell_model_state_binds(const std::span<const BoundCellModelStateSource> sources,
                            const std::size_t index) noexcept {
  return std::any_of(sources.begin(), sources.end(), [&](const BoundCellModelStateSource& source) {
    return std::find(source.target_indices.begin(), source.target_indices.end(), index) !=
           source.target_indices.end();
  });
}

PeerStateVerdict peer_state_verdict(const ObservedRobotStatePublication::ReadView& sample,
                                    const std::size_t expected_coordinates,
                                    const ComponentStreamStaleFlag* stream_stale) noexcept {
  if (!sample || !sample.complete() || sample.positions_rad().size() != expected_coordinates) {
    return PeerStateVerdict::kAbsent;
  }
  const auto positions = sample.positions_rad();
  if (!std::all_of(positions.begin(), positions.end(),
                   [](const double value) { return std::isfinite(value); })) {
    return PeerStateVerdict::kAbsent;
  }
  return stream_stale != nullptr && stream_stale->stale() ? PeerStateVerdict::kStale
                                                          : PeerStateVerdict::kUse;
}

ErrorDTO peer_state_unavailable_error(const PeerStateUnavailable& unavailable,
                                      const std::string_view code) {
  const bool stale = unavailable.verdict == PeerStateVerdict::kStale;
  const std::string component =
      unavailable.source != nullptr ? unavailable.source->component_id.value() : std::string{};
  return ErrorDTO{std::string(code),
                  "Component " + component + " observed state " +
                      (stale ? "is stale" : "has no sample"),
                  {{"reason",
                    std::string(stale ? kComponentStreamStaleCode : kComponentStreamAbsentCode)},
                   {"peer_component_id", component}}};
}

PeerStateUnavailable overlay_cell_model_state(
    const std::span<const BoundCellModelStateSource> sources, const std::int64_t now_ns,
    const std::span<double> coordinates) noexcept {
  for (const auto& source : sources) {
    const auto sample = source.publication->acquire(now_ns);
    const auto verdict =
        peer_state_verdict(sample, source.target_indices.size(), source.stream_stale.get());
    if (verdict != PeerStateVerdict::kUse) {
      return {&source, verdict};
    }
    const auto positions = sample.positions_rad();
    for (std::size_t index = 0; index < source.target_indices.size(); ++index) {
      const auto target = source.target_indices[index];
      if (target < coordinates.size()) {
        coordinates[target] = positions[index];
      }
    }
  }
  return {};
}

PeerStateUnavailable overlay_cell_model_state(
    const std::span<const BoundCellModelStateSource> sources, const std::int64_t now_ns,
    TrajectoryDTO& trajectory) {
  if (sources.empty() || trajectory.waypoints.empty()) {
    return {};
  }
  auto& first = trajectory.waypoints.front().positions_rad;
  if (const auto unavailable =
          overlay_cell_model_state(sources, now_ns, std::span<double>(first))) {
    return unavailable;
  }
  for (std::size_t waypoint = 1; waypoint < trajectory.waypoints.size(); ++waypoint) {
    auto& positions = trajectory.waypoints[waypoint].positions_rad;
    for (const auto& source : sources) {
      for (const auto target : source.target_indices) {
        if (target < positions.size() && target < first.size()) {
          positions[target] = first[target];
        }
      }
    }
  }
  return {};
}

}  // namespace manyforge::core
