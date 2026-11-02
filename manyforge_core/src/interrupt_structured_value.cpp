#include <algorithm>
#include <cmath>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/interrupt.hpp>

// Closed structured-value validation shared by interrupt policy, Stop-profile
// and prepared-participant codecs. Kept apart from the Kernel's interrupt
// manager so the prepared-motion library does not link it.

namespace manyforge::core {
namespace {

[[nodiscard]] constexpr bool ascii_alphanumeric(unsigned char value) noexcept {
  return (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z')) ||
         (value >= static_cast<unsigned char>('A') && value <= static_cast<unsigned char>('Z')) ||
         (value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9'));
}

[[nodiscard]] bool portable_interrupt_structured_key(std::string_view value) noexcept {
  if (value.empty() || value.size() > kMaxInterruptStructuredValueKeyBytes ||
      !ascii_alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return ascii_alphanumeric(byte) || character == '.' || character == '_' || character == '-';
  });
}

}  // namespace

std::optional<std::string> interrupt_structured_value_error(const InterruptStructuredValue& value,
                                                            bool require_nonempty) {
  if (require_nonempty && value.empty()) {
    return "interrupt_structured_value_empty";
  }
  if (value.size() > kMaxInterruptStructuredValueEntries) {
    return "interrupt_structured_value_too_many_entries";
  }
  for (const auto& [key, scalar] : value) {
    if (!portable_interrupt_structured_key(key)) {
      return "interrupt_structured_value_key_invalid";
    }
    if (const auto* number = std::get_if<double>(&scalar);
        number != nullptr && !std::isfinite(*number)) {
      return "interrupt_structured_value_number_non_finite";
    }
    if (const auto* text = std::get_if<std::string>(&scalar); text != nullptr) {
      if (text->size() > kMaxInterruptStructuredValueStringBytes) {
        return "interrupt_structured_value_string_too_long";
      }
      if (!content_identity::is_utf8_nfc(*text)) {
        return "interrupt_structured_value_string_not_nfc";
      }
    }
  }
  return std::nullopt;
}

}  // namespace manyforge::core
