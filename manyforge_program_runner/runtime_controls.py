"""Bounded C2 recovery/value state attached to existing Program runs.

This module deliberately owns no scheduler, Kernel effect, artifact, or
browser state.  It records exact run-scoped intents and delegates validation
and effects to the already-existing Program-run owner.
"""

from __future__ import annotations

from copy import deepcopy
import hashlib
from threading import RLock
import time
from typing import Any, Final, Mapping
import unicodedata

import rfc8785

from manyforge_behavior.failure_details import failure_wire_fields
from manyforge_behavior.json_scalars import nonnegative_int64_value
from manyforge_behavior.recovery import (
    OWNER_RECOVERY_ACTIONS,
    is_non_moving_recovery,
    recovery_retry_permitted,
)


RUNTIME_CONTROL_SCHEMA: Final = "manyforge.program_runtime_controls.v3"
RECOVERY_OFFER_SCHEMA: Final = "manyforge.cycle_recovery_offer.v5"
_MAX_AUDIT: Final = 256
_MAX_REVISION: Final = 9_007_199_254_740_991
# One entry per captured Program node, never per request/attempt.
_MAX_RETRY_NODES: Final = 100_000
# Accumulated explicit failure retries of one node: an exact JSON-safe
# integer, independent of the authored finite budget. Reaching it is an
# explicit refusal, never wraparound or counter eviction.
_MAX_RETRY_COUNT: Final = _MAX_REVISION
_RECOVERY_FENCE_FIELDS: Final = (
    "program_generation",
    "deployment_generation",
    "scene_admission_epoch",
    "pose_source_apply_generation",
)
_SNAPSHOT_FIELDS: Final = frozenset(
    {
        "schema_version",
        "program_run_id",
        "runtime_control_revision",
        "recovery_fences",
        "retry_counts",
        "audit",
    }
)


class ProgramRuntimeControlError(RuntimeError):
    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


def _project_wire_value(value: Any) -> Any:
    """Project immutable owner facts into ordinary JSON-compatible values."""

    if isinstance(value, Mapping):
        return {key: _project_wire_value(nested) for key, nested in value.items()}
    if isinstance(value, (list, tuple)):
        return [_project_wire_value(nested) for nested in value]
    return deepcopy(value)


def _digest(domain: str, value: Mapping[str, Any]) -> str:
    payload = domain.encode("utf-8") + b"\0" + rfc8785.dumps(dict(value))
    return "sha256:" + hashlib.sha256(payload).hexdigest()


def _bounded_text(value: object) -> bool:
    return (
        isinstance(value, str)
        and bool(value)
        and unicodedata.normalize("NFC", value) == value
        and len(value.encode("utf-8")) <= 256
    )


def manual_result_availability(
    intervention,
    policy: Mapping[str, Any],
) -> tuple[bool, str | None]:
    """Return the one backend-owned admission fact used by API and GUI."""

    if policy.get("allow_intervention") is not True:
        return False, "manual_result_forbidden_by_captured_policy"
    if intervention.owner_task_id is None:
        return True, None
    owner = intervention.owner_assessment
    if (
        not isinstance(owner, Mapping)
        or owner.get("acknowledged_non_moving") is not True
    ):
        return False, "manual_result_owner_not_acknowledged"
    return True, None


class ProgramRuntimeControls:
    """One lock-protected control ledger for one exact captured run."""

    def __init__(
        self,
        capture: Mapping[str, Any],
        *,
        recovery_fences: Mapping[str, Any] | None = None,
    ) -> None:
        required = {
            "program_run_id",
            "program_id",
            "program_revision",
            "program_binding_id",
            "retained_bundle_id",
            "graph_revision",
            "node_environment_digest",
        }
        if not isinstance(capture, Mapping) or not required <= set(capture):
            raise ValueError("runtime controls require one exact Program capture")
        self._capture = deepcopy(dict(capture))
        fences = (
            {field: 0 for field in _RECOVERY_FENCE_FIELDS}
            if recovery_fences is None
            else dict(recovery_fences)
        )
        if set(fences) != set(_RECOVERY_FENCE_FIELDS) or any(
            type(fences[field]) is not int
            or not 0 <= fences[field] <= 9_007_199_254_740_991
            for field in _RECOVERY_FENCE_FIELDS
        ):
            raise ValueError("runtime controls require exact recovery fences")
        self._recovery_fences = deepcopy(fences)
        self._revision = 1
        self._audit: list[dict[str, Any]] = []
        self._retry_counts: dict[str, int] = {}
        self._offer_revision = 0
        # The current offer of each held node: (semantic key, offer).
        self._offers: dict[str, tuple[str, dict[str, Any]]] = {}
        self._lock = RLock()

    @property
    def program_run_id(self) -> str:
        return self._capture["program_run_id"]

    @property
    def revision(self) -> int:
        """Read the owner change key without copying retry or audit data."""
        with self._lock:
            return self._revision

    @classmethod
    def restore(
        cls,
        capture: Mapping[str, Any],
        snapshot: Mapping[str, Any],
    ) -> "ProgramRuntimeControls":
        """Restore the latest journaled control state for one retained run."""

        if (
            not isinstance(snapshot, Mapping)
            or snapshot.get("schema_version") != RUNTIME_CONTROL_SCHEMA
        ):
            raise ValueError("retained runtime controls have an unsupported schema")
        if set(snapshot) != _SNAPSHOT_FIELDS:
            raise ValueError("retained runtime controls have an invalid shape")
        fences = snapshot.get("recovery_fences")
        instance = cls(capture, recovery_fences=fences)
        revision = snapshot.get("runtime_control_revision")
        retry_counts = snapshot.get("retry_counts")
        audit = snapshot.get("audit")
        if (
            type(revision) is not int
            or not 1 <= revision <= _MAX_REVISION
            or snapshot.get("program_run_id") != instance.program_run_id
            or not isinstance(retry_counts, dict)
            or len(retry_counts) > _MAX_RETRY_NODES
            or not isinstance(audit, list)
            or len(audit) > _MAX_AUDIT
        ):
            raise ValueError("retained runtime controls exceed their bounds")
        restored_retries: dict[str, int] = {}
        for node_instance_id, count in retry_counts.items():
            if (
                not _bounded_text(node_instance_id)
                or type(count) is not int
                or not 1 <= count <= _MAX_RETRY_COUNT
            ):
                raise ValueError("retained runtime retry count is invalid")
            restored_retries[node_instance_id] = count
        if len(audit) != min(revision - 1, _MAX_AUDIT):
            raise ValueError("retained runtime-control revision is divergent")
        for index, event in enumerate(audit, start=revision - len(audit)):
            if (
                not isinstance(event, dict)
                or type(event.get("event_revision")) is not int
                or event.get("event_revision") != index
                or not isinstance(event.get("kind"), str)
                or not event["kind"]
            ):
                raise ValueError("retained runtime-control audit is invalid")
        instance._revision = revision
        instance._retry_counts = restored_retries
        instance._audit = deepcopy(audit)
        return instance

    def _event(self, kind: str, *, reserved_after: int = 1, **facts: Any) -> None:
        self.require_event_revisions(reserved_after=reserved_after)
        self._audit.append(
            {
                "event_revision": self._revision,
                "kind": kind,
                **deepcopy(facts),
            }
        )
        del self._audit[:-_MAX_AUDIT]
        self._revision += 1

    def require_event_revisions(
        self, *, events: int = 1, reserved_after: int = 1
    ) -> None:
        """Check exact revision arithmetic before effect, not lifetime history size."""

        if type(events) is not int or events < 1:
            raise ValueError("runtime-control audit event count is invalid")
        if type(reserved_after) is not int or not 0 <= reserved_after <= 1:
            raise ValueError("runtime-control audit reservation is invalid")
        with self._lock:
            if self._revision + events + reserved_after > _MAX_REVISION:
                raise ProgramRuntimeControlError(
                    "program_runtime_control_revision_exhausted",
                    "runtime-control revision cannot advance exactly",
                )

    def mark_reset(
        self,
        *,
        request_id: str | None = None,
        correlation_id: str | None = None,
        source_label: str | None = None,
    ) -> None:
        with self._lock:
            # A fresh transport request can resume the same owner Reset after
            # a phase-write failure. It must not consume another revision or
            # replace the original accepted transition's attribution.
            recorded = bool(
                self._audit
                and self._audit[-1]["kind"] == "program_runtime_controls_reset"
            )
            if request_id is None or recorded:
                return
            self.require_event_revisions(reserved_after=0)
            self._event(
                "program_runtime_controls_reset",
                reserved_after=0,
                request_id=request_id,
                correlation_id=correlation_id,
                source_label=source_label,
                action_id="reset_cycle",
            )

    def _retry_counter_available(self, node_instance_id: str) -> bool:
        return self._retry_counts.get(node_instance_id, 0) < _MAX_RETRY_COUNT and (
            node_instance_id in self._retry_counts
            or len(self._retry_counts) < _MAX_RETRY_NODES
        )

    def retry_permitted(
        self, node_instance_id: str, recovery_policy: Mapping[str, Any]
    ) -> bool:
        """The shared failure-retry decision for offers and command checks.

        The captured node's policy predicate (independent of any caller) and
        the exact counter's representable capacity; owner eligibility and
        observation freshness are separate conditions of each action.
        """

        with self._lock:
            if not _bounded_text(node_instance_id):
                raise ValueError("retry requires one exact captured node identity")
            return recovery_retry_permitted(
                recovery_policy,
                retries_used=self._retry_counts.get(node_instance_id, 0),
            ) and self._retry_counter_available(node_instance_id)

    def require_retry_capacity(
        self, node_instance_id: str, recovery_policy: Mapping[str, Any]
    ) -> None:
        """Revalidate one failure Retry against the captured policy before effects."""

        with self._lock:
            if self.retry_permitted(node_instance_id, recovery_policy):
                return
            if recovery_policy.get("allow_intervention") is not True:
                raise ProgramRuntimeControlError(
                    "program_runtime_retry_forbidden_by_policy",
                    "the captured node policy does not allow intervention",
                )
            if self._retry_counter_available(node_instance_id):
                raise ProgramRuntimeControlError(
                    "program_runtime_retry_limit_reached",
                    "the captured node's retry limit is reached",
                )
            raise ProgramRuntimeControlError(
                "program_runtime_retry_count_exhausted",
                "the retained retry count cannot advance exactly",
            )

    def note_retry(self, node_instance_id: str) -> int:
        """Count one accepted explicit failure Retry of the captured node."""

        with self._lock:
            if not _bounded_text(node_instance_id):
                raise ValueError("retry requires one exact captured node identity")
            if not self._retry_counter_available(node_instance_id):
                raise ProgramRuntimeControlError(
                    "program_runtime_retry_count_exhausted",
                    "the retained retry count cannot advance exactly",
                )
            self.require_event_revisions()
            count = self._retry_counts.get(node_instance_id, 0) + 1
            self._retry_counts[node_instance_id] = count
            self._event(
                "program_node_retry",
                node_instance_id=node_instance_id,
                retry_count=count,
            )
            return count

    def retry_count(self, node_instance_id: str) -> int:
        with self._lock:
            return self._retry_counts.get(node_instance_id, 0)

    def audit_value(self, kind: str, **facts: Any) -> None:
        with self._lock:
            self._event(kind, **facts)

    def recovery_offer(
        self,
        *,
        cycle_generation: int,
        owner_epoch: str,
        intervention,
        policy: Mapping[str, Any],
        selection_revision: int | None = None,
        containment_pending: bool = False,
    ) -> dict[str, Any] | None:
        """``containment_pending``: a failure pause has not yet confirmed that
        the Program's other motion stopped; Retry waits for it."""
        if intervention is None:
            return None
        identity = intervention.identity
        owner = (
            _project_wire_value(intervention.owner_assessment)
            if intervention.owner_assessment is not None
            else None
        )
        projected_policy = _project_wire_value(policy)
        # The held node's own failure, carried unchanged: several nodes can be
        # held at once, so each offer names its own. Details and origin are
        # null for an operator Stop; none of it ever chooses an action.
        failure = {
            "reason_code": intervention.reason_code,
            "message": intervention.message,
            **failure_wire_fields(intervention.details, intervention.origin),
        }
        owner_actions = (
            owner.get("allowed_actions", []) if isinstance(owner, dict) else []
        )
        if (
            intervention.policy_parameter == "operator_stop_policy"
            and isinstance(owner, dict)
            and owner.get("recovery_class") == "none"
            and owner.get("status") == "unavailable"
        ):
            # Program scheduling can retain its prior intervention for one
            # tick after the addressed owner releases an ordinary Stop. That
            # stale internal row is not another operator recovery offer.
            return None
        # Offers do not expire: freshness is checked when an action is
        # pressed (the owner re-reads its assessment and the Kernel re-checks
        # the start state), which refuses a stale press with a typed reason.
        operator_stop_recovery = intervention.policy_parameter == "operator_stop_policy"
        if operator_stop_recovery:
            manual_result_enabled = False
            manual_result_disabled_reason = "operator_stop_requires_owner_recovery"
        else:
            manual_result_enabled, manual_result_disabled_reason = (
                manual_result_availability(intervention, policy)
            )
        intervention_allowed = policy.get("allow_intervention") is True
        retries_used = self.retry_count(identity.node_instance_id)
        try:
            retry_available = self.retry_permitted(identity.node_instance_id, policy)
        except ValueError:
            return None
        actions: list[dict[str, Any]] = []
        for action_id in owner_actions:
            if action_id not in OWNER_RECOVERY_ACTIONS:
                continue
            if action_id == "acknowledge_and_continue":
                enabled_by_policy = intervention_allowed
            else:
                enabled_by_policy = operator_stop_recovery or retry_available
            enabled = enabled_by_policy
            actions.append(
                {
                    "action_id": action_id,
                    "requires_confirmation": action_id == "acknowledge_and_continue",
                    "performs_backend_reset": False,
                    "enabled": enabled,
                    "disabled_reason_code": (
                        None
                        if enabled
                        else (
                            "recovery_retry_limit_reached"
                            if intervention_allowed
                            else "recovery_action_forbidden_by_captured_policy"
                        )
                    ),
                }
            )
        if (
            retry_available
            and (
                intervention.owner_task_id is None
                or (
                    isinstance(owner, dict)
                    and owner.get("acknowledged_non_moving") is True
                )
            )
        ):
            actions.append(
                {
                    "action_id": "retry_runtime_step",
                    "requires_confirmation": False,
                    "performs_backend_reset": False,
                    "enabled": True,
                    "disabled_reason_code": None,
                }
            )
        if not operator_stop_recovery:
            # Continue: close the held operation, the node returns FAILURE to
            # the tree, and the hold's groups resume (contract item 40).
            closable = intervention.owner_task_id is None or (
                isinstance(owner, dict)
                and (
                    owner.get("eligible_close") is True
                    or owner.get("acknowledged_non_moving") is True
                )
            )
            continue_enabled = intervention_allowed and closable
            actions.append(
                {
                    "action_id": "continue_with_failure",
                    "requires_confirmation": True,
                    "performs_backend_reset": False,
                    "enabled": continue_enabled,
                    "disabled_reason_code": (
                        None
                        if continue_enabled
                        else (
                            "recovery_action_forbidden_by_captured_policy"
                            if not intervention_allowed
                            else "recovery_close_not_eligible"
                        )
                    ),
                }
            )
        operator_stop_settled = (
            isinstance(owner, dict)
            and owner.get("recovery_class") == "ordinary_operator_stop"
            and owner.get("status") in {"retry_ready", "reset_required"}
            and owner.get("execution_state") == "Stopped"
            and owner.get("backend_stop_confirmed") is True
            and isinstance(owner.get("stop_certificate_id"), str)
            and bool(owner["stop_certificate_id"])
            and type(owner.get("stop_certificate_revision")) is int
            and owner["stop_certificate_revision"] > 0
            and type(owner.get("stop_owner_generation")) is int
            and owner["stop_owner_generation"] > 0
            and "reset_cycle" in owner_actions
        )
        held_reset_enabled = (
            operator_stop_recovery
            and operator_stop_settled
            and owner.get("retry_admissible") is True
        )
        actions.append(
            {
                "action_id": "reset_cycle",
                "requires_confirmation": True,
                "performs_backend_reset": True,
                "enabled": held_reset_enabled,
                "disabled_reason_code": (
                    None
                    if held_reset_enabled
                    else (
                        "operator_stop_not_physically_settled"
                        if operator_stop_recovery
                        else "cycle_not_terminal"
                    )
                ),
            }
        )
        if containment_pending and not operator_stop_recovery:
            for action in actions:
                if not is_non_moving_recovery(action["action_id"]):
                    action["enabled"] = False
                    action["disabled_reason_code"] = (
                        "failure_pause_containment_unconfirmed"
                    )
        material = {
            "cycle_generation": cycle_generation,
            "program_run_id": identity.program_run_id,
            "node_instance_id": identity.node_instance_id,
            "node_attempt_id": identity.node_attempt_id,
            "owner_epoch": owner_epoch,
            # The owner's observation deadline is Kernel freshness, not offer
            # semantics: a newer equivalent sample keeps the same offer (and
            # its command fence).
            "owner_assessment": (
                {
                    key: value
                    for key, value in owner.items()
                    if key != "observed_state_valid_until_ns"
                }
                if isinstance(owner, dict)
                else owner
            ),
            "actions": actions,
            "manual_result": {
                "enabled": manual_result_enabled,
                "disabled_reason_code": manual_result_disabled_reason,
            },
            "policy": projected_policy,
            "retries_used": retries_used,
            "recovery_fences": deepcopy(self._recovery_fences),
            **deepcopy(failure),
        }
        if selection_revision is not None:
            # The opaque offer also fences a newer selection Stop even when
            # it reuses the same physical Stop certificate.
            material["selection_revision"] = selection_revision
        offer_key = _digest("manyforge.cycle_recovery_offer.semantic.v4", material)
        with self._lock:
            cached = self._offers.get(identity.node_instance_id)
            if cached is not None and cached[0] == offer_key:
                return deepcopy(cached[1])
            if self._offer_revision >= _MAX_REVISION:
                raise ProgramRuntimeControlError(
                    "program_recovery_offer_revision_exhausted",
                    "recovery-offer revision cannot advance exactly",
                )
            self._offer_revision += 1
            result = {
                "schema_version": RECOVERY_OFFER_SCHEMA,
                "offer_id": _digest(RECOVERY_OFFER_SCHEMA, material),
                "offer_revision": self._offer_revision,
                "generated_at_ns": nonnegative_int64_value(time.monotonic_ns()),
                "kernel_assessment_revision": (
                    owner.get("assessment_revision", 0)
                    if isinstance(owner, dict)
                    else 0
                ),
                "control_command_epoch": (
                    owner.get("control_command_epoch", 0)
                    if isinstance(owner, dict)
                    else 0
                ),
                "operation_id": intervention.owner_task_id,
                "operation_revision": (
                    owner.get("operation_revision") if isinstance(owner, dict) else None
                ),
                "program_run_id": identity.program_run_id,
                "program_binding_id": self._capture["program_binding_id"],
                "node_instance_id": identity.node_instance_id,
                "node_attempt_id": identity.node_attempt_id,
                "participants": (
                    owner.get("participants", []) if isinstance(owner, dict) else []
                ),
                "graph_revision": self._capture["graph_revision"],
                "retained_bundle_id": self._capture["retained_bundle_id"],
                "cycle_generation": cycle_generation,
                **self._recovery_fences,
                "stop_id": (
                    owner.get("stop_id")
                    if isinstance(owner, dict)
                    and isinstance(owner.get("stop_id"), str)
                    and bool(owner["stop_id"])
                    else None
                ),
                "stop_generation": (
                    owner.get("stop_generation")
                    if isinstance(owner, dict)
                    and type(owner.get("stop_generation")) is int
                    and owner["stop_generation"] > 0
                    else None
                ),
                "stop_certificate_id": (
                    owner.get("stop_certificate_id")
                    if isinstance(owner, dict) and owner.get("stop_certificate_id")
                    else None
                ),
                "stop_certificate_revision": (
                    owner.get("stop_certificate_revision")
                    if isinstance(owner, dict)
                    and type(owner.get("stop_certificate_revision")) is int
                    and owner["stop_certificate_revision"] > 0
                    else None
                ),
                "status": (
                    "retry_ready"
                    if isinstance(owner, dict)
                    and owner.get("acknowledged_non_moving") is True
                    else (
                        owner.get("status", "action_required")
                        if isinstance(owner, dict)
                        else "action_required"
                    )
                ),
                "recovery_class": (
                    owner.get("recovery_class", "unknown_fault")
                    if isinstance(owner, dict)
                    else "unknown_fault"
                ),
                "actions": actions,
                "manual_result": {
                    "enabled": manual_result_enabled,
                    "disabled_reason_code": manual_result_disabled_reason,
                },
                **failure,
                "evidence_refs": (
                    owner.get("evidence_refs", []) if isinstance(owner, dict) else []
                ),
                "policy": deepcopy(projected_policy),
                "retries_used": retries_used,
            }
            # Bounded by the captured nodes: one current offer per held node.
            self._offers[identity.node_instance_id] = (offer_key, deepcopy(result))
            return deepcopy(result)

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "schema_version": RUNTIME_CONTROL_SCHEMA,
                "program_run_id": self.program_run_id,
                "runtime_control_revision": self._revision,
                "recovery_fences": deepcopy(self._recovery_fences),
                "retry_counts": deepcopy(self._retry_counts),
                "audit": deepcopy(self._audit),
            }


__all__ = [
    "ProgramRuntimeControlError",
    "ProgramRuntimeControls",
    "RECOVERY_OFFER_SCHEMA",
    "RUNTIME_CONTROL_SCHEMA",
    "manual_result_availability",
]
