#pragma once

// Shared by the gateway binding files: Python values <-> CanonicalValue and
// the one Python exception type of the core. Conversions run with the GIL
// held and do memory work only; every call that waits releases the GIL.

#include <pybind11/pybind11.h>

#include <cmath>
#include <string>
#include <utility>

#include "manyforge_headless/gateway_core.hpp"

namespace manyforge::headless::gateway::python {

namespace py = pybind11;

inline constexpr double kMaximumSafeInteger = 9007199254740991.0;

/// `manyforge_headless_py.GatewayError(code, message, details, producer)`.
/// Created by RegisterGateway; the other binding files only raise it.
/// Never destroyed: a static py::object would release its reference after
/// the interpreter is finalized.
inline py::object &GatewayErrorType() {
  static auto *type = new py::object();
  return *type;
}

/// Text as UTF-8; a str that has no UTF-8 form (a lone surrogate) is
/// refused like any other value without a JSON form.
inline std::string ToUtf8(const py::handle &text) {
  Py_ssize_t size = 0;
  const char *data = PyUnicode_AsUTF8AndSize(text.ptr(), &size);
  if (data == nullptr) {
    PyErr_Clear();
    throw GatewayError("execution_command_invalid",
                       "a gateway request text has no UTF-8 form");
  }
  return std::string(data, static_cast<std::size_t>(size));
}

inline CanonicalValue ToCanonical(const py::handle &value);

inline CanonicalValue::Object ToCanonicalObject(const py::handle &value) {
  if (!py::isinstance<py::dict>(value))
    throw GatewayError("execution_command_invalid",
                       "a gateway request member set must be a dict");
  CanonicalValue::Object members;
  for (const auto &item : py::reinterpret_borrow<py::dict>(value)) {
    if (!py::isinstance<py::str>(item.first))
      throw GatewayError("execution_command_invalid",
                         "a gateway request member name must be text");
    members.emplace_back(ToUtf8(item.first), ToCanonical(item.second));
  }
  return members;
}

/// A value the canonical form refuses (non-finite number, non-NFC text,
/// ...) is refused like one without a JSON form.
template <typename Make>
inline CanonicalValue CheckedCanonical(Make &&make) {
  try {
    return make();
  } catch (const content_identity::CanonicalValueError &error) {
    throw GatewayError("execution_command_invalid",
                       std::string("a gateway request value is not "
                                   "canonically encodable: ") +
                           error.what());
  }
}

inline CanonicalValue ToCanonical(const py::handle &value) {
  if (value.is_none()) return CanonicalValue(nullptr);
  if (py::isinstance<py::bool_>(value))
    return CanonicalValue(value.cast<bool>());
  if (py::isinstance<py::int_>(value)) {
    int overflow = 0;
    const long long number = PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
    if (number == -1 && PyErr_Occurred() != nullptr) PyErr_Clear();
    if (overflow != 0 || static_cast<double>(number) > kMaximumSafeInteger ||
        static_cast<double>(number) < -kMaximumSafeInteger)
      throw GatewayError("execution_command_invalid",
                         "an integer is outside the safe JSON range");
    return CanonicalValue(static_cast<double>(number));
  }
  if (py::isinstance<py::float_>(value)) {
    // RFC 8785 writes -0 as 0.
    const double number = value.cast<double>();
    return CheckedCanonical(
        [&] { return CanonicalValue(number == 0.0 ? 0.0 : number); });
  }
  if (py::isinstance<py::str>(value)) {
    auto text = ToUtf8(value);
    return CheckedCanonical([&] { return CanonicalValue(std::move(text)); });
  }
  if (py::isinstance<py::dict>(value)) {
    auto members = ToCanonicalObject(value);
    return CheckedCanonical([&] { return CanonicalValue(std::move(members)); });
  }
  if (py::isinstance<py::list>(value) || py::isinstance<py::tuple>(value)) {
    CanonicalValue::Array items;
    for (const auto &item : value) items.push_back(ToCanonical(item));
    return CanonicalValue(std::move(items));
  }
  throw GatewayError("execution_command_invalid",
                     "a gateway request value has no JSON form");
}

/// Integral numbers within the safe range become `int` (as `json.loads` of
/// the canonical bytes would give); other numbers `float`.
inline py::object ToPython(const CanonicalValue &value) {
  if (value.is_null()) return py::none();
  if (value.is_bool()) return py::bool_(value.as_bool());
  if (value.is_number()) {
    const double number = value.as_number();
    if (std::trunc(number) == number && std::fabs(number) <= kMaximumSafeInteger)
      return py::int_(static_cast<long long>(number));
    return py::float_(number);
  }
  if (value.is_string()) return py::str(value.as_string());
  if (value.is_array()) {
    py::list items;
    for (const auto &item : value.as_array()) items.append(ToPython(item));
    return items;
  }
  py::dict members;
  for (const auto &[name, member] : value.as_object())
    members[py::str(name)] = ToPython(member);
  return members;
}

/// Raises the Python GatewayError for a core GatewayError.
[[noreturn]] inline void RaiseGatewayError(const GatewayError &error) {
  py::dict details;
  for (const auto &[name, member] : error.details())
    details[py::str(name)] = ToPython(member);
  PyErr_SetObject(GatewayErrorType().ptr(),
                  py::make_tuple(error.code(), std::string(error.what()),
                                 details, error.producer())
                      .ptr());
  throw py::error_already_set();
}

}  // namespace manyforge::headless::gateway::python
