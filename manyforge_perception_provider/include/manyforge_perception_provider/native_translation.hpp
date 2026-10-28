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
#pragma once

#include <string>
#include <vector>
#include "manyforge_perception_provider/provider_model.hpp"
#include <builtin_interfaces/msg/time.hpp>
#include <isaac_ros_tensor_msgs/msg/tensor_list.hpp>
#include <manyforge_perception_interfaces/action/estimate_instances.hpp>
#include <sensor_msgs/msg/camera_info.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <vision_msgs/msg/detection2_d_array.hpp>
#include <vision_msgs/msg/detection3_d_array.hpp>

namespace manyforge::perception
{

using EstimateInstances = manyforge_perception_interfaces::action::EstimateInstances;

Nanoseconds SourceTime(const builtin_interfaces::msg::Time & stamp);
builtin_interfaces::msg::Time RosTime(Nanoseconds stamp);
Request FromGoal(const EstimateInstances::Goal & goal);
EstimateInstances::Result ToResult(const Result & result);
EstimateInstances::Feedback ToFeedback(const Feedback & feedback);

// CPU subscriptions retain the source buffer owners. No idle conversion or
// implicit CUDA-to-CPU copy is performed by this translator.
Capture FromCapture(
  sensor_msgs::msg::Image::ConstSharedPtr rgb,
  sensor_msgs::msg::Image::ConstSharedPtr depth,
  sensor_msgs::msg::CameraInfo::ConstSharedPtr camera);
std::vector<Detection> FromDetections(
  const vision_msgs::msg::Detection2DArray & detections, const Capture & capture);

struct NativeInputs
{
  sensor_msgs::msg::Image rgb;
  sensor_msgs::msg::CameraInfo camera;
  std::optional<sensor_msgs::msg::Image> depth, mask;
  std::optional<isaac_ros_tensor_msgs::msg::TensorList> seed;
};

// Payload preparation precedes the model's final publication admission check.
NativeInputs MakeNativeInputs(const Admission & admission, std::uint32_t mask_margin_px);
Pose FromNativePose(
  const vision_msgs::msg::Detection3DArray & pose,
  const isaac_ros_tensor_msgs::msg::TensorList & matrix,
  Nanoseconds expected_processing_stamp, const std::string & optical_frame);

}  // namespace manyforge::perception
