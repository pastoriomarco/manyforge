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

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <deque>
#include <functional>

namespace manyforge::perception
{

using Nanoseconds = std::int64_t;

enum class CaptureMode : std::uint8_t { Next, Latest };
enum class Order : std::uint8_t { Score, RegionCenter, TopToBottom };
enum class ReusePolicy : std::uint8_t { None, Refine, ReuseAndRefine };
enum class RegionKind : std::uint8_t { None, ImageBox, SpaceBox };
enum class RegionMembership : std::uint8_t { Whole, Centre };
enum class Outcome : std::uint8_t { Complete, ValidEmpty, Partial, Cancelled, Failed };
enum class InstanceStatus : std::uint8_t { Posed, Failed, NotAttempted };
enum class Method : std::uint8_t { Estimated, Refined, Reused };
enum class NativeWork : std::uint8_t { Detect, Estimate, Refine };

struct Box
{
  double x{}, y{}, width{}, height{};
};

struct Region
{
  RegionKind kind{RegionKind::None};
  Box image_box;
  std::string frame_id;
  std::array<double, 3> minimum{}, maximum{};
};

struct Request
{
  std::string request_id;
  std::uint32_t attempt{};
  std::string model_id, model_revision, execution_id;
  CaptureMode capture_mode{CaptureMode::Next};
  std::uint32_t settle_frames{};
  Nanoseconds max_capture_age_ns{};
  Nanoseconds work_budget_ns{5'000'000'000};
  std::uint32_t max_instances{5};
  double min_detection_score{};
  Order order{Order::Score};
  Region region;
  RegionMembership region_membership{RegionMembership::Whole};
  ReusePolicy reuse_policy{ReusePolicy::Refine};
  std::string viewpoint_id;
  bool preempt{};
};

// Shared raw image storage is retained without conversion while idle.
struct Pixels
{
  std::uint32_t width{}, height{}, step{};
  std::string encoding;
  bool big_endian{};
  std::shared_ptr<const std::vector<std::uint8_t>> data;
};

struct Capture
{
  Nanoseconds source_stamp_ns{}, arrival_ns{};
  std::string frame_id, calibration_id;
  std::uint32_t epoch{};
  Pixels rgb, depth;
  std::array<double, 9> intrinsics{};
  // Immutable source-message ownership; interpreted only by the ROS translator.
  std::shared_ptr<const void> source_metadata;
};

struct Detection
{
  std::uint32_t index{};
  double score{};
  Box box;
  // Current capture depth used for TOP_TO_BOTTOM, not a confidence score.
  double depth_m{};
  bool selected{};
};

struct Pose
{
  std::array<double, 3> position{};
  std::array<double, 4> orientation{0, 0, 0, 1};
  // Exact native seed matrix, column/row interpretation is translation-owned.
  std::array<float, 16> matrix{};
};

struct Instance
{
  std::uint32_t index{};
  InstanceStatus status{InstanceStatus::NotAttempted};
  Method method{Method::Estimated};
  std::string reason;
  Pose pose;
  double estimate_ms{};
};

struct Timing
{
  double capture_wait_ms{}, detect_ms{}, work_ms{}, grace_ms{}, total_ms{};
  std::uint32_t estimated{}, refined{}, reused{};
  std::uint64_t late_outputs{};
};

struct Result
{
  std::string request_id;
  std::uint32_t attempt{};
  std::string provider_session;
  Outcome outcome{Outcome::Failed};
  std::string reason;
  std::shared_ptr<const Capture> capture;
  std::vector<Detection> detections;
  std::vector<Instance> instances;
  Timing timing;
};

struct Admission
{
  NativeWork work{NativeWork::Detect};
  Nanoseconds processing_stamp_ns{};
  std::shared_ptr<const Capture> capture;
  std::optional<Detection> detection;
  std::optional<Pose> seed;
};

struct Feedback
{
  bool detections{};
  Result observation;
  std::uint32_t selected_total{}, finished_count{};
  std::optional<Instance> instance;
};

// One direct update, not an event bus or a retained job registry.
struct Update
{
  bool accepted{};
  std::string rejection;
  std::optional<Admission> admission;
  std::vector<Feedback> feedback;
  // At most two: a preempted result followed by immediate new-goal failure.
  std::vector<Result> results;
};

struct Crop
{
  std::vector<float> depth;
  std::vector<std::array<float, 3>> colour;
};

struct GateLimits
{
  double unchanged_tol_m{std::numeric_limits<double>::quiet_NaN()};
  double unchanged_max_fraction{std::numeric_limits<double>::quiet_NaN()};
  double small_change_max_fraction{std::numeric_limits<double>::quiet_NaN()};
  double small_change_max_m{std::numeric_limits<double>::quiet_NaN()};
  double colour_tol{std::numeric_limits<double>::quiet_NaN()};
  double colour_unchanged_max_fraction{std::numeric_limits<double>::quiet_NaN()};
  double colour_small_change_max_fraction{std::numeric_limits<double>::quiet_NaN()};
};

enum class Change { Unchanged, Small, Large, NoComparablePixels };

struct MatchLimits
{
  double iou{std::numeric_limits<double>::quiet_NaN()};
  double center_px{std::numeric_limits<double>::quiet_NaN()};
  double size_ratio{std::numeric_limits<double>::quiet_NaN()};
};

struct Prior
{
  Box box;
  Pose pose;
  Crop crop;
  std::uint32_t reuse_count{};
  Nanoseconds arrival_ns{};
};

struct RefinementLimits
{
  double translation_m{std::numeric_limits<double>::quiet_NaN()};
  double rotation_rad{std::numeric_limits<double>::quiet_NaN()};
  std::array<bool, 3> continuous_symmetry_axes{};
  std::vector<std::array<double, 4>> discrete_symmetries;
};

struct WarmSettings
{
  bool prior_age_enabled{true};
  Nanoseconds prior_max_age_ns{30'000'000'000};
  std::uint64_t cache_byte_budget{32 * 1024 * 1024};
  // Required measured settings, not production defaults.
  std::uint32_t crop_side{}, max_reuse{};
  MatchLimits matching;
  GateLimits gate;
  RefinementLimits refinement;
};

struct Config
{
  std::string model_id, model_revision, provider_session;
  std::uint32_t buffer_frames{2};
  std::uint32_t mask_margin_px{};
  std::optional<std::uint32_t> settle_frames;
  std::optional<double> duplicate_iou;
  Nanoseconds completion_grace_ns{1'500'000'000};
  // Optional only for full-estimation fixtures. The production node must
  // supply all warm settings to support the default REFINE request policy.
  std::optional<WarmSettings> warm;
};

double IntersectionOverUnion(const Box & a, const Box & b);
Box SourceBox(const Box & letterboxed, std::uint32_t width, std::uint32_t height);
std::vector<std::uint8_t> Mask(
  const Box & box, std::uint32_t width, std::uint32_t height, std::uint32_t margin);
std::vector<std::optional<std::size_t>> MatchPriors(
  const std::vector<Detection> & detections, const std::vector<Prior> & priors,
  const MatchLimits & limits);
Change CompareCrops(const Crop & current, const Crop & prior, const GateLimits & limits);
bool AcceptRefinement(const Pose & refined, const Pose & prior, const RefinementLimits & limits);
double DepthAt(const Capture & capture, std::uint32_t x, std::uint32_t y);
bool InsideRegion(
  const Detection & detection, const Capture & capture, const Region & region,
  RegionMembership membership, std::uint32_t mask_margin_px = 0);
Crop SampleCrop(const Capture & capture, const Box & box, std::uint32_t side);

// Calls are serialized by the wrapper. All deadlines use injected monotonic
// nanoseconds; source timestamps are used only for capture epoch/correlation.
class ProviderModel {
public:
  explicit ProviderModel(
    Config config, Nanoseconds first_processing_stamp = 1,
    std::function<Nanoseconds()> monotonic_now = {});
  std::optional<std::string> Validate(const Request & request) const;
  std::optional<std::string> CheckAcceptance(const Request & request, Nanoseconds now) const;
  // now governs current transitions/capture freshness; an earlier origin is
  // only the new goal's budget start after delayed framework handle delivery.
  Update Accept(
    Request request, Nanoseconds now,
    std::optional<Nanoseconds> budget_origin = std::nullopt);
  Update ReceiveCapture(Capture capture, Nanoseconds now);
  Update ReceiveDetections(
    Nanoseconds processing_stamp, std::vector<Detection> detections, Nanoseconds now);
  Update ReceivePose(Nanoseconds processing_stamp, std::optional<Pose> pose, Nanoseconds now);
  // Called after payload preparation, immediately before native publication.
  // accepted=true confirms the same admission; otherwise process the update.
  Update ConfirmAdmission(Nanoseconds processing_stamp, Nanoseconds now);
  Update FailNative(
    Nanoseconds processing_stamp, std::string reason, bool publication_started, Nanoseconds now);
  Update DiscardNativeOutput(Nanoseconds now);
  Update Tick(Nanoseconds now);
  Update Cancel(Nanoseconds now);
  bool busy() const {return job_.has_value();}
  std::optional<Nanoseconds> awaited_stamp() const
  {return job_ ? job_->awaited_stamp : std::nullopt;}
  std::optional<Nanoseconds> next_deadline() const;
  std::uint64_t cached_bytes() const;
  std::uint64_t cache_capacity_bytes() const;

private:
  enum class Stage { Capture, Detect, Pose };
  struct Job
  {
    Request request;
    Stage stage{Stage::Capture};
    Nanoseconds accepted_ns{}, work_deadline{}, final_deadline{}, admitted_ns{};
    std::uint32_t skipped{};
    std::optional<Nanoseconds> awaited_stamp;
    std::size_t next_instance{};
    NativeWork native_work{NativeWork::Detect};
    std::vector<std::optional<Prior>> matched_priors;
    std::vector<Crop> crops;
    std::vector<Method> routes;
    std::vector<std::size_t> instance_order;
    Result result;
  };
  Nanoseconds AllocateStamp();
  void AdmitCapture(std::shared_ptr<const Capture> capture, Nanoseconds now, Update & update);
  void NextEstimate(Nanoseconds now, Update & update);
  void PrepareWarmRoutes(Nanoseconds now);
  void StoreCache(Nanoseconds now);
  void ClearCache();
  Feedback FeedbackFor(bool detections, std::optional<Instance> instance = std::nullopt) const;
  void Finish(Outcome outcome, std::string reason, Nanoseconds now, Update & update);
  Config config_;
  std::function<Nanoseconds()> monotonic_now_;
  Nanoseconds next_stamp_;
  bool stamps_exhausted_{};
  std::optional<Job> job_;
  std::deque<std::shared_ptr<const Capture>> frames_;
  std::optional<Nanoseconds> last_source_stamp_;
  std::string calibration_id_;
  std::uint32_t capture_epoch_{};
  struct CachedViewpoint
  {
    std::string viewpoint_id, calibration_id, model_revision, execution_id;
    std::uint32_t epoch{};
    Nanoseconds last_used_ns{};
    std::vector<Prior> priors;
  };
  static std::uint64_t CacheEntryBytes(const CachedViewpoint & entry);
  std::vector<CachedViewpoint> cache_;
  std::optional<std::string> execution_id_;
};

// The ROS goal decision can outlive a failed goal-response send without an
// accepted callback. Retain one decision, never a model job without a handle.
// Only the wrapper's actual accepted-handle callback may call Commit.
class PendingGoalAdmission {
public:
  using GoalId = std::array<std::uint8_t, 16>;
  explicit PendingGoalAdmission(ProviderModel & model)
  : model_(model) {}
  std::optional<std::string> Prepare(Request request, GoalId id, Nanoseconds now);
  Update Commit(const GoalId & id, Nanoseconds now);

private:
  struct Decision
  {
    Request request;
    GoalId id;
    Nanoseconds accepted_ns;
  };
  ProviderModel & model_;
  std::optional<Decision> pending_;
};

}  // namespace manyforge::perception
