#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::content_identity {

class CanonicalValueError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Immutable JSON-data-model value used by the qualified RFC 8785 codec.
///
/// Constructors reject invalid UTF-8, non-NFC strings, duplicate object
/// names, non-finite numbers, and negative zero. Object members are stored in
/// RFC 8785 UTF-16 order. Schema/DTO owners remain responsible for closing
/// record shapes, classifying arrays, enforcing safe-integer bounds, and
/// materializing required defaults before constructing this value.
class CanonicalValue final {
 public:
  using Array = std::vector<CanonicalValue>;
  using Object = std::vector<std::pair<std::string, CanonicalValue>>;
  using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

  CanonicalValue() noexcept;
  CanonicalValue(std::nullptr_t) noexcept;
  explicit CanonicalValue(bool value) noexcept;
  explicit CanonicalValue(double value);
  explicit CanonicalValue(std::string value);
  explicit CanonicalValue(const char *value);
  explicit CanonicalValue(Array value);
  explicit CanonicalValue(Object value);

  [[nodiscard]] const Storage &storage() const noexcept;
  [[nodiscard]] bool is_null() const noexcept;
  [[nodiscard]] bool is_bool() const noexcept;
  [[nodiscard]] bool is_number() const noexcept;
  [[nodiscard]] bool is_string() const noexcept;
  [[nodiscard]] bool is_array() const noexcept;
  [[nodiscard]] bool is_object() const noexcept;
  [[nodiscard]] std::nullptr_t as_null() const;
  [[nodiscard]] bool as_bool() const;
  [[nodiscard]] double as_number() const;
  [[nodiscard]] const std::string &as_string() const;
  [[nodiscard]] const Array &as_array() const;
  [[nodiscard]] const Object &as_object() const;
  [[nodiscard]] const CanonicalValue *find_object_member(std::string_view name) const noexcept;

  friend bool operator==(const CanonicalValue &, const CanonicalValue &) = default;

 private:
  Storage storage_;
};

inline constexpr std::size_t kMaximumCanonicalJsonBytes = 16U * 1024U * 1024U;
// Explicit legacy consumers may read 64 nested containers plus their scalar.
// The default stays narrow; widening this ceiling never widens a caller's DTO.
inline constexpr std::size_t kMaximumCanonicalJsonDepth = 65U;
inline constexpr std::size_t kMaximumCanonicalJsonNodes = 1024U * 1024U;

/// Caller-selected limits may narrow, but never exceed, the fixed library
/// ceiling. Depth counts the root value as one node level; nodes count every
/// scalar, array, and object value (object names are covered by the byte
/// bound).
struct CanonicalJsonParseLimits final {
  std::size_t maximum_bytes{kMaximumCanonicalJsonBytes};
  std::size_t maximum_depth{32U};
  std::size_t maximum_nodes{kMaximumCanonicalJsonNodes};

  friend bool operator==(const CanonicalJsonParseLimits &,
                         const CanonicalJsonParseLimits &) = default;
};

/// Returns true only for valid UTF-8 already encoded in Unicode NFC.
[[nodiscard]] bool is_utf8_nfc(std::string_view text);
/// Returns true only for valid NFC UTF-8 with at most `max_code_points`
/// Unicode scalars and no whitespace, Cc, NUL, or Cf scalar. This is the
/// shared source-symbol grammar used by portable URDF/SRDF-keyed maps.
[[nodiscard]] bool is_source_symbol(std::string_view text, std::size_t max_code_points = 1024U);

/// Rejects invalid UTF-8 and valid-but-non-NFC text without modifying it.
void require_utf8_nfc(std::string_view text, std::string_view context);

/// Serializes a validated value using RFC 8785 JSON Canonicalization Scheme.
[[nodiscard]] std::string canonical_json(const CanonicalValue &value);

/// Exact nonnegative int64 scalar. Preserve legacy safe-number bytes; use
/// canonical decimal text above the I-JSON integer limit. The decoder also
/// accepts canonical decimal text at small values for string-only transports.
/// This is a scalar codec, not a clock, duration policy, or identity allocator.
[[nodiscard]] CanonicalValue nonnegative_int64_value(std::int64_t value);
[[nodiscard]] std::int64_t parse_nonnegative_int64(const CanonicalValue &value);

/// Parses exactly one already-canonical RFC 8785 JSON value under fixed hard
/// bounds. It rejects any valid JSON spelling whose reserialization differs
/// byte-for-byte, as well as malformed UTF-8/escapes, duplicate names,
/// non-finite/negative-zero numbers, excess depth/nodes/bytes, or trailing
/// data. Schema owners remain responsible for closing the decoded shape.
[[nodiscard]] CanonicalValue parse_canonical_json(std::string_view input,
                                                  CanonicalJsonParseLimits limits = {});

}  // namespace manyforge::content_identity
