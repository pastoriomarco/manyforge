#pragma once

#include <manyforge_deployment/program_reference_validator.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::deployment {

inline CanonicalValue program_reference_diagnostic_value(
    const ProgramReferenceDiagnostic &diagnostic) {
  using V = CanonicalValue;
  const auto optional_text = [](const std::optional<std::string> &text) {
    return text ? V(*text) : V(nullptr);
  };
  return V(V::Object{
      {"code", V(diagnostic.code)},
      {"location", V(V::Object{
          {"document", V("program")},
          {"path", V(diagnostic.location.path)},
          {"node_instance_id", optional_text(diagnostic.location.node_instance_id)},
          {"parameter", optional_text(diagnostic.location.parameter)},
      })},
      {"payload", diagnostic.payload},
  });
}

inline CanonicalValue program_reference_diagnostics_value(
    const std::vector<ProgramReferenceDiagnostic> &diagnostics) {
  CanonicalValue::Array rows;
  rows.reserve(diagnostics.size());
  for (const auto &diagnostic : diagnostics)
    rows.push_back(program_reference_diagnostic_value(diagnostic));
  return CanonicalValue(std::move(rows));
}

/// Preserve the validator's sorted prefix; `maximum_bytes` includes newline.
inline std::string bounded_program_reference_refusal(
    const std::vector<ProgramReferenceDiagnostic> &diagnostics,
    const std::size_t maximum_bytes) {
  using V = CanonicalValue;
  const auto envelope = [&](V::Array rows, bool truncated) {
    return V(V::Object{
        {"code", V("program_reference_validation_failed")},
        {"diagnostics", V(std::move(rows))},
        {"total_count", V(static_cast<double>(diagnostics.size()))},
        {"truncated", V(truncated)},
    });
  };
  // `false` is one byte longer than `true`, giving a safe bound for either.
  std::size_t bytes = canonical_json(envelope({}, false)).size() + 1U;
  if (bytes > maximum_bytes)
    throw std::length_error("reference refusal envelope exceeds output bound");
  V::Array rows;
  for (const auto &diagnostic : diagnostics) {
    auto row = program_reference_diagnostic_value(diagnostic);
    const auto extra = canonical_json(row).size() + (rows.empty() ? 0U : 1U);
    if (extra > maximum_bytes - bytes) break;
    bytes += extra;
    rows.push_back(std::move(row));
  }
  const bool truncated = rows.size() != diagnostics.size();
  return canonical_json(envelope(std::move(rows), truncated));
}

}  // namespace manyforge::deployment
