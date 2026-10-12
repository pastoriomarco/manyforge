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
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include "manyforge_perception_provider/provider_model.hpp"

namespace manyforge::perception
{
namespace
{

Config FixtureConfig()
{
  Config config;
  config.model_id = "fixture";
  config.model_revision = "fixture-revision";
  config.provider_session = "fixture-session";
  config.settle_frames = 0;
  config.duplicate_iou = 0.8;
  return config;
}

Request FixtureRequest(std::string id = "request")
{
  Request request;
  request.request_id = std::move(id);
  request.model_id = "fixture";
  request.model_revision = "fixture-revision";
  request.reuse_policy = ReusePolicy::None;
  request.work_budget_ns = 1'000'000'000;
  return request;
}

Capture FixtureCapture(Nanoseconds source = 42)
{
  Capture capture;
  capture.source_stamp_ns = source;
  capture.frame_id = "camera_optical";
  capture.calibration_id = "fixture-calibration";
  capture.intrinsics = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  auto rgb = std::make_shared<std::vector<std::uint8_t>>(4 * 2 * 3, 128);
  capture.rgb = Pixels{4, 2, 12, "rgb8", false, rgb};
  auto depth = std::make_shared<std::vector<std::uint8_t>>(4 * 2 * sizeof(float));
  const float metres = 1.0;
  for (std::size_t offset = 0; offset < depth->size(); offset += sizeof(float)) {
    std::memcpy(depth->data() + offset, &metres, sizeof(metres));
  }
  const std::uint16_t endian_test = 1;
  const bool big_endian = reinterpret_cast<const std::uint8_t *>(&endian_test)[0] == 0;
  capture.depth = Pixels{4, 2, 16, "32FC1", big_endian, depth};
  return capture;
}

Pose FixturePose()
{
  Pose pose;
  pose.matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  return pose;
}

Config FixtureWarmConfig()
{
  auto config = FixtureConfig();
  WarmSettings settings;
  settings.crop_side = 2;
  settings.max_reuse = 2;
  settings.matching = MatchLimits{0.8, 1, 0.1};
  settings.gate = GateLimits{0.01, 0.0, 0.5, 0.1, 0.1, 0.0, 0.5};
  settings.refinement = RefinementLimits{0.05, 0.2, {true, false, false}, {}};
  config.warm = settings;
  return config;
}

Update FixtureOnePose(ProviderModel & model, Request request, Nanoseconds begin)
{
  EXPECT_TRUE(model.Accept(std::move(request), begin).accepted);
  auto detect = model.ReceiveCapture(FixtureCapture(), begin + 1).admission;
  EXPECT_TRUE(detect);
  if (!detect) {return {};}
  auto pose = model.ReceiveDetections(detect->processing_stamp_ns,
      {{0, 1, {0, 0, 1, 1}, 1, false}}, begin + 2).admission;
  EXPECT_TRUE(pose);
  if (!pose) {return {};}
  return model.ReceivePose(pose->processing_stamp_ns, FixturePose(), begin + 3);
}

TEST(ProviderModel, NextUsesArrivalAndKeepsRealSourceTime)
{
  ProviderModel model(FixtureConfig());
  model.ReceiveCapture(FixtureCapture(900), 5);
  EXPECT_TRUE(model.Accept(FixtureRequest(), 10).accepted);
  EXPECT_FALSE(model.ReceiveCapture(FixtureCapture(900), 10).admission);
  auto detect = model.ReceiveCapture(FixtureCapture(800), 11);
  ASSERT_TRUE(detect.admission);
  EXPECT_EQ(detect.admission->capture->source_stamp_ns, 800);
  EXPECT_EQ(detect.admission->capture->epoch, 1u);
  EXPECT_NE(detect.admission->processing_stamp_ns, 800);
  auto empty = model.ReceiveDetections(detect.admission->processing_stamp_ns, {}, 12);
  ASSERT_EQ(empty.results.size(), 1u);
  EXPECT_EQ(empty.results[0].outcome, Outcome::ValidEmpty);
  EXPECT_EQ(empty.results[0].capture->source_stamp_ns, 800);
}

TEST(ProviderModel, ImmediateNextRepeatedFrameCannotConsumeCancelledOutput)
{
  ProviderModel model(FixtureConfig());
  model.ReceiveCapture(FixtureCapture(), 0);
  auto first = FixtureRequest("first");
  first.capture_mode = CaptureMode::Latest;
  first.max_capture_age_ns = 100;
  const auto old = model.Accept(first, 1).admission;
  ASSERT_TRUE(old);
  ASSERT_EQ(model.Cancel(2).results.size(), 1u);
  auto second = first;
  second.request_id = "second";
  auto current = model.Accept(second, 3).admission;
  ASSERT_TRUE(current);
  EXPECT_NE(current->processing_stamp_ns, old->processing_stamp_ns);
  EXPECT_TRUE(model.ReceiveDetections(old->processing_stamp_ns, {}, 4).results.empty());
  auto result = model.ReceiveDetections(current->processing_stamp_ns, {}, 5);
  ASSERT_EQ(result.results.size(), 1u);
  EXPECT_EQ(result.results[0].request_id, "second");
  EXPECT_EQ(result.results[0].timing.late_outputs, 1u);
}

TEST(ProviderModel, GraceFinishesInFlightWithoutStartingNext)
{
  ProviderModel model(FixtureConfig());
  model.Accept(FixtureRequest(), 0);
  auto detect = model.ReceiveCapture(FixtureCapture(), 1).admission;
  ASSERT_TRUE(detect);
  const std::vector<Detection> detections{
    {0, 0.9, {0, 0, 1, 1}, 1, false}, {1, 0.8, {2, 0, 1, 1}, 1, false}};
  auto pose = model.ReceiveDetections(detect->processing_stamp_ns, detections, 2).admission;
  ASSERT_TRUE(pose);
  EXPECT_TRUE(model.Tick(1'000'000'000).results.empty());
  auto completed = model.ReceivePose(pose->processing_stamp_ns, FixturePose(), 1'100'000'000);
  EXPECT_FALSE(completed.admission);
  ASSERT_EQ(completed.results.size(), 1u);
  EXPECT_EQ(completed.results[0].outcome, Outcome::Partial);
  EXPECT_EQ(completed.results[0].instances[0].status, InstanceStatus::Posed);
  EXPECT_EQ(completed.results[0].instances[1].status, InstanceStatus::NotAttempted);
  EXPECT_DOUBLE_EQ(completed.results[0].timing.work_ms, 1000);
  EXPECT_DOUBLE_EQ(completed.results[0].timing.grace_ms, 100);
}

TEST(ProviderModel, BudgetExpiryNoCaptureAndGraceExpiryAllowImmediateNext)
{
  ProviderModel model(FixtureConfig());
  model.Accept(FixtureRequest(), 0);
  auto failed = model.Tick(1'000'000'000);
  ASSERT_EQ(failed.results.size(), 1u);
  EXPECT_EQ(failed.results[0].reason, "no_fresh_capture");
  EXPECT_TRUE(model.Accept(FixtureRequest("next"), 1'000'000'000).accepted);
  const auto detect = model.ReceiveCapture(FixtureCapture(), 1'000'000'001).admission;
  ASSERT_TRUE(detect);
  auto timed_out = model.Tick(3'500'000'000);
  ASSERT_EQ(timed_out.results.size(), 1u);
  EXPECT_EQ(timed_out.results[0].reason, "detection_timeout");
  EXPECT_TRUE(model.Accept(FixtureRequest("third"), 3'500'000'000).accepted);
  EXPECT_TRUE(model.ReceiveDetections(detect->processing_stamp_ns, {},
        3'500'000'001).results.empty());
}

TEST(ProviderModel, AcceptEnforcesOverdueDeadlinesBeforeCheckingBusy)
{
  ProviderModel model(FixtureConfig());
  model.Accept(FixtureRequest("old-capture"), 0);
  auto next = model.Accept(FixtureRequest("next"), 1'000'000'000);
  EXPECT_TRUE(next.accepted);
  ASSERT_EQ(next.results.size(), 1u);
  EXPECT_EQ(next.results[0].request_id, "old-capture");
  EXPECT_EQ(next.results[0].reason, "no_fresh_capture");
  auto detect = model.ReceiveCapture(FixtureCapture(), 1'000'000'001).admission;
  ASSERT_TRUE(detect);
  auto after_detect_timeout = model.Accept(FixtureRequest("after-detect"), 3'500'000'000);
  EXPECT_TRUE(after_detect_timeout.accepted);
  ASSERT_EQ(after_detect_timeout.results.size(), 1u);
  EXPECT_EQ(after_detect_timeout.results[0].reason, "detection_timeout");
  detect = model.ReceiveCapture(FixtureCapture(), 3'500'000'001).admission;
  ASSERT_TRUE(detect);
  auto pose = model.ReceiveDetections(detect->processing_stamp_ns,
      {{0, 1, {0, 0, 1, 1}, 1, false}}, 3'500'000'002).admission;
  ASSERT_TRUE(pose);
  auto after_pose_timeout = model.Accept(FixtureRequest("after-pose"), 6'000'000'000);
  EXPECT_TRUE(after_pose_timeout.accepted);
  ASSERT_EQ(after_pose_timeout.results.size(), 1u);
  EXPECT_EQ(after_pose_timeout.results[0].instances[0].reason, "completion_grace_exhausted");
}

TEST(ProviderModel, LatestEmptyOrStaleBufferCanSelectANewArrival)
{
  for (const bool stale : {false, true}) {
    ProviderModel model(FixtureConfig());
    if (stale) {model.ReceiveCapture(FixtureCapture(1), 0);}
    auto request = FixtureRequest();
    request.capture_mode = CaptureMode::Latest;
    request.max_capture_age_ns = 5;
    auto accepted = model.Accept(request, 10);
    EXPECT_TRUE(accepted.accepted);
    EXPECT_FALSE(accepted.admission);
    auto received = model.ReceiveCapture(FixtureCapture(2), 11);
    ASSERT_TRUE(received.admission);
    EXPECT_EQ(received.admission->capture->source_stamp_ns, 2);
    EXPECT_EQ(received.admission->work, NativeWork::Detect);
  }
}

TEST(ProviderModel, ZeroCapHasNoCountLimitAndDetectionsFeedbackIsFirst)
{
  ProviderModel model(FixtureConfig());
  auto request = FixtureRequest();
  request.max_instances = 0;
  model.Accept(request, 0);
  auto detect = model.ReceiveCapture(FixtureCapture(), 1).admission;
  ASSERT_TRUE(detect);
  auto update = model.ReceiveDetections(detect->processing_stamp_ns,
      {{4, 0.8, {2, 0, 1, 1}, 1, false}, {3, 0.9, {0, 0, 1, 1}, 1, false}}, 2);
  ASSERT_EQ(update.feedback.size(), 1u);
  EXPECT_TRUE(update.feedback[0].detections);
  EXPECT_EQ(update.feedback[0].selected_total, 2u);
  ASSERT_TRUE(update.admission);
  EXPECT_EQ(update.admission->detection->index, 3u);
}

TEST(ProviderModel, MalformedDetectorOutputIsFailureNotAuthoritativeEmpty)
{
  ProviderModel model(FixtureConfig());
  model.Accept(FixtureRequest(), 0);
  auto detect = model.ReceiveCapture(FixtureCapture(), 1).admission;
  ASSERT_TRUE(detect);
  auto failed = model.ReceiveDetections(detect->processing_stamp_ns,
      {{0, std::numeric_limits<double>::quiet_NaN(), {0, 0, 1, 1}, 1, false}}, 2);
  ASSERT_EQ(failed.results.size(), 1u);
  EXPECT_EQ(failed.results[0].outcome, Outcome::Failed);
  EXPECT_EQ(failed.results[0].reason, "invalid_native_detection");
  EXPECT_FALSE(failed.admission);
}

TEST(ProviderModel, ExactLongUptimeAndDeadlineOverflow)
{
  ProviderModel model(FixtureConfig());
  constexpr auto uptime = Nanoseconds(31) * 365 * 24 * 60 * 60 * 1'000'000'000;
  EXPECT_TRUE(model.Accept(FixtureRequest(), uptime).accepted);
  EXPECT_TRUE(model.Tick(uptime + 999'999'999).results.empty());
  EXPECT_EQ(model.Tick(uptime + 1'000'000'000).results.size(), 1u);
  EXPECT_THROW(model.Accept(FixtureRequest(), std::numeric_limits<Nanoseconds>::max() - 10),
    std::invalid_argument);
}

TEST(ProviderModel, ProcessingStampExhaustionNeverWraps)
{
  constexpr auto max_stamp = Nanoseconds(std::numeric_limits<std::int32_t>::max()) *
    1'000'000'000 + 999'999'999;
  ProviderModel model(FixtureConfig(), max_stamp);
  model.Accept(FixtureRequest(), 0);
  auto admission = model.ReceiveCapture(FixtureCapture(), 1).admission;
  ASSERT_TRUE(admission);
  EXPECT_EQ(admission->processing_stamp_ns, max_stamp);
  model.Cancel(2);
  model.Accept(FixtureRequest(), 3);
  EXPECT_THROW(model.ReceiveCapture(FixtureCapture(), 4), std::overflow_error);
}

TEST(ProviderGeometry, SourceLetterboxMaskAndWholeVersusCentre)
{
  const auto box = SourceBox({100, 140, 10, 10}, 1280, 720);
  EXPECT_DOUBLE_EQ(box.x, 200);
  EXPECT_DOUBLE_EQ(box.y, 0);
  EXPECT_DOUBLE_EQ(box.width, 20);
  EXPECT_DOUBLE_EQ(box.height, 20);
  const auto mask = Mask({-1, 0, 3, 1}, 4, 2, 0);
  EXPECT_EQ(mask, (std::vector<std::uint8_t>{255, 255, 0, 0, 0, 0, 0, 0}));
  auto capture = FixtureCapture();
  Detection detection{0, 1, {0, 0, 3, 1}, 1, false};
  Region region;
  region.kind = RegionKind::SpaceBox;
  region.frame_id = capture.frame_id;
  region.minimum = {1, -1, 0};
  region.maximum = {2, 1, 2};
  EXPECT_FALSE(InsideRegion(detection, capture, region, RegionMembership::Whole));
  EXPECT_TRUE(InsideRegion(detection, capture, region, RegionMembership::Centre));
  auto empty = std::make_shared<std::vector<std::uint8_t>>(capture.depth.data->size(), 0);
  capture.depth.data = empty;
  EXPECT_FALSE(InsideRegion(detection, capture, region, RegionMembership::Whole));
  EXPECT_FALSE(InsideRegion(detection, capture, region, RegionMembership::Centre));
}

TEST(ProviderGeometry, NonfinitePrincipalPointCannotPassRegionMembership)
{
  auto capture = FixtureCapture();
  capture.intrinsics[2] = std::numeric_limits<double>::quiet_NaN();
  ProviderModel model(FixtureConfig());
  EXPECT_THROW(model.ReceiveCapture(capture, 0), std::invalid_argument);
  Region region;
  region.kind = RegionKind::SpaceBox;
  region.frame_id = capture.frame_id;
  region.minimum = {-10, -10, -10};
  region.maximum = {10, 10, 10};
  Detection detection{0, 1, {0, 0, 1, 1}, 1, false};
  EXPECT_THROW(InsideRegion(detection, capture, region, RegionMembership::Whole),
    std::invalid_argument);
  EXPECT_THROW(InsideRegion(detection, capture, region, RegionMembership::Centre),
    std::invalid_argument);
}

TEST(ProviderWarmOperations, MissingDepthAndAppearanceChangesNeverReuse)
{
  const GateLimits fixture_limits{0.01, 0.0, 0.5, 0.1, 0.1, 0.0, 0.5};
  Crop prior{{1, 1}, {{{0.5f, 0.5f, 0.5f}}, {{0.5f, 0.5f, 0.5f}}}};
  EXPECT_EQ(CompareCrops(prior, prior, fixture_limits), Change::Unchanged);
  auto colour_flip = prior;
  colour_flip.colour[0][0] = 1;
  colour_flip.colour[1][0] = 1;
  EXPECT_EQ(CompareCrops(colour_flip, prior, fixture_limits), Change::Large);
  auto missing = prior;
  missing.depth = {0, std::numeric_limits<float>::quiet_NaN()};
  EXPECT_EQ(CompareCrops(missing, prior, fixture_limits), Change::NoComparablePixels);
  auto nudge = prior;
  nudge.depth[0] += 0.02f;
  EXPECT_EQ(CompareCrops(nudge, prior, fixture_limits), Change::Small);
}

TEST(ProviderWarmOperations, MatchingIsOneToOneAndRefineChecksOwnPriorSymmetry)
{
  Prior prior;
  prior.box = {0, 0, 1, 1};
  prior.pose = FixturePose();
  const auto matches = MatchPriors({{0, 1, prior.box, 1, true}, {1, 1, prior.box, 1, true}},
      {prior}, MatchLimits{0.8, 1, 0.1});
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0], 0u);
  EXPECT_FALSE(matches[1]);
  RefinementLimits fixture_limits{0.01, 0.1, {true, false, false}, {}};
  auto refined = prior.pose;
  refined.orientation = {1, 0, 0, 0};
  EXPECT_TRUE(AcceptRefinement(refined, prior.pose, fixture_limits));
  fixture_limits.continuous_symmetry_axes = {};
  EXPECT_FALSE(AcceptRefinement(refined, prior.pose, fixture_limits));
  refined = prior.pose;
  refined.position[0] = 0.02;
  EXPECT_FALSE(AcceptRefinement(refined, prior.pose, fixture_limits));
}

TEST(ProviderWarmRoutes, RejectedRefinementStartsFullWithNewStampBeforeNextInstance)
{
  ProviderModel model(FixtureWarmConfig());
  auto request = FixtureRequest();
  request.reuse_policy = ReusePolicy::Refine;
  auto first = FixtureOnePose(model, request, 0);
  ASSERT_EQ(first.results.size(), 1u);
  EXPECT_EQ(first.results[0].instances[0].method, Method::Estimated);
  model.Accept(request, 10);
  auto detect = model.ReceiveCapture(FixtureCapture(), 11).admission;
  ASSERT_TRUE(detect);
  auto refined = model.ReceiveDetections(detect->processing_stamp_ns,
      {{0, 1, {0, 0, 1, 1}, 1, false}}, 12).admission;
  ASSERT_TRUE(refined);
  EXPECT_EQ(refined->work, NativeWork::Refine);
  ASSERT_TRUE(refined->seed);
  EXPECT_EQ(refined->seed->position, FixturePose().position);
  auto rejected = FixturePose();
  rejected.position[0] = 1;
  auto fallback = model.ReceivePose(refined->processing_stamp_ns, rejected, 13);
  ASSERT_TRUE(fallback.admission);
  EXPECT_EQ(fallback.admission->work, NativeWork::Estimate);
  EXPECT_FALSE(fallback.admission->seed);
  EXPECT_NE(fallback.admission->processing_stamp_ns, refined->processing_stamp_ns);
  auto result = model.ReceivePose(fallback.admission->processing_stamp_ns, FixturePose(), 14);
  ASSERT_EQ(result.results.size(), 1u);
  EXPECT_EQ(result.results[0].instances[0].status, InstanceStatus::Posed);
  EXPECT_EQ(result.results[0].instances[0].method, Method::Estimated);
  EXPECT_TRUE(result.results[0].instances[0].reason.empty());
}

TEST(ProviderWarmRoutes, AgeOffUsesExecutionIdentityAndMissingIdentityUsesSession)
{
  auto config = FixtureWarmConfig();
  config.warm->prior_age_enabled = false;
  ProviderModel model(config);
  auto request = FixtureRequest();
  request.reuse_policy = ReusePolicy::Refine;
  request.execution_id = "run-A";
  ASSERT_EQ(FixtureOnePose(model, request, 0).results.size(), 1u);
  auto within_execution = FixtureOnePose(model, request, 100'000'000'000);
  ASSERT_EQ(within_execution.results.size(), 1u);
  EXPECT_EQ(within_execution.results[0].instances[0].method, Method::Refined);
  request.execution_id = "run-B";
  auto changed = FixtureOnePose(model, request, 200'000'000'000);
  ASSERT_EQ(changed.results.size(), 1u);
  EXPECT_EQ(changed.results[0].instances[0].method, Method::Estimated);
  request.execution_id.clear();
  auto absent = FixtureOnePose(model, request, 300'000'000'000);
  ASSERT_EQ(absent.results.size(), 1u);
  EXPECT_EQ(absent.results[0].instances[0].method, Method::Estimated);
  auto same_session = FixtureOnePose(model, request, 400'000'000'000);
  ASSERT_EQ(same_session.results.size(), 1u);
  EXPECT_EQ(same_session.results[0].instances[0].method, Method::Refined);
}

TEST(ProviderWarmRoutes, OversizeEntryDoesNotChangeResultAndLruEvictsWholeViewpoint)
{
  auto request = FixtureRequest();
  request.reuse_policy = ReusePolicy::Refine;
  request.viewpoint_id = "A";
  ProviderModel sizing_model(FixtureWarmConfig());
  ASSERT_EQ(FixtureOnePose(sizing_model, request, 0).results.size(), 1u);
  const auto one_entry_bytes = sizing_model.cached_bytes();
  ASSERT_GT(one_entry_bytes, 0u);
  auto config = FixtureWarmConfig();
  config.warm->cache_byte_budget = one_entry_bytes;
  ProviderModel model(config);
  auto result = FixtureOnePose(model, request, 0);
  ASSERT_EQ(result.results.size(), 1u);
  EXPECT_EQ(result.results[0].outcome, Outcome::Complete);
  EXPECT_EQ(model.cached_bytes(), one_entry_bytes);
  request.viewpoint_id = "B";
  ASSERT_EQ(FixtureOnePose(model, request, 10).results.size(), 1u);
  EXPECT_LE(model.cached_bytes(), one_entry_bytes);
  request.viewpoint_id = "A";
  auto evicted = FixtureOnePose(model, request, 20);
  ASSERT_EQ(evicted.results.size(), 1u);
  EXPECT_EQ(evicted.results[0].instances[0].method, Method::Estimated);
  config.warm->cache_byte_budget = 0;
  ProviderModel no_fit(config);
  auto uncached = FixtureOnePose(no_fit, request, 0);
  ASSERT_EQ(uncached.results.size(), 1u);
  EXPECT_EQ(uncached.results[0].outcome, Outcome::Complete);
  EXPECT_EQ(no_fit.cached_bytes(), 0u);
  auto again = FixtureOnePose(no_fit, request, 10);
  ASSERT_EQ(again.results.size(), 1u);
  EXPECT_EQ(again.results[0].instances[0].method, Method::Estimated);
}

TEST(ProviderWarmRoutes, CurrentFrameEpochResetDropsPriorsAndReuseCountForcesFull)
{
  auto config = FixtureWarmConfig();
  config.warm->max_reuse = 1;
  ProviderModel model(config);
  auto request = FixtureRequest();
  request.reuse_policy = ReusePolicy::Refine;
  ASSERT_EQ(FixtureOnePose(model, request, 0).results.size(), 1u);
  auto reused = FixtureOnePose(model, request, 10);
  ASSERT_EQ(reused.results.size(), 1u);
  EXPECT_EQ(reused.results[0].instances[0].method, Method::Refined);
  auto refresh = FixtureOnePose(model, request, 20);
  ASSERT_EQ(refresh.results.size(), 1u);
  EXPECT_EQ(refresh.results[0].instances[0].method, Method::Estimated);
  model.ReceiveCapture(FixtureCapture(0), 30);
  EXPECT_EQ(model.cached_bytes(), 0u);
}

TEST(ProviderWarmRoutes, CpuGateWorkCannotAdmitNativeWorkAfterDeadline)
{
  Nanoseconds clock = 0;
  ProviderModel model(FixtureWarmConfig(), 1, [&] {return clock;});
  model.Accept(FixtureRequest(), 0);
  auto detect = model.ReceiveCapture(FixtureCapture(), 1).admission;
  ASSERT_TRUE(detect);
  clock = 1'000'000'000;
  auto result = model.ReceiveDetections(detect->processing_stamp_ns,
      {{0, 1, {0, 0, 1, 1}, 1, false}}, 2);
  EXPECT_FALSE(result.admission);
  ASSERT_EQ(result.results.size(), 1u);
  EXPECT_EQ(result.results[0].outcome, Outcome::Partial);
  EXPECT_EQ(result.results[0].instances[0].status, InstanceStatus::NotAttempted);
}

TEST(ProviderWarmRoutes, PriorAgeUsesFreshClockAtEligibilityAndAdmission)
{
  for (const auto policy : {ReusePolicy::Refine, ReusePolicy::ReuseAndRefine}) {
    auto config = FixtureWarmConfig();
    Nanoseconds clock = 3;
    ProviderModel model(config, 1, [&] {return clock;});
    auto request = FixtureRequest();
    request.reuse_policy = policy;
    ASSERT_EQ(FixtureOnePose(model, request, 0).results.size(), 1u);
    model.Accept(request, 29'998'000'000);
    auto detect = model.ReceiveCapture(FixtureCapture(), 29'998'000'001).admission;
    ASSERT_TRUE(detect);
    clock = 30'001'000'000;
    auto next = model.ReceiveDetections(detect->processing_stamp_ns,
        {{0, 1, {0, 0, 1, 1}, 1, false}}, 29'999'000'000);
    ASSERT_TRUE(next.admission);
    EXPECT_EQ(next.admission->work, NativeWork::Estimate);
    EXPECT_EQ(next.feedback.size(), 1u);
  }
}

TEST(ProviderWarmRoutes, EnabledAgeAcceptsExactBoundaryAndRejectsBeyond)
{
  for (const Nanoseconds extra : {Nanoseconds(0), Nanoseconds(1)}) {
    ProviderModel model(FixtureWarmConfig());
    auto request = FixtureRequest();
    request.reuse_policy = ReusePolicy::Refine;
    ASSERT_EQ(FixtureOnePose(model, request, 0).results.size(), 1u);
    const auto detection_time = Nanoseconds(30'000'000'001) + extra;
    model.Accept(request, detection_time - 2);
    auto detect = model.ReceiveCapture(FixtureCapture(), detection_time - 1).admission;
    ASSERT_TRUE(detect);
    auto next = model.ReceiveDetections(detect->processing_stamp_ns,
        {{0, 1, {0, 0, 1, 1}, 1, false}}, detection_time);
    ASSERT_TRUE(next.admission);
    EXPECT_EQ(next.admission->work, extra ? NativeWork::Estimate : NativeWork::Refine);
  }
}

TEST(ProviderWarmRoutes, ReusedFeedbackPrecedesFullButAuthoritativeOrderIsPreserved)
{
  ProviderModel model(FixtureWarmConfig());
  auto request = FixtureRequest();
  request.reuse_policy = ReusePolicy::ReuseAndRefine;
  const std::vector<Detection> detections{
    {0, 0.9, {0, 0, 1, 1}, 1, false}, {1, 0.8, {2, 0, 1, 1}, 1, false}};
  model.Accept(request, 0);
  auto detect = model.ReceiveCapture(FixtureCapture(), 1).admission;
  ASSERT_TRUE(detect);
  auto first_pose = model.ReceiveDetections(detect->processing_stamp_ns, detections, 2).admission;
  ASSERT_TRUE(first_pose);
  auto second_pose = model.ReceivePose(first_pose->processing_stamp_ns, FixturePose(), 3).admission;
  ASSERT_TRUE(second_pose);
  ASSERT_EQ(model.ReceivePose(second_pose->processing_stamp_ns, FixturePose(), 4).results.size(),
        1u);
  model.Accept(request, 10);
  auto capture = FixtureCapture();
  auto rgb = std::make_shared<std::vector<std::uint8_t>>(*capture.rgb.data);
  (*rgb)[0] = 255;
  capture.rgb.data = rgb;
  detect = model.ReceiveCapture(capture, 11).admission;
  ASSERT_TRUE(detect);
  auto update = model.ReceiveDetections(detect->processing_stamp_ns, detections, 12);
  ASSERT_EQ(update.feedback.size(), 2u);
  EXPECT_TRUE(update.feedback[0].detections);
  ASSERT_TRUE(update.feedback[1].instance);
  EXPECT_EQ(update.feedback[1].instance->index, 1u);
  EXPECT_EQ(update.feedback[1].instance->method, Method::Reused);
  ASSERT_TRUE(update.admission);
  EXPECT_EQ(update.admission->detection->index, 0u);
  EXPECT_EQ(update.admission->work, NativeWork::Estimate);
  auto result = model.ReceivePose(update.admission->processing_stamp_ns, FixturePose(), 13);
  ASSERT_EQ(result.results.size(), 1u);
  EXPECT_EQ(result.results[0].instances[0].index, 0u);
  EXPECT_EQ(result.results[0].instances[1].index, 1u);
}

TEST(ProviderWarmRoutes, RejectedRefinementAtCutoffIsFailedAndNotPosed)
{
  ProviderModel model(FixtureWarmConfig());
  auto request = FixtureRequest();
  request.reuse_policy = ReusePolicy::Refine;
  ASSERT_EQ(FixtureOnePose(model, request, 0).results.size(), 1u);
  model.Accept(request, 10);
  auto detect = model.ReceiveCapture(FixtureCapture(), 11).admission;
  ASSERT_TRUE(detect);
  auto refine = model.ReceiveDetections(detect->processing_stamp_ns,
      {{0, 1, {0, 0, 1, 1}, 1, false}}, 12).admission;
  ASSERT_TRUE(refine);
  auto rejected = FixturePose();
  rejected.position[0] = 1;
  auto result = model.ReceivePose(refine->processing_stamp_ns, rejected, 1'000'000'010);
  EXPECT_FALSE(result.admission);
  ASSERT_EQ(result.results.size(), 1u);
  EXPECT_EQ(result.results[0].outcome, Outcome::Partial);
  EXPECT_EQ(result.results[0].instances[0].status, InstanceStatus::Failed);
  EXPECT_EQ(result.results[0].instances[0].reason, "refinement_rejected");
}

TEST(ProviderWarmRoutes, CacheInvalidationReleasesOuterCapacityThenRefillsWithinBudget)
{
  auto config = FixtureWarmConfig();
  config.warm->prior_age_enabled = false;
  auto request = FixtureRequest();
  request.reuse_policy = ReusePolicy::Refine;
  request.execution_id = "execution-A";
  ProviderModel sizing_model(config);
  for (std::uint32_t i = 0; i < 5; ++i) {
    request.viewpoint_id = std::to_string(i);
    ASSERT_EQ(FixtureOnePose(sizing_model, request, i * 10).results.size(), 1u);
  }
  config.warm->cache_byte_budget = sizing_model.cached_bytes();
  ProviderModel model(config);
  for (std::uint32_t i = 0; i < 5; ++i) {
    request.viewpoint_id = std::to_string(i);
    ASSERT_EQ(FixtureOnePose(model, request, i * 10).results.size(), 1u);
  }
  ASSERT_GT(model.cache_capacity_bytes(), 0u);
  EXPECT_LE(model.cached_bytes(), config.warm->cache_byte_budget);
  EXPECT_EQ(model.cached_bytes(), sizing_model.cached_bytes());
  request.execution_id = "execution-B";
  EXPECT_TRUE(model.Accept(request, 100).accepted);
  EXPECT_EQ(model.cache_capacity_bytes(), 0u);
  EXPECT_EQ(model.cached_bytes(), 0u);
  model.Cancel(101);
  ASSERT_EQ(FixtureOnePose(model, request, 110).results.size(), 1u);
  EXPECT_LE(model.cached_bytes(), config.warm->cache_byte_budget);
  model.ReceiveCapture(FixtureCapture(0), 120);
  EXPECT_EQ(model.cache_capacity_bytes(), 0u);
  EXPECT_EQ(model.cached_bytes(), 0u);
  ASSERT_EQ(FixtureOnePose(model, request, 130).results.size(), 1u);
  EXPECT_LE(model.cached_bytes(), config.warm->cache_byte_budget);
}

TEST(ProviderGeometry, SpaceRegionCenterOrderingUsesMetresNotImageMidpoint)
{
  ProviderModel model(FixtureConfig());
  auto request = FixtureRequest();
  request.order = Order::RegionCenter;
  request.region.kind = RegionKind::SpaceBox;
  request.region.frame_id = "camera_optical";
  request.region.minimum = {-1, -1, 0};
  request.region.maximum = {2, 2, 2};
  request.region_membership = RegionMembership::Centre;
  model.Accept(request, 0);
  auto detect = model.ReceiveCapture(FixtureCapture(), 1).admission;
  ASSERT_TRUE(detect);
  auto update = model.ReceiveDetections(detect->processing_stamp_ns,
      {{0, 0.9, {1, 0, 1, 1}, 1, false}, {1, 0.8, {0, 0, 1, 1}, 1, false}}, 2);
  ASSERT_TRUE(update.admission);
  EXPECT_EQ(update.admission->detection->index, 1u);
}

TEST(ProviderModel, PayloadPreparationCannotPublishAfterWorkCutoff)
{
  for (const Nanoseconds finish : {Nanoseconds(1'000'000'000), Nanoseconds(3'000'000'000)}) {
    for (bool after_detections : {false, true}) {
      ProviderModel model(FixtureConfig());
      model.Accept(FixtureRequest(), 0);
      auto admission = model.ReceiveCapture(FixtureCapture(), 1).admission;
      ASSERT_TRUE(admission);
      if (after_detections) {
        admission = model.ReceiveDetections(admission->processing_stamp_ns,
            {{0, 1, {0, 0, 1, 1}, 1, false}}, 2).admission;
        ASSERT_TRUE(admission);
      }
      auto update = model.ConfirmAdmission(admission->processing_stamp_ns, finish);
      EXPECT_FALSE(update.accepted);
      ASSERT_EQ(update.results.size(), 1u);
      EXPECT_EQ(update.results.front().outcome,
            after_detections ? Outcome::Partial : Outcome::Failed);
      if (after_detections) {
        EXPECT_EQ(update.results.front().instances.front().status, InstanceStatus::NotAttempted);
      }
      EXPECT_TRUE(model.Accept(FixtureRequest("next"), finish + 1).accepted);
    }
  }
}

TEST(ProviderWarmRoutes, PayloadPreparationRechecksRefinementAgeWithNewFullStamp)
{
  ProviderModel model(FixtureWarmConfig());
  auto request = FixtureRequest();
  request.reuse_policy = ReusePolicy::Refine;
  ASSERT_EQ(FixtureOnePose(model, request, 0).results.size(), 1u);
  model.Accept(request, 29'999'999'990);
  auto detect = model.ReceiveCapture(FixtureCapture(), 29'999'999'991).admission;
  ASSERT_TRUE(detect);
  auto refine = model.ReceiveDetections(detect->processing_stamp_ns,
      {{0, 1, {0, 0, 1, 1}, 1, false}}, 29'999'999'992).admission;
  ASSERT_TRUE(refine);
  EXPECT_EQ(refine->work, NativeWork::Refine);
  auto update = model.ConfirmAdmission(refine->processing_stamp_ns, 30'000'000'002);
  EXPECT_FALSE(update.accepted);
  ASSERT_TRUE(update.admission);
  EXPECT_EQ(update.admission->work, NativeWork::Estimate);
  EXPECT_NE(update.admission->processing_stamp_ns, refine->processing_stamp_ns);
  EXPECT_FALSE(model.ConfirmAdmission(refine->processing_stamp_ns, 30'000'000'003).accepted);
  EXPECT_TRUE(model.ConfirmAdmission(update.admission->processing_stamp_ns,
        30'000'000'004).accepted);
}

TEST(ProviderWarmRoutes, LongIdEvictionAndReplacementCountActualSurvivorCapacities)
{
  auto config = FixtureWarmConfig();
  ProviderModel sizing(config);
  auto request = FixtureRequest();
  request.viewpoint_id = std::string(4096, 'L');
  ASSERT_EQ(FixtureOnePose(sizing, request, 0).results.size(), 1u);
  request.viewpoint_id = "short";
  ASSERT_EQ(FixtureOnePose(sizing, request, 10).results.size(), 1u);
  config.warm->cache_byte_budget = sizing.cached_bytes();
  for (bool replace_long : {false, true}) {
    ProviderModel model(config);
    request.viewpoint_id = std::string(4096, 'L');
    ASSERT_EQ(FixtureOnePose(model, request, 0).results.size(), 1u);
    request.viewpoint_id = "short";
    ASSERT_EQ(FixtureOnePose(model, request, 10).results.size(), 1u);
    request.viewpoint_id = replace_long ? std::string(4096, 'L') : "new";
    ASSERT_EQ(FixtureOnePose(model, request, 20).results.size(), 1u);
    EXPECT_LE(model.cached_bytes(), config.warm->cache_byte_budget);
    request.viewpoint_id = "short";
    request.reuse_policy = ReusePolicy::Refine;
    ASSERT_TRUE(model.Accept(request, 30).accepted);
    auto detect = model.ReceiveCapture(FixtureCapture(), 31).admission;
    ASSERT_TRUE(detect);
    auto pose = model.ReceiveDetections(detect->processing_stamp_ns,
        {{0, 1, {0, 0, 1, 1}, 1, false}}, 32).admission;
    ASSERT_TRUE(pose);
    // libstdc++ retains the old long-ID destination heap on SSO assignment.
    // Its actual capacity forces eviction of the short survivor too. The old
    // ledger undercount retained that prior and incorrectly selected REFINE.
    EXPECT_EQ(pose->work, NativeWork::Estimate);
    request.reuse_policy = ReusePolicy::None;
  }
}

TEST(ProviderModel, NativeFaultReportsAttemptedFactWithoutCrossStampFailure)
{
  for (bool publication_started : {false, true}) {
    ProviderModel model(FixtureConfig());
    model.Accept(FixtureRequest(), 0);
    auto detect = model.ReceiveCapture(FixtureCapture(), 1).admission;
    ASSERT_TRUE(detect);
    auto pose = model.ReceiveDetections(detect->processing_stamp_ns,
        {{0, 1, {0, 0, 1, 1}, 1, false}}, 2).admission;
    ASSERT_TRUE(pose);
    EXPECT_TRUE(model.FailNative(detect->processing_stamp_ns, "old_fault", true,
          3).results.empty());
    auto failed = model.FailNative(pose->processing_stamp_ns, "native_publication_failed",
      publication_started, 4);
    ASSERT_EQ(failed.results.size(), 1u);
    EXPECT_EQ(failed.results.front().outcome, Outcome::Partial);
    EXPECT_EQ(failed.results.front().instances.front().status,
      publication_started ? InstanceStatus::Failed : InstanceStatus::NotAttempted);
    EXPECT_TRUE(model.Accept(FixtureRequest("next"), 5).accepted);
  }
}

TEST(ActionAdmission, MissingHandleCannotPreemptOrCreateAnOrphanJob)
{
  ProviderModel model(FixtureConfig());
  PendingGoalAdmission admission(model);
  PendingGoalAdmission::GoalId abandoned{}, current{};
  abandoned[0] = 1;
  current[0] = 2;
  ASSERT_TRUE(model.Accept(FixtureRequest("old"), 0).accepted);
  const auto old_detect = model.ReceiveCapture(FixtureCapture(), 1).admission;
  ASSERT_TRUE(old_detect);
  auto preempting = FixtureRequest("never-got-handle");
  preempting.preempt = true;
  ASSERT_FALSE(admission.Prepare(preempting, abandoned, 2));
  // Simulate the framework's failed response send: no Commit is called.
  // The real old output must still finish OLD, rather than a new orphan job.
  auto old_result = model.ReceiveDetections(old_detect->processing_stamp_ns, {}, 3);
  ASSERT_EQ(old_result.results.size(), 1u);
  EXPECT_EQ(old_result.results.front().request_id, "old");
  EXPECT_EQ(old_result.results.front().outcome, Outcome::ValidEmpty);
  EXPECT_FALSE(model.busy());

  ASSERT_FALSE(admission.Prepare(FixtureRequest("current"), current, 4));
  EXPECT_EQ(admission.Commit(abandoned, 5).rejection, "stale_goal_handle");
  EXPECT_FALSE(model.busy());
  EXPECT_TRUE(admission.Commit(current, 6).accepted);
  const auto detect = model.ReceiveCapture(FixtureCapture(), 7).admission;
  ASSERT_TRUE(detect);
  auto result = model.ReceiveDetections(detect->processing_stamp_ns, {}, 8);
  ASSERT_EQ(result.results.size(), 1u);
  EXPECT_EQ(result.results.front().request_id, "current");
  EXPECT_EQ(admission.Commit(current, 9).rejection, "stale_goal_handle");
}

TEST(ActionAdmission, LateRealHandlePreservesOriginalBudgetBeforeAnyPublication)
{
  ProviderModel model(FixtureConfig());
  PendingGoalAdmission admission(model);
  model.ReceiveCapture(FixtureCapture(), 0);
  auto request = FixtureRequest();
  request.capture_mode = CaptureMode::Latest;
  request.max_capture_age_ns = 10'000'000'000;
  const PendingGoalAdmission::GoalId id{};
  ASSERT_FALSE(admission.Prepare(request, id, 1));
  EXPECT_FALSE(model.busy());
  const auto committed = admission.Commit(id, 1'000'000'001);
  EXPECT_TRUE(committed.accepted);
  EXPECT_FALSE(committed.admission);
  ASSERT_EQ(committed.results.size(), 1u);
  EXPECT_EQ(committed.results.front().outcome, Outcome::Failed);
  EXPECT_EQ(committed.results.front().reason, "budget_exhausted");
  EXPECT_DOUBLE_EQ(committed.results.front().timing.total_ms, 1000);
  EXPECT_FALSE(model.awaited_stamp());
  ASSERT_FALSE(admission.Prepare(FixtureRequest("immediate-next"), id, 1'000'000'002));
  EXPECT_TRUE(admission.Commit(id, 1'000'000'003).accepted);
}

TEST(ActionAdmission, DecisionIsNotBusyAndOverdueOldResultPrecedesActualPreemption)
{
  ProviderModel model(FixtureConfig());
  PendingGoalAdmission admission(model);
  PendingGoalAdmission::GoalId first{}, second{};
  second[0] = 1;
  ASSERT_FALSE(admission.Prepare(FixtureRequest("lost-response"), first, 0));
  ASSERT_FALSE(admission.Prepare(FixtureRequest("replacement"), second, 1));
  EXPECT_TRUE(admission.Commit(second, 2).accepted);
  auto next = FixtureRequest("new");
  next.preempt = true;
  ASSERT_FALSE(admission.Prepare(next, first, 3));
  auto committed = admission.Commit(first, 1'000'000'002);
  EXPECT_TRUE(committed.accepted);
  ASSERT_EQ(committed.results.size(), 1u);
  EXPECT_EQ(committed.results.front().request_id, "replacement");
  EXPECT_EQ(committed.results.front().reason, "no_fresh_capture");
  EXPECT_TRUE(model.busy());
  EXPECT_EQ(*model.next_deadline(), 1'000'000'003);
}

TEST(ActionAdmission, HandleCommitUsesCurrentOldElapsedAndLatestFreshness)
{
  ProviderModel model(FixtureConfig());
  PendingGoalAdmission admission(model);
  ASSERT_TRUE(model.Accept(FixtureRequest("old"), 0).accepted);
  const auto detect = model.ReceiveCapture(FixtureCapture(), 1).admission;
  ASSERT_TRUE(detect);
  const auto pose = model.ReceiveDetections(detect->processing_stamp_ns,
      {{0, 1, {0, 0, 1, 1}, 1, false}}, 2).admission;
  ASSERT_TRUE(pose);
  auto request = FixtureRequest("new");
  request.preempt = true;
  request.capture_mode = CaptureMode::Latest;
  request.max_capture_age_ns = 4;
  const PendingGoalAdmission::GoalId id{};
  ASSERT_FALSE(admission.Prepare(request, id, 3));
  const auto committed = admission.Commit(id, 100);
  EXPECT_TRUE(committed.accepted);
  ASSERT_EQ(committed.results.size(), 1u);
  const auto & old = committed.results.front();
  EXPECT_EQ(old.request_id, "old");
  EXPECT_EQ(old.outcome, Outcome::Cancelled);
  EXPECT_DOUBLE_EQ(old.timing.total_ms, 100 / 1'000'000.0);
  ASSERT_EQ(old.instances.size(), 1u);
  EXPECT_DOUBLE_EQ(old.instances.front().estimate_ms, 98 / 1'000'000.0);
  // Buffered capture was fresh at decision3 but stale at actual handle100.
  EXPECT_FALSE(committed.admission);
  EXPECT_EQ(*model.next_deadline(), 1'000'000'003);
  const auto fresh = model.ReceiveCapture(FixtureCapture(), 101);
  ASSERT_TRUE(fresh.admission);
  EXPECT_EQ(fresh.admission->capture->arrival_ns, 101);
  const auto completed = model.ReceiveDetections(fresh.admission->processing_stamp_ns, {}, 102);
  ASSERT_EQ(completed.results.size(), 1u);
  EXPECT_DOUBLE_EQ(completed.results.front().timing.capture_wait_ms, 98 / 1'000'000.0);
}

TEST(ProviderOperations, PaddedMaskMembershipAndMedianUseNativeGeometry)
{
  auto capture = FixtureCapture();
  const Detection detection{0, 1, {1, 0, 1, 1}, 1, false};
  Region image;
  image.kind = RegionKind::ImageBox;
  image.image_box = detection.box;
  EXPECT_TRUE(InsideRegion(detection, capture, image, RegionMembership::Whole, 0));
  EXPECT_FALSE(InsideRegion(detection, capture, image, RegionMembership::Whole, 1));
  const auto mask = Mask(detection.box, 4, 2, 1);
  EXPECT_EQ(std::count(mask.begin(), mask.end(), std::uint8_t(255)), 6);

  auto depth = std::make_shared<std::vector<std::uint8_t>>(*capture.depth.data);
  const float three_metres = 3;
  for (std::size_t i = 0; i < 8; ++i) {
    if (i == 1) {continue;}
    std::memcpy(depth->data() + i * sizeof(float), &three_metres, sizeof(float));
  }
  capture.depth.data = depth;
  Region space;
  space.kind = RegionKind::SpaceBox;
  space.frame_id = capture.frame_id;
  space.minimum = {-100, -100, 2.9};
  space.maximum = {100, 100, 3.1};
  EXPECT_FALSE(InsideRegion(detection, capture, space, RegionMembership::Centre, 0));
  EXPECT_TRUE(InsideRegion(detection, capture, space, RegionMembership::Centre, 1));

  auto config = FixtureConfig();
  config.mask_margin_px = 1;
  ProviderModel model(config);
  auto request = FixtureRequest();
  request.region = space;
  request.region_membership = RegionMembership::Centre;
  ASSERT_TRUE(model.Accept(request, 0).accepted);
  const auto detect = model.ReceiveCapture(capture, 1).admission;
  ASSERT_TRUE(detect);
  const auto update = model.ReceiveDetections(detect->processing_stamp_ns, {detection}, 2);
  ASSERT_EQ(update.feedback.size(), 1u);
  ASSERT_EQ(update.feedback.front().observation.detections.size(), 1u);
  EXPECT_DOUBLE_EQ(update.feedback.front().observation.detections.front().depth_m, 3);
}

}  // namespace
}  // namespace manyforge::perception
