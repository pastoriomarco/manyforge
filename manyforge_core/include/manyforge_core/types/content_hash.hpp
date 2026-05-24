#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

namespace manyforge::core {

/// FNV-1a 64-bit content-hash primitives (spec §3.8.1, added by the
/// kernel-architecture sprint P-2-impl-snapshot chunk).
///
/// This is NOT a cryptographic hash. It is a process-independent
/// diagnostic fingerprint used for `CollisionWorldRevision.content_hash`
/// and similar content-defined identifiers. Stability properties:
///
///   - **Deterministic.** Identical byte streams produce identical
///     digests across builds, hosts, and architectures.
///   - **Order-sensitive.** Two streams differing only in byte order
///     produce different digests. Callers that need order-invariance
///     MUST sort their input before mixing.
///   - **Typed and length-prefixed.** Each `mix()` call contributes a
///     type tag and a canonical little-endian length/value encoding, so
///     `["ab","c"]`, `["a","bc"]`, and strings containing arbitrary
///     byte values all hash to distinct streams. Callers still define
///     logical fields by calling `mix()` once per field.
///
/// The hex digest is right-justified to 16 characters so digests sort
/// lexicographically in the same order as numerically, which makes
/// diffing dashboards bearable.
class ContentHasher {
 public:
  ContentHasher() = default;

  /// Mix one logical string field into the running digest. The string's
  /// type tag, byte length, and contents are mixed as separate canonical
  /// pieces so adjacent fields cannot collapse into the same byte stream.
  ContentHasher& mix(std::string_view field) noexcept {
    mix_byte(kTypeString);
    mix_uint64_le(static_cast<std::uint64_t>(field.size()));
    mix_bytes(reinterpret_cast<const unsigned char*>(field.data()),
              field.size());
    return *this;
  }

  /// Mix a 64-bit integer as an explicit type tag plus 8 little-endian
  /// bytes. Useful for numeric fields whose textual representation
  /// would be expensive (large numbers, monotonic ids).
  ContentHasher& mix(std::int64_t v) noexcept {
    mix_byte(kTypeInt64);
    mix_uint64_le(static_cast<std::uint64_t>(v));
    return *this;
  }

  ContentHasher& mix(std::uint64_t v) noexcept {
    mix_byte(kTypeUInt64);
    mix_uint64_le(v);
    return *this;
  }

  /// Mix a `double` by its IEEE-754 bit pattern encoded as explicit
  /// type tag plus 8 little-endian bytes. Bit-exact: two `double`s
  /// that compare equal under `==` may still differ in bit pattern
  /// (NaN, signed zero); callers that need value-equality MUST
  /// normalize upstream.
  ContentHasher& mix(double v) noexcept {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v));
    std::memcpy(&bits, &v, sizeof(bits));
    mix_byte(kTypeDouble);
    mix_uint64_le(bits);
    return *this;
  }

  /// Hex-encoded 16-char digest of the current state. Calling
  /// `finalize()` does NOT consume the hasher; further `mix()` calls
  /// continue from the same state. Callers MUST treat the returned
  /// digest as opaque text — do not parse it.
  [[nodiscard]] std::string finalize() const {
    std::ostringstream oss;
    oss.fill('0');
    oss.width(16);
    oss << std::hex << state_;
    return oss.str();
  }

  /// Reset to the FNV-1a offset basis. Useful for hashers reused across
  /// independent records.
  void reset() noexcept { state_ = kFnv1aOffsetBasis; }

  /// Raw state accessor, exposed for callers that want to chain two
  /// hashers or test the implementation.
  [[nodiscard]] std::uint64_t raw_state() const noexcept { return state_; }

 private:
  void mix_byte(unsigned char byte) noexcept {
    state_ ^= static_cast<std::uint64_t>(byte);
    state_ *= kFnv1aPrime;
  }

  void mix_bytes(const unsigned char* bytes, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
      mix_byte(bytes[i]);
    }
  }

  void mix_uint64_le(std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
      mix_byte(static_cast<unsigned char>((value >> (i * 8)) & 0xffU));
    }
  }

  // Canonical FNV-1a 64-bit constants (RFC draft-eastlake-fnv §3).
  static constexpr std::uint64_t kFnv1aOffsetBasis = 14695981039346656037ULL;
  static constexpr std::uint64_t kFnv1aPrime = 1099511628211ULL;
  static constexpr unsigned char kTypeString = 0x01U;
  static constexpr unsigned char kTypeInt64 = 0x02U;
  static constexpr unsigned char kTypeUInt64 = 0x03U;
  static constexpr unsigned char kTypeDouble = 0x04U;

  std::uint64_t state_ = kFnv1aOffsetBasis;
};

}  // namespace manyforge::core
