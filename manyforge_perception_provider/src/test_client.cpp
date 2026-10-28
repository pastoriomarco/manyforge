// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#include <iostream>
#include <manyforge_perception_interfaces/action/estimate_instances.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>

using Action = manyforge_perception_interfaces::action::EstimateInstances;

namespace
{

std::string RequiredText(const rclcpp::Node::SharedPtr & node, const char * name)
{
  node->declare_parameter(name, rclcpp::ParameterType::PARAMETER_STRING);
  return node->get_parameter(name).as_string();
}

std::uint32_t Unsigned(const rclcpp::Node::SharedPtr & node, const char * name, std::int64_t value)
{
  const auto parameter = node->declare_parameter<std::int64_t>(name, value);
  if (parameter < 0 || parameter > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument(std::string(name) + " outside uint32");
  }
  return static_cast<std::uint32_t>(parameter);
}

}  // namespace

// Qualification client: prints full generated YAML for every feedback/result,
// not a second provider or a Program integration. The caller supplies its own
// explicit server-discovery escape bound; this is not a provider work timeout.
int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<rclcpp::Node>("perception_test_client");
  int exit_code = 1;
  try {
    Action::Goal goal;
    goal.request_id = RequiredText(node, "request_id");
    goal.model_id = RequiredText(node, "model_id");
    goal.model_revision = RequiredText(node, "model_revision");
    goal.attempt = Unsigned(node, "attempt", 0);
    goal.execution_id = node->declare_parameter<std::string>("execution_id", "");
    goal.viewpoint_id = node->declare_parameter<std::string>("viewpoint_id", "");
    goal.max_instances = Unsigned(node, "max_instances", 5);
    goal.settle_frames = Unsigned(node, "settle_frames", 0);
    goal.work_budget = rclcpp::Duration::from_seconds(
      node->declare_parameter<double>("work_budget_s", 5.0));
    node->declare_parameter("min_detection_score", rclcpp::ParameterType::PARAMETER_DOUBLE);
    goal.min_detection_score = node->get_parameter("min_detection_score").as_double();
    goal.preempt = node->declare_parameter<bool>("preempt", false);
    const auto capture = node->declare_parameter<std::string>("capture", "next");
    if (capture == "next") {goal.capture_mode = goal.CAPTURE_NEXT;} else if (capture == "latest") {
      goal.capture_mode = goal.CAPTURE_LATEST;
      node->declare_parameter("max_capture_age_s", rclcpp::ParameterType::PARAMETER_DOUBLE);
      goal.max_capture_age =
        rclcpp::Duration::from_seconds(node->get_parameter("max_capture_age_s").as_double());
    } else {throw std::invalid_argument("capture must be next or latest");}
    const auto reuse = node->declare_parameter<std::string>("reuse", "refine");
    if (reuse == "none") {goal.reuse_policy = goal.REUSE_NONE;} else if (reuse == "refine") {
      goal.reuse_policy = goal.REUSE_REFINE;
    } else if (reuse == "reuse_and_refine") {goal.reuse_policy = goal.REUSE_AND_REFINE;} else {
      throw std::invalid_argument("unknown reuse policy");
    }
    const auto order = node->declare_parameter<std::string>("order", "score");
    if (order == "score") {goal.order = goal.ORDER_SCORE;} else if (order == "region_center") {
      goal.order = goal.ORDER_REGION_CENTER;
    } else if (order == "top_to_bottom") {goal.order = goal.ORDER_TOP_TO_BOTTOM;} else {
      throw std::invalid_argument("unknown order");
    }
    const auto membership = node->declare_parameter<std::string>("region_membership", "whole");
    if (membership == "whole") {
      goal.region_membership = goal.REGION_WHOLE;
    } else if (membership == "centre") {goal.region_membership = goal.REGION_CENTRE;} else {
      throw std::invalid_argument("unknown region membership");
    }
    const auto region = node->declare_parameter<std::string>("region", "none");
    if (region == "image_box" || region == "space_box") {
      node->declare_parameter("region_bounds", rclcpp::ParameterType::PARAMETER_DOUBLE_ARRAY);
      const auto bounds = node->get_parameter("region_bounds").as_double_array();
      if (region == "image_box" && bounds.size() == 4) {
        goal.region.kind = goal.region.IMAGE_BOX;
        std::copy(bounds.begin(), bounds.end(), goal.region.image_box.begin());
      } else if (region == "space_box" && bounds.size() == 6) {
        goal.region.kind = goal.region.SPACE_BOX;
        goal.region.frame_id = RequiredText(node, "region_frame");
        goal.region.minimum.x = bounds[0];
        goal.region.minimum.y = bounds[1];
        goal.region.minimum.z = bounds[2];
        goal.region.maximum.x = bounds[3];
        goal.region.maximum.y = bounds[4];
        goal.region.maximum.z = bounds[5];
      } else {throw std::invalid_argument("region_bounds needs 4 image or 6 space values");}
    } else if (region != "none") {throw std::invalid_argument("unknown region");}
    auto client = rclcpp_action::create_client<Action>(
      node, node->declare_parameter<std::string>("action_name", "estimate_instances"));
    node->declare_parameter("server_wait_s", rclcpp::ParameterType::PARAMETER_INTEGER);
    const auto wait_s = node->get_parameter("server_wait_s").as_int();
    if (wait_s < 0 || !client->wait_for_action_server(std::chrono::seconds(wait_s))) {
      throw std::runtime_error("provider action server unavailable within explicit client bound");
    }
    rclcpp_action::Client<Action>::SendGoalOptions options;
    options.feedback_callback = [](auto, const auto feedback) {
        std::cout << "--- feedback\n" <<
          manyforge_perception_interfaces::action::to_yaml(*feedback) << std::flush;
      };
    options.goal_response_callback = [&](const auto handle) {
        if (!handle) {std::cerr << "goal rejected\n"; rclcpp::shutdown();}
      };
    options.result_callback = [&](const auto & result) {
        std::cout << "--- result\nros_result_code: " << static_cast<int>(result.code) << '\n';
        if (result.result) {
          std::cout << manyforge_perception_interfaces::action::to_yaml(*result.result) <<
            std::flush;
          exit_code = result.result->outcome == Action::Result::FAILED ? 1 : 0;
        }
        rclcpp::shutdown();
      };
    client->async_send_goal(goal, options);
    rclcpp::spin(node);
  } catch (const std::exception & error) {
    std::cerr << "perception client: " << error.what() << '\n';
  }
  if (rclcpp::ok()) {rclcpp::shutdown();}
  return exit_code;
}
