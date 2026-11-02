#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// The one Program expression engine (EXPRESSION_LANGUAGE_REFERENCE.md).
///
/// parse() turns text into a syntax tree, check() binds its `param.`/`key.`
/// names against declared types and a destination type, and evaluate() computes
/// the value from one snapshot. Every failure is a Diagnostic whose position
/// and length count Unicode code points of the expression text.
namespace manyforge::deployment::expression {

/// Approved input bounds; text length counts Unicode code points.
inline constexpr std::size_t kMaxTextLength = 1024U;
inline constexpr std::size_t kMaxNestingDepth = 64U;
inline constexpr std::size_t kMaxTokens = 512U;
inline constexpr std::size_t kMaxSyntaxNodes = 256U;
inline constexpr std::size_t kMaxEvaluationSteps = 256U;

/// A syntax tree never exceeds kMaxSyntaxNodes and evaluation visits each node
/// at most once, so the step bound is a second, independent guard. Callers may
/// only narrow the approved bounds (tests use this to reach every bound).
struct Limits final {
  std::size_t text_length = kMaxTextLength;
  std::size_t nesting_depth = kMaxNestingDepth;
  std::size_t tokens = kMaxTokens;
  std::size_t syntax_nodes = kMaxSyntaxNodes;
  std::size_t evaluation_steps = kMaxEvaluationSteps;
};

enum class ValueKind : std::uint8_t {
  kBool,
  kInt64,
  kFloat64,
  kString,
  kPose,
  kRecord,
};

struct RecordFieldType;

/// Declared type of a parameter or key. Only kRecord carries fields.
struct ValueType final {
  ValueKind kind = ValueKind::kBool;
  std::vector<RecordFieldType> fields;
};

struct RecordFieldType final {
  std::string name;
  ValueType type;
};

struct RpyDeg final {
  std::array<double, 3> values{};
};

struct Xyzw final {
  std::array<double, 4> values{};
};

/// A pose in its own frame; exactly one orientation form is stored.
struct Pose final {
  std::array<double, 3> position_m{};
  std::variant<RpyDeg, Xyzw> orientation;
};

struct RecordFieldValue;
using Record = std::vector<RecordFieldValue>;

struct Value final {
  std::variant<bool, std::int64_t, double, std::string, Pose, Record> data;
};

struct RecordFieldValue final {
  std::string name;
  Value value;
};

enum class Source : std::uint8_t { kParameter, kKey };

/// A declared parameter or key name, without the `param.`/`key.` prefix.
struct Reference final {
  Source source = Source::kParameter;
  std::string name;

  friend auto operator<=>(const Reference &, const Reference &) = default;
};

struct TypeEnvironment final {
  std::map<std::string, ValueType, std::less<>> parameters;
  std::map<std::string, ValueType, std::less<>> keys;
};

/// One consistent snapshot; a key without a value yet is simply absent.
struct ValueSnapshot final {
  std::map<std::string, Value, std::less<>> parameters;
  std::map<std::string, Value, std::less<>> keys;
};

struct Diagnostic final {
  std::string code;
  std::string message;
  std::size_t position = 0;
  std::size_t length = 0;
  /// The `param.`/`key.` name or function the diagnostic is about, if any.
  std::string name;
};

namespace detail {
struct ParsedTree;
struct CheckedTree;
} // namespace detail

class ParsedExpression;
class CheckedExpression;

/// Tokenize and parse; every limit is enforced before work grows past it.
/// Throws std::invalid_argument when `limits` widens an approved bound.
[[nodiscard]] std::variant<ParsedExpression, Diagnostic>
parse(std::string_view text, const Limits &limits = {});

/// Resolve names (longest declared name first, then fields), type-check
/// against `destination` and evaluate constant parts. Diagnostics are sorted
/// by position. Throws std::invalid_argument for a destination that is not
/// bool, int64, float64 or string, or for a malformed record type.
[[nodiscard]] std::variant<CheckedExpression, std::vector<Diagnostic>>
check(const ParsedExpression &parsed, const TypeEnvironment &environment,
      ValueKind destination);

/// Evaluate from one snapshot. The value has the destination kind; an int64
/// result for a float64 destination is converted.
[[nodiscard]] std::variant<Value, Diagnostic>
evaluate(const CheckedExpression &checked, const ValueSnapshot &values);

class ParsedExpression final {
public:
  [[nodiscard]] std::string_view text() const noexcept;

private:
  explicit ParsedExpression(std::shared_ptr<const detail::ParsedTree> tree);

  std::shared_ptr<const detail::ParsedTree> tree_;

  friend std::variant<ParsedExpression, Diagnostic> parse(std::string_view,
                                                          const Limits &);
  friend std::variant<CheckedExpression, std::vector<Diagnostic>>
  check(const ParsedExpression &, const TypeEnvironment &, ValueKind);
};

class CheckedExpression final {
public:
  [[nodiscard]] std::string_view text() const noexcept;
  /// The destination kind; evaluate() always returns a value of this kind.
  [[nodiscard]] ValueKind result_kind() const noexcept;
  /// Every parameter and key the expression reads, with its declared type.
  [[nodiscard]] const std::map<Reference, ValueType> &
  references() const noexcept;

private:
  explicit CheckedExpression(std::shared_ptr<const detail::CheckedTree> tree);

  std::shared_ptr<const detail::CheckedTree> tree_;

  friend std::variant<CheckedExpression, std::vector<Diagnostic>>
  check(const ParsedExpression &, const TypeEnvironment &, ValueKind);
  friend std::variant<Value, Diagnostic> evaluate(const CheckedExpression &,
                                                  const ValueSnapshot &);
};

} // namespace manyforge::deployment::expression
