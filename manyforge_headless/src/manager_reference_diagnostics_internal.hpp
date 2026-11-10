#pragma once

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/program_reference_diagnostics.hpp>

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace manyforge::headless::detail {

// Leaves room for the ordinary Manager envelope inside Headless's 64 KiB cap.
inline constexpr std::size_t kMaximumManagerReferenceRefusalBytes = 48U * 1024U;

inline deployment::CanonicalValue AttachReferenceDiagnostics(
    deployment::CanonicalValue response,
    const std::vector<deployment::ProgramReferenceDiagnostic> &diagnostics) {
  using V = deployment::CanonicalValue;
  const auto envelope = deployment::parse_strict_json_model_yaml(
      deployment::bounded_program_reference_refusal(diagnostics,
                                                     kMaximumManagerReferenceRefusalBytes)).root();
  auto root = response.as_object();
  for (auto &[name, value] : root) {
    if (name != "error") continue;
    auto fields = value.as_object();
    fields.emplace_back("reference_validation", V(V::Object{
        {"diagnostics", *envelope.find_object_member("diagnostics")},
        {"total_count", *envelope.find_object_member("total_count")},
        {"truncated", *envelope.find_object_member("truncated")}}));
    value = V(std::move(fields));
    return V(std::move(root));
  }
  throw std::logic_error("Manager refusal response lacks error field");
}
} // namespace manyforge::headless::detail
