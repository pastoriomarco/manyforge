#pragma once

#include <cstdint>
#include <manyforge_core/types/application_identity.hpp>
#include <manyforge_core/types/request_key.hpp>
#include <string_view>

namespace manyforge::core {

/// The application-start identity `A` of every Kernel and journal in tests.
inline constexpr ApplicationStartId kTestApplicationStartId{0x3f9a0000000000a1ULL};


/// Test-only: a readable request name made into a request key. It is an
/// HTTP-client key (class 3, part 0) whose origin and sequence are two FNV-1a
/// hashes of the name, so equal names give equal keys and distinct names give
/// distinct keys, and no test key can equal a Kernel-minted key (class 1).
[[nodiscard]] constexpr RequestKey TestRequestKey(std::string_view name) noexcept {
  std::uint64_t origin = 0xcbf29ce484222325ULL;
  std::uint64_t sequence = 0x84222325cbf29ce4ULL;
  for (const char character : name) {
    origin = (origin ^ static_cast<unsigned char>(character)) * 0x100000001b3ULL;
    sequence = (sequence ^ static_cast<unsigned char>(character)) * 0x100000001b3ULL;
    sequence ^= sequence >> 29U;
  }
  return RequestKey::Make(RequestKeyClass::kHttpClient, 0U, origin, sequence);
}

}  // namespace manyforge::core
