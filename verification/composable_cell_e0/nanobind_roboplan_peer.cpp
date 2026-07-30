#include <nanobind/nanobind.h>

#include <manyforge_core/types/geometry.hpp>

namespace nb = nanobind;
using namespace nb::literals;

namespace {

nb::dict pose_round_trip(const nb::dict &input) {
  const auto position = nb::cast<nb::dict>(input["position_m"]);
  const auto orientation = nb::cast<nb::dict>(input["orientation"]);
  const manyforge::core::Pose3DTO pose{
      .position_m =
          {
              .x_m = nb::cast<double>(position["x_m"]),
              .y_m = nb::cast<double>(position["y_m"]),
              .z_m = nb::cast<double>(position["z_m"]),
          },
      .orientation =
          {
              .x = nb::cast<double>(orientation["x"]),
              .y = nb::cast<double>(orientation["y"]),
              .z = nb::cast<double>(orientation["z"]),
              .w = nb::cast<double>(orientation["w"]),
          },
  };
  nb::dict output_position;
  output_position["x_m"] = pose.position_m.x_m;
  output_position["y_m"] = pose.position_m.y_m;
  output_position["z_m"] = pose.position_m.z_m;
  nb::dict output_orientation;
  output_orientation["x"] = pose.orientation.x;
  output_orientation["y"] = pose.orientation.y;
  output_orientation["z"] = pose.orientation.z;
  output_orientation["w"] = pose.orientation.w;
  nb::dict output;
  output["position_m"] = output_position;
  output["orientation"] = output_orientation;
  return output;
}

} // namespace

NB_MODULE(e0_nanobind_roboplan_peer, module) {
  module.def("pose_round_trip", &pose_round_trip, "value"_a);
}
