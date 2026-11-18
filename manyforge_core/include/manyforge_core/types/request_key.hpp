#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace manyforge::core {

/// The 16-byte identity of one request, carried unchanged from the sender to
/// the Kernel ledgers, the emergency reserve and the records.
///
/// | Bytes | Field |
/// | 0, bits 7-6 | class (`RequestKeyClass`) |
/// | 0, bits 5-0 | part: 0 for the request itself, 1-63 for each further Kernel action it causes |
/// | 1-7 | origin (56 bits, big-endian) |
/// | 8-15 | sequence (64 bits, big-endian) |
///
/// Wire form: exactly 32 lowercase hex characters. No allocation anywhere.
enum class RequestKeyClass : std::uint8_t {
  kWebSocket = 0U,
  kInternal = 1U,
  kProgramDerived = 2U,
  kHttpClient = 3U,
};

/// Origin of a class-1 (internal) key: the minter.
enum class RequestKeyMinter : std::uint8_t {
  kManager = 1U,
  kComposer = 2U,
  kKernel = 3U,
  kProgramRunner = 4U,
  kHeadless = 5U,
};

inline constexpr std::uint8_t kMaximumRequestKeyPart = 63U;

struct RequestKey final {
  std::array<std::uint8_t, 16> bytes{};

  [[nodiscard]] static constexpr RequestKey Make(RequestKeyClass key_class, std::uint8_t part,
                                                 std::uint64_t origin,
                                                 std::uint64_t sequence) noexcept {
    RequestKey key;
    key.bytes[0] = static_cast<std::uint8_t>((static_cast<std::uint8_t>(key_class) << 6U) |
                                             (part & kMaximumRequestKeyPart));
    for (std::size_t index = 0U; index < 7U; ++index) {
      key.bytes[7U - index] = static_cast<std::uint8_t>(origin >> (8U * index));
    }
    for (std::size_t index = 0U; index < 8U; ++index) {
      key.bytes[15U - index] = static_cast<std::uint8_t>(sequence >> (8U * index));
    }
    return key;
  }

  [[nodiscard]] constexpr RequestKeyClass key_class() const noexcept {
    return static_cast<RequestKeyClass>(bytes[0] >> 6U);
  }
  [[nodiscard]] constexpr std::uint8_t part() const noexcept {
    return static_cast<std::uint8_t>(bytes[0] & kMaximumRequestKeyPart);
  }
  [[nodiscard]] constexpr std::uint64_t origin() const noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 1U; index < 8U; ++index) value = (value << 8U) | bytes[index];
    return value;
  }
  [[nodiscard]] constexpr std::uint64_t sequence() const noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 8U; index < 16U; ++index) value = (value << 8U) | bytes[index];
    return value;
  }
  /// The same request's further Kernel action `part` (1-63).
  [[nodiscard]] constexpr RequestKey WithPart(std::uint8_t part_number) const noexcept {
    RequestKey key = *this;
    key.bytes[0] = static_cast<std::uint8_t>((bytes[0] & 0xC0U) |
                                             (part_number & kMaximumRequestKeyPart));
    return key;
  }
  /// Ledger index: the two big-endian halves.
  [[nodiscard]] constexpr std::uint64_t high() const noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) value = (value << 8U) | bytes[index];
    return value;
  }
  [[nodiscard]] constexpr std::uint64_t low() const noexcept { return sequence(); }

  friend constexpr bool operator==(const RequestKey&, const RequestKey&) = default;
  friend constexpr auto operator<=>(const RequestKey&, const RequestKey&) = default;
};

/// Exactly 32 hex characters; upper case accepted (HTTP normalises), no
/// allocation. Returns nullopt for any other text.
[[nodiscard]] constexpr std::optional<RequestKey> ParseRequestKey(std::string_view text) noexcept {
  if (text.size() != 32U) return std::nullopt;
  const auto nibble = [](char character) -> int {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
  };
  RequestKey key;
  for (std::size_t index = 0U; index < 16U; ++index) {
    const int high = nibble(text[2U * index]);
    const int low = nibble(text[2U * index + 1U]);
    if (high < 0 || low < 0) return std::nullopt;
    key.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return key;
}

/// 32 lowercase hex characters, no allocation.
[[nodiscard]] constexpr std::array<char, 32> FormatRequestKey(const RequestKey& key) noexcept {
  constexpr char kDigits[] = "0123456789abcdef";
  std::array<char, 32> text{};
  for (std::size_t index = 0U; index < 16U; ++index) {
    text[2U * index] = kDigits[key.bytes[index] >> 4U];
    text[2U * index + 1U] = kDigits[key.bytes[index] & 0x0FU];
  }
  return text;
}

/// The wire form as a `std::string`, for record and reply writers only (it
/// allocates; nothing on the Stop or admission path calls it under a lock).
[[nodiscard]] inline std::string RequestKeyText(const RequestKey& key) {
  const auto text = FormatRequestKey(key);
  return std::string(text.data(), text.size());
}

}  // namespace manyforge::core
