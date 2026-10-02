#include <gtest/gtest.h>
#include <yaml.h>

#include <cstddef>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "manyforge_deployment/program_value_path.hpp"

namespace manyforge::deployment {
namespace {

// The shared conformance file is the grammar's executable specification; the
// Python validator and the Composer editor run the same cases.
struct Yaml final {
  std::string scalar;
  std::vector<Yaml> items;
  std::vector<std::string> keys;
  std::vector<Yaml> values;

  const Yaml &at(std::string_view key) const {
    for (std::size_t index = 0; index < keys.size(); ++index) {
      if (keys[index] == key) {
        return values[index];
      }
    }
    throw std::runtime_error("conformance entry lacks '" + std::string(key) +
                             "'");
  }
  bool has(std::string_view key) const {
    for (const auto &name : keys) {
      if (name == key) {
        return true;
      }
    }
    return false;
  }
};

Yaml convert(yaml_document_t &document, yaml_node_t *node) {
  Yaml result;
  if (node->type == YAML_SCALAR_NODE) {
    result.scalar.assign(reinterpret_cast<const char *>(node->data.scalar.value),
                         node->data.scalar.length);
  } else if (node->type == YAML_SEQUENCE_NODE) {
    for (auto *item = node->data.sequence.items.start;
         item < node->data.sequence.items.top; ++item) {
      result.items.push_back(
          convert(document, yaml_document_get_node(&document, *item)));
    }
  } else if (node->type == YAML_MAPPING_NODE) {
    for (auto *pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top; ++pair) {
      result.keys.push_back(
          convert(document, yaml_document_get_node(&document, pair->key))
              .scalar);
      result.values.push_back(
          convert(document, yaml_document_get_node(&document, pair->value)));
    }
  } else {
    throw std::runtime_error("conformance file has an empty YAML node");
  }
  return result;
}

const Yaml &conformance() {
  static const Yaml root = [] {
    std::ifstream stream(MANYFORGE_VALUE_PATH_CONFORMANCE, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    yaml_parser_t parser;
    yaml_parser_initialize(&parser);
    yaml_parser_set_input_string(
        &parser, reinterpret_cast<const unsigned char *>(text.data()),
        text.size());
    yaml_document_t document;
    if (yaml_parser_load(&parser, &document) == 0) {
      yaml_parser_delete(&parser);
      throw std::runtime_error("conformance file is not valid YAML");
    }
    auto converted = convert(document, yaml_document_get_root_node(&document));
    yaml_document_delete(&document);
    yaml_parser_delete(&parser);
    return converted;
  }();
  return root;
}

TEST(ProgramValuePathTest, LimitsMatchTheConformanceFile) {
  const auto &limits = conformance().at("limits");
  EXPECT_EQ(std::stoul(limits.at("max_length").scalar), kMaxValuePathLength);
  EXPECT_EQ(std::stoul(limits.at("max_segments").scalar),
            kMaxValuePathSegments);
  EXPECT_EQ(std::stoul(limits.at("max_name_length").scalar),
            kMaxValuePathNameLength);
  EXPECT_EQ(std::stoul(limits.at("max_index_digits").scalar),
            kMaxValuePathIndexDigits);
}

TEST(ProgramValuePathTest, ParsesAndFormatsEveryValidCase) {
  const auto &cases = conformance().at("valid").items;
  ASSERT_FALSE(cases.empty());
  for (const auto &entry : cases) {
    const std::string &text = entry.at("path").scalar;
    ValuePath expected;
    for (const auto &segment : entry.at("segments").items) {
      if (segment.has("name")) {
        expected.emplace_back(segment.at("name").scalar);
      } else {
        expected.emplace_back(
            static_cast<std::size_t>(std::stoull(segment.at("index").scalar)));
      }
    }
    EXPECT_EQ(parse_value_path(text), expected) << text;
    EXPECT_EQ(format_value_path(expected), text) << text;
  }
}

TEST(ProgramValuePathTest, RefusesEveryInvalidCaseWithItsCode) {
  const auto &cases = conformance().at("invalid").items;
  ASSERT_FALSE(cases.empty());
  for (const auto &entry : cases) {
    const std::string &text = entry.at("path").scalar;
    try {
      static_cast<void>(parse_value_path(text));
      ADD_FAILURE() << "accepted: " << text;
    } catch (const ValuePathError &error) {
      EXPECT_EQ(error.code(), entry.at("code").scalar) << text;
    }
  }
}

} // namespace
} // namespace manyforge::deployment
