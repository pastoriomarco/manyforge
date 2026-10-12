#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <manyforge_core/motion_interrupt/stop_profile_mapper.hpp>
#include <manyforge_core/types/interrupt.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {

void register_interrupt_bindings(py::module_& module) {
  py::enum_<MotionInterruptStopMode>(module, "MotionInterruptStopMode")
      .value("kSpringBackHold", MotionInterruptStopMode::kSpringBackHold)
      .value("kSoftOnPath", MotionInterruptStopMode::kSoftOnPath)
      .value("kHardDeceleration", MotionInterruptStopMode::kHardDeceleration);

  py::enum_<MotionInterruptStopCommandSource>(module, "MotionInterruptStopCommandSource")
      .value("kFjtPreemptHold", MotionInterruptStopCommandSource::kFjtPreemptHold)
      .value("kFjtPreemptPathRamp", MotionInterruptStopCommandSource::kFjtPreemptPathRamp)
      .value("kJtcSpeedScalingRamp", MotionInterruptStopCommandSource::kJtcSpeedScalingRamp)
      .value("kJtcCancelDecelerateOnCancel",
             MotionInterruptStopCommandSource::kJtcCancelDecelerateOnCancel)
      .value("kJtcCancelUnprofiledHold", MotionInterruptStopCommandSource::kJtcCancelUnprofiledHold)
      .value("kNativeSimulatedStop", MotionInterruptStopCommandSource::kNativeSimulatedStop)
      .value("kNativeSimulatedPathRamp", MotionInterruptStopCommandSource::kNativeSimulatedPathRamp)
      .value("kRobotNativeStop", MotionInterruptStopCommandSource::kRobotNativeStop)
      .value("kDiagnosticOther", MotionInterruptStopCommandSource::kDiagnosticOther);

  py::class_<MotionInterruptStopProfileDTO>(module, "MotionInterruptStopProfileDTO")
      .def(py::init<std::string, MotionInterruptStopMode, std::optional<double>,
                    std::optional<double>, std::map<std::string, CoordinateAccelerationDTO>, bool,
                    std::optional<MotionInterruptStopMode>, bool,
                    std::optional<std::map<std::string, CoordinateVelocityDTO>>,
                    InterruptStructuredValue, std::optional<bool>,
                    std::map<std::string, CoordinateJerkDTO>>(),
           py::arg("profile_id"), py::arg("stop_mode"),
           py::arg("deceleration_time_s") = std::nullopt,
           py::arg("max_stop_time_s") = std::nullopt,
           py::arg("per_joint_max_deceleration") =
               std::map<std::string, CoordinateAccelerationDTO>{},
           py::arg("allow_natural_completion_on_short_path") = false,
           py::arg("recoverable_substitution_mode") = std::nullopt,
           py::arg("require_backend_confirmation") = true,
           py::arg("per_joint_settle_velocity_threshold") = std::nullopt,
           py::arg("diagnostics") = InterruptStructuredValue{},
           py::arg("jerk_limits_enabled") = std::nullopt,
           py::arg("per_joint_max_jerk") = std::map<std::string, CoordinateJerkDTO>{})
      .def_readwrite("profile_id", &MotionInterruptStopProfileDTO::profile_id)
      .def_readwrite("stop_mode", &MotionInterruptStopProfileDTO::stop_mode)
      .def_readwrite("deceleration_time_s", &MotionInterruptStopProfileDTO::deceleration_time_s)
      .def_readwrite("max_stop_time_s", &MotionInterruptStopProfileDTO::max_stop_time_s)
      .def_readwrite("per_joint_max_deceleration",
                     &MotionInterruptStopProfileDTO::per_joint_max_deceleration)
      .def_readwrite("allow_natural_completion_on_short_path",
                     &MotionInterruptStopProfileDTO::allow_natural_completion_on_short_path)
      .def_readwrite("recoverable_substitution_mode",
                     &MotionInterruptStopProfileDTO::recoverable_substitution_mode)
      .def_readwrite("require_backend_confirmation",
                     &MotionInterruptStopProfileDTO::require_backend_confirmation)
      .def_readwrite("per_joint_settle_velocity_threshold",
                     &MotionInterruptStopProfileDTO::per_joint_settle_velocity_threshold)
      .def_readwrite("diagnostics", &MotionInterruptStopProfileDTO::diagnostics)
      .def_readwrite("jerk_limits_enabled", &MotionInterruptStopProfileDTO::jerk_limits_enabled)
      .def_readwrite("per_joint_max_jerk", &MotionInterruptStopProfileDTO::per_joint_max_jerk);

  module.def(
      "resolve_motion_interrupt_stop_jerk_limits",
      [](const MotionInterruptStopProfileDTO& profile) {
        auto resolved = resolve_motion_interrupt_stop_jerk_limits(profile);
        if (!resolved.ok()) throw py::value_error(resolved.error().message);
        return resolved.value();
      },
      py::arg("profile"));
  module.def(
      "motion_jerk_preset_limit",
      [](double acceleration, double reference_time_s) {
        if (!std::isfinite(acceleration) || acceleration <= 0.0 ||
            !std::isfinite(reference_time_s) || reference_time_s <= 0.0)
          throw py::value_error("Jerk preset inputs must be finite and positive");
        const auto limit = motion_jerk_preset_limit(acceleration, reference_time_s);
        if (!std::isfinite(limit)) throw py::value_error("Jerk preset overflows");
        return limit;
      },
      py::arg("acceleration_limit"), py::arg("reference_time_s"));

  module.def(
      "interrupt_structured_value_error",
      [](const InterruptStructuredValue& value, bool require_nonempty) {
        return interrupt_structured_value_error(value, require_nonempty);
      },
      py::arg("value"), py::arg("require_nonempty") = false);
  module.def("project_interrupt_structured_value_to_text",
             &project_interrupt_structured_value_to_text, py::arg("value"));
  module.def("motion_interrupt_stop_profile_field_error",
             &motion_interrupt_stop_profile_field_error, py::arg("profile"));
  module.def("motion_interrupt_deceleration_time_or_default",
             &motion_interrupt_deceleration_time_or_default, py::arg("profile"));
}

}  // namespace manyforge::core::python
