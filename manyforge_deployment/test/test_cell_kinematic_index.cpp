#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/kinematic_state_view.hpp>
#include <manyforge_core/types/program_binding.hpp>
#include <manyforge_core/types/resolved_frame.hpp>
#include <manyforge_deployment/cell_kinematic_index.hpp>

// M7 part A: U1 (the index over the maintained cell shapes and synthetic
// graphs) and U2 (the equivalence oracle against verbatim copies of the
// merge-6b helpers the index replaces). The oracle is test-local and goes at
// the next milestone.
namespace {
namespace c = manyforge::core;
namespace d = manyforge::deployment;

// -- Merge-6b reference code (verbatim; only the noted parameter changes) ----
namespace m6b {

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error(message);
}

// program_binding.cpp:2384 (deleted in M7 A).
bool is_self_or_mounted_descendant(
    const c::CellGraphDTO& graph,
    const c::ControllableComponentId& component,
    const c::ControllableComponentId& owner) {
  // A child has at most one incoming attachment and the graph is acyclic, so
  // walking up from the owner reaches every ancestor once.
  for (auto current = owner;;) {
    if (current == component) return true;
    const auto parent = std::find_if(
        graph.component_attachments.begin(), graph.component_attachments.end(),
        [&](const auto& edge) {
          return edge.child.controllable_component_id == current;
        });
    if (parent == graph.component_attachments.end()) return false;
    current = parent->parent.controllable_component_id;
  }
}

// retained_tool_context.cpp:74-78 (R17's carrier set).
std::set<c::ControllableComponentId> carriers(
    const c::CellGraphDTO& graph, const c::ControllableComponentId& robot) {
  std::set<c::ControllableComponentId> result;
  for (const auto& component : graph.controllable_components) {
    if (is_self_or_mounted_descendant(graph, robot,
                                      component.controllable_component_id))
      result.insert(component.controllable_component_id);
  }
  return result;
}

// program_binding_compiler.cpp:1905-1911 (D7) and program_binding.cpp:2422
// (D1): the first edge on the parent's interface. D7 and D1 spell it with
// "flange"; the interface is a parameter here.
const c::ControllableComponentId* first_edge_child(
    const c::CellGraphDTO& graph, const c::ControllableComponentId& robot,
    std::string_view interface) {
  const auto edge = std::find_if(
      graph.component_attachments.begin(), graph.component_attachments.end(),
      [&](const auto& row) {
        return row.parent.controllable_component_id == robot &&
               row.parent.attachment_interface_id.value() == interface;
      });
  if (edge == graph.component_attachments.end()) return nullptr;
  return &edge->child.controllable_component_id;
}

// program_binding_compiler.cpp:2789-2797 (D10's owner_of lambda), over the
// bundle's graph.
std::optional<std::string> d10_owner_of(const c::CellGraphDTO& graph,
                                        const c::ModelInstanceId& model) {
  const auto owner_of = [&](const c::ModelInstanceId& instance) {
    for (const auto& component : graph.controllable_components) {
      for (const auto& role : component.model_role_bindings) {
        if (role.model_instance_id == instance)
          return component.controllable_component_id.value();
      }
    }
    fail("direct-control frame has no owning component");
  };
  try {
    return owner_of(model);
  } catch (const std::runtime_error&) {
    return std::nullopt;
  }
}

// apply_component_delta.cpp:127-153 (D17 FindComponent and the model part of
// Relations); the bundle parameter is its cell graph.
const c::ControllableComponentDTO* FindComponent(
    const c::CellGraphDTO& graph, const c::ControllableComponentId& id) {
  const auto& components = graph.controllable_components;
  const auto found = std::find_if(
      components.begin(), components.end(),
      [&](const auto& component) { return component.controllable_component_id == id; });
  return found == components.end() ? nullptr : &*found;
}

std::set<c::ModelInstanceId> relations_model_instances(
    const c::CellGraphDTO& graph, const c::ControllableComponentDTO& component) {
  std::set<c::ModelInstanceId> model_instances;
  for (const auto& role : component.model_role_bindings) {
    model_instances.insert(role.model_instance_id);
  }
  for (const auto& attachment : graph.component_attachments) {
    if (attachment.parent.controllable_component_id != component.controllable_component_id)
      continue;
    if (const auto* child = FindComponent(graph, attachment.child.controllable_component_id)) {
      for (const auto& role : child->model_role_bindings)
        model_instances.insert(role.model_instance_id);
    }
  }
  return model_instances;
}

// merge-6b apply_component_delta.cpp (RuntimeFamilies' mount rows), at the
// id level: the children of the component's mounts, in edge order.
std::vector<c::ControllableComponentId> runtime_family_mount_children(
    const c::CellGraphDTO& graph, const c::ControllableComponentId& component) {
  std::vector<c::ControllableComponentId> children;
  for (const auto& attachment : graph.component_attachments) {
    if (attachment.parent.controllable_component_id == component)
      children.push_back(attachment.child.controllable_component_id);
  }
  return children;
}

// verified_retained_bundle_store.cpp:2386-2399 (the map's path lookup).
const c::KinematicFramePathDTO* find_kinematic_frame_path(
    const std::vector<c::KinematicFramePathDTO>& kinematic_frame_paths_,
    const c::ResolvedFrameDTO& terminal_frame) noexcept {
  const c::KinematicFramePathDTO* match = nullptr;
  for (const auto& candidate : kinematic_frame_paths_) {
    if (candidate.terminal_frame != terminal_frame) {
      continue;
    }
    if (match != nullptr) {
      return nullptr;
    }
    match = &candidate;
  }
  return match;
}

}  // namespace m6b

// -- The migrated call shapes (what D and R write against the index) --------

std::set<c::ModelInstanceId> new_relations_model_instances(
    const c::CellGraphDTO& graph, const d::CellKinematicIndex& index,
    const c::ControllableComponentDTO& component) {
  std::set<c::ModelInstanceId> model_instances;
  for (const auto& role : component.model_role_bindings)
    model_instances.insert(role.model_instance_id);
  for (const auto& child_id : index.children_of(component.controllable_component_id)) {
    if (const auto* child = m6b::FindComponent(graph, child_id)) {
      for (const auto& role : child->model_role_bindings)
        model_instances.insert(role.model_instance_id);
    }
  }
  return model_instances;
}

// -- Graph builders (the compiled shapes: `<component>.model`, attachment id
// = child id, components and attachments sorted by id) ---------------------

c::ControllableComponentId Id(std::string_view id) {
  return c::ControllableComponentId(std::string(id));
}
c::ModelInstanceId Model(std::string_view id) {
  return c::ModelInstanceId(std::string(id));
}

struct ComponentSpec {
  std::string id;
  std::vector<std::string> models;
};

struct EdgeSpec {
  std::string parent;
  std::string parent_interface;
  std::string child;
};

c::ControllableComponentDTO Component(const ComponentSpec& spec) {
  std::vector<c::ModelRoleBindingDTO> bindings;
  for (const auto& model : spec.models) {
    bindings.push_back(c::ModelRoleBindingDTO{
        c::ModelRoleId(bindings.empty() ? "arm_model" : "tool_model"),
        Model(model)});
  }
  return c::ControllableComponentDTO{Id(spec.id), c::ComponentTypeId("type"),
                                     c::ComponentTypeRevision("1"),
                                     std::move(bindings), {}, std::nullopt};
}

c::ComponentAttachmentEdgeDTO Edge(const EdgeSpec& spec) {
  return c::ComponentAttachmentEdgeDTO{
      c::ComponentAttachmentId(spec.child), c::ComponentAttachmentRevision("1"),
      c::ComponentAttachmentEndpointDTO{
          Id(spec.parent), c::AttachmentInterfaceId(spec.parent_interface)},
      c::ComponentAttachmentEndpointDTO{Id(spec.child),
                                        c::AttachmentInterfaceId("mount")},
      c::PlacementPoseDTO{}};
}

c::CellGraphDTO Graph(const std::vector<ComponentSpec>& components,
                      const std::vector<EdgeSpec>& edges,
                      const std::vector<std::string>& passive_models = {}) {
  std::vector<c::ModelInstanceDTO> instances;
  std::vector<c::ControllableComponentDTO> rows;
  for (const auto& spec : components) {
    rows.push_back(Component(spec));
    for (const auto& model : spec.models) {
      instances.push_back(c::ModelInstanceDTO{Model(model), c::ModelAssetId(model),
                                              c::ModelRevision("1"), std::nullopt, {}});
    }
  }
  for (const auto& model : passive_models) {
    instances.push_back(c::ModelInstanceDTO{Model(model), c::ModelAssetId(model),
                                            c::ModelRevision("1"), std::nullopt, {}});
  }
  std::vector<c::ComponentAttachmentEdgeDTO> attachments;
  for (const auto& spec : edges) attachments.push_back(Edge(spec));
  return c::CellGraphDTO{std::string(c::kCellGraphApiVersion),
                         c::GraphRevision("graph"),
                         c::CellId("cell"),
                         {},
                         std::move(instances),
                         std::move(rows),
                         {},
                         {},
                         std::move(attachments)};
}

// The compiler's owner-stamped paths: the cell frame, one frame per owned
// model (stamped with its owner) and one per passive model.
std::vector<c::KinematicFramePathDTO> OwnerStampedPaths(
    const c::CellGraphDTO& graph) {
  std::vector<c::KinematicFramePathDTO> paths;
  paths.push_back({c::ResolvedCellFrameDTO{graph.cell_id, "world"}, {}});
  std::set<c::ModelInstanceId> owned;
  for (const auto& component : graph.controllable_components) {
    for (const auto& role : component.model_role_bindings) {
      owned.insert(role.model_instance_id);
      paths.push_back(
          {c::ResolvedComponentModelFrameDTO{
               component.controllable_component_id, role.model_instance_id,
               c::FrameRoleId("flange"), role.model_instance_id.value() + "/flange",
               c::ModelRevision("1")},
           {}});
    }
  }
  for (const auto& instance : graph.model_instances) {
    if (owned.contains(instance.model_instance_id)) continue;
    paths.push_back({c::ResolvedPassiveModelFrameDTO{
                         instance.model_instance_id, c::FrameRoleId("origin"),
                         instance.model_instance_id.value() + "/origin",
                         instance.model_revision},
                     {}});
  }
  return paths;
}

// -- The six maintained cell shapes (examples/*.scenario/deployment.yaml) ---

c::CellGraphDTO Ur5() {
  return Graph({{"ur5_robot", {"ur5_robot.model"}}}, {});
}
c::CellGraphDTO Lite6Gripper() {
  return Graph({{"lite6_robot", {"lite6_robot.model"}},
                {"lite_gripper", {"lite_gripper.model"}}},
               {{"lite6_robot", "flange", "lite_gripper"}});
}
c::CellGraphDTO Lite6Pneumatic() {
  return Graph({{"gripper_io", {}},
                {"lite6_robot", {"lite6_robot.model"}},
                {"lite_gripper", {"lite_gripper.model"}}},
               {{"lite6_robot", "flange", "lite_gripper"}});
}
c::CellGraphDTO Ur10eRobotiq() {
  return Graph({{"robotiq_2f_140", {"robotiq_2f_140.model"}},
                {"ur10e_robot", {"ur10e_robot.model"}}},
               {{"ur10e_robot", "flange", "robotiq_2f_140"}});
}
c::CellGraphDTO Uf850XarmGripper() {
  return Graph({{"uf850_robot", {"uf850_robot.model"}},
                {"xarm_gripper", {"xarm_gripper.model"}}},
               {{"uf850_robot", "flange", "xarm_gripper"}});
}
c::CellGraphDTO Wall() {
  return Graph({{"lite6_robot", {"lite6_robot.model"}},
                {"lite_gripper", {"lite_gripper.model"}},
                {"robotiq_2f_85", {"robotiq_2f_85.model"}},
                {"ur5e_robot", {"ur5e_robot.model"}}},
               {{"lite6_robot", "flange", "lite_gripper"},
                {"ur5e_robot", "flange", "robotiq_2f_85"}});
}

// -- Synthetic graphs --------------------------------------------------------

// Two tools on one flange, edge order differs from child-id order.
c::CellGraphDTO SiblingsOutOfIdOrder() {
  return Graph({{"arm", {"arm.model"}},
                {"camera", {"camera.model"}},
                {"nozzle", {"nozzle.model"}}},
               {{"arm", "flange", "nozzle"}, {"arm", "flange", "camera"}});
}
// base -> arm (rail) -> tool (flange): a chained mount.
c::CellGraphDTO Chained() {
  return Graph({{"arm", {"arm.model"}},
                {"base", {"base.model"}},
                {"tool", {"tool.model"}}},
               {{"arm", "flange", "tool"}, {"base", "rail", "arm"}});
}
// A passive model, a model-free component, a two-model component,
// prefix-related ids (`arm`, `arm.tool`, `armature`) and an edge whose child
// row is missing from the components.
c::CellGraphDTO Mixed() {
  return Graph({{"arm", {"arm.model", "arm.camera_model"}},
                {"arm.tool", {"arm.tool.model"}},
                {"armature", {"armature.model"}},
                {"io", {}}},
               {{"arm", "flange", "arm.tool"},
                {"armature", "flange", "ghost"},
                {"io", "port", "armature"}},
               {"fixture.model"});
}

struct NamedGraph {
  std::string name;
  c::CellGraphDTO graph;
};

// Partial Apply: each side gets an index over its own graph. A mount is
// added, removed and moved, each in both directions; `other` is unaffected.
std::vector<NamedGraph> ApplySides() {
  const std::vector<ComponentSpec> components{
      {"arm_a", {"arm_a.model"}},
      {"arm_b", {"arm_b.model"}},
      {"other", {"other.model"}},
      {"tool", {"tool.model"}}};
  const EdgeSpec on_a{"arm_a", "flange", "tool"};
  const EdgeSpec on_b{"arm_b", "flange", "tool"};
  return {{"unmounted", Graph(components, {})},
          {"on_a", Graph(components, {on_a})},
          {"on_b", Graph(components, {on_b})}};
}

std::vector<NamedGraph> AllGraphs() {
  std::vector<NamedGraph> graphs{{"ur5", Ur5()},
                                 {"lite6_gripper", Lite6Gripper()},
                                 {"lite6_pneumatic", Lite6Pneumatic()},
                                 {"ur10e_robotiq", Ur10eRobotiq()},
                                 {"uf850_xarm_gripper", Uf850XarmGripper()},
                                 {"wall", Wall()},
                                 {"siblings", SiblingsOutOfIdOrder()},
                                 {"chained", Chained()},
                                 {"mixed", Mixed()}};
  for (auto& side : ApplySides()) graphs.push_back(std::move(side));
  return graphs;
}

std::vector<c::ControllableComponentId> ProbeComponents(
    const c::CellGraphDTO& graph) {
  std::set<c::ControllableComponentId> ids{Id("missing"), Id("arm"),
                                           Id("arm.tool"), Id("ar")};
  for (const auto& component : graph.controllable_components)
    ids.insert(component.controllable_component_id);
  for (const auto& edge : graph.component_attachments) {
    ids.insert(edge.parent.controllable_component_id);
    ids.insert(edge.child.controllable_component_id);
  }
  return {ids.begin(), ids.end()};
}

std::vector<c::ModelInstanceId> ProbeModels(const c::CellGraphDTO& graph) {
  std::set<c::ModelInstanceId> ids{Model("missing.model"), Model("arm"),
                                   Model("arm.model.x")};
  for (const auto& instance : graph.model_instances)
    ids.insert(instance.model_instance_id);
  for (const auto& component : graph.controllable_components)
    for (const auto& role : component.model_role_bindings)
      ids.insert(role.model_instance_id);
  return {ids.begin(), ids.end()};
}

const std::vector<std::string_view> kInterfaces{"flange", "rail", "port",
                                                "mount", "missing"};

// U2: every query equals the merge-6b code over the graph and its paths.
void ExpectEquivalentToM6b(const c::CellGraphDTO& graph,
                           const std::vector<c::KinematicFramePathDTO>& paths) {
  const d::CellKinematicIndex index(graph, paths);
  const auto components = ProbeComponents(graph);

  for (const auto& carrier : components) {
    for (const auto& member : components) {
      EXPECT_EQ(index.carries(carrier, member),
                m6b::is_self_or_mounted_descendant(graph, carrier, member))
          << carrier << " carries " << member;
    }
    EXPECT_EQ(index.self_and_mounted(carrier), m6b::carriers(graph, carrier))
        << carrier;
    EXPECT_EQ(index.children_of(carrier),
              m6b::runtime_family_mount_children(graph, carrier))
        << carrier;
    for (const auto interface : kInterfaces) {
      const auto* expected = m6b::first_edge_child(graph, carrier, interface);
      const auto* actual = index.tool_on(carrier, interface);
      ASSERT_EQ(actual == nullptr, expected == nullptr) << carrier << " " << interface;
      if (actual != nullptr) { EXPECT_EQ(*actual, *expected); }
    }
    const auto edge = std::find_if(
        graph.component_attachments.begin(), graph.component_attachments.end(),
        [&](const auto& row) { return row.child.controllable_component_id == carrier; });
    const auto* mount = index.mounted_on(carrier);
    ASSERT_EQ(mount == nullptr, edge == graph.component_attachments.end()) << carrier;
    if (mount != nullptr) { EXPECT_EQ(*mount, edge->parent); }
  }

  for (const auto& component : graph.controllable_components) {
    EXPECT_EQ(new_relations_model_instances(graph, index, component),
              m6b::relations_model_instances(graph, component))
        << component.controllable_component_id;
  }

  for (const auto& model : ProbeModels(graph)) {
    const auto expected = m6b::d10_owner_of(graph, model);
    const auto* actual = index.owner_of(model);
    ASSERT_EQ(actual == nullptr, !expected.has_value()) << model;
    if (actual != nullptr) { EXPECT_EQ(actual->value(), *expected); }

    // R16 is proved by ProgramBindingFrameOwnersFromTheIndexEqualTheM6bPathScan
    // (roboplan), against the shipped owner-table code.
  }

  std::vector<c::ResolvedFrameDTO> terminals{
      c::ResolvedCellFrameDTO{c::CellId("other_cell"), "world"}};
  for (const auto& path : paths) terminals.push_back(path.terminal_frame);
  for (const auto& terminal : terminals) {
    const auto* expected = m6b::find_kinematic_frame_path(index.frame_paths(), terminal);
    EXPECT_EQ(index.find_frame_path(terminal), expected);
  }
}

// The R16 invariant on the given paths: every model frame path carries the
// graph owner of its model.
void ExpectOwnerStamped(const c::CellGraphDTO& graph,
                        const std::vector<c::KinematicFramePathDTO>& paths) {
  const d::CellKinematicIndex index(graph, paths);
  for (const auto& path : paths) {
    if (const auto* model =
            std::get_if<c::ResolvedComponentModelFrameDTO>(&path.terminal_frame)) {
      const auto* owner = index.owner_of(model->model_instance_id);
      ASSERT_NE(owner, nullptr) << model->model_instance_id;
      EXPECT_EQ(*owner, model->controllable_component_id);
    }
    if (const auto* passive =
            std::get_if<c::ResolvedPassiveModelFrameDTO>(&path.terminal_frame)) {
      EXPECT_EQ(index.owner_of(passive->model_instance_id), nullptr);
    }
  }
}

// -- U2 ----------------------------------------------------------------------

TEST(CellKinematicIndexOracle, EveryQueryEqualsMergeSixBOnEveryGraph) {
  for (const auto& [name, graph] : AllGraphs()) {
    SCOPED_TRACE(name);
    const auto paths = OwnerStampedPaths(graph);
    ExpectOwnerStamped(graph, paths);
    ExpectEquivalentToM6b(graph, paths);
    ExpectEquivalentToM6b(graph, {});
  }
}

TEST(CellKinematicIndexOracle, EachApplySideKeepsItsOwnMounts) {
  const auto sides = ApplySides();
  const auto relations = [](const c::CellGraphDTO& graph, std::string_view id) {
    const d::CellKinematicIndex index(graph);
    return new_relations_model_instances(graph, index,
                                         *m6b::FindComponent(graph, Id(id)));
  };
  const std::set<c::ModelInstanceId> a_only{Model("arm_a.model")};
  const std::set<c::ModelInstanceId> a_with_tool{Model("arm_a.model"),
                                                 Model("tool.model")};
  const std::set<c::ModelInstanceId> b_only{Model("arm_b.model")};
  const std::set<c::ModelInstanceId> b_with_tool{Model("arm_b.model"),
                                                 Model("tool.model")};
  const auto& unmounted = sides[0].graph;
  const auto& on_a = sides[1].graph;
  const auto& on_b = sides[2].graph;
  // Added (unmounted -> on_a) and removed (on_a -> unmounted).
  EXPECT_EQ(relations(unmounted, "arm_a"), a_only);
  EXPECT_EQ(relations(on_a, "arm_a"), a_with_tool);
  // Moved (on_a -> on_b and back).
  EXPECT_EQ(relations(on_a, "arm_b"), b_only);
  EXPECT_EQ(relations(on_b, "arm_a"), a_only);
  EXPECT_EQ(relations(on_b, "arm_b"), b_with_tool);
  // The tool's own relations and the unaffected component never change.
  for (const auto* graph : {&unmounted, &on_a, &on_b}) {
    EXPECT_EQ(relations(*graph, "tool"),
              std::set<c::ModelInstanceId>{Model("tool.model")});
    EXPECT_EQ(relations(*graph, "other"),
              std::set<c::ModelInstanceId>{Model("other.model")});
  }
}

// -- U1 ----------------------------------------------------------------------

TEST(CellKinematicIndex, Ur5HasNoMounts) {
  const d::CellKinematicIndex index(Ur5());
  ASSERT_NE(index.owner_of(Model("ur5_robot.model")), nullptr);
  EXPECT_EQ(*index.owner_of(Model("ur5_robot.model")), Id("ur5_robot"));
  EXPECT_EQ(index.mounted_on(Id("ur5_robot")), nullptr);
  EXPECT_TRUE(index.carries(Id("ur5_robot"), Id("ur5_robot")));
  EXPECT_EQ(index.self_and_mounted(Id("ur5_robot")),
            std::set<c::ControllableComponentId>{Id("ur5_robot")});
  EXPECT_TRUE(index.children_of(Id("ur5_robot")).empty());
  EXPECT_EQ(index.tool_on(Id("ur5_robot"), "flange"), nullptr);
  EXPECT_TRUE(index.frame_paths().empty());
}

TEST(CellKinematicIndex, MountedToolCells) {
  struct Cell {
    c::CellGraphDTO graph;
    std::string robot;
    std::string tool;
  };
  const std::vector<Cell> cells{
      {Lite6Gripper(), "lite6_robot", "lite_gripper"},
      {Lite6Pneumatic(), "lite6_robot", "lite_gripper"},
      {Ur10eRobotiq(), "ur10e_robot", "robotiq_2f_140"},
      {Uf850XarmGripper(), "uf850_robot", "xarm_gripper"}};
  for (const auto& cell : cells) {
    SCOPED_TRACE(cell.robot);
    const d::CellKinematicIndex index(cell.graph);
    const auto robot = Id(cell.robot);
    const auto tool = Id(cell.tool);
    ASSERT_NE(index.owner_of(Model(cell.tool + ".model")), nullptr);
    EXPECT_EQ(*index.owner_of(Model(cell.tool + ".model")), tool);
    EXPECT_EQ(*index.owner_of(Model(cell.robot + ".model")), robot);
    ASSERT_NE(index.mounted_on(tool), nullptr);
    EXPECT_EQ(*index.mounted_on(tool),
              (c::ComponentAttachmentEndpointDTO{robot, c::AttachmentInterfaceId("flange")}));
    EXPECT_EQ(index.mounted_on(robot), nullptr);
    EXPECT_TRUE(index.carries(robot, tool));
    EXPECT_FALSE(index.carries(tool, robot));
    EXPECT_EQ(index.self_and_mounted(robot),
              (std::set<c::ControllableComponentId>{robot, tool}));
    EXPECT_EQ(index.self_and_mounted(tool), std::set<c::ControllableComponentId>{tool});
    EXPECT_EQ(index.children_of(robot), std::vector<c::ControllableComponentId>{tool});
    ASSERT_NE(index.tool_on(robot, "flange"), nullptr);
    EXPECT_EQ(*index.tool_on(robot, "flange"), tool);
    EXPECT_EQ(index.tool_on(robot, "mount"), nullptr);
    EXPECT_EQ(index.tool_on(tool, "flange"), nullptr);
  }
  // The pneumatic cell's I/O device owns no model and carries nothing.
  const d::CellKinematicIndex pneumatic(Lite6Pneumatic());
  EXPECT_EQ(pneumatic.self_and_mounted(Id("gripper_io")),
            std::set<c::ControllableComponentId>{Id("gripper_io")});
  EXPECT_FALSE(pneumatic.carries(Id("gripper_io"), Id("lite_gripper")));
  EXPECT_FALSE(pneumatic.carries(Id("lite6_robot"), Id("gripper_io")));
}

TEST(CellKinematicIndex, WallCellNeverCarriesAcrossRobots) {
  const d::CellKinematicIndex index(Wall());
  EXPECT_TRUE(index.carries(Id("ur5e_robot"), Id("robotiq_2f_85")));
  EXPECT_TRUE(index.carries(Id("lite6_robot"), Id("lite_gripper")));
  EXPECT_FALSE(index.carries(Id("ur5e_robot"), Id("lite_gripper")));
  EXPECT_FALSE(index.carries(Id("lite6_robot"), Id("robotiq_2f_85")));
  EXPECT_FALSE(index.carries(Id("ur5e_robot"), Id("lite6_robot")));
  EXPECT_EQ(index.self_and_mounted(Id("ur5e_robot")),
            (std::set<c::ControllableComponentId>{Id("robotiq_2f_85"), Id("ur5e_robot")}));
  EXPECT_EQ(*index.tool_on(Id("lite6_robot"), "flange"), Id("lite_gripper"));
  EXPECT_EQ(*index.tool_on(Id("ur5e_robot"), "flange"), Id("robotiq_2f_85"));
}

TEST(CellKinematicIndex, SiblingsKeepGraphEdgeOrder) {
  const d::CellKinematicIndex index(SiblingsOutOfIdOrder());
  ASSERT_NE(index.tool_on(Id("arm"), "flange"), nullptr);
  EXPECT_EQ(*index.tool_on(Id("arm"), "flange"), Id("nozzle"));
  EXPECT_EQ(index.children_of(Id("arm")),
            (std::vector<c::ControllableComponentId>{Id("nozzle"), Id("camera")}));
  EXPECT_EQ(index.self_and_mounted(Id("arm")),
            (std::set<c::ControllableComponentId>{Id("arm"), Id("camera"), Id("nozzle")}));
  EXPECT_FALSE(index.carries(Id("nozzle"), Id("camera")));
}

TEST(CellKinematicIndex, ChainedMountIsTransitiveOnlyWhereItWasBefore) {
  const d::CellKinematicIndex index(Chained());
  // carries and self_and_mounted are transitive.
  EXPECT_TRUE(index.carries(Id("base"), Id("tool")));
  EXPECT_EQ(index.self_and_mounted(Id("base")),
            (std::set<c::ControllableComponentId>{Id("arm"), Id("base"), Id("tool")}));
  // children_of and tool_on are direct.
  EXPECT_EQ(index.children_of(Id("base")), std::vector<c::ControllableComponentId>{Id("arm")});
  EXPECT_EQ(index.tool_on(Id("base"), "flange"), nullptr);
  ASSERT_NE(index.tool_on(Id("base"), "rail"), nullptr);
  EXPECT_EQ(*index.tool_on(Id("base"), "rail"), Id("arm"));
  EXPECT_EQ(*index.mounted_on(Id("arm")),
            (c::ComponentAttachmentEndpointDTO{Id("base"), c::AttachmentInterfaceId("rail")}));
}

TEST(CellKinematicIndex, PassiveModelsModelFreeComponentsAndMissingRows) {
  const d::CellKinematicIndex index(Mixed());
  EXPECT_EQ(index.owner_of(Model("fixture.model")), nullptr);
  EXPECT_EQ(index.owner_of(Model("missing.model")), nullptr);
  EXPECT_EQ(*index.owner_of(Model("arm.camera_model")), Id("arm"));
  EXPECT_EQ(*index.owner_of(Model("arm.tool.model")), Id("arm.tool"));
  // A prefix is not an owner, and `armature` is not carried by `arm`.
  EXPECT_FALSE(index.carries(Id("arm"), Id("armature")));
  EXPECT_FALSE(index.carries(Id("arm.tool"), Id("arm")));
  // The model-free `io` carries `armature` through its port.
  EXPECT_TRUE(index.carries(Id("io"), Id("armature")));
  // An edge child without a component row is walked but never listed.
  EXPECT_TRUE(index.carries(Id("io"), Id("ghost")));
  EXPECT_EQ(index.self_and_mounted(Id("io")),
            (std::set<c::ControllableComponentId>{Id("armature"), Id("io")}));
  EXPECT_EQ(index.children_of(Id("armature")),
            std::vector<c::ControllableComponentId>{Id("ghost")});
  // A component unknown to the graph carries only itself and lists nothing.
  EXPECT_TRUE(index.carries(Id("missing"), Id("missing")));
  EXPECT_TRUE(index.self_and_mounted(Id("missing")).empty());
  EXPECT_EQ(index.mounted_on(Id("missing")), nullptr);
}

TEST(CellKinematicIndex, FindFramePathIsNoneOneOrAmbiguous) {
  const auto graph = Lite6Gripper();
  auto paths = OwnerStampedPaths(graph);
  const c::ResolvedFrameDTO tool_frame = paths.at(2).terminal_frame;
  const c::ResolvedFrameDTO absent =
      c::ResolvedCellFrameDTO{c::CellId("other_cell"), "world"};
  {
    const d::CellKinematicIndex index(graph, paths);
    EXPECT_EQ(index.frame_paths(), paths);
    EXPECT_EQ(index.find_frame_path(tool_frame), &index.frame_paths().at(2));
    EXPECT_EQ(index.find_frame_path(absent), nullptr);
  }
  paths.push_back(paths.at(2));
  const d::CellKinematicIndex ambiguous(graph, paths);
  EXPECT_EQ(ambiguous.find_frame_path(tool_frame), nullptr);
  EXPECT_NE(ambiguous.find_frame_path(paths.front().terminal_frame), nullptr);
}

// -- M7 part B (B-dep): ancestors_of, default_tcp_of, end_effector_entry and
// the primary tool (B5, B1) ---------------------------------------------------

// The compiled `default_tcp` of one component row.
c::CellGraphDTO WithDefault(c::CellGraphDTO graph, std::string_view component,
                            std::string_view target, std::string_view role) {
  for (auto& row : graph.controllable_components) {
    if (row.controllable_component_id == Id(component))
      row.default_tcp = c::ComponentDefaultTcpDTO{
          c::CommandTargetId(std::string(target)), c::FrameRoleId(std::string(role))};
  }
  return graph;
}

// The authored `primary` of the edge whose child is `child`.
c::CellGraphDTO WithPrimary(c::CellGraphDTO graph, std::string_view child) {
  for (auto& edge : graph.component_attachments) {
    if (edge.child.controllable_component_id == Id(child)) edge.primary = true;
  }
  return graph;
}

c::TcpEntryDTO Entry(std::string_view component, std::string_view target,
                     std::string_view role) {
  return c::TcpEntryDTO{Id(component), c::CommandTargetId(std::string(target)),
                        c::FrameRoleId(std::string(role))};
}

TEST(CellKinematicIndexB, AncestorsOfIsTheMountChainNearestFirst) {
  const d::CellKinematicIndex chained(Chained());
  EXPECT_EQ(chained.ancestors_of(Id("tool")),
            (std::vector<c::ControllableComponentId>{Id("arm"), Id("base")}));
  EXPECT_EQ(chained.ancestors_of(Id("arm")),
            std::vector<c::ControllableComponentId>{Id("base")});
  EXPECT_TRUE(chained.ancestors_of(Id("base")).empty());
  EXPECT_TRUE(chained.ancestors_of(Id("missing")).empty());
  const d::CellKinematicIndex lite6(Lite6Gripper());
  EXPECT_EQ(lite6.ancestors_of(Id("lite_gripper")),
            std::vector<c::ControllableComponentId>{Id("lite6_robot")});
  // The wall cell: each tool's chain is its own robot only.
  const d::CellKinematicIndex wall(Wall());
  EXPECT_EQ(wall.ancestors_of(Id("robotiq_2f_85")),
            std::vector<c::ControllableComponentId>{Id("ur5e_robot")});
}

TEST(CellKinematicIndexB, DefaultTcpOfReadsTheCompiledRow) {
  const d::CellKinematicIndex index(WithDefault(
      WithDefault(Lite6Gripper(), "lite6_robot", "arm", "flange"),
      "lite_gripper", "gripper", "cup_center"));
  ASSERT_NE(index.default_tcp_of(Id("lite_gripper")), nullptr);
  EXPECT_EQ(*index.default_tcp_of(Id("lite_gripper")),
            (c::ComponentDefaultTcpDTO{c::CommandTargetId("gripper"),
                                       c::FrameRoleId("cup_center")}));
  ASSERT_NE(index.default_tcp_of(Id("lite6_robot")), nullptr);
  EXPECT_EQ(index.default_tcp_of(Id("lite6_robot"))->frame_role_id.value(), "flange");
  EXPECT_EQ(index.default_tcp_of(Id("missing")), nullptr);
  const d::CellKinematicIndex none(Lite6Pneumatic());
  EXPECT_EQ(none.default_tcp_of(Id("gripper_io")), nullptr);
  EXPECT_EQ(none.default_tcp_of(Id("lite_gripper")), nullptr);
}

TEST(CellKinematicIndexB, ThePrimaryToolWinsOnASharedInterface) {
  // Without a primary the first edge stays (the part-A order).
  const d::CellKinematicIndex plain(SiblingsOutOfIdOrder());
  EXPECT_EQ(*plain.tool_on(Id("arm"), "flange"), Id("nozzle"));
  // A primary on the second tool in edge order selects that tool.
  const d::CellKinematicIndex index(WithPrimary(SiblingsOutOfIdOrder(), "camera"));
  ASSERT_NE(index.tool_on(Id("arm"), "flange"), nullptr);
  EXPECT_EQ(*index.tool_on(Id("arm"), "flange"), Id("camera"));
  // children_of and the carriage relations ignore the mark.
  EXPECT_EQ(index.children_of(Id("arm")),
            (std::vector<c::ControllableComponentId>{Id("nozzle"), Id("camera")}));
  EXPECT_TRUE(index.carries(Id("arm"), Id("nozzle")));
  EXPECT_EQ(index.tool_on(Id("arm"), "rail"), nullptr);
}

TEST(CellKinematicIndexB, EndEffectorEntryIsTheDefaultMark) {
  const c::CommandTargetId arm("arm");
  // 1. The tool on the flange: its default TCP, whatever its name.
  const auto lite6 = WithDefault(
      WithDefault(Lite6Gripper(), "lite6_robot", "arm", "flange"),
      "lite_gripper", "gripper", "cup_center");
  EXPECT_EQ(d::CellKinematicIndex(lite6).end_effector_entry(Id("lite6_robot"), arm),
            Entry("lite_gripper", "gripper", "cup_center"));
  // C-P2: a tool without a default TCP leaves no mark (no robot fallback).
  const auto bare_tool =
      WithDefault(Lite6Gripper(), "lite6_robot", "arm", "flange");
  const d::CellKinematicIndex bare_tool_index(bare_tool);
  ASSERT_NE(bare_tool_index.tool_on(Id("lite6_robot"), "flange"), nullptr);
  EXPECT_EQ(*bare_tool_index.tool_on(Id("lite6_robot"), "flange"),
            Id("lite_gripper"));
  EXPECT_FALSE(d::CellKinematicIndex(bare_tool)
                   .end_effector_entry(Id("lite6_robot"), arm)
                   .has_value());
  const auto primary_without_tcp = WithPrimary(
      WithDefault(SiblingsOutOfIdOrder(), "nozzle", "nozzle", "tip"),
      "camera");
  const d::CellKinematicIndex primary_without_tcp_index(primary_without_tcp);
  ASSERT_NE(primary_without_tcp_index.tool_on(Id("arm"), "flange"), nullptr);
  EXPECT_EQ(*primary_without_tcp_index.tool_on(Id("arm"), "flange"),
            Id("camera"));
  EXPECT_FALSE(primary_without_tcp_index.end_effector_entry(Id("arm"), arm)
                   .has_value());
  // 2. No tool: the robot's own default TCP.
  EXPECT_EQ(d::CellKinematicIndex(WithDefault(Ur5(), "ur5_robot", "arm", "tcp"))
                .end_effector_entry(Id("ur5_robot"), arm),
            Entry("ur5_robot", "arm", "tcp"));
  // 3. No tool and no default: the robot target's flange.
  EXPECT_EQ(d::CellKinematicIndex(Ur5()).end_effector_entry(Id("ur5_robot"), arm),
            Entry("ur5_robot", "arm", "flange"));
  // Two tools: the primary one's default; the other's is not read.
  const auto siblings = WithPrimary(
      WithDefault(WithDefault(SiblingsOutOfIdOrder(), "nozzle", "nozzle", "tip"),
                  "camera", "camera", "view"),
      "camera");
  EXPECT_EQ(d::CellKinematicIndex(siblings).end_effector_entry(Id("arm"), arm),
            Entry("camera", "camera", "view"));
  // A robot `arm` with a tool `arm.tool`: the mark is the tool's own entry,
  // by owner, never by an id prefix.
  const auto prefixed = WithDefault(Mixed(), "arm.tool", "gripper", "tcp");
  EXPECT_EQ(d::CellKinematicIndex(prefixed).end_effector_entry(Id("arm"), arm),
            Entry("arm.tool", "gripper", "tcp"));
  // Direct only: a chained tool marks its own carrier, not the base.
  const auto chained = WithDefault(Chained(), "tool", "gripper", "tcp");
  EXPECT_EQ(d::CellKinematicIndex(chained).end_effector_entry(Id("base"), arm),
            Entry("base", "arm", "flange"));
  EXPECT_EQ(d::CellKinematicIndex(chained).end_effector_entry(Id("arm"), arm),
            Entry("tool", "gripper", "tcp"));
}

}  // namespace
