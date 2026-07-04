"""Typed constants for pick-plan candidate stage diagnostics.

These stay deliberately small until K12 folds them into the shared reason-code
namespace. Keeping them typed now prevents B2/B3 diagnostics from spreading
ad-hoc string literals across the behavior and UI layers.
"""

from __future__ import annotations

from enum import StrEnum


class PickStageFailureCause(StrEnum):
    RAW_IK_FAILURE = "raw_ik_failure"
    COLLISION_AWARE_IK_FAILURE = "collision_aware_ik_failure"
    COLLISION_VALIDATION_FAILURE = "collision_validation_failure"
    SUPPORT_CONTACT_REJECTION = "support_contact_rejection"
    CONTACT_POLICY_REJECTION = "contact_policy_rejection"
    START_STATE_IN_COLLISION = "start_state_in_collision"
    PREVIEW_FAILED = "preview_failed"

