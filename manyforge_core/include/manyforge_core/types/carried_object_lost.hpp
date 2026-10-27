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
    "(bound {bound_mm} mm / {bound_deg}°); it was detached and the motion "
    "stopped."};

/// The row's full text with its values (mm and degrees, one decimal).
[[nodiscard]] inline std::string
CarriedObjectLostFullText(const std::string_view object,
                          const double translation_m, const double rotation_rad,
                          const double bound_translation_m,
                          const double bound_rotation_rad) {
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
  return text;
}

} // namespace manyforge::core
