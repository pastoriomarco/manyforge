#pragma once

#include <cstddef>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kContactPolicyApiVersion = "manyforge.contact_policy.v1";
inline constexpr std::size_t kMaxContactPolicyZones = 256U;
inline constexpr std::size_t kMaxContactPolicyBodyPairs = 4096U;
inline constexpr std::size_t kMaxContactPolicyDescriptionBytes = 4096U;
inline constexpr std::size_t kMaxContactZoneNotesBytes = 4096U;

enum class ContactPolicyModeDTO {
  kContactRelease = 0,
  kBoundedPenetration,
  kContactGrasp,
  kContactProcess,
  kContactHold,
};

enum class ContactPolicySensingModeDTO {
  kGeometryOnly = 0,
  kForceTorque,
  kControllerReported,
};

enum class ContactPolicyViolationResponseDTO {
  kWarn = 0,
  kSoftStop,
  kHardStop,
};

enum class ContactReleaseEvidenceModeDTO {
  kSignedPenetration = 0,
  kBinaryContact,
};

struct ContactComponentModelBodySelectorDTO final {
  ComponentRoleId component_role_id;
  std::optional<ModelRoleId> model_role_id;
  CollisionBodyRoleId collision_body_role_id;

  friend bool operator==(const ContactComponentModelBodySelectorDTO&,
                         const ContactComponentModelBodySelectorDTO&) = default;
};

struct ContactSceneResourceSelectorDTO final {
  SceneResourceId scene_resource_id;

  friend bool operator==(const ContactSceneResourceSelectorDTO&,
                         const ContactSceneResourceSelectorDTO&) = default;
};

/// Portable typed Program blackboard/runtime-object key. Admission resolves it
/// to one current SceneResourceId before a policy becomes effective.
struct ContactRuntimeObjectSelectorDTO final {
  RuntimeObjectId runtime_object_id;

  friend bool operator==(const ContactRuntimeObjectSelectorDTO&,
                         const ContactRuntimeObjectSelectorDTO&) = default;
};

/// Bodies are supplied only by the operation's exact selected motion endpoint.
struct ContactMotionEndpointSelectorDTO final {
  friend bool operator==(const ContactMotionEndpointSelectorDTO&,
                         const ContactMotionEndpointSelectorDTO&) = default;
};

using ContactBodySelectorDTO =
    std::variant<ContactComponentModelBodySelectorDTO, ContactSceneResourceSelectorDTO,
                 ContactRuntimeObjectSelectorDTO, ContactMotionEndpointSelectorDTO>;

/// One exact unordered portable body pair. Qualified values retain the two
/// selectors in canonical encoded order and never use declaration order to
/// choose a matching zone.
struct ContactBodyPairDTO final {
  ContactBodySelectorDTO body_a;
  ContactBodySelectorDTO body_b;

  friend bool operator==(const ContactBodyPairDTO&, const ContactBodyPairDTO&) = default;
};

struct ContactReleaseEnvelopeDTO final {
  ContactReleaseEvidenceModeDTO release_evidence_mode{
      ContactReleaseEvidenceModeDTO::kSignedPenetration};
  bool enforce_max_start_penetration{true};
  double max_start_penetration_m{};
  double min_release_progress_m{};
  double release_tolerance_m{1e-5};
  bool require_strictly_decreasing_penetration{true};
  bool require_separation_by_end{true};
  bool allow_recontact{};

  friend bool operator==(const ContactReleaseEnvelopeDTO&,
                         const ContactReleaseEnvelopeDTO&) = default;
};

struct ContactBoundedPenetrationEnvelopeDTO final {
  double max_contact_penetration_m{};

  friend bool operator==(const ContactBoundedPenetrationEnvelopeDTO&,
                         const ContactBoundedPenetrationEnvelopeDTO&) = default;
};

struct ContactGraspEnvelopeDTO final {
  double max_penetration_m{};
  double terminal_window_start_fraction{};

  friend bool operator==(const ContactGraspEnvelopeDTO&, const ContactGraspEnvelopeDTO&) = default;
};

using ContactZoneEnvelopeDTO =
    std::variant<ContactReleaseEnvelopeDTO, ContactBoundedPenetrationEnvelopeDTO,
                 ContactGraspEnvelopeDTO>;

struct ContactZoneDTO final {
  std::string zone_id;
  std::vector<ContactBodyPairDTO> body_pairs;
  ContactPolicyViolationResponseDTO violation_response{
      ContactPolicyViolationResponseDTO::kHardStop};
  ContactZoneEnvelopeDTO envelope;
  std::optional<std::string> notes;

  friend bool operator==(const ContactZoneDTO&, const ContactZoneDTO&) = default;
};

/// Exact build-qualified portable contact-policy catalog payload. Reserved
/// process/hold mode tokens are represented by ContactPolicyModeDTO so ingress
/// can fail with a stable semantic reason, but a QualifiedContactPolicy never
/// contains either reserved mode.
struct ContactPolicyDTO final {
  ContactPolicyId contact_policy_id;
  ContactPolicyRevision contact_policy_revision;
  std::optional<std::string> description;
  ContactPolicyModeDTO mode{ContactPolicyModeDTO::kContactRelease};
  ContactPolicySensingModeDTO sensing_mode{ContactPolicySensingModeDTO::kGeometryOnly};
  ContactPolicyViolationResponseDTO default_unmatched_response{
      ContactPolicyViolationResponseDTO::kHardStop};
  std::vector<ContactZoneDTO> zones;

  friend bool operator==(const ContactPolicyDTO&, const ContactPolicyDTO&) = default;
};

}  // namespace manyforge::core
