#pragma once

#include "lite6_retained_consumer_fixture.hpp"
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/io_provider_lifecycle_catalog.hpp>
#include <manyforge_deployment/io_provider_domain.hpp>

namespace manyforge::deployment::test {

// Reuse the existing artifact-store/materializer carrier only. The actual
// qualified deployment below has a genuine provider domain and no model,
// controller, physical control binding or coordinates.
inline DurableLite6RetainedConsumerFixture
materialize_boolean_io_retained_fixture(
    const std::filesystem::path &package_root, bool include_input = true,
    bool include_output = false, bool ros_output = false,
    bool second_output = false, bool shared_output_channel = false,
    const CanonicalValue *interrupt_rules = nullptr,
    std::string_view output_profile = {}) {
  namespace fixture = lite6_controllable_fixture_detail;
  if ((!include_input && !include_output) || (ros_output && !include_output) ||
      (second_output && !include_output) ||
      (shared_output_channel && !second_output))
    throw std::logic_error(
        "I/O fixture needs an actual selected signal profile");
  if ((!output_profile.empty() && !include_output) ||
      (!output_profile.empty() && output_profile != "ur_set_io" &&
       output_profile != "ros2_control_gpio"))
    throw std::logic_error("I/O fixture output profile is unsupported");
  auto carrier = make_lite6_controllable_retention_fixture(
      false, false, 0U, false, true, "io.cell", false, false, "resource", false,
      {}, {}, true);
  const auto runtime = *carrier.ros2_control_execution_runtime_profile;
  const auto runtime_key = runtime.reference().content_addressed_store_key;
  std::vector<Lite6InputArtifact> artifacts;
  for (const auto &artifact : carrier.control_catalog_artifacts)
    if (artifact.store_key == runtime_key)
      artifacts.push_back(artifact);
  if (artifacts.size() != 1U)
    throw std::logic_error("I/O fixture has no qualified runtime source");

  std::vector<QualifiedCapabilityDefinition> capabilities;
  CanonicalValue::Array selectors;
  std::vector<core::CatalogRecordRefDTO> records{runtime.reference()};
  std::vector<const char *> capability_names;
  if (include_input)
    capability_names = {"io.gpio.get_input", "io.gpio.wait_for_input"};
  if (include_output)
    capability_names.push_back("io.gpio.set_output");
  for (const auto *name :
       {"component.lifecycle.observe", "component.lifecycle.prepare",
        "component.lifecycle.enable", "component.lifecycle.disable",
        "component.lifecycle.recover"})
    capability_names.push_back(name);
  // Both catalog collections are keyed sets by capability_id. Revision them
  // in their schema-normalized order, including mixed input/output targets.
  std::sort(capability_names.begin(), capability_names.end(),
            [](const char *left, const char *right) {
              return std::string_view(left) < std::string_view(right);
            });
  for (const auto *name : capability_names) {
    const auto *builtin = core::find_builtin_capability_definition(name);
    if (!builtin)
      throw std::logic_error("I/O fixture capability is not builtin");
    const auto bytes = canonical_json(CanonicalValue(CanonicalValue::Object{
        {"api_version",
         CanonicalValue(std::string(core::kCapabilityDefinitionApiVersion))},
        {"capability_id", CanonicalValue(name)},
        {"capability_definition_revision",
         CanonicalValue(std::string(builtin->capability_definition_revision))},
        {"scope", CanonicalValue(std::string(core::to_string(builtin->scope)))},
        {"provider_binding_kind", CanonicalValue(std::string(core::to_string(
                                      builtin->provider_binding_kind)))}}));
    const core::ContentDigest digest(sha256_digest(bytes));
    const core::ContentAddressedStoreKey key(
        fixture::catalog_key("capability_definition", digest.value()));
    const core::CapabilityDefinitionCatalogRecordRefDTO reference{
        core::CapabilityId(name),
        core::CapabilityDefinitionRevision(
            std::string(builtin->capability_definition_revision)),
        core::SchemaId(std::string(core::kCapabilityDefinitionApiVersion)),
        fixture::schema_revision_for(
            core::CatalogRecordKind::kCapabilityDefinition),
        digest,
        key};
    auto capability =
        resolve_and_qualify_capability_definition(reference, key, bytes);
    if (std::string_view(name).starts_with("io.gpio."))
      selectors.emplace_back(CanonicalValue::Object{
          {"capability_id", CanonicalValue(name)},
          {"capability_definition_revision",
           CanonicalValue(
               capability.value().capability_definition_revision.value())}});
    artifacts.push_back(fixture::catalog_artifact(
        "catalog://capability/" + std::string(name), bytes, capability));
    records.push_back(capability.reference());
    capabilities.push_back(std::move(capability));
  }
  auto type_source = parse_strict_json_model_yaml(R"({
    "api_version":"manyforge.component_type.v1","component_type_id":"fixture.boolean_io",
    "tags":["io"],"model_roles":[],"model_mounts":[],"attachment_interfaces":[],
    "command_targets":[{"command_target_id":"gpio","model_group_roles":[],"frame_roles":[],
      "collision_body_roles":[],"resource_roles":[{"resource_role_id":"ready","source_kind":"component_local"}],
      "capabilities":[]}]
  })")
                         .root()
                         .as_object();
  for (auto &[name, value] : type_source)
    if (name == "command_targets") {
      auto targets = value.as_array();
      auto target = targets.front().as_object();
      for (auto &[key, selected] : target) {
        if (key == "capabilities")
          selected = CanonicalValue(selectors);
        if (key == "resource_roles" && (!include_input || include_output)) {
          CanonicalValue::Array roles;
          if (include_input)
            roles.emplace_back(CanonicalValue::Object{
                {"resource_role_id", CanonicalValue("ready")},
                {"source_kind", CanonicalValue("component_local")}});
          if (include_output)
            roles.emplace_back(CanonicalValue::Object{
                {"resource_role_id", CanonicalValue("lamp")},
                {"source_kind", CanonicalValue("component_local")}});
          if (second_output)
            roles.emplace_back(CanonicalValue::Object{
                {"resource_role_id", CanonicalValue("lamp_other")},
                {"source_kind", CanonicalValue("component_local")}});
          selected = CanonicalValue(std::move(roles));
        }
      }
      targets.front() = CanonicalValue(std::move(target));
      value = CanonicalValue(std::move(targets));
    }
  const auto type_bytes = fixture::revisioned_document(
      CanonicalValue(std::move(type_source)),
      "catalog-record-revision/component_type", core::kComponentTypeApiVersion,
      "component_type_revision");
  const core::ContentDigest type_digest(sha256_digest(type_bytes));
  const core::ContentAddressedStoreKey type_key(
      fixture::catalog_key("component_type", type_digest.value()));
  const core::ComponentTypeCatalogRecordRefDTO type_ref{
      core::ComponentTypeId("fixture.boolean_io"),
      core::ComponentTypeRevision(fixture::revision_from_document(
          type_bytes, "component_type_revision")),
      core::SchemaId(std::string(core::kComponentTypeApiVersion)),
      fixture::schema_revision_for(core::CatalogRecordKind::kComponentType),
      type_digest,
      type_key};
  std::vector<QualifiedCapabilityDefinition> target_capabilities;
  for (const auto &capability : capabilities)
    if (capability.value().capability_id.value().starts_with("io.gpio."))
      target_capabilities.push_back(capability);
  const auto type = resolve_and_qualify_component_type(
      type_ref, type_key, type_bytes, target_capabilities);
  artifacts.push_back(fixture::catalog_artifact(
      "catalog://component_type/fixture.boolean_io", type_bytes, type));
  records.push_back(type.reference());

  std::vector<QualifiedCommandSchema> command_schemas;
  if (include_output) {
    const auto schema = core::builtin_io_signal_write_command_schema();
    const auto &compatible = schema.compatible_capabilities.front();
    const auto bytes = canonical_json(CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(
                            core::kIoSignalWriteCommandSchemaApiVersion))},
        {"command_schema_id", CanonicalValue(schema.command_schema_id.value())},
        {"command_schema_revision",
         CanonicalValue(schema.command_schema_revision.value())},
        {"compatible_capabilities",
         CanonicalValue(
             CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                 {"capability_id",
                  CanonicalValue(compatible.capability_id.value())},
                 {"capability_definition_revision",
                  CanonicalValue(
                      compatible.capability_definition_revision.value())}})})},
        {"command_value",
         CanonicalValue(CanonicalValue::Object{
             {"value_kind", CanonicalValue("io_signal_write")}})}}));
    const core::ContentDigest digest(sha256_digest(bytes));
    const core::ContentAddressedStoreKey key(
        fixture::catalog_key("command_schema", digest.value()));
    const core::CommandSchemaCatalogRecordRefDTO reference{
        schema.command_schema_id,
        schema.command_schema_revision,
        core::SchemaId(std::string(core::kCommandSchemaApiVersion)),
        fixture::schema_revision_for(core::CatalogRecordKind::kCommandSchema),
        digest,
        key};
    auto qualified = resolve_and_qualify_command_schema(reference, key, bytes);
    artifacts.push_back(fixture::catalog_artifact(
        "catalog://command_schema/io.signal_write_boolean", bytes, qualified));
    records.push_back(qualified.reference());
    command_schemas.push_back(std::move(qualified));
  }

  CanonicalValue::Array offers;
  for (const auto &capability : capabilities) {
    auto offer = CanonicalValue::Object{
        {"capability_id",
         CanonicalValue(capability.value().capability_id.value())},
        {"capability_definition_revision",
         CanonicalValue(
             capability.value().capability_definition_revision.value())}};
    offer.emplace_back("required_endpoint_role_ids",
                       fixture::text_array({"io_signal_service"}));
    offers.emplace_back(std::move(offer));
  }
  const auto plans = core::MakeIoProviderLifecyclePlans();
  if (!plans.ok())
    throw std::logic_error("I/O fixture lifecycle plans are invalid");
  const auto semantic = core::ComputeBrandLifecyclePlanCatalogSemanticDigest(
      core::ProviderManifestId("manyforge.provider.io_signals"), plans.value());
  if (!semantic.ok())
    throw std::logic_error("I/O fixture lifecycle semantic anchor is invalid");
  const auto manifest_bytes = fixture::revisioned_document(
      CanonicalValue(CanonicalValue::Object{
          {"api_version",
           CanonicalValue(std::string(core::kProviderManifestApiVersion))},
          {"provider_manifest_id",
           CanonicalValue("manyforge.provider.io_signals")},
          {"offered_capabilities", CanonicalValue(std::move(offers))},
          {"provider_contracts", CanonicalValue(CanonicalValue::Array{})},
          {"lifecycle_plan_catalog",
           CanonicalValue(CanonicalValue::Object{
               {"catalog_api_version",
                CanonicalValue(
                    std::string(core::kBrandLifecyclePlanCatalogApiVersion))},
               {"semantic_digest",
                CanonicalValue(semantic.value().value())}})}}),
      "catalog-record-revision/provider_manifest",
      core::kProviderManifestApiVersion, "provider_manifest_revision");
  const core::ContentDigest manifest_digest(sha256_digest(manifest_bytes));
  const core::ContentAddressedStoreKey manifest_key(
      fixture::catalog_key("provider_manifest", manifest_digest.value()));
  const core::ProviderManifestCatalogRecordRefDTO manifest_ref{
      core::ProviderManifestId("manyforge.provider.io_signals"),
      core::ProviderManifestRevision(fixture::revision_from_document(
          manifest_bytes, "provider_manifest_revision")),
      core::SchemaId(std::string(core::kProviderManifestApiVersion)),
      fixture::schema_revision_for(core::CatalogRecordKind::kProviderManifest),
      manifest_digest,
      manifest_key};
  const auto manifest = resolve_and_qualify_provider_manifest(
      manifest_ref, manifest_key, manifest_bytes);
  artifacts.push_back(fixture::catalog_artifact(
      "catalog://provider_manifest/manyforge.provider.io_signals",
      manifest_bytes, manifest));
  records.push_back(manifest.reference());

  auto configuration =
      ros2_control_execution_runtime_contract::empty_host_configuration()
          .as_object();
  configuration.emplace_back("io_signals", parse_strict_json_model_yaml(R"([
    {"controllable_component_id":"device.io","command_target_id":"gpio","resource_role_id":"ready",
     "signal_name":"ready","source_id":"source.io","value_type":"bool","direction":"input",
     "update_mode":"change_only","transition_capability":"sampled_only",
     "freshness":{"mode":"provider_validity"},"provider":{"profile":"mock"}}
  ])")
                                               .root());
  if (!include_input || include_output) {
    auto signals = configuration.back().second.as_array();
    if (!include_input)
      signals.clear();
    if (include_output) {
      auto output = parse_strict_json_model_yaml(R"({
        "controllable_component_id":"device.io","command_target_id":"gpio",
        "resource_role_id":"lamp","signal_name":"lamp","source_id":"source.output",
        "value_type":"bool","direction":"output",
        "provider":{"profile":"mock_output","channel":"lamp"}
      })")
                        .root();
      if (ros_output)
        output = parse_strict_json_model_yaml(R"({
          "controllable_component_id":"device.io","command_target_id":"gpio",
          "resource_role_id":"lamp","signal_name":"lamp","source_id":"source.output",
          "value_type":"bool","direction":"output",
          "provider":{"profile":"ros_set_bool","service":"/io_device/lamp"}
        })")
                     .root();
      if (!output_profile.empty()) {
        auto selected = output.as_object();
        for (auto &[key, value] : selected)
          if (key == "provider") {
            if (output_profile == "ur_set_io")
              value = CanonicalValue(CanonicalValue::Object{
                  {"profile", CanonicalValue("ur_set_io")},
                  {"service", CanonicalValue("/io_device/set_io")},
                  {"pin", CanonicalValue(3.0)}});
            else
              value = CanonicalValue(CanonicalValue::Object{
                  {"profile", CanonicalValue("ros2_control_gpio")},
                  {"topic", CanonicalValue("/io_device/commands")},
                  {"group", CanonicalValue("digital_outputs")},
                  {"interface_name", CanonicalValue("lamp")}});
          }
        output = CanonicalValue(std::move(selected));
      }
      if (second_output) {
        auto other = output.as_object();
        for (auto &[name, value] : other) {
          if (name == "resource_role_id" || name == "signal_name")
            value = CanonicalValue("lamp_other");
          if (name == "source_id" && !shared_output_channel)
            value = CanonicalValue("source.output.other");
          if (name == "provider") {
            auto provider = value.as_object();
            for (auto &[field, address] : provider)
              if (!shared_output_channel &&
                  (field == "channel" || field == "service" ||
                   field == "interface_name"))
                address =
                    CanonicalValue(field == "service" ? "/io_device/lamp_other"
                                                      : "lamp_other");
            value = CanonicalValue(std::move(provider));
          }
        }
        signals.push_back(output);
        signals.emplace_back(std::move(other));
      } else {
        signals.push_back(std::move(output));
      }
    }
    configuration.back().second = CanonicalValue(std::move(signals));
  }
  CanonicalValue::Array providers;
  for (const auto &capability : capabilities) {
    if (!capability.value().capability_id.value().starts_with("io.gpio."))
      continue;
    providers.emplace_back(CanonicalValue::Object{
        {"capability_provider_binding_id",
         CanonicalValue(capability.value().capability_id.value())},
        {"capability_provider_binding_revision",
         CanonicalValue(std::string(fixture::kZeroDigest))},
        {"capability_id",
         CanonicalValue(capability.value().capability_id.value())},
        {"capability_definition_revision",
         CanonicalValue(
             capability.value().capability_definition_revision.value())},
        {"scope", CanonicalValue("target_binding")},
        {"target",
         CanonicalValue(CanonicalValue::Object{
             {"controllable_component_id", CanonicalValue("device.io")},
             {"component_type_revision",
              CanonicalValue(type.value().component_type_revision.value())},
             {"command_target_id", CanonicalValue("gpio")}})},
        {"provider_instance_id", CanonicalValue("provider.io")},
        {"provider_manifest_id",
         CanonicalValue(manifest.value().provider_manifest_id.value())},
        {"provider_manifest_revision",
         CanonicalValue(manifest.value().provider_manifest_revision.value())},
        {"endpoint_bindings",
         CanonicalValue(
             CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                 {"endpoint_role_id", CanonicalValue("io_signal_service")},
                 {"source_kind", CanonicalValue("runtime_unit")},
                 {"source_id", CanonicalValue("io.execution")},
                 {"source_endpoint_role_id",
                  CanonicalValue("ros2_control_execution_status")}})})}});
  }
  CanonicalValue source(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"deployment_artifact_id", CanonicalValue("io.fixture")},
      {"deployment_revision",
       CanonicalValue(std::string(fixture::kZeroDigest))},
      {"cell_id", CanonicalValue("io.cell")},
      {"name", CanonicalValue("Boolean I/O only")},
      {"catalogs", detail::encode_deployment_catalogs(records)},
      {"desired_control_configurations",
       CanonicalValue(CanonicalValue::Array{})},
      {"capability_provider_bindings", CanonicalValue(std::move(providers))},
      {"graph",
       CanonicalValue(CanonicalValue::Object{
           {"runtime_units",
            CanonicalValue(
                CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                    {"runtime_unit_id", CanonicalValue("io.execution")},
                    {"runtime_profile_id",
                     CanonicalValue(
                         runtime.value().runtime_profile_id.value())},
                    {"runtime_profile_revision",
                     CanonicalValue(
                         runtime.value().runtime_profile_revision.value())},
                    {"dependency_runtime_unit_ids",
                     CanonicalValue(CanonicalValue::Array{})},
                    {"configuration",
                     CanonicalValue(std::move(configuration))}})})},
           {"controllable_components",
            CanonicalValue(
                CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                    {"controllable_component_id", CanonicalValue("device.io")},
                    {"component_type_id",
                     CanonicalValue(type.value().component_type_id.value())},
                    {"component_type_revision",
                     CanonicalValue(
                         type.value().component_type_revision.value())},
                    {"model_role_bindings",
                     CanonicalValue(CanonicalValue::Array{})},
                    {"runtime_unit_ids",
                     fixture::text_array({"io.execution"})}})})},
           {"model_instances", CanonicalValue(CanonicalValue::Array{})},
           {"control_domains", CanonicalValue(CanonicalValue::Array{})},
           {"control_bindings", CanonicalValue(CanonicalValue::Array{})},
           {"component_attachments",
            CanonicalValue(CanonicalValue::Array{})}})}});
  if (interrupt_rules && !interrupt_rules->as_array().empty()) {
    auto fields = source.as_object();
    fields.emplace_back(
        "policies",
        CanonicalValue(CanonicalValue::Object{
            {"retained_policy_records",
             CanonicalValue(CanonicalValue::Array{})},
            {"stop_stage_parameters", CanonicalValue(CanonicalValue::Array{})},
            {"commissioned_stop_interfaces",
             CanonicalValue(CanonicalValue::Array{})},
            {"interrupt_sources", CanonicalValue(CanonicalValue::Array{})},
            {"interrupt_policies", CanonicalValue(CanonicalValue::Array{})},
            {"interrupt_config",
             parse_strict_json_model_yaml(R"({"queue_depth":8,
          "cancel_validation_timeout_ns":1000,"motion_scope_arm_timeout_ns":1000,
          "supervisory_poll_rate_hz":10})")
                 .root()},
            {"interrupt_rules", *interrupt_rules}}));
    source = CanonicalValue(std::move(fields));
  }
  const std::vector<QualifiedRuntimeProfile> runtimes{runtime};
  const auto preliminary = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(canonical_json(source)), runtimes);
  const auto derived = DeriveIoProviderDomains(
      preliminary.graph().graph(),
      preliminary.applicability_inputs().value().capability_provider_bindings,
      runtimes, std::vector<QualifiedProviderManifest>{manifest});
  if (!derived.ok() || derived.value().provider_domains.size() != 1U)
    throw std::logic_error("I/O fixture has no actual derived provider domain");
  const auto &domain = derived.value().provider_domains.front();
  auto members = source.as_object();
  for (auto &[field, value] : members) {
    if (field == "graph") {
      auto graph = value.as_object();
      for (auto &[name, table] : graph)
        if (name == "control_domains")
          table = CanonicalValue(
              CanonicalValue::Array{IoProviderDomainRecord(domain)});
      value = CanonicalValue(std::move(graph));
    }
    if (field == "capability_provider_bindings") {
      auto rows = value.as_array();
      const auto original = rows.front().as_object();
      for (const auto &capability : capabilities) {
        if (!capability.value().capability_id.value().starts_with(
                "component.lifecycle."))
          continue;
        auto row = original;
        for (auto &[name, selected] : row) {
          if (name == "capability_provider_binding_id" ||
              name == "capability_id")
            selected = CanonicalValue(capability.value().capability_id.value());
          if (name == "capability_definition_revision")
            selected = CanonicalValue(
                capability.value().capability_definition_revision.value());
          if (name == "scope")
            selected = CanonicalValue("control_domain");
          if (name == "target")
            selected = CanonicalValue(CanonicalValue::Object{
                {"control_domain_id",
                 CanonicalValue(domain.control_domain_id.value())},
                {"control_domain_revision",
                 CanonicalValue(domain.control_domain_revision.value())},
                {"domain_kind", CanonicalValue("provider")}});
        }
        rows.emplace_back(std::move(row));
      }
      value = CanonicalValue(std::move(rows));
    }
  }
  source = CanonicalValue(std::move(members));
  const auto deployment = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(canonical_json(source)), runtimes);
  const auto composition = compose_cell_occurrences(
      deployment.graph(), {}, std::vector<QualifiedComponentType>{type});
  const auto overlay = select_site_overlay_for_deployment({}, {}, nullptr);
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), runtimes, command_schemas,
      capabilities, {}, {}, std::vector<QualifiedProviderManifest>{manifest},
      {}, {}, {}, overlay);
  carrier.qualified = qualify_deployment_for_retention(
      deployment, compile_cell_ir(applicability));
  carrier.component_type = type;
  carrier.control_catalog_artifacts = std::move(artifacts);
  return materialize_lite6_retained_consumer_fixture_from(
      package_root, std::move(carrier), /*retain_empty_model_map=*/true);
}
} // namespace manyforge::deployment::test
