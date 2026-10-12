#pragma once

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::component_manager {

class IsaacSiteSettingsError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

struct IsaacSiteWorld final {
  std::string world_uri;
  std::string expected_world_name;
};

struct IsaacSitePoseObserver final {
  std::string site_config;
  std::vector<std::string> additional_required_topics;
};

/// One structurally decoded caller-supplied Q81 site document.
///
/// No file access, defaults or path/topic normalization occurs here. The
/// existing launch/barrier input validation must run before effects when this
/// value is connected to startup. In particular, converting topics to the
/// existing comma-separated launch argument must preserve each array entry.
class IsaacSiteSettings final {
public:
  [[nodiscard]] double scene_startup_timeout_s() const noexcept;
  [[nodiscard]] double scene_service_timeout_s() const noexcept;
  [[nodiscard]] const IsaacSiteWorld &
  ResolveWorld(std::string_view logical_world) const;
  [[nodiscard]] const IsaacSitePoseObserver *
  FindPoseObserver(std::string_view cell_id) const noexcept;

private:
  IsaacSiteSettings(
      double startup_timeout, double service_timeout,
      std::map<std::string, IsaacSiteWorld, std::less<>> worlds,
      std::map<std::string, IsaacSitePoseObserver, std::less<>> pose_observers);

  double scene_startup_timeout_s_;
  double scene_service_timeout_s_;
  std::map<std::string, IsaacSiteWorld, std::less<>> worlds_;
  std::map<std::string, IsaacSitePoseObserver, std::less<>> pose_observers_;

  friend IsaacSiteSettings ParseIsaacSiteSettings(std::string_view bytes);
};

/// Reuses the existing strict JSON/YAML model decoder and closes only the
/// approved site shape. Runtime URI/path/topic/timeout policy stays with the
/// existing consumers; this value is not a successful Apply preflight.
[[nodiscard]] IsaacSiteSettings ParseIsaacSiteSettings(std::string_view bytes);

} // namespace manyforge::component_manager
