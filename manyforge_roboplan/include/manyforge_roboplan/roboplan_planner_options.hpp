#pragma once

#include <string>

#include <roboplan_rrt/rrt.hpp>
#include <roboplan_simple_ik/simple_ik.hpp>

namespace manyforge::roboplan_backend {

inline roboplan::RRTOptions production_rrt_options(
    const std::string& group_name, double max_planning_time_s) {
  roboplan::RRTOptions options;
  options.group_name = group_name;
  options.max_planning_time = max_planning_time_s;
  options.max_nodes = 5000;
  return options;
}

inline roboplan::SimpleIkOptions production_planning_ik_options(
    const std::string& group_name) {
  roboplan::SimpleIkOptions options;
  options.group_name = group_name;
  options.step_size = 0.25;
  options.max_restarts = 50;
  options.max_time = 1.0;
  return options;
}

inline roboplan::SimpleIkOptions production_single_shot_ik_options(
    const std::string& group_name, double max_time_s) {
  auto options = production_planning_ik_options(group_name);
  options.max_time = max_time_s;
  return options;
}

}  // namespace manyforge::roboplan_backend
