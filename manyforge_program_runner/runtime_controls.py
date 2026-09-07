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

from manyforge_behavior.json_scalars import nonnegative_int64, nonnegative_int64_value


RUNTIME_CONTROL_SCHEMA: Final = "manyforge.program_runtime_controls.v2"
_LEGACY_CONTROL_SCHEMA: Final = "manyforge.program_runtime_controls.v1"
RECOVERY_OFFER_SCHEMA: Final = "manyforge.cycle_recovery_offer.v2"
_MAX_OVERRIDES: Final = 1_024
_MAX_AUDIT: Final = 256
_MAX_REVISION: Final = 9_007_199_254_740_991
_MAX_PROMOTIONS: Final = 1_024
# One entry per captured Program node, never per request/attempt.
_MAX_RETRY_NODES: Final = 100_000
_MAX_RETRIES_PER_NODE: Final = 255
_RECOVERY_FENCE_FIELDS: Final = (
    "program_generation",
    "deployment_generation",
    "scene_admission_epoch",
    "pose_source_apply_generation",
)
_OVERRIDE_FIELDS: Final = frozenset(
    {
        "schema_version",
        "override_id",
        "override_revision",
        "status",
        "program_run_id",
        "program_revision",
        "program_binding_id",
        "retained_bundle_id",
        "graph_revision",
        "node_environment_digest",
        "node_instance_id",
        "node_attempt_id",
        "parameter_id",
        "value",
        "source_observation_id",
        "source_owner_generation",
    }
)
_PROMOTION_FIELDS: Final = frozenset(
    {
        "schema_version",
        "proposal_id",
        "status",
        "program_run_id",
        "override_id",
        "program_revision",
        "program_binding_id",
        "node_instance_id",
        "node_id",
        "parameter_id",
        "value",
        "expected_draft_id",
        "expected_draft_revision",
    }
)
_SNAPSHOT_FIELDS: Final = frozenset(
    {
        "schema_version",
        "program_run_id",
        "runtime_control_revision",
        "recovery_fences",
        "has_active_runtime_overrides",
        "has_staged_runtime_overrides",
        "has_stale_runtime_overrides",
        "overrides",
        "promotion_proposals",
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

    actions = policy.get("allowed_recovery_actions", [])
    if (
        policy.get("allow_intervention") is not True
        or not isinstance(actions, (list, tuple))
        or "intervention" not in actions
    ):
        return False, "manual_result_forbidden_by_captured_policy"
    if intervention.owner_task_id is None:
        return True, None
    owner = intervention.owner_assessment
    if (
        not isinstance(owner, Mapping)
        or owner.get("acknowledged_non_moving") is not True
    ):
        return False, "manual_result_owner_not_acknowledged"
    try:
        raw_deadline = owner.get("observed_state_valid_until_ns", 0)
        valid_until_ns = nonnegative_int64(raw_deadline)
    except ValueError:
        return False, "manual_result_owner_observation_invalid"
    if (
        type(valid_until_ns) is int
        and valid_until_ns > 0
        and time.monotonic_ns() >= valid_until_ns
    ):
        return False, "manual_result_owner_observation_expired"
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
        self._overrides: dict[str, dict[str, Any]] = {}
        self._promotion_proposals: dict[str, dict[str, Any]] = {}
        self._audit: list[dict[str, Any]] = []
        self._retry_counts: dict[str, int] = {}
        self._offer_revision = 0
        self._offer_key: str | None = None
        self._offer: dict[str, Any] | None = None
        self._lock = RLock()

    @property
    def program_run_id(self) -> str:
        return self._capture["program_run_id"]

    @property
    def revision(self) -> int:
        """Read the owner change key without copying overrides or audit data."""
        with self._lock:
            return self._revision

    @classmethod
    def restore(
        cls,
        capture: Mapping[str, Any],
        snapshot: Mapping[str, Any],
    ) -> "ProgramRuntimeControls":
        """Restore the latest journaled control state for one retained run."""

        if not isinstance(snapshot, Mapping) or snapshot.get("schema_version") not in {
            RUNTIME_CONTROL_SCHEMA,
            _LEGACY_CONTROL_SCHEMA,
        }:
            raise ValueError("retained runtime controls have an unsupported schema")
        legacy = snapshot["schema_version"] == _LEGACY_CONTROL_SCHEMA
        if set(snapshot) != _SNAPSHOT_FIELDS:
            raise ValueError("retained runtime controls have an invalid shape")
        fences = snapshot.get("recovery_fences")
        instance = cls(capture, recovery_fences=fences)
        revision = snapshot.get("runtime_control_revision")
        overrides = snapshot.get("overrides")
        proposals = snapshot.get("promotion_proposals")
        retry_counts = snapshot.get("retry_counts")
        audit = snapshot.get("audit")
        if (
            type(revision) is not int
            or not 1 <= revision <= _MAX_REVISION
            or snapshot.get("program_run_id") != instance.program_run_id
            or not isinstance(overrides, list)
            or len(overrides) > _MAX_OVERRIDES
            or not isinstance(proposals, list)
            or len(proposals) > _MAX_PROMOTIONS
            or not isinstance(retry_counts, dict)
            or len(retry_counts) > (1_024 if legacy else _MAX_RETRY_NODES)
            or not isinstance(audit, list)
            or len(audit) > (4_096 if legacy else _MAX_AUDIT)
        ):
            raise ValueError("retained runtime controls exceed their bounds")
        restored_overrides: dict[str, dict[str, Any]] = {}
        for row in overrides:
            if (
                not isinstance(row, dict)
                or set(row) != _OVERRIDE_FIELDS
                or row.get("schema_version") != "manyforge.runtime_override.v1"
                or not _bounded_text(row.get("override_id"))
                or type(row.get("override_revision")) is not int
                or not 1 <= row["override_revision"] <= revision
                or row.get("program_run_id") != instance.program_run_id
                or any(
                    row.get(field) != instance._capture[field]
                    for field in (
                        "program_revision",
                        "program_binding_id",
                        "retained_bundle_id",
                        "graph_revision",
                        "node_environment_digest",
                    )
                )
                or any(
                    not _bounded_text(row.get(field))
                    for field in (
                        "node_instance_id",
                        "node_attempt_id",
                        "parameter_id",
                    )
                )
                or (
                    row.get("source_observation_id") is None
                    and row.get("source_owner_generation") is not None
                )
                or (
                    row.get("source_observation_id") is not None
                    and (
                        not _bounded_text(row.get("source_observation_id"))
                        or type(row.get("source_owner_generation")) is not int
                        or row["source_owner_generation"] < 1
                    )
                )
                or row.get("status") not in {"staged", "active", "cleared", "stale"}
            ):
                raise ValueError("retained runtime override is invalid")
            identifier = row["override_id"]
            if identifier in restored_overrides:
                raise ValueError("retained runtime override identity is repeated")
            restored_overrides[identifier] = deepcopy(row)
        restored_proposals: dict[str, dict[str, Any]] = {}
        for row in proposals:
            if (
                not isinstance(row, dict)
                or set(row) != _PROMOTION_FIELDS
                or row.get("schema_version")
                != "manyforge.runtime_override_promotion.v1"
                or not _bounded_text(row.get("proposal_id"))
                or row.get("program_run_id") != instance.program_run_id
                or not _bounded_text(row.get("override_id"))
                or not _bounded_text(row.get("expected_draft_id"))
                or not _bounded_text(row.get("node_id"))
                or type(row.get("expected_draft_revision")) is not int
                or row["expected_draft_revision"] < 1
                or row.get("status") not in {"awaiting_reset", "ready"}
            ):
                raise ValueError("retained runtime promotion is invalid")
            identifier = row["proposal_id"]
            if identifier in restored_proposals:
                raise ValueError("retained runtime promotion identity is repeated")
            restored_proposals[identifier] = deepcopy(row)
        for proposal in restored_proposals.values():
            override = restored_overrides.get(proposal["override_id"])
            if override is None or any(
                proposal[field] != override[field]
                for field in (
                    "program_revision",
                    "program_binding_id",
                    "node_instance_id",
                    "parameter_id",
                    "value",
                )
            ):
                raise ValueError(
                    "retained runtime promotion does not name its exact override"
                )
        expected_flags = {
            "has_active_runtime_overrides": any(
                row["status"] == "active" for row in restored_overrides.values()
            ),
            "has_staged_runtime_overrides": any(
                row["status"] == "staged" for row in restored_overrides.values()
            ),
            "has_stale_runtime_overrides": any(
                row["status"] == "stale" for row in restored_overrides.values()
            ),
        }
        if any(
            snapshot.get(field) is not expected
            for field, expected in expected_flags.items()
        ):
            raise ValueError("retained runtime-control flags are divergent")
        restored_retries: dict[str, int] = {}
        for node_instance_id, count in retry_counts.items():
            if (
                not _bounded_text(node_instance_id)
                or type(count) is not int
                or not 0 <= count <= _MAX_RETRIES_PER_NODE
            ):
                raise ValueError("retained runtime retry count is invalid")
            restored_retries[node_instance_id] = count
        expected_length = revision - 1 if legacy else min(revision - 1, _MAX_AUDIT)
        if len(audit) != expected_length:
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
        instance._overrides = restored_overrides
        instance._promotion_proposals = restored_proposals
        instance._retry_counts = restored_retries
        instance._audit = deepcopy(audit[-_MAX_AUDIT:])
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

    def _retire_resolved_overrides(self) -> None:
        protected = {row["override_id"] for row in self._promotion_proposals.values()}
        self._overrides = {
            key: row
            for key, row in self._overrides.items()
            if row["status"] in {"staged", "active"} or key in protected
        }

    def stage_override(
        self,
        *,
        node_instance_id: str,
        node_attempt_id: str,
        parameter_id: str,
        value: Any,
        source_observation_id: str | None,
        source_owner_generation: int | None,
        validator,
    ) -> dict[str, Any]:
        validated = validator(
            node_instance_id=node_instance_id,
            node_attempt_id=node_attempt_id,
            parameter_id=parameter_id,
            value=value,
        )
        identity = {
            "program_run_id": self.program_run_id,
            "program_revision": self._capture["program_revision"],
            "program_binding_id": self._capture["program_binding_id"],
            "retained_bundle_id": self._capture["retained_bundle_id"],
            "graph_revision": self._capture["graph_revision"],
            "node_environment_digest": self._capture["node_environment_digest"],
            "node_instance_id": node_instance_id,
            "node_attempt_id": node_attempt_id,
            "parameter_id": parameter_id,
            "value": deepcopy(validated),
            "source_observation_id": source_observation_id,
            "source_owner_generation": source_owner_generation,
        }
        override_id = _digest("manyforge.runtime_override.v1", identity)
        with self._lock:
            current = self._overrides.get(override_id)
            if current is not None and current["status"] in {"staged", "active"}:
                return deepcopy(current)
            self.require_event_revisions()
            if current is not None:
                current["status"] = "staged"
                current["override_revision"] += 1
                self._event("runtime_override_staged", override_id=override_id)
                return deepcopy(current)
            if len(self._overrides) >= _MAX_OVERRIDES:
                self._retire_resolved_overrides()
            if len(self._overrides) >= _MAX_OVERRIDES:
                raise ProgramRuntimeControlError(
                    "program_runtime_override_capacity_full",
                    "bounded runtime-override capacity is full",
                )
            record = {
                "schema_version": "manyforge.runtime_override.v1",
                "override_id": override_id,
                "override_revision": 1,
                "status": "staged",
                **identity,
            }
            self._overrides[override_id] = record
            self._event("runtime_override_staged", override_id=override_id)
            return deepcopy(record)

    def _override(self, override_id: str) -> dict[str, Any]:
        try:
            return self._overrides[override_id]
        except KeyError as error:
            raise ProgramRuntimeControlError(
                "program_runtime_override_not_found",
                "runtime override is not retained",
            ) from error

    def activate_override(self, override_id: str, *, activator) -> dict[str, Any]:
        with self._lock:
            record = self._override(override_id)
            if record["status"] == "active":
                return deepcopy(record)
            if record["status"] != "staged":
                raise ProgramRuntimeControlError(
                    "program_runtime_override_stale",
                    "only a staged runtime override can be activated",
                )
            self.require_event_revisions()
            activator(
                node_instance_id=record["node_instance_id"],
                node_attempt_id=record["node_attempt_id"],
                parameter_id=record["parameter_id"],
                value=record["value"],
            )
            superseded = []
            for prior in self._overrides.values():
                if (
                    prior["status"] == "active"
                    and prior["node_instance_id"] == record["node_instance_id"]
                    and prior["parameter_id"] == record["parameter_id"]
                ):
                    prior["status"] = "cleared"
                    prior["override_revision"] += 1
                    superseded.append(prior["override_id"])
            record["status"] = "active"
            record["override_revision"] += 1
            self._event(
                "runtime_override_activated",
                override_id=override_id,
                superseded_override_ids=superseded,
            )
            return deepcopy(record)

    def clear_override(self, override_id: str, *, clearer) -> dict[str, Any]:
        with self._lock:
            record = self._override(override_id)
            if record["status"] == "cleared":
                return deepcopy(record)
            self.require_event_revisions()
            if record["status"] == "active":
                clearer(
                    node_instance_id=record["node_instance_id"],
                    parameter_id=record["parameter_id"],
                )
            record["status"] = "cleared"
            record["override_revision"] += 1
            self._event("runtime_override_cleared", override_id=override_id)
            return deepcopy(record)

    def request_promotion(
        self,
        override_id: str,
        *,
        node_id: str,
        expected_draft_id: str,
        expected_draft_revision: int,
    ) -> dict[str, Any]:
        if not _bounded_text(node_id):
            raise ProgramRuntimeControlError(
                "program_runtime_promotion_invalid",
                "runtime promotion has no exact captured node kind",
            )
        with self._lock:
            record = self._override(override_id)
            if record["status"] not in {"staged", "active"}:
                raise ProgramRuntimeControlError(
                    "program_runtime_override_stale",
                    "only a current runtime override can be promoted",
                )
            proposal_body = {
                "program_run_id": self.program_run_id,
                "override_id": override_id,
                "program_revision": record["program_revision"],
                "program_binding_id": record["program_binding_id"],
                "node_instance_id": record["node_instance_id"],
                "node_id": node_id,
                "parameter_id": record["parameter_id"],
                "value": deepcopy(record["value"]),
                "expected_draft_id": expected_draft_id,
                "expected_draft_revision": expected_draft_revision,
            }
            proposal_id = _digest(
                "manyforge.runtime_override_promotion.v1", proposal_body
            )
            current = self._promotion_proposals.get(proposal_id)
            if current is not None:
                return deepcopy(current)
            if len(self._promotion_proposals) >= _MAX_PROMOTIONS:
                raise ProgramRuntimeControlError(
                    "program_runtime_promotion_capacity_full",
                    "bounded runtime-override promotion capacity is full",
                )
            self.require_event_revisions()
            proposal = {
                "schema_version": "manyforge.runtime_override_promotion.v1",
                "proposal_id": proposal_id,
                "status": "awaiting_reset",
                **proposal_body,
            }
            self._promotion_proposals[proposal_id] = proposal
            self._event(
                "runtime_override_promotion_requested",
                override_id=override_id,
                proposal_id=proposal_id,
            )
            return deepcopy(proposal)

    def mark_reset(
        self,
        *,
        request_id: str | None = None,
        correlation_id: str | None = None,
        source_label: str | None = None,
    ) -> None:
        with self._lock:
            changed = any(
                override["status"] in {"staged", "active"}
                for override in self._overrides.values()
            ) or any(
                proposal["status"] == "awaiting_reset"
                for proposal in self._promotion_proposals.values()
            )
            # A fresh transport request can resume the same owner Reset after
            # a phase-write failure. It must not consume another revision or
            # replace the original accepted transition's attribution.
            recorded = bool(
                self._audit
                and self._audit[-1]["kind"] == "program_runtime_controls_reset"
            )
            if not changed and (request_id is None or recorded):
                return
            self.require_event_revisions(reserved_after=0)
            for override in self._overrides.values():
                if override["status"] in {"staged", "active"}:
                    override["status"] = "stale"
                    override["override_revision"] += 1
            for proposal in self._promotion_proposals.values():
                if proposal["status"] == "awaiting_reset":
                    proposal["status"] = "ready"
            self._event(
                "program_runtime_controls_reset",
                reserved_after=0,
                request_id=request_id,
                correlation_id=correlation_id,
                source_label=source_label,
                action_id="reset_cycle",
            )

    def require_retry_capacity(self, node_instance_id: str) -> None:
        """Preflight the captured node's retry allowance before owner effects."""
        with self._lock:
            if not _bounded_text(node_instance_id):
                raise ValueError("retry requires one exact captured node identity")
            if self._retry_counts.get(node_instance_id, 0) >= _MAX_RETRIES_PER_NODE:
                raise ProgramRuntimeControlError(
                    "program_runtime_retry_limit_reached",
                    "the retained retry count reached its fixed bound",
                )
            if (
                node_instance_id not in self._retry_counts
                and len(self._retry_counts) >= _MAX_RETRY_NODES
            ):
                raise ProgramRuntimeControlError(
                    "program_runtime_retry_nodes_full",
                    "retry identities exceed the maximum captured Program node count",
                )

    def note_retry(self, node_instance_id: str) -> int:
        with self._lock:
            self.require_retry_capacity(node_instance_id)
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

    def _has_active_override(self, *, node_instance_id: str) -> bool:
        with self._lock:
            return any(
                row["status"] == "active"
                and row["node_instance_id"] == node_instance_id
                for row in self._overrides.values()
            )

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
    ) -> dict[str, Any] | None:
        if intervention is None:
            return None
        identity = intervention.identity
        owner = (
            _project_wire_value(intervention.owner_assessment)
            if intervention.owner_assessment is not None
            else None
        )
        projected_policy = _project_wire_value(policy)
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
        try:
            valid_until_ns = (
                nonnegative_int64(owner.get("observed_state_valid_until_ns", 0)) or None
                if isinstance(owner, dict)
                else None
            )
        except ValueError:
            return None
        observation_current = (
            valid_until_ns is None or time.monotonic_ns() < valid_until_ns
        )
        operator_stop_recovery = intervention.policy_parameter == "operator_stop_policy"
        if operator_stop_recovery:
            manual_result_enabled = False
            manual_result_disabled_reason = "operator_stop_requires_owner_recovery"
        else:
            manual_result_enabled, manual_result_disabled_reason = (
                manual_result_availability(intervention, policy)
            )
        allow_intervention = policy.get("allow_intervention") is True
        allowed_policy_actions = policy.get("allowed_recovery_actions", [])
        intervention_allowed = (
            allow_intervention
            and isinstance(allowed_policy_actions, (list, tuple))
            and "intervention" in allowed_policy_actions
        )
        runtime_override_retry_allowed = (
            isinstance(allowed_policy_actions, (list, tuple))
            and "runtime_override" in allowed_policy_actions
            and self._has_active_override(
                node_instance_id=identity.node_instance_id,
            )
        )
        max_retries = policy.get("max_retries", 0)
        if type(max_retries) is not int or max_retries < 0:
            max_retries = 0
        retry_available = self.retry_count(identity.node_instance_id) < max_retries
        actions: list[dict[str, Any]] = []
        for action_id in owner_actions:
            if action_id not in {
                "retry_motion_from_current_state",
                "acknowledge_and_continue",
                "retry_after_scene_repair",
                "replan_from_stopped_state",
            }:
                continue
            if action_id == "acknowledge_and_continue":
                enabled_by_policy = intervention_allowed
            else:
                enabled_by_policy = operator_stop_recovery or (
                    intervention_allowed and retry_available
                )
            enabled = enabled_by_policy and observation_current
            actions.append(
                {
                    "action_id": action_id,
                    "label_key": f"recovery.{action_id}",
                    "requires_confirmation": action_id == "acknowledge_and_continue",
                    "performs_backend_reset": False,
                    "enabled": enabled,
                    "disabled_reason_code": (
                        None
                        if enabled
                        else (
                            "recovery_owner_observation_expired"
                            if not observation_current
                            else "recovery_action_forbidden_by_captured_policy"
                        )
                    ),
                }
            )
        if (
            retry_available
            and (intervention_allowed or runtime_override_retry_allowed)
            and observation_current
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
                    "label_key": "recovery.retry_runtime_step",
                    "requires_confirmation": False,
                    "performs_backend_reset": False,
                    "enabled": True,
                    "disabled_reason_code": None,
                }
            )
        operator_stop_settled = (
            isinstance(owner, dict)
            and owner.get("recovery_class")
            in {"ordinary_operator_stop", "stop_fault_intervention"}
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
        stop_fault_reset_required = (
            isinstance(owner, dict)
            and owner.get("recovery_class") == "stop_fault_intervention"
            and owner.get("status") == "reset_required"
            and owner.get("retry_admissible") is False
        )
        held_reset_enabled = (
            operator_stop_recovery
            and observation_current
            and operator_stop_settled
            and (owner.get("retry_admissible") is True or stop_fault_reset_required)
        )
        actions.append(
            {
                "action_id": "reset_cycle",
                "label_key": "recovery.reset_cycle",
                "requires_confirmation": True,
                "performs_backend_reset": True,
                "enabled": held_reset_enabled,
                "disabled_reason_code": (
                    None
                    if held_reset_enabled
                    else (
                        "recovery_owner_observation_expired"
                        if operator_stop_recovery and not observation_current
                        else (
                            "operator_stop_not_physically_settled"
                            if operator_stop_recovery
                            else "cycle_not_terminal"
                        )
                    )
                ),
            }
        )
        material = {
            "cycle_generation": cycle_generation,
            "program_run_id": identity.program_run_id,
            "node_instance_id": identity.node_instance_id,
            "node_attempt_id": identity.node_attempt_id,
            "owner_epoch": owner_epoch,
            "owner_assessment": owner,
            "actions": actions,
            "manual_result": {
                "enabled": manual_result_enabled,
                "disabled_reason_code": manual_result_disabled_reason,
            },
            "policy": projected_policy,
            "recovery_fences": deepcopy(self._recovery_fences),
        }
        offer_key = _digest("manyforge.cycle_recovery_offer.semantic.v2", material)
        with self._lock:
            if self._offer_key == offer_key and self._offer is not None:
                return deepcopy(self._offer)
            if self._offer_revision >= _MAX_REVISION:
                raise ProgramRuntimeControlError(
                    "program_recovery_offer_revision_exhausted",
                    "recovery-offer revision cannot advance exactly",
                )
            self._offer_revision += 1
            result = {
                "schema_version": RECOVERY_OFFER_SCHEMA,
                "offer_id": _digest("manyforge.cycle_recovery_offer.v2", material),
                "offer_revision": self._offer_revision,
                "generated_at_ns": nonnegative_int64_value(time.monotonic_ns()),
                "valid_until_ns": (
                    nonnegative_int64_value(valid_until_ns)
                    if valid_until_ns is not None
                    else None
                ),
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
                "latch_id": (
                    owner.get("latch_id")
                    if isinstance(owner, dict) and owner.get("latch_id")
                    else None
                ),
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
                "primary_action": next(
                    (row["action_id"] for row in actions if row["enabled"]), None
                ),
                "reason_code": intervention.reason_code,
                "evidence_refs": (
                    owner.get("evidence_refs", []) if isinstance(owner, dict) else []
                ),
                "policy": deepcopy(projected_policy),
            }
            self._offer_key = offer_key
            self._offer = deepcopy(result)
            return deepcopy(result)

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            overrides = [
                deepcopy(self._overrides[key]) for key in sorted(self._overrides)
            ]
            proposals = [
                deepcopy(self._promotion_proposals[key])
                for key in sorted(self._promotion_proposals)
            ]
            return {
                "schema_version": RUNTIME_CONTROL_SCHEMA,
                "program_run_id": self.program_run_id,
                "runtime_control_revision": self._revision,
                "recovery_fences": deepcopy(self._recovery_fences),
                "has_active_runtime_overrides": any(
                    row["status"] == "active" for row in overrides
                ),
                "has_staged_runtime_overrides": any(
                    row["status"] == "staged" for row in overrides
                ),
                "has_stale_runtime_overrides": any(
                    row["status"] == "stale" for row in overrides
                ),
                "overrides": overrides,
                "promotion_proposals": proposals,
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
