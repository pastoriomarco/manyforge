#pragma once

#include <manyforge_core/types/program_reference.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace manyforge::core {

/// Reserved system key only. User-authored key grammar is unchanged.
[[nodiscard]] inline bool valid_active_tcp_key_component(std::string_view value,
                                                          bool lowercase_first) noexcept {
  if (value.empty() || value.size() > 128U) return false;
  const auto lower = [](unsigned char c) { return c >= 'a' && c <= 'z'; };
  const auto upper = [](unsigned char c) { return c >= 'A' && c <= 'Z'; };
  const auto digit = [](unsigned char c) { return c >= '0' && c <= '9'; };
  const auto first = static_cast<unsigned char>(value.front());
  if (lowercase_first ? !lower(first) : !(lower(first) || upper(first) || digit(first)))
    return false;
  for (const unsigned char c : value) {
    if (!(lower(c) || (!lowercase_first && upper(c)) || digit(c) ||
          c == '.' || c == '_' || c == '-')) return false;
  }
  return true;
}

[[nodiscard]] inline std::optional<std::string> ActiveTcpBlackboardKey(
    const ProgramTargetReferenceDTO& target) {
  if (!valid_active_tcp_key_component(target.component_id.value(), true) ||
      !valid_active_tcp_key_component(target.command_target_id.value(), false)) return std::nullopt;
  return "active_tcp/" + target.component_id.value() + "/" + target.command_target_id.value();
}

[[nodiscard]] inline std::optional<ProgramTargetReferenceDTO> ParseActiveTcpBlackboardKey(
    std::string_view key) {
  constexpr std::string_view prefix = "active_tcp/";
  if (!key.starts_with(prefix)) return std::nullopt;
  key.remove_prefix(prefix.size());
  const auto slash = key.find('/');
  if (slash == std::string_view::npos || key.find('/', slash + 1U) != std::string_view::npos)
    return std::nullopt;
  const auto component = key.substr(0U, slash);
  const auto target = key.substr(slash + 1U);
  if (!valid_active_tcp_key_component(component, true) ||
      !valid_active_tcp_key_component(target, false)) return std::nullopt;
  return ProgramTargetReferenceDTO{ControllableComponentId(std::string(component)),
                                   CommandTargetId(std::string(target))};
}

}  // namespace manyforge::core
