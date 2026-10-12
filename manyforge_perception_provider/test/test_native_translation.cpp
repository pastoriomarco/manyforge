// Copyright 2026 ManyForge contributors
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
#include <gtest/gtest.h>
#include <cstring>
#include <limits>
#include <stdexcept>
#include "manyforge_perception_provider/native_translation.hpp"

namespace manyforge::perception
{
namespace
{

// No accelerator allocation or transfer. Exercise the real received Buffer
// reference-conversion refusal with an explicit non-CPU fixture backend.
class FixtureNonCpuBuffer : public rosidl::BufferImplBase<std::uint8_t> {
public:
  explicit FixtureNonCpuBuffer(std::shared_ptr<unsigned> copies)
  : copies_(std::move(copies)) {}
  std::string get_backend_type() const override {return "fixture_non_cpu";}
  std::size_t size() const override {return 24;}
  std::unique_ptr<rosidl::BufferImplBase<std::uint8_t>> to_cpu() const override
  {
    ++*copies_;
    throw std::runtime_error("fixture implicit CPU transfer forbidden");
  }
  std::unique_ptr<rosidl::BufferImplBase<std::uint8_t>> clone() const override
  {return std::make_unique<FixtureNonCpuBuffer>(copies_);}

private:
  std::shared_ptr<unsigned> copies_;
};

Capture FixtureNativeCapture()
{
  auto rgb = std::make_shared<sensor_msgs::msg::Image>();
  rgb->header.stamp = RosTime(42);
  rgb->header.frame_id = "camera_optical";
  rgb->width = 4;
  rgb->height = 2;
  rgb->step = 12;
  rgb->encoding = "rgb8";
  rgb->data.resize(24, 128);
  auto depth = std::make_shared<sensor_msgs::msg::Image>(*rgb);
  depth->step = 16;
  depth->encoding = "32FC1";
  depth->data.resize(32);
  auto camera = std::make_shared<sensor_msgs::msg::CameraInfo>();
  camera->header = rgb->header;
  camera->width = rgb->width;
  camera->height = rgb->height;
  camera->k = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  return FromCapture(rgb, depth, camera);
}

TEST(NativeTranslation, ExactSourceTimeIncludingNegativeAndLongUptime)
{
  for (Nanoseconds value : {Nanoseconds(-1), Nanoseconds(0),
      Nanoseconds(31LL * 365 * 24 * 60 * 60 * 1'000'000'000)})
  {
    EXPECT_EQ(SourceTime(RosTime(value)), value);
  }
  auto invalid = RosTime(0);
  invalid.nanosec = 1'000'000'000;
  EXPECT_THROW(SourceTime(invalid), std::invalid_argument);
  EXPECT_THROW(RosTime(std::numeric_limits<Nanoseconds>::max()), std::invalid_argument);
}

TEST(NativeTranslation, NonCpuSourceRefusesWithoutImplicitTransfer)
{
  const auto capture = std::make_shared<Capture>(FixtureNativeCapture());
  Admission admission{NativeWork::Estimate, 42, capture,
    Detection{0, 1, {0, 0, 1, 1}, 1, true}, std::nullopt};
  auto packet = MakeNativeInputs(admission, 0);
  auto rgb = std::make_shared<sensor_msgs::msg::Image>(std::move(packet.rgb));
  auto depth = std::make_shared<sensor_msgs::msg::Image>(std::move(*packet.depth));
  auto camera = std::make_shared<sensor_msgs::msg::CameraInfo>(std::move(packet.camera));
  auto copies = std::make_shared<unsigned>(0);
  rgb->data = decltype(rgb->data)(std::make_unique<FixtureNonCpuBuffer>(copies));
  EXPECT_THROW(FromCapture(rgb, depth, camera), std::runtime_error);
  EXPECT_EQ(*copies, 0u);
}

TEST(NativeTranslation, CaptureOwnersAndCalibrationExcludeProcessingStamp)
{
  auto capture = FixtureNativeCapture();
  const auto bytes = capture.rgb.data;
  ASSERT_TRUE(bytes);
  EXPECT_EQ(bytes->size(), 24u);
  EXPECT_EQ(capture.calibration_id.size(), 64u);
  EXPECT_EQ(capture.calibration_id, FixtureNativeCapture().calibration_id);
  Admission admission;
  admission.capture = std::make_shared<Capture>(capture);
  admission.processing_stamp_ns = 100;
  admission.work = NativeWork::Estimate;
  admission.detection = Detection{0, 1, {1, 0, 1, 1}, 0, true};
  auto inputs = MakeNativeInputs(admission, 0);
  EXPECT_EQ(SourceTime(inputs.rgb.header.stamp), 100);
  EXPECT_EQ(SourceTime(inputs.camera.header.stamp), 100);
  ASSERT_TRUE(inputs.depth);
  ASSERT_TRUE(inputs.mask);
  EXPECT_EQ(SourceTime(inputs.depth->header.stamp), 100);
  EXPECT_EQ(SourceTime(inputs.mask->header.stamp), 100);
  EXPECT_EQ(capture.source_stamp_ns, 42);
  EXPECT_EQ(inputs.mask->encoding, "mono8");
  EXPECT_EQ(inputs.mask->data[1], 255);
  EXPECT_EQ(inputs.mask->data[2], 0);
  EXPECT_EQ(capture.rgb.data, bytes);
}

TEST(NativeTranslation, PublicOriginalPoseAndCenteredSeedArePreservedSeparately)
{
  auto capture = std::make_shared<Capture>(FixtureNativeCapture());
  Pose prior;
  prior.position = {1, 2, 3};
  prior.matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 9, 8, 7, 1};
  Admission admission{NativeWork::Refine, 99, capture, std::nullopt, prior};
  auto inputs = MakeNativeInputs(admission, 0);
  ASSERT_TRUE(inputs.seed);
  vision_msgs::msg::Detection3DArray output;
  output.header = inputs.rgb.header;
  output.detections.resize(1);
  output.detections.front().results.resize(1);
  auto & public_pose = output.detections.front().results.front().pose.pose;
  public_pose.position.x = 1;
  public_pose.position.y = 2;
  public_pose.position.z = 3;
  public_pose.orientation.w = 1;
  const auto translated = FromNativePose(output, *inputs.seed, 99, "camera_optical");
  EXPECT_EQ(translated.position, prior.position);
  EXPECT_EQ(translated.matrix, prior.matrix);
  Result result;
  result.capture = capture;
  result.instances = {Instance{0, InstanceStatus::Posed, Method::Refined, "", translated, 1}};
  const auto message = ToResult(result);
  EXPECT_EQ(SourceTime(message.capture.stamp), 42);
  EXPECT_EQ(SourceTime(message.instances.front().pose.header.stamp), 42);
  EXPECT_EQ(message.instances.front().pose.pose.position.x, 1);
  EXPECT_THROW(FromNativePose(output, *inputs.seed, 100, "camera_optical"), std::invalid_argument);
  inputs.seed->tensors.front().byte_offset = 4;
  EXPECT_THROW(FromNativePose(output, *inputs.seed, 99, "camera_optical"), std::invalid_argument);
}

TEST(NativeTranslation, DetectionUsesSourceLetterboxAndMalformedCannotLookEmpty)
{
  auto capture = FixtureNativeCapture();
  capture.rgb.width = capture.depth.width = 1280;
  capture.rgb.height = capture.depth.height = 720;
  vision_msgs::msg::Detection2DArray native;
  native.detections.resize(1);
  auto & detection = native.detections.front();
  detection.bbox.center.position.x = 320;
  detection.bbox.center.position.y = 320;
  detection.bbox.size_x = 100;
  detection.bbox.size_y = 100;
  detection.results.resize(1);
  detection.results.front().hypothesis.score = 0.9;
  const auto result = FromDetections(native, capture);
  ASSERT_EQ(result.size(), 1u);
  EXPECT_EQ(result.front().box.x, 540);
  EXPECT_EQ(result.front().box.y, 260);
  detection.results.front().hypothesis.score = std::numeric_limits<double>::quiet_NaN();
  EXPECT_THROW(FromDetections(native, capture), std::invalid_argument);
  native.detections.clear();
  EXPECT_TRUE(FromDetections(native, capture).empty());
}

TEST(NativeTranslation, GoalFieldsAndTerminalOutcomeUseApprovedPayload)
{
  EstimateInstances::Goal goal;
  goal.request_id = "request";
  goal.execution_id = "run";
  goal.work_budget.sec = 5;
  goal.max_capture_age.nanosec = 100;
  goal.region_membership = goal.REGION_CENTRE;
  goal.reuse_policy = goal.REUSE_AND_REFINE;
  goal.preempt = true;
  const auto request = FromGoal(goal);
  EXPECT_EQ(request.execution_id, "run");
  EXPECT_EQ(request.work_budget_ns, 5'000'000'000);
  EXPECT_EQ(request.region_membership, RegionMembership::Centre);
  EXPECT_EQ(request.reuse_policy, ReusePolicy::ReuseAndRefine);
  EXPECT_TRUE(request.preempt);
  Result result;
  result.outcome = Outcome::Cancelled;
  EXPECT_EQ(ToResult(result).outcome, EstimateInstances::Result::CANCELLED);
  goal.work_budget.nanosec = 1'000'000'000;
  EXPECT_THROW(FromGoal(goal), std::invalid_argument);
}

}  // namespace
}  // namespace manyforge::perception
