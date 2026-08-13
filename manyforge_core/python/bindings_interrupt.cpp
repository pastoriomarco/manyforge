#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

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
                    std::map<std::string, CoordinateAccelerationDTO>, bool,
                    std::optional<MotionInterruptStopMode>, bool,
                    std::optional<std::map<std::string, CoordinateVelocityDTO>>,
                    InterruptStructuredValue>(),
           py::arg("profile_id"), py::arg("stop_mode"),
           py::arg("deceleration_time_s") = std::nullopt,
           py::arg("per_joint_max_deceleration") =
               std::map<std::string, CoordinateAccelerationDTO>{},
           py::arg("allow_natural_completion_on_short_path") = false,
           py::arg("recoverable_substitution_mode") = std::nullopt,
           py::arg("require_backend_confirmation") = true,
           py::arg("per_joint_settle_velocity_threshold") = std::nullopt,
           py::arg("diagnostics") = InterruptStructuredValue{})
      .def_readwrite("profile_id", &MotionInterruptStopProfileDTO::profile_id)
      .def_readwrite("stop_mode", &MotionInterruptStopProfileDTO::stop_mode)
      .def_readwrite("deceleration_time_s", &MotionInterruptStopProfileDTO::deceleration_time_s)
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
      .def_readwrite("diagnostics", &MotionInterruptStopProfileDTO::diagnostics);

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
