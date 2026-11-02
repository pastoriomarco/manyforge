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
#include "manyforge_perception_provider/native_translation.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace manyforge::perception
{
namespace
{

constexpr Nanoseconds kSecond = 1'000'000'000;

struct CaptureMessages
{
  sensor_msgs::msg::Image::ConstSharedPtr rgb, depth;
  sensor_msgs::msg::CameraInfo::ConstSharedPtr camera;
};

Nanoseconds Duration(const builtin_interfaces::msg::Duration & duration)
{
  if (duration.nanosec >= kSecond) {throw std::invalid_argument("noncanonical duration");}
  return static_cast<Nanoseconds>(duration.sec) * kSecond + duration.nanosec;
}

std_msgs::msg::Header CaptureHeader(const std::shared_ptr<const Capture> & capture)
{
  std_msgs::msg::Header header;
  if (capture) {
    header.stamp = RosTime(capture->source_stamp_ns); header.frame_id = capture->frame_id;
  }
  return header;
}

manyforge_perception_interfaces::msg::Detection DetectionMessage(const Detection & detection)
{
  manyforge_perception_interfaces::msg::Detection value;
  value.index = detection.index;
  value.detection_score = detection.score;
  value.bbox = {detection.box.x, detection.box.y, detection.box.width, detection.box.height};
  value.selected = detection.selected;
  return value;
}

manyforge_perception_interfaces::msg::Instance InstanceMessage(
  const Instance & instance, const std::shared_ptr<const Capture> & capture)
{
  manyforge_perception_interfaces::msg::Instance value;
  value.index = instance.index;
  value.status = static_cast<std::uint8_t>(instance.status);
  value.method = static_cast<std::uint8_t>(instance.method);
  value.reason = instance.reason;
  value.pose.header = CaptureHeader(capture);
  value.pose.pose.position.x = instance.pose.position[0];
  value.pose.pose.position.y = instance.pose.position[1];
  value.pose.pose.position.z = instance.pose.position[2];
  value.pose.pose.orientation.x = instance.pose.orientation[0];
  value.pose.pose.orientation.y = instance.pose.orientation[1];
  value.pose.pose.orientation.z = instance.pose.orientation[2];
  value.pose.pose.orientation.w = instance.pose.orientation[3];
  value.estimate_ms = instance.estimate_ms;
  return value;
}

// Canonical little-endian IEEE-754 values, dimensions and D length; no
// capture/processing timestamp contributes to the calibration identity.
std::string CalibrationId(const sensor_msgs::msg::CameraInfo & camera)
{
  std::vector<unsigned char> bytes;
  auto integer = [&](std::uint64_t value, unsigned count) {
      for (unsigned i = 0; i < count; ++i) {
        bytes.push_back((value >> (8 * i)) & 0xff);
      }
    };
  auto real = [&](double value) {
      if (!std::isfinite(value)) {throw std::invalid_argument("nonfinite calibration");}
      std::uint64_t bits;
      static_assert(sizeof(bits) == sizeof(value));
      std::memcpy(&bits, &value, sizeof(bits));
      integer(bits, sizeof(bits));
    };
  integer(camera.width, 4);
  integer(camera.height, 4);
  for (double value : camera.k) {
    real(value);
  }
  integer(camera.d.size(), 8);
  for (double value : camera.d) {
    real(value);
  }
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned length{};
  if (EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha256(), nullptr) != 1) {
    throw std::runtime_error("calibration SHA256 failed");
  }
  std::ostringstream encoded;
  encoded << std::hex << std::setfill('0');
  for (unsigned i = 0; i < length; ++i) {
    encoded << std::setw(2) << unsigned(digest[i]);
  }
  return encoded.str();
}

Pixels ImagePixels(const sensor_msgs::msg::Image::ConstSharedPtr & image)
{
  // Both the Jazzy vector field and Lyrical CPU rosidl::Buffer expose this
  // reference. A non-CPU Buffer refuses access rather than copying silently.
  const auto & data = static_cast<const std::vector<std::uint8_t> &>(image->data);
  if (data.size() < static_cast<std::uint64_t>(image->step) * image->height) {
    throw std::invalid_argument("short image data");
  }
  return {image->width, image->height, image->step, image->encoding,
    image->is_bigendian != 0, std::shared_ptr<const std::vector<std::uint8_t>>(image, &data)};
}

isaac_ros_tensor_msgs::msg::TensorList SeedMessage(
  const Pose & pose,
  const std_msgs::msg::Header & header)
{
  isaac_ros_tensor_msgs::msg::TensorList seed;
  seed.header = header;
  seed.names = {"output"};
  seed.tensors.resize(1);
  auto & tensor = seed.tensors.front();
  tensor.dtype_code = 2;
  tensor.dtype_bits = 32;
  tensor.dtype_lanes = 1;
  tensor.shape = {1, 4, 4};
  tensor.byte_offset = 0;
  tensor.data.resize(sizeof(float) * pose.matrix.size());
  std::memcpy(tensor.data.data(), pose.matrix.data(), tensor.data.size());
  return seed;
}

}  // namespace

Nanoseconds SourceTime(const builtin_interfaces::msg::Time & stamp)
{
  if (stamp.nanosec >= kSecond) {throw std::invalid_argument("noncanonical timestamp");}
  return static_cast<Nanoseconds>(stamp.sec) * kSecond + stamp.nanosec;
}

builtin_interfaces::msg::Time RosTime(Nanoseconds stamp)
{
  auto seconds = stamp / kSecond;
  auto remainder = stamp % kSecond;
  if (remainder < 0) {--seconds; remainder += kSecond;}
  if (seconds < std::numeric_limits<std::int32_t>::min() ||
    seconds > std::numeric_limits<std::int32_t>::max())
  {
    throw std::invalid_argument("timestamp outside ROS Time");
  }
  builtin_interfaces::msg::Time value;
  value.sec = static_cast<std::int32_t>(seconds);
  value.nanosec = static_cast<std::uint32_t>(remainder);
  return value;
}

Request FromGoal(const EstimateInstances::Goal & goal)
{
  Request request;
  request.request_id = goal.request_id;
  request.attempt = goal.attempt;
  request.model_id = goal.model_id;
  request.model_revision = goal.model_revision;
  request.execution_id = goal.execution_id;
  request.capture_mode = static_cast<CaptureMode>(goal.capture_mode);
  request.settle_frames = goal.settle_frames;
  request.max_capture_age_ns = Duration(goal.max_capture_age);
  request.work_budget_ns = Duration(goal.work_budget);
  request.max_instances = goal.max_instances;
  request.min_detection_score = goal.min_detection_score;
  request.order = static_cast<Order>(goal.order);
  request.region.kind = static_cast<RegionKind>(goal.region.kind);
  request.region.image_box = {goal.region.image_box[0], goal.region.image_box[1],
    goal.region.image_box[2], goal.region.image_box[3]};
  request.region.frame_id = goal.region.frame_id;
  request.region.minimum = {goal.region.minimum.x, goal.region.minimum.y, goal.region.minimum.z};
  request.region.maximum = {goal.region.maximum.x, goal.region.maximum.y, goal.region.maximum.z};
  request.region_membership = static_cast<RegionMembership>(goal.region_membership);
  request.reuse_policy = static_cast<ReusePolicy>(goal.reuse_policy);
  request.viewpoint_id = goal.viewpoint_id;
  request.preempt = goal.preempt;
  return request;
}

EstimateInstances::Result ToResult(const Result & result)
{
  EstimateInstances::Result message;
  message.request_id = result.request_id;
  message.attempt = result.attempt;
  message.provider_session = result.provider_session;
  message.outcome = static_cast<std::uint8_t>(result.outcome);
  message.reason = result.reason;
  message.capture = CaptureHeader(result.capture);
  if (result.capture) {
    message.capture_epoch = result.capture->epoch;
    message.calibration_id = result.capture->calibration_id;
  }
  message.detections_total = static_cast<std::uint32_t>(result.detections.size());
  for (const auto & detection : result.detections) {
    message.detections.push_back(DetectionMessage(detection));
  }
  for (const auto & instance : result.instances) {
    message.instances.push_back(InstanceMessage(instance, result.capture));
  }
  message.timing.capture_wait_ms = result.timing.capture_wait_ms;
  message.timing.detect_ms = result.timing.detect_ms;
  message.timing.estimated = result.timing.estimated;
  message.timing.refined = result.timing.refined;
  message.timing.reused = result.timing.reused;
  message.timing.work_ms = result.timing.work_ms;
  message.timing.grace_ms = result.timing.grace_ms;
  message.timing.total_ms = result.timing.total_ms;
  message.timing.late_outputs = result.timing.late_outputs;
  return message;
}

EstimateInstances::Feedback ToFeedback(const Feedback & feedback)
{
  EstimateInstances::Feedback message;
  const auto & observation = feedback.observation;
  message.request_id = observation.request_id;
  message.attempt = observation.attempt;
  message.provider_session = observation.provider_session;
  message.capture = CaptureHeader(observation.capture);
  if (observation.capture) {
    message.capture_epoch = observation.capture->epoch;
    message.calibration_id = observation.capture->calibration_id;
  }
  message.kind = feedback.detections ? message.KIND_DETECTIONS : message.KIND_INSTANCE;
  for (const auto & detection : observation.detections) {
    message.detections.push_back(DetectionMessage(detection));
  }
  message.selected_total = feedback.selected_total;
  message.finished_count = feedback.finished_count;
  if (feedback.instance) {
    message.instance = InstanceMessage(*feedback.instance, observation.capture);
  }
  return message;
}

Capture FromCapture(
  sensor_msgs::msg::Image::ConstSharedPtr rgb,
  sensor_msgs::msg::Image::ConstSharedPtr depth,
  sensor_msgs::msg::CameraInfo::ConstSharedPtr camera)
{
  if (!rgb || !depth || !camera || rgb->header.frame_id.empty() ||
    rgb->header.frame_id != depth->header.frame_id ||
    rgb->header.frame_id != camera->header.frame_id ||
    SourceTime(rgb->header.stamp) != SourceTime(depth->header.stamp) ||
    SourceTime(rgb->header.stamp) != SourceTime(camera->header.stamp) ||
    rgb->width != depth->width || rgb->height != depth->height ||
    rgb->width != camera->width || rgb->height != camera->height || !rgb->width || !rgb->height ||
    (rgb->encoding != "rgb8" && rgb->encoding != "bgr8") || depth->encoding != "32FC1" ||
    rgb->step < static_cast<std::uint64_t>(rgb->width) * 3 ||
    depth->step < static_cast<std::uint64_t>(depth->width) * sizeof(float))
  {
    throw std::invalid_argument("incoherent or unsupported RGB-D capture");
  }
  auto owners = std::make_shared<CaptureMessages>(CaptureMessages{rgb, depth, camera});
  Capture capture;
  capture.source_stamp_ns = SourceTime(rgb->header.stamp);
  capture.frame_id = rgb->header.frame_id;
  capture.calibration_id = CalibrationId(*camera);
  capture.rgb = ImagePixels(rgb);
  capture.depth = ImagePixels(depth);
  capture.intrinsics = camera->k;
  capture.source_metadata = std::move(owners);
  return capture;
}

std::vector<Detection> FromDetections(
  const vision_msgs::msg::Detection2DArray & detections, const Capture & capture)
{
  std::vector<Detection> result;
  for (std::size_t i = 0; i < detections.detections.size(); ++i) {
    const auto & detection = detections.detections[i];
    if (detection.results.empty() || detection.bbox.center.theta != 0) {
      throw std::invalid_argument("unsupported native detection");
    }
    auto score = -std::numeric_limits<double>::infinity();
    for (const auto & hypothesis : detection.results) {
      const auto value = hypothesis.hypothesis.score;
      if (!std::isfinite(value) || value < 0 || value > 1) {
        throw std::invalid_argument("invalid native detection score");
      }
      score = std::max(score, value);
    }
    const auto & box = detection.bbox;
    const auto source = SourceBox({box.center.position.x - box.size_x / 2,
          box.center.position.y - box.size_y / 2, box.size_x, box.size_y},
      capture.rgb.width, capture.rgb.height);
    if (source.width <= 0 || source.height <= 0) {continue;}
    if (i > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("too many detections");
    }
    result.push_back({static_cast<std::uint32_t>(i), score, source, 0, false});
  }
  return result;
}

NativeInputs MakeNativeInputs(const Admission & admission, std::uint32_t mask_margin_px)
{
  if (!admission.capture || !admission.capture->source_metadata) {
    throw std::invalid_argument("capture lacks source messages");
  }
  const auto owners =
    std::static_pointer_cast<const CaptureMessages>(admission.capture->source_metadata);
  NativeInputs inputs;
  inputs.rgb = *owners->rgb;
  inputs.camera = *owners->camera;
  inputs.rgb.header.stamp = inputs.camera.header.stamp = RosTime(admission.processing_stamp_ns);
  if (admission.work != NativeWork::Detect) {
    inputs.depth = *owners->depth;
    inputs.depth->header.stamp = inputs.rgb.header.stamp;
    if (admission.work == NativeWork::Estimate) {
      if (!admission.detection) {throw std::invalid_argument("estimate lacks detection");}
      sensor_msgs::msg::Image mask;
      mask.header = inputs.rgb.header;
      mask.width = inputs.rgb.width;
      mask.height = inputs.rgb.height;
      mask.step = mask.width;
      mask.encoding = "mono8";
      mask.data = Mask(admission.detection->box, mask.width, mask.height, mask_margin_px);
      inputs.mask = std::move(mask);
    } else {
      if (!admission.seed) {throw std::invalid_argument("refine lacks exact prior seed");}
      inputs.seed = SeedMessage(*admission.seed, inputs.rgb.header);
    }
  }
  return inputs;
}

Pose FromNativePose(
  const vision_msgs::msg::Detection3DArray & pose,
  const isaac_ros_tensor_msgs::msg::TensorList & matrix,
  Nanoseconds expected_processing_stamp, const std::string & optical_frame)
{
  if (SourceTime(pose.header.stamp) != expected_processing_stamp ||
    SourceTime(matrix.header.stamp) != expected_processing_stamp ||
    pose.header.frame_id != optical_frame || matrix.header.frame_id != optical_frame ||
    pose.detections.size() != 1 || pose.detections.front().results.size() != 1 ||
    matrix.names != std::vector<std::string>{"output"} || matrix.tensors.size() != 1)
  {
    throw std::invalid_argument("native pose/matrix pair differs from current admission");
  }
  const auto & tensor = matrix.tensors.front();
  if (tensor.dtype_code != 2 || tensor.dtype_bits != 32 || tensor.dtype_lanes != 1 ||
    tensor.shape != std::vector<std::int64_t>{1, 4, 4} || !tensor.strides.empty() ||
    tensor.byte_offset != 0 || tensor.data.size() != 16 * sizeof(float))
  {
    throw std::invalid_argument("unsupported centered pose matrix layout");
  }
  const auto & public_pose = pose.detections.front().results.front().pose.pose;
  Pose value;
  value.position = {public_pose.position.x, public_pose.position.y, public_pose.position.z};
  value.orientation = {public_pose.orientation.x, public_pose.orientation.y,
    public_pose.orientation.z, public_pose.orientation.w};
  std::memcpy(value.matrix.data(), tensor.data.data(), tensor.data.size());
  return value;
}

}  // namespace manyforge::perception
