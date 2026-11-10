// stop_replacement_export: test-support exporter of Core's exact Stop
// replacement trajectories for pinned-JTC characterization. Built with the
// Core tests; never installed or shipped. It
// calls the real Core functions (`compute_stop_join_elapsed_s`,
// `plan_controlled_hard_stop_brake`, `convert_stop_trajectory_to_path_time`,
// `build_stop_replacement_join`); it has no second implementation.
//
// Usage: stop_replacement_export INPUT.json OUTPUT.json
//
// INPUT is one RFC 8785 canonical JSON object (sorted keys, no whitespace,
// ECMAScript number spelling: whole numbers without ".0", e.g. `1`, `0.5`,
// `1e-7`):
//
//   active                  trajectory  the active goal's trajectory as sent
//                                       (path time; may hold negative times)
//   controller_period_s     number      controller update period (> 0)
//   frozen_factor           number      applied speed factor in (0, 1]
//   max_deceleration_rad_s2 object      joint name -> lane max_deceleration_on_cancel
//   ramp                    object      optional: {"lead_in_s": number,
//                                       "origin_path_s": number,
//                                       "trajectory": trajectory} - a wall-time
//                                       Stop lead-in and ramp as Core's planner
//                                       emits it (time 0 = the trip sample at
//                                       `origin_path_s` on the active path; the
//                                       ramp starts at wall time `lead_in_s`,
//                                       the planner's `stop_traj_delta_s`)
//   sample                  object      latest associated controller sample:
//                                       {"cursor_s": number,
//                                        "reference_path_velocities": [number],
//                                        "reference_positions_rad": [number]}
//                                       in the active trajectory's joint order
//   start_allowance_s       number      measured dispatch-to-start allowance
//   wall_since_sample_s     number      local time from sample receipt to dispatch
//
//   trajectory = {"joint_names": [string], "points": [point]}
//   point      = {"a": [number], "p": [number], "t": number, "v": [number]}
//                (accelerations, positions, time_from_start, velocities)
//
// OUTPUT (canonical JSON):
//
//   elapsed_path_s  number   elapsed: cursor + factor * (wall + allowance)
//                            (`elapsed_error` {"error": ...} instead on refusal)
//   brake           object   level 3: {"limiting_joint", "stop_path_time_s",
//                            "stop_time_s", "trajectory" (brake alone, path time,
//                            time 0 = the sample), "replace" (the join sent
//                            with Replace)} or {"error": {"message", "reason"}}
//   ramp            object   present with `ramp`: {"path_time" (the ramp in path
//                            time at the factor), "replace" (the join: the
//                            active points before the trip point, the trip
//                            point, then the ramp after it), "trip_path_s"
//                            (origin_path_s + factor * lead_in_s)} or
//                            {"error": {...}}
//
// Exit status: 0 written (Core refusals are reported as `error` data), 64 usage,
// 65 malformed input, 74 I/O failure.

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/motion_interrupt/stop_replacement.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using manyforge::content_identity::CanonicalValue;
namespace core = manyforge::core;

const CanonicalValue& member(const CanonicalValue& object, std::string_view name) {
  const auto* found = object.find_object_member(name);
  if (found == nullptr) throw std::invalid_argument("missing member: " + std::string(name));
  return *found;
}

std::vector<double> numbers(const CanonicalValue& value) {
  std::vector<double> result;
  for (const auto& item : value.as_array()) result.push_back(item.as_number());
  return result;
}

core::TrajectoryDTO trajectory(const CanonicalValue& value) {
  core::TrajectoryDTO result;
  for (const auto& name : member(value, "joint_names").as_array()) {
    result.joint_names.push_back(name.as_string());
  }
  for (const auto& point : member(value, "points").as_array()) {
    result.waypoints.push_back(core::TrajectoryWaypointDTO{
        member(point, "t").as_number(), numbers(member(point, "p")), numbers(member(point, "v")),
        numbers(member(point, "a"))});
  }
  return result;
}

CanonicalValue number(double value) { return CanonicalValue(value == 0.0 ? 0.0 : value); }

CanonicalValue number_array(const std::vector<double>& values) {
  CanonicalValue::Array array;
  for (const auto value : values) array.push_back(number(value));
  return CanonicalValue(std::move(array));
}

CanonicalValue object(CanonicalValue::Object members) {
  std::sort(members.begin(), members.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
  return CanonicalValue(std::move(members));
}

CanonicalValue trajectory_json(const core::TrajectoryDTO& value) {
  CanonicalValue::Array names;
  for (const auto& name : value.joint_names) names.emplace_back(name);
  CanonicalValue::Array points;
  for (const auto& point : value.waypoints) {
    points.push_back(object({{"a", number_array(point.accelerations_rad_s2)},
                             {"p", number_array(point.positions_rad)},
                             {"t", number(point.time_from_start_s)},
                             {"v", number_array(point.velocities_rad_s)}}));
  }
  return object({{"joint_names", CanonicalValue(std::move(names))},
                 {"points", CanonicalValue(std::move(points))}});
}

CanonicalValue error_json(const core::ErrorDTO& error) {
  const auto reason = error.details.find("reason");
  return object({{"error", object({{"message", CanonicalValue(error.message)},
                                   {"reason", CanonicalValue(reason == error.details.end()
                                                                 ? error.code
                                                                 : reason->second)}})}});
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: stop_replacement_export INPUT.json OUTPUT.json\n";
    return 64;
  }
  std::ostringstream text;
  {
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
      std::cerr << "cannot read " << argv[1] << "\n";
      return 74;
    }
    text << input.rdbuf();
  }
  CanonicalValue output;
  try {
    const auto input = manyforge::content_identity::parse_canonical_json(text.str());
    const auto active = trajectory(member(input, "active"));
    const auto& sample = member(input, "sample");
    const double cursor = member(sample, "cursor_s").as_number();
    const double factor = member(input, "frozen_factor").as_number();
    std::map<std::string, double> limits;
    for (const auto& [name, value] : member(input, "max_deceleration_rad_s2").as_object()) {
      limits.emplace(name, value.as_number());
    }

    CanonicalValue::Object result;
    const auto elapsed = core::compute_stop_join_elapsed_s(
        {cursor, factor, member(input, "wall_since_sample_s").as_number(),
         member(input, "start_allowance_s").as_number(), std::nullopt});
    if (!elapsed.ok()) {
      result.emplace_back("elapsed_error", error_json(elapsed.error()));
    } else {
      result.emplace_back("elapsed_path_s", number(elapsed.value()));
      const auto brake = core::plan_controlled_hard_stop_brake(
          {active.joint_names, numbers(member(sample, "reference_positions_rad")),
           numbers(member(sample, "reference_path_velocities")), factor, limits,
           member(input, "controller_period_s").as_number()});
      if (!brake.ok()) {
        result.emplace_back("brake", error_json(brake.error()));
      } else {
        const auto joined = core::build_stop_replacement_join(
            active, elapsed.value(), brake.value().trajectory, cursor, cursor);
        CanonicalValue::Object brake_json{
            {"limiting_joint", CanonicalValue(brake.value().limiting_joint)},
            {"stop_path_time_s", number(brake.value().stop_path_time_s)},
            {"stop_time_s", number(brake.value().stop_time_s)},
            {"trajectory", trajectory_json(brake.value().trajectory)}};
        brake_json.emplace_back("replace", joined.ok() ? trajectory_json(joined.value())
                                                       : error_json(joined.error()));
        result.emplace_back("brake", object(std::move(brake_json)));
      }
      if (const auto* ramp = input.find_object_member("ramp"); ramp != nullptr) {
        const auto path_time = core::convert_stop_trajectory_to_path_time(
            trajectory(member(*ramp, "trajectory")), factor);
        if (!path_time.ok()) {
          result.emplace_back("ramp", error_json(path_time.error()));
        } else {
          const double origin = member(*ramp, "origin_path_s").as_number();
          const double trip =
              core::stop_trip_path_s(origin, factor, member(*ramp, "lead_in_s").as_number());
          const auto joined = core::build_stop_replacement_join(active, elapsed.value(),
                                                                path_time.value(), origin, trip);
          result.emplace_back(
              "ramp", object({{"path_time", trajectory_json(path_time.value())},
                              {"replace", joined.ok() ? trajectory_json(joined.value())
                                                      : error_json(joined.error())},
                              {"trip_path_s", number(trip)}}));
        }
      }
    }
    output = object(std::move(result));
  } catch (const std::exception& error) {
    std::cerr << "malformed input: " << error.what() << "\n";
    return 65;
  }
  std::ofstream file(argv[2], std::ios::binary);
  file << manyforge::content_identity::canonical_json(output);
  if (!file) {
    std::cerr << "cannot write " << argv[2] << "\n";
    return 74;
  }
  return 0;
}
