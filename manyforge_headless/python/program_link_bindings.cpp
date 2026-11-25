// Python binding of the Program runner's command and status sockets
// (program_link.hpp). JSON text is written and read here with Python's own
// `json` module and the GIL held, so the members keep exactly the values
// the runner and Headless exchanged before (floats stay floats, text stays
// as sent); every wait releases the GIL.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "gateway_py_convert.hpp"
#include "manyforge_headless/program_link.hpp"

namespace manyforge::headless::gateway {
namespace {

namespace py = pybind11;
using namespace pybind11::literals;

/// The code and text Headless reported for a request the runner refused as
/// malformed (its former HTTP 422).
constexpr const char *kMalformedRequestCode = "program_runner_http_error";
constexpr const char *kMalformedRequestMessage = "Program runner returned HTTP 422";

std::chrono::milliseconds Milliseconds(const double seconds) {
  // poll() takes an int; a day bounds one wait call, not the caller's wait.
  const double bounded = std::clamp(seconds, 0.0, 86400.0);
  return std::chrono::milliseconds(static_cast<long long>(bounded * 1000.0));
}

std::string Dumps(const py::handle &value, const bool sort_keys) {
  const py::object text = py::module_::import("json").attr("dumps")(
      value, "allow_nan"_a = false, "ensure_ascii"_a = false,
      "separators"_a = py::make_tuple(",", ":"), "sort_keys"_a = sort_keys);
  return text.cast<std::string>();
}

/// One JSON object, or nullopt for invalid UTF-8 or JSON, a duplicate name,
/// a non-finite number or another root.
std::optional<py::dict> StrictLoads(const std::string &bytes) {
  const py::cpp_function pairs([](const py::list &items) {
    py::dict members;
    for (const auto &item : items) {
      const auto pair = item.cast<py::tuple>();
      if (members.contains(pair[0]))
        throw py::value_error("duplicate JSON member");
      members[pair[0]] = pair[1];
    }
    return members;
  });
  const py::cpp_function constant([](const py::str &) -> py::object {
    throw py::value_error("non-finite JSON number");
  });
  py::object value;
  try {
    const py::str text(bytes.data(), bytes.size());
    value = py::module_::import("json").attr("loads")(
        text, "object_pairs_hook"_a = pairs, "parse_constant"_a = constant);
  } catch (const py::error_already_set &error) {
    if (!error.matches(PyExc_ValueError))
      throw;
    return std::nullopt;
  }
  if (!py::isinstance<py::dict>(value))
    return std::nullopt;
  return py::reinterpret_borrow<py::dict>(value);
}

bool IsText(const py::dict &value, const char *name, const bool non_empty) {
  if (!value.contains(name) || !py::isinstance<py::str>(value[name]))
    return false;
  return !non_empty || py::len(value[name]) > 0;
}

[[noreturn]] void RaiseResponseInvalid() {
  python::RaiseGatewayError(GatewayError(
      "program_runner_response_invalid",
      "Program runner returned an invalid response", {}, "program_runner"));
}

/// The runner's error object as Headless reads it (code, message, details,
/// producer), or nullopt for another shape.
std::optional<py::dict> ErrorObject(const py::handle &value) {
  if (!py::isinstance<py::dict>(value))
    return std::nullopt;
  const auto error = py::reinterpret_borrow<py::dict>(value);
  if (py::len(error) != 4 || !IsText(error, "code", true) ||
      !IsText(error, "message", false) || !IsText(error, "producer", true) ||
      !error.contains("details") || !py::isinstance<py::dict>(error["details"]))
    return std::nullopt;
  return error;
}

void ReplyMalformed(ProgramCommandServer &server, const std::uint64_t token) {
  py::dict error;
  error["code"] = kMalformedRequestCode;
  error["details"] = py::dict();
  error["message"] = kMalformedRequestMessage;
  error["producer"] = "program_runner";
  py::dict envelope;
  envelope["api_version"] = std::string(kProgramCommandResponseApiVersion);
  envelope["ok"] = false;
  envelope["error"] = error;
  const auto bytes = Dumps(envelope, false);
  const py::gil_scoped_release release;
  server.Reply(token, bytes);
}

py::object NextRequest(ProgramCommandServer &server, const double timeout_s) {
  const auto deadline = std::chrono::steady_clock::now() + Milliseconds(timeout_s);
  for (;;) {
    std::optional<ProgramCommandServer::Request> request;
    {
      const py::gil_scoped_release release;
      const auto remaining = std::max(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now()),
          std::chrono::milliseconds(0));
      request = server.Next(remaining);
    }
    if (!request.has_value())
      return py::none();
    const auto envelope = StrictLoads(request->bytes);
    if (envelope.has_value() && py::len(*envelope) == 3 &&
        IsText(*envelope, "api_version", true) &&
        (*envelope)["api_version"].cast<std::string>() ==
            kProgramCommandApiVersion &&
        IsText(*envelope, "command", true) && envelope->contains("request") &&
        py::isinstance<py::dict>((*envelope)["request"])) {
      py::dict value;
      value["command"] = (*envelope)["command"];
      value["request"] = (*envelope)["request"];
      return py::make_tuple(request->token, value);
    }
    ReplyMalformed(server, request->token);
  }
}

void SendReply(ProgramCommandServer &server, const std::uint64_t token,
               const py::dict &reply) {
  if (!reply.contains("ok") || !py::isinstance<py::bool_>(reply["ok"]))
    throw py::value_error("a Program command reply needs a boolean `ok`");
  const bool ok = reply["ok"].cast<bool>();
  const char *member = ok ? "value" : "error";
  if (py::len(reply) != 2 || !reply.contains(member))
    throw py::value_error(
        "a Program command reply is {ok: true, value} or {ok: false, error}");
  if (!ok && !ErrorObject(reply["error"]).has_value())
    throw py::value_error(
        "a Program command error is {code, details, message, producer}");
  py::dict envelope;
  envelope["api_version"] = std::string(kProgramCommandResponseApiVersion);
  envelope["ok"] = reply["ok"];
  envelope[member] = reply[member];
  const auto bytes = Dumps(envelope, false);
  const py::gil_scoped_release release;
  server.Reply(token, bytes);
}

void PublishStatus(ProgramStatusPublisher &publisher, const py::dict &cycle,
                   const bool critical) {
  // The frame's former encoding: sorted members, compact, UTF-8.
  auto bytes = Dumps(cycle, true);
  const py::gil_scoped_release release;
  publisher.Publish(std::move(bytes), critical);
}

/// Runs `start` (binds a socket) and raises core.GatewayError for its typed
/// refusal.
template <typename Server>
void StartServer(Server &server) {
  std::optional<GatewayError> failure;
  {
    const py::gil_scoped_release release;
    try {
      server.Start();
    } catch (const GatewayError &error) {
      failure = error;
    }
  }
  if (failure.has_value())
    python::RaiseGatewayError(*failure);
}

py::dict CallRunner(const ProgramRunnerLink &link, const std::string &command,
                    const py::dict &request, const std::optional<double> limit_s) {
  py::dict envelope;
  envelope["api_version"] = std::string(kProgramCommandApiVersion);
  envelope["command"] = command;
  envelope["request"] = request;
  const auto bytes = Dumps(envelope, false);
  std::string reply;
  std::optional<GatewayError> failure;
  {
    const py::gil_scoped_release release;
    try {
      reply = link.Call(bytes, limit_s.has_value()
                                   ? std::optional(Milliseconds(*limit_s))
                                   : std::nullopt);
    } catch (const GatewayError &error) {
      failure = error;
    }
  }
  if (failure.has_value())
    python::RaiseGatewayError(*failure);
  const auto parsed = StrictLoads(reply);
  if (!parsed.has_value())
    RaiseResponseInvalid();
  const py::dict &value = *parsed;
  if (py::len(value) != 3 || !IsText(value, "api_version", true) ||
      value["api_version"].cast<std::string>() !=
          kProgramCommandResponseApiVersion ||
      !value.contains("ok") || !py::isinstance<py::bool_>(value["ok"]))
    RaiseResponseInvalid();
  const bool ok = value["ok"].cast<bool>();
  py::dict result;
  result["ok"] = value["ok"];
  if (ok) {
    if (!value.contains("value"))
      RaiseResponseInvalid();
    result["value"] = value["value"];
    return result;
  }
  if (!value.contains("error") || !ErrorObject(value["error"]).has_value())
    RaiseResponseInvalid();
  result["error"] = value["error"];
  return result;
}

py::object NextStatus(ProgramRunnerLink::Subscription &subscription,
                      const double timeout_s) {
  std::optional<std::string> message;
  std::optional<GatewayError> failure;
  {
    const py::gil_scoped_release release;
    try {
      message = subscription.Next(Milliseconds(timeout_s));
    } catch (const GatewayError &error) {
      failure = error;
    }
  }
  if (failure.has_value())
    python::RaiseGatewayError(*failure);
  if (!message.has_value())
    return py::none();
  const auto parsed = StrictLoads(*message);
  if (!parsed.has_value())
    RaiseResponseInvalid();
  const py::dict &value = *parsed;
  if (py::len(value) != 3 || !IsText(value, "api_version", true) ||
      value["api_version"].cast<std::string>() != kProgramStatusStreamApiVersion ||
      !IsText(value, "type", true))
    RaiseResponseInvalid();
  const auto type = value["type"].cast<std::string>();
  if (type == "frame" && value.contains("frame") &&
      py::isinstance<py::dict>(value["frame"]))
    return value["frame"];
  if (type == "error" && value.contains("error") &&
      py::isinstance<py::dict>(value["error"])) {
    const auto error = py::reinterpret_borrow<py::dict>(value["error"]);
    if (IsText(error, "code", true) && IsText(error, "message", false))
      python::RaiseGatewayError(GatewayError(error["code"].cast<std::string>(),
                                             error["message"].cast<std::string>(),
                                             {}, "program_runner"));
  }
  RaiseResponseInvalid();
}

}  // namespace

void RegisterProgramLink(py::module_ &module) {
  py::class_<ProgramCommandServer>(module, "ProgramCommandServer")
      .def(py::init([](const std::string &socket_root) {
             return std::make_unique<ProgramCommandServer>(socket_root);
           }),
           "socket_root"_a)
      .def("start", &StartServer<ProgramCommandServer>)
      .def("close", &ProgramCommandServer::Close,
           py::call_guard<py::gil_scoped_release>())
      .def("next", &NextRequest, "timeout_s"_a)
      .def("reply", &SendReply, "token"_a, "reply"_a);

  py::class_<ProgramStatusPublisher>(module, "ProgramStatusPublisher")
      .def(py::init([](const std::string &socket_root) {
             return std::make_unique<ProgramStatusPublisher>(socket_root);
           }),
           "socket_root"_a)
      .def("start", &StartServer<ProgramStatusPublisher>)
      .def("close", &ProgramStatusPublisher::Close,
           py::call_guard<py::gil_scoped_release>())
      .def("publish", &PublishStatus, "program_cycle"_a, "critical"_a)
      .def(
          "withdraw",
          [](ProgramStatusPublisher &publisher, const std::string &code,
             const std::string &message) {
            const py::gil_scoped_release release;
            publisher.Withdraw(code, message);
          },
          "code"_a, "message"_a)
      .def("request_read", &ProgramStatusPublisher::RequestRead,
           py::call_guard<py::gil_scoped_release>())
      .def(
          "wait_read_request",
          [](ProgramStatusPublisher &publisher,
             const std::optional<double> timeout_s) {
            const py::gil_scoped_release release;
            return publisher.WaitReadRequest(
                timeout_s.has_value() ? std::optional(Milliseconds(*timeout_s))
                                      : std::nullopt);
          },
          "timeout_s"_a = py::none());

  py::class_<ProgramRunnerLink::Subscription>(module, "ProgramStatusSubscription")
      .def("next", &NextStatus, "timeout_s"_a)
      .def("close", &ProgramRunnerLink::Subscription::Close);

  py::class_<ProgramRunnerLink>(module, "ProgramRunnerLink")
      .def(py::init([](const std::string &socket_root, const double connect_limit_s) {
             try {
               return std::make_unique<ProgramRunnerLink>(
                   socket_root, Milliseconds(connect_limit_s));
             } catch (const GatewayError &error) {
               python::RaiseGatewayError(error);
             }
           }),
           "socket_root"_a, "connect_limit_s"_a = 10.0)
      .def("call", &CallRunner, "command"_a, "request"_a,
           "limit_s"_a = py::none())
      .def(
          "subscribe_status",
          [](const ProgramRunnerLink &link) {
            std::unique_ptr<ProgramRunnerLink::Subscription> subscription;
            std::optional<GatewayError> failure;
            {
              const py::gil_scoped_release release;
              try {
                subscription = link.SubscribeStatus();
              } catch (const GatewayError &error) {
                failure = error;
              }
            }
            if (failure.has_value())
              python::RaiseGatewayError(*failure);
            return subscription;
          });
}

}  // namespace manyforge::headless::gateway
