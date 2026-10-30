#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_io_condition_compiler.hpp>
#include <manyforge_deployment/program_io_stop_response_validation.hpp>
#include <manyforge_deployment/prepared_activation_repository.hpp>
#include <unistd.h>

#include "content_identity_internal.hpp"
#include "control_domain_control_internal.hpp"
#include "maintained_interrupt_program_fixture.hpp"

namespace {
namespace d = manyforge::deployment;
namespace c = manyforge::core;
using V = d::CanonicalValue;

V replace(const V &root, std::string name, V value) {
  auto object = root.as_object();
  for (auto &[key, selected] : object)
    if (key == name) {
      selected = std::move(value);
      return V(std::move(object));
    }
  object.emplace_back(std::move(name), std::move(value));
  return V(std::move(object));
}

class ProgramIoStopResponseTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<unsigned> number{};
    root = std::filesystem::temp_directory_path() /
           ("manyforge-node-stop-" + std::to_string(::getpid()) + "." +
            std::to_string(++number));
    ASSERT_TRUE(std::filesystem::create_directory(root));
    ASSERT_TRUE(std::filesystem::create_directory(root / "programs"));
    const auto maintained = d::test::LoadMaintainedInterruptProgram(
        MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT, root / "programs");
    bundle = maintained.bundle;
    map = maintained.model_map;
    const auto &bindings = bundle->value().cell_graph.control_bindings;
    const auto arm =
        std::find_if(bindings.begin(), bindings.end(), [](const auto &row) {
          return row.controllable_component_id ==
                     c::ControllableComponentId("lite6_robot") &&
                 row.command_target_id == c::CommandTargetId("arm") &&
                 row.capability_id ==
                     c::CapabilityId("execution.trajectory.execute");
        });
    ASSERT_NE(arm, bindings.end());
    arm_binding = &*arm;
    const auto reference = d::required_io_stop_artifact_for_binding(
        *bundle, arm->control_binding_id, arm->control_binding_revision);
    ASSERT_TRUE(reference.ok()) << reference.error().message;
    d::FilesystemVerifiedRawArtifactSource source(package_root);
    const auto &ref = reference.value();
    const auto bytes = source.get_verified(ref.content_addressed_store_key,
                                           ref.content_digest, ref.byte_size);
    ASSERT_TRUE(bytes);
    artifacts.push_back(
        {ref.content_addressed_store_key, ref.content_digest, *bytes});
    ASSERT_EQ(artifacts.size(), 1U);
    const auto control = d::detail::parse_and_verify_control_domain_control(
        artifacts.front().bytes, &bundle->value().cell_graph,
        map->control_bindings());
    const auto &stop_rows =
        control.find_object_member("stop_applicability")->as_array();
    const auto selected_stop =
        std::find_if(stop_rows.begin(), stop_rows.end(), [&](const auto &row) {
          return row.find_object_member("control_binding_id")->as_string() ==
                 arm_binding->control_binding_id.value();
        });
    ASSERT_NE(selected_stop, stop_rows.end());
    const auto &stop = *selected_stop;
    const auto &stages =
        stop.find_object_member("escalation_stages")->as_array();
    nominal = stages.front()
                  .find_object_member("default_contract_parameters")
                  ->find_object_member("stop_mode")
                  ->as_string();
    first = stages.front().find_object_member("stop_escalation_stage_id")
                ->as_string();
    for (const auto &stage : stages)
      if (stage.find_object_member("stage_kind")->as_string() == "software_hard") {
        floor = stage.find_object_member("stop_escalation_stage_id")->as_string();
        break;
      }
    ASSERT_FALSE(floor.empty());
    ASSERT_NE(first, floor);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  auto Program(std::string mode, std::optional<std::string> entry = {},
               std::optional<std::string> success_through = {},
               std::optional<V> condition = {}) {
    auto value = d::parse_strict_json_model_yaml(R"({
      "api_version":"manyforge.program.v2","program_id":"node_stop_fixture",
      "program_revision":"sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "program_profile":"composable-cell-v1","name":"Node Stop fixture",
      "catalog":{"api_version":"manyforge.node_catalog.v2","content_version":"0.0.0"},
      "component_roles":{"robot":{}},
      "target_roles":{"arm":{"component_role":"robot","required_capabilities":[],
        "required_frame_roles":["base"],"required_resource_roles":["arm_joints"],
        "required_model_roles":["arm_model"],"required_collision_body_roles":["arm_collision"]}},
      "blackboard":{"namespaces":[],"keys":[{"key":"ready","type":"bool","access":"read_write","persistence":"run","initial":false}]},
      "tree":{"node_id":"motion.move_until_signal","instance_id":"move","children":[],
        "parameters":{"target_role":"arm","tcp_endpoint":{"kind":"current"},"stop_mode":"soft_on_path","priority":7,
          "activation_mode":"require_inactive_at_arm",
          "on_signal":"success","on_no_signal":"failure",
          "goal":{"frame_ref":{"kind":"target_frame","target_role":"arm","frame_role":"base"},
            "position_m":[0.1,0.1,0.1],"orientation":{"xyzw":[0,0,0,1]}},
          "interrupt_condition":{"compare":{"operand":{"key":"ready"},"op":"==","value":true}}}}})")
                     .root();
    value = replace(
        value, "catalog",
        replace(*value.find_object_member("catalog"), "content_version",
                V(bundle->value().catalogs.node_environment.content_version)));
    auto targets = *value.find_object_member("target_roles");
    auto arm = *targets.find_object_member("arm");
    V::Array capabilities;
    for (const auto *id :
         {"execution.feedback.phase_events", "execution.trajectory.execute",
          "planning.motion.pose_goal", "planning.profile.balanced",
          "planning.trajectory.time_parameterized", "state.observed.current",
          "validation.trajectory.non_collision"})
      capabilities.emplace_back(V::Object{{"capability_id", V(id)},
                                          {"parameters", V(V::Object{})},
                                          {"disposition", V("required")}});
    value = replace(
        value, "target_roles",
        replace(targets, "arm",
                replace(arm, "required_capabilities", V(capabilities))));
    auto tree = *value.find_object_member("tree");
    auto parameters =
        replace(*tree.find_object_member("parameters"), "stop_mode", V(mode));
    if (entry)
      parameters = replace(parameters, "stop_entry_stage_id", V(*entry));
    if (success_through)
      parameters = replace(parameters, "success_through_stage_id", V(*success_through));
    if (condition)
      parameters = replace(parameters, "interrupt_condition", *condition);
    value = replace(value, "tree", replace(tree, "parameters", parameters));
    value = d::normalize_program_authoring(value);
    const auto revision = d::detail::compute_content_identity(
        value,
        {"program-revision", "manyforge.program.v2", "program_revision"});
    value = replace(value, "program_revision", V(revision));
    return d::verify_program_binding_contract(d::canonical_json(value),
                                              *bundle);
  }
  auto Binding(const d::ProgramBindingContract &program) {
    d::FilesystemCatalogArtifactStore catalog(package_root);
    return d::compile_program_binding(
        program,
        {{{c::ComponentRoleId("robot"),
           c::ControllableComponentId("lite6_robot")}},
         {{c::TargetRoleId("arm"), c::CommandTargetId("arm")}}},
        *bundle, *map, catalog);
  }
  d::PreparedActivationInput Prepare(const d::ProgramBindingContract &program) {
    const auto roles = d::parse_strict_json_model_yaml(R"({
      "component_roles":[{"component_role_id":"robot","controllable_component_id":"lite6_robot"}],
      "target_roles":[{"target_role_id":"arm","command_target_id":"arm"}]})")
                           .root();
    d::FilesystemCatalogArtifactStore catalog(package_root);
    d::FilesystemRawArtifactStore raw(package_root);
    return d::assemble_prepared_activation(
        {d::PreparedScenarioOrigin{
             "condition.apply.fixture",
             c::ContentDigest("sha256:" + std::string(64U, 'a')),
             "/fixtures/condition-apply.scenario"},
         c::ContentDigest("sha256:" + std::string(64U, 'b')), "optional"},
        *bundle, *map, program, roles, catalog, raw);
  }
  const std::filesystem::path package_root{
      MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT};
  const c::ControlBindingDTO *arm_binding{};
  std::filesystem::path root;
  std::shared_ptr<const d::RetainedBundle> bundle;
  std::optional<d::VerifiedRetainedModelMap> map;
  std::vector<d::PreparedActivationArtifact> artifacts;
  std::string nominal, first, floor;
};

TEST_F(ProgramIoStopResponseTest,
       MetadataUsesSameQualifiedArtifactRouteAndSavedStageOrder) {
  ASSERT_NE(arm_binding, nullptr);
  const auto &bound = *arm_binding;
  const auto &desired_rows = bundle->value().desired_control_configurations;
  const auto desired_it = std::find_if(
      desired_rows.begin(), desired_rows.end(), [&](const auto &row) {
        return std::any_of(row.binding_routes.begin(), row.binding_routes.end(),
                           [&](const auto &route) {
                             return route.control_binding_id ==
                                    bound.control_binding_id;
                           });
      });
  ASSERT_NE(desired_it, desired_rows.end());
  const auto &desired = *desired_it;
  const auto route =
      std::find_if(desired.binding_routes.begin(), desired.binding_routes.end(),
                   [&](const auto &row) {
                     return row.control_binding_id == bound.control_binding_id;
                   });
  ASSERT_NE(route, desired.binding_routes.end());
  const d::FilesystemVerifiedRetainedProductArtifactSource source(package_root);
  const auto metadata = d::read_qualified_io_stop_response_metadata(
      *bundle, *map, bound.control_binding_id, bound.control_binding_revision,
      route->control_route_id, source);
  ASSERT_TRUE(metadata.ok()) << metadata.error().message;
  const auto &value = metadata.value();
  EXPECT_EQ(value.find_object_member("control_binding_id")->as_string(),
            bound.control_binding_id.value());
  EXPECT_EQ(value.find_object_member("control_route_id")->as_string(),
            route->control_route_id.value());
  EXPECT_EQ(value.find_object_member("nominal_stop_mode")->as_string(),
            nominal);
  const auto &projected = value.find_object_member("stages")->as_array();
  const auto raw = d::detail::parse_and_verify_control_domain_control(
      artifacts.front().bytes, &bundle->value().cell_graph,
      map->control_bindings());
  const auto &raw_stops =
      raw.find_object_member("stop_applicability")->as_array();
  const auto raw_stop =
      std::find_if(raw_stops.begin(), raw_stops.end(), [&](const auto &row) {
        return row.find_object_member("control_binding_id")->as_string() ==
               bound.control_binding_id.value();
      });
  ASSERT_NE(raw_stop, raw_stops.end());
  const auto &rows =
      raw_stop->find_object_member("escalation_stages")->as_array();
  ASSERT_EQ(projected.size(), rows.size());
  ASSERT_FALSE(projected.empty());
  EXPECT_EQ(projected.front().find_object_member("stage_id")->as_string(),
            first);
  for (std::size_t i = 0; i < rows.size(); ++i) {
    EXPECT_EQ(projected[i].as_object().size(), 3U);
    EXPECT_EQ(
        projected[i].find_object_member("stage_id")->as_string(),
        rows[i].find_object_member("stop_escalation_stage_id")->as_string());
    EXPECT_EQ(projected[i].find_object_member("stage_kind")->as_string(),
              rows[i].find_object_member("stage_kind")->as_string());
    EXPECT_EQ(projected[i].find_object_member("command_kind")->as_string(),
              rows[i]
                  .find_object_member("containment_command")
                  ->find_object_member("command_kind")
                  ->as_string());
  }
  EXPECT_FALSE(d::read_qualified_io_stop_response_metadata(
                   *bundle, *map, bound.control_binding_id,
                   c::ControlBindingRevision("foreign.revision"),
                   route->control_route_id, source)
                   .ok());
  EXPECT_FALSE(d::read_qualified_io_stop_response_metadata(
                   *bundle, *map, bound.control_binding_id,
                   bound.control_binding_revision,
                   c::ControlRouteId("foreign.route"), source)
                   .ok());
  const auto ref = d::required_io_stop_artifact_for_binding(
      *bundle, bound.control_binding_id, bound.control_binding_revision);
  ASSERT_TRUE(ref.ok());
  EXPECT_EQ(ref.value().content_addressed_store_key,
            artifacts.front().content_addressed_store_key);
  const auto shared_rows = d::read_qualified_io_stop_stage_rows(
      *bundle, *map, bound.control_binding_id, bound.control_binding_revision,
      route->control_route_id, artifacts);
  ASSERT_TRUE(shared_rows.ok()) << shared_rows.error().message;
  ASSERT_EQ(shared_rows.value().size(), projected.size());
  for (std::size_t i = 0; i < projected.size(); ++i)
    EXPECT_EQ(shared_rows.value()[i].stop_escalation_stage_id.value(),
              projected[i].find_object_member("stage_id")->as_string());
  EXPECT_FALSE(d::read_qualified_io_stop_stage_rows(
                   *bundle, *map, bound.control_binding_id,
                   bound.control_binding_revision, route->control_route_id, {})
                   .ok());
  auto corrupted = artifacts;
  corrupted.front().bytes.push_back(' ');
  EXPECT_FALSE(d::read_qualified_io_stop_stage_rows(
                   *bundle, *map, bound.control_binding_id,
                   bound.control_binding_revision, route->control_route_id,
                   corrupted)
                   .ok());
  auto duplicated = artifacts;
  duplicated.push_back(artifacts.front());
  EXPECT_FALSE(d::read_qualified_io_stop_stage_rows(
                   *bundle, *map, bound.control_binding_id,
                   bound.control_binding_revision, route->control_route_id,
                   duplicated)
                   .ok());
  // Querying metadata does not change authoring validation of that same chain.
  const auto program = Program(nominal, first, floor);
  const auto binding = Binding(*program);
  EXPECT_TRUE(d::validate_program_io_stop_responses(*program, *binding, *bundle,
                                                    *map, artifacts)
                  .ok());
}

TEST_F(ProgramIoStopResponseTest,
       GenuineSealedNodeUsesOwnRuleAndQualifiedSavedStopChain) {
  const auto program = Program(nominal, first, floor);
  const auto binding = Binding(*program);
  const auto refs =
      d::required_program_io_stop_artifacts(*program, *binding, *bundle);
  ASSERT_TRUE(refs.ok());
  ASSERT_EQ(refs.value().size(), 1U);
  const auto valid = d::validate_program_io_stop_responses(
      *program, *binding, *bundle, *map, artifacts);
  ASSERT_TRUE(valid.ok()) << valid.error().details.at("reason");
  const auto rules =
      d::compile_program_io_conditions(*program, *binding, *bundle);
  ASSERT_TRUE(rules.ok()) << rules.error().message;
  ASSERT_EQ(rules.value().program_node_conditions.size(), 1U);
  const auto &node = rules.value().program_node_conditions.front();
  ASSERT_TRUE(node.node_rule);
  EXPECT_EQ(node.node_rule->target_role_id, c::TargetRoleId("arm"));
  EXPECT_EQ(node.node_rule->priority, 7);
  EXPECT_EQ(node.node_rule->stop_entry_stage_id,
            c::StopEscalationStageId(first));
  EXPECT_EQ(node.node_rule->success_through_stage_id,
            c::StopEscalationStageId(floor));
  EXPECT_EQ(node.policy_id.value().size(), 69U);
  const auto again =
      d::compile_program_io_conditions(*program, *binding, *bundle);
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(again.value().program_node_conditions.front().policy_id,
            node.policy_id);
}

TEST_F(ProgramIoStopResponseTest,
       ApplyAcceptsLiteralSelectedModeAndStillRejectsUnknownEntry) {
  for (const auto *mode : {"soft_on_path", "spring_back_hold",
                           "spring_back_fast", "hard_deceleration"}) {
    SCOPED_TRACE(mode);
    const auto selected = Program(mode);
    const auto selected_binding = Binding(*selected);
    const auto accepted = d::validate_program_io_stop_responses(
        *selected, *selected_binding, *bundle, *map, artifacts);
    ASSERT_TRUE(accepted.ok()) << accepted.error().details.at("reason");
  }
  const auto unknown = Program(nominal, "foreign");
  const auto unknown_binding = Binding(*unknown);
  const auto wrong_entry = d::validate_program_io_stop_responses(
      *unknown, *unknown_binding, *bundle, *map, artifacts);
  ASSERT_FALSE(wrong_entry.ok());
  EXPECT_EQ(wrong_entry.error().details.at("reason"),
            "io_rule_stop_entry_not_qualified");
}

TEST_F(ProgramIoStopResponseTest,
       OmittedThresholdRetainsEntryOnlyAndExplicitThresholdMustBeInItsSuffix) {
  const auto omitted = Program(nominal, floor);
  const auto omitted_binding = Binding(*omitted);
  ASSERT_TRUE(d::validate_program_io_stop_responses(
                  *omitted, *omitted_binding, *bundle, *map, artifacts).ok());
  const auto projection =
      d::compile_program_io_conditions(*omitted, *omitted_binding, *bundle);
  ASSERT_TRUE(projection.ok());
  ASSERT_TRUE(projection.value().program_node_conditions.front().node_rule);
  EXPECT_FALSE(projection.value().program_node_conditions.front()
                   .node_rule->success_through_stage_id);

  const auto before = Program(nominal, floor, first);
  const auto before_binding = Binding(*before);
  const auto invalid_order = d::validate_program_io_stop_responses(
      *before, *before_binding, *bundle, *map, artifacts);
  ASSERT_FALSE(invalid_order.ok());
  EXPECT_EQ(invalid_order.error().details.at("reason"),
            "io_rule_success_through_outside_suffix");

  const auto foreign = Program(nominal, first, "foreign");
  const auto foreign_binding = Binding(*foreign);
  const auto unknown = d::validate_program_io_stop_responses(
      *foreign, *foreign_binding, *bundle, *map, artifacts);
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().details.at("reason"),
            "io_rule_stop_entry_not_qualified");
}

TEST_F(ProgramIoStopResponseTest,
       IndependentReadbackRefusesMissingCorruptOrDuplicateControlBytes) {
  const auto program = Program(nominal);
  const auto binding = Binding(*program);
  EXPECT_FALSE(d::validate_program_io_stop_responses(*program, *binding,
                                                     *bundle, *map, {})
                   .ok());
  auto corrupt = artifacts;
  corrupt.front().bytes += " ";
  EXPECT_FALSE(d::validate_program_io_stop_responses(*program, *binding,
                                                     *bundle, *map, corrupt)
                   .ok());
  auto duplicate = artifacts;
  duplicate.push_back(duplicate.front());
  EXPECT_FALSE(d::validate_program_io_stop_responses(*program, *binding,
                                                     *bundle, *map, duplicate)
                   .ok());
}
TEST_F(ProgramIoStopResponseTest,
       SharedConditionCheckAcceptsGenuinePublicAssemblyAndDurableReadback) {
  const auto program = Program(nominal, first, floor);
  const auto prepared = Prepare(*program);
  ASSERT_NE(prepared.closure().program_binding, nullptr);
  d::FilesystemPreparedActivationRepository repository(root);
  ASSERT_TRUE(repository.put_if_absent(prepared));
  const auto reread = repository.get_verified(prepared.prepared_activation_id());
  ASSERT_NE(reread, nullptr);
  EXPECT_EQ(reread->prepared_activation_id(), prepared.prepared_activation_id());
  const auto captured = d::compile_program_io_conditions(
      *reread->closure().program, *reread->closure().program_binding,
      *reread->closure().retained_bundle);
  ASSERT_TRUE(captured.ok());
  ASSERT_EQ(captured.value().referenced_program_blackboard_keys.size(), 1U);
  EXPECT_EQ(captured.value().referenced_program_blackboard_keys.front().name,
            "ready");
}

TEST_F(ProgramIoStopResponseTest,
       SharedConditionCheckRefusesMissingKeyAndWrongTypeBeforeAssemblyReturns) {
  for (const auto &condition : {
           d::parse_strict_json_model_yaml(R"({"compare":{"operand":{"key":"missing"},"op":"==","value":true}})").root(),
           d::parse_strict_json_model_yaml(R"({"compare":{"operand":{"key":"ready"},"op":"==","value":1}})").root()}) {
    const auto program = Program(nominal, {}, {}, condition);
    try {
      static_cast<void>(Prepare(*program));
      FAIL() << "invalid bound condition crossed public preparation";
    } catch (const d::PreparedActivationInputError &failure) {
      const std::string message = failure.what();
      EXPECT_TRUE(message.find("interrupt_condition_key_refused") != std::string::npos ||
                  message.find("io_signal_type_mismatch") != std::string::npos)
          << message;
    }
  }
}

TEST_F(ProgramIoStopResponseTest,
       ReidentifiedClosedPayloadStillFailsIndependentRepositoryConditionCheck) {
  const auto valid_program = Program(nominal);
  const auto valid = Prepare(*valid_program);
  const auto invalid_condition = d::parse_strict_json_model_yaml(
      R"({"compare":{"operand":{"key":"missing"},"op":"==","value":true}})").root();
  const auto invalid_program = Program(nominal, {}, {}, invalid_condition);
  const auto invalid_binding = Binding(*invalid_program);
  auto changed = valid.normalized_root();
  auto payload = *changed.find_object_member("activation_payload");
  payload = replace(payload, "program", invalid_program->normalized_program_root());
  payload = replace(payload, "program_binding", invalid_binding->normalized_root());
  changed = replace(changed, "activation_payload", payload);
  changed = replace(changed, "program_revision",
                    V(invalid_program->value().program_revision.value()));
  auto outcome = *changed.find_object_member("program_outcome");
  outcome = replace(outcome, "program_binding_id",
                    V(invalid_binding->program_binding_id().value()));
  changed = replace(changed, "program_outcome", outcome);
  auto identity = changed.as_object();
  std::erase_if(identity, [](const auto &field) {
    return field.first == "prepared_activation_id";
  });
  const auto id = d::sha256_digest(d::canonical_json(V(identity)));
  identity.emplace_back("prepared_activation_id", V(id));
  // The ordinary structural parser/writer supplies the candidate; only the
  // independent repository readback may accept it for actual Apply.
  const auto candidate = d::parse_prepared_activation_input(V(std::move(identity)));
  d::FilesystemPreparedActivationRepository repository(root);
  ASSERT_TRUE(repository.put_if_absent(candidate));
  try {
    static_cast<void>(repository.get_verified(candidate.prepared_activation_id()));
    FAIL() << "reidentified invalid condition crossed repository readback";
  } catch (const d::PreparedActivationInputError &failure) {
    EXPECT_NE(std::string(failure.what()).find("interrupt_condition_key_refused"),
              std::string::npos) << failure.what();
  }
}

} // namespace
