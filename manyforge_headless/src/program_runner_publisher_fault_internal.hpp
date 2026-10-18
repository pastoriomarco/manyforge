#pragma once

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/manager_liveness.hpp>

#include <optional>
#include <stdexcept>
#include <utility>

namespace manyforge::headless::detail {

/// An acknowledged real Manager heartbeat can report only these existing
/// states. Never normalize a foreign state before validating its digest.
[[nodiscard]] inline core::ManagerLivenessStateDTO
DecodeProgramRunnerManagerState(std::string_view state) {
  if (state == "current")
    return core::ManagerLivenessStateDTO::kCurrent;
  if (state == "unreconciled")
    return core::ManagerLivenessStateDTO::kUnreconciled;
  throw std::invalid_argument("Program Runner liveness state is invalid");
}

/// Decode only after the existing heartbeat receiver/set/sequence identity
/// checks. The body supplies no predicate or operation/Run authority.
[[nodiscard]] inline std::optional<core::ErrorDTO>
DecodeProgramRunnerPublisherFault(
    const content_identity::CanonicalValue &value) {
  if (value.is_null())
    return std::nullopt;
  const auto invalid = [] {
    throw std::invalid_argument(
        "Program Runner publisher fault has an invalid closed shape");
  };
  if (!value.is_object() || value.as_object().size() != 3U)
    invalid();
  const auto *code = value.find_object_member("code");
  const auto *message = value.find_object_member("message");
  const auto *details = value.find_object_member("details");
  if (!code || !code->is_string() ||
      code->as_string() != "program_bbkey_publication_failed" || !message ||
      !message->is_string() || message->as_string().size() > 1024U ||
      !content_identity::is_utf8_nfc(message->as_string()) || !details ||
      !details->is_object() || !details->as_object().empty())
    invalid();
  return core::ErrorDTO{code->as_string(), message->as_string(), {}};
}

/// The existing retained Runner-loss diagnostic has a 1024-byte bound. Keep
/// its typed cause when clipping a multibyte message at that boundary.
[[nodiscard]] inline std::string
ProgramRunnerPublisherFaultMessage(const core::ErrorDTO &error) {
  auto message = error.code + ": " + error.message;
  if (message.size() > 1024U) {
    message.resize(1024U);
    while (!content_identity::is_utf8_nfc(message))
      message.pop_back();
  }
  return message;
}

class ProgramRunnerPublisherFault final : public std::runtime_error {
public:
  ProgramRunnerPublisherFault(core::ErrorDTO error,
                              core::ProgramCycleStatusDTO cycle)
      : std::runtime_error(ProgramRunnerPublisherFaultMessage(error)),
        cycle_(std::move(cycle)) {}
  const core::ProgramCycleStatusDTO &cycle() const noexcept { return cycle_; }

private:
  core::ProgramCycleStatusDTO cycle_;
};

/// Caller has authenticated the ACK and validated its existing status digest.
/// This check precedes ordinary current/reconciliation acceptance and effects.
inline void CheckProgramRunnerPublicationHealth(
    const content_identity::CanonicalValue &value,
    const core::ProgramCycleStatusDTO &cycle,
    const core::RuntimeEpoch &receiver_runtime_epoch, bool command_fenced,
    const std::optional<std::string> &fault_reference) {
  if (auto fault = DecodeProgramRunnerPublisherFault(value)) {
    // Reuse ProgramRuntime._digest's direct RFC8785 ErrorDTO digest. This
    // existing status field remains inside the original fourteen-field hash.
    if (!fault_reference ||
        *fault_reference != content_identity::sha256_digest(
                                content_identity::canonical_json(value)))
      throw std::invalid_argument(
          "Program Runner publisher fault reference differs from its error");
    if (!command_fenced || cycle.program_runner_owner_epoch.value() !=
                               receiver_runtime_epoch.value())
      throw std::invalid_argument("Program Runner publisher fault must name "
                                  "its current owner and fence commands");
    throw ProgramRunnerPublisherFault(std::move(*fault), cycle);
  }
}

} // namespace manyforge::headless::detail
