"""Single non-real-time recovery-offer authority for cycle/UI consumers.

The kernel remains authoritative for stop, latch, observation, and execution
facts. This module only combines its immutable assessment with cycle-owned
facts and validates generation-bound operator requests. It never infers safety
from error text and never acknowledges a kernel latch on its own.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
import json
import threading
import time
import uuid
from typing import Any, Callable, Mapping, Optional, Sequence


RECOVERY_OFFER_SCHEMA_VERSION = "manyforge.cycle_recovery_offer.v1"
_MAX_EVIDENCE_REFS = 32
_MAX_OPERATOR_REASON = 512


class CycleRecoveryClass(StrEnum):
    NONE = "none"
    RUNTIME_COLLISION = "runtime_collision"
    STOP_FAULT_INTERVENTION = "stop_fault_intervention"
    FAILED_VALIDATION = "failed_validation"
    ORDINARY_OPERATOR_STOP = "ordinary_operator_stop"
    UNKNOWN_FAULT = "unknown_fault"
    RUNTIME_DOMAIN_FAULT = "runtime_domain_fault"


class CycleRecoveryActionId(StrEnum):
    RETRY_MOTION_FROM_CURRENT_STATE = "retry_motion_from_current_state"
    ACKNOWLEDGE_AND_CONTINUE = "acknowledge_and_continue"
    RETRY_AFTER_SCENE_REPAIR = "retry_after_scene_repair"
    REPLAN_FROM_STOPPED_STATE = "replan_from_stopped_state"
    RETRY_RUNTIME_STEP = "retry_runtime_step"
    RESET_CYCLE = "reset_cycle"


@dataclass(frozen=True)
class CycleRecoveryFacts:
    cycle_state: str
    motion_stop_state: str
    has_resumable_context: bool
    cycle_error_code: Optional[str]
    active_runtime_fault_ids: tuple[str, ...]
    active_runtime_fault_domains: tuple[str, ...]
    intervention_active: bool
    direct_control_active: bool
    lifecycle_command_pending: bool
    cycle_generation: int
    program_generation: int
    deployment_generation: int
    scene_admission_epoch: int
    pose_source_apply_generation: int
    live_collision_world_revision: int


@dataclass(frozen=True)
class CycleRecoveryAction:
    action_id: str
    label_key: str
    requires_confirmation: bool
    performs_backend_reset: bool
    enabled: bool
    disabled_reason_code: Optional[str] = None

    def to_wire(self) -> dict[str, Any]:
        return {
            "actionId": self.action_id,
            "labelKey": self.label_key,
            "requiresConfirmation": self.requires_confirmation,
            "performsBackendReset": self.performs_backend_reset,
            "enabled": self.enabled,
            "disabledReasonCode": self.disabled_reason_code,
        }


@dataclass(frozen=True)
class CycleRecoveryOffer:
    offer_id: str
    offer_revision: int
    generated_at_ns: int
    valid_until_ns: Optional[int]
    kernel_assessment_revision: int
    control_command_epoch: int
    execution_id: Optional[int]
    latch_id: Optional[str]
    stop_certificate_id: Optional[str]
    stop_owner_generation: int
    cycle_generation: int
    program_generation: int
    deployment_generation: int
    scene_admission_epoch: int
    pose_source_apply_generation: int
    live_collision_world_revision: Optional[int]
    recovery_class: str
    status: str
    actions: tuple[CycleRecoveryAction, ...]
    primary_action: Optional[str]
    reason_code: str
    evidence_refs: tuple[str, ...]

    def to_wire(self) -> dict[str, Any]:
        return {
            "schemaVersion": RECOVERY_OFFER_SCHEMA_VERSION,
            "offerId": self.offer_id,
            "offerRevision": self.offer_revision,
            "generatedAtNs": self.generated_at_ns,
            "validUntilNs": self.valid_until_ns,
            "kernelAssessmentRevision": self.kernel_assessment_revision,
            "controlCommandEpoch": self.control_command_epoch,
            "executionId": self.execution_id,
            "latchId": self.latch_id,
            "stopCertificateId": self.stop_certificate_id,
            "stopOwnerGeneration": self.stop_owner_generation,
            "cycleGeneration": self.cycle_generation,
            "programGeneration": self.program_generation,
            "deploymentGeneration": self.deployment_generation,
            "sceneAdmissionEpoch": self.scene_admission_epoch,
            "poseSourceApplyGeneration": self.pose_source_apply_generation,
            "liveCollisionWorldRevision": self.live_collision_world_revision,
            "recoveryClass": self.recovery_class,
            "status": self.status,
            "actions": [action.to_wire() for action in self.actions],
            "primaryAction": self.primary_action,
            "reasonCode": self.reason_code,
            "evidenceRefs": list(self.evidence_refs),
        }


@dataclass(frozen=True)
class RecoveryRequestValidation:
    accepted: bool
    offer: Optional[CycleRecoveryOffer]
    action: Optional[CycleRecoveryAction]
    error: Optional[dict[str, Any]]


class RunRecoveryDisposition(StrEnum):
    BYPASS = "bypass"
    APPLY_PRIMARY = "apply_primary"
    EXPLICIT_ACTION_REQUIRED = "explicit_action_required"


@dataclass(frozen=True)
class RunRecoveryDirective:
    disposition: RunRecoveryDisposition
    offer: Optional[CycleRecoveryOffer]
    action_id: Optional[CycleRecoveryActionId] = None
    reason_code: str = ""
    message: str = ""


_ACTION_LABELS = {
    CycleRecoveryActionId.RETRY_MOTION_FROM_CURRENT_STATE.value: "cycle.recovery.retry_motion",
    CycleRecoveryActionId.ACKNOWLEDGE_AND_CONTINUE.value: "cycle.recovery.acknowledge_and_continue",
    CycleRecoveryActionId.RETRY_AFTER_SCENE_REPAIR.value: "cycle.recovery.retry_after_scene_repair",
    CycleRecoveryActionId.REPLAN_FROM_STOPPED_STATE.value: "cycle.recovery.replan_from_stopped_state",
    CycleRecoveryActionId.RETRY_RUNTIME_STEP.value: "cycle.recovery.retry_runtime_step",
    CycleRecoveryActionId.RESET_CYCLE.value: "cycle.recovery.reset_cycle",
}
_KERNEL_ACTIONS = frozenset(
    {
        CycleRecoveryActionId.RETRY_MOTION_FROM_CURRENT_STATE.value,
        CycleRecoveryActionId.ACKNOWLEDGE_AND_CONTINUE.value,
        CycleRecoveryActionId.RETRY_AFTER_SCENE_REPAIR.value,
        CycleRecoveryActionId.REPLAN_FROM_STOPPED_STATE.value,
    }
)
RUN_AUTOMATIC_RECOVERY_ACTIONS = frozenset(
    {
        CycleRecoveryActionId.RETRY_MOTION_FROM_CURRENT_STATE,
        CycleRecoveryActionId.RETRY_AFTER_SCENE_REPAIR,
        CycleRecoveryActionId.REPLAN_FROM_STOPPED_STATE,
        CycleRecoveryActionId.RETRY_RUNTIME_STEP,
    }
)
RUN_AUTOMATIC_RECOVERY_ACTION_IDS = frozenset(
    action.value for action in RUN_AUTOMATIC_RECOVERY_ACTIONS
)


def is_kernel_recovery_action(action_id: str) -> bool:
    return action_id in _KERNEL_ACTIONS


def _bounded_strings(values: Sequence[Any]) -> tuple[str, ...]:
    result: list[str] = []
    seen: set[str] = set()
    for raw in values:
        value = str(raw or "")[:256]
        if not value or value in seen:
            continue
        seen.add(value)
        result.append(value)
        if len(result) >= _MAX_EVIDENCE_REFS:
            break
    return tuple(result)


def _error(reason: str, message: str, **details: Any) -> dict[str, Any]:
    return {
        "code": "recovery_action_rejected",
        "message": message,
        "details": {"reason": reason, **details},
    }


class CycleRecoveryCoordinator:
    """Projects one immutable offer and validates exact offer-bound actions."""

    def __init__(
        self,
        *,
        on_offer_published: Optional[Callable[[dict[str, Any]], None]] = None,
    ) -> None:
        self._lock = threading.Lock()
        self._offer: Optional[CycleRecoveryOffer] = None
        self._semantic_key: Optional[str] = None
        self._next_offer_revision = 0
        self._on_offer_published = on_offer_published

    def set_on_offer_published(
        self,
        callback: Optional[Callable[[dict[str, Any]], None]],
    ) -> None:
        with self._lock:
            self._on_offer_published = callback

    def current_offer(self) -> Optional[CycleRecoveryOffer]:
        with self._lock:
            return self._offer

    def current_offer_wire(self) -> Optional[dict[str, Any]]:
        offer = self.current_offer()
        return offer.to_wire() if offer is not None else None

    def resolve_run_request(self) -> RunRecoveryDirective:
        """Resolve Run against the currently published immutable offer.

        Run is allowed to consume only retry-ready, non-confirming primary
        actions. Every other offer remains an explicit operator choice. This is
        NRT orchestration policy; kernel retry admission remains authoritative.
        """
        offer = self.current_offer()
        if offer is None:
            return RunRecoveryDirective(
                disposition=RunRecoveryDisposition.BYPASS,
                offer=None,
            )
        try:
            action_id = CycleRecoveryActionId(str(offer.primary_action or ""))
        except ValueError:
            action_id = None
        action = next(
            (
                candidate
                for candidate in offer.actions
                if candidate.action_id == str(offer.primary_action or "")
            ),
            None,
        )
        if (
            offer.status == "retry_ready"
            and action_id in RUN_AUTOMATIC_RECOVERY_ACTIONS
            and action is not None
            and action.enabled
            and not action.requires_confirmation
        ):
            return RunRecoveryDirective(
                disposition=RunRecoveryDisposition.APPLY_PRIMARY,
                offer=offer,
                action_id=action_id,
                reason_code="operator_run_primary_recovery",
            )
        return RunRecoveryDirective(
            disposition=RunRecoveryDisposition.EXPLICIT_ACTION_REQUIRED,
            offer=offer,
            reason_code=offer.reason_code,
            message=(
                "Run cannot choose the current recovery action. "
                f"status={offer.status}, reason={offer.reason_code}"
            ),
        )

    def project(
        self,
        assessment: Mapping[str, Any],
        facts: CycleRecoveryFacts,
    ) -> Optional[CycleRecoveryOffer]:
        candidate = self._build_candidate(assessment, facts)
        callback: Optional[Callable[[dict[str, Any]], None]] = None
        published: Optional[CycleRecoveryOffer] = None
        with self._lock:
            if candidate is None:
                if self._offer is None:
                    return None
                self._offer = None
                self._semantic_key = None
                return None
            semantic_key = json.dumps(
                candidate,
                sort_keys=True,
                separators=(",", ":"),
                default=lambda value: (
                    value.to_wire()
                    if isinstance(value, CycleRecoveryAction)
                    else str(value)
                ),
            )
            if semantic_key == self._semantic_key and self._offer is not None:
                return self._offer
            self._next_offer_revision += 1
            now_ns = time.monotonic_ns()
            published = CycleRecoveryOffer(
                offer_id=f"recovery-{self._next_offer_revision}-{uuid.uuid4().hex[:12]}",
                offer_revision=self._next_offer_revision,
                generated_at_ns=now_ns,
                valid_until_ns=candidate["valid_until_ns"],
                kernel_assessment_revision=candidate["kernel_assessment_revision"],
                control_command_epoch=candidate["control_command_epoch"],
                execution_id=candidate["execution_id"],
                latch_id=candidate["latch_id"],
                stop_certificate_id=candidate["stop_certificate_id"],
                stop_owner_generation=candidate["stop_owner_generation"],
                cycle_generation=facts.cycle_generation,
                program_generation=facts.program_generation,
                deployment_generation=facts.deployment_generation,
                scene_admission_epoch=facts.scene_admission_epoch,
                pose_source_apply_generation=facts.pose_source_apply_generation,
                live_collision_world_revision=(
                    facts.live_collision_world_revision
                    if facts.live_collision_world_revision > 0
                    else None
                ),
                recovery_class=candidate["recovery_class"],
                status=candidate["status"],
                actions=tuple(candidate["actions"]),
                primary_action=candidate["primary_action"],
                reason_code=candidate["reason_code"],
                evidence_refs=candidate["evidence_refs"],
            )
            self._offer = published
            self._semantic_key = semantic_key
            callback = self._on_offer_published
        if callback is not None and published is not None:
            callback(published.to_wire())
        return published

    def validate_request(
        self,
        request: Mapping[str, Any],
        current_offer: Optional[CycleRecoveryOffer],
    ) -> RecoveryRequestValidation:
        request_id = str(request.get("request_id") or request.get("requestId") or "")
        offer_id = str(request.get("offer_id") or request.get("offerId") or "")
        raw_revision = request.get("offer_revision", request.get("offerRevision", 0))
        action_id = str(request.get("action_id") or request.get("actionId") or "")
        try:
            offer_revision = int(raw_revision or 0)
        except (TypeError, ValueError):
            offer_revision = 0
        if not request_id:
            return RecoveryRequestValidation(
                False,
                current_offer,
                None,
                _error("request_id_missing", "Recovery request id is required"),
            )
        if current_offer is None:
            return RecoveryRequestValidation(
                False,
                None,
                None,
                _error("recovery_offer_unavailable", "No recovery offer is active"),
            )
        if (
            offer_id != current_offer.offer_id
            or offer_revision != current_offer.offer_revision
        ):
            return RecoveryRequestValidation(
                False,
                current_offer,
                None,
                _error(
                    "recovery_offer_stale",
                    "The recovery offer changed; review the current action",
                    current_offer_id=current_offer.offer_id,
                    current_offer_revision=current_offer.offer_revision,
                ),
            )
        action = next(
            (item for item in current_offer.actions if item.action_id == action_id),
            None,
        )
        if action is None:
            return RecoveryRequestValidation(
                False,
                current_offer,
                None,
                _error("action_not_offered", "Recovery action is not in the current offer"),
            )
        if not action.enabled:
            return RecoveryRequestValidation(
                False,
                current_offer,
                action,
                _error(
                    action.disabled_reason_code or "action_not_enabled",
                    "Recovery action is not currently enabled",
                ),
            )
        return RecoveryRequestValidation(True, current_offer, action, None)

    @staticmethod
    def bounded_operator_reason(raw: Any) -> str:
        return str(raw or "")[:_MAX_OPERATOR_REASON]

    def _build_candidate(
        self,
        assessment: Mapping[str, Any],
        facts: CycleRecoveryFacts,
    ) -> Optional[dict[str, Any]]:
        execution_state = str(assessment.get("execution_state") or "idle")
        assessment_status = str(assessment.get("status") or "unavailable")
        recovery_class = str(
            assessment.get("recovery_class") or CycleRecoveryClass.NONE.value
        )
        runtime_fault_active = bool(facts.active_runtime_fault_ids)
        cycle_fault_active = bool(facts.cycle_error_code)

        # A plain cooperative Pause remains the bounded lifecycle path. A held
        # runtime fault is not a plain pause and therefore still gets an offer.
        if (
            facts.cycle_state == "stopped"
            and execution_state == "paused"
            and not runtime_fault_active
        ):
            return None

        relevant_cycle_state = facts.cycle_state == "stopped" or (
            facts.motion_stop_state == "held" or runtime_fault_active
        )
        kernel_recovery_active = (
            recovery_class != CycleRecoveryClass.NONE.value
            or assessment_status != "unavailable"
        )
        if not relevant_cycle_state and not kernel_recovery_active:
            return None
        if not kernel_recovery_active and not runtime_fault_active and not cycle_fault_active:
            return None

        if cycle_fault_active and not kernel_recovery_active and not runtime_fault_active:
            recovery_class = CycleRecoveryClass.UNKNOWN_FAULT.value
            allowed = [CycleRecoveryActionId.RESET_CYCLE.value]
            status = "reset_required"
            reason_code = f"cycle_error_requires_reset:{facts.cycle_error_code}"
        else:
            allowed = [str(item) for item in assessment.get("allowed_actions") or []]
            reason_code = str(assessment.get("reason_code") or "recovery_unavailable")
            status = assessment_status

        # Cycle-only runtime faults are safe to retry only when the kernel says
        # no recovery authority is active and its immutable state is quiescent.
        if runtime_fault_active and not kernel_recovery_active:
            recovery_class = CycleRecoveryClass.RUNTIME_DOMAIN_FAULT.value
            if execution_state in {"idle", "completed"}:
                allowed = [
                    CycleRecoveryActionId.RETRY_RUNTIME_STEP.value,
                    CycleRecoveryActionId.RESET_CYCLE.value,
                ]
                status = "retry_ready"
                reason_code = "runtime_step_retry_ready"
            else:
                allowed = [CycleRecoveryActionId.RESET_CYCLE.value]
                status = "waiting"
                reason_code = "kernel_not_quiescent_for_runtime_step_retry"

        blocking_reason: Optional[str] = None
        if facts.lifecycle_command_pending:
            blocking_reason = "cycle_lifecycle_command_pending"
        elif facts.direct_control_active or facts.intervention_active:
            blocking_reason = "stronger_cycle_authority_active"
        elif facts.scene_admission_epoch <= 0:
            blocking_reason = "scene_admission_authority_unavailable"
        elif facts.program_generation <= 0 or facts.deployment_generation <= 0:
            blocking_reason = "runtime_generation_unavailable"

        evidence_expired = False
        valid_until_raw = assessment.get("observed_state_valid_until_ns")
        try:
            valid_until = int(valid_until_raw or 0)
        except (TypeError, ValueError):
            valid_until = 0
        if (
            kernel_recovery_active
            and valid_until > 0
            and time.monotonic_ns() >= valid_until
        ):
            evidence_expired = True

        if (
            facts.cycle_state == "stopped"
            and CycleRecoveryActionId.RESET_CYCLE.value not in allowed
        ):
            allowed.append(CycleRecoveryActionId.RESET_CYCLE.value)

        actions: list[CycleRecoveryAction] = []
        for action_id in allowed:
            if action_id not in _ACTION_LABELS:
                continue
            is_reset = action_id == CycleRecoveryActionId.RESET_CYCLE.value
            if is_reset:
                # Reset is the fail-safe escape from direct control and an
                # active intervention. It is blocked only while another
                # lifecycle transition already owns the serialized lane.
                disabled_reason = (
                    "cycle_lifecycle_command_pending"
                    if facts.lifecycle_command_pending
                    else None
                )
            else:
                disabled_reason = blocking_reason
            if not is_reset and disabled_reason is None and evidence_expired:
                disabled_reason = "recovery_evidence_expired"
            if (
                disabled_reason is None
                and not is_reset
                and not (
                    facts.has_resumable_context
                    or facts.motion_stop_state == "held"
                )
            ):
                disabled_reason = "cycle_context_not_resumable"
            enabled = disabled_reason is None
            actions.append(
                CycleRecoveryAction(
                    action_id=action_id,
                    label_key=_ACTION_LABELS[action_id],
                    requires_confirmation=(
                        action_id
                        in {
                            CycleRecoveryActionId.ACKNOWLEDGE_AND_CONTINUE.value,
                            CycleRecoveryActionId.RESET_CYCLE.value,
                        }
                    ),
                    performs_backend_reset=is_reset,
                    enabled=enabled,
                    disabled_reason_code=disabled_reason,
                )
            )

        if blocking_reason is not None:
            status = "waiting"
            reason_code = blocking_reason
        elif evidence_expired:
            status = "waiting"
            reason_code = "recovery_evidence_expired"

        primary_action = next(
            (
                action.action_id
                for action in actions
                if action.enabled
                and action.action_id != CycleRecoveryActionId.RESET_CYCLE.value
            ),
            None,
        )
        if primary_action is None and status == "reset_required":
            primary_action = next(
                (action.action_id for action in actions if action.enabled),
                None,
            )

        evidence_refs = _bounded_strings(
            [
                *(assessment.get("evidence_refs") or []),
                *(f"runtime_fault:{fault_id}" for fault_id in facts.active_runtime_fault_ids),
                *(
                    [f"cycle_error:{facts.cycle_error_code}"]
                    if facts.cycle_error_code
                    else []
                ),
            ]
        )
        def optional_string(key: str) -> Optional[str]:
            value = str(assessment.get(key) or "")
            return value or None

        execution_id_raw = assessment.get("execution_id")
        try:
            execution_id = int(execution_id_raw or 0)
        except (TypeError, ValueError):
            execution_id = 0

        return {
            "kernel_assessment_revision": int(assessment.get("assessment_revision") or 0),
            "control_command_epoch": int(assessment.get("control_command_epoch") or 0),
            "execution_id": execution_id or None,
            "latch_id": optional_string("latch_id"),
            "stop_certificate_id": optional_string("stop_certificate_id"),
            "stop_owner_generation": int(assessment.get("stop_owner_generation") or 0),
            "valid_until_ns": valid_until or None,
            "recovery_class": recovery_class,
            "status": status,
            "actions": actions,
            "primary_action": primary_action,
            "reason_code": reason_code,
            "evidence_refs": evidence_refs,
            # Facts belong in the semantic key so any stale-action token change
            # publishes a distinct immutable offer.
            "cycle_generation": facts.cycle_generation,
            "program_generation": facts.program_generation,
            "deployment_generation": facts.deployment_generation,
            "scene_admission_epoch": facts.scene_admission_epoch,
            "pose_source_apply_generation": facts.pose_source_apply_generation,
            "cycle_error_code": facts.cycle_error_code,
        }
