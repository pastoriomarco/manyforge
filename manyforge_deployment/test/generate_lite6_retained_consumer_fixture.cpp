#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "lite6_retained_consumer_fixture.hpp"

namespace {

using manyforge::deployment::CanonicalValue;

[[nodiscard]] std::string_view symbol_kind_name(
    manyforge::deployment::OccurrenceSymbolKind kind) {
  using Kind = manyforge::deployment::OccurrenceSymbolKind;
  switch (kind) {
    case Kind::kModel:
      return "model";
    case Kind::kRobot:
      return "robot";
    case Kind::kMaterial:
      return "material";
    case Kind::kLink:
      return "link";
    case Kind::kJoint:
      return "joint";
    case Kind::kVisual:
      return "visual";
    case Kind::kCollision:
      return "collision";
    case Kind::kSrdfGroup:
      return "srdf_group";
    case Kind::kSrdfGroupState:
      return "srdf_group_state";
    case Kind::kSrdfEndEffector:
      return "srdf_end_effector";
  }
  throw std::logic_error("unknown occurrence symbol kind");
}

[[nodiscard]] std::string_view coordinate_kind_name(
    manyforge::deployment::UrdfCoordinateKind kind) {
  using Kind = manyforge::deployment::UrdfCoordinateKind;
  switch (kind) {
    case Kind::kNone:
      return "none";
    case Kind::kAngular:
      return "angular";
    case Kind::kLinear:
      return "linear";
    case Kind::kPlanar:
      return "planar";
    case Kind::kFloating:
      return "floating";
  }
  throw std::logic_error("unknown coordinate kind");
}

[[nodiscard]] std::string_view coordinate_unit_name(
    manyforge::deployment::CompositionCoordinateUnit unit) {
  using Unit = manyforge::deployment::CompositionCoordinateUnit;
  switch (unit) {
    case Unit::kNone:
      return "none";
    case Unit::kRadians:
      return "radians";
    case Unit::kMeters:
      return "meters";
    case Unit::kPlanarMetersRadians:
      return "planar_meters_radians";
    case Unit::kFloatingMetersRadians:
      return "floating_meters_radians";
  }
  throw std::logic_error("unknown coordinate unit");
}

[[nodiscard]] CanonicalValue text_array(
    const std::vector<std::string>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue resolved_frame(
    const manyforge::deployment::CompositionResolvedFrame& frame) {
  return std::visit(
      [](const auto& value) -> CanonicalValue {
        using Frame = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<
                          Frame,
                          manyforge::deployment::CompositionCellFrame>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("cell")},
              {"cell_id", CanonicalValue(value.cell_id.value())},
              {"compiled_frame_name",
               CanonicalValue(value.compiled_frame_name)}});
        } else if constexpr (std::is_same_v<
                                 Frame,
                                 manyforge::deployment::
                                     CompositionComponentModelFrame>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("component_model")},
              {"controllable_component_id",
               CanonicalValue(value.controllable_component_id.value())},
              {"model_instance_id",
               CanonicalValue(value.model_instance_id.value())},
              {"model_revision",
               CanonicalValue(value.model_revision.value())},
              {"frame_role_id", CanonicalValue(value.frame_role_id.value())},
              {"compiled_frame_name",
               CanonicalValue(value.compiled_frame_name)}});
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("passive_model")},
              {"model_instance_id",
               CanonicalValue(value.model_instance_id.value())},
              {"model_revision",
               CanonicalValue(value.model_revision.value())},
              {"frame_role_id", CanonicalValue(value.frame_role_id.value())},
              {"compiled_frame_name",
               CanonicalValue(value.compiled_frame_name)}});
        }
      },
      frame);
}

[[nodiscard]] CanonicalValue project_symbols(
    const manyforge::deployment::VerifiedRetainedModelMap& map) {
  CanonicalValue::Array rows;
  rows.reserve(map.symbols().size());
  for (const auto& symbol : map.symbols()) {
    rows.emplace_back(CanonicalValue::Object{
        {"model_instance_id",
         CanonicalValue(symbol.source.model_instance_id.value())},
        {"kind", CanonicalValue(std::string(symbol_kind_name(symbol.source.kind)))},
        {"scope_name", CanonicalValue(symbol.source.scope_name)},
        {"source_name", CanonicalValue(symbol.source.source_name)},
        {"compiled_name", CanonicalValue(symbol.compiled_name)}});
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue project_coordinates(
    const manyforge::deployment::VerifiedRetainedModelMap& map) {
  CanonicalValue::Array rows;
  rows.reserve(map.coordinates().size());
  for (const auto& coordinate : map.coordinates()) {
    rows.emplace_back(CanonicalValue::Object{
        {"model_instance_id",
         CanonicalValue(coordinate.model_instance_id.value())},
        {"source_joint_index",
         CanonicalValue(static_cast<double>(coordinate.source_joint_index))},
        {"source_joint_name", CanonicalValue(coordinate.source_joint_name)},
        {"compiled_joint_name",
         CanonicalValue(coordinate.compiled_joint_name)},
        {"mimic_source_compiled_joint_name",
         coordinate.mimic_source_compiled_joint_name.has_value()
             ? CanonicalValue(*coordinate.mimic_source_compiled_joint_name)
             : CanonicalValue(nullptr)},
        {"mimic_multiplier", CanonicalValue(coordinate.mimic_multiplier)},
        {"mimic_offset", CanonicalValue(coordinate.mimic_offset)}});
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue project_selected_binding(
    const manyforge::deployment::CompositionControlBindingCoordinateMap&
        binding) {
  CanonicalValue::Array resources;
  resources.reserve(binding.resources.size());
  for (const auto& resource : binding.resources) {
    CanonicalValue::Array coordinates;
    coordinates.reserve(resource.coordinates.size());
    for (const auto& coordinate : resource.coordinates) {
      coordinates.emplace_back(CanonicalValue::Object{
          {"model_instance_id",
           CanonicalValue(coordinate.model_instance_id.value())},
          {"source_joint_name", CanonicalValue(coordinate.source_joint_name)},
          {"compiled_joint_name",
           CanonicalValue(coordinate.compiled_joint_name)},
          {"coordinate_kind",
           CanonicalValue(std::string(
               coordinate_kind_name(coordinate.coordinate_kind)))},
          {"unit",
           CanonicalValue(std::string(coordinate_unit_name(coordinate.unit)))},
          {"multiplier", CanonicalValue(coordinate.multiplier)},
          {"offset", CanonicalValue(coordinate.offset)},
          {"derived_mimic", CanonicalValue(coordinate.derived_mimic)}});
    }
    resources.emplace_back(CanonicalValue::Object{
        {"resource_role_id", CanonicalValue(resource.resource_role_id.value())},
        {"resource_id", CanonicalValue(resource.resource_id.value())},
        {"coordinates", CanonicalValue(std::move(coordinates))}});
  }
  return CanonicalValue(CanonicalValue::Array{CanonicalValue(
      CanonicalValue::Object{
          {"control_binding_id",
           CanonicalValue(binding.control_binding_id.value())},
          {"controllable_component_id",
           CanonicalValue(binding.controllable_component_id.value())},
          {"command_target_id",
           CanonicalValue(binding.command_target_id.value())},
          {"resources", CanonicalValue(std::move(resources))}})});
}

[[nodiscard]] CanonicalValue project_selected_target(
    const manyforge::deployment::CompositionTargetResolution& target) {
  CanonicalValue::Array groups;
  groups.reserve(target.model_groups.size());
  for (const auto& group : target.model_groups) {
    groups.emplace_back(CanonicalValue::Object{
        {"model_group_role_id",
         CanonicalValue(group.model_group_role_id.value())},
        {"model_instance_id", CanonicalValue(group.model_instance_id.value())},
        {"compiled_group_name", CanonicalValue(group.compiled_group_name)},
        {"expanded_compiled_link_names",
         text_array(group.expanded_compiled_link_names)},
        {"expanded_compiled_joint_names",
         text_array(group.expanded_compiled_joint_names)}});
  }
  CanonicalValue::Array frames;
  frames.reserve(target.frames.size());
  for (const auto& frame : target.frames) {
    frames.emplace_back(CanonicalValue::Object{
        {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
        {"frame", resolved_frame(frame.frame)}});
  }
  CanonicalValue::Array collisions;
  collisions.reserve(target.collision_bodies.size());
  for (const auto& collision : target.collision_bodies) {
    collisions.emplace_back(CanonicalValue::Object{
        {"collision_body_role_id",
         CanonicalValue(collision.collision_body_role_id.value())},
        {"model_instance_id",
         CanonicalValue(collision.model_instance_id.value())},
        {"compiled_link_names", text_array(collision.compiled_link_names)}});
  }
  return CanonicalValue(CanonicalValue::Array{CanonicalValue(
      CanonicalValue::Object{
          {"controllable_component_id",
           CanonicalValue(target.controllable_component_id.value())},
          {"command_target_id",
           CanonicalValue(target.command_target_id.value())},
          {"model_groups", CanonicalValue(std::move(groups))},
          {"frames", CanonicalValue(std::move(frames))},
          {"collision_bodies", CanonicalValue(std::move(collisions))}})});
}

void write_exact_file(const std::filesystem::path& path,
                      std::string_view bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error(
        "cannot create retained consumer fixture file: " + path.string());
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream) {
    throw std::runtime_error(
        "cannot write retained consumer fixture file: " + path.string());
  }
}

[[nodiscard]] std::filesystem::path parse_output_root(
    int argc, char** argv) {
  if (argc != 3 || std::string_view(argv[1]) != "--output-root" ||
      std::string_view(argv[2]).empty()) {
    throw std::invalid_argument(
        "usage: generate_lite6_retained_consumer_fixture "
        "--output-root <path>");
  }
  return std::filesystem::path(argv[2]);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::filesystem::path output_root =
        parse_output_root(argc, argv);
    std::filesystem::create_directories(output_root);
    const std::filesystem::path package_root =
        output_root / "retained-package";
    std::filesystem::create_directories(package_root);

    const auto materialized =
        manyforge::deployment::test::
            materialize_lite6_retained_consumer_fixture(package_root);
    const auto& planning =
        materialized.bundle->value()
            .compiled_products.planning_collision_model;
    const manyforge::deployment::RetainedPlanningModelQuery query{
        materialized.bundle->retained_bundle_id(), planning.product_id};
    const manyforge::deployment::
        FilesystemVerifiedRetainedPlanningModelSource source(package_root);
    const auto model = source.get_verified(query);
    if (!model.has_value()) {
      throw std::runtime_error(
          "generated retained Lite6 planning model is absent");
    }

    const auto urdf_path = output_root / "retained-model.urdf";
    const auto srdf_path = output_root / "retained-model.srdf";
    write_exact_file(urdf_path, model->urdf_bytes());
    write_exact_file(srdf_path, model->srdf_bytes());

    const auto& symbol_products =
        materialized.bundle->value().compiled_products.symbol_role_maps;
    if (symbol_products.size() != 1U) {
      throw std::runtime_error(
          "generated controllable Lite6 bundle has no exact symbol map");
    }
    const auto& symbol_product = symbol_products.front();
    const manyforge::deployment::RetainedModelMapQuery symbol_query{
        materialized.bundle->retained_bundle_id(),
        symbol_product.product_id};
    const manyforge::deployment::
        FilesystemVerifiedRetainedModelMapSource symbol_source(
            package_root);
    const auto model_map = symbol_source.get_verified(symbol_query);
    if (!model_map.has_value()) {
      throw std::runtime_error(
          "generated controllable Lite6 symbol map point-read is absent");
    }
    model_map->require_same_bundle_as(*model);
    const manyforge::core::ControllableComponentId component_id(
        std::string(manyforge::deployment::test::
                        kLite6ControllableComponentId));
    const manyforge::core::CommandTargetId command_target_id(
        std::string(manyforge::deployment::test::kLite6ArmTargetId));
    const auto* selected_binding =
        model_map->find_binding(component_id, command_target_id);
    const auto* selected_target =
        model_map->find_target(component_id, command_target_id);
    if (selected_binding == nullptr || selected_target == nullptr) {
      throw std::runtime_error(
          "retained Lite6 map cannot resolve its logical arm target");
    }

    const CanonicalValue manifest(CanonicalValue::Object{
        {"api_version",
         CanonicalValue(
             "manyforge.i3.retained-consumer-fixture.v1")},
        {"model_instance_id",
         CanonicalValue("lite6_fixture_instance")},
        {"retained_bundle_id",
         CanonicalValue(
             materialized.bundle->retained_bundle_id().value())},
        {"product_id", CanonicalValue(planning.product_id.value())},
        {"symbol_role_map_product_id",
         CanonicalValue(symbol_product.product_id.value())},
        {"urdf_sha256",
         CanonicalValue(manyforge::deployment::sha256_digest(
             model->urdf_bytes()))},
        {"srdf_sha256",
         CanonicalValue(manyforge::deployment::sha256_digest(
             model->srdf_bytes()))},
        {"symbol_rewrites", project_symbols(*model_map)},
        {"coordinates", project_coordinates(*model_map)},
        {"control_binding_coordinate_maps",
         project_selected_binding(*selected_binding)},
        {"target_role_maps", project_selected_target(*selected_target)},
    });
    write_exact_file(
        output_root / "manifest.json",
        manyforge::deployment::canonical_json(manifest));
    std::cout << (output_root / "manifest.json").string() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
