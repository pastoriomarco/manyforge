// `manyforge_headless_py.GatewayError` and `Gateway`
// (EXECUTION_PATH plan S1, OD-380). Arguments are converted with the GIL
// held; the GIL is released for the exchange and re-acquired to convert the
// reply.

#include <pybind11/eval.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

#include "gateway_py_convert.hpp"
#include "manyforge_headless/gateway_core.hpp"

namespace manyforge::headless::gateway {
namespace {

namespace py = pybind11;
using python::GatewayErrorType;
using python::RaiseGatewayError;
using python::ToCanonicalObject;
using python::ToPython;

constexpr const char *kGatewayErrorSource = R"PY(
class GatewayError(Exception):
    """A typed failure of the gateway core: transport, envelope or a refused
    local precondition. Owner refusals are replies, not errors."""

    def __init__(self, code, message, details=None, producer="headless_api"):
        details = dict(details or {})
        super().__init__(code, message, details, producer)
        self.code = code
        self.message = message
        self.details = details
        self.producer = producer

    def __str__(self):
        return self.message
)PY";

/// Request members with the GIL held; a value without a canonical form is
/// refused with the protocol's own invalid-command code.
CanonicalValue::Object Members(const py::handle &members,
                               const char *invalid_code) {
  try {
    return ToCanonicalObject(members);
  } catch (const GatewayError &error) {
    RaiseGatewayError(GatewayError(invalid_code, error.what()));
  }
}

std::optional<std::string> Principal(const py::object &principal) {
  if (principal.is_none()) return std::nullopt;
  if (!py::isinstance<py::str>(principal))
    RaiseGatewayError(GatewayError("execution_request_attribution_invalid",
                                   "the principal must be text"));
  try {
    return python::ToUtf8(principal);
  } catch (const GatewayError &) {
    RaiseGatewayError(GatewayError(
        "execution_request_attribution_invalid",
        "trusted execution principal must be bounded non-empty NFC text"));
  }
}

/// Runs `call` without the GIL; a core error becomes the Python one.
template <typename Call>
py::object Released(Call &&call) {
  CanonicalValue reply;
  try {
    py::gil_scoped_release release;
    reply = call();
  } catch (const GatewayError &error) {
    RaiseGatewayError(error);
  }
  return ToPython(reply);
}

py::object CallHost(const Gateway &gateway, const std::string &command,
                    const py::handle &members, const py::object &principal) {
  auto object = Members(members, "execution_command_invalid");
  auto principal_id = Principal(principal);
  return Released([&] {
    return gateway.Host(command, std::move(object), std::move(principal_id));
  });
}

py::object CallManager(const Gateway &gateway, const std::string &command,
                       const py::handle &members,
                       const std::optional<double> limit_s = std::nullopt) {
  std::optional<std::chrono::milliseconds> limit;
  if (limit_s.has_value()) {
    if (!std::isfinite(*limit_s) || *limit_s <= 0.0 || *limit_s > 60.0)
      throw py::value_error("Manager call limit must be in (0, 60] seconds");
    limit = std::chrono::milliseconds(
        std::max<long long>(1, std::llround(*limit_s * 1000.0)));
  }
  auto object = Members(members, "manager_command_invalid");
  return Released(
      [&] { return gateway.Manager(command, std::move(object), limit); });
}

std::unique_ptr<Gateway> MakeGateway(const py::object &execution_run_root,
                                     const py::object &manager_run_root,
                                     const std::string &attribution,
                                     double connect_limit_s) {
  GatewayConfig config;
  if (!execution_run_root.is_none())
    config.execution_run_root =
        std::filesystem::path(py::str(execution_run_root).cast<std::string>());
  if (!manager_run_root.is_none())
    config.manager_run_root =
        std::filesystem::path(py::str(manager_run_root).cast<std::string>());
  if (attribution == "internal")
    config.attribution = AttributionTransport::kInternal;
  else if (attribution == "http")
    config.attribution = AttributionTransport::kHttp;
  else
    throw py::value_error("gateway attribution must be 'internal' or 'http'");
  if (!std::isfinite(connect_limit_s) || connect_limit_s <= 0.0 ||
      connect_limit_s > 60.0)
    throw py::value_error("gateway connect limit must be in (0, 60] seconds");
  config.connect_limit = std::chrono::milliseconds(
      std::max<long long>(1, std::llround(connect_limit_s * 1000.0)));
  try {
    return std::make_unique<Gateway>(std::move(config));
  } catch (const GatewayError &error) {
    RaiseGatewayError(error);
  }
}

}  // namespace

void RegisterGateway(py::module_ &module) {
  py::dict scope;
  scope["__builtins__"] = py::module_::import("builtins");
  scope["__name__"] = module.attr("__name__");
  py::exec(kGatewayErrorSource, scope);
  GatewayErrorType() = scope["GatewayError"];
  module.attr("GatewayError") = GatewayErrorType();

  py::class_<Gateway> gateway(module, "Gateway");
  gateway.def(py::init(&MakeGateway), py::arg("execution_run_root"),
              py::arg("manager_run_root"), py::arg("attribution") = "internal",
              py::arg("connect_limit_s") = 10.0);
  gateway.def(
      "host",
      [](const Gateway &self, const std::string &command,
         const py::object &members, const py::object &principal_id) {
        return CallHost(self, command, members, principal_id);
      },
      py::arg("command"), py::arg("members") = py::dict(),
      py::arg("principal_id") = py::none());
  gateway.def(
      "manager",
      [](const Gateway &self, const std::string &command,
         const py::object &members, const std::optional<double> limit_s) {
        return CallManager(self, command, members, limit_s);
      },
      py::arg("command"), py::arg("members") = py::dict(),
      py::arg("limit_s") = py::none());
  gateway.def(
      "scene_commit",
      [](const Gateway &self, const py::object &transaction,
         const py::object &principal_id, const py::object &request_id,
         const py::object &transport) {
        auto object = Members(transaction, "manager_command_invalid");
        // The HTTP caller for History (OD-400); the Manager checks its form.
        const auto caller_text = [](const py::object &value)
            -> std::optional<std::string> {
          if (value.is_none())
            return std::nullopt;
          if (!py::isinstance<py::str>(value))
            RaiseGatewayError(GatewayError("scene_request_attribution_invalid",
                                           "the request caller must be text"));
          try {
            return python::ToUtf8(value);
          } catch (const GatewayError &) {
            RaiseGatewayError(GatewayError("scene_request_attribution_invalid",
                                           "the request caller must be NFC text"));
          }
        };
        auto caller_id = caller_text(request_id);
        auto caller_transport = caller_text(transport);
        std::optional<std::string> principal;
        if (!principal_id.is_none()) {
          if (!py::isinstance<py::str>(principal_id))
            RaiseGatewayError(GatewayError("scene_request_attribution_invalid",
                                           "the principal must be text"));
          try {
            principal = python::ToUtf8(principal_id);
          } catch (const GatewayError &) {
            RaiseGatewayError(GatewayError(
                "scene_request_attribution_invalid",
                "trusted scene principal must be bounded non-empty NFC text"));
          }
        }
        return Released([&] {
          return self.SceneCommit(std::move(object), std::move(principal),
                                  std::move(caller_id),
                                  std::move(caller_transport));
        });
      },
      py::arg("transaction"), py::arg("principal_id") = py::none(),
      py::arg("request_id") = py::none(), py::arg("transport") = py::none());
  gateway.def_static("host_commands", [] {
    py::list names;
    for (const auto &spec : HostCommands())
      names.append(py::str(spec.name.data(), spec.name.size()));
    return names;
  });
  gateway.def_static("manager_commands", [] {
    py::list names;
    for (const auto &spec : ManagerCommands())
      names.append(py::str(spec.name.data(), spec.name.size()));
    return names;
  });

  // One method per wire command, named as the command. The names are one
  // namespace: no host and Manager command, and no fixed method, share one.
  std::set<std::string> names{"host", "manager", "scene_commit",
                              "host_commands", "manager_commands"};
  for (const auto &spec : HostCommands()) {
    const std::string name(spec.name);
    if (!names.insert(name).second)
      throw std::logic_error("gateway command name collides: " + name);
    gateway.def(
        name.c_str(),
        [name](const Gateway &self, const py::object &members,
               const py::object &principal_id) {
          return CallHost(self, name, members, principal_id);
        },
        py::arg("members") = py::dict(), py::arg("principal_id") = py::none());
  }
  for (const auto &spec : ManagerCommands()) {
    const std::string name(spec.name);
    if (!names.insert(name).second)
      throw std::logic_error("gateway command name collides: " + name);
    gateway.def(
        name.c_str(),
        [name](const Gateway &self, const py::object &members) {
          return CallManager(self, name, members);
        },
        py::arg("members") = py::dict());
  }
}

}  // namespace manyforge::headless::gateway
