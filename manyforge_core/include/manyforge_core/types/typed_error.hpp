#pragma once

#include "manyforge_core/types/error_code.hpp"

#include <exception>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace manyforge::core {

/// The one typed exception thrown across a layer boundary.
///
/// It carries the `ErrorDTO` carrier `{code, message, details}` so a boundary
/// catch keeps the cause's stable code instead of reducing it to `what()` text.
/// `producer` names the owner that produced the error when it is not the
/// thrower (for example a Kernel refusal forwarded by the Manager); empty means
/// the thrower's own process. `what()` is the producer's message.
class TypedError : public std::runtime_error {
public:
  TypedError(std::string code, const std::string &message,
             std::map<std::string, std::string> details = {},
             std::string producer = {})
      : std::runtime_error(message), code_(std::move(code)),
        details_(std::move(details)), producer_(std::move(producer)) {}

  explicit TypedError(const ErrorDTO &error, std::string producer = {})
      : TypedError(error.code, error.message, error.details,
                   std::move(producer)) {}

  [[nodiscard]] const std::string &code() const noexcept { return code_; }
  [[nodiscard]] const std::map<std::string, std::string> &details() const noexcept {
    return details_;
  }
  [[nodiscard]] const std::string &producer() const noexcept { return producer_; }
  [[nodiscard]] ErrorDTO error() const { return {code_, what(), details_}; }

private:
  std::string code_;
  std::map<std::string, std::string> details_;
  std::string producer_;
};

/// The typed cause of one caught exception and the owner that produced it.
struct TypedCause {
  ErrorDTO error;
  std::string producer;

  friend bool operator==(const TypedCause &, const TypedCause &) = default;
};

/// A `TypedError` keeps its code, details and producer (its producer defaults
/// to `boundary_producer`). Any other exception carries no code: it becomes
/// `fallback_code` with its `what()` text, produced at `boundary_producer`.
[[nodiscard]] inline TypedCause CauseOf(const std::exception &error,
                                        const std::string_view fallback_code,
                                        const std::string_view boundary_producer) {
  if (const auto *typed = dynamic_cast<const TypedError *>(&error)) {
    return {typed->error(), typed->producer().empty()
                                ? std::string(boundary_producer)
                                : typed->producer()};
  }
  return {{std::string(fallback_code), error.what(), {}},
          std::string(boundary_producer)};
}

/// The request-validation code shared by HTTP and RPC. Its details map each
/// refused field path to that field's own code (`missing`, `invalid`, ...).
inline constexpr std::string_view kErrorRequestInvalidCode = "invalid_request";

/// One request-validation refusal for the field at `path`.
[[nodiscard]] inline TypedError RequestInvalid(const std::string &path,
                                               std::string field_code,
                                               const std::string &message) {
  return TypedError(std::string(kErrorRequestInvalidCode), message,
                    {{path, std::move(field_code)}});
}

}  // namespace manyforge::core
