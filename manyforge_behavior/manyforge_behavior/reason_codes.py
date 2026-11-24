"""Shared diagnostic reason-code vocabulary.

These values mirror the C++ reason-code constants used by the kernel. They are
serialized into events, failure archives, incident chains, and UI/MCP payloads;
changing a value is a contract change.
"""

from __future__ import annotations

from enum import StrEnum


class ReasonCode(StrEnum):
    PREVIEW_FAILED = "preview_failed"

    MISSING_GRASP_CONTACT_CLEARANCE = "missing_grasp_contact_clearance"
    CONTACT_PENETRATION_EXCEEDED = "contact_penetration_exceeded"
    MISSING_SIGNED_CLEARANCE = "missing_signed_clearance"
    INITIAL_SAMPLE_IN_CONTACT = "initial_sample_in_contact"
    CONTACT_BEFORE_TERMINAL_WINDOW = "contact_before_terminal_window"

    COLLISION_VALIDATION_SERIAL_FALLBACK_EXCEPTION = (
        "collision_validation_serial_fallback_exception"
    )
    COLLISION_VALIDATION_WORKER_EXCEPTION = "collision_validation_worker_exception"
    EVENT_SUBSCRIBER_EXCEPTION = "event_subscriber_exception"
    GRIPPER_STATE_UNAVAILABLE = "gripper_state_unavailable"
    UNKNOWN_NON_STD_EXCEPTION = "unknown_non_std_exception"
    TRACKING_SAMPLE_INVALID = "tracking_sample_invalid"
