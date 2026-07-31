#pragma once

#include <cstddef>
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
  using Storage =
      std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

  CanonicalValue() noexcept;
  CanonicalValue(std::nullptr_t) noexcept;
  explicit CanonicalValue(bool value) noexcept;
  explicit CanonicalValue(double value);
  explicit CanonicalValue(std::string value);
  explicit CanonicalValue(const char* value);
  explicit CanonicalValue(Array value);
  explicit CanonicalValue(Object value);

  [[nodiscard]] const Storage& storage() const noexcept;
  [[nodiscard]] bool is_object() const noexcept;
  [[nodiscard]] const Object& as_object() const;
  [[nodiscard]] const CanonicalValue* find_object_member(
      std::string_view name) const noexcept;

  friend bool operator==(const CanonicalValue&, const CanonicalValue&) =
      default;

 private:
  Storage storage_;
};

/// Returns true only for valid UTF-8 already encoded in Unicode NFC.
[[nodiscard]] bool is_utf8_nfc(std::string_view text);

/// Rejects invalid UTF-8 and valid-but-non-NFC text without modifying it.
void require_utf8_nfc(std::string_view text, std::string_view context);

/// Serializes a validated value using RFC 8785 JSON Canonicalization Scheme.
[[nodiscard]] std::string canonical_json(const CanonicalValue& value);

}  // namespace manyforge::content_identity
