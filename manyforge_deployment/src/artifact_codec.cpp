#include "manyforge_deployment/artifact_codec.hpp"

#include <yaml.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {
namespace {

[[nodiscard]] std::string mark_suffix(const yaml_mark_t& mark) {
  return " at line " + std::to_string(mark.line + 1U) + ", column " +
         std::to_string(mark.column + 1U);
}

[[noreturn]] void fail(std::string message, const yaml_mark_t& mark) {
  throw ArtifactCodecError(std::move(message) + mark_suffix(mark));
}

[[nodiscard]] std::string bytes(const yaml_char_t* value, std::size_t length) {
  if (value == nullptr) {
    return {};
  }
  return {reinterpret_cast<const char*>(value), length};
}

[[nodiscard]] std::string ascii_lower(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char character : value) {
    if (character >= 'A' && character <= 'Z') {
      result.push_back(static_cast<char>(character - 'A' + 'a'));
    } else {
      result.push_back(character);
    }
  }
  return result;
}

[[nodiscard]] bool is_json_number_syntax(std::string_view value) noexcept {
  std::size_t cursor = 0U;
  if (cursor < value.size() && value[cursor] == '-') {
    ++cursor;
  }
  if (cursor == value.size()) {
    return false;
  }

  if (value[cursor] == '0') {
    ++cursor;
    if (cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9') {
      return false;
    }
  } else {
    if (value[cursor] < '1' || value[cursor] > '9') {
      return false;
    }
    while (cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9') {
      ++cursor;
    }
  }

  if (cursor < value.size() && value[cursor] == '.') {
    ++cursor;
    const auto fraction_start = cursor;
    while (cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9') {
      ++cursor;
    }
    if (cursor == fraction_start) {
      return false;
    }
  }

  if (cursor < value.size() && (value[cursor] == 'e' || value[cursor] == 'E')) {
    ++cursor;
    if (cursor < value.size() && (value[cursor] == '+' || value[cursor] == '-')) {
      ++cursor;
    }
    const auto exponent_start = cursor;
    while (cursor < value.size() && value[cursor] >= '0' && value[cursor] <= '9') {
      ++cursor;
    }
    if (cursor == exponent_start) {
      return false;
    }
  }

  return cursor == value.size();
}

[[nodiscard]] bool is_integer_syntax(std::string_view value) noexcept {
  return value.find_first_of(".eE") == std::string_view::npos;
}

[[nodiscard]] double parse_json_number(std::string_view value, const yaml_mark_t& mark) {
  if (is_integer_syntax(value)) {
    std::int64_t integer = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), integer);
    if (error != std::errc{} || end != value.data() + value.size() ||
        integer < -kMaxArtifactSafeInteger || integer > kMaxArtifactSafeInteger) {
      fail("integer is outside the lossless I-JSON range", mark);
    }
  }

  double result = 0.0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result,
                                            std::chars_format::general);
  if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(result)) {
    fail("number is not a finite I-JSON binary64 value", mark);
  }
  if (result == 0.0 && std::signbit(result)) {
    fail("negative zero is forbidden", mark);
  }
  if (std::trunc(result) == result &&
      (result < -static_cast<double>(kMaxArtifactSafeInteger) ||
       result > static_cast<double>(kMaxArtifactSafeInteger))) {
    fail("integer-valued number is outside the lossless I-JSON range", mark);
  }
  return result;
}

[[nodiscard]] CanonicalValue decode_scalar(const yaml_event_t& event) {
  const auto& scalar = event.data.scalar;
  const auto mark = event.start_mark;
  if (scalar.anchor != nullptr) {
    fail("YAML anchors are forbidden", mark);
  }
  if (scalar.tag != nullptr) {
    fail("explicit YAML tags are forbidden", mark);
  }

  const auto value = bytes(scalar.value, scalar.length);
  if (scalar.style != YAML_PLAIN_SCALAR_STYLE) {
    return CanonicalValue(value);
  }

  if (value.empty() || value == "~") {
    fail("YAML-only null spellings are forbidden; use plain null", mark);
  }
  if (value == "null") {
    return CanonicalValue(nullptr);
  }
  if (value == "true") {
    return CanonicalValue(true);
  }
  if (value == "false") {
    return CanonicalValue(false);
  }
  const auto lower = ascii_lower(value);
  if (lower == ".nan" || lower == "+.nan" || lower == "-.nan" || lower == ".inf" ||
      lower == "+.inf" || lower == "-.inf") {
    fail("non-finite YAML numbers are forbidden", mark);
  }
  if (is_json_number_syntax(value)) {
    return CanonicalValue(parse_json_number(value, mark));
  }
  return CanonicalValue(value);
}

struct ContainerFrame {
  enum class Kind : std::uint8_t { kSequence, kMapping };

  Kind kind;
  yaml_mark_t start_mark;
  CanonicalValue::Array sequence;
  CanonicalValue::Object mapping;
  std::optional<std::string> pending_key;
  // Artifact input is adversarial. A tree keeps duplicate detection at a
  // deterministic O(log n) bound instead of relying on hash quality.
  std::set<std::string, std::less<>> mapping_keys;
};

class CandidateBuilder final {
 public:
  void begin_document(const yaml_event_t& event) {
    if (document_count_ != 0U) {
      fail("exactly one YAML document is required", event.start_mark);
    }
    if (event.data.document_start.version_directive != nullptr ||
        event.data.document_start.tag_directives.start !=
            event.data.document_start.tag_directives.end) {
      fail("YAML directives are forbidden", event.start_mark);
    }
    ++document_count_;
  }

  void end_document(const yaml_event_t& event) const {
    if (!stack_.empty()) {
      fail("document ended inside a collection", event.start_mark);
    }
    if (!root_.has_value()) {
      fail("empty YAML documents are forbidden", event.start_mark);
    }
  }

  void begin_sequence(const yaml_event_t& event) {
    reject_collection_metadata(event.data.sequence_start.anchor, event.data.sequence_start.tag,
                               event.start_mark);
    begin_node(event.start_mark);
    stack_.push_back({ContainerFrame::Kind::kSequence, event.start_mark, {}, {}, {}, {}});
  }

  void end_sequence(const yaml_event_t& event) {
    if (stack_.empty() || stack_.back().kind != ContainerFrame::Kind::kSequence) {
      fail("unexpected YAML sequence end", event.start_mark);
    }
    auto values = std::move(stack_.back().sequence);
    stack_.pop_back();
    append(CanonicalValue(std::move(values)), event.start_mark);
  }

  void begin_mapping(const yaml_event_t& event) {
    reject_collection_metadata(event.data.mapping_start.anchor, event.data.mapping_start.tag,
                               event.start_mark);
    begin_node(event.start_mark);
    stack_.push_back({ContainerFrame::Kind::kMapping, event.start_mark, {}, {}, {}, {}});
  }

  void end_mapping(const yaml_event_t& event) {
    if (stack_.empty() || stack_.back().kind != ContainerFrame::Kind::kMapping) {
      fail("unexpected YAML mapping end", event.start_mark);
    }
    if (stack_.back().pending_key.has_value()) {
      fail("mapping key has no value", event.start_mark);
    }
    auto values = std::move(stack_.back().mapping);
    stack_.pop_back();
    append(CanonicalValue(std::move(values)), event.start_mark);
  }

  void scalar(const yaml_event_t& event) {
    if (event.data.scalar.length > kMaxArtifactScalarBytes) {
      fail("scalar exceeds the v1 byte limit", event.start_mark);
    }
    consume_node(event.start_mark);
    if (!stack_.empty() && stack_.back().kind == ContainerFrame::Kind::kMapping &&
        !stack_.back().pending_key.has_value() &&
        event.data.scalar.style == YAML_PLAIN_SCALAR_STYLE &&
        bytes(event.data.scalar.value, event.data.scalar.length) == "<<") {
      fail("YAML merge keys are forbidden", event.start_mark);
    }
    try {
      append(decode_scalar(event), event.start_mark);
    } catch (const CanonicalValueError& error) {
      fail(error.what(), event.start_mark);
    }
  }

  [[nodiscard]] CanonicalValue finish(const yaml_mark_t& mark) {
    if (document_count_ != 1U || !root_.has_value()) {
      fail("exactly one non-empty YAML document is required", mark);
    }
    return std::move(*root_);
  }

 private:
  void consume_node(const yaml_mark_t& mark) {
    if (node_count_ >= kMaxArtifactNodeCount) {
      fail("artifact exceeds the v1 node-count limit", mark);
    }
    ++node_count_;
  }

  void begin_node(const yaml_mark_t& mark) {
    if (stack_.size() >= kMaxArtifactNestingDepth) {
      fail("artifact exceeds the v1 nesting-depth limit", mark);
    }
    consume_node(mark);
  }

  static void reject_collection_metadata(const yaml_char_t* anchor, const yaml_char_t* tag,
                                         const yaml_mark_t& mark) {
    if (anchor != nullptr) {
      fail("YAML anchors are forbidden", mark);
    }
    if (tag != nullptr) {
      fail("explicit YAML tags are forbidden", mark);
    }
  }

  void append(CanonicalValue value, const yaml_mark_t& mark) {
    if (stack_.empty()) {
      if (root_.has_value()) {
        fail("document contains more than one root value", mark);
      }
      root_.emplace(std::move(value));
      return;
    }

    auto& container = stack_.back();
    if (container.kind == ContainerFrame::Kind::kSequence) {
      container.sequence.emplace_back(std::move(value));
      return;
    }

    if (!container.pending_key.has_value()) {
      const auto* key = std::get_if<std::string>(&value.storage());
      if (key == nullptr) {
        fail("mapping keys must be strings", mark);
      }
      if (!container.mapping_keys.emplace(*key).second) {
        fail("duplicate mapping key: " + *key, mark);
      }
      container.pending_key = *key;
      return;
    }

    container.mapping.emplace_back(std::move(*container.pending_key), std::move(value));
    container.pending_key.reset();
  }

  std::size_t document_count_ = 0U;
  std::size_t node_count_ = 0U;
  std::vector<ContainerFrame> stack_;
  std::optional<CanonicalValue> root_;
};

[[nodiscard]] std::string parser_problem(const yaml_parser_t& parser) {
  std::ostringstream message;
  message << "invalid YAML";
  if (parser.problem != nullptr) {
    message << ": " << parser.problem;
  }
  message << mark_suffix(parser.problem_mark);
  return message.str();
}

}  // namespace

ParsedArtifactCandidate parse_strict_json_model_yaml(std::string_view yaml_bytes) {
  if (yaml_bytes.empty()) {
    throw ArtifactCodecError("exactly one non-empty YAML document is required");
  }
  if (yaml_bytes.size() > kMaxArtifactEncodedBytes) {
    throw ArtifactCodecError("artifact exceeds the v1 encoded-byte limit");
  }
  if (yaml_bytes.find('\0') != std::string_view::npos) {
    throw ArtifactCodecError("NUL bytes are forbidden in YAML input");
  }

  yaml_parser_t parser;
  if (yaml_parser_initialize(&parser) == 0) {
    throw std::bad_alloc();
  }
  yaml_parser_set_input_string(&parser, reinterpret_cast<const unsigned char*>(yaml_bytes.data()),
                               yaml_bytes.size());

  CandidateBuilder builder;
  bool stream_ended = false;
  yaml_mark_t last_mark{};

  try {
    while (!stream_ended) {
      yaml_event_t event;
      if (yaml_parser_parse(&parser, &event) == 0) {
        throw ArtifactCodecError(parser_problem(parser));
      }
      last_mark = event.end_mark;
      try {
        switch (event.type) {
          case YAML_NO_EVENT:
          case YAML_STREAM_START_EVENT:
            break;
          case YAML_STREAM_END_EVENT:
            stream_ended = true;
            break;
          case YAML_DOCUMENT_START_EVENT:
            builder.begin_document(event);
            break;
          case YAML_DOCUMENT_END_EVENT:
            builder.end_document(event);
            break;
          case YAML_ALIAS_EVENT:
            fail("YAML aliases are forbidden", event.start_mark);
          case YAML_SCALAR_EVENT:
            builder.scalar(event);
            break;
          case YAML_SEQUENCE_START_EVENT:
            builder.begin_sequence(event);
            break;
          case YAML_SEQUENCE_END_EVENT:
            builder.end_sequence(event);
            break;
          case YAML_MAPPING_START_EVENT:
            builder.begin_mapping(event);
            break;
          case YAML_MAPPING_END_EVENT:
            builder.end_mapping(event);
            break;
        }
      } catch (...) {
        yaml_event_delete(&event);
        throw;
      }
      yaml_event_delete(&event);
    }
    auto root = builder.finish(last_mark);
    yaml_parser_delete(&parser);
    return ParsedArtifactCandidate(std::move(root));
  } catch (...) {
    yaml_parser_delete(&parser);
    throw;
  }
}

}  // namespace manyforge::deployment
