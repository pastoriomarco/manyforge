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
#include "manyforge_perception_provider/provider_model.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <numeric>

namespace manyforge::perception
{
namespace
{

constexpr Nanoseconds kSecond = 1'000'000'000;
constexpr Nanoseconds kMaxProcessingStamp =
  static_cast<Nanoseconds>(std::numeric_limits<std::int32_t>::max()) * kSecond + kSecond - 1;

bool FiniteBox(const Box & box)
{
  return std::isfinite(box.x) && std::isfinite(box.y) && std::isfinite(box.width) &&
         std::isfinite(box.height) && box.width > 0 && box.height > 0 &&
         std::isfinite(box.x + box.width) && std::isfinite(box.y + box.height);
}

bool Fraction(double value)
{
  return std::isfinite(value) && value >= 0 && value <= 1;
}

double Milliseconds(Nanoseconds ns) {return static_cast<double>(ns) / 1'000'000;}

Nanoseconds Add(Nanoseconds now, Nanoseconds duration)
{
  if (now < 0 || duration < 0 || now > std::numeric_limits<Nanoseconds>::max() - duration) {
    throw std::invalid_argument("monotonic deadline overflow");
  }
  return now + duration;
}

double Median(std::vector<double> values)
{
  if (values.empty()) {return std::numeric_limits<double>::quiet_NaN();}
  std::sort(values.begin(), values.end());
  const auto middle = values.size() / 2;
  return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

std::array<std::uint32_t, 4> PixelBounds(
  const Box & box, std::uint32_t width, std::uint32_t height, std::uint32_t margin = 0)
{
  if (!FiniteBox(box) || width == 0 || height == 0) {
    throw std::invalid_argument("invalid mask geometry");
  }
  auto bound = [](double value, std::uint32_t extent) {
      return static_cast<std::uint32_t>(std::clamp(value, 0.0, static_cast<double>(extent)));
    };
  return {bound(std::floor(box.x) - margin, width), bound(std::floor(box.y) - margin, height),
    bound(std::ceil(box.x + box.width) + margin, width),
    bound(std::ceil(box.y + box.height) + margin, height)};
}

std::vector<double> BoxDepths(const Capture & capture, const Box & box, std::uint32_t margin = 0)
{
  const auto bounds = PixelBounds(box, capture.depth.width, capture.depth.height, margin);
  std::vector<double> depths;
  for (auto y = bounds[1]; y < bounds[3]; ++y) {
    for (auto x = bounds[0]; x < bounds[2]; ++x) {
      const auto depth = DepthAt(capture, x, y);
      if (std::isfinite(depth) && depth > 0) {depths.push_back(depth);}
    }
  }
  return depths;
}

bool ValidPose(const Pose & pose)
{
  return std::all_of(pose.position.begin(), pose.position.end(), [](double x) {
             return std::isfinite(x);
    }) && std::all_of(pose.orientation.begin(), pose.orientation.end(), [](double x) {
             return std::isfinite(x);
    }) && std::abs(std::inner_product(pose.orientation.begin(), pose.orientation.end(),
      pose.orientation.begin(), 0.0) - 1.0) < 1e-6 &&
         std::all_of(pose.matrix.begin(), pose.matrix.end(), [](float x) {
             return std::isfinite(x);
    });
}

std::array<double, 4> Multiply(
  const std::array<double, 4> & a, const std::array<double, 4> & b)
{
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

}  // namespace

double IntersectionOverUnion(const Box & a, const Box & b)
{
  if (!FiniteBox(a) || !FiniteBox(b)) {throw std::invalid_argument("invalid boxes");}
  const auto intersection = std::max(0.0, std::min(a.x + a.width, b.x + b.width) -
    std::max(a.x, b.x)) * std::max(0.0, std::min(a.y + a.height, b.y + b.height) -
    std::max(a.y, b.y));
  return intersection / (a.width * a.height + b.width * b.height - intersection);
}

Box SourceBox(const Box & letterboxed, std::uint32_t width, std::uint32_t height)
{
  if (!FiniteBox(letterboxed) || width == 0 || height == 0) {
    throw std::invalid_argument("invalid letterbox geometry");
  }
  const auto scale = static_cast<double>(std::max(width, height)) / 640.0;
  const auto pad_x = (640 - width / scale) / 2;
  const auto pad_y = (640 - height / scale) / 2;
  const auto x0 = std::clamp((letterboxed.x - pad_x) * scale, 0.0, static_cast<double>(width));
  const auto y0 = std::clamp((letterboxed.y - pad_y) * scale, 0.0, static_cast<double>(height));
  const auto x1 = std::clamp((letterboxed.x + letterboxed.width - pad_x) * scale, 0.0,
    static_cast<double>(width));
  const auto y1 = std::clamp((letterboxed.y + letterboxed.height - pad_y) * scale, 0.0,
    static_cast<double>(height));
  return {x0, y0, x1 - x0, y1 - y0};
}

std::vector<std::uint8_t> Mask(
  const Box & box, std::uint32_t width, std::uint32_t height, std::uint32_t margin)
{
  const auto bounds = PixelBounds(box, width, height, margin);
  if (height && width > std::numeric_limits<std::size_t>::max() / height) {
    throw std::invalid_argument("mask size overflow");
  }
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height, 0);
  for (auto y = bounds[1]; y < bounds[3]; ++y) {
    std::fill(pixels.begin() + static_cast<std::size_t>(y) * width + bounds[0],
      pixels.begin() + static_cast<std::size_t>(y) * width + bounds[2], 255);
  }
  return pixels;
}

double DepthAt(const Capture & capture, std::uint32_t x, std::uint32_t y)
{
  const auto & image = capture.depth;
  if (image.encoding != "32FC1" || x >= image.width || y >= image.height || !image.data ||
    image.step < static_cast<std::uint64_t>(image.width) * sizeof(float) ||
    image.data->size() < static_cast<std::uint64_t>(image.step) * image.height)
  {
    throw std::invalid_argument("unsupported or malformed depth image");
  }
  std::array<std::uint8_t, sizeof(float)> bytes;
  std::copy_n(image.data->data() + static_cast<std::size_t>(y) * image.step + x * sizeof(float),
    bytes.size(), bytes.begin());
  const std::uint16_t endian_test = 1;
  const bool host_big_endian = reinterpret_cast<const std::uint8_t *>(&endian_test)[0] == 0;
  if (host_big_endian != image.big_endian) {std::reverse(bytes.begin(), bytes.end());}
  float depth;
  std::memcpy(&depth, bytes.data(), sizeof(depth));
  return depth;
}

bool InsideRegion(
  const Detection & detection, const Capture & capture, const Region & region,
  RegionMembership membership, std::uint32_t mask_margin_px)
{
  if (region.kind == RegionKind::None) {return true;}
  if (!std::isfinite(capture.intrinsics[0]) || !std::isfinite(capture.intrinsics[2]) ||
    !std::isfinite(capture.intrinsics[4]) || !std::isfinite(capture.intrinsics[5]) ||
    capture.intrinsics[0] <= 0 || capture.intrinsics[4] <= 0)
  {
    throw std::invalid_argument("invalid projection intrinsics");
  }
  if (region.frame_id != capture.frame_id && region.kind == RegionKind::SpaceBox) {
    throw std::invalid_argument("region frame differs from capture frame");
  }
  const auto depths = BoxDepths(capture, detection.box, mask_margin_px);
  if (depths.empty()) {return false;}
  auto inside = [&](double x, double y, double z) {
      if (region.kind == RegionKind::ImageBox) {
        return x >= region.image_box.x && x < region.image_box.x + region.image_box.width &&
               y >= region.image_box.y && y < region.image_box.y + region.image_box.height;
      }
      const std::array<double, 3> point{(x - capture.intrinsics[2]) * z / capture.intrinsics[0],
        (y - capture.intrinsics[5]) * z / capture.intrinsics[4], z};
      for (std::size_t i = 0; i < point.size(); ++i) {
        if (!std::isfinite(point[i]) || point[i] < region.minimum[i] ||
          point[i] > region.maximum[i]) {return false;}
      }
      return true;
    };
  if (membership == RegionMembership::Centre) {
    return inside(detection.box.x + detection.box.width / 2,
      detection.box.y + detection.box.height / 2, Median(depths));
  }
  const auto bounds = PixelBounds(detection.box, capture.depth.width, capture.depth.height,
      mask_margin_px);
  for (auto y = bounds[1]; y < bounds[3]; ++y) {
    for (auto x = bounds[0]; x < bounds[2]; ++x) {
      const auto depth = DepthAt(capture, x, y);
      if (std::isfinite(depth) && depth > 0 && !inside(x, y, depth)) {return false;}
    }
  }
  return true;
}

Crop SampleCrop(const Capture & capture, const Box & box, std::uint32_t side)
{
  if (!side || !FiniteBox(box) || (capture.rgb.encoding != "rgb8" &&
    capture.rgb.encoding != "bgr8") || !capture.rgb.data ||
    capture.rgb.step < static_cast<std::uint64_t>(capture.rgb.width) * 3 ||
    capture.rgb.data->size() < static_cast<std::uint64_t>(capture.rgb.step) * capture.rgb.height ||
    capture.rgb.width != capture.depth.width || capture.rgb.height != capture.depth.height)
  {
    throw std::invalid_argument("invalid crop inputs");
  }
  const auto bounds = PixelBounds(box, capture.rgb.width, capture.rgb.height);
  if (bounds[0] == bounds[2] || bounds[1] == bounds[3]) {
    throw std::invalid_argument("empty crop bounds");
  }
  if (side > std::numeric_limits<std::size_t>::max() / side) {
    throw std::invalid_argument("crop size overflow");
  }
  Crop crop;
  crop.depth.reserve(static_cast<std::size_t>(side) * side);
  crop.colour.reserve(static_cast<std::size_t>(side) * side);
  for (std::uint32_t row = 0; row < side; ++row) {
    for (std::uint32_t column = 0; column < side; ++column) {
      auto x0 = bounds[0] + static_cast<std::uint32_t>(
        std::uint64_t(bounds[2] - bounds[0]) * column / side);
      auto y0 = bounds[1] + static_cast<std::uint32_t>(
        std::uint64_t(bounds[3] - bounds[1]) * row / side);
      const auto x1 = std::min(bounds[2], std::max(x0 + 1, bounds[0] +
        static_cast<std::uint32_t>(std::uint64_t(bounds[2] - bounds[0]) * (column + 1) / side)));
      const auto y1 = std::min(bounds[3], std::max(y0 + 1, bounds[1] +
        static_cast<std::uint32_t>(std::uint64_t(bounds[3] - bounds[1]) * (row + 1) / side)));
      std::vector<double> depths;
      std::array<std::vector<double>, 3> colours;
      for (auto y = y0; y < y1; ++y) {
        for (auto x = x0; x < x1; ++x) {
          const auto depth = DepthAt(capture, x, y);
          if (!std::isfinite(depth) || depth <= 0) {continue;}
          depths.push_back(depth);
          for (std::size_t channel = 0; channel < 3; ++channel) {
            const auto native_channel = capture.rgb.encoding == "rgb8" ? channel : 2 - channel;
            colours[channel].push_back((*capture.rgb.data)[std::size_t(y) * capture.rgb.step +
              std::size_t(x) * 3 + native_channel] / 255.0);
          }
        }
      }
      crop.depth.push_back(static_cast<float>(Median(std::move(depths))));
      crop.colour.push_back({static_cast<float>(Median(std::move(colours[0]))),
          static_cast<float>(Median(std::move(colours[1]))),
          static_cast<float>(Median(std::move(colours[2])))});
    }
  }
  return crop;
}

std::vector<std::optional<std::size_t>> MatchPriors(
  const std::vector<Detection> & detections, const std::vector<Prior> & priors,
  const MatchLimits & limits)
{
  if (!Fraction(limits.iou) || !std::isfinite(limits.center_px) || limits.center_px < 0 ||
    !std::isfinite(limits.size_ratio) || limits.size_ratio < 0)
  {
    throw std::invalid_argument("invalid prior matching limits");
  }
  std::vector<bool> used(priors.size());
  std::vector<std::optional<std::size_t>> matches;
  for (const auto & detection : detections) {
    std::optional<std::size_t> best;
    auto best_iou = -1.0;
    for (std::size_t i = 0; i < priors.size(); ++i) {
      if (used[i]) {continue;}
      const auto & box = priors[i].box;
      const auto iou = IntersectionOverUnion(detection.box, box);
      const auto center = std::hypot(detection.box.x + detection.box.width / 2 - box.x -
        box.width / 2, detection.box.y + detection.box.height / 2 - box.y - box.height / 2);
      const auto size = std::max(std::abs(detection.box.width / box.width - 1),
        std::abs(detection.box.height / box.height - 1));
      if (iou >= limits.iou && center <= limits.center_px && size <= limits.size_ratio &&
        iou > best_iou)
      {
        best = i;
        best_iou = iou;
      }
    }
    if (best) {used[*best] = true;}
    matches.push_back(best);
  }
  return matches;
}

Change CompareCrops(const Crop & current, const Crop & prior, const GateLimits & limits)
{
  if (current.depth.size() != prior.depth.size() || current.colour.size() != current.depth.size() ||
    prior.colour.size() != prior.depth.size())
  {
    throw std::invalid_argument("crop geometry mismatch");
  }
  if (!std::isfinite(limits.unchanged_tol_m) || limits.unchanged_tol_m < 0 ||
    !std::isfinite(limits.small_change_max_m) || limits.small_change_max_m < 0 ||
    !std::isfinite(limits.colour_tol) || limits.colour_tol < 0 ||
    !Fraction(limits.unchanged_max_fraction) || !Fraction(limits.small_change_max_fraction) ||
    !Fraction(limits.colour_unchanged_max_fraction) ||
    !Fraction(limits.colour_small_change_max_fraction) ||
    limits.unchanged_max_fraction > limits.small_change_max_fraction ||
    limits.colour_unchanged_max_fraction > limits.colour_small_change_max_fraction)
  {
    throw std::invalid_argument("invalid depth/colour gate limits");
  }
  std::size_t valid = 0, changed_colour = 0;
  std::vector<double> changed_depth;
  for (std::size_t i = 0; i < current.depth.size(); ++i) {
    if (!std::isfinite(current.depth[i]) || !std::isfinite(prior.depth[i]) ||
      current.depth[i] <= 0 || prior.depth[i] <= 0)
    {
      continue;
    }
    auto colour_difference = 0.0;
    bool valid_colour = true;
    for (std::size_t channel = 0; channel < 3; ++channel) {
      if (!Fraction(current.colour[i][channel]) || !Fraction(prior.colour[i][channel])) {
        valid_colour = false;
        break;
      }
      colour_difference = std::max(colour_difference,
        std::abs(static_cast<double>(current.colour[i][channel]) - prior.colour[i][channel]));
    }
    if (!valid_colour) {continue;}
    ++valid;
    const auto depth_difference = std::abs(static_cast<double>(current.depth[i]) - prior.depth[i]);
    if (depth_difference > limits.unchanged_tol_m) {changed_depth.push_back(depth_difference);}
    if (colour_difference > limits.colour_tol) {++changed_colour;}
  }
  if (!valid) {return Change::NoComparablePixels;}
  const auto depth_fraction = static_cast<double>(changed_depth.size()) / valid;
  const auto colour_fraction = static_cast<double>(changed_colour) / valid;
  if (depth_fraction <= limits.unchanged_max_fraction &&
    colour_fraction <= limits.colour_unchanged_max_fraction)
  {
    return Change::Unchanged;
  }
  if (depth_fraction <= limits.small_change_max_fraction &&
    colour_fraction <= limits.colour_small_change_max_fraction &&
    (changed_depth.empty() || Median(changed_depth) <= limits.small_change_max_m))
  {
    return Change::Small;
  }
  return Change::Large;
}

bool AcceptRefinement(const Pose & refined, const Pose & prior, const RefinementLimits & limits)
{
  if (!ValidPose(refined) || !ValidPose(prior) || !std::isfinite(limits.translation_m) ||
    !std::isfinite(limits.rotation_rad) || limits.translation_m < 0 || limits.rotation_rad < 0)
  {
    return false;
  }
  auto translation_squared = 0.0;
  for (std::size_t i = 0; i < 3; ++i) {
    translation_squared += std::pow(refined.position[i] - prior.position[i], 2);
  }
  if (std::sqrt(translation_squared) > limits.translation_m) {return false;}
  auto rotation = [&](const std::array<double, 4> & symmetry) {
      const auto expected = Multiply(prior.orientation, symmetry);
      auto relative = Multiply({-expected[0], -expected[1], -expected[2], expected[3]},
        refined.orientation);
      const auto axes = std::count(limits.continuous_symmetry_axes.begin(),
        limits.continuous_symmetry_axes.end(), true);
      if (axes >= 2) {return 0.0;}
      if (axes == 1) {
        const auto axis = std::find(limits.continuous_symmetry_axes.begin(),
          limits.continuous_symmetry_axes.end(), true) - limits.continuous_symmetry_axes.begin();
        return 2 * std::acos(std::clamp(std::hypot(relative[3], relative[axis]), 0.0, 1.0));
      }
      return 2 * std::acos(std::clamp(std::abs(relative[3]), 0.0, 1.0));
    };
  auto difference = rotation({0, 0, 0, 1});
  for (const auto & symmetry : limits.discrete_symmetries) {
    const auto norm = std::inner_product(symmetry.begin(), symmetry.end(), symmetry.begin(), 0.0);
    if (!std::isfinite(norm) || std::abs(norm - 1) >= 1e-6) {return false;}
    difference = std::min(difference, rotation(symmetry));
  }
  return difference <= limits.rotation_rad;
}

ProviderModel::ProviderModel(
  Config config, Nanoseconds first_processing_stamp, std::function<Nanoseconds()> monotonic_now)
: config_(std::move(config)), monotonic_now_(std::move(monotonic_now)),
  next_stamp_(first_processing_stamp)
{
  if (config_.model_id.empty() || config_.model_revision.empty() ||
    config_.provider_session.empty() ||
    config_.buffer_frames == 0 || !config_.settle_frames || !config_.duplicate_iou ||
    !Fraction(*config_.duplicate_iou) ||
    config_.completion_grace_ns < 0 || next_stamp_ <= 0 || next_stamp_ > kMaxProcessingStamp)
  {
    throw std::invalid_argument("invalid provider configuration");
  }
  if (config_.warm) {
    const auto & warm = *config_.warm;
    if (!warm.crop_side || warm.prior_max_age_ns < 0 ||
      !std::isfinite(warm.refinement.translation_m) || warm.refinement.translation_m < 0 ||
      !std::isfinite(warm.refinement.rotation_rad) || warm.refinement.rotation_rad < 0)
    {
      throw std::invalid_argument("invalid required warm settings");
    }
    MatchPriors({}, {}, warm.matching);
    CompareCrops({}, {}, warm.gate);
  }
}

std::optional<std::string> ProviderModel::Validate(const Request & request) const
{
  if (request.request_id.empty()) {return "missing_request_id";}
  if (request.model_id != config_.model_id || request.model_revision != config_.model_revision) {
    return "model_mismatch";
  }
  if (request.work_budget_ns < kSecond) {return "invalid_work_budget";}
  if (request.reuse_policy != ReusePolicy::None && !config_.warm) {return "warm_settings_missing";}
  if (!Fraction(request.min_detection_score)) {return "invalid_detection_score";}
  if (request.capture_mode > CaptureMode::Latest || request.order > Order::TopToBottom ||
    request.reuse_policy > ReusePolicy::ReuseAndRefine ||
    request.region.kind > RegionKind::SpaceBox ||
    request.region_membership > RegionMembership::Centre)
  {
    return "invalid_request_enum";
  }
  if (request.capture_mode == CaptureMode::Latest && request.max_capture_age_ns < 0) {
    return "invalid_capture_age";
  }
  if (request.region.kind == RegionKind::ImageBox && !FiniteBox(request.region.image_box)) {
    return "invalid_image_region";
  }
  if (request.region.kind == RegionKind::SpaceBox) {
    if (request.region.frame_id.empty()) {return "missing_region_frame";}
    for (std::size_t i = 0; i < 3; ++i) {
      if (!std::isfinite(request.region.minimum[i]) || !std::isfinite(request.region.maximum[i]) ||
        request.region.minimum[i] > request.region.maximum[i]) {return "invalid_space_region";}
    }
  }
  return std::nullopt;
}

Nanoseconds ProviderModel::AllocateStamp()
{
  if (stamps_exhausted_) {throw std::overflow_error("processing stamps exhausted");}
  const auto stamp = next_stamp_;
  if (next_stamp_ == kMaxProcessingStamp) {stamps_exhausted_ = true;} else {++next_stamp_;}
  return stamp;
}

std::optional<std::string> ProviderModel::CheckAcceptance(
  const Request & request, Nanoseconds now) const
{
  if (auto reason = Validate(request)) {return reason;}
  const auto deadline = Add(now, request.work_budget_ns);
  Add(deadline, config_.completion_grace_ns);
  if (job_ && now < job_->accepted_ns) {
    throw std::invalid_argument("monotonic clock moved backwards");
  }
  if (job_ && !request.preempt && now < *next_deadline()) {return "busy";}
  return std::nullopt;
}

Update ProviderModel::Accept(
  Request request, Nanoseconds now, std::optional<Nanoseconds> budget_origin)
{
  const auto accepted_ns = budget_origin.value_or(now);
  if (accepted_ns < 0 || accepted_ns > now) {
    throw std::invalid_argument("invalid goal budget origin");
  }
  // Goal callbacks may run before an overdue timer callback. Enforce the
  // old job's deadline here as well so timer scheduling cannot prolong busy.
  Update update = Tick(now);
  if (auto reason = Validate(request)) {update.rejection = *reason; return update;}
  const auto work_deadline = Add(accepted_ns, request.work_budget_ns);
  const auto final_deadline = Add(work_deadline, config_.completion_grace_ns);
  if (job_ && !request.preempt) {update.rejection = "busy"; return update;}
  if (job_) {Finish(Outcome::Cancelled, "preempted", now, update);}
  if (config_.warm && !config_.warm->prior_age_enabled &&
    execution_id_ != request.execution_id)
  {
    ClearCache();
  }
  execution_id_ = request.execution_id;
  Job job;
  job.result.request_id = request.request_id;
  job.result.attempt = request.attempt;
  job.result.provider_session = config_.provider_session;
  job.request = std::move(request);
  job.accepted_ns = accepted_ns;
  job.work_deadline = work_deadline;
  job.final_deadline = final_deadline;
  job_ = std::move(job);
  update.accepted = true;
  if (job_->request.capture_mode == CaptureMode::Latest && !frames_.empty() &&
    now >= frames_.back()->arrival_ns &&
    now - frames_.back()->arrival_ns <= job_->request.max_capture_age_ns)
  {
    AdmitCapture(frames_.back(), now, update);
  }
  return update;
}

Update ProviderModel::ReceiveCapture(Capture capture, Nanoseconds now)
{
  if (now < 0 || capture.frame_id.empty() || capture.calibration_id.empty() ||
    capture.rgb.width == 0 || capture.rgb.height == 0 ||
    capture.rgb.width != capture.depth.width || capture.rgb.height != capture.depth.height ||
    !std::all_of(capture.intrinsics.begin(), capture.intrinsics.end(), [](double value) {
      return std::isfinite(value);
    }) ||
    capture.intrinsics[0] <= 0 || capture.intrinsics[4] <= 0)
  {
    throw std::invalid_argument("invalid capture metadata");
  }
  Update update = Tick(now);
  if (last_source_stamp_ && (capture.source_stamp_ns < *last_source_stamp_ ||
    capture.calibration_id != calibration_id_))
  {
    if (capture_epoch_ == std::numeric_limits<std::uint32_t>::max()) {
      throw std::overflow_error("capture epoch exhausted");
    }
    ++capture_epoch_;
    frames_.clear();
    ClearCache();
  }
  last_source_stamp_ = capture.source_stamp_ns;
  calibration_id_ = capture.calibration_id;
  capture.epoch = capture_epoch_;
  capture.arrival_ns = now;
  auto frame = std::make_shared<const Capture>(std::move(capture));
  frames_.push_back(frame);
  while (frames_.size() > config_.buffer_frames) {frames_.pop_front();}
  if (job_ && job_->stage == Stage::Capture && now > job_->accepted_ns) {
    if (job_->request.capture_mode == CaptureMode::Latest) {
      AdmitCapture(frame, now, update);
    } else {
      const auto settle = job_->request.settle_frames ?
        job_->request.settle_frames : *config_.settle_frames;
      if (job_->skipped < settle) {++job_->skipped;} else {AdmitCapture(frame, now, update);}
    }
  }
  return update;
}

void ProviderModel::AdmitCapture(
  std::shared_ptr<const Capture> capture, Nanoseconds now, Update & update)
{
  if (job_->request.region.kind == RegionKind::SpaceBox &&
    job_->request.region.frame_id != capture->frame_id)
  {
    Finish(Outcome::Failed, "region_frame_mismatch", now, update);
    return;
  }
  job_->result.capture = std::move(capture);
  job_->result.timing.capture_wait_ms = Milliseconds(now - job_->accepted_ns);
  job_->stage = Stage::Detect;
  job_->awaited_stamp = AllocateStamp();
  job_->admitted_ns = now;
  update.admission = Admission{NativeWork::Detect, *job_->awaited_stamp, job_->result.capture,
    std::nullopt, std::nullopt};
}

Update ProviderModel::ReceiveDetections(
  Nanoseconds processing_stamp, std::vector<Detection> detections, Nanoseconds now)
{
  auto update = Tick(now);
  if (!job_) {return update;}
  if (job_->stage != Stage::Detect || job_->awaited_stamp != processing_stamp) {
    ++job_->result.timing.late_outputs;
    return update;
  }
  auto & job = *job_;
  job.result.timing.detect_ms = Milliseconds(now - job.admitted_ns);
  job.awaited_stamp.reset();
  std::vector<Detection> filtered;
  for (auto & detection : detections) {
    if (!FiniteBox(detection.box) || !Fraction(detection.score)) {
      Finish(Outcome::Failed, "invalid_native_detection", now, update);
      return update;
    }
    detection.selected = false;
    if (detection.score >= job.request.min_detection_score && InsideRegion(detection,
      *job.result.capture, job.request.region, job.request.region_membership,
        config_.mask_margin_px))
    {
      detection.depth_m = Median(BoxDepths(*job.result.capture, detection.box,
          config_.mask_margin_px));
      filtered.push_back(detection);
    }
  }
  std::stable_sort(filtered.begin(), filtered.end(), [](const Detection & a, const Detection & b) {
      return a.score > b.score;
    });
  for (const auto & detection : filtered) {
    if (std::none_of(job.result.detections.begin(), job.result.detections.end(),
      [&](const Detection & existing) {
        return IntersectionOverUnion(existing.box, detection.box) > *config_.duplicate_iou;
      })) {job.result.detections.push_back(detection);}
  }
  if (job.request.order == Order::TopToBottom) {
    std::stable_sort(job.result.detections.begin(), job.result.detections.end(),
      [](const Detection & a, const Detection & b) {
        if (!std::isfinite(a.depth_m)) {return false;}
        if (!std::isfinite(b.depth_m)) {return true;}
        return a.depth_m < b.depth_m;
      });
  } else if (job.request.order == Order::RegionCenter) {
    const auto & region = job.request.region;
    const auto cx = region.kind == RegionKind::ImageBox ?
      region.image_box.x + region.image_box.width / 2 : job.result.capture->rgb.width / 2.0;
    const auto cy = region.kind == RegionKind::ImageBox ?
      region.image_box.y + region.image_box.height / 2 : job.result.capture->rgb.height / 2.0;
    std::stable_sort(job.result.detections.begin(), job.result.detections.end(),
      [&](const Detection & a, const Detection & b) {
        auto distance = [&](const Detection & detection) {
          if (region.kind == RegionKind::SpaceBox) {
            const auto & k = job.result.capture->intrinsics;
            const std::array<double, 3> point{
              (detection.box.x + detection.box.width / 2 - k[2]) * detection.depth_m / k[0],
              (detection.box.y + detection.box.height / 2 - k[5]) * detection.depth_m / k[4],
              detection.depth_m};
            auto squared = 0.0;
            for (std::size_t i = 0; i < point.size(); ++i) {
              const auto center = region.minimum[i] / 2 + region.maximum[i] / 2;
              squared += std::pow(point[i] - center, 2);
            }
            return std::sqrt(squared);
          }
          return std::hypot(detection.box.x + detection.box.width / 2 - cx,
              detection.box.y + detection.box.height / 2 - cy);
        };
        return distance(a) < distance(b);
      });
  }
  const auto count = job.request.max_instances == 0 ? job.result.detections.size() :
    std::min(job.result.detections.size(), std::size_t(job.request.max_instances));
  for (std::size_t i = 0; i < count; ++i) {
    job.result.detections[i].selected = true;
    Instance instance;
    instance.index = job.result.detections[i].index;
    job.result.instances.push_back(std::move(instance));
  }
  PrepareWarmRoutes(now);
  update.feedback.push_back(FeedbackFor(true));
  if (count == 0) {Finish(Outcome::ValidEmpty, "", now, update);} else {NextEstimate(now, update);}
  return update;
}

void ProviderModel::NextEstimate(Nanoseconds now, Update & update)
{
  if (monotonic_now_) {now = monotonic_now_();}
  auto & job = *job_;
  while (job.next_instance < job.instance_order.size() && now < job.work_deadline) {
    const auto index = job.instance_order[job.next_instance];
    auto & instance = job.result.instances[index];
    if (job.routes[index] != Method::Estimated && config_.warm->prior_age_enabled) {
      const auto prior_arrival = job.matched_priors[index]->arrival_ns;
      if (now < prior_arrival || now - prior_arrival > config_.warm->prior_max_age_ns) {
        job.routes[index] = Method::Estimated;
      }
    }
    if (job.routes[index] == Method::Reused) {
      instance.status = InstanceStatus::Posed;
      instance.method = Method::Reused;
      instance.pose = job.matched_priors[index]->pose;
      ++job.next_instance;
      ++job.result.timing.reused;
      update.feedback.push_back(FeedbackFor(false, instance));
      if (monotonic_now_) {now = monotonic_now_();}
      continue;
    }
    job.stage = Stage::Pose;
    job.awaited_stamp = AllocateStamp();
    job.admitted_ns = now;
    instance.method = job.routes[index];
    job.native_work = instance.method ==
      Method::Refined ? NativeWork::Refine : NativeWork::Estimate;
    update.admission = Admission{job.native_work, *job.awaited_stamp, job.result.capture,
      job.result.detections[index], job.native_work == NativeWork::Refine ?
      std::optional<Pose>(job.matched_priors[index]->pose) : std::nullopt};
    return;
  }
  if (job.next_instance == job.result.instances.size()) {
    Finish(Outcome::Complete, "", now, update);
  } else {Finish(Outcome::Partial, "budget_exhausted", now, update);}
}

Update ProviderModel::ReceivePose(
  Nanoseconds processing_stamp, std::optional<Pose> pose, Nanoseconds now)
{
  auto update = Tick(now);
  if (!job_) {return update;}
  if (job_->stage != Stage::Pose || job_->awaited_stamp != processing_stamp) {
    ++job_->result.timing.late_outputs;
    return update;
  }
  auto & job = *job_;
  job.awaited_stamp.reset();
  const auto index = job.instance_order[job.next_instance];
  auto & instance = job.result.instances[index];
  if (job.native_work == NativeWork::Refine && (!pose || !AcceptRefinement(*pose,
    job.matched_priors[index]->pose, config_.warm->refinement)))
  {
    instance.estimate_ms += Milliseconds(now - job.admitted_ns);
    instance.status = InstanceStatus::Failed;
    instance.reason = "refinement_rejected";
    job.routes[index] = Method::Estimated;
    instance.method = Method::Estimated;
    NextEstimate(now, update);
    return update;
  }
  instance.estimate_ms += Milliseconds(now - job.admitted_ns);
  if (pose && ValidPose(*pose)) {
    instance.status = InstanceStatus::Posed;
    instance.reason.clear();
    instance.pose = *pose;
    if (instance.method == Method::Refined) {++job.result.timing.refined;} else {
      ++job.result.timing.estimated;
    }
  } else {
    instance.status = InstanceStatus::Failed;
    instance.reason = "invalid_native_pose";
  }
  ++job.next_instance;
  update.feedback.push_back(FeedbackFor(false, instance));
  NextEstimate(now, update);
  return update;
}

Update ProviderModel::ConfirmAdmission(Nanoseconds processing_stamp, Nanoseconds now)
{
  if (!job_ || job_->awaited_stamp != processing_stamp) {return Tick(now);}
  Update update;
  auto & job = *job_;
  if (now >= job.work_deadline) {
    job.awaited_stamp.reset();
    Finish(job.stage == Stage::Detect ? Outcome::Failed : Outcome::Partial,
      "budget_exhausted", now, update);
    return update;
  }
  if (job.stage == Stage::Pose && job.native_work == NativeWork::Refine &&
    config_.warm->prior_age_enabled)
  {
    const auto index = job.instance_order[job.next_instance];
    const auto arrival = job.matched_priors[index]->arrival_ns;
    if (now < arrival || now - arrival > config_.warm->prior_max_age_ns) {
      job.awaited_stamp.reset();
      job.routes[index] = Method::Estimated;
      NextEstimate(now, update);
      return update;
    }
  }
  job.admitted_ns = now;
  update.accepted = true;
  return update;
}

Update ProviderModel::FailNative(
  Nanoseconds processing_stamp, std::string reason, bool publication_started, Nanoseconds now)
{
  auto update = Tick(now);
  if (!job_ || job_->awaited_stamp != processing_stamp) {return update;}
  auto & job = *job_;
  if (job.stage == Stage::Pose && publication_started) {
    auto & instance = job.result.instances[job.instance_order[job.next_instance]];
    instance.status = InstanceStatus::Failed;
    instance.reason = reason;
    instance.estimate_ms += Milliseconds(now - job.admitted_ns);
  }
  job.awaited_stamp.reset();
  Finish(job.stage == Stage::Detect ? Outcome::Failed : Outcome::Partial,
    std::move(reason), now, update);
  return update;
}

Update ProviderModel::DiscardNativeOutput(Nanoseconds now)
{
  if (job_) {++job_->result.timing.late_outputs;}
  return Tick(now);
}

Feedback ProviderModel::FeedbackFor(bool detections, std::optional<Instance> instance) const
{
  const auto & job = *job_;
  Feedback feedback;
  feedback.detections = detections;
  feedback.observation.request_id = job.result.request_id;
  feedback.observation.attempt = job.result.attempt;
  feedback.observation.provider_session = job.result.provider_session;
  feedback.observation.capture = job.result.capture;
  if (detections) {feedback.observation.detections = job.result.detections;}
  feedback.selected_total = static_cast<std::uint32_t>(job.result.instances.size());
  feedback.finished_count = static_cast<std::uint32_t>(job.next_instance);
  feedback.instance = std::move(instance);
  return feedback;
}

void ProviderModel::PrepareWarmRoutes(Nanoseconds now)
{
  auto & job = *job_;
  const auto count = job.result.instances.size();
  job.routes.assign(count, Method::Estimated);
  job.matched_priors.resize(count);
  job.crops.resize(count);
  job.instance_order.resize(count);
  std::iota(job.instance_order.begin(), job.instance_order.end(), std::size_t(0));
  if (!config_.warm || !count) {return;}
  const auto & warm = *config_.warm;
  auto cached = std::find_if(cache_.begin(), cache_.end(), [&](const CachedViewpoint & entry) {
        return entry.viewpoint_id == job.request.viewpoint_id &&
               entry.epoch == job.result.capture->epoch &&
               entry.calibration_id == job.result.capture->calibration_id &&
               entry.model_revision == job.request.model_revision &&
               (warm.prior_age_enabled || entry.execution_id == job.request.execution_id);
    });
  std::vector<std::optional<std::size_t>> matches(count);
  if (cached != cache_.end()) {
    cached->last_used_ns = now;
    std::vector<Detection> selected(job.result.detections.begin(),
      job.result.detections.begin() + count);
    matches = MatchPriors(selected, cached->priors, warm.matching);
  }
  for (std::size_t i = 0; i < count; ++i) {
    if (monotonic_now_ && monotonic_now_() >= job.work_deadline) {break;}
    job.crops[i] = SampleCrop(*job.result.capture, job.result.detections[i].box, warm.crop_side);
    if (!matches[i] || job.request.reuse_policy == ReusePolicy::None) {continue;}
    const auto & prior = cached->priors[*matches[i]];
    const auto eligibility_now = monotonic_now_ ? monotonic_now_() : now;
    if (prior.reuse_count >= warm.max_reuse || (warm.prior_age_enabled &&
      (eligibility_now < prior.arrival_ns ||
      eligibility_now - prior.arrival_ns > warm.prior_max_age_ns))) {continue;}
    const auto change = CompareCrops(job.crops[i], prior.crop, warm.gate);
    if (change == Change::Large || change == Change::NoComparablePixels) {continue;}
    job.matched_priors[i] = prior;
    job.routes[i] = change == Change::Unchanged &&
      job.request.reuse_policy == ReusePolicy::ReuseAndRefine ? Method::Reused : Method::Refined;
  }
  std::stable_sort(job.instance_order.begin(), job.instance_order.end(), [&](std::size_t a,
    std::size_t b) {return job.routes[a] == Method::Reused && job.routes[b] != Method::Reused;});
}

void ProviderModel::StoreCache(Nanoseconds now)
{
  const auto & job = *job_;
  if (!config_.warm || !job.result.capture || job.result.outcome == Outcome::Cancelled ||
    job.result.capture->epoch != capture_epoch_ ||
    job.result.capture->calibration_id != calibration_id_) {return;}
  auto old = std::find_if(cache_.begin(), cache_.end(), [&](const CachedViewpoint & entry) {
        return entry.viewpoint_id == job.request.viewpoint_id;
    });
  if (old != cache_.end()) {cache_.erase(old);}
  CachedViewpoint entry;
  entry.viewpoint_id = job.request.viewpoint_id;
  entry.calibration_id = job.result.capture->calibration_id;
  entry.model_revision = job.request.model_revision;
  entry.execution_id = job.request.execution_id;
  entry.epoch = job.result.capture->epoch;
  entry.last_used_ns = now;
  for (std::size_t i = 0; i < job.result.instances.size(); ++i) {
    const auto & instance = job.result.instances[i];
    if (instance.status != InstanceStatus::Posed || job.crops[i].depth.empty()) {continue;}
    Prior prior;
    prior.box = job.result.detections[i].box;
    prior.pose = instance.pose;
    prior.crop = job.crops[i];
    prior.arrival_ns = job.result.capture->arrival_ns;
    prior.reuse_count = instance.method == Method::Estimated ? 0 :
      job.matched_priors[i]->reuse_count + 1;
    entry.priors.push_back(std::move(prior));
  }
  if (entry.priors.empty()) {return;}
  // Account retained values and container capacities. Allocator bookkeeping
  // and the separate current-job/capture storage are not an RSS guarantee.
  const auto entry_bytes = CacheEntryBytes(entry);
  const auto budget = config_.warm->cache_byte_budget;
  if (entry_bytes > budget || sizeof(CachedViewpoint) > budget - entry_bytes) {return;}
  if (cache_.size() == cache_.capacity()) {cache_.reserve(cache_.size() + 1);}
  while (cached_bytes() > budget - entry_bytes && !cache_.empty()) {
    const auto oldest = std::min_element(cache_.begin(), cache_.end(),
        [](const CachedViewpoint & a, const CachedViewpoint & b) {
          return a.last_used_ns < b.last_used_ns;
      });
    cache_.erase(oldest);
  }
  if (cached_bytes() > budget - entry_bytes) {
    // Empty outer backing storage still occupies the single byte budget.
    // Release it before reserving a single slot; never hide its capacity.
    ClearCache();
    cache_.reserve(1);
  }
  if (cached_bytes() > budget - entry_bytes) {ClearCache(); return;}
  cache_.push_back(std::move(entry));
}

std::uint64_t ProviderModel::CacheEntryBytes(const CachedViewpoint & entry)
{
  auto bytes = entry.viewpoint_id.capacity() + 1 + entry.calibration_id.capacity() + 1 +
    entry.model_revision.capacity() + 1 + entry.execution_id.capacity() + 1 +
    entry.priors.capacity() * sizeof(Prior);
  for (const auto & prior : entry.priors) {
    const auto crop_bytes = prior.crop.depth.capacity() * sizeof(float) +
      prior.crop.colour.capacity() * sizeof(std::array<float, 3>);
    if (bytes > std::numeric_limits<std::uint64_t>::max() - crop_bytes) {
      throw std::length_error("cache capacity size overflow");
    }
    bytes += crop_bytes;
  }
  return bytes;
}

std::uint64_t ProviderModel::cached_bytes() const
{
  // Read actual capacities after erase/move assignment: strings may retain a
  // heap-backed destination even when the source fits in its small buffer.
  auto bytes = cache_capacity_bytes();
  for (const auto & entry : cache_) {
    bytes += CacheEntryBytes(entry);
  }
  return bytes;
}

std::uint64_t ProviderModel::cache_capacity_bytes() const
{
  return cache_.capacity() * sizeof(CachedViewpoint);
}

void ProviderModel::ClearCache()
{
  std::vector<CachedViewpoint>().swap(cache_);
}

void ProviderModel::Finish(Outcome outcome, std::string reason, Nanoseconds now, Update & update)
{
  auto & job = *job_;
  if (job.stage == Stage::Pose && job.awaited_stamp && outcome == Outcome::Cancelled) {
    auto & in_flight = job.result.instances[job.instance_order[job.next_instance]];
    in_flight.status = InstanceStatus::Failed;
    in_flight.reason = reason;
    in_flight.estimate_ms += Milliseconds(now - job.admitted_ns);
  }
  job.result.outcome = outcome;
  job.result.reason = std::move(reason);
  job.result.timing.total_ms = Milliseconds(now - job.accepted_ns);
  job.result.timing.work_ms = Milliseconds(std::min(now, job.work_deadline) - job.accepted_ns);
  job.result.timing.grace_ms = Milliseconds(std::max(Nanoseconds(0), now - job.work_deadline));
  for (auto & instance : job.result.instances) {
    if (instance.status == InstanceStatus::NotAttempted) {instance.reason = job.result.reason;}
  }
  StoreCache(now);
  update.results.push_back(std::move(job.result));
  job_.reset();
}

Update ProviderModel::Tick(Nanoseconds now)
{
  Update update;
  if (!job_) {return update;}
  if (now < job_->accepted_ns) {throw std::invalid_argument("monotonic clock moved backwards");}
  if (job_->stage == Stage::Capture && now >= job_->work_deadline) {
    Finish(Outcome::Failed, "no_fresh_capture", now, update);
  } else if (job_->stage != Stage::Capture && now >= job_->final_deadline) {
    if (job_->stage == Stage::Pose) {
      auto & instance = job_->result.instances[job_->instance_order[job_->next_instance]];
      instance.status = InstanceStatus::Failed;
      instance.reason = "completion_grace_exhausted";
      instance.estimate_ms += Milliseconds(now - job_->admitted_ns);
      Finish(Outcome::Partial, "budget_exhausted", now, update);
    } else {Finish(Outcome::Failed, "detection_timeout", now, update);}
  }
  return update;
}

Update ProviderModel::Cancel(Nanoseconds now)
{
  Update update;
  if (job_) {Finish(Outcome::Cancelled, "cancelled", now, update);}
  return update;
}

std::optional<Nanoseconds> ProviderModel::next_deadline() const
{
  if (!job_) {return std::nullopt;}
  return job_->stage == Stage::Capture ? job_->work_deadline : job_->final_deadline;
}

std::optional<std::string> PendingGoalAdmission::Prepare(
  Request request, GoalId id, Nanoseconds now)
{
  // The framework's mutually exclusive callback ordering guarantees that any
  // earlier accepted callback would already have run. Replace an orphaned
  // decision without making it busy, preempting, or allocating native work.
  pending_.reset();
  if (auto reason = model_.CheckAcceptance(request, now)) {return reason;}
  pending_ = Decision{std::move(request), id, now};
  return std::nullopt;
}

Update PendingGoalAdmission::Commit(const GoalId & id, Nanoseconds now)
{
  Update update;
  if (!pending_ || pending_->id != id) {
    update.rejection = "stale_goal_handle";
    return update;
  }
  auto decision = std::move(*pending_);
  pending_.reset();
  if (now < decision.accepted_ns) {
    throw std::invalid_argument("monotonic clock moved backwards");
  }
  // OLD timeout/preemption and LATEST freshness use real current time. Only
  // the NEW job budget retains the earlier framework admission decision.
  update = model_.Accept(std::move(decision.request), now, decision.accepted_ns);
  if (!update.accepted) {return update;}
  // Nothing has been published. Enforce an expired original work budget now,
  // before the wrapper performs CPU payload copying or dispatches native work.
  auto current = update.admission ?
    model_.ConfirmAdmission(update.admission->processing_stamp_ns, now) : model_.Tick(now);
  if (!current.accepted && update.admission) {update.admission = std::move(current.admission);}
  for (auto & result : current.results) {
    update.results.push_back(std::move(result));
  }
  for (auto & feedback : current.feedback) {
    update.feedback.push_back(std::move(feedback));
  }
  return update;
}

}  // namespace manyforge::perception
