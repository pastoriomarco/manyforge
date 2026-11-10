#pragma once

#include <time.h>

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>

namespace manyforge::core {

/// One process's view of the host boot it runs in, for ordering owner
/// incarnations across processes on that host. CLOCK_BOOTTIME
/// is monotonic across processes of one boot and includes suspend; the boot id
/// names the boot, so two stamps are comparable only under the same boot id.
/// The core control plane is single-host by decision; a reader on another host
/// or boot carries a different boot id and is refused rather than compared.
struct HostBootStamp final {
  std::string boot_id;
  /// CLOCK_BOOTTIME in microseconds (an I-JSON safe integer for ~285 years).
  std::uint64_t boottime_us{};
};

/// Reads the running Linux host's boot id and current CLOCK_BOOTTIME.
[[nodiscard]] inline HostBootStamp ReadHostBootStamp() {
  std::ifstream file("/proc/sys/kernel/random/boot_id");
  std::string boot_id;
  if (!std::getline(file, boot_id) || boot_id.empty() || boot_id.size() > 64U) {
    throw std::runtime_error("host boot id is unavailable");
  }
  timespec now{};
  if (::clock_gettime(CLOCK_BOOTTIME, &now) != 0 || now.tv_sec < 0) {
    throw std::runtime_error("CLOCK_BOOTTIME is unavailable");
  }
  const auto microseconds = static_cast<std::uint64_t>(now.tv_sec) * 1'000'000U +
                            static_cast<std::uint64_t>(now.tv_nsec) / 1'000U;
  return {std::move(boot_id), microseconds};
}

}  // namespace manyforge::core
