#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace manyforge::core {

/// Typed reason of the Stop and of the moving Program node's failure.
inline constexpr std::string_view kCarriedObjectLostReason =
    "carried_object_lost";

/// One error row, shaped like a future error-catalog line: code (area is the
/// prefix), compact text, full text. All carried-object-lost wording lives
/// here; the alarm catalog title repeats the compact text (tested).
struct ErrorCatalogRow final {
  std::string_view code;
  std::string_view compact_text;
  std::string_view full_text;
};

inline constexpr ErrorCatalogRow kCarriedObjectLostRow{
    "scene.carried_object_lost", "Carried object lost",
    "{object} moved {translation_mm} mm / {rotation_deg}° from its grasp pose "
    "(bound {bound_mm} mm / {bound_deg}°){outcome}."};

/// The row's full text with its values (mm and degrees, one decimal). The
/// outcome names what the attachment's `on_grasp_lost` did: the detach and
/// the motion Stop (both by default).
[[nodiscard]] inline std::string
CarriedObjectLostFullText(const std::string_view object,
                          const double translation_m, const double rotation_rad,
                          const double bound_translation_m,
                          const double bound_rotation_rad,
                          const bool motion_stopped = true,
                          const bool detached = true) {
  const auto fixed = [](const double value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", value);
    return std::string(text);
  };
  const double degrees = 180.0 / std::acos(-1.0);
  std::string text(kCarriedObjectLostRow.full_text);
  const auto put = [&text](const std::string_view key,
                           const std::string &value) {
    const auto at = text.find(key);
    text.replace(at, key.size(), value);
  };
  put("{object}", std::string(object));
  put("{translation_mm}", fixed(translation_m * 1000.0));
  put("{rotation_deg}", fixed(rotation_rad * degrees));
  put("{bound_mm}", fixed(bound_translation_m * 1000.0));
  put("{bound_deg}", fixed(bound_rotation_rad * degrees));
  put("{outcome}", detached && motion_stopped
                      ? "; it was detached and the motion stopped"
                  : detached       ? "; it was detached"
                  : motion_stopped ? "; the motion stopped"
                                   : "");
  return text;
}

/// The same carried-object-lost text with the holder and measured rigid body
/// identified for a relative-to-attached observation.
[[nodiscard]] inline std::string CarriedObjectLostFullTextWithReference(
    const std::string_view object,
    const std::string_view holder_component_id,
    const std::string_view command_target_id,
    const std::string_view gripper_link,
    const std::string_view measured_entity,
    const std::string_view measured_link,
    const double translation_m, const double rotation_rad,
    const double bound_translation_m, const double bound_rotation_rad,
    const bool motion_stopped = true, const bool detached = true) {
  auto text = CarriedObjectLostFullText(
      object, translation_m, rotation_rad, bound_translation_m,
      bound_rotation_rad, motion_stopped, detached);
  text += " Measured body " + std::string(measured_entity) + "/" +
          std::string(measured_link) + "; holder " +
          std::string(holder_component_id) + "/" +
          std::string(command_target_id) + " (base " +
          std::string(gripper_link) + ").";
  return text;
}

} // namespace manyforge::core
