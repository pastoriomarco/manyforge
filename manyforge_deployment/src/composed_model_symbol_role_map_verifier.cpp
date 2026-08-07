#include "composed_model_symbol_role_map_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "manyforge_deployment/artifact_codec.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

[[nodiscard]] const CanonicalValue::Object& require_object(
    const CanonicalValue& value, std::string_view path) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail_retained_bundle(std::string(path) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view path) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail_retained_bundle(std::string(path) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view path) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  if (found == object.end()) {
    fail_retained_bundle(std::string(path) + " is missing member " +
                         std::string(name));
  }
  return found->second;
}

void require_fields(const CanonicalValue::Object& object,
                    std::initializer_list<std::string_view> fields,
                    std::string_view path) {
  if (object.size() != fields.size()) {
    fail_retained_bundle(std::string(path) +
                         " has an incorrect closed field count");
  }
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(fields.begin(), fields.end(), name) == fields.end()) {
      fail_retained_bundle(std::string(path) +
                           " contains unknown member " + name);
    }
  }
  for (const auto field : fields) {
    (void)require_member(object, field, path);
  }
}

[[nodiscard]] const std::string& require_text(
    const CanonicalValue& value, std::string_view path,
    bool allow_empty = false) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr || (!allow_empty && text->empty())) {
    fail_retained_bundle(std::string(path) +
                         (allow_empty ? " must be text"
                                      : " must be non-empty text"));
  }
  return *text;
}

[[nodiscard]] std::optional<std::string> require_optional_identity(
    const CanonicalValue& value, std::string_view path) {
  if (std::holds_alternative<std::nullptr_t>(value.storage())) {
    return std::nullopt;
  }
  const std::string& text = require_text(value, path);
  require_retained_portable_identity(text, path);
  return text;
}

[[nodiscard]] std::size_t require_index(const CanonicalValue& value,
                                        std::string_view path) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) || *number < 0.0 ||
      std::trunc(*number) != *number ||
      *number > static_cast<double>(kMaxArtifactSafeInteger) ||
      *number > static_cast<double>(
                    std::numeric_limits<std::size_t>::max())) {
    fail_retained_bundle(std::string(path) +
                         " must be a non-negative bounded integer");
  }
  return static_cast<std::size_t>(*number);
}

[[nodiscard]] double require_finite(const CanonicalValue& value,
                                    std::string_view path) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number)) {
    fail_retained_bundle(std::string(path) + " must be finite numeric data");
  }
  return *number;
}

[[nodiscard]] bool require_bool(const CanonicalValue& value,
                                std::string_view path) {
  const auto* flag = std::get_if<bool>(&value.storage());
  if (flag == nullptr) {
    fail_retained_bundle(std::string(path) + " must be boolean");
  }
  return *flag;
}

[[nodiscard]] core::PlacementPoseDTO require_placement_pose(
    const CanonicalValue& value, std::string_view path) {
  const auto& pose = require_object(value, path);
  require_fields(pose, {"position_m", "orientation_xyzw"}, path);
  const auto& position = require_array(
      require_member(pose, "position_m", path),
      std::string(path) + ".position_m");
  const auto& orientation = require_array(
      require_member(pose, "orientation_xyzw", path),
      std::string(path) + ".orientation_xyzw");
  if (position.size() != 3U || orientation.size() != 4U) {
    fail_retained_bundle(std::string(path) +
                         " must contain a 3-vector and XYZW quaternion");
  }
  core::PlacementPoseDTO result;
  for (std::size_t index = 0U; index < result.position_m.size(); ++index) {
    result.position_m[index] = require_finite(
        position[index], std::string(path) + ".position_m[" +
                             std::to_string(index) + "]");
  }
  double norm_squared = 0.0;
  for (std::size_t index = 0U; index < result.orientation_xyzw.size();
       ++index) {
    result.orientation_xyzw[index] = require_finite(
        orientation[index], std::string(path) + ".orientation_xyzw[" +
                                std::to_string(index) + "]");
    norm_squared += result.orientation_xyzw[index] *
                    result.orientation_xyzw[index];
  }
  if (!std::isfinite(norm_squared) ||
      std::abs(norm_squared - 1.0) > 1.0e-6) {
    fail_retained_bundle(std::string(path) +
                         " quaternion norm squared must be within 1e-6 of one");
  }
  return result;
}

[[nodiscard]] std::uint8_t symbol_kind_rank(std::string_view kind,
                                            std::string_view path) {
  constexpr std::array<std::string_view, 10U> values{
      "model", "robot", "material", "link", "joint", "visual",
      "collision", "srdf_group", "srdf_group_state",
      "srdf_end_effector"};
  const auto found = std::find(values.begin(), values.end(), kind);
  if (found == values.end()) {
    fail_retained_bundle(std::string(path) + " has an unknown symbol kind");
  }
  return static_cast<std::uint8_t>(found - values.begin());
}

[[nodiscard]] std::uint8_t reference_kind_rank(std::string_view kind,
                                               std::string_view path) {
  constexpr std::array<std::string_view, 17U> values{
      "joint_parent_link",
      "joint_child_link",
      "joint_mimic",
      "visual_material",
      "srdf_group_link",
      "srdf_group_joint",
      "srdf_chain_base_link",
      "srdf_chain_tip_link",
      "srdf_subgroup",
      "srdf_group_state_group",
      "srdf_group_state_joint",
      "srdf_end_effector_parent_link",
      "srdf_end_effector_group",
      "srdf_end_effector_parent_group",
      "srdf_passive_joint",
      "srdf_disabled_collision_first_link",
      "srdf_disabled_collision_second_link"};
  const auto found = std::find(values.begin(), values.end(), kind);
  if (found == values.end()) {
    fail_retained_bundle(std::string(path) +
                         " has an unknown reference kind");
  }
  return static_cast<std::uint8_t>(found - values.begin());
}

void require_one_of(std::string_view value,
                    std::initializer_list<std::string_view> allowed,
                    std::string_view path) {
  if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
    fail_retained_bundle(std::string(path) + " has an unknown enum value");
  }
}

using SymbolKey =
    std::tuple<std::string, std::uint8_t, std::string, std::string>;
using SymbolIndex = std::map<SymbolKey, std::string>;
using ReverseSymbolIndex = std::map<std::string, SymbolKey, std::less<>>;

[[nodiscard]] const std::string& require_symbol(
    const SymbolIndex& symbols, std::string_view model_instance_id,
    std::string_view kind, std::string_view scope, std::string_view source,
    std::string_view path) {
  const auto found = symbols.find(
      {std::string(model_instance_id), symbol_kind_rank(kind, path),
       std::string(scope), std::string(source)});
  if (found == symbols.end()) {
    fail_retained_bundle(std::string(path) +
                         " references an absent source symbol");
  }
  return found->second;
}

[[nodiscard]] std::vector<std::string> require_text_array(
    const CanonicalValue& value, std::string_view path,
    bool require_non_empty = false) {
  const auto& rows = require_array(value, path);
  if (require_non_empty && rows.empty()) {
    fail_retained_bundle(std::string(path) + " must be non-empty");
  }
  std::vector<std::string> result;
  result.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    result.push_back(require_text(
        rows[index], std::string(path) + "[" + std::to_string(index) + "]"));
  }
  return result;
}

struct OccurrenceInfo final {
  std::string asset_id;
  std::string revision;
  std::optional<std::string> owner_component_id;
};

void validate_role_rows(const CanonicalValue::Object& occurrence,
                        std::string_view model_instance_id,
                        const SymbolIndex& symbols, std::string_view path) {
  const auto validate_scalar_role =
      [&](std::string_view array_name, std::string_view role_name,
          std::string_view source_name, std::string_view compiled_name,
          std::string_view symbol_kind) {
        const auto& rows = require_array(
            require_member(occurrence, array_name, path),
            std::string(path) + "." + std::string(array_name));
        std::string previous_role;
        for (std::size_t index = 0U; index < rows.size(); ++index) {
          const std::string row_path = std::string(path) + "." +
                                       std::string(array_name) + "[" +
                                       std::to_string(index) + "]";
          const auto& row = require_object(rows[index], row_path);
          require_fields(row, {role_name, source_name, compiled_name},
                         row_path);
          const std::string& role = require_text(
              require_member(row, role_name, row_path), row_path);
          require_retained_portable_identity(role, row_path);
          if (!previous_role.empty() && previous_role >= role) {
            fail_retained_bundle(std::string(array_name) +
                                 " are not canonical and unique");
          }
          previous_role = role;
          const std::string& source = require_text(
              require_member(row, source_name, row_path), row_path);
          const std::string& compiled = require_text(
              require_member(row, compiled_name, row_path), row_path);
          if (compiled != require_symbol(
                              symbols, model_instance_id, symbol_kind, {},
                              source, row_path)) {
            fail_retained_bundle(row_path +
                                 " does not match the symbol bijection");
          }
        }
      };
  validate_scalar_role("frame_roles", "frame_role_id", "source_link_name",
                       "compiled_link_name", "link");
  validate_scalar_role("model_group_roles", "model_group_role_id",
                       "source_group_name", "compiled_group_name",
                       "srdf_group");

  const auto validate_vector_role =
      [&](std::string_view array_name, std::string_view role_name,
          std::string_view source_names, std::string_view compiled_names,
          std::string_view symbol_kind) {
        const auto& rows = require_array(
            require_member(occurrence, array_name, path),
            std::string(path) + "." + std::string(array_name));
        std::string previous_role;
        for (std::size_t index = 0U; index < rows.size(); ++index) {
          const std::string row_path = std::string(path) + "." +
                                       std::string(array_name) + "[" +
                                       std::to_string(index) + "]";
          const auto& row = require_object(rows[index], row_path);
          require_fields(row, {role_name, source_names, compiled_names},
                         row_path);
          const std::string& role = require_text(
              require_member(row, role_name, row_path), row_path);
          require_retained_portable_identity(role, row_path);
          if (!previous_role.empty() && previous_role >= role) {
            fail_retained_bundle(std::string(array_name) +
                                 " are not canonical and unique");
          }
          previous_role = role;
          const auto sources = require_text_array(
              require_member(row, source_names, row_path), row_path, true);
          const auto compiled = require_text_array(
              require_member(row, compiled_names, row_path), row_path, true);
          if (sources.size() != compiled.size()) {
            fail_retained_bundle(row_path +
                                 " source/compiled cardinalities differ");
          }
          std::set<std::string> unique_sources;
          for (std::size_t item = 0U; item < sources.size(); ++item) {
            if (!unique_sources.insert(sources[item]).second ||
                compiled[item] != require_symbol(
                                      symbols, model_instance_id, symbol_kind,
                                      {}, sources[item], row_path)) {
              fail_retained_bundle(row_path +
                                   " is not an ordered exact symbol map");
            }
          }
        }
      };
  validate_vector_role("resource_roles", "resource_role_id",
                       "source_joint_names", "compiled_joint_names",
                       "joint");
  validate_vector_role("collision_body_roles", "collision_body_role_id",
                       "source_link_names", "compiled_link_names", "link");
}

void validate_resolved_frame(
    const CanonicalValue& value, const SymbolIndex& symbols,
    const std::map<std::string, OccurrenceInfo, std::less<>>& occurrences,
    std::string_view cell_id, std::string_view cell_frame,
    std::string_view expected_owner_component_id, std::string_view path) {
  const auto& row = require_object(value, path);
  const std::string& kind = require_text(
      require_member(row, "kind", path), std::string(path) + ".kind");
  if (kind == "cell") {
    require_fields(row, {"kind", "cell_id", "compiled_frame_name"}, path);
    if (require_text(require_member(row, "cell_id", path), path) != cell_id ||
        require_text(require_member(row, "compiled_frame_name", path), path) !=
            cell_frame) {
      fail_retained_bundle(std::string(path) +
                           " does not select the exact cell frame");
    }
    fail_retained_bundle(std::string(path) +
                         " cannot use the cell frame as a component target");
  }
  const bool component = kind == "component_model";
  if (!component && kind != "passive_model") {
    fail_retained_bundle(std::string(path) + " has an unknown frame kind");
  }
  if (component) {
    require_fields(row,
                   {"kind", "controllable_component_id",
                    "model_instance_id", "model_revision", "frame_role_id",
                    "compiled_frame_name"},
                   path);
  } else {
    require_fields(row,
                   {"kind", "model_instance_id", "model_revision",
                    "frame_role_id", "compiled_frame_name"},
                   path);
  }
  const std::string& model = require_text(
      require_member(row, "model_instance_id", path), path);
  const auto occurrence = occurrences.find(model);
  if (occurrence == occurrences.end() ||
      require_text(require_member(row, "model_revision", path), path) !=
          occurrence->second.revision) {
    fail_retained_bundle(std::string(path) +
                         " selects an unknown model occurrence");
  }
  if (component) {
    const std::string& owner = require_text(
        require_member(row, "controllable_component_id", path), path);
    if (!occurrence->second.owner_component_id.has_value() ||
        *occurrence->second.owner_component_id != owner ||
        owner != expected_owner_component_id) {
      fail_retained_bundle(std::string(path) +
                           " disagrees with model component ownership");
    }
  } else {
    fail_retained_bundle(std::string(path) +
                         " must select a component-owned model frame");
  }
  const std::string& compiled = require_text(
      require_member(row, "compiled_frame_name", path), path);
  const bool matches_link = std::any_of(
      symbols.begin(), symbols.end(),
      [&model, &compiled](const auto& symbol) {
        return std::get<0>(symbol.first) == model &&
               std::get<1>(symbol.first) == 3U && symbol.second == compiled;
      });
  if (!matches_link) {
    fail_retained_bundle(std::string(path) +
                         " does not name a compiled link in the occurrence");
  }
  require_retained_portable_identity(
      require_text(require_member(row, "frame_role_id", path), path), path);
}

}  // namespace

CanonicalValue parse_and_verify_composed_model_symbol_role_map(
    std::string_view canonical_bytes) {
  if (canonical_bytes.empty() ||
      canonical_bytes.size() > kMaxComposedModelSymbolRoleMapBytes) {
    fail_retained_bundle(
        "composed model symbol/role map violates its byte bound");
  }
  CanonicalValue root;
  try {
    root = parse_strict_json_model_yaml(canonical_bytes).root();
  } catch (const ArtifactCodecError& error) {
    fail_retained_bundle(
        std::string("composed model symbol/role map cannot be decoded: ") +
        error.what());
  }
  if (canonical_json(root) != canonical_bytes) {
    fail_retained_bundle(
        "composed model symbol/role map is not exact canonical JSON");
  }
  const auto& object = require_object(root, "symbol_role_map");
  require_fields(object,
                 {"api_version", "cell_id", "cell_frame",
                  "model_occurrences", "symbol_rows", "reference_rows",
                  "coordinate_rows", "fixed_connections", "frame_paths",
                  "control_binding_coordinate_maps", "target_role_maps"},
                 "symbol_role_map");
  if (require_text(require_member(object, "api_version", "symbol_role_map"),
                   "symbol_role_map.api_version") !=
      kComposedModelSymbolRoleMapApiVersion) {
    fail_retained_bundle(
        "composed model symbol/role map has an unsupported API version");
  }
  const std::string& cell_id = require_text(
      require_member(object, "cell_id", "symbol_role_map"),
      "symbol_role_map.cell_id");
  require_retained_portable_identity(cell_id, "symbol_role_map.cell_id");
  const auto& cell_frame_object = require_object(
      require_member(object, "cell_frame", "symbol_role_map"),
      "symbol_role_map.cell_frame");
  require_fields(cell_frame_object, {"compiled_frame_name"},
                 "symbol_role_map.cell_frame");
  const std::string& cell_frame = require_text(
      require_member(cell_frame_object, "compiled_frame_name",
                     "symbol_role_map.cell_frame"),
      "symbol_role_map.cell_frame.compiled_frame_name");
  require_retained_portable_identity(
      cell_frame, "symbol_role_map.cell_frame.compiled_frame_name");

  const auto& symbol_values = require_array(
      require_member(object, "symbol_rows", "symbol_role_map"),
      "symbol_role_map.symbol_rows");
  if (symbol_values.empty()) {
    fail_retained_bundle(
        "a retained composed-model map must contain source symbols");
  }
  SymbolIndex symbols;
  ReverseSymbolIndex reverse_symbols;
  std::optional<SymbolKey> previous_symbol;
  for (std::size_t index = 0U; index < symbol_values.size(); ++index) {
    const std::string path =
        "symbol_role_map.symbol_rows[" + std::to_string(index) + "]";
    const auto& row = require_object(symbol_values[index], path);
    require_fields(row, {"model_instance_id", "kind", "scope_name",
                         "source_name", "compiled_name"},
                   path);
    const std::string& model = require_text(
        require_member(row, "model_instance_id", path), path);
    require_retained_portable_identity(model, path);
    const std::string& kind = require_text(
        require_member(row, "kind", path), path);
    const std::string& scope = require_text(
        require_member(row, "scope_name", path), path, true);
    const std::string& source = require_text(
        require_member(row, "source_name", path), path);
    const std::string& compiled = require_text(
        require_member(row, "compiled_name", path), path);
    require_retained_portable_identity(compiled, path);
    SymbolKey key{model, symbol_kind_rank(kind, path), scope, source};
    if (previous_symbol.has_value() && *previous_symbol >= key) {
      fail_retained_bundle(
          "symbol rows are not strictly canonical and source-unique");
    }
    previous_symbol = key;
    if (!symbols.emplace(key, compiled).second ||
        !reverse_symbols.emplace(compiled, key).second) {
      fail_retained_bundle(
          "symbol rows are not a one-to-one reversible map");
    }
  }

  const auto& occurrence_values = require_array(
      require_member(object, "model_occurrences", "symbol_role_map"),
      "symbol_role_map.model_occurrences");
  if (occurrence_values.empty()) {
    fail_retained_bundle(
        "a retained composed-model map must contain model occurrences");
  }
  std::map<std::string, OccurrenceInfo, std::less<>> occurrences;
  using FramePathKey =
      std::tuple<std::uint8_t, std::string, std::string, std::string,
                 std::string, std::string>;
  std::set<FramePathKey> expected_frame_paths;
  expected_frame_paths.emplace(0U, cell_id, std::string{}, std::string{},
                               std::string{}, cell_frame);
  std::string previous_model;
  for (std::size_t index = 0U; index < occurrence_values.size(); ++index) {
    const std::string path = "symbol_role_map.model_occurrences[" +
                             std::to_string(index) + "]";
    const auto& row = require_object(occurrence_values[index], path);
    require_fields(
        row,
        {"model_instance_id", "model_asset_id", "model_revision",
         "owner_component_id", "owner_model_role_id", "compiled_model_name",
         "compiled_robot_name", "compiled_root_link_name", "frame_roles",
         "model_group_roles", "resource_roles", "collision_body_roles"},
        path);
    const std::string& model = require_text(
        require_member(row, "model_instance_id", path), path);
    const std::string& asset = require_text(
        require_member(row, "model_asset_id", path), path);
    const std::string& revision = require_text(
        require_member(row, "model_revision", path), path);
    require_retained_portable_identity(model, path);
    require_retained_portable_identity(asset, path);
    require_retained_digest(revision, path);
    if (!previous_model.empty() && previous_model >= model) {
      fail_retained_bundle(
          "model occurrences are not strictly canonical and unique");
    }
    previous_model = model;
    const auto owner = require_optional_identity(
        require_member(row, "owner_component_id", path), path);
    const auto owner_role = require_optional_identity(
        require_member(row, "owner_model_role_id", path), path);
    if (owner.has_value() != owner_role.has_value()) {
      fail_retained_bundle(
          path + " has only one half of component/model-role ownership");
    }
    if (!occurrences.emplace(model, OccurrenceInfo{asset, revision, owner})
             .second) {
      fail_retained_bundle(path + " duplicates a model occurrence");
    }
    const auto require_occurrence_symbol =
        [&](std::string_view member_name, std::string_view kind,
            std::string_view source) {
          const std::string& compiled = require_text(
              require_member(row, member_name, path), path);
          if (compiled !=
              require_symbol(symbols, model, kind, {}, source, path)) {
            fail_retained_bundle(path +
                                 " occurrence name differs from symbol map");
          }
        };
    require_occurrence_symbol("compiled_model_name", "model", model);
    const std::string& compiled_robot = require_text(
        require_member(row, "compiled_robot_name", path), path);
    const bool has_robot = std::any_of(
        symbols.begin(), symbols.end(), [&](const auto& symbol) {
          return std::get<0>(symbol.first) == model &&
                 std::get<1>(symbol.first) == 1U &&
                 symbol.second == compiled_robot;
        });
    const std::string& compiled_root = require_text(
        require_member(row, "compiled_root_link_name", path), path);
    const bool has_root = std::any_of(
        symbols.begin(), symbols.end(), [&](const auto& symbol) {
          return std::get<0>(symbol.first) == model &&
                 std::get<1>(symbol.first) == 3U &&
                 symbol.second == compiled_root;
        });
    if (!has_robot || !has_root) {
      fail_retained_bundle(path +
                           " omits a compiled robot or root-link symbol");
    }
    validate_role_rows(row, model, symbols, path);
    for (const auto& frame_role : require_array(
             require_member(row, "frame_roles", path),
             path + ".frame_roles")) {
      const std::string& role =
          require_text(require_member(require_object(frame_role, path),
                                      "frame_role_id", path),
                       path);
      const std::string& compiled =
          require_text(require_member(require_object(frame_role, path),
                                      "compiled_link_name", path),
                       path);
      const auto key = owner.has_value()
                           ? FramePathKey{1U, *owner, model, revision, role,
                                          compiled}
                           : FramePathKey{2U, std::string{}, model, revision,
                                          role, compiled};
      if (!expected_frame_paths.insert(key).second) {
        fail_retained_bundle(path +
                             " duplicates an exported frame-path identity");
      }
    }
  }
  for (const auto& [key, unused] : symbols) {
    (void)unused;
    if (!occurrences.contains(std::get<0>(key))) {
      fail_retained_bundle(
          "symbol map contains a row outside the model-occurrence set");
    }
  }

  const auto& reference_values = require_array(
      require_member(object, "reference_rows", "symbol_role_map"),
      "symbol_role_map.reference_rows");
  using ReferenceKey = std::tuple<std::string, std::uint8_t, std::string,
                                  std::string, std::size_t, SymbolKey>;
  std::optional<ReferenceKey> previous_reference;
  using MimicOwnerKey = std::pair<std::string, std::string>;
  std::map<MimicOwnerKey, std::string> mimic_reference_targets;
  for (std::size_t index = 0U; index < reference_values.size(); ++index) {
    const std::string path = "symbol_role_map.reference_rows[" +
                             std::to_string(index) + "]";
    const auto& row = require_object(reference_values[index], path);
    require_fields(
        row,
        {"model_instance_id", "kind", "owner_scope_name",
         "owner_source_name", "reference_ordinal",
         "target_model_instance_id", "target_kind", "target_scope_name",
         "target_source_name", "compiled_target_name"},
        path);
    const std::string& model = require_text(
        require_member(row, "model_instance_id", path),
        path + ".model_instance_id");
    const std::string& kind = require_text(
        require_member(row, "kind", path), path + ".kind");
    const std::string& owner_scope = require_text(
        require_member(row, "owner_scope_name", path),
        path + ".owner_scope_name", true);
    const std::string& owner_source = require_text(
        require_member(row, "owner_source_name", path),
        path + ".owner_source_name", true);
    const std::size_t ordinal = require_index(
        require_member(row, "reference_ordinal", path),
        path + ".reference_ordinal");
    const std::string& target_model = require_text(
        require_member(row, "target_model_instance_id", path),
        path + ".target_model_instance_id");
    const std::string& target_kind = require_text(
        require_member(row, "target_kind", path), path + ".target_kind");
    const std::string& target_scope = require_text(
        require_member(row, "target_scope_name", path),
        path + ".target_scope_name", true);
    const std::string& target_source = require_text(
        require_member(row, "target_source_name", path),
        path + ".target_source_name");
    const std::string& compiled = require_text(
        require_member(row, "compiled_target_name", path),
        path + ".compiled_target_name");
    const SymbolKey target{target_model,
                           symbol_kind_rank(target_kind, path),
                           target_scope, target_source};
    ReferenceKey key{model, reference_kind_rank(kind, path), owner_scope,
                     owner_source, ordinal, target};
    if (previous_reference.has_value() && *previous_reference >= key) {
      fail_retained_bundle(
          "reference rows are not strictly canonical and unique");
    }
    previous_reference = key;
    const auto target_row = symbols.find(target);
    if (!occurrences.contains(model) || target_row == symbols.end() ||
        target_row->second != compiled) {
      fail_retained_bundle(
          path + " does not resolve through the exact symbol bijection");
    }
    if (kind == "joint_mimic") {
      if (!owner_scope.empty() || owner_source.empty() || ordinal != 0U ||
          target_model != model || target_kind != "joint" ||
          !target_scope.empty() ||
          !symbols.contains(
              {model, symbol_kind_rank("joint", path), {}, owner_source})) {
        fail_retained_bundle(
            path + " is not a closed same-occurrence joint-mimic reference");
      }
      if (!mimic_reference_targets
               .emplace(MimicOwnerKey{model, owner_source}, compiled)
               .second) {
        fail_retained_bundle(
            path + " duplicates one joint's mimic reference");
      }
    }
  }

  const auto& coordinate_values = require_array(
      require_member(object, "coordinate_rows", "symbol_role_map"),
      "symbol_role_map.coordinate_rows");
  using CoordinateKey = std::pair<std::string, std::size_t>;
  std::optional<CoordinateKey> previous_coordinate;
  std::set<std::tuple<std::string, std::string, std::string>>
      coordinate_identity;
  struct CoordinateInfo final {
    std::string model_instance_id;
    std::string source_joint_name;
    std::size_t source_joint_index;
  };
  std::map<std::string, CoordinateInfo, std::less<>>
      coordinates_by_compiled_name;
  std::map<MimicOwnerKey, std::string> coordinate_mimic_targets;
  for (std::size_t index = 0U; index < coordinate_values.size(); ++index) {
    const std::string path = "symbol_role_map.coordinate_rows[" +
                             std::to_string(index) + "]";
    const auto& row = require_object(coordinate_values[index], path);
    require_fields(
        row,
        {"model_instance_id", "owner_component_id", "source_joint_index",
         "source_joint_name", "compiled_joint_name", "joint_type",
         "coordinate_kind", "unit", "mimic_source_compiled_joint_name",
         "mimic_multiplier", "mimic_offset"},
        path);
    const std::string& model = require_text(
        require_member(row, "model_instance_id", path), path);
    const auto owner = require_optional_identity(
        require_member(row, "owner_component_id", path), path);
    const auto occurrence = occurrences.find(model);
    if (occurrence == occurrences.end() ||
        occurrence->second.owner_component_id != owner) {
      fail_retained_bundle(path +
                           " disagrees with model occurrence ownership");
    }
    const std::size_t source_index = require_index(
        require_member(row, "source_joint_index", path), path);
    CoordinateKey key{model, source_index};
    if (previous_coordinate.has_value() && *previous_coordinate >= key) {
      fail_retained_bundle(
          "coordinate rows are not strictly source-order canonical");
    }
    previous_coordinate = key;
    const std::string& source = require_text(
        require_member(row, "source_joint_name", path), path);
    const std::string& compiled = require_text(
        require_member(row, "compiled_joint_name", path), path);
    if (compiled != require_symbol(symbols, model, "joint", {}, source,
                                   path)) {
      fail_retained_bundle(path + " differs from the joint symbol map");
    }
    require_one_of(require_text(require_member(row, "joint_type", path), path),
                   {"revolute", "continuous", "prismatic", "fixed",
                    "floating", "planar"},
                   path);
    require_one_of(
        require_text(require_member(row, "coordinate_kind", path), path),
        {"none", "angular", "linear", "planar", "floating"}, path);
    require_one_of(require_text(require_member(row, "unit", path), path),
                   {"none", "radians", "meters", "planar_meters_radians",
                    "floating_meters_radians"},
                   path);
    const auto mimic = require_optional_identity(
        require_member(row, "mimic_source_compiled_joint_name", path), path);
    if (mimic.has_value()) {
      const auto found = reverse_symbols.find(*mimic);
      if (found == reverse_symbols.end() ||
          std::get<0>(found->second) != model ||
          std::get<1>(found->second) !=
              symbol_kind_rank("joint", path)) {
        fail_retained_bundle(
            path + " has a mimic source outside its model joint namespace");
      }
      if (!coordinate_mimic_targets
               .emplace(MimicOwnerKey{model, source}, *mimic)
               .second) {
        fail_retained_bundle(path + " duplicates one mimic coordinate");
      }
    } else if (mimic_reference_targets.contains({model, source})) {
      fail_retained_bundle(
          path + " omits the source named by its joint-mimic reference");
    }
    (void)require_finite(require_member(row, "mimic_multiplier", path), path);
    (void)require_finite(require_member(row, "mimic_offset", path), path);
    coordinate_identity.emplace(model, source, compiled);
    if (!coordinates_by_compiled_name
             .emplace(compiled,
                      CoordinateInfo{model, source, source_index})
             .second) {
      fail_retained_bundle(path + " duplicates a compiled coordinate");
    }
  }
  if (coordinate_mimic_targets != mimic_reference_targets) {
    fail_retained_bundle(
        "coordinate mimic rows differ from the exact URDF mimic references");
  }
  for (const auto& [owner, compiled_target] : mimic_reference_targets) {
    const auto target = coordinates_by_compiled_name.find(compiled_target);
    if (target == coordinates_by_compiled_name.end() ||
        target->second.model_instance_id != owner.first ||
        target->second.source_joint_name == owner.second) {
      fail_retained_bundle(
          "joint-mimic reference does not select another coordinate in "
          "the same model occurrence");
    }
  }

  struct FixedConnectionInfo final {
    std::string compiled_parent_link_name;
    std::string compiled_child_link_name;
  };
  const auto& fixed_connection_values = require_array(
      require_member(object, "fixed_connections", "symbol_role_map"),
      "symbol_role_map.fixed_connections");
  std::vector<FixedConnectionInfo> fixed_connections;
  fixed_connections.reserve(fixed_connection_values.size());
  std::set<std::string, std::less<>> fixed_joint_names;
  std::set<std::string, std::less<>> fixed_child_links;
  const auto require_occurrence_owner =
      [&](std::string_view model, std::string_view component,
          std::string_view path) {
        const auto found = occurrences.find(std::string(model));
        if (found == occurrences.end() ||
            !found->second.owner_component_id.has_value() ||
            *found->second.owner_component_id != component) {
          fail_retained_bundle(std::string(path) +
                               " disagrees with component/model ownership");
        }
      };
  const auto require_compiled_model_link =
      [&](std::string_view compiled, std::string_view model,
          std::string_view path) {
        const auto found = reverse_symbols.find(std::string(compiled));
        if (found == reverse_symbols.end() ||
            std::get<0>(found->second) != model ||
            std::get<1>(found->second) !=
                symbol_kind_rank("link", path)) {
          fail_retained_bundle(std::string(path) +
                               " does not name a link in the expected model");
        }
      };
  const auto compiled_link_owner =
      [&](std::string_view compiled, std::string_view path)
      -> std::optional<std::string> {
    const auto found = reverse_symbols.find(std::string(compiled));
    if (found == reverse_symbols.end() ||
        std::get<1>(found->second) != symbol_kind_rank("link", path)) {
      return std::nullopt;
    }
    const auto occurrence = occurrences.find(std::get<0>(found->second));
    if (occurrence == occurrences.end()) {
      return std::nullopt;
    }
    return occurrence->second.owner_component_id;
  };
  for (std::size_t index = 0U; index < fixed_connection_values.size();
       ++index) {
    const std::string path = "symbol_role_map.fixed_connections[" +
                             std::to_string(index) + "]";
    const auto& row = require_object(fixed_connection_values[index], path);
    require_fields(row,
                   {"source", "compiled_joint_name",
                    "compiled_parent_link_name", "compiled_child_link_name",
                    "child_pose_in_parent"},
                   path);
    const std::string& joint = require_text(
        require_member(row, "compiled_joint_name", path), path);
    const std::string& parent = require_text(
        require_member(row, "compiled_parent_link_name", path), path);
    const std::string& child = require_text(
        require_member(row, "compiled_child_link_name", path), path);
    require_retained_portable_identity(joint, path);
    require_retained_portable_identity(parent, path);
    require_retained_portable_identity(child, path);
    if (!fixed_joint_names.insert(joint).second ||
        !fixed_child_links.insert(child).second || parent == child) {
      fail_retained_bundle(path +
                           " duplicates a fixed joint/child or self-connects");
    }
    (void)require_placement_pose(
        require_member(row, "child_pose_in_parent", path),
        path + ".child_pose_in_parent");

    const auto& source = require_object(
        require_member(row, "source", path), path + ".source");
    const std::string& kind = require_text(
        require_member(source, "kind", path + ".source"),
        path + ".source.kind");
    if (kind == "direct_model_anchor") {
      require_fields(source, {"kind", "model_instance_id"},
                     path + ".source");
      const std::string& model = require_text(
          require_member(source, "model_instance_id", path + ".source"),
          path + ".source.model_instance_id");
      const auto occurrence = occurrences.find(model);
      if (occurrence == occurrences.end() ||
          occurrence->second.owner_component_id.has_value() ||
          parent != cell_frame) {
        fail_retained_bundle(path +
                             " is not an unowned model cell anchor");
      }
      require_compiled_model_link(child, model, path);
    } else if (kind == "direct_component_anchor") {
      require_fields(source,
                     {"kind", "controllable_component_id",
                      "root_model_instance_id"},
                     path + ".source");
      const std::string& component = require_text(
          require_member(source, "controllable_component_id",
                         path + ".source"),
          path + ".source.controllable_component_id");
      const std::string& model = require_text(
          require_member(source, "root_model_instance_id",
                         path + ".source"),
          path + ".source.root_model_instance_id");
      require_occurrence_owner(model, component, path);
      if (parent != cell_frame) {
        fail_retained_bundle(path +
                             " direct component anchor is not cell-rooted");
      }
      require_compiled_model_link(child, model, path);
    } else if (kind == "model_mount") {
      require_fields(source,
                     {"kind", "controllable_component_id",
                      "parent_model_instance_id", "child_model_instance_id"},
                     path + ".source");
      const std::string& component = require_text(
          require_member(source, "controllable_component_id",
                         path + ".source"),
          path + ".source.controllable_component_id");
      const std::string& parent_model = require_text(
          require_member(source, "parent_model_instance_id",
                         path + ".source"),
          path + ".source.parent_model_instance_id");
      const std::string& child_model = require_text(
          require_member(source, "child_model_instance_id",
                         path + ".source"),
          path + ".source.child_model_instance_id");
      require_occurrence_owner(parent_model, component, path);
      require_occurrence_owner(child_model, component, path);
      if (parent_model == child_model) {
        fail_retained_bundle(path + " mounts a model to itself");
      }
      require_compiled_model_link(parent, parent_model, path);
      require_compiled_model_link(child, child_model, path);
    } else if (kind == "component_attachment") {
      require_fields(source,
                     {"kind", "component_attachment_id",
                      "parent_component_id", "child_component_id"},
                     path + ".source");
      const std::string& attachment = require_text(
          require_member(source, "component_attachment_id",
                         path + ".source"),
          path + ".source.component_attachment_id");
      const std::string& parent_component = require_text(
          require_member(source, "parent_component_id", path + ".source"),
          path + ".source.parent_component_id");
      const std::string& child_component = require_text(
          require_member(source, "child_component_id", path + ".source"),
          path + ".source.child_component_id");
      require_retained_portable_identity(attachment, path);
      require_retained_portable_identity(parent_component, path);
      require_retained_portable_identity(child_component, path);
      const auto resolved_parent = compiled_link_owner(parent, path);
      const auto resolved_child = compiled_link_owner(child, path);
      if (parent_component == child_component ||
          resolved_parent != parent_component ||
          resolved_child != child_component) {
        fail_retained_bundle(path +
                             " disagrees with attachment endpoint ownership");
      }
    } else {
      fail_retained_bundle(path + " has an unknown connection source kind");
    }
    fixed_connections.push_back(FixedConnectionInfo{parent, child});
  }

  const auto& frame_path_values = require_array(
      require_member(object, "frame_paths", "symbol_role_map"),
      "symbol_role_map.frame_paths");
  if (frame_path_values.size() != expected_frame_paths.size()) {
    fail_retained_bundle(
        "frame paths do not exactly cover the cell and exported frames");
  }
  std::optional<FramePathKey> previous_frame_path;
  std::set<FramePathKey> found_frame_paths;
  for (std::size_t index = 0U; index < frame_path_values.size(); ++index) {
    const std::string path = "symbol_role_map.frame_paths[" +
                             std::to_string(index) + "]";
    const auto& row = require_object(frame_path_values[index], path);
    require_fields(row, {"terminal_frame", "segments"}, path);
    const auto& terminal = require_object(
        require_member(row, "terminal_frame", path),
        path + ".terminal_frame");
    const std::string& kind = require_text(
        require_member(terminal, "kind", path + ".terminal_frame"),
        path + ".terminal_frame.kind");
    FramePathKey terminal_key;
    if (kind == "cell") {
      require_fields(terminal, {"kind", "cell_id", "compiled_frame_name"},
                     path + ".terminal_frame");
      terminal_key = FramePathKey{
          0U,
          require_text(require_member(terminal, "cell_id", path), path),
          {}, {}, {},
          require_text(require_member(terminal, "compiled_frame_name", path),
                       path)};
    } else if (kind == "component_model") {
      require_fields(terminal,
                     {"kind", "controllable_component_id",
                      "model_instance_id", "model_revision", "frame_role_id",
                      "compiled_frame_name"},
                     path + ".terminal_frame");
      terminal_key = FramePathKey{
          1U,
          require_text(require_member(terminal, "controllable_component_id",
                                      path),
                       path),
          require_text(require_member(terminal, "model_instance_id", path),
                       path),
          require_text(require_member(terminal, "model_revision", path), path),
          require_text(require_member(terminal, "frame_role_id", path), path),
          require_text(require_member(terminal, "compiled_frame_name", path),
                       path)};
    } else if (kind == "passive_model") {
      require_fields(terminal,
                     {"kind", "model_instance_id", "model_revision",
                      "frame_role_id", "compiled_frame_name"},
                     path + ".terminal_frame");
      terminal_key = FramePathKey{
          2U, {},
          require_text(require_member(terminal, "model_instance_id", path),
                       path),
          require_text(require_member(terminal, "model_revision", path), path),
          require_text(require_member(terminal, "frame_role_id", path), path),
          require_text(require_member(terminal, "compiled_frame_name", path),
                       path)};
    } else {
      fail_retained_bundle(path + " has an unknown terminal-frame kind");
    }
    if (!expected_frame_paths.contains(terminal_key) ||
        !found_frame_paths.insert(terminal_key).second ||
        (previous_frame_path.has_value() &&
         *previous_frame_path >= terminal_key)) {
      fail_retained_bundle(path +
                           " is not a canonical exact exported frame path");
    }
    previous_frame_path = terminal_key;

    std::string current_link = cell_frame;
    const auto& segments = require_array(
        require_member(row, "segments", path), path + ".segments");
    for (std::size_t segment_index = 0U;
         segment_index < segments.size(); ++segment_index) {
      const std::string segment_path =
          path + ".segments[" + std::to_string(segment_index) + "]";
      const auto& segment = require_object(segments[segment_index],
                                           segment_path);
      const std::string& segment_kind = require_text(
          require_member(segment, "kind", segment_path), segment_path);
      std::string parent;
      std::string child;
      if (segment_kind == "fixed_connection") {
        require_fields(segment, {"kind", "fixed_connection_index"},
                       segment_path);
        const std::size_t connection_index = require_index(
            require_member(segment, "fixed_connection_index", segment_path),
            segment_path);
        if (connection_index >= fixed_connections.size()) {
          fail_retained_bundle(segment_path +
                               " references an absent fixed connection");
        }
        parent = fixed_connections[connection_index]
                     .compiled_parent_link_name;
        child = fixed_connections[connection_index]
                    .compiled_child_link_name;
      } else if (segment_kind == "model_joint") {
        require_fields(
            segment,
            {"kind", "model_instance_id",
             "composition_coordinate_index", "source_joint_name",
             "compiled_joint_name", "compiled_parent_link_name",
             "compiled_child_link_name"},
            segment_path);
        const std::size_t coordinate_index = require_index(
            require_member(segment, "composition_coordinate_index",
                           segment_path),
            segment_path);
        if (coordinate_index >= coordinate_values.size()) {
          fail_retained_bundle(segment_path +
                               " references an absent model coordinate");
        }
        const auto& coordinate = require_object(
            coordinate_values[coordinate_index], segment_path);
        const std::string& model = require_text(
            require_member(segment, "model_instance_id", segment_path),
            segment_path);
        const std::string& source = require_text(
            require_member(segment, "source_joint_name", segment_path),
            segment_path);
        const std::string& compiled = require_text(
            require_member(segment, "compiled_joint_name", segment_path),
            segment_path);
        if (model != require_text(
                         require_member(coordinate, "model_instance_id",
                                        segment_path),
                         segment_path) ||
            source != require_text(
                          require_member(coordinate, "source_joint_name",
                                         segment_path),
                          segment_path) ||
            compiled != require_text(
                            require_member(coordinate, "compiled_joint_name",
                                           segment_path),
                            segment_path)) {
          fail_retained_bundle(segment_path +
                               " differs from its exact coordinate row");
        }
        parent = require_text(
            require_member(segment, "compiled_parent_link_name",
                           segment_path),
            segment_path);
        child = require_text(
            require_member(segment, "compiled_child_link_name", segment_path),
            segment_path);
        require_compiled_model_link(parent, model, segment_path);
        require_compiled_model_link(child, model, segment_path);
      } else {
        fail_retained_bundle(segment_path +
                             " has an unknown path-segment kind");
      }
      if (parent != current_link || parent == child) {
        fail_retained_bundle(segment_path +
                             " does not continue the cell-rooted path");
      }
      current_link = std::move(child);
    }
    if (current_link != std::get<5>(terminal_key)) {
      fail_retained_bundle(path +
                           " does not terminate at its resolved frame");
    }
  }
  if (found_frame_paths != expected_frame_paths) {
    fail_retained_bundle(
        "frame paths do not exactly cover the compiler-exported frames");
  }

  const auto& binding_values = require_array(
      require_member(object, "control_binding_coordinate_maps",
                     "symbol_role_map"),
      "symbol_role_map.control_binding_coordinate_maps");
  std::string previous_binding;
  std::set<std::pair<std::string, std::string>> binding_target_keys;
  for (std::size_t index = 0U; index < binding_values.size(); ++index) {
    const std::string path =
        "symbol_role_map.control_binding_coordinate_maps[" +
        std::to_string(index) + "]";
    const auto& row = require_object(binding_values[index], path);
    require_fields(row, {"control_binding_id", "controllable_component_id",
                         "command_target_id", "resources"},
                   path);
    const std::string& binding = require_text(
        require_member(row, "control_binding_id", path), path);
    require_retained_portable_identity(binding, path);
    const std::string& component = require_text(
        require_member(row, "controllable_component_id", path), path);
    const std::string& target = require_text(
        require_member(row, "command_target_id", path), path);
    require_retained_portable_identity(component, path);
    require_retained_portable_identity(target, path);
    binding_target_keys.emplace(component, target);
    if (!previous_binding.empty() && previous_binding >= binding) {
      fail_retained_bundle(
          "control binding coordinate maps are not canonical and unique");
    }
    previous_binding = binding;
    const auto& resources = require_array(
        require_member(row, "resources", path), path + ".resources");
    if (resources.empty()) {
      fail_retained_bundle(path + " has no resource projection");
    }
    std::pair<std::string, std::string> previous_resource;
    bool first_resource = true;
    std::set<std::string, std::less<>> resource_ids;
    for (std::size_t resource_index = 0U;
         resource_index < resources.size(); ++resource_index) {
      const std::string resource_path =
          path + ".resources[" + std::to_string(resource_index) + "]";
      const auto& resource = require_object(resources[resource_index],
                                            resource_path);
      require_fields(resource,
                     {"resource_role_id", "resource_id", "coordinates"},
                     resource_path);
      const std::string& role = require_text(
          require_member(resource, "resource_role_id", resource_path),
          resource_path);
      const std::string& resource_id = require_text(
          require_member(resource, "resource_id", resource_path),
          resource_path);
      require_retained_portable_identity(role, resource_path);
      require_retained_portable_identity(resource_id, resource_path);
      if (!resource_ids.insert(resource_id).second) {
        fail_retained_bundle(path + " contains a duplicate ResourceId");
      }
      const std::pair<std::string, std::string> resource_key{role,
                                                             resource_id};
      if (!first_resource && previous_resource >= resource_key) {
        fail_retained_bundle(path +
                             " resource rows are not canonical and unique");
      }
      first_resource = false;
      previous_resource = resource_key;
      const auto& projections = require_array(
          require_member(resource, "coordinates", resource_path),
          resource_path + ".coordinates");
      if (projections.empty()) {
        fail_retained_bundle(resource_path +
                             " has no coordinate projection");
      }
      std::set<std::tuple<std::string, std::string, std::string>>
          selected_projections;
      for (std::size_t projection_index = 0U;
           projection_index < projections.size(); ++projection_index) {
        const std::string projection_path =
            resource_path + ".coordinates[" +
            std::to_string(projection_index) + "]";
        const auto& projection = require_object(
            projections[projection_index], projection_path);
        require_fields(
            projection,
            {"model_instance_id", "source_joint_name",
             "compiled_joint_name", "coordinate_kind", "unit",
             "multiplier", "offset", "derived_mimic"},
            projection_path);
        const std::string& model = require_text(
            require_member(projection, "model_instance_id", projection_path),
            projection_path);
        const std::string& source = require_text(
            require_member(projection, "source_joint_name", projection_path),
            projection_path);
        const std::string& compiled = require_text(
            require_member(projection, "compiled_joint_name",
                           projection_path),
            projection_path);
        const auto occurrence = occurrences.find(model);
        if (occurrence == occurrences.end() ||
            !occurrence->second.owner_component_id.has_value() ||
            *occurrence->second.owner_component_id != component) {
          fail_retained_bundle(
              projection_path +
              " selects a coordinate outside the binding component");
        }
        if (!selected_projections.emplace(model, source, compiled).second ||
            !coordinate_identity.contains({model, source, compiled}) ||
            compiled != require_symbol(symbols, model, "joint", {}, source,
                                       projection_path)) {
          fail_retained_bundle(projection_path +
                               " does not select a model coordinate");
        }
        require_one_of(
            require_text(require_member(projection, "coordinate_kind",
                                        projection_path),
                         projection_path),
            {"none", "angular", "linear", "planar", "floating"},
            projection_path);
        require_one_of(
            require_text(require_member(projection, "unit", projection_path),
                         projection_path),
            {"none", "radians", "meters", "planar_meters_radians",
             "floating_meters_radians"},
            projection_path);
        (void)require_finite(
            require_member(projection, "multiplier", projection_path),
            projection_path);
        (void)require_finite(
            require_member(projection, "offset", projection_path),
            projection_path);
        (void)require_bool(
            require_member(projection, "derived_mimic", projection_path),
            projection_path);
      }
    }
  }

  const auto& target_values = require_array(
      require_member(object, "target_role_maps", "symbol_role_map"),
      "symbol_role_map.target_role_maps");
  std::pair<std::string, std::string> previous_target;
  bool first_target = true;
  std::set<std::pair<std::string, std::string>> target_keys;
  for (std::size_t index = 0U; index < target_values.size(); ++index) {
    const std::string path = "symbol_role_map.target_role_maps[" +
                             std::to_string(index) + "]";
    const auto& row = require_object(target_values[index], path);
    require_fields(row, {"controllable_component_id", "command_target_id",
                         "model_groups", "frames", "collision_bodies"},
                   path);
    const std::string& component = require_text(
        require_member(row, "controllable_component_id", path), path);
    const std::string& target = require_text(
        require_member(row, "command_target_id", path), path);
    require_retained_portable_identity(component, path);
    require_retained_portable_identity(target, path);
    const std::pair<std::string, std::string> target_key{component, target};
    if (!first_target && previous_target >= target_key) {
      fail_retained_bundle("target role maps are not canonical and unique");
    }
    first_target = false;
    previous_target = target_key;
    target_keys.insert(target_key);

    const auto& groups = require_array(
        require_member(row, "model_groups", path), path + ".model_groups");
    std::set<std::string, std::less<>> group_role_ids;
    for (std::size_t group_index = 0U; group_index < groups.size();
         ++group_index) {
      const std::string group_path =
          path + ".model_groups[" + std::to_string(group_index) + "]";
      const auto& group = require_object(groups[group_index], group_path);
      require_fields(group,
                     {"model_group_role_id", "model_instance_id",
                      "compiled_group_name", "expanded_compiled_link_names",
                      "expanded_compiled_joint_names"},
                     group_path);
      const std::string& group_role = require_text(
          require_member(group, "model_group_role_id", group_path),
          group_path);
      require_retained_portable_identity(group_role, group_path);
      if (!group_role_ids.insert(group_role).second) {
        fail_retained_bundle(path +
                             " contains a duplicate model-group role");
      }
      const std::string& model = require_text(
          require_member(group, "model_instance_id", group_path), group_path);
      const auto occurrence = occurrences.find(model);
      if (occurrence == occurrences.end() ||
          !occurrence->second.owner_component_id.has_value() ||
          *occurrence->second.owner_component_id != component) {
        fail_retained_bundle(
            group_path + " selects a model outside the target component");
      }
      const std::string& compiled_group = require_text(
          require_member(group, "compiled_group_name", group_path),
          group_path);
      const bool group_exists = std::any_of(
          symbols.begin(), symbols.end(), [&](const auto& symbol) {
            return std::get<0>(symbol.first) == model &&
                   std::get<1>(symbol.first) == 7U &&
                   symbol.second == compiled_group;
          });
      if (!group_exists) {
        fail_retained_bundle(group_path +
                             " does not name a compiled SRDF group");
      }
      std::set<std::string, std::less<>> expanded_links;
      for (const auto& link : require_text_array(
               require_member(group, "expanded_compiled_link_names",
                              group_path),
               group_path)) {
        const auto symbol = reverse_symbols.find(link);
        if (!expanded_links.insert(link).second ||
            symbol == reverse_symbols.end() ||
            std::get<0>(symbol->second) != model ||
            std::get<1>(symbol->second) !=
                symbol_kind_rank("link", group_path)) {
          fail_retained_bundle(group_path +
                               " contains a duplicate or foreign expanded "
                               "link");
        }
      }
      std::set<std::string, std::less<>> expanded_joints;
      for (const auto& joint : require_text_array(
               require_member(group, "expanded_compiled_joint_names",
                              group_path),
               group_path)) {
        const auto symbol = reverse_symbols.find(joint);
        if (!expanded_joints.insert(joint).second ||
            symbol == reverse_symbols.end() ||
            std::get<0>(symbol->second) != model ||
            std::get<1>(symbol->second) !=
                symbol_kind_rank("joint", group_path)) {
          fail_retained_bundle(group_path +
                               " contains a duplicate or foreign expanded "
                               "joint");
        }
      }
    }

    const auto& frames = require_array(require_member(row, "frames", path),
                                       path + ".frames");
    std::set<std::string, std::less<>> frame_role_ids;
    for (std::size_t frame_index = 0U; frame_index < frames.size();
         ++frame_index) {
      const std::string frame_path =
          path + ".frames[" + std::to_string(frame_index) + "]";
      const auto& frame = require_object(frames[frame_index], frame_path);
      require_fields(frame, {"frame_role_id", "frame"}, frame_path);
      const std::string& frame_role = require_text(
          require_member(frame, "frame_role_id", frame_path), frame_path);
      require_retained_portable_identity(frame_role, frame_path);
      if (!frame_role_ids.insert(frame_role).second) {
        fail_retained_bundle(path + " contains a duplicate frame role");
      }
      validate_resolved_frame(require_member(frame, "frame", frame_path),
                              symbols, occurrences, cell_id, cell_frame,
                              component, frame_path + ".frame");
    }

    const auto& collisions = require_array(
        require_member(row, "collision_bodies", path),
        path + ".collision_bodies");
    std::set<std::string, std::less<>> collision_role_ids;
    for (std::size_t collision_index = 0U;
         collision_index < collisions.size(); ++collision_index) {
      const std::string collision_path =
          path + ".collision_bodies[" +
          std::to_string(collision_index) + "]";
      const auto& collision = require_object(collisions[collision_index],
                                             collision_path);
      require_fields(collision,
                     {"collision_body_role_id", "model_instance_id",
                      "compiled_link_names"},
                     collision_path);
      const std::string& collision_role = require_text(
          require_member(collision, "collision_body_role_id",
                         collision_path),
          collision_path);
      require_retained_portable_identity(collision_role, collision_path);
      if (!collision_role_ids.insert(collision_role).second) {
        fail_retained_bundle(path +
                             " contains a duplicate collision-body role");
      }
      const std::string& model = require_text(
          require_member(collision, "model_instance_id", collision_path),
          collision_path);
      const auto occurrence = occurrences.find(model);
      if (occurrence == occurrences.end() ||
          !occurrence->second.owner_component_id.has_value() ||
          *occurrence->second.owner_component_id != component) {
        fail_retained_bundle(collision_path +
                             " selects a model outside the target component");
      }
      std::set<std::string, std::less<>> collision_links;
      for (const auto& link : require_text_array(
               require_member(collision, "compiled_link_names",
                              collision_path),
               collision_path, true)) {
        const auto symbol = reverse_symbols.find(link);
        if (!collision_links.insert(link).second ||
            symbol == reverse_symbols.end() ||
            std::get<0>(symbol->second) != model ||
            std::get<1>(symbol->second) !=
                symbol_kind_rank("link", collision_path)) {
          fail_retained_bundle(collision_path +
                               " contains a duplicate or foreign compiled "
                               "link");
        }
      }
    }
  }
  for (const auto& binding_target : binding_target_keys) {
    if (!target_keys.contains(binding_target)) {
      fail_retained_bundle(
          "control binding coordinate map selects an unknown component "
          "command target");
    }
  }

  return root;
}

}  // namespace manyforge::deployment::detail
