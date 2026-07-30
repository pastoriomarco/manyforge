#pragma once

#include <compare>
#include <ostream>
#include <string>
#include <utility>

namespace manyforge::core {

/// String-backed identity/revision value with no implicit conversion between
/// semantic domains.
///
/// Validation of spelling, digest form, and non-emptiness belongs to the
/// schema/value validators that construct trusted records. This type supplies
/// only the compile-time separation required by the ROS-free contracts.
template <typename Tag>
class StrongString {
 public:
  explicit StrongString(std::string value) : value_(std::move(value)) {}

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

  friend bool operator==(const StrongString&, const StrongString&) = default;
  friend auto operator<=>(const StrongString&, const StrongString&) = default;

 private:
  std::string value_;
};

template <typename Tag>
std::ostream& operator<<(std::ostream& stream, const StrongString<Tag>& value) {
  return stream << value.value();
}

#define MANYFORGE_DECLARE_STRONG_STRING(Name) \
  struct Name##Tag final {};                  \
  using Name = StrongString<Name##Tag>

MANYFORGE_DECLARE_STRONG_STRING(CellId);
MANYFORGE_DECLARE_STRONG_STRING(GraphRevision);
MANYFORGE_DECLARE_STRONG_STRING(SchemaId);
MANYFORGE_DECLARE_STRONG_STRING(SchemaRevision);
MANYFORGE_DECLARE_STRONG_STRING(ContentDigest);
MANYFORGE_DECLARE_STRONG_STRING(ContentAddressedStoreKey);

MANYFORGE_DECLARE_STRONG_STRING(RuntimeProfileId);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeProfileRevision);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeRoleId);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeUnitId);
MANYFORGE_DECLARE_STRONG_STRING(RuntimePolicyId);
MANYFORGE_DECLARE_STRONG_STRING(RuntimePolicyRevision);

MANYFORGE_DECLARE_STRONG_STRING(ComponentTypeId);
MANYFORGE_DECLARE_STRONG_STRING(ComponentTypeRevision);
MANYFORGE_DECLARE_STRONG_STRING(ControllableComponentId);
MANYFORGE_DECLARE_STRONG_STRING(ModelAssetId);
MANYFORGE_DECLARE_STRONG_STRING(ModelInstanceId);
MANYFORGE_DECLARE_STRONG_STRING(ModelRevision);
MANYFORGE_DECLARE_STRONG_STRING(ModelRoleId);
MANYFORGE_DECLARE_STRONG_STRING(ModelGroupRoleId);
MANYFORGE_DECLARE_STRONG_STRING(CommandTargetId);
MANYFORGE_DECLARE_STRONG_STRING(CapabilityId);
MANYFORGE_DECLARE_STRONG_STRING(CapabilityDefinitionRevision);
MANYFORGE_DECLARE_STRONG_STRING(FrameRoleId);
MANYFORGE_DECLARE_STRONG_STRING(CollisionBodyRoleId);
MANYFORGE_DECLARE_STRONG_STRING(ResourceId);
MANYFORGE_DECLARE_STRONG_STRING(ResourceRoleId);
MANYFORGE_DECLARE_STRONG_STRING(AttachmentInterfaceId);

MANYFORGE_DECLARE_STRONG_STRING(ControlInterfaceContractId);
MANYFORGE_DECLARE_STRONG_STRING(ControlInterfaceRevision);
MANYFORGE_DECLARE_STRONG_STRING(ControllerTopologyId);
MANYFORGE_DECLARE_STRONG_STRING(TopologyRevision);
MANYFORGE_DECLARE_STRONG_STRING(ControlRouteId);
MANYFORGE_DECLARE_STRONG_STRING(ControlDomainId);
MANYFORGE_DECLARE_STRONG_STRING(ControlDomainRevision);
MANYFORGE_DECLARE_STRONG_STRING(ControlBindingId);
MANYFORGE_DECLARE_STRONG_STRING(ControlBindingRevision);
MANYFORGE_DECLARE_STRONG_STRING(EndpointRoleId);
MANYFORGE_DECLARE_STRONG_STRING(StateAuthorityId);
MANYFORGE_DECLARE_STRONG_STRING(StopEvidencePolicyId);
MANYFORGE_DECLARE_STRONG_STRING(StopEvidencePolicyRevision);
MANYFORGE_DECLARE_STRONG_STRING(StopBudgetPhaseId);
MANYFORGE_DECLARE_STRONG_STRING(StopEvidenceItemId);
MANYFORGE_DECLARE_STRONG_STRING(ProviderContractRoleId);
MANYFORGE_DECLARE_STRONG_STRING(RegisteredStopCommandKindId);
MANYFORGE_DECLARE_STRONG_STRING(RegisteredStopEvidenceKindId);
MANYFORGE_DECLARE_STRONG_STRING(ContactPolicyId);
MANYFORGE_DECLARE_STRONG_STRING(ContactPolicyRevision);

MANYFORGE_DECLARE_STRONG_STRING(ProviderInstanceId);
MANYFORGE_DECLARE_STRONG_STRING(ProviderManifestRevision);

MANYFORGE_DECLARE_STRONG_STRING(ComponentAttachmentId);
MANYFORGE_DECLARE_STRONG_STRING(ComponentAttachmentRevision);

MANYFORGE_DECLARE_STRONG_STRING(CommandSchemaId);
MANYFORGE_DECLARE_STRONG_STRING(CommandSchemaRevision);

#undef MANYFORGE_DECLARE_STRONG_STRING

}  // namespace manyforge::core
