#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "activation_io_provider_internal.hpp"

// A sensor (the point-cloud source model `manyforge.pointcloud_source`) is a
// model-based, target-less component without a lane: the intent parse keeps
// its settings (and refuses a lane), and it compiles to a derived type without
// targets and one `sensors` row with the model's one adapter row and the
// effective settings, all required.
namespace {
namespace d = manyforge::deployment;
namespace c = manyforge::core;
namespace detail = manyforge::deployment::detail;
using Value = d::CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;

Object Settings() {
  return Object{{"topic", Value("/front_stereo_camera/pointcloud")},
                {"source_frame_id", Value("camera_link")},
                {"max_points", Value(200000.0)},
                {"minimum_period_s", Value(0.1)}};
}

Value Sensor(Object settings = Settings()) {
  return Value(Object{
      {"component_id", Value("front_camera_points")},
      {"component_profile_id", Value("manyforge.pointcloud_source")},
      {"target_settings", Value(Object{})},
      {"configuration",
       Value(Object{{"runtime_kind", Value("sensor")},
                    {"runtime_configuration", Value(std::move(settings))}})}});
}

Value Intent(Array components) {
  return Value(Object{{"api_version", Value("manyforge.deployment_intent.v2")},
                      {"deployment_artifact_id", Value("sensor")},
                      {"name", Value("sensor")},
                      {"attachments", Value(Array{})},
                      {"components", Value(std::move(components))},
                      {"policies", Value(Object{})}});
}

detail::ActivationDeploymentIntent Parse(Value sensor) {
  return detail::parse_activation_deployment_intent(Intent(Array{std::move(sensor)}), {});
}

std::vector<detail::ActivationComponentProfile>
Resolve(const detail::ActivationDeploymentIntent &intent) {
  return detail::resolve_activation_component_profiles(
      std::filesystem::path(MANYFORGE_WORKSPACE_ROOT), intent);
}

template <class Call> std::string Code(Call call) {
  try {
    call();
  } catch (const c::TypedError &error) {
    return error.code();
  }
  return {};
}

// The one compiled `sensors` row of `sensor`.
Value Row(Value sensor) {
  const auto intent = Parse(std::move(sensor));
  const auto types = detail::prepare_activation_sensor_types(intent, Resolve(intent));
  EXPECT_EQ(types.size(), 1U);
  EXPECT_TRUE(types.front().type.value().command_targets.empty());
  EXPECT_TRUE(types.front().type.value().model_roles.empty());
  const auto &rows =
      types.front().normalized_configuration.find_object_member("sensors")->as_array();
  EXPECT_EQ(rows.size(), 1U);
  return rows.front();
}

Value Parsed(const Value &row, std::string_view name) {
  return d::parse_strict_json_model_yaml(row.find_object_member(name)->as_string()).root();
}

TEST(ActivationSensor, ParseKeepsSettingsWithoutLaneOrModelSource) {
  const auto intent = Parse(Sensor());
  ASSERT_EQ(intent.components.size(), 1U);
  const auto &row = intent.components.front();
  EXPECT_FALSE(row.lane.has_value());
  EXPECT_FALSE(row.model_source.has_value());
  EXPECT_FALSE(row.root_pose_in_cell.has_value());
  EXPECT_EQ(row.configuration.find_object_member("runtime_kind")->as_string(), "sensor");
}

// Like every component, a sensor may set its lifecycle time (seconds,
// (0, 3600]); absent means the default 10 s.
TEST(ActivationSensor, ParseKeepsItsLifecycleTimeWithinBounds) {
  const auto with_budget = [](Value seconds) {
    auto row = Sensor().as_object();
    for (auto &[name, value] : row) {
      if (name != "configuration") continue;
      auto configuration = value.as_object();
      configuration.emplace_back("lifecycle_command_timeout_s", std::move(seconds));
      value = Value(std::move(configuration));
    }
    return Value(std::move(row));
  };
  const auto intent = Parse(with_budget(Value(20.0)));
  EXPECT_EQ(intent.components.front()
                .configuration.find_object_member("lifecycle_command_timeout_s")
                ->as_number(),
            20.0);
  EXPECT_THROW((void)Parse(with_budget(Value(0.0))), std::exception);
  EXPECT_THROW((void)Parse(with_budget(Value(3601.0))), std::exception);
}

TEST(ActivationSensor, ProfileIsASensorWithoutLane) {
  const auto profiles = Resolve(Parse(Sensor()));
  ASSERT_EQ(profiles.size(), 1U);
  EXPECT_EQ(profiles.front().runtime_kind, detail::ActivationRuntimeKind::kSensor);
  EXPECT_TRUE(detail::is_model_free_runtime_kind(profiles.front().runtime_kind));
  EXPECT_FALSE(profiles.front().lane.has_value());
  EXPECT_FALSE(profiles.front().lane_row.has_value());
}

TEST(ActivationSensor, CompilesOneTargetLessSensorRowWithEffectiveSettings) {
  const auto row = Row(Sensor());
  EXPECT_EQ(row.find_object_member("controllable_component_id")->as_string(),
            "front_camera_points");
  EXPECT_EQ(row.find_object_member("lane"), nullptr);
  const auto adapter = Parsed(row, "adapter_row");
  EXPECT_EQ(adapter.find_object_member("adapter")->as_string(), "pointcloud");
  EXPECT_EQ(adapter.find_object_member("stream_budget_s")->as_number(), 1.0);
  const auto settings = Parsed(row, "settings");
  EXPECT_EQ(settings.find_object_member("topic")->as_string(),
            "/front_stereo_camera/pointcloud");
  EXPECT_EQ(settings.find_object_member("source_frame_id")->as_string(), "camera_link");
  EXPECT_EQ(settings.find_object_member("max_points")->as_number(), 200000.0);
  EXPECT_EQ(settings.find_object_member("minimum_period_s")->as_number(), 0.1);
}

TEST(ActivationSensor, RefusesInvalidSettingsKindAndLane) {
  const auto prepare = [](Value sensor) {
    return [sensor = std::move(sensor)] {
      const auto intent = Parse(sensor);
      (void)detail::prepare_activation_sensor_types(intent, Resolve(intent));
    };
  };
  auto relative = Settings();
  relative.front().second = Value("front_stereo_camera/pointcloud");
  EXPECT_EQ(Code(prepare(Sensor(relative))), "sensor_configuration_invalid");
  auto many = Settings();
  many[2].second = Value(200001.0);
  EXPECT_EQ(Code(prepare(Sensor(many))), "sensor_configuration_invalid");
  auto slow = Settings();
  slow[3].second = Value(10.5);
  EXPECT_EQ(Code(prepare(Sensor(slow))), "sensor_configuration_invalid");
  auto unknown = Settings();
  unknown.emplace_back("http_timeout_s", Value(1.0));
  EXPECT_EQ(Code(prepare(Sensor(unknown))), "sensor_configuration_invalid");
  // Topic and source frame are required (no default).
  for (const auto *required : {"topic", "source_frame_id"}) {
    auto missing = Settings();
    std::erase_if(missing, [&](const auto &field) { return field.first == required; });
    EXPECT_EQ(Code(prepare(Sensor(missing))), "sensor_configuration_invalid");
  }
  auto wrong_kind = Parse(Sensor());
  wrong_kind.components.front().configuration =
      Value(Object{{"runtime_kind", Value("ros2_control")},
                   {"runtime_configuration", Value(Settings())}});
  EXPECT_THROW((void)Resolve(wrong_kind), std::exception);
  // A sensor has no lane: an authored one is refused.
  auto with_lane = Sensor().as_object();
  with_lane.emplace_back("lane", Value("isaac"));
  EXPECT_THROW((void)Parse(Value(std::move(with_lane))), std::exception);
}

} // namespace
