#include "manyforge_deployment/model_source.hpp"

#include <expat.h>
#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw ModelSourceError(std::move(message));
}

[[nodiscard]] std::string store_key_for_digest(
    const core::ContentDigest& digest) {
  if (!is_sha256_digest(digest.value())) {
    fail("model source declaration contains a non-SHA-256 digest");
  }
  return "sha256/" + digest.value().substr(7U);
}

void validate_source_symbol(std::string_view symbol,
                            std::string_view field_name) {
  if (symbol.empty()) {
    fail(std::string(field_name) + " contains an empty source symbol");
  }
  utf8proc_ssize_t offset = 0;
  std::size_t code_points = 0U;
  while (offset < static_cast<utf8proc_ssize_t>(symbol.size())) {
    utf8proc_int32_t code_point = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(symbol.data()) + offset,
        static_cast<utf8proc_ssize_t>(symbol.size()) - offset, &code_point);
    if (consumed <= 0) {
      fail(std::string(field_name) + " contains invalid UTF-8");
    }
    const auto category = utf8proc_category(code_point);
    if (code_point == 0 || category == UTF8PROC_CATEGORY_CC ||
        category == UTF8PROC_CATEGORY_CF ||
        category == UTF8PROC_CATEGORY_ZS ||
        category == UTF8PROC_CATEGORY_ZL ||
        category == UTF8PROC_CATEGORY_ZP) {
      fail(std::string(field_name) +
           " contains whitespace, control, or format code points");
    }
    offset += consumed;
    ++code_points;
  }
  if (code_points > core::kMaxModelAssetSourceSymbolCodePoints) {
    fail(std::string(field_name) + " exceeds the source-symbol bound");
  }
  if (!is_utf8_nfc(symbol)) {
    fail(std::string(field_name) + " is not valid UTF-8 encoded in NFC");
  }
}

struct XmlNode final {
  std::string name;
  std::vector<std::pair<std::string, std::string>> attributes;
  std::vector<XmlNode> children;
  bool has_non_whitespace_text = false;
};

struct XmlBudget final {
  std::size_t element_count = 0U;
  std::size_t attribute_count = 0U;
};

struct XmlParseState final {
  std::optional<XmlNode> root;
  std::vector<XmlNode*> stack;
  XmlBudget* budget = nullptr;
  std::string error;
  XML_Parser parser = nullptr;
};

void stop_with_error(XmlParseState& state, std::string message) noexcept {
  if (state.error.empty()) {
    state.error = std::move(message);
  }
  if (state.parser != nullptr) {
    XML_StopParser(state.parser, XML_FALSE);
  }
}

[[nodiscard]] bool forbidden_xml_name(std::string_view name) noexcept {
  return name.empty() || name.find(':') != std::string_view::npos ||
         name == "xmlns" || name.starts_with("xmlns:");
}

void XMLCALL start_element(void* user_data, const XML_Char* raw_name,
                           const XML_Char** raw_attributes) noexcept {
  auto& state = *static_cast<XmlParseState*>(user_data);
  try {
    if (state.stack.size() >= kMaxModelSourceXmlDepth) {
      stop_with_error(state, "model source XML exceeds the nesting bound");
      return;
    }
    if (++state.budget->element_count > kMaxModelSourceXmlElementCount) {
      stop_with_error(state, "model source XML exceeds the element bound");
      return;
    }
    const std::string_view name(raw_name);
    if (forbidden_xml_name(name)) {
      stop_with_error(state,
                      "model source XML namespaces are not supported");
      return;
    }

    XmlNode node;
    node.name = std::string(name);
    std::unordered_set<std::string> seen_attributes;
    for (std::size_t index = 0U; raw_attributes[index] != nullptr;
         index += 2U) {
      const std::string_view attribute_name(raw_attributes[index]);
      const std::string_view attribute_value(raw_attributes[index + 1U]);
      if (forbidden_xml_name(attribute_name)) {
        stop_with_error(state,
                        "model source XML namespaces are not supported");
        return;
      }
      if (!seen_attributes.emplace(attribute_name).second) {
        stop_with_error(state,
                        "model source XML contains a duplicate attribute");
        return;
      }
      if (++state.budget->attribute_count >
          kMaxModelSourceXmlAttributeCount) {
        stop_with_error(
            state,
            "model source XML exceeds the attribute-count bound");
        return;
      }
      node.attributes.emplace_back(attribute_name, attribute_value);
    }

    XmlNode* inserted = nullptr;
    if (state.stack.empty()) {
      if (state.root.has_value()) {
        stop_with_error(state,
                        "model source XML has more than one root element");
        return;
      }
      state.root.emplace(std::move(node));
      inserted = &*state.root;
    } else {
      state.stack.back()->children.emplace_back(std::move(node));
      inserted = &state.stack.back()->children.back();
    }
    state.stack.push_back(inserted);
  } catch (const std::exception& error) {
    stop_with_error(state,
                    std::string("model source XML parser failed: ") +
                        error.what());
  } catch (...) {
    stop_with_error(state, "model source XML parser failed");
  }
}

void XMLCALL end_element(void* user_data, const XML_Char*) noexcept {
  auto& state = *static_cast<XmlParseState*>(user_data);
  if (!state.stack.empty()) {
    state.stack.pop_back();
  }
}

void XMLCALL character_data(void* user_data, const XML_Char* data,
                            int length) noexcept {
  auto& state = *static_cast<XmlParseState*>(user_data);
  try {
    if (length < 0) {
      stop_with_error(state, "model source XML reported invalid text");
      return;
    }
    if (state.stack.empty()) {
      return;
    }
    if (std::any_of(data, data + length, [](char byte) {
          return byte != ' ' && byte != '\t' && byte != '\r' &&
                 byte != '\n';
        })) {
      state.stack.back()->has_non_whitespace_text = true;
    }
  } catch (...) {
    stop_with_error(state, "model source XML parser failed");
  }
}

void XMLCALL reject_doctype(void* user_data, const XML_Char*,
                            const XML_Char*, const XML_Char*, int) noexcept {
  stop_with_error(*static_cast<XmlParseState*>(user_data),
                  "model source XML document types are forbidden");
}

void XMLCALL reject_entity(void* user_data, const XML_Char*, int,
                           const XML_Char*, int, const XML_Char*,
                           const XML_Char*, const XML_Char*,
                           const XML_Char*) noexcept {
  stop_with_error(*static_cast<XmlParseState*>(user_data),
                  "model source XML entity declarations are forbidden");
}

int XMLCALL reject_external_entity(XML_Parser parser, const XML_Char*,
                                   const XML_Char*, const XML_Char*,
                                   const XML_Char*) noexcept {
  auto* state =
      static_cast<XmlParseState*>(XML_GetUserData(parser));
  if (state != nullptr) {
    stop_with_error(*state,
                    "model source XML external entities are forbidden");
  }
  return XML_STATUS_ERROR;
}

void XMLCALL reject_processing_instruction(void* user_data, const XML_Char*,
                                           const XML_Char*) noexcept {
  stop_with_error(
      *static_cast<XmlParseState*>(user_data),
      "model source XML processing instructions are forbidden");
}

[[nodiscard]] XmlNode parse_xml(std::string_view bytes,
                                std::string_view entrypoint_kind,
                                XmlBudget& budget) {
  if (bytes.size() > kMaxModelSourceXmlEntrypointBytes ||
      bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    fail(std::string(entrypoint_kind) +
         " XML exceeds the compiler-owned byte bound");
  }

  struct ParserDeleter final {
    void operator()(XML_Parser parser) const noexcept {
      if (parser != nullptr) {
        XML_ParserFree(parser);
      }
    }
  };
  std::unique_ptr<std::remove_pointer_t<XML_Parser>, ParserDeleter> parser(
      XML_ParserCreate(nullptr));
  if (!parser) {
    fail("could not allocate the model source XML parser");
  }

  XmlParseState state;
  state.budget = &budget;
  state.parser = parser.get();
  XML_SetUserData(parser.get(), &state);
  XML_SetElementHandler(parser.get(), start_element, end_element);
  XML_SetCharacterDataHandler(parser.get(), character_data);
  XML_SetStartDoctypeDeclHandler(parser.get(), reject_doctype);
  XML_SetEntityDeclHandler(parser.get(), reject_entity);
  XML_SetExternalEntityRefHandler(parser.get(), reject_external_entity);
  XML_SetProcessingInstructionHandler(parser.get(),
                                      reject_processing_instruction);
  XML_SetParamEntityParsing(parser.get(), XML_PARAM_ENTITY_PARSING_NEVER);

  const auto status = XML_Parse(parser.get(), bytes.data(),
                                static_cast<int>(bytes.size()), XML_TRUE);
  if (status != XML_STATUS_OK || !state.error.empty()) {
    if (!state.error.empty()) {
      fail(std::move(state.error));
    }
    std::ostringstream message;
    message << entrypoint_kind << " XML is malformed at line "
            << XML_GetCurrentLineNumber(parser.get()) << ": "
            << XML_ErrorString(XML_GetErrorCode(parser.get()));
    fail(message.str());
  }
  if (!state.root.has_value() || !state.stack.empty()) {
    fail(std::string(entrypoint_kind) +
         " XML does not contain one complete root element");
  }
  return std::move(*state.root);
}

struct ColladaParseState final {
  XmlBudget* budget = nullptr;
  XML_Parser parser = nullptr;
  std::size_t depth = 0U;
  bool saw_root = false;
  std::string document_namespace;
  std::string error;
};

[[nodiscard]] std::string_view xml_local_name(
    std::string_view expanded_name) noexcept {
  constexpr char kNamespaceSeparator = '\x1f';
  const auto separator = expanded_name.rfind(kNamespaceSeparator);
  return separator == std::string_view::npos
             ? expanded_name
             : expanded_name.substr(separator + 1U);
}

[[nodiscard]] std::string_view xml_namespace_name(
    std::string_view expanded_name) noexcept {
  constexpr char kNamespaceSeparator = '\x1f';
  const auto separator = expanded_name.rfind(kNamespaceSeparator);
  return separator == std::string_view::npos
             ? std::string_view{}
             : expanded_name.substr(0U, separator);
}

void stop_collada_with_error(ColladaParseState& state,
                             std::string message) noexcept {
  if (state.error.empty()) {
    state.error = std::move(message);
  }
  if (state.parser != nullptr) {
    XML_StopParser(state.parser, XML_FALSE);
  }
}

void XMLCALL start_collada_element(
    void* user_data, const XML_Char* raw_name,
    const XML_Char** raw_attributes) noexcept {
  auto& state = *static_cast<ColladaParseState*>(user_data);
  try {
    if (state.depth >= kMaxModelSourceColladaXmlDepth) {
      stop_collada_with_error(
          state, "Collada mesh exceeds the XML nesting bound");
      return;
    }
    if (++state.budget->element_count >
        kMaxModelSourceColladaXmlElementCount) {
      stop_collada_with_error(
          state, "model source XML exceeds the element bound");
      return;
    }

    const std::string_view expanded_name(raw_name);
    const auto local_name = xml_local_name(expanded_name);
    const auto namespace_name = xml_namespace_name(expanded_name);
    if (!state.saw_root) {
      if (local_name != "COLLADA" ||
          namespace_name !=
              "http://www.collada.org/2005/11/COLLADASchema") {
        stop_collada_with_error(
            state, "Collada mesh has an unsupported root or namespace");
        return;
      }
      bool is_collada_1_4_1 = false;
      for (std::size_t index = 0U; raw_attributes[index] != nullptr;
           index += 2U) {
        if (xml_namespace_name(raw_attributes[index]).empty() &&
            xml_local_name(raw_attributes[index]) == "version" &&
            std::string_view(raw_attributes[index + 1U]) == "1.4.1") {
          is_collada_1_4_1 = true;
          break;
        }
      }
      if (!is_collada_1_4_1) {
        stop_collada_with_error(
            state, "Collada mesh must declare version 1.4.1");
        return;
      }
      state.saw_root = true;
      state.document_namespace = namespace_name;
    } else if (namespace_name != state.document_namespace) {
      stop_collada_with_error(
          state, "Collada mesh contains a foreign-namespace extension");
      return;
    }

    // A retained Collada mesh is admitted as one exact, self-contained
    // source artifact. Image/init_from/source_data elements and non-fragment
    // URI attributes can cause a downstream loader to consult files or
    // documents outside that retained closure, so reject them here.
    if (local_name == "image" || local_name == "init_from" ||
        local_name == "source_data" || local_name == "skeleton" ||
        local_name == "extra") {
      stop_collada_with_error(
          state, "Collada mesh contains an external-resource construct");
      return;
    }

    for (std::size_t index = 0U; raw_attributes[index] != nullptr;
         index += 2U) {
      if (++state.budget->attribute_count >
          kMaxModelSourceColladaXmlAttributeCount) {
        stop_collada_with_error(
            state, "model source XML exceeds the attribute-count bound");
        return;
      }
      const auto attribute_name =
          xml_local_name(raw_attributes[index]);
      if (!xml_namespace_name(raw_attributes[index]).empty()) {
        stop_collada_with_error(
            state, "Collada mesh contains a namespaced attribute");
        return;
      }
      const std::string_view attribute_value(raw_attributes[index + 1U]);
      // COLLADA 1.4.1 declares the following attributes as xs:anyURI (or
      // the stricter same-document URIFragmentType). `target` is a URI only
      // on these two instance elements; animation-channel target is a SID
      // path and must not be interpreted as one.
      const bool uri_attribute =
          attribute_name == "url" || attribute_name == "href" ||
          attribute_name == "source" ||
          attribute_name == "camera_node" ||
          attribute_name == "parent" ||
          attribute_name == "convex_hull_of" ||
          attribute_name == "rigid_body" ||
          (attribute_name == "target" &&
           (local_name == "instance_material" ||
            local_name == "instance_rigid_body"));
      if (uri_attribute &&
          (attribute_value.empty() ||
           !attribute_value.starts_with('#'))) {
        stop_collada_with_error(
            state, "Collada mesh references an external resource");
        return;
      }
    }
    ++state.depth;
  } catch (const std::exception& error) {
    stop_collada_with_error(
        state, std::string("Collada mesh parser failed: ") + error.what());
  } catch (...) {
    stop_collada_with_error(state, "Collada mesh parser failed");
  }
}

void XMLCALL end_collada_element(void* user_data,
                                 const XML_Char*) noexcept {
  auto& state = *static_cast<ColladaParseState*>(user_data);
  if (state.depth > 0U) {
    --state.depth;
  }
}

void XMLCALL reject_collada_doctype(void* user_data, const XML_Char*,
                                    const XML_Char*, const XML_Char*,
                                    int) noexcept {
  stop_collada_with_error(
      *static_cast<ColladaParseState*>(user_data),
      "Collada mesh document types are forbidden");
}

void XMLCALL reject_collada_entity(
    void* user_data, const XML_Char*, int, const XML_Char*, int,
    const XML_Char*, const XML_Char*, const XML_Char*,
    const XML_Char*) noexcept {
  stop_collada_with_error(
      *static_cast<ColladaParseState*>(user_data),
      "Collada mesh entity declarations are forbidden");
}

int XMLCALL reject_collada_external_entity(
    XML_Parser parser, const XML_Char*, const XML_Char*,
    const XML_Char*, const XML_Char*) noexcept {
  auto* state =
      static_cast<ColladaParseState*>(XML_GetUserData(parser));
  if (state != nullptr) {
    stop_collada_with_error(
        *state, "Collada mesh external entities are forbidden");
  }
  return XML_STATUS_ERROR;
}

void XMLCALL reject_collada_processing_instruction(
    void* user_data, const XML_Char*, const XML_Char*) noexcept {
  stop_collada_with_error(
      *static_cast<ColladaParseState*>(user_data),
      "Collada mesh processing instructions are forbidden");
}

void validate_self_contained_collada(std::string_view bytes,
                                     XmlBudget& budget) {
  if (bytes.size() > kMaxModelSourceOpaqueArtifactBytes ||
      bytes.size() >
          static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    fail("Collada mesh exceeds the compiler-owned byte bound");
  }

  struct ParserDeleter final {
    void operator()(XML_Parser parser) const noexcept {
      if (parser != nullptr) {
        XML_ParserFree(parser);
      }
    }
  };
  constexpr char kNamespaceSeparator = '\x1f';
  std::unique_ptr<std::remove_pointer_t<XML_Parser>, ParserDeleter> parser(
      XML_ParserCreateNS(nullptr, kNamespaceSeparator));
  if (!parser) {
    fail("could not allocate the Collada mesh parser");
  }

  ColladaParseState state;
  state.budget = &budget;
  state.parser = parser.get();
  XML_SetUserData(parser.get(), &state);
  XML_SetElementHandler(parser.get(), start_collada_element,
                        end_collada_element);
  XML_SetStartDoctypeDeclHandler(parser.get(),
                                 reject_collada_doctype);
  XML_SetEntityDeclHandler(parser.get(), reject_collada_entity);
  XML_SetExternalEntityRefHandler(
      parser.get(), reject_collada_external_entity);
  XML_SetProcessingInstructionHandler(parser.get(),
                                      reject_collada_processing_instruction);
  XML_SetParamEntityParsing(parser.get(), XML_PARAM_ENTITY_PARSING_NEVER);

  const auto status = XML_Parse(parser.get(), bytes.data(),
                                static_cast<int>(bytes.size()), XML_TRUE);
  if (status != XML_STATUS_OK || !state.error.empty()) {
    if (!state.error.empty()) {
      fail(std::move(state.error));
    }
    std::ostringstream message;
    message << "Collada mesh is malformed at line "
            << XML_GetCurrentLineNumber(parser.get()) << ": "
            << XML_ErrorString(XML_GetErrorCode(parser.get()));
    fail(message.str());
  }
  if (!state.saw_root || state.depth != 0U) {
    fail("Collada mesh does not contain one complete root element");
  }
}

[[nodiscard]] const std::string* find_attribute(
    const XmlNode& node, std::string_view name) {
  const auto found = std::find_if(
      node.attributes.begin(), node.attributes.end(),
      [name](const auto& item) { return item.first == name; });
  return found == node.attributes.end() ? nullptr : &found->second;
}

void require_allowed_attributes(
    const XmlNode& node,
    std::initializer_list<std::string_view> allowed,
    std::initializer_list<std::string_view> required = {}) {
  for (const auto& [name, value] : node.attributes) {
    (void)value;
    if (std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
      fail("unsupported attribute '" + name + "' on <" + node.name + ">");
    }
  }
  for (const auto name : required) {
    if (find_attribute(node, name) == nullptr) {
      fail("missing required attribute '" + std::string(name) + "' on <" +
           node.name + ">");
    }
  }
}

[[nodiscard]] const std::string& require_symbol_attribute(
    const XmlNode& node, std::string_view name) {
  const auto* value = find_attribute(node, name);
  if (value == nullptr) {
    fail("missing required attribute '" + std::string(name) + "' on <" +
         node.name + ">");
  }
  validate_source_symbol(*value, name);
  return *value;
}

[[nodiscard]] const std::string& require_logical_uri_attribute(
    const XmlNode& node, std::string_view name) {
  const auto* value = find_attribute(node, name);
  if (value == nullptr) {
    fail("missing required attribute '" + std::string(name) + "' on <" +
         node.name + ">");
  }
  if (value->empty() ||
      value->size() > core::kMaxModelAssetLogicalUriBytes ||
      !std::all_of(value->begin(), value->end(), [](unsigned char byte) {
        return byte <= 0x7fU;
      }) ||
      !value->starts_with("asset://")) {
    fail(std::string(name) +
         " must be a bounded closure-local asset:// logical URI");
  }
  return *value;
}

void require_no_text(const XmlNode& node) {
  if (node.has_non_whitespace_text) {
    fail("non-whitespace character data is forbidden in <" + node.name + ">");
  }
}

[[nodiscard]] std::vector<double> parse_numbers(std::string_view value,
                                                std::size_t minimum,
                                                std::size_t maximum,
                                                std::string_view field) {
  std::vector<double> result;
  std::size_t cursor = 0U;
  while (cursor < value.size()) {
    while (cursor < value.size() &&
           (value[cursor] == ' ' || value[cursor] == '\t' ||
            value[cursor] == '\r' || value[cursor] == '\n')) {
      ++cursor;
    }
    if (cursor == value.size()) {
      break;
    }
    const auto start = cursor;
    while (cursor < value.size() && value[cursor] != ' ' &&
           value[cursor] != '\t' && value[cursor] != '\r' &&
           value[cursor] != '\n') {
      ++cursor;
    }
    double parsed = 0.0;
    const auto token = value.substr(start, cursor - start);
    const auto [end, error] =
        std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (error != std::errc{} || end != token.data() + token.size() ||
        !std::isfinite(parsed)) {
      fail(std::string(field) + " contains a non-finite or invalid number");
    }
    if (parsed == 0.0) {
      parsed = 0.0;  // Normalize authored negative zero.
    }
    result.push_back(parsed);
    if (result.size() > maximum) {
      fail(std::string(field) + " contains too many numbers");
    }
  }
  if (result.size() < minimum) {
    fail(std::string(field) + " contains too few numbers");
  }
  return result;
}

[[nodiscard]] std::optional<double> parse_optional_scalar(
    const XmlNode& node, std::string_view name) {
  const auto* value = find_attribute(node, name);
  if (value == nullptr) {
    return std::nullopt;
  }
  return parse_numbers(*value, 1U, 1U, name).front();
}

[[nodiscard]] double parse_required_scalar(const XmlNode& node,
                                           std::string_view name) {
  const auto* value = find_attribute(node, name);
  if (value == nullptr) {
    fail("missing required attribute '" + std::string(name) + "' on <" +
         node.name + ">");
  }
  return parse_numbers(*value, 1U, 1U, name).front();
}

template <std::size_t Size>
[[nodiscard]] std::array<double, Size> numbers_as_array(
    const XmlNode& node, std::string_view name) {
  const auto* value = find_attribute(node, name);
  if (value == nullptr) {
    fail("missing required attribute '" + std::string(name) + "' on <" +
         node.name + ">");
  }
  const auto parsed = parse_numbers(*value, Size, Size, name);
  std::array<double, Size> result{};
  std::copy(parsed.begin(), parsed.end(), result.begin());
  return result;
}

[[nodiscard]] std::optional<ModelSourceVector3> parse_optional_vector3(
    const XmlNode& node, std::string_view name) {
  if (find_attribute(node, name) == nullptr) {
    return std::nullopt;
  }
  return numbers_as_array<3U>(node, name);
}

[[nodiscard]] UrdfJointType parse_joint_type(std::string_view value) {
  if (value == "revolute") {
    return UrdfJointType::kRevolute;
  }
  if (value == "continuous") {
    return UrdfJointType::kContinuous;
  }
  if (value == "prismatic") {
    return UrdfJointType::kPrismatic;
  }
  if (value == "fixed") {
    return UrdfJointType::kFixed;
  }
  if (value == "floating") {
    return UrdfJointType::kFloating;
  }
  if (value == "planar") {
    return UrdfJointType::kPlanar;
  }
  fail("URDF joint has an unsupported type");
}

[[nodiscard]] UrdfCoordinateKind coordinate_kind(UrdfJointType type) {
  switch (type) {
    case UrdfJointType::kRevolute:
    case UrdfJointType::kContinuous:
      return UrdfCoordinateKind::kAngular;
    case UrdfJointType::kPrismatic:
      return UrdfCoordinateKind::kLinear;
    case UrdfJointType::kPlanar:
      return UrdfCoordinateKind::kPlanar;
    case UrdfJointType::kFloating:
      return UrdfCoordinateKind::kFloating;
    case UrdfJointType::kFixed:
      return UrdfCoordinateKind::kNone;
  }
  fail("URDF joint has an unsupported coordinate kind");
}

[[nodiscard]] bool has_axis_semantics(UrdfJointType type) noexcept {
  return type == UrdfJointType::kRevolute ||
         type == UrdfJointType::kContinuous ||
         type == UrdfJointType::kPrismatic ||
         type == UrdfJointType::kPlanar;
}

[[nodiscard]] bool is_scalar_coordinate(
    UrdfCoordinateKind kind) noexcept {
  return kind == UrdfCoordinateKind::kAngular ||
         kind == UrdfCoordinateKind::kLinear;
}

struct ParseAccumulator final {
  UrdfDocumentIr urdf;
  std::optional<SrdfDocumentIr> srdf;
  std::string root_link_name;
  std::vector<std::string> group_expansion_postorder;
  std::unordered_map<std::string, SourceArtifactReferenceKind>
      referenced_artifacts;
  std::unordered_set<std::string> top_level_materials;
  std::unordered_set<std::string> referenced_materials;
};

void add_artifact_reference(ParseAccumulator& accumulator,
                            const std::string& logical_uri,
                            SourceArtifactReferenceKind kind) {
  const auto [found, inserted] =
      accumulator.referenced_artifacts.emplace(logical_uri, kind);
  if (!inserted && found->second != kind) {
    fail("one model source artifact is referenced as both mesh and texture");
  }
}

[[nodiscard]] UrdfOriginIr default_origin() {
  return UrdfOriginIr{};
}

[[nodiscard]] UrdfOriginIr parse_origin(const XmlNode& node) {
  require_no_text(node);
  require_allowed_attributes(node, {"xyz", "rpy"});
  if (!node.children.empty()) {
    fail("<origin> must be empty");
  }
  UrdfOriginIr result;
  result.element_authored = true;
  result.authored_xyz_m = parse_optional_vector3(node, "xyz");
  result.authored_rpy_rad = parse_optional_vector3(node, "rpy");
  if (result.authored_xyz_m.has_value()) {
    result.xyz_m = *result.authored_xyz_m;
  }
  if (result.authored_rpy_rad.has_value()) {
    result.rpy_rad = *result.authored_rpy_rad;
  }
  return result;
}

[[nodiscard]] UrdfGeometryIr parse_geometry(
    const XmlNode& node, ParseAccumulator& accumulator) {
  require_no_text(node);
  require_allowed_attributes(node, {});
  if (node.children.size() != 1U) {
    fail("<geometry> must contain exactly one primitive");
  }
  const auto& primitive = node.children.front();
  require_no_text(primitive);
  if (!primitive.children.empty()) {
    fail("URDF geometry primitives must be empty");
  }
  if (primitive.name == "box") {
    require_allowed_attributes(primitive, {"size"}, {"size"});
    const auto size = numbers_as_array<3U>(primitive, "size");
    if (std::any_of(size.begin(), size.end(),
                    [](double value) { return value <= 0.0; })) {
      fail("URDF box dimensions must be positive");
    }
    return UrdfBoxGeometryIr{size};
  }
  if (primitive.name == "cylinder") {
    require_allowed_attributes(primitive, {"radius", "length"},
                               {"radius", "length"});
    const auto radius = parse_required_scalar(primitive, "radius");
    const auto length = parse_required_scalar(primitive, "length");
    if (radius <= 0.0 || length <= 0.0) {
      fail("URDF cylinder dimensions must be positive");
    }
    return UrdfCylinderGeometryIr{radius, length};
  }
  if (primitive.name == "sphere") {
    require_allowed_attributes(primitive, {"radius"}, {"radius"});
    const auto radius = parse_required_scalar(primitive, "radius");
    if (radius <= 0.0) {
      fail("URDF sphere radius must be positive");
    }
    return UrdfSphereGeometryIr{radius};
  }
  if (primitive.name == "mesh") {
    require_allowed_attributes(primitive, {"filename", "scale"},
                               {"filename"});
    const auto& filename =
        require_logical_uri_attribute(primitive, "filename");
    add_artifact_reference(
        accumulator, filename,
        SourceArtifactReferenceKind::kMesh);
    UrdfMeshGeometryIr mesh;
    mesh.logical_uri = filename;
    mesh.authored_scale = parse_optional_vector3(primitive, "scale");
    if (mesh.authored_scale.has_value()) {
      mesh.scale = *mesh.authored_scale;
    }
    if (std::any_of(mesh.scale.begin(), mesh.scale.end(),
                    [](double value) { return value <= 0.0; })) {
      fail("URDF mesh scale values must be positive");
    }
    return mesh;
  }
  fail("unsupported URDF geometry primitive <" + primitive.name + ">");
}

[[nodiscard]] UrdfMaterialIr parse_material(
    const XmlNode& node, ParseAccumulator& accumulator, bool top_level) {
  require_no_text(node);
  require_allowed_attributes(node, {"name"}, {"name"});
  const auto* name = find_attribute(node, "name");
  validate_source_symbol(*name, "material name");
  UrdfMaterialIr material;
  material.name = *name;
  for (const auto& child : node.children) {
    require_no_text(child);
    if (!child.children.empty()) {
      fail("URDF material children must be empty");
    }
    if (child.name == "color") {
      if (material.color.has_value()) {
        fail("URDF material repeats <color>");
      }
      require_allowed_attributes(child, {"rgba"}, {"rgba"});
      const auto rgba = numbers_as_array<4U>(child, "rgba");
      if (std::any_of(rgba.begin(), rgba.end(), [](double value) {
            return value < 0.0 || value > 1.0;
          })) {
        fail("URDF material color values must be within [0, 1]");
      }
      material.color = UrdfMaterialColorIr{rgba};
    } else if (child.name == "texture") {
      if (material.texture.has_value()) {
        fail("URDF material repeats <texture>");
      }
      require_allowed_attributes(child, {"filename"}, {"filename"});
      const auto& filename =
          require_logical_uri_attribute(child, "filename");
      add_artifact_reference(
          accumulator, filename,
          SourceArtifactReferenceKind::kTexture);
      material.texture = UrdfMaterialTextureIr{filename};
    } else {
      fail("unsupported child <" + child.name + "> in <material>");
    }
  }
  if (top_level) {
    if (!material.color.has_value() && !material.texture.has_value()) {
      fail("top-level URDF material must define a color or texture");
    }
    if (!accumulator.top_level_materials.emplace(*name).second) {
      fail("URDF contains duplicate top-level material name '" + *name +
           "'");
    }
  } else if (!material.color.has_value() && !material.texture.has_value()) {
    accumulator.referenced_materials.emplace(*name);
  }
  return material;
}

[[nodiscard]] UrdfInertialIr parse_inertial(const XmlNode& node) {
  require_no_text(node);
  require_allowed_attributes(node, {});
  UrdfInertialIr inertial{default_origin(), 0.0,
                          UrdfInertiaIr{0.0, 0.0, 0.0,
                                           0.0, 0.0, 0.0}};
  bool saw_origin = false;
  bool saw_mass = false;
  bool saw_inertia = false;
  for (const auto& child : node.children) {
    if (child.name == "origin") {
      if (saw_origin) {
        fail("<inertial> repeats <origin>");
      }
      saw_origin = true;
      inertial.origin = parse_origin(child);
    } else if (child.name == "mass") {
      if (saw_mass) {
        fail("<inertial> repeats <mass>");
      }
      saw_mass = true;
      require_no_text(child);
      require_allowed_attributes(child, {"value"}, {"value"});
      inertial.mass_kg = parse_required_scalar(child, "value");
      if (inertial.mass_kg < 0.0) {
        fail("URDF inertial mass must not be negative");
      }
      if (!child.children.empty()) {
        fail("<mass> must be empty");
      }
    } else if (child.name == "inertia") {
      if (saw_inertia) {
        fail("<inertial> repeats <inertia>");
      }
      saw_inertia = true;
      require_no_text(child);
      require_allowed_attributes(
          child, {"ixx", "ixy", "ixz", "iyy", "iyz", "izz"},
          {"ixx", "ixy", "ixz", "iyy", "iyz", "izz"});
      inertial.inertia = UrdfInertiaIr{
          parse_required_scalar(child, "ixx"),
          parse_required_scalar(child, "ixy"),
          parse_required_scalar(child, "ixz"),
          parse_required_scalar(child, "iyy"),
          parse_required_scalar(child, "iyz"),
          parse_required_scalar(child, "izz")};
      if (inertial.inertia.ixx < 0.0 || inertial.inertia.iyy < 0.0 ||
          inertial.inertia.izz < 0.0) {
        fail("URDF principal inertia values must not be negative");
      }
      if (!child.children.empty()) {
        fail("<inertia> must be empty");
      }
    } else {
      fail("unsupported child <" + child.name + "> in <inertial>");
    }
  }
  if (!saw_mass || !saw_inertia) {
    fail("<inertial> requires exactly one <mass> and <inertia>");
  }
  return inertial;
}

[[nodiscard]] UrdfVisualIr parse_visual(
    const XmlNode& node, ParseAccumulator& accumulator) {
  require_no_text(node);
  require_allowed_attributes(node, {"name"});
  UrdfVisualIr visual;
  if (const auto* name = find_attribute(node, "name"); name != nullptr) {
    validate_source_symbol(*name, "geometry role name");
    visual.name = *name;
  }
  bool saw_origin = false;
  bool saw_geometry = false;
  bool saw_material = false;
  for (const auto& child : node.children) {
    if (child.name == "origin") {
      if (saw_origin) {
        fail("<" + node.name + "> repeats <origin>");
      }
      saw_origin = true;
      visual.origin = parse_origin(child);
    } else if (child.name == "geometry") {
      if (saw_geometry) {
        fail("<" + node.name + "> repeats <geometry>");
      }
      saw_geometry = true;
      visual.geometry = parse_geometry(child, accumulator);
    } else if (child.name == "material") {
      if (saw_material) {
        fail("<visual> repeats <material>");
      }
      saw_material = true;
      visual.material = parse_material(child, accumulator, false);
    } else {
      fail("unsupported child <" + child.name + "> in <" + node.name + ">");
    }
  }
  if (!saw_geometry) {
    fail("<" + node.name + "> requires exactly one <geometry>");
  }
  return visual;
}

[[nodiscard]] UrdfCollisionIr parse_collision(
    const XmlNode& node, ParseAccumulator& accumulator) {
  require_no_text(node);
  require_allowed_attributes(node, {"name"});
  UrdfCollisionIr collision;
  if (const auto* name = find_attribute(node, "name"); name != nullptr) {
    validate_source_symbol(*name, "geometry role name");
    collision.name = *name;
  }
  bool saw_origin = false;
  bool saw_geometry = false;
  for (const auto& child : node.children) {
    if (child.name == "origin") {
      if (saw_origin) {
        fail("<collision> repeats <origin>");
      }
      saw_origin = true;
      collision.origin = parse_origin(child);
    } else if (child.name == "geometry") {
      if (saw_geometry) {
        fail("<collision> repeats <geometry>");
      }
      saw_geometry = true;
      collision.geometry = parse_geometry(child, accumulator);
    } else if (child.name == "material") {
      fail("URDF collision material is forbidden");
    } else {
      fail("unsupported child <" + child.name + "> in <collision>");
    }
  }
  if (!saw_geometry) {
    fail("<collision> requires exactly one <geometry>");
  }
  return collision;
}

[[nodiscard]] UrdfLinkIr parse_link(const XmlNode& node,
                                    ParseAccumulator& accumulator) {
  require_no_text(node);
  require_allowed_attributes(node, {"name"}, {"name"});
  UrdfLinkIr link;
  link.name = require_symbol_attribute(node, "name");
  bool saw_inertial = false;
  for (const auto& child : node.children) {
    if (child.name == "inertial") {
      if (saw_inertial) {
        fail("<link> repeats <inertial>");
      }
      saw_inertial = true;
      link.inertial = parse_inertial(child);
    } else if (child.name == "visual") {
      link.visuals.push_back(parse_visual(child, accumulator));
    } else if (child.name == "collision") {
      link.collisions.push_back(parse_collision(child, accumulator));
    } else {
      fail("unsupported child <" + child.name + "> in <link>");
    }
  }
  return link;
}

void resolve_material_references(ParseAccumulator& accumulator) {
  std::unordered_map<std::string, const UrdfMaterialIr*> definitions;
  definitions.reserve(accumulator.urdf.materials.size());
  for (const auto& material : accumulator.urdf.materials) {
    definitions.emplace(*material.name, &material);
  }
  for (auto& link : accumulator.urdf.links) {
    for (auto& visual : link.visuals) {
      if (!visual.material.has_value() ||
          visual.material->color.has_value() ||
          visual.material->texture.has_value()) {
        continue;
      }
      if (!visual.material->name.has_value()) {
        fail("empty inline URDF material must reference a named material");
      }
      const auto found = definitions.find(*visual.material->name);
      if (found == definitions.end()) {
        fail("URDF visual references unknown top-level material '" +
             *visual.material->name + "'");
      }
      visual.material->resolved_reference =
          UrdfResolvedMaterialReferenceIr{
              *visual.material->name, found->second->color,
              found->second->texture};
    }
  }
}

template <typename Range, typename Projection>
void sort_by(Range& range, Projection projection) {
  std::sort(range.begin(), range.end(),
            [&projection](const auto& left, const auto& right) {
              return projection(left) < projection(right);
            });
}

void emit_visual(std::string& output, const UrdfVisualIr& visual);
void emit_collision(std::string& output,
                    const UrdfCollisionIr& collision);

[[nodiscard]] std::optional<ModelSourceVector3> authored_mesh_scale(
    const UrdfGeometryIr& geometry) {
  const auto* mesh = std::get_if<UrdfMeshGeometryIr>(&geometry);
  return mesh == nullptr ? std::nullopt : mesh->authored_scale;
}

void canonicalize_urdf_document(UrdfDocumentIr& urdf) {
  sort_by(urdf.materials,
          [](const UrdfMaterialIr& material) -> const std::string& {
            return *material.name;
          });
  for (auto& link : urdf.links) {
    using OccurrenceKey =
        std::tuple<std::string, bool, std::optional<ModelSourceVector3>,
                   std::optional<ModelSourceVector3>,
                   std::optional<ModelSourceVector3>>;
    std::vector<std::pair<OccurrenceKey, UrdfVisualIr>> visual_keys;
    visual_keys.reserve(link.visuals.size());
    for (auto& visual : link.visuals) {
      std::string key;
      emit_visual(key, visual);
      visual_keys.emplace_back(
          OccurrenceKey{std::move(key), visual.origin.element_authored,
                        visual.origin.authored_xyz_m,
                        visual.origin.authored_rpy_rad,
                        authored_mesh_scale(visual.geometry)},
          std::move(visual));
    }
    std::sort(visual_keys.begin(), visual_keys.end(),
              [](const auto& left, const auto& right) {
                return left.first < right.first;
              });
    link.visuals.clear();
    link.visuals.reserve(visual_keys.size());
    for (auto& [key, visual] : visual_keys) {
      (void)key;
      link.visuals.push_back(std::move(visual));
    }

    std::vector<std::pair<OccurrenceKey, UrdfCollisionIr>>
        collision_keys;
    collision_keys.reserve(link.collisions.size());
    for (auto& collision : link.collisions) {
      std::string key;
      emit_collision(key, collision);
      collision_keys.emplace_back(
          OccurrenceKey{std::move(key),
                        collision.origin.element_authored,
                        collision.origin.authored_xyz_m,
                        collision.origin.authored_rpy_rad,
                        authored_mesh_scale(collision.geometry)},
          std::move(collision));
    }
    std::sort(collision_keys.begin(), collision_keys.end(),
              [](const auto& left, const auto& right) {
                return left.first < right.first;
              });
    link.collisions.clear();
    link.collisions.reserve(collision_keys.size());
    for (auto& [key, collision] : collision_keys) {
      (void)key;
      link.collisions.push_back(std::move(collision));
    }
  }
  sort_by(urdf.links,
          [](const UrdfLinkIr& link) -> const std::string& {
            return link.name;
          });
  sort_by(urdf.joints,
          [](const UrdfJointIr& joint) -> const std::string& {
            return joint.name;
          });
}

void require_empty_scalar_element(
    const XmlNode& node,
    std::initializer_list<std::string_view> allowed) {
  require_no_text(node);
  require_allowed_attributes(node, allowed);
  if (!node.children.empty()) {
    fail("<" + node.name + "> must be empty");
  }
}

[[nodiscard]] UrdfJointIr parse_joint(const XmlNode& node) {
  require_no_text(node);
  require_allowed_attributes(node, {"name", "type"}, {"name", "type"});
  const auto type = parse_joint_type(*find_attribute(node, "type"));
  UrdfJointIr joint;
  joint.name = require_symbol_attribute(node, "name");
  joint.type = type;
  joint.coordinate_kind = coordinate_kind(type);
  if (has_axis_semantics(type)) {
    joint.axis = UrdfAxisIr{};
  }
  bool saw_origin = false;
  bool saw_parent = false;
  bool saw_child = false;
  bool saw_axis = false;
  bool saw_calibration = false;
  bool saw_dynamics = false;
  bool saw_limit = false;
  bool saw_mimic = false;
  bool saw_safety = false;
  for (const auto& child : node.children) {
    if (child.name == "origin") {
      if (saw_origin) {
        fail("<joint> repeats <origin>");
      }
      saw_origin = true;
      joint.origin = parse_origin(child);
    } else if (child.name == "parent" || child.name == "child") {
      auto& seen = child.name == "parent" ? saw_parent : saw_child;
      if (seen) {
        fail("<joint> repeats <" + child.name + ">");
      }
      seen = true;
      require_no_text(child);
      require_allowed_attributes(child, {"link"}, {"link"});
      const auto& link = require_symbol_attribute(child, "link");
      if (!child.children.empty()) {
        fail("<" + child.name + "> must be empty");
      }
      if (child.name == "parent") {
        joint.parent_link_name = link;
      } else {
        joint.child_link_name = link;
      }
    } else if (child.name == "axis") {
      if (saw_axis) {
        fail("<joint> repeats <axis>");
      }
      if (!has_axis_semantics(type)) {
        fail("<axis> is only valid for revolute, continuous, prismatic, or "
             "planar joints");
      }
      saw_axis = true;
      require_no_text(child);
      require_allowed_attributes(child, {"xyz"}, {"xyz"});
      if (!child.children.empty()) {
        fail("<axis> must be empty");
      }
      const auto axis = numbers_as_array<3U>(child, "xyz");
      if (std::all_of(axis.begin(), axis.end(),
                      [](double value) { return value == 0.0; })) {
        fail("URDF movable-joint axis must not be zero");
      }
      joint.axis =
          UrdfAxisIr{true, std::optional<ModelSourceVector3>(axis), axis};
    } else if (child.name == "calibration") {
      if (saw_calibration) {
        fail("<joint> repeats <calibration>");
      }
      saw_calibration = true;
      require_empty_scalar_element(
          child, {"rising", "falling", "reference_position"});
      UrdfCalibrationIr calibration;
      calibration.rising = parse_optional_scalar(child, "rising");
      calibration.falling = parse_optional_scalar(child, "falling");
      calibration.authored_reference_position =
          parse_optional_scalar(child, "reference_position");
      if (calibration.authored_reference_position.has_value()) {
        calibration.reference_position =
            *calibration.authored_reference_position;
      }
      joint.calibration = std::move(calibration);
    } else if (child.name == "dynamics") {
      if (saw_dynamics) {
        fail("<joint> repeats <dynamics>");
      }
      saw_dynamics = true;
      require_empty_scalar_element(child, {"damping", "friction"});
      UrdfDynamicsIr dynamics;
      dynamics.authored_damping =
          parse_optional_scalar(child, "damping");
      dynamics.authored_friction =
          parse_optional_scalar(child, "friction");
      if (!dynamics.authored_damping.has_value() &&
          !dynamics.authored_friction.has_value()) {
        fail("<dynamics> requires damping or friction");
      }
      if (dynamics.authored_damping.has_value()) {
        dynamics.damping = *dynamics.authored_damping;
      }
      if (dynamics.authored_friction.has_value()) {
        dynamics.friction = *dynamics.authored_friction;
      }
      if (dynamics.damping < 0.0 || dynamics.friction < 0.0) {
        fail("URDF joint dynamics values must not be negative");
      }
      joint.dynamics = std::move(dynamics);
    } else if (child.name == "limit") {
      if (saw_limit) {
        fail("<joint> repeats <limit>");
      }
      saw_limit = true;
      require_empty_scalar_element(
          child, {"lower", "upper", "effort", "velocity"});
      UrdfLimitIr limit;
      limit.authored_lower = parse_optional_scalar(child, "lower");
      limit.authored_upper = parse_optional_scalar(child, "upper");
      const auto effort = parse_optional_scalar(child, "effort");
      const auto velocity = parse_optional_scalar(child, "velocity");
      if (!effort.has_value() || !velocity.has_value()) {
        fail("<limit> requires effort and velocity");
      }
      if (limit.authored_lower.has_value()) {
        limit.lower = *limit.authored_lower;
      }
      if (limit.authored_upper.has_value()) {
        limit.upper = *limit.authored_upper;
      }
      limit.effort = *effort;
      limit.velocity = *velocity;
      if (limit.lower > limit.upper) {
        fail("URDF joint lower limit exceeds its upper limit");
      }
      if (limit.effort < 0.0 || limit.velocity < 0.0) {
        fail("URDF joint effort and velocity limits must not be negative");
      }
      joint.limit = std::move(limit);
    } else if (child.name == "mimic") {
      if (saw_mimic) {
        fail("<joint> repeats <mimic>");
      }
      saw_mimic = true;
      require_no_text(child);
      require_allowed_attributes(child, {"joint", "multiplier", "offset"},
                                 {"joint"});
      if (!child.children.empty()) {
        fail("<mimic> must be empty");
      }
      UrdfMimicIr mimic;
      mimic.joint_name = require_symbol_attribute(child, "joint");
      mimic.authored_multiplier =
          parse_optional_scalar(child, "multiplier");
      mimic.authored_offset = parse_optional_scalar(child, "offset");
      if (mimic.authored_multiplier.has_value()) {
        mimic.multiplier = *mimic.authored_multiplier;
      }
      if (mimic.authored_offset.has_value()) {
        mimic.offset = *mimic.authored_offset;
      }
      joint.mimic = std::move(mimic);
    } else if (child.name == "safety_controller") {
      if (saw_safety) {
        fail("<joint> repeats <safety_controller>");
      }
      saw_safety = true;
      require_empty_scalar_element(
          child,
          {"soft_lower_limit", "soft_upper_limit", "k_position",
           "k_velocity"});
      UrdfSafetyControllerIr safety;
      safety.authored_soft_lower_limit =
          parse_optional_scalar(child, "soft_lower_limit");
      safety.authored_soft_upper_limit =
          parse_optional_scalar(child, "soft_upper_limit");
      safety.authored_k_position =
          parse_optional_scalar(child, "k_position");
      const auto k_velocity =
          parse_optional_scalar(child, "k_velocity");
      if (!k_velocity.has_value()) {
        fail("<safety_controller> requires k_velocity");
      }
      if (safety.authored_soft_lower_limit.has_value()) {
        safety.soft_lower_limit =
            *safety.authored_soft_lower_limit;
      }
      if (safety.authored_soft_upper_limit.has_value()) {
        safety.soft_upper_limit =
            *safety.authored_soft_upper_limit;
      }
      if (safety.authored_k_position.has_value()) {
        safety.k_position = *safety.authored_k_position;
      }
      safety.k_velocity = *k_velocity;
      if (safety.soft_lower_limit > safety.soft_upper_limit) {
        fail("URDF safety-controller lower limit exceeds its upper limit");
      }
      if (safety.k_position < 0.0 || safety.k_velocity < 0.0) {
        fail("URDF safety-controller gains must not be negative");
      }
      joint.safety_controller = std::move(safety);
    } else {
      fail("unsupported child <" + child.name + "> in <joint>");
    }
  }
  if (!saw_parent || !saw_child) {
    fail("<joint> requires exactly one <parent> and <child>");
  }
  if ((type == UrdfJointType::kRevolute ||
       type == UrdfJointType::kPrismatic) &&
      !joint.limit.has_value()) {
    fail("URDF revolute and prismatic joints require <limit>");
  }
  return joint;
}

void validate_urdf_semantics(ParseAccumulator& accumulator) {
  auto& ir = accumulator.urdf;
  std::unordered_set<std::string> links;
  for (const auto& link : ir.links) {
    if (!links.emplace(link.name).second) {
      fail("URDF contains duplicate link name '" + link.name + "'");
    }
    std::unordered_set<std::string> visual_names;
    for (const auto& visual : link.visuals) {
      if (visual.name.has_value() &&
          !visual_names.emplace(*visual.name).second) {
        fail("URDF link '" + link.name +
             "' contains a duplicate visual name");
      }
    }
    std::unordered_set<std::string> collision_names;
    for (const auto& collision : link.collisions) {
      if (collision.name.has_value() &&
          !collision_names.emplace(*collision.name).second) {
        fail("URDF link '" + link.name +
             "' contains a duplicate collision name");
      }
    }
  }
  if (links.empty()) {
    fail("URDF must declare at least one link");
  }

  std::unordered_set<std::string> joints;
  std::unordered_map<std::string, std::string> parent_by_child;
  std::unordered_map<std::string, std::vector<std::string>> children_by_parent;
  for (const auto& joint : ir.joints) {
    if (!joints.emplace(joint.name).second) {
      fail("URDF contains duplicate joint name '" + joint.name + "'");
    }
    if (!links.contains(joint.parent_link_name) ||
        !links.contains(joint.child_link_name)) {
      fail("URDF joint '" + joint.name + "' references an unknown link");
    }
    if (joint.parent_link_name == joint.child_link_name) {
      fail("URDF joint '" + joint.name + "' is self-referential");
    }
    if (!parent_by_child
             .emplace(joint.child_link_name, joint.parent_link_name)
             .second) {
      fail("URDF link '" + joint.child_link_name +
           "' has more than one parent joint");
    }
    children_by_parent[joint.parent_link_name].push_back(
        joint.child_link_name);
  }
  for (const auto& joint : ir.joints) {
    if (joint.mimic.has_value()) {
      if (!joints.contains(joint.mimic->joint_name)) {
        fail("URDF joint '" + joint.name +
             "' mimics an unknown joint");
      }
      if (joint.mimic->joint_name == joint.name) {
        fail("URDF joint '" + joint.name + "' mimics itself");
      }
    }
  }

  std::vector<std::string> roots;
  for (const auto& link : links) {
    if (!parent_by_child.contains(link)) {
      roots.push_back(link);
    }
  }
  if (roots.size() != 1U) {
    fail("URDF link graph must have exactly one root");
  }
  accumulator.root_link_name = roots.front();
  std::unordered_set<std::string> visited;
  std::vector<std::string> pending{accumulator.root_link_name};
  while (!pending.empty()) {
    auto current = std::move(pending.back());
    pending.pop_back();
    if (!visited.emplace(current).second) {
      fail("URDF link graph contains a cycle");
    }
    const auto children = children_by_parent.find(current);
    if (children != children_by_parent.end()) {
      pending.insert(pending.end(), children->second.begin(),
                     children->second.end());
    }
  }
  if (visited.size() != links.size()) {
    fail("URDF link graph is disconnected or cyclic");
  }

  std::unordered_map<std::string, const UrdfJointIr*> joint_by_name;
  joint_by_name.reserve(ir.joints.size());
  for (const auto& joint : ir.joints) {
    joint_by_name.emplace(joint.name, &joint);
  }
  enum class Visit : std::uint8_t { kUnvisited = 0, kVisiting, kVisited };
  std::unordered_map<std::string, Visit> mimic_visit;
  mimic_visit.reserve(ir.joints.size());
  for (const auto& joint : ir.joints) {
    if (mimic_visit[joint.name] == Visit::kVisited) {
      continue;
    }
    std::vector<std::string> path;
    const UrdfJointIr* current = &joint;
    while (true) {
      auto& state = mimic_visit[current->name];
      if (state == Visit::kVisiting) {
        fail("URDF mimic relation contains a cycle");
      }
      if (state == Visit::kVisited) {
        break;
      }
      state = Visit::kVisiting;
      path.push_back(current->name);
      if (!current->mimic.has_value()) {
        break;
      }
      const auto* target = joint_by_name.at(current->mimic->joint_name);
      if ((current->coordinate_kind != UrdfCoordinateKind::kAngular &&
           current->coordinate_kind != UrdfCoordinateKind::kLinear) ||
          target->coordinate_kind != current->coordinate_kind) {
        fail("URDF mimic relation crosses incompatible coordinate kinds");
      }
      current = target;
    }
    for (const auto& visited : path) {
      mimic_visit[visited] = Visit::kVisited;
    }
  }
}

void parse_urdf(const XmlNode& root, ParseAccumulator& accumulator) {
  require_no_text(root);
  if (root.name != "robot") {
    fail("URDF root element must be <robot>");
  }
  require_allowed_attributes(root, {"name"}, {"name"});
  accumulator.urdf.robot_name = require_symbol_attribute(root, "name");
  for (const auto& child : root.children) {
    if (child.name == "link") {
      accumulator.urdf.links.push_back(parse_link(child, accumulator));
    } else if (child.name == "joint") {
      accumulator.urdf.joints.push_back(parse_joint(child));
    } else if (child.name == "material") {
      accumulator.urdf.materials.push_back(
          parse_material(child, accumulator, true));
    } else {
      fail("unsupported top-level URDF element <" + child.name + ">");
    }
  }
  for (const auto& material : accumulator.referenced_materials) {
    if (!accumulator.top_level_materials.contains(material)) {
      fail("URDF visual references unknown top-level material '" + material +
           "'");
    }
  }
  resolve_material_references(accumulator);
  validate_urdf_semantics(accumulator);
  canonicalize_urdf_document(accumulator.urdf);
}

struct LinkTraversalInterval final {
  std::size_t entered;
  std::size_t exited;
};

[[nodiscard]] std::unordered_map<std::string, LinkTraversalInterval>
build_link_traversal_intervals(const UrdfDocumentIr& urdf,
                               std::string_view root_link_name) {
  std::unordered_map<std::string, std::vector<std::string>>
      children_by_parent;
  children_by_parent.reserve(urdf.joints.size());
  for (const auto& joint : urdf.joints) {
    children_by_parent[joint.parent_link_name].push_back(
        joint.child_link_name);
  }

  struct Frame final {
    std::string_view link_name;
    std::size_t next_child = 0U;
  };
  std::unordered_map<std::string, LinkTraversalInterval> intervals;
  intervals.reserve(urdf.links.size());
  std::vector<Frame> stack;
  stack.reserve(urdf.links.size());
  std::size_t timestamp = 0U;
  intervals.emplace(std::string(root_link_name),
                    LinkTraversalInterval{timestamp++, 0U});
  stack.push_back(Frame{root_link_name, 0U});
  while (!stack.empty()) {
    auto& frame = stack.back();
    const auto children = children_by_parent.find(
        std::string(frame.link_name));
    if (children != children_by_parent.end() &&
        frame.next_child < children->second.size()) {
      const auto& child = children->second[frame.next_child++];
      intervals.emplace(child, LinkTraversalInterval{timestamp++, 0U});
      stack.push_back(Frame{child, 0U});
      continue;
    }
    intervals.at(std::string(frame.link_name)).exited = timestamp++;
    stack.pop_back();
  }
  return intervals;
}

[[nodiscard]] bool link_is_ancestor(
    std::string_view base, std::string_view tip,
    const std::unordered_map<std::string, LinkTraversalInterval>&
        intervals) {
  const auto base_interval = intervals.find(std::string(base));
  const auto tip_interval = intervals.find(std::string(tip));
  if (base_interval == intervals.end() || tip_interval == intervals.end()) {
    return false;
  }
  return base_interval->second.entered <= tip_interval->second.entered &&
         tip_interval->second.exited <= base_interval->second.exited;
}

void canonicalize_srdf_document(SrdfDocumentIr& document) {
  // Tagged group-member order is deliberately not normalized: it defines
  // deterministic first-occurrence order in the expanded resource view.
  sort_by(document.groups,
          [](const SrdfGroupIr& group) -> const std::string& {
            return group.name;
          });
  for (auto& state : document.group_states) {
    sort_by(state.joints,
            [](const SrdfGroupStateJointIr& joint)
                -> const std::string& { return joint.joint_name; });
  }
  sort_by(document.group_states, [](const SrdfGroupStateIr& state) {
    return std::pair(state.group_name, state.name);
  });
  sort_by(document.end_effectors,
          [](const SrdfEndEffectorIr& end_effector)
              -> const std::string& { return end_effector.name; });
  std::sort(document.passive_joint_names.begin(),
            document.passive_joint_names.end());
  for (auto& disabled : document.disabled_collisions) {
    if (disabled.second_link_name < disabled.first_link_name) {
      std::swap(disabled.first_link_name, disabled.second_link_name);
    }
  }
  sort_by(document.disabled_collisions,
          [](const SrdfDisabledCollisionIr& disabled) {
            return std::tuple(disabled.first_link_name,
                              disabled.second_link_name,
                              disabled.reason);
          });
}

void parse_srdf(const XmlNode& root, ParseAccumulator& accumulator) {
  require_no_text(root);
  if (root.name != "robot") {
    fail("SRDF root element must be <robot>");
  }
  require_allowed_attributes(root, {"name"}, {"name"});
  const auto& robot_name = require_symbol_attribute(root, "name");
  if (robot_name != accumulator.urdf.robot_name) {
    fail("SRDF robot name does not match the URDF robot name");
  }
  SrdfDocumentIr document;
  document.robot_name = robot_name;

  std::unordered_set<std::string> links;
  links.reserve(accumulator.urdf.links.size());
  for (const auto& link : accumulator.urdf.links) {
    links.emplace(link.name);
  }
  std::unordered_set<std::string> joints;
  for (const auto& joint : accumulator.urdf.joints) {
    joints.emplace(joint.name);
  }
  const auto link_intervals =
      build_link_traversal_intervals(accumulator.urdf,
                                     accumulator.root_link_name);

  for (const auto& node : root.children) {
    require_no_text(node);
    if (node.name == "virtual_joint") {
      fail("SRDF <virtual_joint> is forbidden: cell placement owns roots");
    }
    if (node.name == "group") {
      require_allowed_attributes(node, {"name"}, {"name"});
      SrdfGroupIr group;
      group.name = require_symbol_attribute(node, "name");
      for (const auto& child : node.children) {
        require_no_text(child);
        if (!child.children.empty()) {
          fail("SRDF group members must be empty");
        }
        if (child.name == "link") {
          require_allowed_attributes(child, {"name"}, {"name"});
          group.members.emplace_back(SrdfLinkGroupMemberIr{
              require_symbol_attribute(child, "name")});
        } else if (child.name == "joint") {
          require_allowed_attributes(child, {"name"}, {"name"});
          group.members.emplace_back(SrdfJointGroupMemberIr{
              require_symbol_attribute(child, "name")});
        } else if (child.name == "chain") {
          require_allowed_attributes(child, {"base_link", "tip_link"},
                                     {"base_link", "tip_link"});
          group.members.emplace_back(SrdfChainGroupMemberIr{
              require_symbol_attribute(child, "base_link"),
              require_symbol_attribute(child, "tip_link")});
        } else if (child.name == "group") {
          require_allowed_attributes(child, {"name"}, {"name"});
          group.members.emplace_back(SrdfSubgroupMemberIr{
              require_symbol_attribute(child, "name")});
        } else {
          fail("unsupported SRDF group member <" + child.name + ">");
        }
      }
      document.groups.push_back(std::move(group));
    } else if (node.name == "group_state") {
      require_allowed_attributes(node, {"name", "group"},
                                 {"name", "group"});
      SrdfGroupStateIr state{
          require_symbol_attribute(node, "name"),
          require_symbol_attribute(node, "group"), {}};
      for (const auto& child : node.children) {
        require_no_text(child);
        if (child.name != "joint" || !child.children.empty()) {
          fail("<group_state> supports only empty <joint> children");
        }
        require_allowed_attributes(child, {"name", "value"},
                                   {"name", "value"});
        const auto values =
            parse_numbers(*find_attribute(child, "value"), 1U, 1U, "value");
        state.joints.push_back(SrdfGroupStateJointIr{
            require_symbol_attribute(child, "name"), values.front()});
      }
      document.group_states.push_back(std::move(state));
    } else if (node.name == "end_effector") {
      require_allowed_attributes(
          node, {"name", "parent_link", "group", "parent_group"},
          {"name", "parent_link", "group"});
      if (!node.children.empty()) {
        fail("<end_effector> must be empty");
      }
      SrdfEndEffectorIr end_effector{
          require_symbol_attribute(node, "name"),
          require_symbol_attribute(node, "parent_link"),
          require_symbol_attribute(node, "group"), std::nullopt};
      if (const auto* parent_group = find_attribute(node, "parent_group");
          parent_group != nullptr) {
        validate_source_symbol(*parent_group, "parent_group");
        end_effector.parent_group_name = *parent_group;
      }
      document.end_effectors.push_back(std::move(end_effector));
    } else if (node.name == "passive_joint") {
      require_allowed_attributes(node, {"name"}, {"name"});
      if (!node.children.empty()) {
        fail("<passive_joint> must be empty");
      }
      document.passive_joint_names.push_back(
          require_symbol_attribute(node, "name"));
    } else if (node.name == "disable_collisions") {
      require_allowed_attributes(node, {"link1", "link2", "reason"},
                                 {"link1", "link2"});
      if (!node.children.empty()) {
        fail("<disable_collisions> must be empty");
      }
      SrdfDisabledCollisionIr disabled{
          require_symbol_attribute(node, "link1"),
          require_symbol_attribute(node, "link2"), std::nullopt};
      if (const auto* reason = find_attribute(node, "reason");
          reason != nullptr) {
        if (reason->size() > core::kMaxModelAssetLogicalUriBytes) {
          fail("SRDF disabled-collision reason exceeds the text bound");
        }
        if (!is_utf8_nfc(*reason)) {
          fail("SRDF disabled-collision reason must be valid NFC UTF-8");
        }
        disabled.reason = *reason;
      }
      document.disabled_collisions.push_back(std::move(disabled));
    } else {
      fail("unsupported top-level SRDF element <" + node.name + ">");
    }
  }

  canonicalize_srdf_document(document);

  std::unordered_set<std::string> groups;
  for (const auto& group : document.groups) {
    if (!groups.emplace(group.name).second) {
      fail("SRDF contains duplicate group name '" + group.name + "'");
    }
  }
  for (const auto& group : document.groups) {
    std::unordered_set<std::string> direct_links;
    std::unordered_set<std::string> direct_joints;
    std::set<std::pair<std::string, std::string>> direct_chains;
    std::unordered_set<std::string> direct_subgroups;
    for (const auto& member : group.members) {
      if (const auto* link =
              std::get_if<SrdfLinkGroupMemberIr>(&member);
          link != nullptr) {
        if (!links.contains(link->link_name) ||
            !direct_links.emplace(link->link_name).second) {
          fail("SRDF group '" + group.name +
               "' references an unknown or duplicate direct link");
        }
      } else if (const auto* joint =
                     std::get_if<SrdfJointGroupMemberIr>(&member);
                 joint != nullptr) {
        if (!joints.contains(joint->joint_name) ||
            !direct_joints.emplace(joint->joint_name).second) {
          fail("SRDF group '" + group.name +
               "' references an unknown or duplicate direct joint");
        }
      } else if (const auto* chain =
                     std::get_if<SrdfChainGroupMemberIr>(&member);
                 chain != nullptr) {
        if (!links.contains(chain->base_link_name) ||
            !links.contains(chain->tip_link_name) ||
            chain->base_link_name == chain->tip_link_name ||
            !link_is_ancestor(chain->base_link_name, chain->tip_link_name,
                              link_intervals) ||
            !direct_chains
                 .emplace(chain->base_link_name, chain->tip_link_name)
                 .second) {
          fail("SRDF group '" + group.name +
               "' contains an invalid or duplicate directed chain");
        }
      } else {
        const auto& subgroup = std::get<SrdfSubgroupMemberIr>(member);
        if (!groups.contains(subgroup.group_name) ||
            !direct_subgroups.emplace(subgroup.group_name).second) {
          fail("SRDF group '" + group.name +
               "' references an unknown or duplicate subgroup");
        }
      }
    }
  }
  std::unordered_map<std::string, const SrdfGroupIr*> group_by_name;
  group_by_name.reserve(document.groups.size());
  for (const auto& group : document.groups) {
    group_by_name.emplace(group.name, &group);
  }
  enum class Visit : std::uint8_t { kUnvisited = 0, kVisiting, kVisited };
  std::unordered_map<std::string, Visit> group_visit;
  group_visit.reserve(document.groups.size());
  std::vector<std::string> expansion_postorder;
  expansion_postorder.reserve(document.groups.size());
  for (const auto& group : document.groups) {
    if (group_visit[group.name] == Visit::kVisited) {
      continue;
    }
    struct Frame final {
      const SrdfGroupIr* group;
      std::size_t next_member = 0U;
    };
    std::vector<Frame> stack;
    group_visit[group.name] = Visit::kVisiting;
    stack.push_back(Frame{&group, 0U});
    while (!stack.empty()) {
      auto& frame = stack.back();
      while (frame.next_member < frame.group->members.size() &&
             !std::holds_alternative<SrdfSubgroupMemberIr>(
                 frame.group->members[frame.next_member])) {
        ++frame.next_member;
      }
      if (frame.next_member == frame.group->members.size()) {
        group_visit[frame.group->name] = Visit::kVisited;
        expansion_postorder.push_back(frame.group->name);
        stack.pop_back();
        continue;
      }
      const auto& subgroup_name =
          std::get<SrdfSubgroupMemberIr>(
              frame.group->members[frame.next_member++])
              .group_name;
      const auto child = group_by_name.at(subgroup_name);
      auto& child_state = group_visit[subgroup_name];
      if (child_state == Visit::kVisiting) {
        fail("SRDF subgroup relation contains a cycle");
      }
      if (child_state == Visit::kUnvisited) {
        child_state = Visit::kVisiting;
        stack.push_back(Frame{child, 0U});
      }
    }
  }
  std::set<std::pair<std::string, std::string>> state_names;
  std::unordered_map<std::string, UrdfCoordinateKind> coordinate_by_joint;
  coordinate_by_joint.reserve(accumulator.urdf.joints.size());
  for (const auto& joint : accumulator.urdf.joints) {
    coordinate_by_joint.emplace(joint.name, joint.coordinate_kind);
  }
  for (const auto& state : document.group_states) {
    if (!groups.contains(state.group_name)) {
      fail("SRDF group_state references an unknown group");
    }
    if (!state_names.emplace(state.group_name, state.name).second) {
      fail("SRDF contains a duplicate group_state name within one group");
    }
    std::unordered_set<std::string> state_joints;
    for (const auto& joint : state.joints) {
      if (!joints.contains(joint.joint_name) ||
          !state_joints.emplace(joint.joint_name).second) {
        fail("SRDF group_state contains an unknown or duplicate joint");
      }
      const auto kind = coordinate_by_joint.at(joint.joint_name);
      if (!is_scalar_coordinate(kind)) {
        fail("SRDF group_state cannot assign a non-scalar joint");
      }
    }
  }
  std::unordered_set<std::string> end_effector_names;
  for (const auto& end_effector : document.end_effectors) {
    if (!links.contains(end_effector.parent_link_name) ||
        !groups.contains(end_effector.group_name) ||
        (end_effector.parent_group_name.has_value() &&
         !groups.contains(*end_effector.parent_group_name)) ||
        !end_effector_names.emplace(end_effector.name).second) {
      fail("SRDF end_effector references an unknown link or group");
    }
  }
  std::unordered_set<std::string> passive_joints;
  for (const auto& joint : document.passive_joint_names) {
    if (!joints.contains(joint) || !passive_joints.emplace(joint).second) {
      fail("SRDF passive_joint is unknown or duplicated");
    }
  }
  std::set<std::pair<std::string, std::string>> disabled_pairs;
  for (const auto& disabled : document.disabled_collisions) {
    if (!links.contains(disabled.first_link_name) ||
        !links.contains(disabled.second_link_name) ||
        disabled.first_link_name == disabled.second_link_name) {
      fail("SRDF disable_collisions references an invalid link pair");
    }
    if (!disabled_pairs
             .emplace(disabled.first_link_name,
                      disabled.second_link_name)
             .second) {
      fail("SRDF contains a duplicate disabled-collision link pair");
    }
  }
  accumulator.group_expansion_postorder = std::move(expansion_postorder);
  accumulator.srdf = std::move(document);
}

template <typename Range, typename NameProjection>
[[nodiscard]] std::vector<ModelSourceNameIndexEntryIr> make_name_index(
    const Range& range, NameProjection name_projection) {
  std::vector<ModelSourceNameIndexEntryIr> result;
  result.reserve(range.size());
  for (std::size_t index = 0U; index < range.size(); ++index) {
    result.push_back(
        ModelSourceNameIndexEntryIr{name_projection(range[index]), index});
  }
  sort_by(result, [](const ModelSourceNameIndexEntryIr& item)
                      -> const std::string& { return item.name; });
  return result;
}

[[nodiscard]] std::vector<SourceArtifactReferenceIr>
canonical_artifact_references(const ParseAccumulator& accumulator) {
  std::vector<SourceArtifactReferenceIr> result;
  result.reserve(accumulator.referenced_artifacts.size());
  for (const auto& [logical_uri, kind] : accumulator.referenced_artifacts) {
    result.push_back(SourceArtifactReferenceIr{logical_uri, kind});
  }
  sort_by(result, [](const SourceArtifactReferenceIr& item) {
    return std::pair(item.logical_uri,
                     static_cast<std::uint8_t>(item.kind));
  });
  return result;
}

struct UrdfChainPath final {
  std::vector<std::string> link_names;
  std::vector<std::string> joint_names;
};

using ParentJointByChildIndex =
    std::unordered_map<std::string, const UrdfJointIr*>;

[[nodiscard]] ParentJointByChildIndex build_parent_joint_by_child_index(
    const UrdfDocumentIr& urdf) {
  ParentJointByChildIndex result;
  result.reserve(urdf.joints.size());
  for (const auto& joint : urdf.joints) {
    result.emplace(joint.child_link_name, &joint);
  }
  return result;
}

[[nodiscard]] UrdfChainPath resolve_chain_path(
    const ParentJointByChildIndex& parent_joint_by_child,
    std::string_view base_link, std::string_view tip_link) {
  UrdfChainPath path;
  std::string current(tip_link);
  path.link_names.push_back(current);
  while (current != base_link) {
    const auto parent = parent_joint_by_child.find(current);
    if (parent == parent_joint_by_child.end()) {
      fail("SRDF chain path cannot be resolved through the URDF tree");
    }
    path.joint_names.push_back(parent->second->name);
    current = parent->second->parent_link_name;
    path.link_names.push_back(current);
  }
  std::reverse(path.link_names.begin(), path.link_names.end());
  std::reverse(path.joint_names.begin(), path.joint_names.end());
  return path;
}

class SrdfSemanticBudget final {
 public:
  void consume(std::size_t count, std::string_view operation) {
    if (count > kMaxModelSourceExpandedSrdfSemanticRows - used_) {
      fail("expanded SRDF semantics exceed the aggregate row bound while " +
           std::string(operation));
    }
    used_ += count;
  }

 private:
  std::size_t used_ = 0U;
};

struct ExpandedGroupDraft final {
  ExpandedSrdfGroupIr value;
  std::unordered_map<std::string, std::size_t> link_index;
  std::unordered_map<std::string, std::size_t> joint_index;
};

enum class ExpansionMemberKind : std::uint8_t {
  kLink = 0,
  kJoint,
};

void add_expanded_occurrence(
    std::vector<SrdfExpandedMemberIr>& members,
    std::unordered_map<std::string, std::size_t>& member_index,
    std::string_view symbol, std::size_t provenance_index,
    SrdfSemanticBudget& budget) {
  const auto found = member_index.find(std::string(symbol));
  std::size_t index = 0U;
  if (found == member_index.end()) {
    budget.consume(1U, "retaining an expanded member");
    index = members.size();
    members.push_back(SrdfExpandedMemberIr{std::string(symbol), {}});
    member_index.emplace(members.back().name, index);
  } else {
    index = found->second;
  }
  members[index].provenance_indices.push_back(provenance_index);
}

[[nodiscard]] std::size_t add_provenance(
    ModelSourceSemanticIndex& index,
    SrdfExpansionProvenanceNodeIr provenance,
    SrdfSemanticBudget& budget) {
  budget.consume(1U, "retaining expansion provenance");
  const auto result = index.group_expansion_provenance.size();
  index.group_expansion_provenance.push_back(std::move(provenance));
  return result;
}

[[nodiscard]] ModelSourceSemanticIndex build_semantic_index(
    const ParseAccumulator& accumulator) {
  ModelSourceSemanticIndex index;
  index.root_link_name = accumulator.root_link_name;
  index.material_names = make_name_index(
      accumulator.urdf.materials, [](const UrdfMaterialIr& material) {
        return *material.name;
      });
  index.link_names = make_name_index(
      accumulator.urdf.links,
      [](const UrdfLinkIr& link) { return link.name; });
  index.joint_names = make_name_index(
      accumulator.urdf.joints,
      [](const UrdfJointIr& joint) { return joint.name; });

  if (!accumulator.srdf.has_value()) {
    return index;
  }
  const auto& srdf = *accumulator.srdf;
  index.group_names = make_name_index(
      srdf.groups, [](const SrdfGroupIr& group) { return group.name; });
  index.end_effector_names = make_name_index(
      srdf.end_effectors,
      [](const SrdfEndEffectorIr& end_effector) {
        return end_effector.name;
      });
  index.group_state_names.reserve(srdf.group_states.size());
  for (std::size_t source_index = 0U;
       source_index < srdf.group_states.size(); ++source_index) {
    const auto& state = srdf.group_states[source_index];
    index.group_state_names.push_back(ModelSourceGroupStateIndexEntryIr{
        state.group_name, state.name, source_index});
  }
  sort_by(index.group_state_names,
          [](const ModelSourceGroupStateIndexEntryIr& item) {
            return std::pair(item.group_name, item.state_name);
          });
  index.group_expansion_postorder =
      accumulator.group_expansion_postorder;

  std::unordered_map<std::string, const SrdfGroupIr*> group_by_name;
  group_by_name.reserve(srdf.groups.size());
  for (const auto& group : srdf.groups) {
    group_by_name.emplace(group.name, &group);
  }
  std::unordered_map<std::string, std::size_t> expanded_index_by_name;
  expanded_index_by_name.reserve(srdf.groups.size());
  std::vector<ExpandedGroupDraft> expanded;
  expanded.reserve(srdf.groups.size());
  SrdfSemanticBudget budget;
  const auto parent_joint_by_child =
      build_parent_joint_by_child_index(accumulator.urdf);

  for (const auto& group_name : accumulator.group_expansion_postorder) {
    const auto& group = *group_by_name.at(group_name);
    ExpandedGroupDraft draft;
    draft.value.group_name = group.name;

    for (std::size_t member_index = 0U;
         member_index < group.members.size(); ++member_index) {
      const auto& member = group.members[member_index];
      const auto add_direct =
          [&](std::string_view symbol, ExpansionMemberKind member_kind,
              SrdfExpansionProvenanceKind provenance_kind,
              const std::optional<SrdfChainGroupMemberIr>& chain) {
            auto provenance = SrdfExpansionProvenanceNodeIr{
                group.name, member_index, provenance_kind,
                std::string(symbol), std::nullopt, std::nullopt,
                std::nullopt};
            if (chain.has_value()) {
              provenance.chain_base_link_name = chain->base_link_name;
              provenance.chain_tip_link_name = chain->tip_link_name;
            }
            const auto provenance_index =
                add_provenance(index, std::move(provenance), budget);
            if (member_kind == ExpansionMemberKind::kLink) {
              add_expanded_occurrence(
                  draft.value.links, draft.link_index, symbol,
                  provenance_index, budget);
            } else {
              add_expanded_occurrence(
                  draft.value.joints, draft.joint_index, symbol,
                  provenance_index, budget);
            }
          };

      if (const auto* link =
              std::get_if<SrdfLinkGroupMemberIr>(&member);
          link != nullptr) {
        add_direct(link->link_name, ExpansionMemberKind::kLink,
                   SrdfExpansionProvenanceKind::kDirectLink,
                   std::nullopt);
      } else if (const auto* joint =
                     std::get_if<SrdfJointGroupMemberIr>(&member);
                 joint != nullptr) {
        add_direct(joint->joint_name, ExpansionMemberKind::kJoint,
                   SrdfExpansionProvenanceKind::kDirectJoint,
                   std::nullopt);
      } else if (const auto* chain =
                     std::get_if<SrdfChainGroupMemberIr>(&member);
                 chain != nullptr) {
        const auto path = resolve_chain_path(
            parent_joint_by_child, chain->base_link_name,
            chain->tip_link_name);
        for (const auto& link : path.link_names) {
          add_direct(link, ExpansionMemberKind::kLink,
                     SrdfExpansionProvenanceKind::kChainLink, *chain);
        }
        for (const auto& joint : path.joint_names) {
          add_direct(joint, ExpansionMemberKind::kJoint,
                     SrdfExpansionProvenanceKind::kChainJoint, *chain);
        }
      } else {
        const auto& subgroup = std::get<SrdfSubgroupMemberIr>(member);
        const auto child_index =
            expanded_index_by_name.at(subgroup.group_name);
        const auto& child = expanded[child_index].value;
        const auto propagate =
            [&](const std::vector<SrdfExpandedMemberIr>& child_members,
                ExpansionMemberKind member_kind) {
              for (const auto& child_member : child_members) {
                for (const auto nested_provenance :
                     child_member.provenance_indices) {
                  const auto provenance_index = add_provenance(
                      index,
                      SrdfExpansionProvenanceNodeIr{
                          group.name, member_index,
                          SrdfExpansionProvenanceKind::kSubgroup,
                          subgroup.group_name, std::nullopt, std::nullopt,
                          nested_provenance},
                      budget);
                  if (member_kind == ExpansionMemberKind::kLink) {
                    add_expanded_occurrence(
                        draft.value.links, draft.link_index,
                        child_member.name, provenance_index, budget);
                  } else {
                    add_expanded_occurrence(
                        draft.value.joints, draft.joint_index,
                        child_member.name, provenance_index, budget);
                  }
                }
              }
            };
        propagate(child.links, ExpansionMemberKind::kLink);
        propagate(child.joints, ExpansionMemberKind::kJoint);
      }
    }

    expanded_index_by_name.emplace(group.name, expanded.size());
    expanded.push_back(std::move(draft));
  }

  index.expanded_groups.reserve(expanded.size());
  for (auto& group : expanded) {
    index.expanded_groups.push_back(std::move(group.value));
  }

  std::unordered_map<std::string, const ExpandedSrdfGroupIr*>
      expanded_by_name;
  expanded_by_name.reserve(index.expanded_groups.size());
  for (const auto& group : index.expanded_groups) {
    expanded_by_name.emplace(group.group_name, &group);
  }
  std::unordered_map<std::string, UrdfCoordinateKind> coordinate_by_joint;
  coordinate_by_joint.reserve(accumulator.urdf.joints.size());
  for (const auto& joint : accumulator.urdf.joints) {
    coordinate_by_joint.emplace(joint.name, joint.coordinate_kind);
  }
  for (const auto& state : srdf.group_states) {
    const auto& expanded_group = *expanded_by_name.at(state.group_name);
    std::unordered_set<std::string> expanded_joints;
    expanded_joints.reserve(expanded_group.joints.size());
    for (const auto& joint : expanded_group.joints) {
      expanded_joints.emplace(joint.name);
    }
    for (const auto& joint : state.joints) {
      if (!expanded_joints.contains(joint.joint_name)) {
        fail("SRDF group_state assigns a joint outside its expanded group");
      }
      const auto kind = coordinate_by_joint.at(joint.joint_name);
      if (!is_scalar_coordinate(kind)) {
        fail("SRDF group_state cannot assign a non-scalar joint");
      }
    }
  }
  for (const auto& end_effector : srdf.end_effectors) {
    if (!end_effector.parent_group_name.has_value()) {
      continue;
    }
    const auto& parent =
        *expanded_by_name.at(*end_effector.parent_group_name);
    const bool parent_contains_link = std::any_of(
        parent.links.begin(), parent.links.end(),
        [&end_effector](const SrdfExpandedMemberIr& link) {
          return link.name == end_effector.parent_link_name;
        });
    if (!parent_contains_link) {
      fail("SRDF end_effector parent link is outside its parent group");
    }
  }
  return index;
}

void validate_declared_bindings(const core::ModelAssetDTO& asset,
                                const ParseAccumulator& source,
                                const ModelSourceSemanticIndex& index) {
  std::unordered_set<std::string> links;
  links.reserve(source.urdf.links.size());
  for (const auto& link : source.urdf.links) {
    links.emplace(link.name);
  }
  std::unordered_set<std::string> joints;
  for (const auto& joint : source.urdf.joints) {
    joints.emplace(joint.name);
  }
  std::unordered_set<std::string> groups;
  if (source.srdf.has_value()) {
    for (const auto& group : source.srdf->groups) {
      groups.emplace(group.name);
    }
  }

  const auto root_binding = std::find_if(
      asset.frame_role_bindings.begin(), asset.frame_role_bindings.end(),
      [&asset](const core::ModelFrameRoleBindingDTO& binding) {
        return binding.frame_role_id == asset.root_frame_role_id;
      });
  if (root_binding == asset.frame_role_bindings.end() ||
      root_binding->source_link_name != index.root_link_name) {
    fail("model root frame role does not resolve to the unique URDF root");
  }
  for (const auto& binding : asset.frame_role_bindings) {
    if (!links.contains(binding.source_link_name)) {
      fail("model frame role references an unknown URDF link");
    }
  }
  for (const auto& binding : asset.model_group_role_bindings) {
    if (!groups.contains(binding.source_group_name)) {
      fail("model group role references an unknown SRDF group");
    }
  }
  for (const auto& binding : asset.resource_role_bindings) {
    for (const auto& joint : binding.source_joint_names) {
      if (!joints.contains(joint)) {
        fail("model resource role references an unknown URDF joint");
      }
    }
  }
  for (const auto& binding : asset.collision_body_role_bindings) {
    for (const auto& link : binding.source_link_names) {
      if (!links.contains(link)) {
        fail("model collision-body role references an unknown URDF link");
      }
    }
  }
}

[[nodiscard]] std::string escape_xml_attribute(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const char byte : value) {
    switch (byte) {
      case '\t':
        result.append("&#9;");
        break;
      case '\n':
        result.append("&#10;");
        break;
      case '\r':
        result.append("&#13;");
        break;
      case '&':
        result.append("&amp;");
        break;
      case '<':
        result.append("&lt;");
        break;
      case '"':
        result.append("&quot;");
        break;
      default:
        result.push_back(byte);
        break;
    }
  }
  return result;
}

[[nodiscard]] std::string format_round_trip_double(double value) {
  if (!std::isfinite(value)) {
    fail("typed model source contains a non-finite number");
  }
  if (value == 0.0) {
    return "0";
  }
  std::array<char, 128U> buffer{};
  const auto result = std::to_chars(
      buffer.data(), buffer.data() + buffer.size(), value,
      std::chars_format::general);
  if (result.ec != std::errc{}) {
    fail("could not emit a typed model-source number");
  }
  return std::string(buffer.data(), result.ptr);
}

void append_attribute(std::string& output, std::string_view name,
                      std::string_view value) {
  output.push_back(' ');
  output.append(name);
  output.append("=\"");
  output.append(escape_xml_attribute(value));
  output.push_back('"');
}

void append_number_attribute(std::string& output, std::string_view name,
                             double value) {
  append_attribute(output, name, format_round_trip_double(value));
}

template <std::size_t Size>
[[nodiscard]] std::string format_number_array(
    const std::array<double, Size>& values) {
  std::string result;
  for (std::size_t index = 0U; index < Size; ++index) {
    if (index != 0U) {
      result.push_back(' ');
    }
    result.append(format_round_trip_double(values[index]));
  }
  return result;
}

template <typename Value, typename Compare>
[[nodiscard]] std::vector<const Value*> sorted_references(
    const std::vector<Value>& values, Compare compare) {
  std::vector<const Value*> result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.push_back(&value);
  }
  std::sort(result.begin(), result.end(),
            [&compare](const Value* left, const Value* right) {
              return compare(*left, *right);
            });
  return result;
}

template <typename Value, typename KeyProjection>
[[nodiscard]] std::vector<const Value*> sorted_references_by_string_key(
    const std::vector<Value>& values, KeyProjection key_projection) {
  std::vector<std::pair<std::string, const Value*>> keyed;
  keyed.reserve(values.size());
  for (const auto& value : values) {
    keyed.emplace_back(key_projection(value), &value);
  }
  std::sort(keyed.begin(), keyed.end(),
            [](const auto& left, const auto& right) {
              return left.first < right.first;
            });
  std::vector<const Value*> result;
  result.reserve(keyed.size());
  for (const auto& [key, value] : keyed) {
    (void)key;
    result.push_back(value);
  }
  return result;
}

void emit_origin(std::string& output, const UrdfOriginIr& origin) {
  const ModelSourceVector3 zero{0.0, 0.0, 0.0};
  if (origin.xyz_m == zero && origin.rpy_rad == zero) {
    return;
  }
  output.append("<origin");
  if (origin.xyz_m != zero) {
    append_attribute(output, "xyz", format_number_array(origin.xyz_m));
  }
  if (origin.rpy_rad != zero) {
    append_attribute(output, "rpy", format_number_array(origin.rpy_rad));
  }
  output.append("/>");
}

void emit_geometry(std::string& output, const UrdfGeometryIr& geometry) {
  output.append("<geometry>");
  std::visit(
      [&output](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, UrdfBoxGeometryIr>) {
          output.append("<box");
          append_attribute(output, "size", format_number_array(value.size_m));
          output.append("/>");
        } else if constexpr (std::is_same_v<Value,
                                            UrdfCylinderGeometryIr>) {
          output.append("<cylinder");
          append_number_attribute(output, "radius", value.radius_m);
          append_number_attribute(output, "length", value.length_m);
          output.append("/>");
        } else if constexpr (std::is_same_v<Value,
                                            UrdfSphereGeometryIr>) {
          output.append("<sphere");
          append_number_attribute(output, "radius", value.radius_m);
          output.append("/>");
        } else {
          output.append("<mesh");
          append_attribute(output, "filename", value.logical_uri);
          const ModelSourceVector3 default_scale{1.0, 1.0, 1.0};
          if (value.scale != default_scale) {
            append_attribute(output, "scale",
                             format_number_array(value.scale));
          }
          output.append("/>");
        }
      },
      geometry);
  output.append("</geometry>");
}

void emit_material(std::string& output, const UrdfMaterialIr& material) {
  output.append("<material");
  if (material.name.has_value()) {
    append_attribute(output, "name", *material.name);
  }
  if (!material.color.has_value() && !material.texture.has_value()) {
    output.append("/>");
    return;
  }
  output.push_back('>');
  if (material.color.has_value()) {
    output.append("<color");
    append_attribute(output, "rgba",
                     format_number_array(material.color->rgba));
    output.append("/>");
  }
  if (material.texture.has_value()) {
    output.append("<texture");
    append_attribute(output, "filename", material.texture->logical_uri);
    output.append("/>");
  }
  output.append("</material>");
}

void emit_inertial(std::string& output, const UrdfInertialIr& inertial) {
  output.append("<inertial>");
  emit_origin(output, inertial.origin);
  output.append("<mass");
  append_number_attribute(output, "value", inertial.mass_kg);
  output.append("/><inertia");
  append_number_attribute(output, "ixx", inertial.inertia.ixx);
  append_number_attribute(output, "ixy", inertial.inertia.ixy);
  append_number_attribute(output, "ixz", inertial.inertia.ixz);
  append_number_attribute(output, "iyy", inertial.inertia.iyy);
  append_number_attribute(output, "iyz", inertial.inertia.iyz);
  append_number_attribute(output, "izz", inertial.inertia.izz);
  output.append("/></inertial>");
}

void emit_visual(std::string& output, const UrdfVisualIr& visual) {
  output.append("<visual");
  if (visual.name.has_value()) {
    append_attribute(output, "name", *visual.name);
  }
  output.push_back('>');
  emit_origin(output, visual.origin);
  emit_geometry(output, visual.geometry);
  if (visual.material.has_value()) {
    emit_material(output, *visual.material);
  }
  output.append("</visual>");
}

void emit_collision(std::string& output,
                    const UrdfCollisionIr& collision) {
  output.append("<collision");
  if (collision.name.has_value()) {
    append_attribute(output, "name", *collision.name);
  }
  output.push_back('>');
  emit_origin(output, collision.origin);
  emit_geometry(output, collision.geometry);
  output.append("</collision>");
}

[[nodiscard]] std::string_view joint_type_text(UrdfJointType type) {
  switch (type) {
    case UrdfJointType::kRevolute:
      return "revolute";
    case UrdfJointType::kContinuous:
      return "continuous";
    case UrdfJointType::kPrismatic:
      return "prismatic";
    case UrdfJointType::kFixed:
      return "fixed";
    case UrdfJointType::kFloating:
      return "floating";
    case UrdfJointType::kPlanar:
      return "planar";
  }
  fail("typed model source contains an unsupported joint type");
}

template <typename Record>
void emit_optional_number_attributes(
    std::string& output, const Record& record,
    std::initializer_list<
        std::pair<std::string_view, std::optional<double> Record::*>> fields) {
  for (const auto& [name, member] : fields) {
    const auto& value = record.*member;
    if (value.has_value()) {
      append_number_attribute(output, name, *value);
    }
  }
}

void emit_joint(std::string& output, const UrdfJointIr& joint) {
  output.append("<joint");
  append_attribute(output, "name", joint.name);
  append_attribute(output, "type", joint_type_text(joint.type));
  output.push_back('>');
  emit_origin(output, joint.origin);
  output.append("<parent");
  append_attribute(output, "link", joint.parent_link_name);
  output.append("/><child");
  append_attribute(output, "link", joint.child_link_name);
  output.append("/>");
  const ModelSourceVector3 default_axis{1.0, 0.0, 0.0};
  if (joint.axis.has_value() && joint.axis->xyz != default_axis) {
    output.append("<axis");
    append_attribute(output, "xyz", format_number_array(joint.axis->xyz));
    output.append("/>");
  }
  if (joint.calibration.has_value()) {
    output.append("<calibration");
    emit_optional_number_attributes(
        output, *joint.calibration,
        {{"rising", &UrdfCalibrationIr::rising},
         {"falling", &UrdfCalibrationIr::falling}});
    if (joint.calibration->reference_position != 0.0) {
      append_number_attribute(output, "reference_position",
                              joint.calibration->reference_position);
    }
    output.append("/>");
  }
  if (joint.dynamics.has_value()) {
    output.append("<dynamics");
    append_number_attribute(output, "damping",
                            joint.dynamics->damping);
    append_number_attribute(output, "friction",
                            joint.dynamics->friction);
    output.append("/>");
  }
  if (joint.limit.has_value()) {
    output.append("<limit");
    append_number_attribute(output, "lower", joint.limit->lower);
    append_number_attribute(output, "upper", joint.limit->upper);
    append_number_attribute(output, "effort", joint.limit->effort);
    append_number_attribute(output, "velocity",
                            joint.limit->velocity);
    output.append("/>");
  }
  if (joint.mimic.has_value()) {
    output.append("<mimic");
    append_attribute(output, "joint", joint.mimic->joint_name);
    if (joint.mimic->multiplier != 1.0) {
      append_number_attribute(output, "multiplier",
                              joint.mimic->multiplier);
    }
    if (joint.mimic->offset != 0.0) {
      append_number_attribute(output, "offset",
                              joint.mimic->offset);
    }
    output.append("/>");
  }
  if (joint.safety_controller.has_value()) {
    output.append("<safety_controller");
    append_number_attribute(output, "soft_lower_limit",
                            joint.safety_controller->soft_lower_limit);
    append_number_attribute(output, "soft_upper_limit",
                            joint.safety_controller->soft_upper_limit);
    append_number_attribute(output, "k_position",
                            joint.safety_controller->k_position);
    append_number_attribute(output, "k_velocity",
                            joint.safety_controller->k_velocity);
    output.append("/>");
  }
  output.append("</joint>");
}

[[nodiscard]] std::string emit_urdf_document(const UrdfDocumentIr& urdf) {
  std::string output{"<robot"};
  append_attribute(output, "name", urdf.robot_name);
  output.push_back('>');
  const auto materials = sorted_references(
      urdf.materials,
      [](const UrdfMaterialIr& left, const UrdfMaterialIr& right) {
        return *left.name < *right.name;
      });
  for (const auto* material : materials) {
    emit_material(output, *material);
  }
  const auto links = sorted_references(
      urdf.links, [](const UrdfLinkIr& left, const UrdfLinkIr& right) {
        return left.name < right.name;
      });
  for (const auto* link_ptr : links) {
    const auto& link = *link_ptr;
    output.append("<link");
    append_attribute(output, "name", link.name);
    if (!link.inertial.has_value() && link.visuals.empty() &&
        link.collisions.empty()) {
      output.append("/>");
      continue;
    }
    output.push_back('>');
    if (link.inertial.has_value()) {
      emit_inertial(output, *link.inertial);
    }
    const auto visuals = sorted_references_by_string_key(
        link.visuals,
        [](const UrdfVisualIr& value) {
          std::string xml;
          emit_visual(xml, value);
          return xml;
        });
    for (const auto* visual : visuals) {
      emit_visual(output, *visual);
    }
    const auto collisions = sorted_references_by_string_key(
        link.collisions,
        [](const UrdfCollisionIr& value) {
          std::string xml;
          emit_collision(xml, value);
          return xml;
        });
    for (const auto* collision : collisions) {
      emit_collision(output, *collision);
    }
    output.append("</link>");
  }
  const auto joints = sorted_references(
      urdf.joints,
      [](const UrdfJointIr& left, const UrdfJointIr& right) {
        return left.name < right.name;
      });
  for (const auto* joint : joints) {
    emit_joint(output, *joint);
  }
  output.append("</robot>");
  if (output.size() > kMaxModelSourceXmlEntrypointBytes) {
    fail("canonical URDF exceeds the compiler-owned byte bound");
  }
  return output;
}

[[nodiscard]] std::string emit_srdf_document(const SrdfDocumentIr& srdf) {
  std::string output{"<robot"};
  append_attribute(output, "name", srdf.robot_name);
  output.push_back('>');
  const auto groups = sorted_references(
      srdf.groups,
      [](const SrdfGroupIr& left, const SrdfGroupIr& right) {
        return left.name < right.name;
      });
  for (const auto* group_ptr : groups) {
    const auto& group = *group_ptr;
    output.append("<group");
    append_attribute(output, "name", group.name);
    if (group.members.empty()) {
      output.append("/>");
      continue;
    }
    output.push_back('>');
    for (const auto& member : group.members) {
      std::visit(
          [&output](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value,
                                         SrdfLinkGroupMemberIr>) {
              output.append("<link");
              append_attribute(output, "name", value.link_name);
            } else if constexpr (std::is_same_v<
                                     Value, SrdfJointGroupMemberIr>) {
              output.append("<joint");
              append_attribute(output, "name", value.joint_name);
            } else if constexpr (std::is_same_v<
                                     Value, SrdfChainGroupMemberIr>) {
              output.append("<chain");
              append_attribute(output, "base_link",
                               value.base_link_name);
              append_attribute(output, "tip_link", value.tip_link_name);
            } else {
              output.append("<group");
              append_attribute(output, "name", value.group_name);
            }
            output.append("/>");
          },
          member);
    }
    output.append("</group>");
  }
  const auto states = sorted_references(
      srdf.group_states,
      [](const SrdfGroupStateIr& left, const SrdfGroupStateIr& right) {
        return std::pair(left.group_name, left.name) <
               std::pair(right.group_name, right.name);
      });
  for (const auto* state_ptr : states) {
    const auto& state = *state_ptr;
    output.append("<group_state");
    append_attribute(output, "name", state.name);
    append_attribute(output, "group", state.group_name);
    if (state.joints.empty()) {
      output.append("/>");
      continue;
    }
    output.push_back('>');
    const auto state_joints = sorted_references(
        state.joints,
        [](const SrdfGroupStateJointIr& left,
           const SrdfGroupStateJointIr& right) {
          return left.joint_name < right.joint_name;
        });
    for (const auto* joint_ptr : state_joints) {
      const auto& joint = *joint_ptr;
      output.append("<joint");
      append_attribute(output, "name", joint.joint_name);
      append_number_attribute(output, "value", joint.value);
      output.append("/>");
    }
    output.append("</group_state>");
  }
  const auto end_effectors = sorted_references(
      srdf.end_effectors,
      [](const SrdfEndEffectorIr& left,
         const SrdfEndEffectorIr& right) {
        return left.name < right.name;
      });
  for (const auto* end_effector_ptr : end_effectors) {
    const auto& end_effector = *end_effector_ptr;
    output.append("<end_effector");
    append_attribute(output, "name", end_effector.name);
    append_attribute(output, "parent_link",
                     end_effector.parent_link_name);
    append_attribute(output, "group", end_effector.group_name);
    if (end_effector.parent_group_name.has_value()) {
      append_attribute(output, "parent_group",
                       *end_effector.parent_group_name);
    }
    output.append("/>");
  }
  auto passive_joints = srdf.passive_joint_names;
  std::sort(passive_joints.begin(), passive_joints.end());
  for (const auto& passive_joint : passive_joints) {
    output.append("<passive_joint");
    append_attribute(output, "name", passive_joint);
    output.append("/>");
  }
  const auto disabled_collisions = sorted_references(
      srdf.disabled_collisions,
      [](const SrdfDisabledCollisionIr& left,
         const SrdfDisabledCollisionIr& right) {
        const auto left_pair =
            std::minmax(left.first_link_name, left.second_link_name);
        const auto right_pair =
            std::minmax(right.first_link_name, right.second_link_name);
        return std::tuple(left_pair.first, left_pair.second, left.reason) <
               std::tuple(right_pair.first, right_pair.second,
                          right.reason);
      });
  for (const auto* disabled_ptr : disabled_collisions) {
    const auto& disabled = *disabled_ptr;
    const auto canonical_pair =
        std::minmax(disabled.first_link_name, disabled.second_link_name);
    output.append("<disable_collisions");
    append_attribute(output, "link1", canonical_pair.first);
    append_attribute(output, "link2", canonical_pair.second);
    if (disabled.reason.has_value()) {
      append_attribute(output, "reason", *disabled.reason);
    }
    output.append("/>");
  }
  output.append("</robot>");
  if (output.size() > kMaxModelSourceXmlEntrypointBytes) {
    fail("canonical SRDF exceeds the compiler-owned byte bound");
  }
  return output;
}

[[nodiscard]] std::optional<ModelSourceArtifactKind>
opaque_artifact_kind(std::string_view media_type) {
  if (media_type == "model/stl") {
    return ModelSourceArtifactKind::kStlMesh;
  }
  if (media_type == "model/vnd.collada+xml") {
    return ModelSourceArtifactKind::kColladaMesh;
  }
  if (media_type == "image/png") {
    return ModelSourceArtifactKind::kPngTexture;
  }
  if (media_type == "image/jpeg") {
    return ModelSourceArtifactKind::kJpegTexture;
  }
  return std::nullopt;
}

}  // namespace

ModelSourceRetrievalPlan preflight_model_source_declaration(
    const QualifiedModelAsset& qualified_asset) {
  const auto& asset = qualified_asset.value();
  const auto& declared = asset.expanded_source.artifacts;
  if (declared.size() > core::kMaxModelAssetCollectionItems) {
    fail("model source artifact count exceeds the compiler bound");
  }

  std::unordered_set<std::string> declared_uris;
  std::vector<ModelSourceRetrievalPlanRow> rows;
  rows.reserve(declared.size());
  std::uint64_t declared_total = 0U;
  for (const auto& artifact : declared) {
    if (!declared_uris.emplace(artifact.logical_uri).second) {
      fail("model source declaration contains a duplicate logical URI");
    }
    ModelSourceArtifactKind artifact_kind;
    if (artifact.logical_uri == asset.expanded_source.urdf_logical_uri) {
      artifact_kind = ModelSourceArtifactKind::kUrdfXmlEntrypoint;
    } else if (asset.expanded_source.srdf_logical_uri.has_value() &&
               artifact.logical_uri ==
                   *asset.expanded_source.srdf_logical_uri) {
      artifact_kind = ModelSourceArtifactKind::kSrdfXmlEntrypoint;
    } else {
      const auto opaque_kind = opaque_artifact_kind(artifact.media_type);
      if (!opaque_kind.has_value()) {
        fail("model source dependency has an unsupported media type");
      }
      artifact_kind = *opaque_kind;
    }
    const bool entrypoint =
        artifact_kind == ModelSourceArtifactKind::kUrdfXmlEntrypoint ||
        artifact_kind == ModelSourceArtifactKind::kSrdfXmlEntrypoint;
    if (entrypoint) {
      if (artifact.media_type != "application/xml" ||
          artifact.byte_size > kMaxModelSourceXmlEntrypointBytes) {
        fail("model source XML entrypoint has an unsupported type or size");
      }
    } else if (artifact.byte_size >
               kMaxModelSourceOpaqueArtifactBytes) {
      fail("model source dependency has an unsupported type or size");
    }
    if (artifact.byte_size > kMaxModelSourceClosureBytes - declared_total) {
      fail("model source closure exceeds the compiler-owned byte bound");
    }
    declared_total += artifact.byte_size;
    rows.push_back(ModelSourceRetrievalPlanRow{
        artifact.logical_uri, artifact.media_type, artifact.byte_size,
        artifact.content_digest,
        core::ContentAddressedStoreKey(
            store_key_for_digest(artifact.content_digest)),
        artifact_kind});
  }
  sort_by(rows, [](const ModelSourceRetrievalPlanRow& row)
                    -> const std::string& { return row.logical_uri; });
  return ModelSourceRetrievalPlan(
      qualified_asset, std::move(rows), declared_total);
}

SourceVerifiedModelAsset verify_model_source_closure(
    const ModelSourceRetrievalPlan& retrieval_plan,
    std::vector<ResolvedModelSourceArtifactCandidate> resolved_artifacts) {
  const auto& qualified_asset = retrieval_plan.qualified_asset();
  const auto& asset = qualified_asset.value();
  const auto& declared = retrieval_plan.artifacts();
  if (resolved_artifacts.size() != declared.size()) {
    fail("resolved model source must contain exactly the declared artifacts");
  }

  std::unordered_map<std::string, const ModelSourceRetrievalPlanRow*>
      declared_by_uri;
  declared_by_uri.reserve(declared.size());
  for (const auto& artifact : declared) {
    declared_by_uri.emplace(artifact.logical_uri, &artifact);
  }
  std::unordered_map<std::string, std::string_view> bytes_by_uri;
  std::vector<VerifiedModelSourceArtifact> verified;
  verified.reserve(resolved_artifacts.size());
  XmlBudget collada_xml_budget;
  for (const auto& candidate : resolved_artifacts) {
    const auto declaration = declared_by_uri.find(candidate.logical_uri);
    if (declaration == declared_by_uri.end()) {
      fail("resolved model source contains an undeclared logical URI");
    }
    if (bytes_by_uri.contains(candidate.logical_uri)) {
      fail("resolved model source contains a duplicate logical URI");
    }
    const auto& expected = *declaration->second;
    if (candidate.resolved_store_key != expected.expected_store_key) {
      fail("resolved model source store key does not match its digest");
    }
    if (candidate.stored_bytes.size() != expected.byte_size) {
      fail("resolved model source byte size does not match its declaration");
    }
    if (sha256_digest(candidate.stored_bytes) !=
        expected.content_digest.value()) {
      fail("resolved model source bytes do not match their digest");
    }
    if (expected.artifact_kind ==
        ModelSourceArtifactKind::kColladaMesh) {
      validate_self_contained_collada(candidate.stored_bytes,
                                      collada_xml_budget);
    }
    bytes_by_uri.emplace(candidate.logical_uri, candidate.stored_bytes);
    verified.push_back(VerifiedModelSourceArtifact{
        expected.logical_uri, expected.media_type, expected.byte_size,
        expected.content_digest, expected.expected_store_key});
  }
  if (bytes_by_uri.size() != declared.size()) {
    fail("resolved model source is missing a declared artifact");
  }

  ParseAccumulator accumulator;
  XmlBudget urdf_srdf_xml_budget;
  const auto urdf_bytes =
      bytes_by_uri.find(asset.expanded_source.urdf_logical_uri);
  if (urdf_bytes == bytes_by_uri.end()) {
    fail("resolved model source is missing the URDF entrypoint");
  }
  parse_urdf(parse_xml(urdf_bytes->second, "URDF",
                       urdf_srdf_xml_budget),
             accumulator);

  if (asset.expanded_source.srdf_logical_uri.has_value()) {
    const auto srdf_bytes =
        bytes_by_uri.find(*asset.expanded_source.srdf_logical_uri);
    if (srdf_bytes == bytes_by_uri.end()) {
      fail("resolved model source is missing the SRDF entrypoint");
    }
    parse_srdf(parse_xml(srdf_bytes->second, "SRDF",
                         urdf_srdf_xml_budget),
               accumulator);
  }

  for (const auto& [uri, kind] : accumulator.referenced_artifacts) {
    const auto declaration = declared_by_uri.find(uri);
    if (declaration == declared_by_uri.end()) {
      fail("URDF references an artifact outside the exact source closure");
    }
    const bool compatible =
        (kind == SourceArtifactReferenceKind::kMesh &&
         (declaration->second->artifact_kind ==
              ModelSourceArtifactKind::kStlMesh ||
          declaration->second->artifact_kind ==
              ModelSourceArtifactKind::kColladaMesh)) ||
        (kind == SourceArtifactReferenceKind::kTexture &&
         (declaration->second->artifact_kind ==
              ModelSourceArtifactKind::kPngTexture ||
          declaration->second->artifact_kind ==
              ModelSourceArtifactKind::kJpegTexture));
    if (!compatible) {
      fail("URDF artifact reference has an incompatible media type");
    }
  }
  for (const auto& artifact : declared) {
    const bool entrypoint =
        artifact.artifact_kind ==
            ModelSourceArtifactKind::kUrdfXmlEntrypoint ||
        artifact.artifact_kind ==
            ModelSourceArtifactKind::kSrdfXmlEntrypoint;
    if (!entrypoint &&
        !accumulator.referenced_artifacts.contains(artifact.logical_uri)) {
      fail("model source closure contains an unreferenced artifact");
    }
  }
  auto semantic_index = build_semantic_index(accumulator);
  validate_declared_bindings(asset, accumulator, semantic_index);
  auto artifact_references = canonical_artifact_references(accumulator);
  sort_by(verified,
          [](const VerifiedModelSourceArtifact& item)
              -> const std::string& { return item.logical_uri; });

  auto source_ir =
      ModelSourceIr(std::move(accumulator.urdf),
                    std::move(accumulator.srdf),
                    std::move(semantic_index),
                    std::move(artifact_references));
  // A source-qualified value must remain re-emittable through the same
  // build-qualified ingress bounds. Canonical numeric spelling can differ
  // from the authored spelling, so validate the actual canonical bytes
  // before minting the trusted root.
  (void)emit_urdf_document(source_ir.urdf());
  if (source_ir.srdf().has_value()) {
    (void)emit_srdf_document(*source_ir.srdf());
  }

  return SourceVerifiedModelAsset(
      qualified_asset, std::move(verified), std::move(source_ir));
}

SourceVerifiedModelAsset verify_model_source_closure(
    const QualifiedModelAsset& qualified_asset,
    std::vector<ResolvedModelSourceArtifactCandidate> resolved_artifacts) {
  return verify_model_source_closure(
      preflight_model_source_declaration(qualified_asset),
      std::move(resolved_artifacts));
}

bool operator==(const ModelSourceIr& left, const ModelSourceIr& right) {
  return emit_urdf_document(left.urdf_) ==
             emit_urdf_document(right.urdf_) &&
         ((!left.srdf_.has_value() && !right.srdf_.has_value()) ||
          (left.srdf_.has_value() && right.srdf_.has_value() &&
           emit_srdf_document(*left.srdf_) ==
               emit_srdf_document(*right.srdf_))) &&
         left.semantic_index_ == right.semantic_index_ &&
         left.artifact_references_ == right.artifact_references_;
}

std::string emit_canonical_urdf(const ModelSourceIr& source_ir) {
  return emit_urdf_document(source_ir.urdf());
}

std::optional<std::string> emit_canonical_srdf(
    const ModelSourceIr& source_ir) {
  if (!source_ir.srdf().has_value()) {
    return std::nullopt;
  }
  return emit_srdf_document(*source_ir.srdf());
}

}  // namespace manyforge::deployment
