#pragma once

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/json_value.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace manyforge::core::detail {

inline void normalize_stop_fallback_parameters(JsonObjectDTO& parameters) {
  const auto legacy = parameters.find("recoverable_substitution_mode");
  if (legacy == parameters.end()) return;
  const auto primary = parameters.find("stop_mode");
  const auto* mode =
      primary == parameters.end() ? nullptr : std::get_if<std::string>(&primary->second.value);
  const auto* fallback = std::get_if<std::string>(&legacy->second.value);
  if (parameters.contains("fallback_stop_modes") || !mode || *mode != "soft_on_path" || !fallback ||
      *fallback != "spring_back_hold")
    throw std::invalid_argument("invalid legacy Stop fallback parameters");
  parameters.erase(legacy);
  parameters.emplace("fallback_stop_modes",
                     JsonValueDTO{JsonValueDTO::Array{JsonValueDTO{std::string("spring_back_hold")}}});
}

/// Cold codec projection only. Legacy replay retains its exact historical hash;
/// new objects always use the list. No runtime policy or identity is invented.
inline content_identity::CanonicalValue stop_fallback_wire_fields(
    const content_identity::CanonicalValue& value, bool legacy) {
  using V = content_identity::CanonicalValue;
  if (value.is_array()) {
    V::Array result;
    for (const auto& item : value.as_array())
      result.push_back(stop_fallback_wire_fields(item, legacy));
    return V(std::move(result));
  }
  if (!value.is_object()) return value;
  const auto* requested = value.find_object_member("requested_stop_mode");
  const bool qualification =
      requested != nullptr && value.find_object_member("provider_contract_ref") != nullptr;
  const bool profile = value.find_object_member("profile_id") != nullptr ||
                       value.find_object_member("per_resource_limits") != nullptr;
  const auto* primary = qualification ? requested
                        : profile     ? value.find_object_member("stop_mode")
                                      : nullptr;
  const auto old_key = qualification ? "executed_stop_mode" : "recoverable_substitution_mode";
  const auto* old = value.find_object_member(old_key);
  const auto* modes = value.find_object_member("fallback_stop_modes");
  const bool stop_object = primary != nullptr && (old != nullptr || modes != nullptr);
  if (stop_object && old && modes) throw std::invalid_argument("conflicting Stop fallback fields");
  V::Object result;
  for (const auto& [key, item] : value.as_object()) {
    if (stop_object && (key == old_key || key == "fallback_stop_modes")) continue;
    result.emplace_back(key, stop_fallback_wire_fields(item, legacy));
  }
  if (!stop_object) return V(std::move(result));
  if (!primary->is_string()) throw std::invalid_argument("invalid Stop fallback primary");
  if (old) {
    if (primary->as_string() != "soft_on_path" || !old->is_string() ||
        old->as_string() != "spring_back_hold")
      throw std::invalid_argument("invalid legacy Stop fallback");
    if (legacy)
      result.emplace_back(old_key, *old);
    else
      result.emplace_back("fallback_stop_modes", V(V::Array{V("spring_back_hold")}));
  } else {
    if (!modes->is_array() || modes->as_array().size() > 2U)
      throw std::invalid_argument("invalid Stop fallback list");
    if (modes->as_array().empty()) return V(std::move(result));
    if (legacy) {
      if (primary->as_string() != "soft_on_path" || modes->as_array().size() != 1U ||
          !modes->as_array().front().is_string() ||
          modes->as_array().front().as_string() != "spring_back_hold")
        throw std::invalid_argument("Stop sequence has no legacy encoding");
      result.emplace_back(old_key, V("spring_back_hold"));
    } else
      result.emplace_back("fallback_stop_modes", *modes);
  }
  return V(std::move(result));
}

}  // namespace manyforge::core::detail
