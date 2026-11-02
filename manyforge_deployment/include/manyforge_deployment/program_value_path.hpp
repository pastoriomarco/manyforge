#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

/// Program value paths: the keys of a node's `value_sources`, naming one value
/// inside the node's parameters (for example `goal.position_m[2]`). There is
/// exactly one spelling per path, shared with the Python Program validator and
/// the Composer editor through
/// resources/expression/value_path_conformance.v1.yaml.
namespace manyforge::deployment {

inline constexpr std::size_t kMaxValuePathLength = 1024U;
inline constexpr std::size_t kMaxValuePathSegments = 33U;
inline constexpr std::size_t kMaxValuePathNameLength = 256U;
inline constexpr std::size_t kMaxValuePathIndexDigits = 9U;

/// A field name or a list index.
using ValuePathSegment = std::variant<std::string, std::size_t>;
using ValuePath = std::vector<ValuePathSegment>;

class ValuePathError final : public std::invalid_argument {
public:
  ValuePathError(std::string code, const std::string &message)
      : std::invalid_argument(message), code_(std::move(code)) {}

  /// path_empty, path_too_long, path_too_deep, path_name_invalid,
  /// path_index_invalid or path_syntax_invalid.
  [[nodiscard]] const std::string &code() const noexcept { return code_; }

private:
  std::string code_;
};

/// Parse the one canonical spelling; throws ValuePathError otherwise.
[[nodiscard]] ValuePath parse_value_path(std::string_view text);

/// Spell a path canonically; throws ValuePathError for a malformed path.
[[nodiscard]] std::string format_value_path(const ValuePath &path);

} // namespace manyforge::deployment
