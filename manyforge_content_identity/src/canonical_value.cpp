#include "manyforge_content_identity/canonical_value.hpp"

#include <double-conversion/double-conversion.h>
#include <utf8proc.h>

#include <algorithm>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <sstream>

namespace manyforge::content_identity {
namespace {

using Utf8procBuffer =
    std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)>;

struct NfcInspection final {
  bool is_nfc;
  std::string error;
};

[[nodiscard]] NfcInspection inspect_utf8_nfc(std::string_view text) {
  if (std::all_of(text.begin(), text.end(),
                  [](unsigned char byte) { return byte <= 0x7fU; })) {
    return {true, {}};
  }

  static constexpr utf8proc_uint8_t kEmptyInput = 0;
  utf8proc_uint8_t* raw_normalized = nullptr;
  const auto* input =
      text.empty()
          ? &kEmptyInput
          : reinterpret_cast<const utf8proc_uint8_t*>(text.data());
  const auto options =
      static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE);
  const utf8proc_ssize_t normalized_length = utf8proc_map(
      input, static_cast<utf8proc_ssize_t>(text.size()), &raw_normalized,
      options);
  Utf8procBuffer normalized(raw_normalized, &std::free);

  if (normalized_length == UTF8PROC_ERROR_NOMEM) {
    throw std::bad_alloc();
  }
  if (normalized_length < 0) {
    return {false, utf8proc_errmsg(normalized_length)};
  }

  const auto normalized_size = static_cast<std::size_t>(normalized_length);
  const bool unchanged =
      normalized_size == text.size() &&
      (normalized_size == 0U ||
       std::memcmp(normalized.get(), text.data(), normalized_size) == 0);
  return {unchanged,
          unchanged ? std::string{} : "text is not Unicode NFC"};
}

void validate_number(double value) {
  if (!std::isfinite(value)) {
    throw CanonicalValueError("I-JSON number must be finite");
  }
  if (value == 0.0 && std::signbit(value)) {
    throw CanonicalValueError("negative zero is not accepted by ManyForge");
  }
}

[[nodiscard]] std::vector<std::uint16_t> utf16_units(
    std::string_view utf8) {
  std::vector<std::uint16_t> result;
  result.reserve(utf8.size());

  utf8proc_ssize_t offset = 0;
  while (offset < static_cast<utf8proc_ssize_t>(utf8.size())) {
    utf8proc_int32_t code_point = 0;
    const utf8proc_ssize_t consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(utf8.data()) + offset,
        static_cast<utf8proc_ssize_t>(utf8.size()) - offset, &code_point);
    if (consumed <= 0) {
      throw CanonicalValueError(
          "invalid UTF-8 reached RFC 8785 object sorting");
    }
    offset += consumed;

    const auto scalar = static_cast<std::uint32_t>(code_point);
    if (scalar <= 0xffffU) {
      if (scalar >= 0xd800U && scalar <= 0xdfffU) {
        throw CanonicalValueError(
            "Unicode surrogate reached RFC 8785 object sorting");
      }
      result.push_back(static_cast<std::uint16_t>(scalar));
      continue;
    }
    if (scalar > 0x10ffffU) {
      throw CanonicalValueError(
          "out-of-range Unicode scalar reached RFC 8785 object sorting");
    }

    const auto supplementary = scalar - 0x10000U;
    result.push_back(
        static_cast<std::uint16_t>(0xd800U + (supplementary >> 10U)));
    result.push_back(static_cast<std::uint16_t>(
        0xdc00U + (supplementary & 0x3ffU)));
  }
  return result;
}

struct ObjectMemberSortKey final {
  std::vector<std::uint16_t> utf16_name;
  std::size_t source_index;
};

void append_escaped_string(std::string_view value, std::string& output) {
  static constexpr char kHex[] = "0123456789abcdef";

  output.push_back('"');
  for (const unsigned char byte : value) {
    switch (byte) {
      case 0x08:
        output.append("\\b");
        break;
      case 0x09:
        output.append("\\t");
        break;
      case 0x0a:
        output.append("\\n");
        break;
      case 0x0c:
        output.append("\\f");
        break;
      case 0x0d:
        output.append("\\r");
        break;
      case '"':
        output.append("\\\"");
        break;
      case '\\':
        output.append("\\\\");
        break;
      default:
        if (byte <= 0x1fU) {
          output.append("\\u00");
          output.push_back(kHex[(byte >> 4U) & 0x0fU]);
          output.push_back(kHex[byte & 0x0fU]);
        } else {
          output.push_back(static_cast<char>(byte));
        }
    }
  }
  output.push_back('"');
}

void append_number(double value, std::string& output) {
  validate_number(value);

  char buffer[128];
  double_conversion::StringBuilder builder(buffer, sizeof(buffer));
  const auto& converter =
      double_conversion::DoubleToStringConverter::EcmaScriptConverter();
  if (!converter.ToShortest(value, &builder)) {
    throw CanonicalValueError("ECMAScript number serialization failed");
  }
  const auto length = static_cast<std::size_t>(builder.position());
  builder.Finalize();
  output.append(buffer, length);
}

void append_canonical_json(const CanonicalValue& value, std::string& output) {
  const auto& storage = value.storage();
  if (std::holds_alternative<std::nullptr_t>(storage)) {
    output.append("null");
    return;
  }
  if (const auto* boolean = std::get_if<bool>(&storage)) {
    output.append(*boolean ? "true" : "false");
    return;
  }
  if (const auto* number = std::get_if<double>(&storage)) {
    append_number(*number, output);
    return;
  }
  if (const auto* string = std::get_if<std::string>(&storage)) {
    append_escaped_string(*string, output);
    return;
  }
  if (const auto* array = std::get_if<CanonicalValue::Array>(&storage)) {
    output.push_back('[');
    for (std::size_t index = 0; index < array->size(); ++index) {
      if (index != 0U) {
        output.push_back(',');
      }
      append_canonical_json((*array)[index], output);
    }
    output.push_back(']');
    return;
  }

  const auto& object = std::get<CanonicalValue::Object>(storage);
  output.push_back('{');
  for (std::size_t index = 0; index < object.size(); ++index) {
    if (index != 0U) {
      output.push_back(',');
    }
    append_escaped_string(object[index].first, output);
    output.push_back(':');
    append_canonical_json(object[index].second, output);
  }
  output.push_back('}');
}

}  // namespace

CanonicalValue::CanonicalValue() noexcept : storage_(nullptr) {}

CanonicalValue::CanonicalValue(std::nullptr_t) noexcept : storage_(nullptr) {}

CanonicalValue::CanonicalValue(bool value) noexcept : storage_(value) {}

CanonicalValue::CanonicalValue(double value) : storage_(value) {
  validate_number(value);
}

CanonicalValue::CanonicalValue(std::string value)
    : storage_(std::move(value)) {
  require_utf8_nfc(std::get<std::string>(storage_), "string value");
}

CanonicalValue::CanonicalValue(const char* value)
    : CanonicalValue(
          value == nullptr
              ? throw CanonicalValueError("string pointer must not be null")
              : std::string(value)) {}

CanonicalValue::CanonicalValue(Array value) : storage_(std::move(value)) {}

CanonicalValue::CanonicalValue(Object value) : storage_(std::move(value)) {
  auto& object = std::get<Object>(storage_);
  std::vector<ObjectMemberSortKey> sort_keys;
  sort_keys.reserve(object.size());
  std::size_t source_index = 0U;
  for (const auto& [name, member_value] : object) {
    (void)member_value;
    require_utf8_nfc(name, "object member name");
    sort_keys.push_back({utf16_units(name), source_index});
    ++source_index;
  }

  std::sort(sort_keys.begin(), sort_keys.end(),
            [](const ObjectMemberSortKey& left,
               const ObjectMemberSortKey& right) {
              const auto name_order = left.utf16_name <=> right.utf16_name;
              if (name_order != 0) {
                return name_order < 0;
              }
              return left.source_index < right.source_index;
            });

  Object sorted_object;
  sorted_object.reserve(object.size());
  for (const auto& sort_key : sort_keys) {
    sorted_object.push_back(std::move(object[sort_key.source_index]));
  }
  object = std::move(sorted_object);

  for (std::size_t index = 1U; index < object.size(); ++index) {
    if (object[index - 1U].first == object[index].first) {
      throw CanonicalValueError("duplicate object member name: " +
                                object[index].first);
    }
  }
}

const CanonicalValue::Storage& CanonicalValue::storage() const noexcept {
  return storage_;
}

bool CanonicalValue::is_object() const noexcept {
  return std::holds_alternative<Object>(storage_);
}

const CanonicalValue::Object& CanonicalValue::as_object() const {
  if (!is_object()) {
    throw CanonicalValueError("canonical value is not an object");
  }
  return std::get<Object>(storage_);
}

const CanonicalValue* CanonicalValue::find_object_member(
    std::string_view name) const noexcept {
  if (!is_object()) {
    return nullptr;
  }
  const auto& object = std::get<Object>(storage_);
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  return found == object.end() ? nullptr : &found->second;
}

bool is_utf8_nfc(std::string_view text) {
  return inspect_utf8_nfc(text).is_nfc;
}

void require_utf8_nfc(std::string_view text, std::string_view context) {
  const NfcInspection result = inspect_utf8_nfc(text);
  if (!result.is_nfc) {
    std::ostringstream message;
    message << context << ": " << result.error;
    throw CanonicalValueError(message.str());
  }
}

std::string canonical_json(const CanonicalValue& value) {
  std::string output;
  append_canonical_json(value, output);
  return output;
}

}  // namespace manyforge::content_identity
