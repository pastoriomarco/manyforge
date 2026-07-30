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

void validate_optional_numbers(const XmlNode& node, std::string_view name,
                               std::size_t count) {
  const auto* value = find_attribute(node, name);
  if (value != nullptr) {
    (void)parse_numbers(*value, count, count, name);
  }
}

void validate_required_numbers(const XmlNode& node, std::string_view name,
                               std::size_t count) {
  const auto* value = find_attribute(node, name);
  if (value == nullptr) {
    fail("missing required attribute '" + std::string(name) + "' on <" +
         node.name + ">");
  }
  (void)parse_numbers(*value, count, count, name);
}

[[nodiscard]] ParsedUrdfJointType parse_joint_type(std::string_view value) {
  if (value == "revolute") {
    return ParsedUrdfJointType::kRevolute;
  }
  if (value == "continuous") {
    return ParsedUrdfJointType::kContinuous;
  }
  if (value == "prismatic") {
    return ParsedUrdfJointType::kPrismatic;
  }
  if (value == "fixed") {
    return ParsedUrdfJointType::kFixed;
  }
  if (value == "floating") {
    return ParsedUrdfJointType::kFloating;
  }
  if (value == "planar") {
    return ParsedUrdfJointType::kPlanar;
  }
  fail("URDF joint has an unsupported type");
}

struct ParseAccumulator final {
  enum class ArtifactReferenceKind : std::uint8_t {
    kMesh = 0,
    kTexture,
  };

  ParsedModelSourceIr ir;
  std::unordered_map<std::string, ArtifactReferenceKind>
      referenced_artifacts;
  std::unordered_set<std::string> top_level_materials;
  std::unordered_set<std::string> referenced_materials;
};

void add_artifact_reference(ParseAccumulator& accumulator,
                            const std::string& logical_uri,
                            ParseAccumulator::ArtifactReferenceKind kind) {
  const auto [found, inserted] =
      accumulator.referenced_artifacts.emplace(logical_uri, kind);
  if (!inserted && found->second != kind) {
    fail("one model source artifact is referenced as both mesh and texture");
  }
}

void parse_origin(const XmlNode& node) {
  require_no_text(node);
  require_allowed_attributes(node, {"xyz", "rpy"});
  validate_optional_numbers(node, "xyz", 3U);
  validate_optional_numbers(node, "rpy", 3U);
  if (!node.children.empty()) {
    fail("<origin> must be empty");
  }
}

void parse_geometry(const XmlNode& node, ParseAccumulator& accumulator) {
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
    validate_required_numbers(primitive, "size", 3U);
    return;
  }
  if (primitive.name == "cylinder") {
    require_allowed_attributes(primitive, {"radius", "length"},
                               {"radius", "length"});
    validate_required_numbers(primitive, "radius", 1U);
    validate_required_numbers(primitive, "length", 1U);
    return;
  }
  if (primitive.name == "sphere") {
    require_allowed_attributes(primitive, {"radius"}, {"radius"});
    validate_required_numbers(primitive, "radius", 1U);
    return;
  }
  if (primitive.name == "mesh") {
    require_allowed_attributes(primitive, {"filename", "scale"},
                               {"filename"});
    const auto& filename =
        require_logical_uri_attribute(primitive, "filename");
    add_artifact_reference(
        accumulator, filename,
        ParseAccumulator::ArtifactReferenceKind::kMesh);
    validate_optional_numbers(primitive, "scale", 3U);
    return;
  }
  fail("unsupported URDF geometry primitive <" + primitive.name + ">");
}

void parse_material(const XmlNode& node, ParseAccumulator& accumulator,
                    bool top_level) {
  require_no_text(node);
  require_allowed_attributes(node, {"name"}, top_level
                                                ? std::initializer_list<
                                                      std::string_view>{"name"}
                                                : std::initializer_list<
                                                      std::string_view>{});
  const auto* name = find_attribute(node, "name");
  if (name != nullptr) {
    validate_source_symbol(*name, "material name");
  }
  bool saw_color = false;
  bool saw_texture = false;
  for (const auto& child : node.children) {
    require_no_text(child);
    if (!child.children.empty()) {
      fail("URDF material children must be empty");
    }
    if (child.name == "color") {
      if (saw_color) {
        fail("URDF material repeats <color>");
      }
      saw_color = true;
      require_allowed_attributes(child, {"rgba"}, {"rgba"});
      validate_required_numbers(child, "rgba", 4U);
    } else if (child.name == "texture") {
      if (saw_texture) {
        fail("URDF material repeats <texture>");
      }
      saw_texture = true;
      require_allowed_attributes(child, {"filename"}, {"filename"});
      const auto& filename =
          require_logical_uri_attribute(child, "filename");
      add_artifact_reference(
          accumulator, filename,
          ParseAccumulator::ArtifactReferenceKind::kTexture);
    } else {
      fail("unsupported child <" + child.name + "> in <material>");
    }
  }
  if (top_level) {
    if (!accumulator.top_level_materials.emplace(*name).second) {
      fail("URDF contains duplicate top-level material name '" + *name +
           "'");
    }
    accumulator.ir.material_names.push_back(*name);
  } else if (!saw_color && !saw_texture) {
    if (name == nullptr) {
      fail("empty inline URDF material must reference a named material");
    }
    accumulator.referenced_materials.emplace(*name);
  }
}

void parse_inertial(const XmlNode& node) {
  require_no_text(node);
  require_allowed_attributes(node, {});
  bool saw_origin = false;
  bool saw_mass = false;
  bool saw_inertia = false;
  for (const auto& child : node.children) {
    if (child.name == "origin") {
      if (saw_origin) {
        fail("<inertial> repeats <origin>");
      }
      saw_origin = true;
      parse_origin(child);
    } else if (child.name == "mass") {
      if (saw_mass) {
        fail("<inertial> repeats <mass>");
      }
      saw_mass = true;
      require_no_text(child);
      require_allowed_attributes(child, {"value"}, {"value"});
      validate_required_numbers(child, "value", 1U);
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
      for (const auto field : {"ixx", "ixy", "ixz", "iyy", "iyz", "izz"}) {
        validate_required_numbers(child, field, 1U);
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
}

void parse_visual_or_collision(const XmlNode& node,
                               ParseAccumulator& accumulator,
                               bool is_visual) {
  require_no_text(node);
  require_allowed_attributes(node, {"name"});
  if (const auto* name = find_attribute(node, "name"); name != nullptr) {
    validate_source_symbol(*name, "geometry role name");
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
      parse_origin(child);
    } else if (child.name == "geometry") {
      if (saw_geometry) {
        fail("<" + node.name + "> repeats <geometry>");
      }
      saw_geometry = true;
      parse_geometry(child, accumulator);
    } else if (is_visual && child.name == "material") {
      if (saw_material) {
        fail("<visual> repeats <material>");
      }
      saw_material = true;
      parse_material(child, accumulator, false);
    } else {
      fail("unsupported child <" + child.name + "> in <" + node.name + ">");
    }
  }
  if (!saw_geometry) {
    fail("<" + node.name + "> requires exactly one <geometry>");
  }
}

void parse_link(const XmlNode& node, ParseAccumulator& accumulator) {
  require_no_text(node);
  require_allowed_attributes(node, {"name"}, {"name"});
  const auto& name = require_symbol_attribute(node, "name");
  accumulator.ir.link_names.push_back(name);
  bool saw_inertial = false;
  for (const auto& child : node.children) {
    if (child.name == "inertial") {
      if (saw_inertial) {
        fail("<link> repeats <inertial>");
      }
      saw_inertial = true;
      parse_inertial(child);
    } else if (child.name == "visual") {
      parse_visual_or_collision(child, accumulator, true);
    } else if (child.name == "collision") {
      parse_visual_or_collision(child, accumulator, false);
    } else {
      fail("unsupported child <" + child.name + "> in <link>");
    }
  }
}

void parse_scalar_attribute_element(
    const XmlNode& node,
    std::initializer_list<std::string_view> allowed,
    std::initializer_list<std::string_view> required) {
  require_no_text(node);
  require_allowed_attributes(node, allowed, required);
  for (const auto& [name, value] : node.attributes) {
    (void)parse_numbers(value, 1U, 1U, name);
  }
  if (!node.children.empty()) {
    fail("<" + node.name + "> must be empty");
  }
}

void parse_joint(const XmlNode& node, ParseAccumulator& accumulator) {
  require_no_text(node);
  require_allowed_attributes(node, {"name", "type"}, {"name", "type"});
  ParsedUrdfJoint joint{
      require_symbol_attribute(node, "name"),
      parse_joint_type(*find_attribute(node, "type")), {}, {}, std::nullopt};
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
      parse_origin(child);
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
      saw_axis = true;
      require_no_text(child);
      require_allowed_attributes(child, {"xyz"}, {"xyz"});
      validate_required_numbers(child, "xyz", 3U);
      if (!child.children.empty()) {
        fail("<axis> must be empty");
      }
    } else if (child.name == "calibration") {
      if (saw_calibration) {
        fail("<joint> repeats <calibration>");
      }
      saw_calibration = true;
      parse_scalar_attribute_element(
          child, {"rising", "falling", "reference_position"}, {});
    } else if (child.name == "dynamics") {
      if (saw_dynamics) {
        fail("<joint> repeats <dynamics>");
      }
      saw_dynamics = true;
      parse_scalar_attribute_element(child, {"damping", "friction"}, {});
    } else if (child.name == "limit") {
      if (saw_limit) {
        fail("<joint> repeats <limit>");
      }
      saw_limit = true;
      parse_scalar_attribute_element(
          child, {"lower", "upper", "effort", "velocity"}, {});
    } else if (child.name == "mimic") {
      if (saw_mimic) {
        fail("<joint> repeats <mimic>");
      }
      saw_mimic = true;
      require_no_text(child);
      require_allowed_attributes(child, {"joint", "multiplier", "offset"},
                                 {"joint"});
      joint.mimic_joint_name = require_symbol_attribute(child, "joint");
      validate_optional_numbers(child, "multiplier", 1U);
      validate_optional_numbers(child, "offset", 1U);
      if (!child.children.empty()) {
        fail("<mimic> must be empty");
      }
    } else if (child.name == "safety_controller") {
      if (saw_safety) {
        fail("<joint> repeats <safety_controller>");
      }
      saw_safety = true;
      parse_scalar_attribute_element(
          child,
          {"soft_lower_limit", "soft_upper_limit", "k_position",
           "k_velocity"},
          {});
    } else {
      fail("unsupported child <" + child.name + "> in <joint>");
    }
  }
  if (!saw_parent || !saw_child) {
    fail("<joint> requires exactly one <parent> and <child>");
  }
  accumulator.ir.joints.push_back(std::move(joint));
}

void validate_urdf_semantics(ParseAccumulator& accumulator) {
  auto& ir = accumulator.ir;
  std::unordered_set<std::string> links;
  for (const auto& link : ir.link_names) {
    if (!links.emplace(link).second) {
      fail("URDF contains duplicate link name '" + link + "'");
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
    if (joint.mimic_joint_name.has_value()) {
      if (!joints.contains(*joint.mimic_joint_name)) {
        fail("URDF joint '" + joint.name +
             "' mimics an unknown joint");
      }
      if (*joint.mimic_joint_name == joint.name) {
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
  ir.root_link_name = roots.front();
  std::unordered_set<std::string> visited;
  std::vector<std::string> pending{ir.root_link_name};
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

  std::unordered_map<std::string, const ParsedUrdfJoint*> joint_by_name;
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
    const ParsedUrdfJoint* current = &joint;
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
      if (!current->mimic_joint_name.has_value()) {
        break;
      }
      current = joint_by_name.at(*current->mimic_joint_name);
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
  accumulator.ir.robot_name = require_symbol_attribute(root, "name");
  for (const auto& child : root.children) {
    if (child.name == "link") {
      parse_link(child, accumulator);
    } else if (child.name == "joint") {
      parse_joint(child, accumulator);
    } else if (child.name == "material") {
      parse_material(child, accumulator, true);
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
  validate_urdf_semantics(accumulator);
}

struct LinkTraversalInterval final {
  std::size_t entered;
  std::size_t exited;
};

[[nodiscard]] std::unordered_map<std::string, LinkTraversalInterval>
build_link_traversal_intervals(const ParsedModelSourceIr& ir) {
  std::unordered_map<std::string, std::vector<std::string>>
      children_by_parent;
  children_by_parent.reserve(ir.joints.size());
  for (const auto& joint : ir.joints) {
    children_by_parent[joint.parent_link_name].push_back(
        joint.child_link_name);
  }

  struct Frame final {
    std::string_view link_name;
    std::size_t next_child = 0U;
  };
  std::unordered_map<std::string, LinkTraversalInterval> intervals;
  intervals.reserve(ir.link_names.size());
  std::vector<Frame> stack;
  stack.reserve(ir.link_names.size());
  std::size_t timestamp = 0U;
  intervals.emplace(ir.root_link_name,
                    LinkTraversalInterval{timestamp++, 0U});
  stack.push_back(Frame{ir.root_link_name, 0U});
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

void parse_srdf(const XmlNode& root, ParseAccumulator& accumulator) {
  require_no_text(root);
  if (root.name != "robot") {
    fail("SRDF root element must be <robot>");
  }
  require_allowed_attributes(root, {"name"}, {"name"});
  if (require_symbol_attribute(root, "name") != accumulator.ir.robot_name) {
    fail("SRDF robot name does not match the URDF robot name");
  }

  const std::unordered_set<std::string> links(
      accumulator.ir.link_names.begin(), accumulator.ir.link_names.end());
  std::unordered_set<std::string> joints;
  for (const auto& joint : accumulator.ir.joints) {
    joints.emplace(joint.name);
  }
  const auto link_intervals =
      build_link_traversal_intervals(accumulator.ir);

  for (const auto& node : root.children) {
    require_no_text(node);
    if (node.name == "virtual_joint") {
      fail("SRDF <virtual_joint> is forbidden: cell placement owns roots");
    }
    if (node.name == "group") {
      require_allowed_attributes(node, {"name"}, {"name"});
      ParsedSrdfGroup group;
      group.name = require_symbol_attribute(node, "name");
      for (const auto& child : node.children) {
        require_no_text(child);
        if (!child.children.empty()) {
          fail("SRDF group members must be empty");
        }
        if (child.name == "link") {
          require_allowed_attributes(child, {"name"}, {"name"});
          group.link_names.push_back(
              require_symbol_attribute(child, "name"));
        } else if (child.name == "joint") {
          require_allowed_attributes(child, {"name"}, {"name"});
          group.joint_names.push_back(
              require_symbol_attribute(child, "name"));
        } else if (child.name == "chain") {
          require_allowed_attributes(child, {"base_link", "tip_link"},
                                     {"base_link", "tip_link"});
          group.chains.push_back(
              ParsedSrdfChain{
                  require_symbol_attribute(child, "base_link"),
                  require_symbol_attribute(child, "tip_link")});
        } else if (child.name == "group") {
          require_allowed_attributes(child, {"name"}, {"name"});
          group.subgroup_names.push_back(
              require_symbol_attribute(child, "name"));
        } else {
          fail("unsupported SRDF group member <" + child.name + ">");
        }
      }
      accumulator.ir.groups.push_back(std::move(group));
    } else if (node.name == "group_state") {
      require_allowed_attributes(node, {"name", "group"},
                                 {"name", "group"});
      ParsedSrdfGroupState state{
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
        state.joints.push_back(ParsedSrdfGroupStateJoint{
            require_symbol_attribute(child, "name"), values.front()});
      }
      accumulator.ir.group_states.push_back(std::move(state));
    } else if (node.name == "end_effector") {
      require_allowed_attributes(
          node, {"name", "parent_link", "group", "parent_group"},
          {"name", "parent_link", "group"});
      if (!node.children.empty()) {
        fail("<end_effector> must be empty");
      }
      ParsedSrdfEndEffector end_effector{
          require_symbol_attribute(node, "name"),
          require_symbol_attribute(node, "parent_link"),
          require_symbol_attribute(node, "group"), std::nullopt};
      if (const auto* parent_group = find_attribute(node, "parent_group");
          parent_group != nullptr) {
        validate_source_symbol(*parent_group, "parent_group");
        end_effector.parent_group_name = *parent_group;
      }
      accumulator.ir.end_effectors.push_back(std::move(end_effector));
    } else if (node.name == "passive_joint") {
      require_allowed_attributes(node, {"name"}, {"name"});
      if (!node.children.empty()) {
        fail("<passive_joint> must be empty");
      }
      accumulator.ir.passive_joint_names.push_back(
          require_symbol_attribute(node, "name"));
    } else if (node.name == "disable_collisions") {
      require_allowed_attributes(node, {"link1", "link2", "reason"},
                                 {"link1", "link2"});
      if (!node.children.empty()) {
        fail("<disable_collisions> must be empty");
      }
      ParsedSrdfDisabledCollision disabled{
          require_symbol_attribute(node, "link1"),
          require_symbol_attribute(node, "link2"), std::nullopt};
      if (const auto* reason = find_attribute(node, "reason");
          reason != nullptr) {
        if (reason->size() > core::kMaxModelAssetLogicalUriBytes) {
          fail("SRDF disabled-collision reason exceeds the text bound");
        }
        disabled.reason = *reason;
      }
      accumulator.ir.disabled_collisions.push_back(std::move(disabled));
    } else {
      fail("unsupported top-level SRDF element <" + node.name + ">");
    }
  }

  std::unordered_set<std::string> groups;
  for (const auto& group : accumulator.ir.groups) {
    if (!groups.emplace(group.name).second) {
      fail("SRDF contains duplicate group name '" + group.name + "'");
    }
  }
  for (const auto& group : accumulator.ir.groups) {
    std::unordered_set<std::string> direct_links;
    for (const auto& link : group.link_names) {
      if (!links.contains(link) || !direct_links.emplace(link).second) {
        fail("SRDF group '" + group.name + "' references an unknown link");
      }
    }
    std::unordered_set<std::string> direct_joints;
    for (const auto& joint : group.joint_names) {
      if (!joints.contains(joint) || !direct_joints.emplace(joint).second) {
        fail("SRDF group '" + group.name + "' references an unknown joint");
      }
    }
    std::set<std::pair<std::string, std::string>> direct_chains;
    for (const auto& chain : group.chains) {
      if (!links.contains(chain.base_link_name) ||
          !links.contains(chain.tip_link_name) ||
          !link_is_ancestor(chain.base_link_name, chain.tip_link_name,
                            link_intervals) ||
          !direct_chains
               .emplace(chain.base_link_name, chain.tip_link_name)
               .second) {
        fail("SRDF group '" + group.name +
             "' contains an invalid directed chain");
      }
    }
    std::unordered_set<std::string> direct_subgroups;
    for (const auto& subgroup : group.subgroup_names) {
      if (!groups.contains(subgroup) ||
          !direct_subgroups.emplace(subgroup).second) {
        fail("SRDF group '" + group.name +
             "' references an unknown subgroup");
      }
    }
  }
  std::unordered_map<std::string, const ParsedSrdfGroup*> group_by_name;
  group_by_name.reserve(accumulator.ir.groups.size());
  for (const auto& group : accumulator.ir.groups) {
    group_by_name.emplace(group.name, &group);
  }
  enum class Visit : std::uint8_t { kUnvisited = 0, kVisiting, kVisited };
  std::unordered_map<std::string, Visit> group_visit;
  group_visit.reserve(accumulator.ir.groups.size());
  for (const auto& group : accumulator.ir.groups) {
    if (group_visit[group.name] == Visit::kVisited) {
      continue;
    }
    struct Frame final {
      const ParsedSrdfGroup* group;
      std::size_t next_subgroup = 0U;
    };
    std::vector<Frame> stack;
    group_visit[group.name] = Visit::kVisiting;
    stack.push_back(Frame{&group, 0U});
    while (!stack.empty()) {
      auto& frame = stack.back();
      if (frame.next_subgroup == frame.group->subgroup_names.size()) {
        group_visit[frame.group->name] = Visit::kVisited;
        stack.pop_back();
        continue;
      }
      const auto& subgroup_name =
          frame.group->subgroup_names[frame.next_subgroup++];
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
  for (const auto& state : accumulator.ir.group_states) {
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
    }
  }
  std::unordered_set<std::string> end_effector_names;
  for (const auto& end_effector : accumulator.ir.end_effectors) {
    if (!links.contains(end_effector.parent_link_name) ||
        !groups.contains(end_effector.group_name) ||
        (end_effector.parent_group_name.has_value() &&
         !groups.contains(*end_effector.parent_group_name)) ||
        !end_effector_names.emplace(end_effector.name).second) {
      fail("SRDF end_effector references an unknown link or group");
    }
  }
  std::unordered_set<std::string> passive_joints;
  for (const auto& joint : accumulator.ir.passive_joint_names) {
    if (!joints.contains(joint) || !passive_joints.emplace(joint).second) {
      fail("SRDF passive_joint is unknown or duplicated");
    }
  }
  std::set<std::pair<std::string, std::string>> disabled_pairs;
  for (auto& disabled : accumulator.ir.disabled_collisions) {
    if (!links.contains(disabled.first_link_name) ||
        !links.contains(disabled.second_link_name) ||
        disabled.first_link_name == disabled.second_link_name) {
      fail("SRDF disable_collisions references an invalid link pair");
    }
    if (disabled.second_link_name < disabled.first_link_name) {
      std::swap(disabled.first_link_name, disabled.second_link_name);
    }
    if (!disabled_pairs
             .emplace(disabled.first_link_name, disabled.second_link_name)
             .second) {
      fail("SRDF contains a duplicate disabled-collision link pair");
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

void canonicalize_ir(ParsedModelSourceIr& ir) {
  std::sort(ir.material_names.begin(), ir.material_names.end());
  std::sort(ir.link_names.begin(), ir.link_names.end());
  sort_by(ir.joints, [](const ParsedUrdfJoint& item) -> const std::string& {
    return item.name;
  });
  for (auto& group : ir.groups) {
    std::sort(group.link_names.begin(), group.link_names.end());
    std::sort(group.joint_names.begin(), group.joint_names.end());
    std::sort(group.subgroup_names.begin(), group.subgroup_names.end());
    sort_by(group.chains,
            [](const ParsedSrdfChain& item) {
              return std::pair(item.base_link_name, item.tip_link_name);
            });
  }
  sort_by(ir.groups, [](const ParsedSrdfGroup& item) -> const std::string& {
    return item.name;
  });
  for (auto& state : ir.group_states) {
    sort_by(state.joints,
            [](const ParsedSrdfGroupStateJoint& item)
                -> const std::string& { return item.joint_name; });
  }
  sort_by(ir.group_states,
          [](const ParsedSrdfGroupState& item) {
            return std::pair(item.group_name, item.name);
          });
  sort_by(ir.end_effectors,
          [](const ParsedSrdfEndEffector& item) -> const std::string& {
            return item.name;
          });
  std::sort(ir.passive_joint_names.begin(), ir.passive_joint_names.end());
  sort_by(ir.disabled_collisions,
          [](const ParsedSrdfDisabledCollision& item) {
            return std::pair(item.first_link_name, item.second_link_name);
          });
  std::sort(ir.referenced_artifact_logical_uris.begin(),
            ir.referenced_artifact_logical_uris.end());
}

void validate_declared_bindings(const core::ModelAssetDTO& asset,
                                const ParsedModelSourceIr& ir) {
  const std::unordered_set<std::string> links(ir.link_names.begin(),
                                               ir.link_names.end());
  std::unordered_set<std::string> joints;
  for (const auto& joint : ir.joints) {
    joints.emplace(joint.name);
  }
  std::unordered_set<std::string> groups;
  for (const auto& group : ir.groups) {
    groups.emplace(group.name);
  }

  const auto root_binding = std::find_if(
      asset.frame_role_bindings.begin(), asset.frame_role_bindings.end(),
      [&asset](const core::ModelFrameRoleBindingDTO& binding) {
        return binding.frame_role_id == asset.root_frame_role_id;
      });
  if (root_binding == asset.frame_role_bindings.end() ||
      root_binding->source_link_name != ir.root_link_name) {
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

[[nodiscard]] std::optional<ModelSourceArtifactKind>
opaque_artifact_kind(std::string_view media_type) {
  if (media_type == "model/stl") {
    return ModelSourceArtifactKind::kStlMesh;
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
    bytes_by_uri.emplace(candidate.logical_uri, candidate.stored_bytes);
    verified.push_back(VerifiedModelSourceArtifact{
        expected.logical_uri, expected.media_type, expected.byte_size,
        expected.content_digest, expected.expected_store_key});
  }
  if (bytes_by_uri.size() != declared.size()) {
    fail("resolved model source is missing a declared artifact");
  }

  ParseAccumulator accumulator;
  XmlBudget xml_budget;
  const auto urdf_bytes =
      bytes_by_uri.find(asset.expanded_source.urdf_logical_uri);
  if (urdf_bytes == bytes_by_uri.end()) {
    fail("resolved model source is missing the URDF entrypoint");
  }
  parse_urdf(parse_xml(urdf_bytes->second, "URDF", xml_budget), accumulator);

  if (asset.expanded_source.srdf_logical_uri.has_value()) {
    const auto srdf_bytes =
        bytes_by_uri.find(*asset.expanded_source.srdf_logical_uri);
    if (srdf_bytes == bytes_by_uri.end()) {
      fail("resolved model source is missing the SRDF entrypoint");
    }
    parse_srdf(parse_xml(srdf_bytes->second, "SRDF", xml_budget),
               accumulator);
  }

  for (const auto& [uri, kind] : accumulator.referenced_artifacts) {
    const auto declaration = declared_by_uri.find(uri);
    if (declaration == declared_by_uri.end()) {
      fail("URDF references an artifact outside the exact source closure");
    }
    const bool compatible =
        (kind == ParseAccumulator::ArtifactReferenceKind::kMesh &&
         declaration->second->artifact_kind ==
             ModelSourceArtifactKind::kStlMesh) ||
        (kind == ParseAccumulator::ArtifactReferenceKind::kTexture &&
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
  accumulator.ir.referenced_artifact_logical_uris.reserve(
      accumulator.referenced_artifacts.size());
  for (const auto& [uri, kind] : accumulator.referenced_artifacts) {
    (void)kind;
    accumulator.ir.referenced_artifact_logical_uris.push_back(uri);
  }
  validate_declared_bindings(asset, accumulator.ir);
  canonicalize_ir(accumulator.ir);
  sort_by(verified,
          [](const VerifiedModelSourceArtifact& item)
              -> const std::string& { return item.logical_uri; });

  return SourceVerifiedModelAsset(qualified_asset, std::move(verified),
                                  std::move(accumulator.ir));
}

SourceVerifiedModelAsset verify_model_source_closure(
    const QualifiedModelAsset& qualified_asset,
    std::vector<ResolvedModelSourceArtifactCandidate> resolved_artifacts) {
  return verify_model_source_closure(
      preflight_model_source_declaration(qualified_asset),
      std::move(resolved_artifacts));
}

}  // namespace manyforge::deployment
