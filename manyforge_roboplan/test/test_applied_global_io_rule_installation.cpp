#include "applied_global_io_rule_installation.hpp"
#include "../../manyforge_deployment/test/maintained_interrupt_program_fixture.hpp"
#include "product_backed_runtime_participant_resolver.hpp"
#include <manyforge_core/types/program_io_condition.hpp>

#include "../../manyforge_deployment/test/io_retained_fixture.hpp"
#include "content_identity_internal.hpp"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <gtest/gtest.h>
#include <manyforge_core/io_signal_subsystem.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_io_condition_compiler.hpp>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace manyforge::roboplan_backend::host_detail {
namespace {
namespace d = deployment;
namespace c = core;
using V = d::CanonicalValue;

V replace(const V &root, std::string name, V value) {
  auto fields = root.as_object();
  for (auto &[key, selected] : fields)
    if (key == name) {
      selected = std::move(value);
      return V(std::move(fields));
    }
  fields.emplace_back(std::move(name), std::move(value));
  return V(std::move(fields));
}

class AppliedGlobalIoRuleInstallationTest : public ::testing::Test {
protected:
  void SetUp() override {
    root = std::filesystem::temp_directory_path() /
           ("manyforge-global-catalog-" + std::to_string(::getpid()) + "." +
            std::to_string(next.fetch_add(1U)));
    ASSERT_TRUE(std::filesystem::create_directory(root));
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
  void Build(bool with_rules) {
    if (!with_rules) {
      bundle =
          d::test::materialize_lite6_retained_consumer_fixture_from(
              root,
              d::test::
                  make_lite6_ros2_control_shared_consumer_retention_fixture(
                      {}, 1U, true))
              .bundle;
    } else {
      // Out-of-order authored IDs cross the real normalizer, qualifier,
      // materializer and retained reload, rather than a constructed catalog.
      const auto rules = d::parse_strict_json_model_yaml(R"([
        {"rule_id":"zeta","condition":{"compare":{"operand":{"signal":{
          "controllable_component_id":"device.io","signal_name":"ready"}},
          "op":"==","value":false}},"scope":"all_motion","on_trip":"stop","after_stop":"hold","active_at_start":true},
        {"rule_id":"alpha","condition":{"compare":{"operand":{"key":"ready"},
          "op":"==","value":true}},"scope":"everything","on_trip":"stop","after_stop":"hold","active_at_start":true}])")
                             .root();
      bundle = d::test::materialize_boolean_io_retained_fixture(
                   root, true, false, false, false, false, &rules)
                   .bundle;
    }
  }
  void BuildPureKeyRules() {
    const auto configuration = d::parse_strict_json_model_yaml(R"({
      "interrupt_rules":[
        {"rule_id":"zeta","condition":{"compare":{"operand":{"key":"ready"},
          "op":"==","value":false}},"scope":"all_motion","on_trip":"stop","after_stop":"hold","active_at_start":true},
        {"rule_id":"alpha","condition":{"not":{"compare":{"operand":{"key":"ready"},
          "op":"==","value":true}}},"scope":"everything","on_trip":"stop","after_stop":"hold","active_at_start":true}]})")
                                   .root();
    bundle = d::test::materialize_lite6_retained_consumer_fixture_from(
        root, d::test::make_lite6_controllable_retention_fixture(
                  false, false, 1U, {}, false, true, "resource",
                  true, {}, {}, false, false, std::nullopt,
                  d::test::kLite6ArmTargetId, std::nullopt, std::nullopt, {}, true,
                  {}, "manyforge.provider.ros2_control", false, true,
                  nullptr, nullptr, true, "pneumatic", std::nullopt,
                  "commanded", nullptr, &configuration))
                 .bundle;
  }
  c::IoRuleAppliedContextDTO Context() const {
    // This helper test supplies prospective identity only. It does not forge
    // successful Apply, a current facade, an admission claim or driver facts.
    return {c::RequestOwnerEpoch("host.catalog"),
            c::ActiveBundlePublicationGeneration(7U),
            bundle->retained_bundle_id(),
            bundle->value().cell_graph.graph_revision};
  }
  void BindProgram() {
    auto value = d::parse_strict_json_model_yaml(R"({
      "api_version":"manyforge.program.v3","program_id":"catalog_projection",
      "program_revision":"sha256:0000000000000000000000000000000000000000000000000000000000000000",
      "program_profile":"composable-cell-v1","name":"Catalog projection fixture",
      "catalog":{"api_version":"manyforge.node_catalog.v3","content_version":"0.0.0"},
        "blackboard":{"namespaces":[],"keys":[{"key":"ready","type":"bool",
        "access":"read_write","persistence":"run","initial":false}]},
      "tree":{"node_id":"sequence","instance_id":"root","parameters":{},"children":[]}})")
                     .root();
    value = replace(
        value, "catalog",
        replace(*value.find_object_member("catalog"), "content_version",
                V(bundle->value().catalogs.node_environment.content_version)));
    value = d::normalize_program_authoring(value);
    value = replace(value, "program_revision",
                    V(d::detail::compute_content_identity(
                        value, {"program-revision", "manyforge.program.v3",
                                "program_revision"})));
    if (bundle->value().compiled_products.symbol_role_maps.size() != 1U)
      throw std::logic_error("genuine sole retained model-map product missing");
    d::FilesystemVerifiedRetainedModelMapSource maps(root);
    const auto map =
        maps.get_verified({bundle->retained_bundle_id(),
                           bundle->value()
                               .compiled_products.symbol_role_maps.front()
                               .product_id});
    if (!map)
      throw std::logic_error("genuine retained model map missing");
    d::FilesystemCatalogArtifactStore artifacts(root);
    program = d::verify_program_binding_contract(
        d::canonical_json(value), *bundle, *map, artifacts);
    binding = d::compile_program_binding(*program, *bundle, *map,
                                         artifacts);
  }
  std::shared_ptr<c::IoSignalSubsystem>
  Service(const c::RequestOwnerEpoch &owner =
              c::RequestOwnerEpoch("host.catalog")) {
    const auto configured =
        d::ResolveRetainedIoSignalCatalog(bundle->value().cell_graph);
    if (!configured.ok())
      throw std::logic_error(configured.error().message);
    std::vector<c::IoSignalDescriptorDTO> descriptors;
    std::set<c::InterruptSourceId> synthetic;
    for (const auto &row : configured.value()) {
      descriptors.push_back(row.descriptor);
      if (row.provider.profile == "mock")
        synthetic.insert(row.descriptor.source_id);
    }
    auto created = c::IoSignalSubsystem::Create(
        {owner, 19U, std::move(descriptors),
         std::vector<c::InterruptSourceId>(synthetic.begin(), synthetic.end())},
        {16U, 32U, 8U, 8U, 256U, 1024U}, clock);
    if (!created.ok())
      throw std::logic_error(created.error().message);
    return std::shared_ptr<c::IoSignalSubsystem>(std::move(created.value()));
  }

  static inline std::atomic<std::uint64_t> next{};
  std::filesystem::path root;
  std::shared_ptr<const d::RetainedBundle> bundle;
  std::shared_ptr<const d::ProgramBindingContract> program;
  std::shared_ptr<const d::ProgramBinding> binding;
  c::FakeClock clock;
};

TEST_F(AppliedGlobalIoRuleInstallationTest,
       GenuineEmptyReferenceUnionReturnsNoCatalogWithoutService) {
  Build(false);
  const auto empty =
      CompileGlobalIoRuleCatalog(Context(), *bundle, nullptr, nullptr, {});
  ASSERT_TRUE(empty.ok()) << empty.error().message;
  EXPECT_EQ(empty.value().catalog, nullptr);
  EXPECT_EQ(empty.value().waiting_for_program, nullptr);
  // A declared but unreferenced ordinary key does not invent BB demand.
  BindProgram();
  const auto with_program = CompileGlobalIoRuleCatalog(
      Context(), *bundle, program.get(), binding.get(), {});
  ASSERT_TRUE(with_program.ok()) << with_program.error().message;
  EXPECT_EQ(with_program.value().catalog, nullptr);
  EXPECT_EQ(with_program.value().waiting_for_program, nullptr);
}

TEST_F(AppliedGlobalIoRuleInstallationTest,
       ProgramOnlyConditionCannotUseTheVerifiedEmptyShortcut) {
  const auto fixture = deployment::test::LoadMaintainedInterruptProgram(
      MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT, root);
  bundle = fixture.bundle;
  program = fixture.program;
  binding = fixture.binding;
  const auto conditions =
      d::compile_program_io_conditions(*program, *binding, *bundle);
  ASSERT_TRUE(conditions.ok()) << conditions.error().message;
  ASSERT_EQ(conditions.value().referenced_program_blackboard_keys.size(), 1U);
  EXPECT_EQ(conditions.value().referenced_program_blackboard_keys.front().name,
            "ready");
  const auto absent = CompileGlobalIoRuleCatalog(
      Context(), *bundle, program.get(), binding.get(), {});
  ASSERT_FALSE(absent.ok());
  EXPECT_EQ(absent.error().details.at("reason"),
            "io_global_rule_service_missing");
  const auto compiled = CompileGlobalIoRuleCatalog(
      Context(), *bundle, program.get(), binding.get(), Service());
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  ASSERT_NE(compiled.value().catalog, nullptr);
  EXPECT_EQ(compiled.value().waiting_for_program, nullptr);
  EXPECT_TRUE(compiled.value().catalog->rules.empty());
}

TEST_F(AppliedGlobalIoRuleInstallationTest,
       MaintainedNodeConditionCrossesTheActualResolverCaptureBoundary) {
  const auto fixture = deployment::test::LoadMaintainedInterruptProgram(
      MANYFORGE_PROGRAM_CAPTURE_PACKAGE_ROOT, root);
  const auto target = d::resolve_runtime_program_target(
      *fixture.binding, fixture.binding->value().target_bindings.front().target,
      c::CapabilityId("execution.trajectory.execute"), *fixture.bundle,
      fixture.model_map);
  ASSERT_TRUE(target.control_binding());
  ASSERT_TRUE(target.control_domain());
  ASSERT_TRUE(target.desired_route());
  const auto &control = *target.control_binding();
  std::set<c::RuntimeUnitId> runtime_units(
      target.component().runtime_unit_ids.begin(),
      target.component().runtime_unit_ids.end());
  runtime_units.insert(target.control_domain()->runtime_unit_ids.begin(),
                       target.control_domain()->runtime_unit_ids.end());
  runtime_units.insert(control.provider_runtime_unit_id);
  std::vector<c::RuntimeUnitEpochBindingDTO> epochs;
  for (const auto &id : runtime_units)
    epochs.push_back({id, c::RuntimeEpoch("capture.fixture." + id.value())});
  // These explicit composition observations construct a qualified prototype;
  // they do not assert Kernel readiness, an Enabled controller or a held Run.
  const auto prototype = c::verify_runtime_participant_binding(
      d::build_runtime_participant_binding_dto(
          target,
          {*target.desired_route(),
           c::ActiveControlConfigurationId("capture.fixture.configuration"),
           c::ActiveControlConfigurationGeneration(1U),
           std::move(epochs),
           {{control.provider_instance_id, control.provider_runtime_unit_id,
             c::RuntimeEpoch("capture.fixture." +
                             control.provider_runtime_unit_id.value())}}}));
  ASSERT_TRUE(prototype.ok()) << prototype.error().message;
  ProductBackedRuntimeParticipantResolver resolver(
      fixture.package_root, fixture.bundle, fixture.model_map,
      {prototype.value()}, {}, fixture.program_artifact_root);
  const auto &identity = fixture.binding->value();
  const c::ProgramRunExecutionCaptureDTO request{
      c::RequestOwnerEpoch("capture.fixture.kernel"),
      c::RequestOwnerEpoch("capture.fixture.runner"),
      c::CellAdmissionClaimGeneration(1U), c::ProgramRunId("capture.fixture.run"),
      identity.program_id, identity.program_revision, identity.program_binding_id,
      identity.retained_bundle_id, identity.graph_revision, {}};
  const auto captured = resolver.ResolveProgramCapture(request);
  ASSERT_TRUE(captured.ok()) << captured.error().message;
  ASSERT_NE(captured.value().table, nullptr);
  ASSERT_NE(captured.value().binding, nullptr);
  EXPECT_EQ(*captured.value().table, fixture.program->execution_table());
  EXPECT_EQ(*captured.value().binding, fixture.binding->value());
  ASSERT_NE(captured.value().io_conditions, nullptr);
  const auto &conditions = *captured.value().io_conditions;
  ASSERT_EQ(conditions.program_node_conditions.size(), 1U);
  const auto &node = conditions.program_node_conditions.front();
  EXPECT_EQ(node.node_instance_id, c::ProgramNodeInstanceId("move"));
  ASSERT_NE(node.condition, nullptr);
  ASSERT_TRUE(node.node_rule);
  EXPECT_EQ(node.node_rule->target, target.target_binding().target);
  EXPECT_EQ(node.node_rule->activation_mode,
            c::MotionInterruptActivationMode::kRequireInactiveAtArm);
  EXPECT_EQ(node.node_rule->stop_mode,
            c::MotionInterruptStopMode::kSoftOnPath);
  EXPECT_TRUE(node.required_signal_sources.empty());
  ASSERT_EQ(conditions.referenced_program_blackboard_keys.size(), 1U);
  EXPECT_EQ(conditions.referenced_program_blackboard_keys.front().name, "ready");
  // Only identity-only ingress may acquire these server-derived references.
  auto injected = request;
  injected.io_conditions = captured.value().io_conditions;
  EXPECT_FALSE(resolver.ResolveProgramCapture(std::move(injected)).ok());
}

TEST_F(AppliedGlobalIoRuleInstallationTest,
       GenuineRulesUseActualServiceIdentityAndDoNotCreateAReading) {
  Build(true);
  BindProgram();
  const auto service = Service();
  const auto before = service->RuleServiceConfiguration();
  ASSERT_TRUE(before.ok());
  const c::IoSignalId signal{c::ControllableComponentId("device.io"), "ready"};
  const auto missing = service->ReadSignal(signal);
  ASSERT_TRUE(missing.ok());
  EXPECT_EQ(missing.value().availability, c::IoSignalAvailability::kMissing);
  const auto compiled = CompileGlobalIoRuleCatalog(
      Context(), *bundle, program.get(), binding.get(), service);
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  ASSERT_NE(compiled.value().catalog, nullptr);
  EXPECT_EQ(compiled.value().waiting_for_program, nullptr);
  const auto &catalog = *compiled.value().catalog;
  EXPECT_EQ(catalog.context.owner_epoch, before.value().owner_epoch);
  EXPECT_EQ(catalog.context.catalog_generation,
            before.value().catalog_generation);
  EXPECT_EQ(catalog.context.active_publication_generation,
            Context().active_publication_generation);
  EXPECT_EQ(catalog.context.retained_bundle_id, bundle->retained_bundle_id());
  EXPECT_EQ(catalog.context.graph_revision,
            bundle->value().cell_graph.graph_revision);
  ASSERT_EQ(catalog.rules.size(), 2U);
  EXPECT_EQ(catalog.rules[0].policy_id, c::InterruptPolicyId("alpha"));
  EXPECT_EQ(catalog.rules[1].policy_id, c::InterruptPolicyId("zeta"));
  EXPECT_EQ(catalog.program_binding_id, binding->program_binding_id());
  EXPECT_EQ(catalog.referenced_program_blackboard_keys,
            std::vector<std::string>{"ready"});
  EXPECT_EQ(
      catalog.rules[1].required_signal_sources,
      std::vector<c::InterruptSourceId>{c::InterruptSourceId("source.io")});
  const auto after = service->ReadSignal(signal);
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value().availability, c::IoSignalAvailability::kMissing);
  EXPECT_FALSE(after.value().latest_sample.has_value());
  EXPECT_EQ(after.value().cursor, missing.value().cursor);
}

TEST_F(AppliedGlobalIoRuleInstallationTest,
       MissingActualServiceForeignContextAndUnpairedProgramRefuse) {
  Build(true);
  BindProgram();
  const auto service = Service();
  const auto absent = CompileGlobalIoRuleCatalog(
      Context(), *bundle, program.get(), binding.get(), {});
  ASSERT_FALSE(absent.ok());
  EXPECT_EQ(absent.error().details.at("reason"),
            "io_global_rule_service_missing");
  const auto no_service_waiting =
      CompileGlobalIoRuleCatalog(Context(), *bundle, nullptr, nullptr, {});
  ASSERT_FALSE(no_service_waiting.ok());
  EXPECT_EQ(no_service_waiting.error().details.at("reason"),
            "io_global_rule_service_missing");
  const auto foreign_service = Service(c::RequestOwnerEpoch("foreign.owner"));
  const auto foreign = CompileGlobalIoRuleCatalog(
      Context(), *bundle, program.get(), binding.get(), foreign_service);
  ASSERT_FALSE(foreign.ok());
  EXPECT_EQ(foreign.error().details.at("reason"),
            "io_global_rule_service_context_mismatch");
  auto context = Context();
  context.graph_revision = c::GraphRevision("foreign.graph");
  const auto wrong_product = CompileGlobalIoRuleCatalog(
      context, *bundle, program.get(), binding.get(), service);
  ASSERT_FALSE(wrong_product.ok());
  EXPECT_EQ(wrong_product.error().details.at("reason"),
            "io_global_rule_candidate_context_mismatch");
  EXPECT_FALSE(CompileGlobalIoRuleCatalog(Context(), *bundle, program.get(),
                                          nullptr, service)
                   .ok());
  EXPECT_FALSE(CompileGlobalIoRuleCatalog(Context(), *bundle, nullptr,
                                          binding.get(), service)
                   .ok());
  // No Program grants no key source: the whole key rule waits, while the
  // independently authored signal-only sibling remains a checked rule.
  const auto waiting =
      CompileGlobalIoRuleCatalog(Context(), *bundle, nullptr, nullptr, service);
  ASSERT_TRUE(waiting.ok()) << waiting.error().message;
  ASSERT_NE(waiting.value().catalog, nullptr);
  EXPECT_EQ(waiting.value().waiting_for_program, nullptr);
  const auto &rules = waiting.value().catalog->rules;
  ASSERT_EQ(rules.size(), 2U);
  EXPECT_EQ(rules[0].policy_id, c::InterruptPolicyId("alpha"));
  EXPECT_EQ(rules[0].condition, nullptr);
  EXPECT_EQ(rules[0].waiting_for_program_keys,
            std::vector<std::string>{"ready"});
  EXPECT_EQ(rules[1].policy_id, c::InterruptPolicyId("zeta"));
  EXPECT_NE(rules[1].condition, nullptr);
  EXPECT_TRUE(rules[1].waiting_for_program_keys.empty());
  EXPECT_TRUE(waiting.value().catalog->referenced_program_blackboard_keys.empty());
  const auto untouched = service->ReadSignal(
      {c::ControllableComponentId("device.io"), "ready"});
  ASSERT_TRUE(untouched.ok());
  EXPECT_EQ(untouched.value().availability, c::IoSignalAvailability::kMissing);
  EXPECT_FALSE(untouched.value().latest_sample);
}

TEST_F(AppliedGlobalIoRuleInstallationTest,
       PureKeyNoIoCatalogWaitsWithAppliedIdentityAndNoServiceGeneration) {
  BuildPureKeyRules();
  const auto signals =
      d::ResolveRetainedIoSignalCatalog(bundle->value().cell_graph);
  ASSERT_TRUE(signals.ok());
  ASSERT_TRUE(signals.value().empty());
  const auto compiled =
      CompileGlobalIoRuleCatalog(Context(), *bundle, nullptr, nullptr, {});
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  EXPECT_EQ(compiled.value().catalog, nullptr);
  ASSERT_NE(compiled.value().waiting_for_program, nullptr);
  const auto &waiting = *compiled.value().waiting_for_program;
  EXPECT_EQ(waiting.context, Context());
  ASSERT_EQ(waiting.rules.size(), 2U);
  EXPECT_EQ(waiting.rules[0].policy_id, c::InterruptPolicyId("alpha"));
  EXPECT_EQ(waiting.rules[1].policy_id, c::InterruptPolicyId("zeta"));
  for (const auto &rule : waiting.rules) {
    EXPECT_EQ(rule.condition, nullptr);
    EXPECT_EQ(rule.waiting_for_program_keys, std::vector<std::string>{"ready"});
    EXPECT_TRUE(rule.required_signal_sources.empty());
  }
}

TEST_F(AppliedGlobalIoRuleInstallationTest,
       ActualProgramRecompilesWaitingRulesAndCannotAvoidTheRequiredService) {
  BuildPureKeyRules();
  const auto before =
      CompileGlobalIoRuleCatalog(Context(), *bundle, nullptr, nullptr, {});
  ASSERT_TRUE(before.ok()) << before.error().message;
  const auto waiting = before.value().waiting_for_program;
  ASSERT_NE(waiting, nullptr);
  BindProgram();
  const auto absent = CompileGlobalIoRuleCatalog(
      Context(), *bundle, program.get(), binding.get(), {});
  ASSERT_FALSE(absent.ok());
  EXPECT_EQ(absent.error().details.at("reason"),
            "io_global_rule_service_missing");
  const auto service = Service();
  const auto actual = service->RuleServiceConfiguration();
  ASSERT_TRUE(actual.ok());
  const auto after = CompileGlobalIoRuleCatalog(
      Context(), *bundle, program.get(), binding.get(), service);
  ASSERT_TRUE(after.ok()) << after.error().message;
  ASSERT_NE(after.value().catalog, nullptr);
  EXPECT_EQ(after.value().waiting_for_program, nullptr);
  EXPECT_EQ(after.value().catalog->context.catalog_generation,
            actual.value().catalog_generation);
  EXPECT_EQ(after.value().catalog->program_binding_id,
            binding->program_binding_id());
  EXPECT_EQ(after.value().catalog->referenced_program_blackboard_keys,
            std::vector<std::string>{"ready"});
  for (const auto &rule : after.value().catalog->rules) {
    EXPECT_NE(rule.condition, nullptr);
    EXPECT_TRUE(rule.waiting_for_program_keys.empty());
  }
  // Compilation of a later actual product cannot mutate the retained waiting
  // product or turn its absent condition into a source/activation claim.
  EXPECT_EQ(waiting->context, Context());
  for (const auto &rule : waiting->rules)
    EXPECT_EQ(rule.condition, nullptr);
}

} // namespace
} // namespace manyforge::roboplan_backend::host_detail
