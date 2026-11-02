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
#include <cmath>
#include <iomanip>
#include <limits>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>
#include "manyforge_perception_provider/native_translation.hpp"
#include <composition_interfaces/srv/list_nodes.hpp>
#include <message_filters/subscriber.hpp>
#include <message_filters/sync_policies/exact_time.hpp>
#include <message_filters/synchronizer.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <rclcpp_components/register_node_macro.hpp>

namespace manyforge::perception
{
namespace
{

Nanoseconds Now()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

Nanoseconds Seconds(double seconds)
{
  const auto scaled = static_cast<long double>(seconds) * 1'000'000'000;
  if (!std::isfinite(seconds) || seconds < 0 ||
    scaled > static_cast<long double>(std::numeric_limits<Nanoseconds>::max()) - 0.5L)
  {
    throw std::invalid_argument("invalid provider duration setting");
  }
  return std::llround(scaled);
}

std::string Session()
{
  std::random_device random;
  std::ostringstream session;
  session << std::hex << std::setfill('0');
  for (unsigned i = 0; i < 16; ++i) {
    session << std::setw(2) << (random() & 0xff);
  }
  return session.str();
}

}  // namespace

class InstancePoseSequencer : public rclcpp::Node {
public:
  explicit InstancePoseSequencer(const rclcpp::NodeOptions & options)
  : Node("instance_pose_sequencer", options)
  {
    auto config = ReadConfig();
    const auto buffer_frames = config.buffer_frames;
    mask_margin_px_ = config.mask_margin_px;
    provider_session_ = config.provider_session;
    model_ = std::make_unique<ProviderModel>(std::move(config), 1, Now);
    goal_admission_ = std::make_unique<PendingGoalAdmission>(*model_);
    container_name_ = RequiredText("component_container");
    const auto reliable = rclcpp::QoS(10).reliable();
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
      const std::string prefix = i == 0 ? "native/detect/" : i ==
        1 ? "native/full/" : "native/refine/";
      inputs_[i].rgb = create_publisher<sensor_msgs::msg::Image>(prefix + "image", reliable);
      inputs_[i].camera = create_publisher<sensor_msgs::msg::CameraInfo>(prefix + "camera_info",
          reliable);
      if (i != 0) {
        inputs_[i].depth = create_publisher<sensor_msgs::msg::Image>(prefix + "depth", reliable);
      }
    }
    mask_pub_ = create_publisher<sensor_msgs::msg::Image>("native/full/mask", reliable);
    seed_pub_ = create_publisher<isaac_ros_tensor_msgs::msg::TensorList>("native/refine/seed",
        reliable);
    detections_sub_ = create_subscription<vision_msgs::msg::Detection2DArray>(
      "native/detections", reliable,
      [this](vision_msgs::msg::Detection2DArray::ConstSharedPtr message) {
        OnDetections(std::move(message));
      });
    for (std::size_t i = 0; i < pose_subs_.size(); ++i) {
      const auto work = i == 0 ? NativeWork::Estimate : NativeWork::Refine;
      const std::string prefix = i == 0 ? "native/full/" : "native/refine/";
      pose_subs_[i] = create_subscription<vision_msgs::msg::Detection3DArray>(
        prefix + "pose", reliable,
        [this, work](vision_msgs::msg::Detection3DArray::ConstSharedPtr message) {
          OnPose(std::move(message), work);
        });
      matrix_subs_[i] = create_subscription<isaac_ros_tensor_msgs::msg::TensorList>(
        prefix + "matrix", reliable,
        [this, work](isaac_ros_tensor_msgs::msg::TensorList::ConstSharedPtr message) {
          OnMatrix(std::move(message), work);
        });
    }
    // All model calls, action operations and translations use this node's
    // default mutually exclusive callback group, including the source filters.
    rgb_sub_.subscribe(this, "image_rect", rclcpp::SensorDataQoS());
    depth_sub_.subscribe(this, "depth", rclcpp::SensorDataQoS());
    camera_sub_.subscribe(this, "camera_info", rclcpp::SensorDataQoS());
    captures_ = std::make_unique<message_filters::Synchronizer<ExactCapture>>(
      ExactCapture(buffer_frames), rgb_sub_, depth_sub_, camera_sub_);
    captures_->registerCallback([this](auto rgb, auto depth, auto camera) {
        DrainCancelTerminal();
        Capture capture;
        try {capture = FromCapture(rgb, depth, camera);} catch (const std::exception & error) {
          RCLCPP_ERROR_THROTTLE(get_logger(), *get_clock(), 5000, "capture rejected: %s",
            error.what());
          return;
        }
        Update update;
        try {
          update = model_->ReceiveCapture(std::move(capture), Now());
        } catch (const std::invalid_argument & error) {
          RCLCPP_ERROR_THROTTLE(get_logger(), *get_clock(), 5000, "capture rejected: %s",
            error.what());
          return;
        }
        Apply(std::move(update));
      });
    composition_client_ = create_client<composition_interfaces::srv::ListNodes>(
      container_name_ + "/_container/list_nodes");
    RCLCPP_INFO(get_logger(), "Awaiting composition service discovery; action unavailable");
    // Standard ComponentManager stores the wrapper before adding this node to
    // its executor. The first callback therefore runs after self registration.
    // Discovery scheduling has no deadline. Submit exactly one construction
    // query once the existing service is matched; never retry that query.
    timer_ = create_wall_timer(std::chrono::milliseconds(100), [this] {
          if (!composition_client_->service_is_ready()) {return;}
          timer_->cancel();
          BeginConstructionRead();
      });
  }

private:
  using GoalHandle = rclcpp_action::ServerGoalHandle<EstimateInstances>;
  using ExactCapture = message_filters::sync_policies::ExactTime<
    sensor_msgs::msg::Image, sensor_msgs::msg::Image, sensor_msgs::msg::CameraInfo>;

  std::string RequiredText(const char * name)
  {
    declare_parameter(name, rclcpp::ParameterType::PARAMETER_STRING);
    auto value = get_parameter(name).as_string();
    if (value.empty()) {throw std::invalid_argument(std::string(name) + " is required");}
    return value;
  }

  double RequiredNumber(const char * name)
  {
    const auto & overrides = get_node_parameters_interface()->get_parameter_overrides();
    const auto supplied = overrides.find(name);
    if (supplied == overrides.end() ||
      (supplied->second.get_type() != rclcpp::ParameterType::PARAMETER_DOUBLE &&
      supplied->second.get_type() != rclcpp::ParameterType::PARAMETER_INTEGER))
    {
      throw std::invalid_argument(std::string(name) + " requires an integer or double value");
    }
    // Keep the declared type fixed, as before; do not enable arbitrary dynamic
    // parameter-type changes after the construction settings are consumed.
    const auto value = declare_parameter(name, supplied->second.get_type());
    return value.get_type() == rclcpp::ParameterType::PARAMETER_INTEGER ?
           static_cast<double>(value.get<std::int64_t>()) : value.get<double>();
  }

  std::uint32_t Integer(const char * name, std::optional<std::int64_t> default_value = std::nullopt)
  {
    if (default_value) {declare_parameter<std::int64_t>(name, *default_value);} else {
      declare_parameter(name, rclcpp::ParameterType::PARAMETER_INTEGER);
    }
    const auto value = get_parameter(name).as_int();
    if (value < 0 || value > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument(std::string(name) + " outside uint32");
    }
    return static_cast<std::uint32_t>(value);
  }

  Config ReadConfig()
  {
    Config config;
    config.model_id = RequiredText("model_id");
    config.model_revision = RequiredText("model_revision");
    config.provider_session = Session();
    config.buffer_frames = Integer("buffer_frames", 2);
    config.mask_margin_px = Integer("mask_margin_px", 0);
    config.settle_frames = Integer("settle_frames");
    config.duplicate_iou = RequiredNumber("duplicate_iou");
    config.completion_grace_ns = Seconds(declare_parameter<double>("completion_grace_s", 1.5));
    WarmSettings warm;
    warm.prior_age_enabled = declare_parameter<bool>("prior_age_enabled", true);
    warm.prior_max_age_ns = Seconds(declare_parameter<double>("prior_max_age_s", 30.0));
    const auto budget = declare_parameter<std::int64_t>("cache_byte_budget", 32 * 1024 * 1024);
    if (budget < 0) {throw std::invalid_argument("cache_byte_budget must be nonnegative");}
    warm.cache_byte_budget = static_cast<std::uint64_t>(budget);
    warm.crop_side = Integer("crop_side");
    warm.max_reuse = Integer("max_reuse");
    warm.matching = {RequiredNumber("prior_iou"), RequiredNumber("prior_center_px"),
      RequiredNumber("prior_size_ratio")};
    warm.gate = {RequiredNumber("unchanged_tol_m"), RequiredNumber("unchanged_max_fraction"),
      RequiredNumber("small_change_max_fraction"), RequiredNumber("small_change_max_m"),
      RequiredNumber("colour_tol"), RequiredNumber("colour_unchanged_max_fraction"),
      RequiredNumber("colour_small_change_max_fraction")};
    warm.refinement.translation_m = RequiredNumber("refine_max_translation");
    warm.refinement.rotation_rad = RequiredNumber("refine_max_rotation");
    declare_parameter("asset_symmetry_axes", rclcpp::ParameterType::PARAMETER_STRING_ARRAY);
    const auto axes = get_parameter("asset_symmetry_axes").as_string_array();
    for (const auto & axis : axes) {
      if (axis == "x_full") {
        warm.refinement.continuous_symmetry_axes[0] = true;
      } else if (axis == "y_full") {
        warm.refinement.continuous_symmetry_axes[1] = true;
      } else if (axis == "z_full") {warm.refinement.continuous_symmetry_axes[2] = true;} else {
        throw std::invalid_argument("unsupported continuous asset symmetry");
      }
    }
    declare_parameter("asset_symmetry_quaternions", rclcpp::ParameterType::PARAMETER_DOUBLE_ARRAY);
    const auto rotations = get_parameter("asset_symmetry_quaternions").as_double_array();
    if (rotations.size() % 4) {
      throw std::invalid_argument("asset symmetries require xyzw quaternions");
    }
    for (std::size_t i = 0; i < rotations.size(); i += 4) {
      warm.refinement.discrete_symmetries.push_back(
        {rotations[i], rotations[i + 1], rotations[i + 2], rotations[i + 3]});
    }
    Pose identity;
    identity.matrix = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (!AcceptRefinement(identity, identity, warm.refinement)) {
      throw std::invalid_argument("invalid refinement limits or asset symmetry");
    }
    config.warm = std::move(warm);
    return config;
  }

  void BeginConstructionRead()
  {
    construction_read_started_ = true;
    RCLCPP_INFO(get_logger(),
        "Awaiting one native composition construction read; action unavailable");
    try {
      composition_client_->async_send_request(
        std::make_shared<composition_interfaces::srv::ListNodes::Request>(),
        [this](rclcpp::Client<composition_interfaces::srv::ListNodes>::SharedFuture future) {
          try {
            const auto response = future.get();
            const std::set<std::string> expected{
              "/encoder/resize_node", "/encoder/image_format_converter_node", "/encoder/crop_node",
              "/encoder/image_to_tensor", "/encoder/normalize_node",
              "/encoder/interleaved_to_planar_node",
              "/encoder/reshape_node", "/yolov8_tensor_rt", "/yolov8_decoder", "/refine", "/score",
              "/tracking_refine", "/foundationpose", "/tracking", "/instance_pose_sequencer"};
            if (!response || response->full_node_names.size() != expected.size() ||
            response->unique_ids.size() != expected.size() ||
            std::set<std::string>(response->full_node_names.begin(),
              response->full_node_names.end()) != expected ||
            std::set<std::uint64_t>(response->unique_ids.begin(),
              response->unique_ids.end()).size() != expected.size())
            {
              throw std::runtime_error(
                "native composition differs from exact phase1 constructed nodes; "
                "inspect load errors");
            }
            StartActionServer();
            RCLCPP_INFO(get_logger(),
              "Native composition constructed; action available; "
              "engine execution is not yet qualified");
          } catch (const std::exception & error) {
            RCLCPP_ERROR(get_logger(), "Provider startup unavailable: %s", error.what());
          }
        });
    } catch (const std::exception & error) {
      RCLCPP_ERROR(get_logger(), "Provider startup unavailable: %s", error.what());
    }
  }

  void StartActionServer()
  {
    action_ = rclcpp_action::create_server<EstimateInstances>(this, "estimate_instances",
        [this](const auto & id, const auto goal) {
          DrainCancelTerminal();
          try {
            auto request = FromGoal(*goal);
            if (auto reason = goal_admission_->Prepare(std::move(request), id, Now())) {
              RCLCPP_WARN(get_logger(), "Goal rejected: %s", reason->c_str());
              return rclcpp_action::GoalResponse::REJECT;
            }
          // A failed framework goal-response send can omit the accepted
          // callback. This decision therefore owns no model job or effects.
            return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
          } catch (const std::exception & error) {
            RCLCPP_WARN(get_logger(), "Goal rejected: %s", error.what());
            return rclcpp_action::GoalResponse::REJECT;
          }
      },
        [this](const std::shared_ptr<GoalHandle> handle) {
          DrainCancelTerminal();
          Apply(model_->Tick(Now()));
          if (handle != current_goal_) {return rclcpp_action::CancelResponse::REJECT;}
          auto update = model_->Cancel(Now());
          if (update.results.size() != 1 || update.results.front().outcome != Outcome::Cancelled) {
            Apply(std::move(update));
            return rclcpp_action::CancelResponse::REJECT;
          }
          pending_cancel_ = CancelTerminal{handle, std::move(update.results.front())};
          current_goal_.reset();
          native_.reset();
          native_pose_.reset();
          native_matrix_.reset();
          Schedule();
          return rclcpp_action::CancelResponse::ACCEPT;
      },
        [this](const std::shared_ptr<GoalHandle> handle) {
        // rclcpp_action invokes accepted synchronously after its goal callback
        // in the same default callback group, so there is only one pending one.
          auto update = goal_admission_->Commit(handle->get_goal_id(), Now());
          if (!update.accepted) {
            Result rejected;
            rejected.request_id = handle->get_goal()->request_id;
            rejected.attempt = handle->get_goal()->attempt;
            rejected.provider_session = provider_session_;
            rejected.reason = update.rejection;
            handle->abort(std::make_shared<EstimateInstances::Result>(ToResult(rejected)));
            return;
          }
          if (current_goal_) {
            if (update.results.empty()) {
              throw std::logic_error("accepted goal did not retire old job");
            }
            DeliverResult(update.results.front());
            update.results.erase(update.results.begin());
          }
          current_goal_ = handle;
          Apply(std::move(update));
      });
  }

  void DrainCancelTerminal()
  {
    if (!pending_cancel_) {return;}
    // The framework enters canceling after returning from our cancel callback,
    // before any other callback in this mutually exclusive group can execute.
    auto terminal = std::move(*pending_cancel_);
    pending_cancel_.reset();
    terminal.handle->canceled(std::make_shared<EstimateInstances::Result>(ToResult(
        terminal.result)));
  }

  void DeliverResult(const Result & result)
  {
    if (!current_goal_) {throw std::logic_error("result has no current action handle");}
    auto message = std::make_shared<EstimateInstances::Result>(ToResult(result));
    if (result.outcome == Outcome::Failed || result.outcome == Outcome::Cancelled) {
      // Server preemption has no client cancel transition. Its authoritative
      // payload is CANCELLED; use the standard abort API, never private state.
      current_goal_->abort(message);
    } else {current_goal_->succeed(message);}
    current_goal_.reset();
    native_.reset();
    native_pose_.reset();
    native_matrix_.reset();
  }

  void Apply(Update update)
  {
    for (const auto & feedback : update.feedback) {
      if (!current_goal_) {throw std::logic_error("feedback has no current action handle");}
      current_goal_->publish_feedback(std::make_shared<EstimateInstances::Feedback>(ToFeedback(
          feedback)));
    }
    for (const auto & result : update.results) {DeliverResult(result);}
    if (update.admission) {Publish(std::move(*update.admission));}
    Schedule();
  }

  void Schedule()
  {
    // Source intake may run before the first scheduled callback. It must not
    // cancel the one pending construction read before that callback is sent.
    if (!construction_read_started_) {return;}
    if (timer_) {timer_->cancel();}
    auto deadline = model_->next_deadline();
    if (pending_cancel_) {deadline = Now();}
    if (!deadline) {return;}
    const auto delay = std::max(Nanoseconds(1), *deadline - Now());
    timer_ = create_wall_timer(std::chrono::nanoseconds(delay), [this] {
          timer_->cancel();
          DrainCancelTerminal();
          Apply(model_->Tick(Now()));
      });
  }

  void Publish(Admission admission)
  {
    bool publication_started = false;
    try {
      auto packet = MakeNativeInputs(admission, mask_margin_px_);
      // Construct all outgoing owned messages before the final model check.
      // Unique publication avoids another full CPU-image clone at this seam.
      auto rgb = std::make_unique<sensor_msgs::msg::Image>(std::move(packet.rgb));
      auto camera = std::make_unique<sensor_msgs::msg::CameraInfo>(std::move(packet.camera));
      auto depth =
        packet.depth ? std::make_unique<sensor_msgs::msg::Image>(std::move(*packet.depth)) :
        nullptr;
      auto mask =
        packet.mask ? std::make_unique<sensor_msgs::msg::Image>(std::move(*packet.mask)) : nullptr;
      auto seed =
        packet.seed ?
        std::make_unique<isaac_ros_tensor_msgs::msg::TensorList>(std::move(*packet.seed)) : nullptr;
      auto confirmation = model_->ConfirmAdmission(admission.processing_stamp_ns, Now());
      if (!confirmation.accepted) {Apply(std::move(confirmation)); return;}
      native_ = admission;
      native_pose_.reset();
      native_matrix_.reset();
      const auto index = admission.work == NativeWork::Detect ? 0u : admission.work ==
        NativeWork::Estimate ? 1u : 2u;
      publication_started = true;
      // Tracking is invoked once per selected REFINE route. Seed publication
      // precedes this image trio; no continuous stream or atomic seed ACK.
      if (seed) {seed_pub_->publish(std::move(seed));}
      if (mask) {mask_pub_->publish(std::move(mask));}
      inputs_[index].camera->publish(std::move(camera));
      if (depth) {inputs_[index].depth->publish(std::move(depth));}
      inputs_[index].rgb->publish(std::move(rgb));
    } catch (const std::exception & error) {
      RCLCPP_ERROR(get_logger(), "Native publication failed: %s", error.what());
      Apply(model_->FailNative(admission.processing_stamp_ns, "native_publication_failed",
          publication_started, Now()));
    }
  }

  void OnDetections(vision_msgs::msg::Detection2DArray::ConstSharedPtr message)
  {
    DrainCancelTerminal();
    const auto parsed_stamp = OutputStamp(message->header.stamp, "detector");
    if (!parsed_stamp) {Apply(model_->DiscardNativeOutput(Now())); return;}
    const auto stamp = *parsed_stamp;
    if (!native_ || native_->work != NativeWork::Detect ||
      native_->processing_stamp_ns != stamp || model_->awaited_stamp() != stamp)
    {
      Apply(model_->DiscardNativeOutput(Now()));
      return;
    }
    std::vector<Detection> detections;
    try {
      if (message->header.frame_id != native_->capture->frame_id) {
        throw std::invalid_argument("detector output frame differs from capture");
      }
      detections = FromDetections(*message, *native_->capture);
    } catch (const std::exception & error) {
      RCLCPP_ERROR(get_logger(), "Native detection rejected: %s", error.what());
      Apply(model_->FailNative(stamp, "invalid_native_detection", true, Now()));
      return;
    }
    Apply(model_->ReceiveDetections(stamp, std::move(detections), Now()));
  }

  void OnPose(vision_msgs::msg::Detection3DArray::ConstSharedPtr message, NativeWork work)
  {
    DrainCancelTerminal();
    const auto parsed_stamp = OutputStamp(message->header.stamp, "pose");
    if (!parsed_stamp) {Apply(model_->DiscardNativeOutput(Now())); return;}
    const auto stamp = *parsed_stamp;
    if (!native_ || native_->work != work || native_->processing_stamp_ns != stamp ||
      model_->awaited_stamp() != stamp || native_pose_)
    {
      Apply(model_->DiscardNativeOutput(Now()));
      return;
    }
    native_pose_ = std::move(message);
    CompletePosePair();
  }

  void OnMatrix(isaac_ros_tensor_msgs::msg::TensorList::ConstSharedPtr message, NativeWork work)
  {
    DrainCancelTerminal();
    const auto parsed_stamp = OutputStamp(message->header.stamp, "matrix");
    if (!parsed_stamp) {Apply(model_->DiscardNativeOutput(Now())); return;}
    const auto stamp = *parsed_stamp;
    if (!native_ || native_->work != work || native_->processing_stamp_ns != stamp ||
      model_->awaited_stamp() != stamp || native_matrix_)
    {
      Apply(model_->DiscardNativeOutput(Now()));
      return;
    }
    native_matrix_ = std::move(message);
    CompletePosePair();
  }

  std::optional<Nanoseconds> OutputStamp(
    const builtin_interfaces::msg::Time & stamp,
    const char * source)
  {
    try {return SourceTime(stamp);} catch (const std::invalid_argument & error) {
      RCLCPP_ERROR(get_logger(), "Discarding malformed %s header: %s", source, error.what());
      return std::nullopt;
    }
  }

  void CompletePosePair()
  {
    if (!native_pose_ || !native_matrix_) {return;}
    const auto stamp = native_->processing_stamp_ns;
    std::optional<Pose> pose;
    try {
      pose = FromNativePose(*native_pose_, *native_matrix_, stamp, native_->capture->frame_id);
    } catch (const std::exception & error) {
      RCLCPP_ERROR(get_logger(), "Native pose pair rejected: %s", error.what());
    }
    native_pose_.reset();
    native_matrix_.reset();
    Apply(model_->ReceivePose(stamp, std::move(pose), Now()));
  }

  struct InputPublishers
  {
    rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr rgb, depth;
    rclcpp::Publisher<sensor_msgs::msg::CameraInfo>::SharedPtr camera;
  };
  struct CancelTerminal
  {
    std::shared_ptr<GoalHandle> handle;
    Result result;
  };
  std::unique_ptr<ProviderModel> model_;
  std::unique_ptr<PendingGoalAdmission> goal_admission_;
  std::uint32_t mask_margin_px_{};
  std::string container_name_;
  std::string provider_session_;
  bool construction_read_started_{};
  std::array<InputPublishers, 3> inputs_;
  rclcpp::Publisher<sensor_msgs::msg::Image>::SharedPtr mask_pub_;
  rclcpp::Publisher<isaac_ros_tensor_msgs::msg::TensorList>::SharedPtr seed_pub_;
  message_filters::Subscriber<sensor_msgs::msg::Image> rgb_sub_, depth_sub_;
  message_filters::Subscriber<sensor_msgs::msg::CameraInfo> camera_sub_;
  std::unique_ptr<message_filters::Synchronizer<ExactCapture>> captures_;
  rclcpp::Subscription<vision_msgs::msg::Detection2DArray>::SharedPtr detections_sub_;
  std::array<rclcpp::Subscription<vision_msgs::msg::Detection3DArray>::SharedPtr, 2> pose_subs_;
  std::array<rclcpp::Subscription<isaac_ros_tensor_msgs::msg::TensorList>::SharedPtr,
    2> matrix_subs_;
  rclcpp::Client<composition_interfaces::srv::ListNodes>::SharedPtr composition_client_;
  rclcpp_action::Server<EstimateInstances>::SharedPtr action_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::shared_ptr<GoalHandle> current_goal_;
  std::optional<CancelTerminal> pending_cancel_;
  std::optional<Admission> native_;
  vision_msgs::msg::Detection3DArray::ConstSharedPtr native_pose_;
  isaac_ros_tensor_msgs::msg::TensorList::ConstSharedPtr native_matrix_;
};

}  // namespace manyforge::perception

RCLCPP_COMPONENTS_REGISTER_NODE(manyforge::perception::InstancePoseSequencer)
