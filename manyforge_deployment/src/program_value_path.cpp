#include "manyforge_deployment/program_value_path.hpp"

#include <string>

namespace manyforge::deployment {
namespace {

[[nodiscard]] bool name_start(char value) { return value >= 'a' && value <= 'z'; }

[[nodiscard]] bool name_char(char value) {
  return name_start(value) || (value >= '0' && value <= '9') || value == '_' ||
         value == '-';
}

[[nodiscard]] bool digit(char value) { return value >= '0' && value <= '9'; }

} // namespace

ValuePath parse_value_path(std::string_view text) {
  const std::string quoted = "value path '" + std::string(text) + "'";
  if (text.empty()) {
    throw ValuePathError("path_empty", "value path is empty");
  }
  if (text.size() > kMaxValuePathLength) {
    throw ValuePathError("path_too_long",
                         "value path is longer than " +
                             std::to_string(kMaxValuePathLength) +
                             " characters");
  }
  ValuePath path;
  std::size_t position = 0U;
  const auto append = [&](ValuePathSegment segment) {
    if (path.size() == kMaxValuePathSegments) {
      throw ValuePathError("path_too_deep",
                           quoted + " has more than " +
                               std::to_string(kMaxValuePathSegments) +
                               " segments");
    }
    path.push_back(std::move(segment));
  };
  const auto name = [&]() {
    const std::size_t start = position;
    if (position >= text.size() || !name_start(text[position])) {
      throw ValuePathError("path_name_invalid",
                           quoted + " needs a lower-case name at character " +
                               std::to_string(start));
    }
    while (position < text.size() && name_char(text[position])) {
      ++position;
    }
    if (position - start > kMaxValuePathNameLength) {
      throw ValuePathError("path_name_invalid",
                           quoted + " has a name longer than " +
                               std::to_string(kMaxValuePathNameLength));
    }
    return std::string(text.substr(start, position - start));
  };
  const auto index = [&]() {
    const std::size_t start = position;
    while (position < text.size() && digit(text[position])) {
      ++position;
    }
    const std::string_view digits = text.substr(start, position - start);
    if (digits.empty() || (digits.size() > 1U && digits.front() == '0') ||
        digits.size() > kMaxValuePathIndexDigits || position >= text.size() ||
        text[position] != ']') {
      throw ValuePathError("path_index_invalid",
                           quoted +
                               " needs a decimal index without leading zeros "
                               "at character " +
                               std::to_string(start));
    }
    ++position;
    std::size_t value = 0U;
    for (const char item : digits) {
      value = value * 10U + static_cast<std::size_t>(item - '0');
    }
    return value;
  };
  append(name());
  while (position < text.size()) {
    const char separator = text[position++];
    if (separator == '.') {
      append(name());
    } else if (separator == '[') {
      append(index());
    } else {
      throw ValuePathError("path_syntax_invalid",
                           quoted + " has an unexpected character at " +
                               std::to_string(position - 1U));
    }
  }
  return path;
}

std::string format_value_path(const ValuePath &path) {
  if (path.empty() || !std::holds_alternative<std::string>(path.front())) {
    throw ValuePathError("path_name_invalid",
                         "a value path starts with a name");
  }
  std::string text = std::get<std::string>(path.front());
  for (std::size_t item = 1U; item < path.size(); ++item) {
    if (const auto *field = std::get_if<std::string>(&path[item])) {
      text += '.';
      text += *field;
    } else {
      text += '[';
      text += std::to_string(std::get<std::size_t>(path[item]));
      text += ']';
    }
  }
  if (parse_value_path(text) != path) {
    throw ValuePathError("path_syntax_invalid", "segments are not a value path");
  }
  return text;
}

} // namespace manyforge::deployment
