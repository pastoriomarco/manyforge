#pragma once

#include <compare>
#include <cstdint>
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

/// Unsigned revision/generation value with compile-time separation between
/// semantic domains.
///
/// Zero is representable because absence and initialization rules belong to
/// the enclosing DTO state machine. DTOs that require an accepted/minted
/// generation validate `nonzero()` at their trust boundary.
template <typename Tag>
class StrongCounter {
 public:
  explicit constexpr StrongCounter(std::uint64_t value) noexcept : value_(value) {}

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool nonzero() const noexcept { return value_ != 0U; }

  friend constexpr bool operator==(const StrongCounter&, const StrongCounter&) = default;
  friend constexpr auto operator<=>(const StrongCounter&, const StrongCounter&) = default;

 private:
  std::uint64_t value_;
};

template <typename Tag>
std::ostream& operator<<(std::ostream& stream, const StrongCounter<Tag>& value) {
  return stream << value.value();
}

#define MANYFORGE_DECLARE_STRONG_STRING(Name) \
  struct Name##Tag final {};                  \
  using Name = StrongString<Name##Tag>

#define MANYFORGE_DECLARE_STRONG_COUNTER(Name) \
  struct Name##Tag final {};                   \
  using Name = StrongCounter<Name##Tag>

MANYFORGE_DECLARE_STRONG_STRING(CellId);
MANYFORGE_DECLARE_STRONG_STRING(GraphRevision);
MANYFORGE_DECLARE_STRONG_STRING(SchemaId);
MANYFORGE_DECLARE_STRONG_STRING(SchemaRevision);
MANYFORGE_DECLARE_STRONG_STRING(ContentDigest);
MANYFORGE_DECLARE_STRONG_STRING(ContentAddressedStoreKey);

MANYFORGE_DECLARE_STRONG_STRING(RuntimeProfileId);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeProfileRevision);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeImplementationFamilyId);
MANYFORGE_DECLARE_STRONG_STRING(ApiContractId);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeContractOfferDigest);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeContractRequirementDigest);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeContractManifestSetDigest);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeRoleId);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeUnitId);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeEpoch);
MANYFORGE_DECLARE_STRONG_STRING(RuntimePolicyId);
MANYFORGE_DECLARE_STRONG_STRING(RuntimePolicyRevision);
MANYFORGE_DECLARE_STRONG_STRING(InterruptSourceId);
MANYFORGE_DECLARE_STRONG_STRING(InterruptPolicyId);
MANYFORGE_DECLARE_STRONG_STRING(MotionInterruptScopeId);

MANYFORGE_DECLARE_STRONG_STRING(ComponentTypeId);
MANYFORGE_DECLARE_STRONG_STRING(ComponentTypeRevision);
MANYFORGE_DECLARE_STRONG_STRING(ControllableComponentId);
MANYFORGE_DECLARE_STRONG_STRING(ModelAssetId);
MANYFORGE_DECLARE_STRONG_STRING(ModelInstanceId);
MANYFORGE_DECLARE_STRONG_STRING(ModelRevision);
MANYFORGE_DECLARE_STRONG_STRING(ModelRoleId);
MANYFORGE_DECLARE_STRONG_STRING(ModelGroupRoleId);
MANYFORGE_DECLARE_STRONG_STRING(ComponentRoleId);
MANYFORGE_DECLARE_STRONG_STRING(CommandTargetId);
MANYFORGE_DECLARE_STRONG_STRING(TargetRoleId);
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
MANYFORGE_DECLARE_STRONG_STRING(ControllerRoleId);
MANYFORGE_DECLARE_STRONG_STRING(ActivationSetId);
MANYFORGE_DECLARE_STRONG_STRING(ControlRouteId);
MANYFORGE_DECLARE_STRONG_STRING(ControlDomainId);
MANYFORGE_DECLARE_STRONG_STRING(ControlDomainRevision);
MANYFORGE_DECLARE_STRONG_STRING(ControlBindingId);
MANYFORGE_DECLARE_STRONG_STRING(ControlBindingRevision);
MANYFORGE_DECLARE_STRONG_STRING(ScalingDomainId);
MANYFORGE_DECLARE_STRONG_STRING(CoordinatedScalingBarrierId);
MANYFORGE_DECLARE_STRONG_STRING(ActiveControlConfigurationId);
MANYFORGE_DECLARE_STRONG_STRING(ResolvedStopBasePlanId);
MANYFORGE_DECLARE_STRONG_STRING(EndpointRoleId);
MANYFORGE_DECLARE_STRONG_STRING(StateAuthorityId);
MANYFORGE_DECLARE_STRONG_STRING(StopEvidencePolicyId);
MANYFORGE_DECLARE_STRONG_STRING(StopEvidencePolicyRevision);
MANYFORGE_DECLARE_STRONG_STRING(StopEscalationStageId);
MANYFORGE_DECLARE_STRONG_STRING(StopEvidenceItemId);
MANYFORGE_DECLARE_STRONG_STRING(ProviderContractRoleId);
MANYFORGE_DECLARE_STRONG_STRING(RegisteredStopCommandKindId);
MANYFORGE_DECLARE_STRONG_STRING(RegisteredStopEvidenceKindId);
MANYFORGE_DECLARE_STRONG_STRING(CommissionedStopInterfaceId);
MANYFORGE_DECLARE_STRONG_STRING(CommissionedStopInterfaceRevision);
MANYFORGE_DECLARE_STRONG_STRING(StopInvocationLaneId);
MANYFORGE_DECLARE_STRONG_STRING(ContactPolicyId);
MANYFORGE_DECLARE_STRONG_STRING(ContactPolicyRevision);

MANYFORGE_DECLARE_STRONG_STRING(ProviderInstanceId);
MANYFORGE_DECLARE_STRONG_STRING(ProviderManifestId);
MANYFORGE_DECLARE_STRONG_STRING(ProviderManifestRevision);
MANYFORGE_DECLARE_STRONG_STRING(CapabilityProviderBindingId);
MANYFORGE_DECLARE_STRONG_STRING(CapabilityProviderBindingRevision);
MANYFORGE_DECLARE_STRONG_STRING(SiteOverlayId);
MANYFORGE_DECLARE_STRONG_STRING(SiteOverlayRevision);
// Deployment-local aliases and overlay references are distinct typed values.
// Their exact applicability and overlay-schema resolution remain compiler
// policy; compile-time separation prevents a runtime-unit ID or arbitrary
// identity domain from substituting for either field.
MANYFORGE_DECLARE_STRONG_STRING(ExternalResourceAlias);
MANYFORGE_DECLARE_STRONG_STRING(SiteOverlayValueRef);

MANYFORGE_DECLARE_STRONG_STRING(ComponentAttachmentId);
MANYFORGE_DECLARE_STRONG_STRING(ComponentAttachmentRevision);

MANYFORGE_DECLARE_STRONG_STRING(RetainedBundleId);
MANYFORGE_DECLARE_STRONG_STRING(SceneId);
MANYFORGE_DECLARE_STRONG_STRING(SceneResourceId);
MANYFORGE_DECLARE_STRONG_STRING(RuntimeObjectId);
MANYFORGE_DECLARE_STRONG_STRING(CarriedObjectAttachmentId);
MANYFORGE_DECLARE_STRONG_STRING(CarriedObjectAttachmentRevision);
MANYFORGE_DECLARE_STRONG_STRING(ProjectionLeaseId);

MANYFORGE_DECLARE_STRONG_STRING(RequestOwnerEpoch);
MANYFORGE_DECLARE_STRONG_STRING(RequestId);
MANYFORGE_DECLARE_STRONG_STRING(BootstrapId);
MANYFORGE_DECLARE_STRONG_STRING(ApplyId);
MANYFORGE_DECLARE_STRONG_STRING(LifecycleTransactionId);
MANYFORGE_DECLARE_STRONG_STRING(LifecycleFactId);
MANYFORGE_DECLARE_STRONG_STRING(ConfigurationFenceId);

MANYFORGE_DECLARE_STRONG_STRING(CommandSchemaId);
MANYFORGE_DECLARE_STRONG_STRING(CommandSchemaRevision);
MANYFORGE_DECLARE_STRONG_STRING(CommandOptionId);
MANYFORGE_DECLARE_STRONG_STRING(ChannelRoleId);

MANYFORGE_DECLARE_STRONG_STRING(ProgramId);
MANYFORGE_DECLARE_STRONG_STRING(ProgramRevision);
MANYFORGE_DECLARE_STRONG_STRING(ProgramBindingId);
MANYFORGE_DECLARE_STRONG_STRING(ProgramRunId);
MANYFORGE_DECLARE_STRONG_STRING(MoveId);

MANYFORGE_DECLARE_STRONG_STRING(OperationId);
MANYFORGE_DECLARE_STRONG_STRING(OperationParticipantId);
MANYFORGE_DECLARE_STRONG_STRING(ValidationRevision);
MANYFORGE_DECLARE_STRONG_STRING(ControllerGoalId);
MANYFORGE_DECLARE_STRONG_STRING(StopId);
MANYFORGE_DECLARE_STRONG_STRING(StopCertificateId);

MANYFORGE_DECLARE_STRONG_COUNTER(ProjectionStageGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(ActiveBundlePublicationGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(BootstrapGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(ApplyGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(LifecycleTransactionGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(ComponentLifecycleGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(ConfigurationFenceGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(AdmissionGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(CellAdmissionClaimGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(ActiveControlConfigurationGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(OperationRevision);
MANYFORGE_DECLARE_STRONG_COUNTER(ControllerGoalGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(StopGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(StopCertificateRevision);
MANYFORGE_DECLARE_STRONG_COUNTER(RuntimeScaleGeneration);
MANYFORGE_DECLARE_STRONG_COUNTER(RuntimeScaleControlGeneration);

#undef MANYFORGE_DECLARE_STRONG_COUNTER
#undef MANYFORGE_DECLARE_STRONG_STRING

}  // namespace manyforge::core
