#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

#include <manyforge_core/types/carried_object_lost.hpp>
#include "manyforge_headless/operator_alarm_catalog.hpp"

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-headless-alarm-catalog-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("alarm catalog test mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot read alarm catalog test input");
  }
  std::ostringstream output;
  output << stream.rdbuf();
  return output.str();
}

std::filesystem::path PackagedCatalog() {
  return MANYFORGE_HEADLESS_TEST_ALARM_CATALOG;
}

std::string Redigest(std::string source) {
  const auto root = deployment::parse_strict_json_model_yaml(source).root();
  const auto *old_digest = std::get_if<std::string>(
      &root.find_object_member("descriptor_digest")->storage());
  if (old_digest == nullptr) {
    throw std::runtime_error("catalog test input has no descriptor digest");
  }
  const CanonicalValue digest_value(CanonicalValue::Object{
      {"catalog_version", *root.find_object_member("catalog_version")},
      {"definitions", *root.find_object_member("definitions")},
      {"schema_version", *root.find_object_member("schema_version")},
  });
  const auto replacement = content_identity::sha256_digest(
      content_identity::canonical_json(digest_value));
  const auto position = source.find(*old_digest);
  if (position == std::string::npos) {
    throw std::runtime_error("catalog test digest spelling is absent");
  }
  source.replace(position, old_digest->size(), replacement);
  return source;
}

// V2-5: acknowledgement never gates work. The Program failure alarm needs
// acknowledgement for History but gates no motion (an automatic or operator
// Retry is never refused because it is unacknowledged).
TEST(OperatorAlarmCatalog,
     ProgramFailureHasDistinctProgramAuthorityAndGatesNoMotion) {
  const auto catalog = OperatorAlarmCatalog::Load(PackagedCatalog());
  const auto *definition = catalog.Find("manyforge.program.operation_failure");
  ASSERT_NE(definition, nullptr);
  EXPECT_EQ(definition->priority, "high");
  EXPECT_TRUE(definition->acknowledgement_required);
  EXPECT_FALSE(definition->requires_motion_attention);
  EXPECT_EQ(definition->set_authority,
            "manyforge.program_runner.failure_condition");
  EXPECT_EQ(definition->clear_authority, definition->set_authority);
  EXPECT_NE(definition->set_authority,
            catalog.Find("manyforge.motion.execution_fault")->set_authority);
}
TEST(OperatorAlarmCatalog, LoadsThePackagedDigestVerifiedClosedCatalog) {
  const auto catalog = OperatorAlarmCatalog::Load(PackagedCatalog());
  EXPECT_EQ(catalog.schema_version(), "manyforge.operator_alarm_catalog.v1");
  EXPECT_EQ(catalog.catalog_version(), "2.13.0");
  EXPECT_EQ(catalog.descriptor_digest(),
            "sha256:"
            "5d3199014adcc6b3dba1fc832cccf09e6ec195178668b51a70780a15e17e5a9e");
  ASSERT_FALSE(catalog.definitions().empty());
  // The dedicated carried-object alarm repeats its error row's compact text,
  // needs acknowledgement and gates no motion.
  const auto *lost = catalog.Find(manyforge::core::kCarriedObjectLostRow.code);
  ASSERT_NE(lost, nullptr);
  EXPECT_EQ(lost->title, manyforge::core::kCarriedObjectLostRow.compact_text);
  EXPECT_EQ(lost->alarm_class, "scene");
  EXPECT_TRUE(lost->acknowledgement_required);
  EXPECT_FALSE(lost->requires_motion_attention);
  for (const auto name : {"manyforge.runtime.program_runner_loss",
                          "manyforge.runtime.operation_journal_write_fault",
                          "manyforge.runtime.kernel_internal_fault"}) {
    const auto* fault = catalog.Find(name);
    ASSERT_NE(fault, nullptr);
    const bool journal =
        std::string_view(name) == "manyforge.runtime.operation_journal_write_fault";
    EXPECT_EQ(fault->priority, journal ? "low" : "high");
    EXPECT_EQ(fault->acknowledgement_required, !journal);
    EXPECT_FALSE(fault->requires_motion_attention);  // V2-5
    EXPECT_EQ(fault->adapter_id, "manyforge.alarm_adapter.runtime_fault_condition");
    EXPECT_EQ(fault->max_evidence_refs, 1U);
  }
  // OD-215: the disk check's alarm gates motion exactly while active, clears
  // by itself and needs no acknowledgement.
  const auto *disk = catalog.Find("manyforge.runtime.disk_cannot_keep_records");
  ASSERT_NE(disk, nullptr);
  EXPECT_EQ(disk->title, "Disk cannot keep ManyForge's records");
  EXPECT_EQ(disk->priority, "high");
  EXPECT_FALSE(disk->acknowledgement_required);
  EXPECT_TRUE(disk->requires_motion_attention);
  EXPECT_EQ(disk->adapter_id, "manyforge.alarm_adapter.runtime_health_condition");
  // OD-247/OD-248: one generic row for every component fault; acknowledged,
  // never a motion gate (the cell latch already blocks execution).
  const auto *component_fault =
      catalog.Find("manyforge.runtime.component_fault");
  ASSERT_NE(component_fault, nullptr);
  EXPECT_EQ(component_fault->title, "Component fault");
  EXPECT_EQ(component_fault->message_template, "Fault on component {subject_id}");
  EXPECT_EQ(component_fault->alarm_class, "runtime_health");
  EXPECT_EQ(component_fault->priority, "high");
  EXPECT_TRUE(component_fault->acknowledgement_required);
  EXPECT_FALSE(component_fault->acknowledgement_reason_required);
  EXPECT_FALSE(component_fault->requires_motion_attention);
  EXPECT_EQ(component_fault->adapter_id,
            "manyforge.alarm_adapter.runtime_health_condition");
  EXPECT_EQ(component_fault->set_authority,
            "manyforge.component_coordinator.fault_condition");
  EXPECT_EQ(component_fault->clear_authority, component_fault->set_authority);
  EXPECT_EQ(component_fault->runtime_modes, (std::vector<std::string>{"all"}));
  EXPECT_EQ(component_fault->max_evidence_refs, 1U);
  // Lost operation records clear by themselves and gate no motion.
  const auto *records_lost =
      catalog.Find("manyforge.runtime.operation_journal_write_fault");
  EXPECT_EQ(records_lost->title, "Operation records are being lost");
  EXPECT_FALSE(records_lost->acknowledgement_reason_required);
  EXPECT_EQ(records_lost->version, 2U);
  // Neutral: the details say whether its containment Full Stop was accepted.
  EXPECT_EQ(catalog.Find("manyforge.runtime.kernel_internal_fault")->title,
            "Internal fault");
  const auto *collision = catalog.Find("manyforge.motion.runtime_collision");
  ASSERT_NE(collision, nullptr);
  EXPECT_EQ(collision->priority, "critical");
  EXPECT_FALSE(collision->requires_motion_attention);  // V2-5
  EXPECT_TRUE(collision->acknowledgement_required);
  EXPECT_EQ(collision->max_evidence_refs, 32U);
  // Lost measured robot state stops motion through the same containment;
  // its alarm differs from the collision alarm only in its wording.
  const auto *observation_lost =
      catalog.Find("manyforge.motion.runtime_monitor_observation_lost");
  ASSERT_NE(observation_lost, nullptr);
  EXPECT_EQ(observation_lost->title, "Robot position feedback lost");
  EXPECT_EQ(observation_lost->priority, collision->priority);
  EXPECT_EQ(observation_lost->set_authority, collision->set_authority);
  EXPECT_EQ(observation_lost->clear_authority, collision->clear_authority);
  EXPECT_EQ(observation_lost->adapter_id, collision->adapter_id);
  EXPECT_FALSE(observation_lost->requires_motion_attention);  // V2-5
  EXPECT_TRUE(observation_lost->acknowledgement_required);
  EXPECT_EQ(observation_lost->max_evidence_refs, 32U);
  // OD-217: collision checks that did not run in time stop motion through
  // the same containment; their alarm differs only in its wording.
  const auto *checks_stopped =
      catalog.Find("manyforge.motion.runtime_monitor_checks_stopped");
  ASSERT_NE(checks_stopped, nullptr);
  EXPECT_EQ(checks_stopped->title, "Collision checks stopped");
  EXPECT_EQ(checks_stopped->priority, collision->priority);
  EXPECT_EQ(checks_stopped->set_authority, collision->set_authority);
  EXPECT_EQ(checks_stopped->clear_authority, collision->clear_authority);
  EXPECT_EQ(checks_stopped->adapter_id, collision->adapter_id);
  EXPECT_FALSE(checks_stopped->requires_motion_attention);  // V2-5
  EXPECT_TRUE(checks_stopped->acknowledgement_required);
  EXPECT_EQ(checks_stopped->max_evidence_refs, 32U);
  const auto *scope = catalog.Find("manyforge.runtime.alarm_scope_unavailable");
  ASSERT_NE(scope, nullptr);
  EXPECT_EQ(scope->alarm_class, "runtime_health");
  EXPECT_EQ(scope->adapter_id,
            "manyforge.alarm_adapter.runtime_health_condition");
  // The one alarm that gates motion: while active (no acknowledgement).
  EXPECT_TRUE(scope->requires_motion_attention);
  EXPECT_FALSE(scope->acknowledgement_required);
  EXPECT_FALSE(scope->acknowledgement_reason_required);
  EXPECT_EQ(scope->version, 2U);
  const auto *previous = catalog.Find("manyforge.runtime.previous_session_unclean");
  ASSERT_NE(previous, nullptr);
  EXPECT_EQ(previous->alarm_class, "runtime_health");
  EXPECT_EQ(previous->priority, "medium");
  EXPECT_TRUE(previous->acknowledgement_required);
  EXPECT_FALSE(previous->acknowledgement_reason_required);
  EXPECT_FALSE(previous->requires_motion_attention);
  // History's own loss alarm: it clears by itself and gates nothing.
  const auto *history = catalog.Find("manyforge.runtime.history_missing_records");
  ASSERT_NE(history, nullptr);
  EXPECT_EQ(history->title, "History is missing records");
  EXPECT_EQ(history->alarm_class, "runtime_health");
  EXPECT_EQ(history->priority, "low");
  EXPECT_EQ(history->adapter_id,
            "manyforge.alarm_adapter.runtime_health_condition");
  EXPECT_FALSE(history->acknowledgement_required);
  EXPECT_FALSE(history->acknowledgement_reason_required);
  EXPECT_FALSE(history->requires_motion_attention);
  EXPECT_EQ(catalog.Find("manyforge.unknown"), nullptr);
}

TEST(OperatorAlarmCatalog,
     AcceptsSemanticJsonButRejectsTamperUnknownFieldsAndYamlFeatures) {
  const auto source = ReadFile(PackagedCatalog());
  const auto parsed = deployment::parse_strict_json_model_yaml(source);
  const auto canonical_json = content_identity::canonical_json(parsed.root());
  EXPECT_EQ(OperatorAlarmCatalog::Decode(canonical_json).definitions().size(),
            OperatorAlarmCatalog::Decode(source).definitions().size());

  auto tampered = source;
  const auto title = tampered.find("Runtime collision stop");
  ASSERT_NE(title, std::string::npos);
  tampered.replace(title, std::string("Runtime collision stop").size(),
                   "Changed collision stop");
  EXPECT_THROW(OperatorAlarmCatalog::Decode(tampered),
               OperatorAlarmCatalogError);

  auto unknown = source;
  const auto definitions = unknown.find("definitions:");
  ASSERT_NE(definitions, std::string::npos);
  unknown.insert(definitions, "unknown: true\n");
  EXPECT_THROW(OperatorAlarmCatalog::Decode(unknown),
               OperatorAlarmCatalogError);

  EXPECT_THROW(OperatorAlarmCatalog::Decode(
                   "schema_version: manyforge.operator_alarm_catalog.v1\n"
                   "schema_version: duplicate\n"),
               OperatorAlarmCatalogError);
  EXPECT_THROW(
      OperatorAlarmCatalog::Decode("base: &base {value: one}\ncopy: *base\n"),
      OperatorAlarmCatalogError);
  EXPECT_THROW(OperatorAlarmCatalog::Decode("value: !!str text\n"),
               OperatorAlarmCatalogError);

  auto invalid_authority = source;
  const auto authority =
      invalid_authority.find("manyforge.kernel.runtime_collision_condition");
  ASSERT_NE(authority, std::string::npos);
  invalid_authority.replace(
      authority,
      std::string("manyforge.kernel.runtime_collision_condition").size(),
      "InvalidAuthority");
  EXPECT_THROW(OperatorAlarmCatalog::Decode(Redigest(invalid_authority)),
               OperatorAlarmCatalogError);

  auto invalid_template = source;
  const auto placeholder = invalid_template.find("{subject_id}");
  ASSERT_NE(placeholder, std::string::npos);
  invalid_template.replace(placeholder, std::string("{subject_id}").size(),
                           "{unknown}");
  EXPECT_THROW(OperatorAlarmCatalog::Decode(Redigest(invalid_template)),
               OperatorAlarmCatalogError);

  auto invalid_mode = source;
  const auto mode = invalid_mode.find("      - program\n");
  ASSERT_NE(mode, std::string::npos);
  invalid_mode.replace(mode, std::string("      - program\n").size(),
                       "      - Invalid\n");
  EXPECT_THROW(OperatorAlarmCatalog::Decode(Redigest(invalid_mode)),
               OperatorAlarmCatalogError);
}

TEST(OperatorAlarmCatalog, AcknowledgementPolicyIsPerDefinition) {
  const auto source = ReadFile(PackagedCatalog());
  constexpr std::string_view kRequired = "    acknowledgement_required: true\n";
  constexpr std::string_view kNotRequired =
      "    acknowledgement_required: false\n";

  // The first definition may drop acknowledgement like the alarm-scope one.
  auto no_ack = source;
  const auto first = no_ack.find(kRequired);
  ASSERT_NE(first, std::string::npos);
  no_ack.replace(first, kRequired.size(), kNotRequired);
  const auto decoded = OperatorAlarmCatalog::Decode(Redigest(no_ack));
  EXPECT_FALSE(
      decoded.Find("manyforge.motion.runtime_collision")->acknowledgement_required);

  // A required acknowledgement reason needs a required acknowledgement.
  auto reason = no_ack;
  constexpr std::string_view kNoReason =
      "    acknowledgement_reason_required: false\n";
  const auto reason_field = reason.find(kNoReason, first);
  ASSERT_NE(reason_field, std::string::npos);
  reason.replace(reason_field, kNoReason.size(),
                 "    acknowledgement_reason_required: true\n");
  EXPECT_THROW(OperatorAlarmCatalog::Decode(Redigest(reason)),
               OperatorAlarmCatalogError);

  auto not_boolean = source;
  not_boolean.replace(first, kRequired.size(),
                      "    acknowledgement_required: until_acknowledged\n");
  EXPECT_THROW(OperatorAlarmCatalog::Decode(Redigest(not_boolean)),
               OperatorAlarmCatalogError);
}

TEST(OperatorAlarmCatalog, RejectsSymlinkAndNonCatalogFiles) {
  TemporaryDirectory directory;
  const auto target = directory.path() / "catalog.yaml";
  {
    std::ofstream stream(target);
    stream << ReadFile(PackagedCatalog());
  }
  const auto link = directory.path() / "catalog-link.yaml";
  std::filesystem::create_symlink(target.filename(), link);
  EXPECT_THROW(OperatorAlarmCatalog::Load(link), OperatorAlarmCatalogError);

  const auto empty = directory.path() / "empty.yaml";
  { std::ofstream stream(empty); }
  EXPECT_THROW(OperatorAlarmCatalog::Load(empty), OperatorAlarmCatalogError);
}

} // namespace
// V2-5: acknowledgement never gates work. The loader refuses an alarm that
// both requires acknowledgement and gates motion; only a condition interlock
// (no acknowledgement) can gate motion.
TEST(OperatorAlarmCatalog, RefusesAnAcknowledgementAlarmThatGatesMotion) {
  const auto source = ReadFile(PackagedCatalog());
  // The first definition (runtime collision) requires acknowledgement.
  constexpr std::string_view kNoMotion = "    requires_motion_attention: false\n";
  auto gating = source;
  const auto field = gating.find(kNoMotion);
  ASSERT_NE(field, std::string::npos);
  ASSERT_LT(field, gating.find("  - definition_id:", gating.find("  - definition_id:") + 1U));
  gating.replace(field, kNoMotion.size(), "    requires_motion_attention: true\n");
  EXPECT_THROW(OperatorAlarmCatalog::Decode(Redigest(gating)), OperatorAlarmCatalogError);
}

} // namespace manyforge::headless
