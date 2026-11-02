#include "manyforge_content_identity/canonical_value.hpp"

#include <double-conversion/double-conversion.h>
#include <utf8proc.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <sstream>

namespace manyforge::content_identity {

CanonicalValue nonnegative_int64_value(const std::int64_t value) {
  if (value < 0) {
    throw CanonicalValueError("nonnegative int64 required");
  }
  return value <= 9'007'199'254'740'991LL
             ? CanonicalValue(static_cast<double>(value))
             : CanonicalValue(std::to_string(value));
}

std::int64_t parse_nonnegative_int64(const CanonicalValue &value) {
  if (value.is_number()) {
    const auto number = value.as_number();
    if (number >= 0 && number <= 9'007'199'254'740'991.0 &&
        std::trunc(number) == number) {
      return static_cast<std::int64_t>(number);
    }
  } else if (value.is_string()) {
    const auto &text = value.as_string();
    if (!text.empty() && text.size() <= 19U &&
        (text.size() == 1U || text.front() != '0') &&
        std::all_of(text.begin(), text.end(), [](const char digit) {
          return digit >= '0' && digit <= '9';
        })) {
      std::int64_t result{};
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
      if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
        return result;
      }
    }
  }
  throw CanonicalValueError("exact nonnegative int64 scalar required");
}

namespace {

using Utf8procBuffer = std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)>;

struct NfcInspection final {
  bool is_nfc;
  std::string error;
};

[[nodiscard]] NfcInspection inspect_utf8_nfc(std::string_view text) {
  if (std::all_of(text.begin(), text.end(), [](unsigned char byte) { return byte <= 0x7fU; })) {
    return {true, {}};
  }

  static constexpr utf8proc_uint8_t kEmptyInput = 0;
  utf8proc_uint8_t *raw_normalized = nullptr;
  const auto *input =
      text.empty() ? &kEmptyInput : reinterpret_cast<const utf8proc_uint8_t *>(text.data());
  const auto options = static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE);
  const utf8proc_ssize_t normalized_length =
      utf8proc_map(input, static_cast<utf8proc_ssize_t>(text.size()), &raw_normalized, options);
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
      (normalized_size == 0U || std::memcmp(normalized.get(), text.data(), normalized_size) == 0);
  return {unchanged, unchanged ? std::string{} : "text is not Unicode NFC"};
}

void validate_number(double value) {
  if (!std::isfinite(value)) {
    throw CanonicalValueError("I-JSON number must be finite");
  }
  if (value == 0.0 && std::signbit(value)) {
    throw CanonicalValueError("negative zero is not accepted by ManyForge");
  }
}

[[nodiscard]] std::vector<std::uint16_t> utf16_units(std::string_view utf8) {
  std::vector<std::uint16_t> result;
  result.reserve(utf8.size());

  utf8proc_ssize_t offset = 0;
  while (offset < static_cast<utf8proc_ssize_t>(utf8.size())) {
    utf8proc_int32_t code_point = 0;
    const utf8proc_ssize_t consumed =
        utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(utf8.data()) + offset,
                         static_cast<utf8proc_ssize_t>(utf8.size()) - offset, &code_point);
    if (consumed <= 0) {
      throw CanonicalValueError("invalid UTF-8 reached RFC 8785 object sorting");
    }
    offset += consumed;

    const auto scalar = static_cast<std::uint32_t>(code_point);
    if (scalar <= 0xffffU) {
      if (scalar >= 0xd800U && scalar <= 0xdfffU) {
        throw CanonicalValueError("Unicode surrogate reached RFC 8785 object sorting");
      }
      result.push_back(static_cast<std::uint16_t>(scalar));
      continue;
    }
    if (scalar > 0x10ffffU) {
      throw CanonicalValueError("out-of-range Unicode scalar reached RFC 8785 object sorting");
    }

    const auto supplementary = scalar - 0x10000U;
    result.push_back(static_cast<std::uint16_t>(0xd800U + (supplementary >> 10U)));
    result.push_back(static_cast<std::uint16_t>(0xdc00U + (supplementary & 0x3ffU)));
  }
  return result;
}

struct ObjectMemberSortKey final {
  std::vector<std::uint16_t> utf16_name;
  std::size_t source_index;
};

void append_escaped_string(std::string_view value, std::string &output) {
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

void append_number(double value, std::string &output) {
  validate_number(value);

  char buffer[128];
  double_conversion::StringBuilder builder(buffer, sizeof(buffer));
  const auto &converter = double_conversion::DoubleToStringConverter::EcmaScriptConverter();
  if (!converter.ToShortest(value, &builder)) {
    throw CanonicalValueError("ECMAScript number serialization failed");
  }
  const auto length = static_cast<std::size_t>(builder.position());
  builder.Finalize();
  output.append(buffer, length);
}

void append_canonical_json(const CanonicalValue &value, std::string &output) {
  const auto &storage = value.storage();
  if (std::holds_alternative<std::nullptr_t>(storage)) {
    output.append("null");
    return;
  }
  if (const auto *boolean = std::get_if<bool>(&storage)) {
    output.append(*boolean ? "true" : "false");
    return;
  }
  if (const auto *number = std::get_if<double>(&storage)) {
    append_number(*number, output);
    return;
  }
  if (const auto *string = std::get_if<std::string>(&storage)) {
    append_escaped_string(*string, output);
    return;
  }
  if (const auto *array = std::get_if<CanonicalValue::Array>(&storage)) {
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

  const auto &object = std::get<CanonicalValue::Object>(storage);
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

class CanonicalJsonParser final {
 public:
  CanonicalJsonParser(std::string_view input, CanonicalJsonParseLimits limits)
      : input_(input), limits_(limits) {}

  [[nodiscard]] CanonicalValue parse() {
    CanonicalValue value = parse_value(1U);
    if (position_ != input_.size()) {
      fail("trailing bytes after canonical JSON value");
    }
    return value;
  }

 private:
  [[noreturn]] static void fail(std::string_view reason) {
    throw CanonicalValueError(std::string(reason));
  }

  [[nodiscard]] bool at_end() const noexcept { return position_ == input_.size(); }

  [[nodiscard]] char current() const {
    if (at_end()) {
      fail("unexpected end of canonical JSON");
    }
    return input_[position_];
  }

  [[nodiscard]] bool consume(char byte) noexcept {
    if (!at_end() && input_[position_] == byte) {
      ++position_;
      return true;
    }
    return false;
  }

  void require(char byte, std::string_view reason) {
    if (!consume(byte)) {
      fail(reason);
    }
  }

  void consume_literal(std::string_view literal) {
    if (input_.substr(position_, literal.size()) != literal) {
      fail("invalid canonical JSON literal");
    }
    position_ += literal.size();
  }

  [[nodiscard]] CanonicalValue parse_value(std::size_t depth) {
    if (depth > limits_.maximum_depth) {
      fail("canonical JSON depth limit exceeded");
    }
    if (++nodes_ > limits_.maximum_nodes) {
      fail("canonical JSON node limit exceeded");
    }
    switch (current()) {
      case 'n':
        consume_literal("null");
        return CanonicalValue(nullptr);
      case 't':
        consume_literal("true");
        return CanonicalValue(true);
      case 'f':
        consume_literal("false");
        return CanonicalValue(false);
      case '"':
        return CanonicalValue(parse_string());
      case '[':
        return parse_array(depth);
      case '{':
        return parse_object(depth);
      default:
        if (current() == '-' || (current() >= '0' && current() <= '9')) {
          return parse_number();
        }
        fail("invalid byte at canonical JSON value boundary");
    }
  }

  [[nodiscard]] CanonicalValue parse_array(std::size_t depth) {
    require('[', "canonical JSON array must begin with '['");
    CanonicalValue::Array values;
    if (consume(']')) {
      return CanonicalValue(std::move(values));
    }
    while (true) {
      values.push_back(parse_value(depth + 1U));
      if (consume(']')) {
        return CanonicalValue(std::move(values));
      }
      require(',', "canonical JSON array requires ',' or ']'");
    }
  }

  [[nodiscard]] CanonicalValue parse_object(std::size_t depth) {
    require('{', "canonical JSON object must begin with '{'");
    CanonicalValue::Object members;
    if (consume('}')) {
      return CanonicalValue(std::move(members));
    }
    while (true) {
      if (current() != '"') {
        fail("canonical JSON object member name must be a string");
      }
      std::string name = parse_string();
      require(':', "canonical JSON object member requires ':'");
      members.emplace_back(std::move(name), parse_value(depth + 1U));
      if (consume('}')) {
        return CanonicalValue(std::move(members));
      }
      require(',', "canonical JSON object requires ',' or '}'");
    }
  }

  [[nodiscard]] static std::uint32_t hex_digit(unsigned char byte) {
    if (byte >= '0' && byte <= '9') {
      return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
      return 10U + byte - 'a';
    }
    if (byte >= 'A' && byte <= 'F') {
      return 10U + byte - 'A';
    }
    fail("invalid hexadecimal digit in JSON Unicode escape");
  }

  [[nodiscard]] std::uint32_t parse_hex_quad() {
    if (input_.size() - position_ < 4U) {
      fail("incomplete JSON Unicode escape");
    }
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
      value = (value << 4U) | hex_digit(static_cast<unsigned char>(input_[position_++]));
    }
    return value;
  }

  static void append_utf8(std::uint32_t scalar, std::string &output) {
    if (scalar <= 0x7fU) {
      output.push_back(static_cast<char>(scalar));
    } else if (scalar <= 0x7ffU) {
      output.push_back(static_cast<char>(0xc0U | (scalar >> 6U)));
      output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    } else if (scalar <= 0xffffU) {
      output.push_back(static_cast<char>(0xe0U | (scalar >> 12U)));
      output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    } else if (scalar <= 0x10ffffU) {
      output.push_back(static_cast<char>(0xf0U | (scalar >> 18U)));
      output.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
      output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    } else {
      fail("out-of-range JSON Unicode scalar");
    }
  }

  void append_unicode_escape(std::string &output) {
    std::uint32_t scalar = parse_hex_quad();
    if (scalar >= 0xd800U && scalar <= 0xdbffU) {
      if (!consume('\\') || !consume('u')) {
        fail("JSON high surrogate requires a low-surrogate escape");
      }
      const std::uint32_t low = parse_hex_quad();
      if (low < 0xdc00U || low > 0xdfffU) {
        fail("JSON high surrogate requires a valid low surrogate");
      }
      scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + (low - 0xdc00U);
    } else if (scalar >= 0xdc00U && scalar <= 0xdfffU) {
      fail("JSON low surrogate cannot appear without a high surrogate");
    }
    append_utf8(scalar, output);
  }

  void append_escape(std::string &output) {
    if (at_end()) {
      fail("incomplete JSON escape");
    }
    const char escape = input_[position_++];
    switch (escape) {
      case '"':
      case '\\':
      case '/':
        output.push_back(escape);
        return;
      case 'b':
        output.push_back('\b');
        return;
      case 'f':
        output.push_back('\f');
        return;
      case 'n':
        output.push_back('\n');
        return;
      case 'r':
        output.push_back('\r');
        return;
      case 't':
        output.push_back('\t');
        return;
      case 'u':
        append_unicode_escape(output);
        return;
      default:
        fail("invalid JSON escape");
    }
  }

  [[nodiscard]] std::string parse_string() {
    require('"', "canonical JSON string must begin with a quote");
    std::string result;
    while (!at_end()) {
      const unsigned char byte = static_cast<unsigned char>(input_[position_++]);
      if (byte == '"') {
        require_utf8_nfc(result, "parsed JSON string");
        return result;
      }
      if (byte == '\\') {
        append_escape(result);
        continue;
      }
      if (byte <= 0x1fU) {
        fail("unescaped control byte in JSON string");
      }
      result.push_back(static_cast<char>(byte));
    }
    fail("unterminated JSON string");
  }

  void consume_digits() noexcept {
    while (!at_end() && input_[position_] >= '0' && input_[position_] <= '9') {
      ++position_;
    }
  }

  [[nodiscard]] CanonicalValue parse_number() {
    const std::size_t begin = position_;
    const bool negative = consume('-');
    (void)negative;
    if (at_end()) {
      fail("incomplete JSON number");
    }
    if (consume('0')) {
      // A following digit remains unconsumed and is rejected at its structural
      // boundary. This keeps the exact JSON leading-zero rule explicit.
    } else {
      if (current() < '1' || current() > '9') {
        fail("invalid JSON integer part");
      }
      consume_digits();
    }
    if (consume('.')) {
      const std::size_t fraction_begin = position_;
      consume_digits();
      if (fraction_begin == position_) {
        fail("JSON fraction requires a digit");
      }
    }
    if (consume('e') || consume('E')) {
      if (!consume('+')) {
        const bool negative_exponent = consume('-');
        (void)negative_exponent;
      }
      const std::size_t exponent_begin = position_;
      consume_digits();
      if (exponent_begin == position_) {
        fail("JSON exponent requires a digit");
      }
    }

    const std::string_view token = input_.substr(begin, position_ - begin);
    if (token.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      fail("JSON number token exceeds parser representation");
    }
    const double invalid = std::numeric_limits<double>::quiet_NaN();
    const double_conversion::StringToDoubleConverter converter(
        double_conversion::StringToDoubleConverter::NO_FLAGS, invalid, invalid, nullptr, nullptr);
    int processed = 0;
    const double value =
        converter.StringToDouble(token.data(), static_cast<int>(token.size()), &processed);
    if (processed != static_cast<int>(token.size()) || !std::isfinite(value)) {
      fail("JSON number is not a finite binary64 value");
    }
    return CanonicalValue(value);
  }

  std::string_view input_;
  CanonicalJsonParseLimits limits_;
  std::size_t position_{};
  std::size_t nodes_{};
};

}  // namespace

CanonicalValue::CanonicalValue() noexcept : storage_(nullptr) {}

CanonicalValue::CanonicalValue(std::nullptr_t) noexcept : storage_(nullptr) {}

CanonicalValue::CanonicalValue(bool value) noexcept : storage_(value) {}

CanonicalValue::CanonicalValue(double value) : storage_(value) { validate_number(value); }

CanonicalValue::CanonicalValue(std::string value) : storage_(std::move(value)) {
  require_utf8_nfc(std::get<std::string>(storage_), "string value");
}

CanonicalValue::CanonicalValue(const char *value)
    : CanonicalValue(value == nullptr ? throw CanonicalValueError("string pointer must not be null")
                                      : std::string(value)) {}

CanonicalValue::CanonicalValue(Array value) : storage_(std::move(value)) {}

CanonicalValue::CanonicalValue(Object value) : storage_(std::move(value)) {
  auto &object = std::get<Object>(storage_);
  std::vector<ObjectMemberSortKey> sort_keys;
  sort_keys.reserve(object.size());
  std::size_t source_index = 0U;
  for (const auto &[name, member_value] : object) {
    (void)member_value;
    require_utf8_nfc(name, "object member name");
    sort_keys.push_back({utf16_units(name), source_index});
    ++source_index;
  }

  std::sort(sort_keys.begin(), sort_keys.end(),
            [](const ObjectMemberSortKey &left, const ObjectMemberSortKey &right) {
              const auto name_order = left.utf16_name <=> right.utf16_name;
              if (name_order != 0) {
                return name_order < 0;
              }
              return left.source_index < right.source_index;
            });

  Object sorted_object;
  sorted_object.reserve(object.size());
  for (const auto &sort_key : sort_keys) {
    sorted_object.push_back(std::move(object[sort_key.source_index]));
  }
  object = std::move(sorted_object);

  for (std::size_t index = 1U; index < object.size(); ++index) {
    if (object[index - 1U].first == object[index].first) {
      throw CanonicalValueError("duplicate object member name: " + object[index].first);
    }
  }
}

const CanonicalValue::Storage &CanonicalValue::storage() const noexcept { return storage_; }

bool CanonicalValue::is_null() const noexcept {
  return std::holds_alternative<std::nullptr_t>(storage_);
}

bool CanonicalValue::is_bool() const noexcept { return std::holds_alternative<bool>(storage_); }

bool CanonicalValue::is_number() const noexcept { return std::holds_alternative<double>(storage_); }

bool CanonicalValue::is_string() const noexcept {
  return std::holds_alternative<std::string>(storage_);
}

bool CanonicalValue::is_array() const noexcept { return std::holds_alternative<Array>(storage_); }

bool CanonicalValue::is_object() const noexcept { return std::holds_alternative<Object>(storage_); }

std::nullptr_t CanonicalValue::as_null() const {
  if (!is_null()) {
    throw CanonicalValueError("canonical value is not null");
  }
  return nullptr;
}

bool CanonicalValue::as_bool() const {
  if (!is_bool()) {
    throw CanonicalValueError("canonical value is not a boolean");
  }
  return std::get<bool>(storage_);
}

double CanonicalValue::as_number() const {
  if (!is_number()) {
    throw CanonicalValueError("canonical value is not a number");
  }
  return std::get<double>(storage_);
}

const std::string &CanonicalValue::as_string() const {
  if (!is_string()) {
    throw CanonicalValueError("canonical value is not a string");
  }
  return std::get<std::string>(storage_);
}

const CanonicalValue::Array &CanonicalValue::as_array() const {
  if (!is_array()) {
    throw CanonicalValueError("canonical value is not an array");
  }
  return std::get<Array>(storage_);
}

const CanonicalValue::Object &CanonicalValue::as_object() const {
  if (!is_object()) {
    throw CanonicalValueError("canonical value is not an object");
  }
  return std::get<Object>(storage_);
}

const CanonicalValue *CanonicalValue::find_object_member(std::string_view name) const noexcept {
  if (!is_object()) {
    return nullptr;
  }
  const auto &object = std::get<Object>(storage_);
  const auto found = std::find_if(object.begin(), object.end(),
                                  [name](const auto &member) { return member.first == name; });
  return found == object.end() ? nullptr : &found->second;
}

bool is_utf8_nfc(std::string_view text) { return inspect_utf8_nfc(text).is_nfc; }

bool is_source_symbol(std::string_view text, std::size_t max_code_points) {
  if (text.empty() || max_code_points == 0U || !is_utf8_nfc(text)) {
    return false;
  }
  utf8proc_ssize_t offset = 0;
  std::size_t code_points = 0U;
  while (offset < static_cast<utf8proc_ssize_t>(text.size())) {
    utf8proc_int32_t code_point = 0;
    const utf8proc_ssize_t consumed =
        utf8proc_iterate(reinterpret_cast<const utf8proc_uint8_t *>(text.data()) + offset,
                         static_cast<utf8proc_ssize_t>(text.size()) - offset, &code_point);
    if (consumed <= 0 || ++code_points > max_code_points) {
      return false;
    }
    offset += consumed;
    const auto category = utf8proc_category(code_point);
    if (code_point == 0 || category == UTF8PROC_CATEGORY_ZS || category == UTF8PROC_CATEGORY_ZL ||
        category == UTF8PROC_CATEGORY_ZP || category == UTF8PROC_CATEGORY_CC ||
        category == UTF8PROC_CATEGORY_CF) {
      return false;
    }
  }
  return true;
}

void require_utf8_nfc(std::string_view text, std::string_view context) {
  const NfcInspection result = inspect_utf8_nfc(text);
  if (!result.is_nfc) {
    std::ostringstream message;
    message << context << ": " << result.error;
    throw CanonicalValueError(message.str());
  }
}

std::string canonical_json(const CanonicalValue &value) {
  std::string output;
  append_canonical_json(value, output);
  return output;
}

CanonicalValue parse_canonical_json(std::string_view input, CanonicalJsonParseLimits limits) {
  if (limits.maximum_bytes == 0U || limits.maximum_bytes > kMaximumCanonicalJsonBytes ||
      limits.maximum_depth == 0U || limits.maximum_depth > kMaximumCanonicalJsonDepth ||
      limits.maximum_nodes == 0U || limits.maximum_nodes > kMaximumCanonicalJsonNodes) {
    throw CanonicalValueError("canonical JSON parse limits exceed the fixed library bounds");
  }
  if (input.empty()) {
    throw CanonicalValueError("canonical JSON input must contain exactly one value");
  }
  if (input.size() > limits.maximum_bytes) {
    throw CanonicalValueError("canonical JSON byte limit exceeded");
  }

  CanonicalValue value = CanonicalJsonParser(input, limits).parse();
  if (canonical_json(value) != input) {
    throw CanonicalValueError("JSON input is valid but not in canonical byte form");
  }
  return value;
}

}  // namespace manyforge::content_identity
