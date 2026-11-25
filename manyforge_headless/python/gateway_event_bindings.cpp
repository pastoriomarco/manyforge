// `manyforge_headless_py.ExecutionEvents`, `ExecutionEventSubscription` and
// `WakeSet` (EXECUTION_PATH plan S1/S2, OD-380). Calls that wait release the
// GIL; conversions run with it held.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

#include "gateway_py_convert.hpp"
#include "manyforge_headless/gateway_events.hpp"

namespace manyforge::headless::gateway {
namespace {

namespace py = pybind11;
using python::RaiseGatewayError;
using python::ToPython;

/// A wait bound in seconds, clamped to [0, one year] (chrono durations must
/// not overflow).
std::chrono::nanoseconds Seconds(double seconds) {
  constexpr double kYear = 365.0 * 24.0 * 3600.0;
  if (!(seconds > 0.0)) return std::chrono::nanoseconds::zero();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(std::min(seconds, kYear)));
}

/// `time.monotonic()` seconds as a steady-clock instant (both CLOCK_MONOTONIC).
std::chrono::steady_clock::time_point MonotonicDeadline(double seconds) {
  const auto now = std::chrono::steady_clock::now();
  const double now_seconds =
      std::chrono::duration<double>(now.time_since_epoch()).count();
  return now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                   Seconds(seconds - now_seconds));
}

}  // namespace

void RegisterGatewayEvents(py::module_ &module) {
  py::class_<WakeSet, std::shared_ptr<WakeSet>>(module, "WakeSet")
      .def(py::init<>())
      .def("watch_operation", &WakeSet::WatchOperation, py::arg("run_id"),
           py::arg("operation_id"))
      .def("watch_planning_job", &WakeSet::WatchPlanningJob, py::arg("run_id"),
           py::arg("job_id"))
      .def("unwatch_operation", &WakeSet::UnwatchOperation, py::arg("run_id"),
           py::arg("operation_id"))
      .def("unwatch_planning_job", &WakeSet::UnwatchPlanningJob, py::arg("run_id"),
           py::arg("job_id"))
      .def("unwatch_run", &WakeSet::UnwatchRun, py::arg("run_id"))
      .def("wake", &WakeSet::Wake, py::arg("run_id"))
      .def(
          "wait",
          [](WakeSet &self, double deadline_monotonic_s) {
            std::vector<WakeSet::RunId> woken;
            {
              py::gil_scoped_release release;
              woken = self.Wait(MonotonicDeadline(deadline_monotonic_s));
            }
            py::list result;
            for (const auto run_id : woken) result.append(py::int_(run_id));
            return result;
          },
          py::arg("deadline_monotonic_s"));

  py::class_<EventSubscription, std::shared_ptr<EventSubscription>>(
      module, "ExecutionEventSubscription")
      .def(
          "next",
          [](EventSubscription &self, double timeout_s) -> py::object {
            EventMessage message;
            {
              py::gil_scoped_release release;
              message = self.Next(Seconds(timeout_s));
            }
            if (message == nullptr) return py::none();
            return ToPython(*message);
          },
          py::arg("timeout_s"))
      .def("resync",
           [](EventSubscription &self) {
             py::gil_scoped_release release;
             self.Resync();
           })
      .def("close", [](EventSubscription &self) {
        py::gil_scoped_release release;
        self.Close();
      });

  py::class_<ExecutionEvents>(module, "ExecutionEvents")
      .def(py::init([](const std::string &execution_run_root,
                       std::shared_ptr<WakeSet> wake_set) {
             try {
               return std::make_unique<ExecutionEvents>(execution_run_root,
                                                        std::move(wake_set));
             } catch (const GatewayError &error) {
               RaiseGatewayError(error);
             }
           }),
           py::arg("execution_run_root"), py::arg("wake_set") = nullptr)
      .def("start", &ExecutionEvents::Start)
      .def("close",
           [](ExecutionEvents &self) {
             py::gil_scoped_release release;
             self.Close();
           })
      .def("subscribe", &ExecutionEvents::Subscribe)
      .def("connected", &ExecutionEvents::Connected)
      .def(
          "operation_envelope",
          [](const ExecutionEvents &self, std::uint64_t operation_id,
             std::uint64_t after_change) -> py::object {
            std::optional<CanonicalValue> envelope;
            try {
              envelope = self.OperationEnvelope(operation_id, after_change);
            } catch (const GatewayError &error) {
              RaiseGatewayError(error);
            }
            if (!envelope) return py::none();
            return ToPython(*envelope);
          },
          py::arg("operation_id"), py::arg("after_change"))
      .def(
          "planning_job_state",
          [](const ExecutionEvents &self, const std::string &job_id) -> py::object {
            std::optional<std::string> state;
            try {
              state = self.PlanningJobState(job_id);
            } catch (const GatewayError &error) {
              RaiseGatewayError(error);
            }
            if (!state) return py::none();
            return py::str(*state);
          },
          py::arg("job_id"));
}

}  // namespace manyforge::headless::gateway
