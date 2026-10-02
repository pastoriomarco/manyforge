#include "manyforge_deployment/expression.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment::expression {

namespace detail {

enum class NodeKind : std::uint8_t {
  kLiteral,
  kName,
  kUnary,
  kBinary,
  kConditional,
  kCall,
};

enum class Operator : std::uint8_t {
  kNegate,
  kNot,
  kMultiply,
  kDivide,
  kRemainder,
  kAdd,
  kSubtract,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
  kEqual,
  kNotEqual,
  kAnd,
  kOr,
};

enum class Function : std::uint8_t {
  kAbs,
  kMin,
  kMax,
  kClamp,
  kFloor,
  kCeil,
  kRound,
  kSqrt,
  kSin,
  kCos,
  kTan,
  kAtan2,
  kDeg,
  kRad,
};

enum class PoseField : std::uint8_t {
  kNone,
  kX,
  kY,
  kZ,
  kRoll,
  kPitch,
  kYaw,
  kQx,
  kQy,
  kQz,
  kQw,
};

using Scalar = std::variant<bool, std::int64_t, double, std::string>;

struct Node final {
  NodeKind kind = NodeKind::kLiteral;
  // Byte offsets into the text.
  std::size_t begin = 0;
  std::size_t end = 0;
  Operator op = Operator::kNegate;
  Function function = Function::kAbs;
  Scalar literal;
  Source source = Source::kParameter;
  std::vector<std::string> segments;
  std::vector<std::size_t> children;
};

struct ParsedTree final {
  std::string text;
  Limits limits;
  std::vector<Node> nodes;
  std::size_t root = 0;
};

struct ResolvedName final {
  Reference reference;
  std::vector<std::string> record_path;
  PoseField pose_field = PoseField::kNone;
};

struct CheckedTree final {
  std::shared_ptr<const ParsedTree> parsed;
  std::vector<ValueKind> kinds;
  std::vector<ResolvedName> names;
  ValueKind destination = ValueKind::kBool;
  std::map<Reference, ValueType> references;
};

} // namespace detail

namespace {

using detail::CheckedTree;
using detail::Function;
using detail::Node;
using detail::NodeKind;
using detail::Operator;
using detail::ParsedTree;
using detail::PoseField;
using detail::ResolvedName;
using detail::Scalar;

// Diagnostics unwind the recursive parser/checker/evaluator to the API
// boundary, where they become return values.
struct DiagnosticError final {
  Diagnostic diagnostic;
};

std::size_t code_points(std::string_view text, std::size_t end) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < end && index < text.size(); ++index) {
    if ((static_cast<unsigned char>(text[index]) & 0xC0U) != 0x80U) {
      ++count;
    }
  }
  return count;
}

Diagnostic make_diagnostic(std::string_view text, std::string code,
                           std::string message, std::size_t begin,
                           std::size_t end, std::string name = {}) {
  const auto position = code_points(text, begin);
  return Diagnostic{std::move(code), std::move(message), position,
                    code_points(text, end) - position, std::move(name)};
}

[[noreturn]] void fail(std::string_view text, std::string code,
                       std::string message, std::size_t begin, std::size_t end,
                       std::string name = {}) {
  throw DiagnosticError{make_diagnostic(
      text, std::move(code), std::move(message), begin, end, std::move(name))};
}

std::optional<std::size_t> first_invalid_utf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80U) {
      ++index;
      continue;
    }
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if ((lead & 0xE0U) == 0xC0U) {
      length = 2;
      code_point = lead & 0x1FU;
      minimum = 0x80U;
    } else if ((lead & 0xF0U) == 0xE0U) {
      length = 3;
      code_point = lead & 0x0FU;
      minimum = 0x800U;
    } else if ((lead & 0xF8U) == 0xF0U) {
      length = 4;
      code_point = lead & 0x07U;
      minimum = 0x10000U;
    } else {
      return index;
    }
    if (length > text.size() - index) {
      return index;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto next = static_cast<unsigned char>(text[index + offset]);
      if ((next & 0xC0U) != 0x80U) {
        return index;
      }
      code_point = (code_point << 6U) | (next & 0x3FU);
    }
    if (code_point < minimum || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
      return index;
    }
    index += length;
  }
  return std::nullopt;
}

// End of the (valid UTF-8) character starting at `index`.
std::size_t character_end(std::string_view text, std::size_t index) {
  ++index;
  while (index < text.size() &&
         (static_cast<unsigned char>(text[index]) & 0xC0U) == 0x80U) {
    ++index;
  }
  return index;
}

bool is_digit(char value) { return value >= '0' && value <= '9'; }

bool is_name_start(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         value == '_';
}

bool is_name_char(char value) {
  return is_name_start(value) || is_digit(value);
}

std::string_view kind_name(ValueKind kind) {
  switch (kind) {
  case ValueKind::kBool:
    return "bool";
  case ValueKind::kInt64:
    return "int64";
  case ValueKind::kFloat64:
    return "float64";
  case ValueKind::kString:
    return "string";
  case ValueKind::kPose:
    return "pose";
  case ValueKind::kRecord:
    return "record";
  }
  return "unknown";
}

std::string prefixed(const Reference &reference) {
  return (reference.source == Source::kParameter ? "param." : "key.") +
         reference.name;
}

// ---------------------------------------------------------------- tokenizer

enum class TokenKind : std::uint8_t {
  kInteger,
  kFloat,
  kString,
  kName,
  kLeftParen,
  kRightParen,
  kComma,
  kQuestion,
  kColon,
  kPlus,
  kMinus,
  kStar,
  kSlash,
  kPercent,
  kBang,
  kLess,
  kLessEqual,
  kGreater,
  kGreaterEqual,
  kEqual,
  kNotEqual,
  kAnd,
  kOr,
  kEnd,
};

struct Token final {
  TokenKind kind = TokenKind::kEnd;
  std::size_t begin = 0;
  std::size_t end = 0;
};

std::size_t lex_number(std::string_view text, std::size_t begin,
                       bool &is_float) {
  const auto size = text.size();
  auto index = begin;
  while (index < size && is_digit(text[index])) {
    ++index;
  }
  if (text[begin] == '0' && index - begin > 1U) {
    fail(text, "invalid_number", "a number cannot start with a leading zero",
         begin, index);
  }
  is_float = false;
  if (index < size && text[index] == '.') {
    if (index + 1 >= size || !is_digit(text[index + 1])) {
      fail(text, "invalid_number", "a decimal point must be followed by digits",
           begin, index + 1);
    }
    is_float = true;
    index += 1;
    while (index < size && is_digit(text[index])) {
      ++index;
    }
  }
  if (index < size && (text[index] == 'e' || text[index] == 'E')) {
    auto exponent = index + 1;
    if (exponent < size && (text[exponent] == '+' || text[exponent] == '-')) {
      ++exponent;
    }
    if (exponent >= size || !is_digit(text[exponent])) {
      fail(text, "invalid_number", "an exponent must have digits", begin,
           std::min(exponent + 1, size));
    }
    is_float = true;
    index = exponent;
    while (index < size && is_digit(text[index])) {
      ++index;
    }
  }
  if (index < size && (is_name_char(text[index]) || text[index] == '.')) {
    fail(text, "invalid_number", "malformed number", begin, index + 1);
  }
  return index;
}

std::size_t lex_name(std::string_view text, std::size_t begin) {
  const auto size = text.size();
  auto index = begin;
  while (index < size && is_name_char(text[index])) {
    ++index;
  }
  while (index < size && text[index] == '.') {
    if (index + 1 >= size || !is_name_start(text[index + 1])) {
      fail(text, "invalid_name", "a '.' must be followed by a name", begin,
           index + 1);
    }
    index += 2;
    while (index < size && is_name_char(text[index])) {
      ++index;
    }
  }
  return index;
}

std::size_t lex_string(std::string_view text, std::size_t begin) {
  const auto size = text.size();
  auto index = begin + 1;
  while (true) {
    if (index >= size) {
      fail(text, "unterminated_string", "string is not closed with '\"'", begin,
           size);
    }
    const auto value = static_cast<unsigned char>(text[index]);
    if (value == '"') {
      return index + 1;
    }
    if (value == '\\') {
      if (index + 1 >= size) {
        fail(text, "unterminated_string", "string is not closed with '\"'",
             begin, size);
      }
      if (text[index + 1] != '"' && text[index + 1] != '\\') {
        fail(text, "invalid_escape", "only \\\" and \\\\ escapes are allowed",
             index, character_end(text, index + 1));
      }
      index += 2;
      continue;
    }
    if (value < 0x20U || value == 0x7FU) {
      fail(text, "invalid_character",
           "control characters are not allowed in strings", index, index + 1);
    }
    index = character_end(text, index);
  }
}

std::vector<Token> tokenize(std::string_view text, const Limits &limits) {
  std::vector<Token> tokens;
  const auto size = text.size();
  std::size_t index = 0;
  while (true) {
    while (index < size && (text[index] == ' ' || text[index] == '\t' ||
                            text[index] == '\n' || text[index] == '\r')) {
      ++index;
    }
    if (index == size) {
      tokens.push_back(Token{TokenKind::kEnd, index, index});
      return tokens;
    }
    const auto begin = index;
    if (tokens.size() == limits.tokens) {
      fail(text, "too_many_tokens",
           "expression has more than " + std::to_string(limits.tokens) +
               " tokens",
           begin, character_end(text, begin));
    }
    const char value = text[index];
    auto kind = TokenKind::kEnd;
    if (is_digit(value)) {
      bool is_float = false;
      index = lex_number(text, begin, is_float);
      kind = is_float ? TokenKind::kFloat : TokenKind::kInteger;
    } else if (is_name_start(value)) {
      index = lex_name(text, begin);
      kind = TokenKind::kName;
    } else if (value == '"') {
      index = lex_string(text, begin);
      kind = TokenKind::kString;
    } else {
      const char next = index + 1 < size ? text[index + 1] : '\0';
      const auto two = [&](TokenKind token) {
        index += 2;
        return token;
      };
      const auto one = [&](TokenKind token) {
        index += 1;
        return token;
      };
      switch (value) {
      case '(':
        kind = one(TokenKind::kLeftParen);
        break;
      case ')':
        kind = one(TokenKind::kRightParen);
        break;
      case ',':
        kind = one(TokenKind::kComma);
        break;
      case '?':
        kind = one(TokenKind::kQuestion);
        break;
      case ':':
        kind = one(TokenKind::kColon);
        break;
      case '+':
        kind = one(TokenKind::kPlus);
        break;
      case '-':
        kind = one(TokenKind::kMinus);
        break;
      case '*':
        kind = one(TokenKind::kStar);
        break;
      case '/':
        kind = one(TokenKind::kSlash);
        break;
      case '%':
        kind = one(TokenKind::kPercent);
        break;
      case '!':
        kind = next == '=' ? two(TokenKind::kNotEqual) : one(TokenKind::kBang);
        break;
      case '<':
        kind = next == '=' ? two(TokenKind::kLessEqual) : one(TokenKind::kLess);
        break;
      case '>':
        kind = next == '=' ? two(TokenKind::kGreaterEqual)
                           : one(TokenKind::kGreater);
        break;
      case '=':
        if (next != '=') {
          fail(text, "invalid_character",
               "'=' is not an operator; compare with '=='", begin, begin + 1);
        }
        kind = two(TokenKind::kEqual);
        break;
      case '&':
        if (next != '&') {
          fail(text, "invalid_character", "'&' is not an operator; use '&&'",
               begin, begin + 1);
        }
        kind = two(TokenKind::kAnd);
        break;
      case '|':
        if (next != '|') {
          fail(text, "invalid_character", "'|' is not an operator; use '||'",
               begin, begin + 1);
        }
        kind = two(TokenKind::kOr);
        break;
      default:
        fail(text, "invalid_character", "character is not allowed here", begin,
             character_end(text, begin));
      }
    }
    tokens.push_back(Token{kind, begin, index});
  }
}

// ------------------------------------------------------------------ parser

struct FunctionSpec final {
  std::string_view name;
  Function function;
  std::size_t arity;
};

constexpr std::array<FunctionSpec, 14> kFunctions{{
    {"abs", Function::kAbs, 1},
    {"min", Function::kMin, 2},
    {"max", Function::kMax, 2},
    {"clamp", Function::kClamp, 3},
    {"floor", Function::kFloor, 1},
    {"ceil", Function::kCeil, 1},
    {"round", Function::kRound, 1},
    {"sqrt", Function::kSqrt, 1},
    {"sin", Function::kSin, 1},
    {"cos", Function::kCos, 1},
    {"tan", Function::kTan, 1},
    {"atan2", Function::kAtan2, 2},
    {"deg", Function::kDeg, 1},
    {"rad", Function::kRad, 1},
}};

const FunctionSpec *find_function(std::string_view name) {
  for (const auto &spec : kFunctions) {
    if (spec.name == name) {
      return &spec;
    }
  }
  return nullptr;
}

std::string_view function_name(Function function) {
  for (const auto &spec : kFunctions) {
    if (spec.function == function) {
      return spec.name;
    }
  }
  return "unknown";
}

std::string_view operator_text(Operator op) {
  switch (op) {
  case Operator::kNegate:
  case Operator::kSubtract:
    return "-";
  case Operator::kNot:
    return "!";
  case Operator::kMultiply:
    return "*";
  case Operator::kDivide:
    return "/";
  case Operator::kRemainder:
    return "%";
  case Operator::kAdd:
    return "+";
  case Operator::kLess:
    return "<";
  case Operator::kLessEqual:
    return "<=";
  case Operator::kGreater:
    return ">";
  case Operator::kGreaterEqual:
    return ">=";
  case Operator::kEqual:
    return "==";
  case Operator::kNotEqual:
    return "!=";
  case Operator::kAnd:
    return "&&";
  case Operator::kOr:
    return "||";
  }
  return "?";
}

// Binary precedence levels from loosest (||) to tightest (* / %).
struct BinaryLevel final {
  std::array<std::pair<TokenKind, Operator>, 4> operators;
  std::size_t count;
};

constexpr std::array<BinaryLevel, 6> kBinaryLevels{{
    {{{{TokenKind::kOr, Operator::kOr}}}, 1},
    {{{{TokenKind::kAnd, Operator::kAnd}}}, 1},
    {{{{TokenKind::kEqual, Operator::kEqual},
       {TokenKind::kNotEqual, Operator::kNotEqual}}},
     2},
    {{{{TokenKind::kLess, Operator::kLess},
       {TokenKind::kLessEqual, Operator::kLessEqual},
       {TokenKind::kGreater, Operator::kGreater},
       {TokenKind::kGreaterEqual, Operator::kGreaterEqual}}},
     4},
    {{{{TokenKind::kPlus, Operator::kAdd},
       {TokenKind::kMinus, Operator::kSubtract}}},
     2},
    {{{{TokenKind::kStar, Operator::kMultiply},
       {TokenKind::kSlash, Operator::kDivide},
       {TokenKind::kPercent, Operator::kRemainder}}},
     3},
}};

// A parsed operand: its node and its source span including parentheses.
struct Operand final {
  std::size_t node;
  std::size_t begin;
  std::size_t end;
};

class Parser final {
public:
  Parser(ParsedTree &tree, std::vector<Token> tokens)
      : tree_(tree), text_(tree.text), tokens_(std::move(tokens)) {}

  void run() {
    const auto root = conditional(0);
    if (peek().kind != TokenKind::kEnd) {
      unexpected(peek());
    }
    tree_.root = root.node;
  }

private:
  const Token &peek() const { return tokens_[position_]; }

  const Token &take() { return tokens_[position_++]; }

  std::string_view token_text(const Token &token) const {
    return std::string_view(text_).substr(token.begin, token.end - token.begin);
  }

  [[noreturn]] void unexpected(const Token &token) const {
    if (token.kind == TokenKind::kEnd) {
      fail(text_, "unexpected_end",
           position_ == 0 ? "expression is empty" : "expression ends too early",
           token.begin, token.end);
    }
    fail(text_, "unexpected_token",
         "unexpected '" + std::string(token_text(token)) + "'", token.begin,
         token.end);
  }

  const Token &expect(TokenKind kind) {
    if (peek().kind != kind) {
      unexpected(peek());
    }
    return take();
  }

  void enter(std::size_t depth) const {
    if (depth > tree_.limits.nesting_depth) {
      const auto &token = peek();
      fail(text_, "nesting_too_deep",
           "expression nests deeper than " +
               std::to_string(tree_.limits.nesting_depth) + " levels",
           token.begin, std::max(token.end, token.begin + 1));
    }
  }

  Operand add(Node node) {
    if (tree_.nodes.size() == tree_.limits.syntax_nodes) {
      fail(text_, "syntax_tree_too_large",
           "expression has more than " +
               std::to_string(tree_.limits.syntax_nodes) + " syntax nodes",
           node.begin, node.end);
    }
    const auto begin = node.begin;
    const auto end = node.end;
    tree_.nodes.push_back(std::move(node));
    return Operand{tree_.nodes.size() - 1, begin, end};
  }

  Operand conditional(std::size_t depth) {
    enter(depth);
    auto condition = binary(0, depth);
    if (peek().kind != TokenKind::kQuestion) {
      return condition;
    }
    take();
    const auto when_true = conditional(depth + 1);
    expect(TokenKind::kColon);
    const auto when_false = conditional(depth + 1);
    Node node;
    node.kind = NodeKind::kConditional;
    node.begin = condition.begin;
    node.end = when_false.end;
    node.children = {condition.node, when_true.node, when_false.node};
    return add(std::move(node));
  }

  Operand binary(std::size_t level, std::size_t depth) {
    if (level == kBinaryLevels.size()) {
      return unary(depth);
    }
    auto left = binary(level + 1, depth);
    while (true) {
      const auto &spec = kBinaryLevels[level];
      const auto kind = peek().kind;
      const auto *match = std::find_if(
          spec.operators.begin(), spec.operators.begin() + spec.count,
          [kind](const auto &entry) { return entry.first == kind; });
      if (match == spec.operators.begin() + spec.count) {
        return left;
      }
      take();
      const auto right = binary(level + 1, depth);
      Node node;
      node.kind = NodeKind::kBinary;
      node.op = match->second;
      node.begin = left.begin;
      node.end = right.end;
      node.children = {left.node, right.node};
      left = add(std::move(node));
    }
  }

  Operand unary(std::size_t depth) {
    const auto kind = peek().kind;
    if (kind != TokenKind::kMinus && kind != TokenKind::kBang) {
      return primary(depth);
    }
    const auto token = take();
    enter(depth + 1);
    const auto operand = unary(depth + 1);
    Node node;
    node.kind = NodeKind::kUnary;
    node.op = kind == TokenKind::kMinus ? Operator::kNegate : Operator::kNot;
    node.begin = token.begin;
    node.end = operand.end;
    node.children = {operand.node};
    return add(std::move(node));
  }

  Operand literal(const Token &token, Scalar value) {
    Node node;
    node.kind = NodeKind::kLiteral;
    node.begin = token.begin;
    node.end = token.end;
    node.literal = std::move(value);
    return add(std::move(node));
  }

  Operand primary(std::size_t depth) {
    const auto token = peek();
    switch (token.kind) {
    case TokenKind::kLeftParen: {
      take();
      const auto inner = conditional(depth + 1);
      const auto close = expect(TokenKind::kRightParen);
      return Operand{inner.node, token.begin, close.end};
    }
    case TokenKind::kInteger: {
      take();
      const auto text = token_text(token);
      std::int64_t value = 0;
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size()) {
        fail(text_, "integer_literal_out_of_range",
             "integer literal does not fit int64", token.begin, token.end);
      }
      return literal(token, value);
    }
    case TokenKind::kFloat: {
      take();
      const auto text = token_text(token);
      double value = 0.0;
      const auto [end, error] =
          std::from_chars(text.data(), text.data() + text.size(), value);
      if (error != std::errc{} || end != text.data() + text.size() ||
          !std::isfinite(value)) {
        fail(text_, "float_literal_out_of_range",
             "number literal is outside the float64 range", token.begin,
             token.end);
      }
      return literal(token, value);
    }
    case TokenKind::kString: {
      take();
      std::string value;
      for (auto index = token.begin + 1; index + 1 < token.end; ++index) {
        if (text_[index] == '\\') {
          ++index;
        }
        value.push_back(text_[index]);
      }
      return literal(token, std::move(value));
    }
    case TokenKind::kName:
      return name(depth);
    default:
      unexpected(token);
    }
  }

  Operand name(std::size_t depth) {
    const auto token = take();
    const auto text = token_text(token);
    std::vector<std::string> segments;
    for (std::size_t begin = 0; begin <= text.size();) {
      const auto dot = std::min(text.find('.', begin), text.size());
      segments.emplace_back(text.substr(begin, dot - begin));
      begin = dot + 1;
    }
    const auto &head = segments.front();
    if (segments.size() == 1U) {
      if (head == "true" || head == "false") {
        return literal(token, head == "true");
      }
      if (peek().kind == TokenKind::kLeftParen) {
        return call(token, head, depth);
      }
      if (head == "param" || head == "key") {
        fail(text_, "invalid_name",
             "'" + head + "' must be followed by '.' and a name", token.begin,
             token.end, head);
      }
      if (find_function(head) != nullptr) {
        fail(text_, "invalid_name", "function '" + head + "' must be called",
             token.begin, token.end, head);
      }
      fail(text_, "invalid_name",
           "unknown identifier '" + head +
               "'; write param.<name> or key.<name>",
           token.begin, token.end, head);
    }
    if (head != "param" && head != "key") {
      fail(text_, "invalid_name",
           "names start with 'param.' or 'key.', not '" + head + ".'",
           token.begin, token.end, std::string(text));
    }
    Node node;
    node.kind = NodeKind::kName;
    node.begin = token.begin;
    node.end = token.end;
    node.source = head == "param" ? Source::kParameter : Source::kKey;
    node.segments.assign(segments.begin() + 1, segments.end());
    return add(std::move(node));
  }

  Operand call(const Token &token, const std::string &identifier,
               std::size_t depth) {
    const auto *spec = find_function(identifier);
    if (spec == nullptr) {
      fail(text_, "unknown_function", "unknown function '" + identifier + "'",
           token.begin, token.end, identifier);
    }
    take();
    std::vector<std::size_t> arguments;
    if (peek().kind != TokenKind::kRightParen) {
      while (true) {
        arguments.push_back(conditional(depth + 1).node);
        if (peek().kind != TokenKind::kComma) {
          break;
        }
        take();
      }
    }
    const auto close = expect(TokenKind::kRightParen);
    if (arguments.size() != spec->arity) {
      fail(text_, "wrong_argument_count",
           identifier + "() takes " + std::to_string(spec->arity) +
               (spec->arity == 1U ? " argument" : " arguments") + ", got " +
               std::to_string(arguments.size()),
           token.begin, close.end, identifier);
    }
    Node node;
    node.kind = NodeKind::kCall;
    node.function = spec->function;
    node.begin = token.begin;
    node.end = close.end;
    node.children = std::move(arguments);
    return add(std::move(node));
  }

  ParsedTree &tree_;
  const std::string &text_;
  std::vector<Token> tokens_;
  std::size_t position_ = 0;
};

void require_narrowing(std::size_t value, std::size_t approved,
                       const char *name) {
  if (value == 0U || value > approved) {
    throw std::invalid_argument(std::string("expression limit ") + name +
                                " must be between 1 and the approved " +
                                std::to_string(approved));
  }
}

// -------------------------------------------------------------- evaluation

constexpr double kTwoTo63 = 9223372036854775808.0;
constexpr double kQuaternionNormTolerance = 1e-6;

bool is_number(ValueKind kind) {
  return kind == ValueKind::kInt64 || kind == ValueKind::kFloat64;
}

bool is_int(const Scalar &value) {
  return std::holds_alternative<std::int64_t>(value);
}

double as_double(const Scalar &value) {
  return is_int(value) ? static_cast<double>(std::get<std::int64_t>(value))
                       : std::get<double>(value);
}

// Exact ordering of an int64 and a finite double, without rounding the int.
std::partial_ordering compare_mixed(std::int64_t integer, double real) {
  if (real >= kTwoTo63) {
    return std::partial_ordering::less;
  }
  if (real < -kTwoTo63) {
    return std::partial_ordering::greater;
  }
  const double truncated = std::trunc(real);
  const auto whole = static_cast<std::int64_t>(truncated);
  if (whole != integer) {
    return integer <=> whole;
  }
  return 0.0 <=> (real - truncated);
}

std::partial_ordering compare_numbers(const Scalar &left, const Scalar &right) {
  if (is_int(left) && is_int(right)) {
    return std::get<std::int64_t>(left) <=> std::get<std::int64_t>(right);
  }
  if (is_int(left)) {
    return compare_mixed(std::get<std::int64_t>(left), std::get<double>(right));
  }
  if (is_int(right)) {
    return 0 <=>
           compare_mixed(std::get<std::int64_t>(right), std::get<double>(left));
  }
  return std::get<double>(left) <=> std::get<double>(right);
}

std::optional<std::string> validate_value(const Value &value,
                                          const ValueType &type) {
  const auto finite = [](const auto &values) {
    return std::all_of(values.begin(), values.end(),
                       [](double item) { return std::isfinite(item); });
  };
  switch (type.kind) {
  case ValueKind::kBool:
    if (std::holds_alternative<bool>(value.data)) {
      return std::nullopt;
    }
    break;
  case ValueKind::kInt64:
    if (std::holds_alternative<std::int64_t>(value.data)) {
      return std::nullopt;
    }
    break;
  case ValueKind::kFloat64:
    if (const auto *real = std::get_if<double>(&value.data)) {
      if (!std::isfinite(*real)) {
        return "float64 value is not finite";
      }
      return std::nullopt;
    }
    break;
  case ValueKind::kString:
    if (std::holds_alternative<std::string>(value.data)) {
      return std::nullopt;
    }
    break;
  case ValueKind::kPose:
    if (const auto *pose = std::get_if<Pose>(&value.data)) {
      if (!finite(pose->position_m)) {
        return "pose position is not finite";
      }
      if (const auto *rpy = std::get_if<RpyDeg>(&pose->orientation)) {
        if (!finite(rpy->values)) {
          return "pose rpy_deg is not finite";
        }
        return std::nullopt;
      }
      const auto &xyzw = std::get<Xyzw>(pose->orientation).values;
      if (!finite(xyzw)) {
        return "pose xyzw is not finite";
      }
      double squared = 0.0;
      for (const double item : xyzw) {
        squared += item * item;
      }
      if (std::abs(squared - 1.0) > kQuaternionNormTolerance) {
        return "pose xyzw is not a unit quaternion";
      }
      return std::nullopt;
    }
    break;
  case ValueKind::kRecord:
    if (const auto *record = std::get_if<Record>(&value.data)) {
      if (record->size() != type.fields.size()) {
        return "record does not have exactly its declared fields";
      }
      for (const auto &field : type.fields) {
        const auto found = std::count_if(
            record->begin(), record->end(),
            [&](const auto &item) { return item.name == field.name; });
        if (found != 1) {
          return "record field '" + field.name + "' is missing or repeated";
        }
        const auto item =
            std::find_if(record->begin(), record->end(), [&](const auto &row) {
              return row.name == field.name;
            });
        if (auto error = validate_value(item->value, field.type)) {
          return "record field '" + field.name + "': " + *error;
        }
      }
      return std::nullopt;
    }
    break;
  }
  return "value is not a " + std::string(kind_name(type.kind));
}

// Unit quaternion with the scaling used by the Program geometry helpers.
std::array<double, 4> normalized(const std::array<double, 4> &values) {
  double scale = 0.0;
  for (const double item : values) {
    scale = std::max(scale, std::abs(item));
  }
  std::array<double, 4> scaled{};
  double norm = 0.0;
  for (std::size_t index = 0; index < 4U; ++index) {
    scaled[index] = values[index] / scale;
    norm += scaled[index] * scaled[index];
  }
  norm = std::sqrt(norm);
  for (auto &item : scaled) {
    item /= norm;
  }
  return scaled;
}

constexpr double kRadiansPerDegree = std::numbers::pi / 180.0;
constexpr double kDegreesPerRadian = 180.0 / std::numbers::pi;

// Intrinsic ZYX roll/pitch/yaw, the Program geometry convention.
std::array<double, 4> rpy_deg_to_xyzw(const std::array<double, 3> &rpy) {
  const double roll = rpy[0] * kRadiansPerDegree;
  const double pitch = rpy[1] * kRadiansPerDegree;
  const double yaw = rpy[2] * kRadiansPerDegree;
  const double cr = std::cos(roll / 2);
  const double sr = std::sin(roll / 2);
  const double cp = std::cos(pitch / 2);
  const double sp = std::sin(pitch / 2);
  const double cy = std::cos(yaw / 2);
  const double sy = std::sin(yaw / 2);
  return normalized({sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy,
                     cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy});
}

std::array<double, 3> xyzw_to_rpy_deg(const std::array<double, 4> &stored) {
  const auto [x, y, z, w] = normalized(stored);
  const double roll =
      std::atan2(2.0 * (w * x + y * z), 1.0 - 2.0 * (x * x + y * y));
  const double sinp = 2.0 * (w * y - z * x);
  const double pitch = std::abs(sinp) >= 1.0
                           ? std::copysign(std::numbers::pi / 2.0, sinp)
                           : std::asin(sinp);
  const double yaw =
      std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
  return {roll * kDegreesPerRadian, pitch * kDegreesPerRadian,
          yaw * kDegreesPerRadian};
}

double pose_field(const Pose &pose, PoseField field) {
  switch (field) {
  case PoseField::kX:
    return pose.position_m[0];
  case PoseField::kY:
    return pose.position_m[1];
  case PoseField::kZ:
    return pose.position_m[2];
  case PoseField::kRoll:
  case PoseField::kPitch:
  case PoseField::kYaw: {
    const auto index = static_cast<std::size_t>(field) -
                       static_cast<std::size_t>(PoseField::kRoll);
    if (const auto *rpy = std::get_if<RpyDeg>(&pose.orientation)) {
      return rpy->values[index];
    }
    return xyzw_to_rpy_deg(std::get<Xyzw>(pose.orientation).values)[index];
  }
  case PoseField::kQx:
  case PoseField::kQy:
  case PoseField::kQz:
  case PoseField::kQw: {
    const auto index = static_cast<std::size_t>(field) -
                       static_cast<std::size_t>(PoseField::kQx);
    if (const auto *xyzw = std::get_if<Xyzw>(&pose.orientation)) {
      return xyzw->values[index];
    }
    return rpy_deg_to_xyzw(std::get<RpyDeg>(pose.orientation).values)[index];
  }
  case PoseField::kNone:
    break;
  }
  throw std::logic_error("expression pose field is unresolved");
}

class Evaluator final {
public:
  // `values` is null only for constant parts, which read no names.
  // `exhaustive` evaluates every operand (no short-circuit) so a constant part
  // that fails is reported even where it would not be reached.
  Evaluator(const ParsedTree &tree, const std::vector<ValueKind> &kinds,
            const std::vector<ResolvedName> &names,
            const std::map<Reference, ValueType> &references,
            const ValueSnapshot *values, bool exhaustive)
      : tree_(tree), text_(tree.text), kinds_(kinds), names_(names),
        references_(references), values_(values), exhaustive_(exhaustive) {}

  Scalar run(std::size_t index) {
    const auto &node = tree_.nodes[index];
    if (++steps_ > tree_.limits.evaluation_steps) {
      failure(node, "evaluation_step_limit",
              "evaluation needs more than " +
                  std::to_string(tree_.limits.evaluation_steps) + " steps");
    }
    switch (node.kind) {
    case NodeKind::kLiteral:
      return node.literal;
    case NodeKind::kName:
      return read(index);
    case NodeKind::kUnary:
      return unary(node);
    case NodeKind::kBinary:
      return binary(node);
    case NodeKind::kConditional:
      return conditional(index);
    case NodeKind::kCall:
      return call(node);
    }
    throw std::logic_error("expression node kind is invalid");
  }

private:
  [[noreturn]] void failure(const Node &node, std::string code,
                            std::string message, std::string name = {}) const {
    fail(text_, std::move(code), std::move(message), node.begin, node.end,
         std::move(name));
  }

  double finite(const Node &node, double value) const {
    if (!std::isfinite(value)) {
      failure(node, "non_finite_result", "result is not a finite number");
    }
    return value;
  }

  std::int64_t checked_int(const Node &node, bool overflow,
                           std::int64_t value) const {
    if (overflow) {
      failure(node, "integer_overflow", "int64 result is out of range");
    }
    return value;
  }

  std::int64_t to_int(const Node &node, double value) const {
    if (!(value >= -kTwoTo63 && value < kTwoTo63)) {
      failure(node, "integer_overflow", "result does not fit int64");
    }
    return static_cast<std::int64_t>(value);
  }

  Scalar read(std::size_t index) {
    const auto &node = tree_.nodes[index];
    const auto &resolved = names_[index];
    const auto display = prefixed(resolved.reference);
    const auto &map = resolved.reference.source == Source::kParameter
                          ? values_->parameters
                          : values_->keys;
    const auto found = map.find(resolved.reference.name);
    if (found == map.end()) {
      failure(node, "missing_value", display + " has no value", display);
    }
    const auto &type = references_.at(resolved.reference);
    if (auto error = validate_value(found->second, type)) {
      failure(node, "invalid_value", display + ": " + *error, display);
    }
    const Value *value = &found->second;
    for (const auto &field : resolved.record_path) {
      const auto &record = std::get<Record>(value->data);
      value = &std::find_if(record.begin(), record.end(), [&](const auto &row) {
                 return row.name == field;
               })->value;
    }
    if (resolved.pose_field != PoseField::kNone) {
      return pose_field(std::get<Pose>(value->data), resolved.pose_field);
    }
    return std::visit(
        [](const auto &item) -> Scalar {
          using Item = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<Item, Pose> ||
                        std::is_same_v<Item, Record>) {
            throw std::logic_error("expression name resolves to a whole value");
          } else {
            return item;
          }
        },
        value->data);
  }

  Scalar unary(const Node &node) {
    const auto operand = run(node.children[0]);
    if (node.op == Operator::kNot) {
      return !std::get<bool>(operand);
    }
    if (is_int(operand)) {
      std::int64_t result = 0;
      const bool overflow = __builtin_sub_overflow(
          std::int64_t{0}, std::get<std::int64_t>(operand), &result);
      return checked_int(node, overflow, result);
    }
    return -std::get<double>(operand);
  }

  Scalar binary(const Node &node) {
    if (node.op == Operator::kAnd || node.op == Operator::kOr) {
      const bool left = std::get<bool>(run(node.children[0]));
      const bool decided = node.op == Operator::kAnd ? !left : left;
      if (decided && !exhaustive_) {
        return left;
      }
      const bool right = std::get<bool>(run(node.children[1]));
      return decided ? left : right;
    }
    const auto left = run(node.children[0]);
    const auto right = run(node.children[1]);
    const bool ints = is_int(left) && is_int(right);
    switch (node.op) {
    case Operator::kAdd:
    case Operator::kSubtract:
    case Operator::kMultiply: {
      if (ints) {
        const auto a = std::get<std::int64_t>(left);
        const auto b = std::get<std::int64_t>(right);
        std::int64_t result = 0;
        const bool overflow = node.op == Operator::kAdd
                                  ? __builtin_add_overflow(a, b, &result)
                              : node.op == Operator::kSubtract
                                  ? __builtin_sub_overflow(a, b, &result)
                                  : __builtin_mul_overflow(a, b, &result);
        return checked_int(node, overflow, result);
      }
      const double a = as_double(left);
      const double b = as_double(right);
      return finite(node, node.op == Operator::kAdd        ? a + b
                          : node.op == Operator::kSubtract ? a - b
                                                           : a * b);
    }
    case Operator::kDivide: {
      const double divisor = as_double(right);
      if (divisor == 0.0) {
        failure(node, "division_by_zero", "division by zero");
      }
      return finite(node, as_double(left) / divisor);
    }
    case Operator::kRemainder: {
      const auto a = std::get<std::int64_t>(left);
      const auto b = std::get<std::int64_t>(right);
      if (b == 0) {
        failure(node, "division_by_zero", "remainder by zero");
      }
      if (a < 0 || b < 0) {
        failure(node, "negative_remainder_operand",
                "'%' is defined only for non-negative integers");
      }
      return a % b;
    }
    case Operator::kLess:
      return compare_numbers(left, right) < 0;
    case Operator::kLessEqual:
      return compare_numbers(left, right) <= 0;
    case Operator::kGreater:
      return compare_numbers(left, right) > 0;
    case Operator::kGreaterEqual:
      return compare_numbers(left, right) >= 0;
    case Operator::kEqual:
    case Operator::kNotEqual: {
      const bool equal = (std::holds_alternative<bool>(left) ||
                          std::holds_alternative<std::string>(left))
                             ? left == right
                             : compare_numbers(left, right) == 0;
      return node.op == Operator::kEqual ? equal : !equal;
    }
    default:
      break;
    }
    throw std::logic_error("expression binary operator is invalid");
  }

  Scalar conditional(std::size_t index) {
    const auto &node = tree_.nodes[index];
    const bool condition = std::get<bool>(run(node.children[0]));
    Scalar result;
    if (exhaustive_) {
      auto when_true = run(node.children[1]);
      auto when_false = run(node.children[2]);
      result = condition ? std::move(when_true) : std::move(when_false);
    } else {
      result = run(node.children[condition ? 1 : 2]);
    }
    if (kinds_[index] == ValueKind::kFloat64 && is_int(result)) {
      return as_double(result);
    }
    return result;
  }

  Scalar call(const Node &node) {
    std::vector<Scalar> arguments;
    arguments.reserve(node.children.size());
    for (const auto child : node.children) {
      arguments.push_back(run(child));
    }
    const auto &x = arguments[0];
    const bool ints = std::all_of(arguments.begin(), arguments.end(), is_int);
    switch (node.function) {
    case Function::kAbs:
      if (ints) {
        const auto value = std::get<std::int64_t>(x);
        checked_int(node, value == std::numeric_limits<std::int64_t>::min(),
                    value);
        return value < 0 ? -value : value;
      }
      return std::abs(std::get<double>(x));
    case Function::kMin:
    case Function::kMax: {
      const bool second = node.function == Function::kMin
                              ? compare_numbers(arguments[1], x) < 0
                              : compare_numbers(arguments[1], x) > 0;
      const auto &chosen = second ? arguments[1] : x;
      return ints ? chosen : Scalar(as_double(chosen));
    }
    case Function::kClamp: {
      const auto &low = arguments[1];
      const auto &high = arguments[2];
      if (compare_numbers(low, high) > 0) {
        failure(node, "clamp_bounds_inverted",
                "clamp() lower bound is greater than its upper bound");
      }
      const auto &chosen = compare_numbers(x, low) < 0    ? low
                           : compare_numbers(x, high) > 0 ? high
                                                          : x;
      return ints ? chosen : Scalar(as_double(chosen));
    }
    case Function::kFloor:
    case Function::kCeil:
    case Function::kRound: {
      if (ints) {
        return x;
      }
      const double value = std::get<double>(x);
      return to_int(node, node.function == Function::kFloor ? std::floor(value)
                          : node.function == Function::kCeil
                              ? std::ceil(value)
                              : std::round(value));
    }
    case Function::kSqrt: {
      const double value = as_double(x);
      if (value < 0.0) {
        failure(node, "sqrt_of_negative", "sqrt() of a negative number");
      }
      return std::sqrt(value);
    }
    case Function::kSin:
      return finite(node, std::sin(as_double(x)));
    case Function::kCos:
      return finite(node, std::cos(as_double(x)));
    case Function::kTan:
      return finite(node, std::tan(as_double(x)));
    case Function::kAtan2:
      return std::atan2(as_double(x), as_double(arguments[1]));
    case Function::kDeg:
      return finite(node, as_double(x) * kDegreesPerRadian);
    case Function::kRad:
      return finite(node, as_double(x) * kRadiansPerDegree);
    }
    throw std::logic_error("expression function is invalid");
  }

  const ParsedTree &tree_;
  const std::string &text_;
  const std::vector<ValueKind> &kinds_;
  const std::vector<ResolvedName> &names_;
  const std::map<Reference, ValueType> &references_;
  const ValueSnapshot *values_;
  bool exhaustive_;
  std::size_t steps_ = 0;
};

// ----------------------------------------------------------------- checker

constexpr std::array<std::pair<std::string_view, PoseField>, 10> kPoseFields{{
    {"x", PoseField::kX},
    {"y", PoseField::kY},
    {"z", PoseField::kZ},
    {"roll", PoseField::kRoll},
    {"pitch", PoseField::kPitch},
    {"yaw", PoseField::kYaw},
    {"qx", PoseField::kQx},
    {"qy", PoseField::kQy},
    {"qz", PoseField::kQz},
    {"qw", PoseField::kQw},
}};

void validate_type(const ValueType &type) {
  if (type.kind != ValueKind::kRecord) {
    if (!type.fields.empty()) {
      throw std::invalid_argument(
          "expression type environment: only records declare fields");
    }
    return;
  }
  if (type.fields.empty()) {
    throw std::invalid_argument(
        "expression type environment: a record needs fields");
  }
  for (std::size_t index = 0; index < type.fields.size(); ++index) {
    const auto &field = type.fields[index];
    if (field.name.empty() || !is_name_start(field.name.front()) ||
        !std::all_of(field.name.begin(), field.name.end(), is_name_char)) {
      throw std::invalid_argument(
          "expression type environment: record field name '" + field.name +
          "' is not an identifier");
    }
    for (std::size_t other = 0; other < index; ++other) {
      if (type.fields[other].name == field.name) {
        throw std::invalid_argument(
            "expression type environment: record field '" + field.name +
            "' is repeated");
      }
    }
    validate_type(field.type);
  }
}

class Checker final {
public:
  Checker(const std::shared_ptr<const ParsedTree> &tree,
          const TypeEnvironment &environment, CheckedTree &out)
      : tree_(*tree), text_(tree->text), environment_(environment), out_(out) {
    out_.kinds.assign(tree_.nodes.size(), ValueKind::kBool);
    out_.names.assign(tree_.nodes.size(), ResolvedName{});
    constant_.assign(tree_.nodes.size(), false);
  }

  std::optional<ValueKind> run() {
    const auto root = tree_.root;
    const auto kind = visit(root);
    if (kind && constant_[root]) {
      evaluate_constant(root);
    }
    return kind;
  }

  std::vector<Diagnostic> diagnostics;

private:
  void report(const Node &node, std::string code, std::string message,
              std::string name = {}) {
    diagnostics.push_back(make_diagnostic(text_, std::move(code),
                                          std::move(message), node.begin,
                                          node.end, std::move(name)));
  }

  void evaluate_constant(std::size_t index) {
    if (tree_.nodes[index].kind == NodeKind::kLiteral) {
      return;
    }
    try {
      Evaluator(tree_, out_.kinds, out_.names, out_.references, nullptr, true)
          .run(index);
    } catch (const DiagnosticError &error) {
      diagnostics.push_back(error.diagnostic);
    }
  }

  std::optional<ValueKind> visit(std::size_t index) {
    const auto &node = tree_.nodes[index];
    if (node.kind == NodeKind::kName) {
      return resolve(index);
    }
    std::vector<std::optional<ValueKind>> children;
    bool all_constant = true;
    for (const auto child : node.children) {
      children.push_back(visit(child));
      all_constant = all_constant && constant_[child];
    }
    constant_[index] = all_constant;
    if (std::any_of(children.begin(), children.end(),
                    [](const auto &kind) { return !kind.has_value(); })) {
      return std::nullopt;
    }
    std::vector<ValueKind> kinds;
    for (const auto &kind : children) {
      kinds.push_back(*kind);
    }
    const auto kind = type_of(node, kinds);
    if (!kind) {
      return std::nullopt;
    }
    out_.kinds[index] = *kind;
    if (!all_constant) {
      for (const auto child : node.children) {
        if (constant_[child]) {
          evaluate_constant(child);
        }
      }
    }
    return kind;
  }

  std::optional<ValueKind> mismatch(const Node &node, std::string_view what,
                                    const std::vector<ValueKind> &kinds) {
    std::string got;
    for (const auto kind : kinds) {
      got += (got.empty() ? "" : ", ") + std::string(kind_name(kind));
    }
    report(node, "type_mismatch", std::string(what) + ", got " + got);
    return std::nullopt;
  }

  std::optional<ValueKind> type_of(const Node &node,
                                   const std::vector<ValueKind> &kinds) {
    const auto numbers = std::all_of(kinds.begin(), kinds.end(), is_number);
    const auto numeric = [&]() {
      return std::all_of(
                 kinds.begin(), kinds.end(),
                 [](ValueKind kind) { return kind == ValueKind::kInt64; })
                 ? ValueKind::kInt64
                 : ValueKind::kFloat64;
    };
    switch (node.kind) {
    case NodeKind::kLiteral:
      return std::visit(
          [](const auto &value) {
            using Item = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Item, bool>) {
              return ValueKind::kBool;
            } else if constexpr (std::is_same_v<Item, std::int64_t>) {
              return ValueKind::kInt64;
            } else if constexpr (std::is_same_v<Item, double>) {
              return ValueKind::kFloat64;
            } else {
              return ValueKind::kString;
            }
          },
          node.literal);
    case NodeKind::kUnary:
      if (node.op == Operator::kNot) {
        if (kinds[0] != ValueKind::kBool) {
          return mismatch(node, "'!' needs a bool", kinds);
        }
        return ValueKind::kBool;
      }
      if (!numbers) {
        return mismatch(node, "unary '-' needs a number", kinds);
      }
      return kinds[0];
    case NodeKind::kBinary:
      return binary_type(node, kinds, numbers, numeric);
    case NodeKind::kConditional:
      if (kinds[0] != ValueKind::kBool) {
        return mismatch(node, "'?' needs a bool condition", kinds);
      }
      if (kinds[1] == kinds[2]) {
        return kinds[1];
      }
      if (is_number(kinds[1]) && is_number(kinds[2])) {
        return ValueKind::kFloat64;
      }
      return mismatch(node, "'?' branches need the same type", kinds);
    case NodeKind::kCall: {
      const auto name = std::string(function_name(node.function));
      if (!numbers) {
        return mismatch(node, name + "() needs numbers", kinds);
      }
      switch (node.function) {
      case Function::kAbs:
        return kinds[0];
      case Function::kMin:
      case Function::kMax:
      case Function::kClamp:
        return numeric();
      case Function::kFloor:
      case Function::kCeil:
      case Function::kRound:
        return ValueKind::kInt64;
      default:
        return ValueKind::kFloat64;
      }
    }
    case NodeKind::kName:
      break;
    }
    throw std::logic_error("expression node kind is invalid");
  }

  template <typename Numeric>
  std::optional<ValueKind> binary_type(const Node &node,
                                       const std::vector<ValueKind> &kinds,
                                       bool numbers, const Numeric &numeric) {
    std::string what = "'";
    what += operator_text(node.op);
    what += "' ";
    switch (node.op) {
    case Operator::kAdd:
    case Operator::kSubtract:
    case Operator::kMultiply:
      if (!numbers) {
        return mismatch(node, what + "needs numbers", kinds);
      }
      return numeric();
    case Operator::kDivide:
      if (!numbers) {
        return mismatch(node, what + "needs numbers", kinds);
      }
      return ValueKind::kFloat64;
    case Operator::kRemainder:
      if (kinds[0] != ValueKind::kInt64 || kinds[1] != ValueKind::kInt64) {
        return mismatch(node, what + "needs int64 operands", kinds);
      }
      return ValueKind::kInt64;
    case Operator::kLess:
    case Operator::kLessEqual:
    case Operator::kGreater:
    case Operator::kGreaterEqual:
      if (!numbers) {
        return mismatch(node, what + "needs numbers", kinds);
      }
      return ValueKind::kBool;
    case Operator::kEqual:
    case Operator::kNotEqual:
      if (kinds[0] != kinds[1] && !numbers) {
        return mismatch(node, what + "needs two values of the same type",
                        kinds);
      }
      return ValueKind::kBool;
    case Operator::kAnd:
    case Operator::kOr:
      if (kinds[0] != ValueKind::kBool || kinds[1] != ValueKind::kBool) {
        return mismatch(node, what + "needs bools", kinds);
      }
      return ValueKind::kBool;
    default:
      break;
    }
    throw std::logic_error("expression binary operator is invalid");
  }

  std::optional<ValueKind> resolve(std::size_t index) {
    const auto &node = tree_.nodes[index];
    const auto &segments = node.segments;
    const auto &declared = node.source == Source::kParameter
                               ? environment_.parameters
                               : environment_.keys;
    const auto written =
        std::string(node.source == Source::kParameter ? "param." : "key.") +
        [&] {
          std::string joined;
          for (const auto &segment : segments) {
            joined += (joined.empty() ? "" : ".") + segment;
          }
          return joined;
        }();
    // The longest declared name wins; the remaining segments are fields.
    for (std::size_t count = segments.size(); count > 0; --count) {
      std::string candidate;
      for (std::size_t item = 0; item < count; ++item) {
        candidate += (item == 0 ? "" : ".") + segments[item];
      }
      const auto found = declared.find(candidate);
      if (found == declared.end()) {
        continue;
      }
      validate_type(found->second);
      auto &resolved = out_.names[index];
      resolved.reference = Reference{node.source, candidate};
      const ValueType *type = &found->second;
      auto current = type->kind;
      for (std::size_t item = count; item < segments.size(); ++item) {
        const auto &field = segments[item];
        if (current == ValueKind::kPose) {
          const auto *pose = std::find_if(
              kPoseFields.begin(), kPoseFields.end(),
              [&](const auto &entry) { return entry.first == field; });
          if (pose != kPoseFields.end()) {
            resolved.pose_field = pose->second;
            current = ValueKind::kFloat64;
            continue;
          }
        } else if (current == ValueKind::kRecord &&
                   resolved.pose_field == PoseField::kNone) {
          const auto next =
              std::find_if(type->fields.begin(), type->fields.end(),
                           [&](const auto &row) { return row.name == field; });
          if (next != type->fields.end()) {
            resolved.record_path.push_back(field);
            type = &next->type;
            current = type->kind;
            continue;
          }
        }
        report(node, "unknown_field",
               "'" + field + "' is not a field of a " +
                   std::string(kind_name(current)),
               written);
        return std::nullopt;
      }
      if (current == ValueKind::kPose || current == ValueKind::kRecord) {
        report(node, "type_mismatch",
               written + " is a whole " + std::string(kind_name(current)) +
                   "; read one of its fields",
               written);
        return std::nullopt;
      }
      out_.references.emplace(resolved.reference, found->second);
      out_.kinds[index] = current;
      return current;
    }
    report(node, "unknown_name", written + " is not declared", written);
    return std::nullopt;
  }

  const ParsedTree &tree_;
  const std::string &text_;
  const TypeEnvironment &environment_;
  CheckedTree &out_;
  std::vector<bool> constant_;
};

Value to_value(Scalar scalar) {
  return std::visit([](auto &&item) { return Value{std::move(item)}; },
                    std::move(scalar));
}

} // namespace

std::variant<ParsedExpression, Diagnostic> parse(std::string_view text,
                                                 const Limits &limits) {
  require_narrowing(limits.text_length, kMaxTextLength, "text_length");
  require_narrowing(limits.nesting_depth, kMaxNestingDepth, "nesting_depth");
  require_narrowing(limits.tokens, kMaxTokens, "tokens");
  require_narrowing(limits.syntax_nodes, kMaxSyntaxNodes, "syntax_nodes");
  require_narrowing(limits.evaluation_steps, kMaxEvaluationSteps,
                    "evaluation_steps");
  const auto too_long = [&](std::size_t length) {
    return Diagnostic{"text_too_long",
                      "expression is longer than " +
                          std::to_string(limits.text_length) + " characters",
                      limits.text_length,
                      length,
                      {}};
  };
  // A UTF-8 character has at most four bytes, so longer text is rejected
  // before it is scanned; its excess length is then unknown.
  if (text.size() > limits.text_length * 4U) {
    return too_long(0);
  }
  if (const auto invalid = first_invalid_utf8(text)) {
    return Diagnostic{"invalid_utf8",
                      "expression text is not valid UTF-8",
                      code_points(text, *invalid),
                      1,
                      {}};
  }
  if (const auto count = code_points(text, text.size());
      count > limits.text_length) {
    return too_long(count - limits.text_length);
  }
  auto tree = std::make_shared<ParsedTree>();
  tree->text = std::string(text);
  tree->limits = limits;
  try {
    Parser(*tree, tokenize(tree->text, limits)).run();
  } catch (const DiagnosticError &error) {
    return error.diagnostic;
  }
  return ParsedExpression(std::move(tree));
}

std::variant<CheckedExpression, std::vector<Diagnostic>>
check(const ParsedExpression &parsed, const TypeEnvironment &environment,
      ValueKind destination) {
  if (destination == ValueKind::kPose || destination == ValueKind::kRecord) {
    throw std::invalid_argument(
        "expression destination must be bool, int64, float64 or string");
  }
  auto tree = std::make_shared<CheckedTree>();
  tree->parsed = parsed.tree_;
  tree->destination = destination;
  Checker checker(parsed.tree_, environment, *tree);
  const auto kind = checker.run();
  auto diagnostics = std::move(checker.diagnostics);
  if (kind && *kind != destination &&
      !(*kind == ValueKind::kInt64 && destination == ValueKind::kFloat64)) {
    const auto &root = tree->parsed->nodes[tree->parsed->root];
    diagnostics.push_back(make_diagnostic(
        tree->parsed->text, "destination_type_mismatch",
        "expression result is " + std::string(kind_name(*kind)) +
            " but the destination needs " + std::string(kind_name(destination)),
        root.begin, root.end));
  }
  if (!diagnostics.empty()) {
    std::stable_sort(diagnostics.begin(), diagnostics.end(),
                     [](const Diagnostic &left, const Diagnostic &right) {
                       return left.position < right.position;
                     });
    return diagnostics;
  }
  return CheckedExpression(std::move(tree));
}

std::variant<Value, Diagnostic> evaluate(const CheckedExpression &checked,
                                         const ValueSnapshot &values) {
  const auto &tree = *checked.tree_;
  const auto &parsed = *tree.parsed;
  try {
    auto result = Evaluator(parsed, tree.kinds, tree.names, tree.references,
                            &values, false)
                      .run(parsed.root);
    if (tree.destination == ValueKind::kFloat64 && is_int(result)) {
      return Value{as_double(result)};
    }
    return to_value(std::move(result));
  } catch (const DiagnosticError &error) {
    return error.diagnostic;
  }
}

ParsedExpression::ParsedExpression(
    std::shared_ptr<const detail::ParsedTree> tree)
    : tree_(std::move(tree)) {}

std::string_view ParsedExpression::text() const noexcept { return tree_->text; }

CheckedExpression::CheckedExpression(
    std::shared_ptr<const detail::CheckedTree> tree)
    : tree_(std::move(tree)) {}

std::string_view CheckedExpression::text() const noexcept {
  return tree_->parsed->text;
}

ValueKind CheckedExpression::result_kind() const noexcept {
  return tree_->destination;
}

const std::map<Reference, ValueType> &
CheckedExpression::references() const noexcept {
  return tree_->references;
}

} // namespace manyforge::deployment::expression
