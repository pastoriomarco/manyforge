#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/resolved_frame.hpp>

namespace manyforge::core {

/// Deployment `tcp_speed_check` of one robot motion target (spec 430 §1.2).
/// The one admitted TCP linear-speed limit holds at every selected point; the
/// flange is the vendor's rated TCP-speed point (controller TCP offset zero).
enum class TcpSpeedCheck : std::uint8_t {
  kActiveTcp,
  kActiveTcpAndFlange,
};

[[nodiscard]] inline std::optional<TcpSpeedCheck> parse_tcp_speed_check(
    const std::string_view value) noexcept {
  if (value == "active_tcp") return TcpSpeedCheck::kActiveTcp;
  if (value == "active_tcp_and_flange") return TcpSpeedCheck::kActiveTcpAndFlange;
  return std::nullopt;
}

[[nodiscard]] inline std::string_view to_string(const TcpSpeedCheck value) noexcept {
  return value == TcpSpeedCheck::kActiveTcp ? "active_tcp" : "active_tcp_and_flange";
}

/// Portable role of a resolved frame; empty for a cell frame.
[[nodiscard]] inline std::string frame_role_id(const ResolvedFrameDTO& frame) {
  if (const auto* component = std::get_if<ResolvedComponentModelFrameDTO>(&frame))
    return component->frame_role_id.value();
  if (const auto* passive = std::get_if<ResolvedPassiveModelFrameDTO>(&frame))
    return passive->frame_role_id.value();
  return {};
}

/// The speed-check frame set: the active TCP first, then the robot flange
/// when one is selected and it is another link (on UR `tcp` and `flange` are
/// both `tool0`, so the set is one point).
[[nodiscard]] inline std::vector<ResolvedFrameDTO> speed_check_frames(
    const ResolvedFrameDTO& tcp, const std::optional<ResolvedFrameDTO>& flange) {
  std::vector<ResolvedFrameDTO> frames{tcp};
  if (flange && compiled_frame_name(*flange) != compiled_frame_name(tcp))
    frames.push_back(*flange);
  return frames;
}

}  // namespace manyforge::core
