#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::headless {

inline constexpr std::string_view kOperatorAlarmCatalogSchemaVersion =
    "manyforge.operator_alarm_catalog.v1";
inline constexpr std::size_t kMaximumOperatorAlarmCatalogBytes = 1024U * 1024U;
inline constexpr std::size_t kMaximumOperatorAlarmDefinitions = 256U;

class OperatorAlarmCatalogError : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

struct OperatorAlarmDefinition final {
  std::string definition_id;
  std::uint64_t version;
  std::string title;
  std::string message_template;
  std::string alarm_class;
  std::string priority;
  std::string priority_rationale;
  std::string consequence;
  std::string operator_response;
  std::string set_authority;
  std::string clear_authority;
  std::string adapter_id;
  std::vector<std::string> runtime_modes;
  /// False selects the ISA-18.2 no-acknowledgement path: activation enters
  /// active_acknowledged without attribution and return is terminal.
  bool acknowledgement_required;
  bool acknowledgement_reason_required;
  /// Motion gating: an acknowledgement-required definition gates while
  /// unacknowledged; one without acknowledgement gates exactly while active.
  bool requires_motion_attention;
  std::uint64_t max_evidence_refs;

  friend bool operator==(const OperatorAlarmDefinition &,
                         const OperatorAlarmDefinition &) = default;
};

class OperatorAlarmCatalog final {
public:
  [[nodiscard]] static OperatorAlarmCatalog Decode(std::string_view yaml_bytes);
  [[nodiscard]] static OperatorAlarmCatalog
  Load(const std::filesystem::path &path);

  [[nodiscard]] const std::string &schema_version() const noexcept;
  [[nodiscard]] const std::string &catalog_version() const noexcept;
  [[nodiscard]] const std::string &descriptor_digest() const noexcept;
  [[nodiscard]] const std::vector<OperatorAlarmDefinition> &
  definitions() const noexcept;
  [[nodiscard]] const OperatorAlarmDefinition *
  Find(std::string_view definition_id) const noexcept;

private:
  OperatorAlarmCatalog(std::string schema_version, std::string catalog_version,
                       std::string descriptor_digest,
                       std::vector<OperatorAlarmDefinition> definitions);

  std::string schema_version_;
  std::string catalog_version_;
  std::string descriptor_digest_;
  std::vector<OperatorAlarmDefinition> definitions_;
};

} // namespace manyforge::headless
