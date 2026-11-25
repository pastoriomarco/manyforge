#pragma once

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/manager_liveness.hpp>

#include <map>
#include <optional>
#include <string>
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

/// A Runner fault (OD-267) has one of these codes: a failed BB key
/// publication or a failed Program tick worker.
[[nodiscard]] inline bool IsProgramRunnerFaultCode(std::string_view code) {
  return code == "program_bbkey_publication_failed" ||
         code == "program_tick_worker_failed";
}

/// Decode only after the existing heartbeat receiver/set/sequence identity
/// checks. The body supplies no predicate or operation/Run authority.
[[nodiscard]] inline std::optional<core::ErrorDTO>
DecodeProgramRunnerFault(const content_identity::CanonicalValue &value) {
  if (value.is_null())
    return std::nullopt;
  const auto invalid = [] {
    throw std::invalid_argument(
        "Program Runner fault has an invalid closed shape");
  };
  if (!value.is_object() || value.as_object().size() != 3U)
    invalid();
  const auto *code = value.find_object_member("code");
  const auto *message = value.find_object_member("message");
  const auto *details = value.find_object_member("details");
  if (!code || !code->is_string() ||
      !IsProgramRunnerFaultCode(code->as_string()) || !message ||
      !message->is_string() || message->as_string().size() > 1024U ||
      !content_identity::is_utf8_nfc(message->as_string()) || !details ||
      !details->is_object())
    invalid();
  // A tick worker fault may name where it was raised: `last_frame`, the
  // last traceback frame as "file:line function" (<= 512 bytes).
  std::map<std::string, std::string> kept;
  for (const auto &[name, value] : details->as_object()) {
    if (name != "last_frame" ||
        code->as_string() != "program_tick_worker_failed" ||
        !value.is_string() || value.as_string().empty() ||
        value.as_string().size() > 512U ||
        !content_identity::is_utf8_nfc(value.as_string()))
      invalid();
    kept.emplace(name, value.as_string());
  }
  return core::ErrorDTO{code->as_string(), message->as_string(),
                        std::move(kept)};
}

/// The Runner fault keeps its typed error; the text is only the diagnostic.
class ProgramRunnerFault final : public std::runtime_error {
public:
  ProgramRunnerFault(core::ErrorDTO error,
                              core::ProgramCycleStatusDTO cycle)
      : std::runtime_error(error.code + ": " + error.message),
        error_(std::move(error)), cycle_(std::move(cycle)) {}
  const core::ErrorDTO &error() const noexcept { return error_; }
  const core::ProgramCycleStatusDTO &cycle() const noexcept { return cycle_; }

private:
  core::ErrorDTO error_;
  core::ProgramCycleStatusDTO cycle_;
};

/// This check precedes ordinary current/reconciliation acceptance and effects.
inline void CheckProgramRunnerHealth(
    const content_identity::CanonicalValue &value,
    const core::ProgramCycleStatusDTO &cycle,
    bool command_fenced) {
  if (auto fault = DecodeProgramRunnerFault(value)) {
    if (!command_fenced)
      throw std::invalid_argument("Program Runner fault must fence commands");
    throw ProgramRunnerFault(std::move(*fault), cycle);
  }
}

} // namespace manyforge::headless::detail
