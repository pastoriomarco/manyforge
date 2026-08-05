#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

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

TEST(OperatorAlarmCatalog, LoadsThePackagedDigestVerifiedClosedCatalog) {
  const auto catalog = OperatorAlarmCatalog::Load(PackagedCatalog());
  EXPECT_EQ(catalog.schema_version(), "manyforge.operator_alarm_catalog.v1");
  EXPECT_EQ(catalog.catalog_version(), "2.0.0");
  EXPECT_EQ(catalog.descriptor_digest(),
            "sha256:"
            "8742de67090238e86bc90d0ac0bc6a2dae9463873c8a362ea255709f6d563d3a");
  ASSERT_EQ(catalog.definitions().size(), 6U);
  const auto *collision = catalog.Find("manyforge.motion.runtime_collision");
  ASSERT_NE(collision, nullptr);
  EXPECT_EQ(collision->priority, "critical");
  EXPECT_TRUE(collision->requires_motion_attention);
  EXPECT_EQ(collision->max_evidence_refs, 32U);
  EXPECT_EQ(catalog.Find("manyforge.unknown"), nullptr);
}

TEST(OperatorAlarmCatalog,
     AcceptsSemanticJsonButRejectsTamperUnknownFieldsAndYamlFeatures) {
  const auto source = ReadFile(PackagedCatalog());
  const auto parsed = deployment::parse_strict_json_model_yaml(source);
  const auto canonical_json = content_identity::canonical_json(parsed.root());
  EXPECT_EQ(OperatorAlarmCatalog::Decode(canonical_json).definitions().size(),
            6U);

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
} // namespace manyforge::headless
