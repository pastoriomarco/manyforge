"""ROS-only physical-robot lifecycle bridge for Composer.

The browser receives a generic robot-state contract and may request only
bounded product operations.  Vendor primitives and controller-manager stages
remain behind the lifecycle provider's serialized transaction boundary.
"""
from __future__ import annotations

from collections import deque
from dataclasses import dataclass
import hashlib
import hmac
import json
import logging
from pathlib import Path
import threading
import time
import uuid
from typing import Any, Callable, Mapping, Protocol


_LOGGER = logging.getLogger(__name__)
_SNAPSHOT_SCHEMA = "manyforge.robot_lifecycle.snapshot.v1"
_TERMINAL_PHASES = {"succeeded", "failed", "outcome_unknown"}
_FACT_VALUES = {"true", "false", "unknown"}
_STATUS_HEARTBEAT_NS = 500_000_000
_RUNTIME_RECORD_SCHEMA = "manyforge.real_robot_runtime_record.v1"
_RESOLUTION_SCHEMA = "manyforge.real_robot_resolution.v1"
_PREPARE_FOR_MOTION_ACTION = "robot.prepare_for_motion"
_MAKE_NONCOMMANDABLE_ACTION = "robot.make_noncommandable"
_DEFAULT_AUTHORITY_LEASE_MS = 5_000
_DEFAULT_AUTHORITY_HEARTBEAT_PERIOD_MS = 1_000
_DEFAULT_AUTHORITY_HEALTH_MAX_AGE_MS = 2_000
_RECOVERABLE_OWNER_PROGRESS_REASONS = frozenset(
    {"xarm_owner_publication_stale"}
)
_MATERIAL_STATUS_FIELDS = (
    ("transport_connected", "transportConnected"),
    ("identity_verified", "identityVerified"),
    ("position_valid", "positionValid"),
    ("robot_enabled", "robotEnabled"),
    ("vendor_control_ready", "vendorControlReady"),
    ("hardware_component_active", "hardwareComponentActive"),
    ("trajectory_controller_active", "trajectoryControllerActive"),
    ("fault_present", "faultPresent"),
    ("warning_present", "warningPresent"),
    ("recovery_required", "recoveryRequired"),
    ("stationary", "stationary"),
    ("transport_owner_progress_healthy", "transportOwnerProgressHealthy"),
    ("transport_owner_progress_reason", "transportOwnerProgressReason"),
    ("observation_fresh", "observationFresh"),
    ("authority_fresh", "authorityFresh"),
    ("observe_ready", "observeReady"),
    ("controller_ready", "controllerReady"),
    ("motion_admitted", "motionAdmitted"),
    ("reconciliation_required", "reconciliationRequired"),
    ("authority_component_epoch", "authorityComponentEpoch"),
    ("authority_session_generation", "authoritySessionGeneration"),
    ("command_source", "commandSource"),
)
_OBSERVATION_IDENTITY_FIELDS = (
    ("provider_id", "providerId"),
    ("robot_id", "robotId"),
    ("authority_profile", "authorityProfile"),
    ("runtime_generation_id", "runtimeGenerationId"),
    ("provider_component_epoch", "providerComponentEpoch"),
    ("transport_owner_component_epoch", "transportOwnerComponentEpoch"),
    ("cell_supervisor_component_epoch", "cellSupervisorComponentEpoch"),
)


def _required_text(mapping: Mapping[str, Any], key: str) -> str:
    value = mapping.get(key)
    if not isinstance(value, str) or not value.strip() or len(value) > 512:
        raise ValueError(f"robot_lifecycle_provider.{key} is invalid")
    return value.strip()


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_real_robot_runtime_record(
    *,
    deployment_path: Path,
    execution_config: Mapping[str, Any],
) -> dict[str, Any]:
    """Validate every retained input before a physical deployment cutover."""
    declaration = execution_config.get("real_robot_runtime_record")
    if not isinstance(declaration, Mapping):
        raise ValueError(
            "real-robot execution config has no retained runtime record"
        )
    if declaration.get("schema") != _RESOLUTION_SCHEMA:
        raise ValueError("real-robot runtime resolution schema is unsupported")
    raw_path = declaration.get("path")
    if not isinstance(raw_path, str) or not raw_path.strip():
        raise ValueError("real-robot runtime record path is invalid")
    record_path = Path(raw_path).resolve(strict=True)
    deployment_path = deployment_path.resolve(strict=True)
    if record_path.parent != deployment_path.parent:
        raise ValueError(
            "real-robot runtime record must accompany the resolved deployment"
        )
    try:
        record = json.loads(record_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError("real-robot runtime record is unreadable") from exc
    if not isinstance(record, dict) or record.get("schema") != _RUNTIME_RECORD_SCHEMA:
        raise ValueError("real-robot runtime record schema is unsupported")
    expected_generation = declaration.get("bindingGenerationId")
    if (
        not isinstance(expected_generation, str)
        or record.get("runtimeGenerationId") != expected_generation
    ):
        raise ValueError("real-robot runtime generation does not match deployment")
    robot = record.get("robot")
    provider = execution_config.get("robot_lifecycle_provider")
    if not isinstance(robot, Mapping) or not isinstance(provider, Mapping):
        raise ValueError("real-robot provider identity record is malformed")
    if (
        robot.get("lifecycleProviderInstanceId")
        != declaration.get("providerInstanceId")
        or robot.get("lifecycleProviderInstanceId")
        != provider.get("provider_instance_id")
    ):
        raise ValueError("real-robot lifecycle-provider identity mismatch")
    expected_health_policy = {
        "leaseDurationMs": provider.get("authority_lease_duration_ms"),
        "heartbeatPeriodMs": provider.get("authority_heartbeat_period_ms"),
        "criticalProgressMaxAgeMs": provider.get(
            "authority_health_max_age_ms"
        ),
        "providerMaximumLeaseMs": provider.get(
            "maximum_authority_lease_ms"
        ),
    }
    if robot.get("authorityHealthPolicy") != expected_health_policy:
        raise ValueError("real-robot authority-health policy mismatch")
    supervision = execution_config.get("cell_supervision")
    recorded_supervision = record.get("cellSupervision")
    if (
        not isinstance(supervision, Mapping)
        or not isinstance(recorded_supervision, Mapping)
        or recorded_supervision.get("schema") != supervision.get("schema")
        or recorded_supervision.get("cellId") != supervision.get("cell_id")
        or recorded_supervision.get("supervisorComponentEpoch")
        != supervision.get("cell_supervisor_component_epoch")
        or recorded_supervision.get("supervisorHeartbeatPath")
        != supervision.get("supervisor_heartbeat_path")
        or recorded_supervision.get("components")
        != supervision.get("components")
        or recorded_supervision.get("startupScopedEvidenceOnly") is not True
    ):
        raise ValueError("real-robot cell-supervision identity mismatch")
    controller_binding = execution_config.get("controller_binding")
    if (
        not isinstance(controller_binding, Mapping)
        or controller_binding.get("generation_id")
        != record.get("bindingGenerationId")
    ):
        raise ValueError("real-robot controller-binding identity mismatch")

    artifacts = record.get("artifacts")
    if not isinstance(artifacts, Mapping):
        raise ValueError("real-robot runtime artifact record is malformed")
    for name, artifact in artifacts.items():
        if not isinstance(name, str) or not isinstance(artifact, Mapping):
            raise ValueError("real-robot runtime artifact entry is malformed")
        artifact_path_raw = artifact.get("path")
        expected_sha256 = artifact.get("sha256")
        if (
            not isinstance(artifact_path_raw, str)
            or not isinstance(expected_sha256, str)
            or len(expected_sha256) != 64
        ):
            raise ValueError(
                f"real-robot runtime artifact '{name}' identity is malformed"
            )
        try:
            artifact_path = Path(artifact_path_raw).resolve(strict=True)
            actual_sha256 = _sha256(artifact_path)
        except OSError as exc:
            raise ValueError(
                f"real-robot runtime artifact '{name}' is unavailable"
            ) from exc
        if not hmac.compare_digest(actual_sha256, expected_sha256):
            raise ValueError(
                f"real-robot runtime artifact '{name}' digest mismatch"
            )
    resolved = artifacts.get("resolvedDeployment")
    if (
        not isinstance(resolved, Mapping)
        or Path(str(resolved.get("path") or "")).resolve() != deployment_path
        or not hmac.compare_digest(
            str(resolved.get("sha256") or ""),
            _sha256(deployment_path),
        )
    ):
        raise ValueError(
            "real-robot runtime record does not bind this deployment"
        )
    return record


@dataclass(frozen=True, slots=True)
class RobotLifecycleConfig:
    robot_id: str
    provider_instance_id: str
    runtime_generation_id: str
    cell_supervisor_component_epoch: str
    status_topic: str
    open_session_service: str
    close_session_service: str
    submit_command_service: str
    reconcile_command_service: str
    material_adapter_status_keys: tuple[str, ...] = ()
    caller_assurance: str = "unverified"
    authority_lease_ns: int = _DEFAULT_AUTHORITY_LEASE_MS * 1_000_000
    authority_heartbeat_period_ns: int = (
        _DEFAULT_AUTHORITY_HEARTBEAT_PERIOD_MS * 1_000_000
    )
    authority_health_max_age_ns: int = (
        _DEFAULT_AUTHORITY_HEALTH_MAX_AGE_MS * 1_000_000
    )
    status_delivery_max_age_ns: int = 750_000_000
    service_timeout_s: float = 5.0
    transaction_timeout_s: float = 8.0

    @classmethod
    def from_mapping(
        cls,
        value: Mapping[str, Any],
        *,
        fallback_robot_id: str,
    ) -> "RobotLifecycleConfig":
        if value.get("schema") != "manyforge.robot_lifecycle.ros_provider.v1":
            raise ValueError("robot_lifecycle_provider schema is unsupported")
        assurance = str(value.get("caller_assurance") or "")
        if assurance not in {"unverified", "authenticated"}:
            raise ValueError("robot_lifecycle_provider caller assurance is invalid")
        raw_material_keys = value.get("material_adapter_status_keys", [])
        if (
            not isinstance(raw_material_keys, list)
            or len(raw_material_keys) > 16
            or any(
                not isinstance(key, str)
                or not key.strip()
                or key != key.strip()
                or len(key) > 128
                for key in raw_material_keys
            )
            or len(set(raw_material_keys)) != len(raw_material_keys)
        ):
            raise ValueError(
                "robot_lifecycle_provider material adapter keys are invalid"
            )
        maximum_lease_ms = value.get("maximum_authority_lease_ms")
        if (
            isinstance(maximum_lease_ms, bool)
            or not isinstance(maximum_lease_ms, int)
            or maximum_lease_ms < 1_000
        ):
            raise ValueError(
                "robot_lifecycle_provider maximum authority lease is invalid"
            )

        def timing_ms(key: str, default: int) -> int:
            raw = value.get(key, default)
            if isinstance(raw, bool) or not isinstance(raw, int) or raw <= 0:
                raise ValueError(
                    f"robot_lifecycle_provider.{key} is invalid"
                )
            return raw

        default_lease_ms = min(
            _DEFAULT_AUTHORITY_LEASE_MS,
            maximum_lease_ms // 2,
        )
        lease_ms = timing_ms(
            "authority_lease_duration_ms",
            default_lease_ms,
        )
        heartbeat_ms = timing_ms(
            "authority_heartbeat_period_ms",
            min(
                _DEFAULT_AUTHORITY_HEARTBEAT_PERIOD_MS,
                max(1, lease_ms // 4),
            ),
        )
        health_max_age_ms = timing_ms(
            "authority_health_max_age_ms",
            min(
                _DEFAULT_AUTHORITY_HEALTH_MAX_AGE_MS,
                max(1, lease_ms // 2),
            ),
        )
        if lease_ms > maximum_lease_ms:
            raise ValueError(
                "robot_lifecycle_provider authority lease exceeds provider maximum"
            )
        if lease_ms < heartbeat_ms * 4:
            raise ValueError(
                "robot_lifecycle_provider authority lease has insufficient "
                "heartbeat margin"
            )
        if health_max_age_ms >= lease_ms:
            raise ValueError(
                "robot_lifecycle_provider authority health age must be below "
                "the lease"
            )
        return cls(
            robot_id=_required_text(value, "robot_id")
            if value.get("robot_id")
            else fallback_robot_id,
            provider_instance_id=_required_text(
                value, "provider_instance_id"
            ),
            runtime_generation_id=_required_text(
                value, "runtime_generation_id"
            ),
            cell_supervisor_component_epoch=_required_text(
                value, "cell_supervisor_component_epoch"
            ),
            status_topic=_required_text(value, "status_topic"),
            open_session_service=_required_text(
                value, "open_session_service"
            ),
            close_session_service=_required_text(
                value, "close_session_service"
            ),
            submit_command_service=_required_text(
                value, "submit_command_service"
            ),
            reconcile_command_service=_required_text(
                value, "reconcile_command_service"
            ),
            material_adapter_status_keys=tuple(raw_material_keys),
            caller_assurance=assurance,
            authority_lease_ns=lease_ms * 1_000_000,
            authority_heartbeat_period_ns=heartbeat_ms * 1_000_000,
            authority_health_max_age_ns=health_max_age_ms * 1_000_000,
        )


@dataclass(frozen=True, slots=True)
class AuthorityHealthEvidence:
    """Receiver-local proof that the selected headless authority progressed."""

    component_epoch: str
    sequence: int
    observed_at_ns: int
    healthy: bool
    reason: str = ""


class LifecycleTransport(Protocol):
    def open_session(self, payload: Mapping[str, Any]) -> dict[str, Any]: ...

    def close_session(self, payload: Mapping[str, Any]) -> dict[str, Any]: ...

    def submit_command(self, payload: Mapping[str, Any]) -> dict[str, Any]: ...

    def reconcile_command(
        self,
        payload: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def close(self) -> None: ...


def _transaction_payload(message: Any) -> dict[str, Any]:
    return {
        "generation": int(message.generation),
        "requestId": str(message.request_id),
        "requestSequence": int(message.request_sequence),
        "actionId": str(message.action_id),
        "commandSource": str(message.command_source),
        "authoritySessionGeneration": int(
            message.authority_session_generation
        ),
        "phase": str(message.phase),
        "acceptedAtSteadyTimeNs": int(message.accepted_at_steady_time_ns),
        "deadlineSteadyTimeNs": int(message.deadline_steady_time_ns),
        "terminalAtSteadyTimeNs": int(message.terminal_at_steady_time_ns),
        "resultReason": str(message.result_reason),
    }


def snapshot_message_payload(message: Any) -> dict[str, Any]:
    """Convert and bound the generated ROS message without leaking ROS types."""
    facts = {
        "transportConnected": str(message.transport_connected),
        "identityVerified": str(message.identity_verified),
        "positionValid": str(message.position_valid),
        "robotEnabled": str(message.robot_enabled),
        "vendorControlReady": str(message.vendor_control_ready),
        "hardwareComponentActive": str(message.hardware_component_active),
        "trajectoryControllerActive": str(
            message.trajectory_controller_active
        ),
        "faultPresent": str(message.fault_present),
        "warningPresent": str(message.warning_present),
        "recoveryRequired": str(message.recovery_required),
        "stationary": str(message.stationary),
    }
    if any(value not in _FACT_VALUES for value in facts.values()):
        raise ValueError("robot lifecycle snapshot contains an invalid fact")
    keys = [str(value)[:128] for value in message.adapter_status_keys[:32]]
    values = [
        str(value)[:512] for value in message.adapter_status_values[:32]
    ]
    if len(keys) != len(values) or len(set(keys)) != len(keys):
        raise ValueError("robot lifecycle adapter status is malformed")
    capabilities = []
    for raw in message.capabilities[:32]:
        capabilities.append(
            {
                "actionId": str(raw.action_id)[:128],
                "label": str(raw.label)[:256],
                "supported": bool(raw.supported),
                "available": bool(raw.available),
                "availabilityReason": str(raw.availability_reason)[:512],
                "confirmationClass": str(raw.confirmation_class)[:64],
                "sideEffectSummary": str(raw.side_effect_summary)[:1024],
                "expectedPostcondition": str(
                    raw.expected_postcondition
                )[:1024],
                "timeoutNs": int(raw.timeout_ns),
            }
        )
    return {
        "schemaVersion": str(message.schema),
        "providerInstanceId": str(message.provider_instance_id),
        "robotId": str(message.robot_id),
        "supervisorInstanceId": str(message.supervisor_instance_id),
        "authorityProfile": str(message.authority_profile),
        "runtimeGenerationId": str(message.runtime_generation_id),
        "providerComponentEpoch": str(message.provider_component_epoch),
        "transportOwnerComponentEpoch": str(
            message.transport_owner_component_epoch
        ),
        "authorityComponentEpoch": str(
            message.authority_component_epoch
        ),
        "cellSupervisorComponentEpoch": str(
            message.cell_supervisor_component_epoch
        ),
        "providerPublicationSequence": int(
            message.provider_publication_sequence
        ),
        "providerPublishedSteadyTimeNs": int(
            message.provider_published_steady_time_ns
        ),
        "transportOwnerProgressSequence": int(
            message.transport_owner_progress_sequence
        ),
        "transportOwnerProgressReceivedSteadyTimeNs": int(
            message.transport_owner_progress_received_steady_time_ns
        ),
        "transportOwnerProgressHealthy": bool(
            message.transport_owner_progress_healthy
        ),
        "transportOwnerProgressReason": str(
            message.transport_owner_progress_reason
        )[:512],
        "snapshotGeneration": int(message.snapshot_generation),
        "capabilityGeneration": int(message.capability_generation),
        "observationGeneration": int(message.observation_generation),
        "sourceSteadyTimeNs": int(message.source_steady_time_ns),
        "receivedSteadyTimeNs": int(message.received_steady_time_ns),
        "freshUntilSteadyTimeNs": int(message.fresh_until_steady_time_ns),
        **facts,
        "observationFresh": bool(message.observation_fresh),
        "authorityFresh": bool(message.authority_fresh),
        "observeReady": bool(message.observe_ready),
        "controllerReady": bool(message.controller_ready),
        "motionAdmitted": bool(message.motion_admitted),
        "reconciliationRequired": bool(message.reconciliation_required),
        "admissionReasonCodes": [
            str(value)[:256] for value in message.admission_reason_codes[:32]
        ],
        "adapterStatus": dict(zip(keys, values, strict=True)),
        "authoritySessionId": str(message.authority_session_id),
        "authoritySessionGeneration": int(
            message.authority_session_generation
        ),
        "commandSource": str(message.command_source),
        "authorityValidUntilSteadyTimeNs": int(
            message.authority_valid_until_steady_time_ns
        ),
        "capabilities": capabilities,
        "pendingTransaction": (
            _transaction_payload(message.pending_transaction)
            if message.has_pending_transaction
            else None
        ),
        "lastTerminalTransaction": (
            _transaction_payload(message.last_terminal_transaction)
            if message.has_last_terminal_transaction
            else None
        ),
    }


class RobotLifecycleTransitionRecorder:
    """Project source-ordered material lifecycle changes into history.

    This is a non-authoritative NRT observer. It never infers which external
    tool, safety input, or physical event caused a state change. It retains
    command/transaction identities only as temporal correlations.
    """

    def __init__(
        self,
        event_sink: Callable[..., None],
        *,
        material_adapter_status_keys: tuple[str, ...] = (),
    ) -> None:
        self._event_sink = event_sink
        self._material_adapter_status_keys = material_adapter_status_keys
        self._lock = threading.Lock()
        self._identity_key: tuple[str, ...] | None = None
        self._last_provider_publication_sequence = 0
        self._last_facets: dict[str, Any] = {}
        self._terminal_scope: tuple[str, ...] | None = None
        self._last_terminal_provider_publication_sequence = 0
        self._seen_terminal_keys: deque[tuple[str, int, str]] = deque(
            maxlen=64
        )

    @staticmethod
    def _status_identity(
        status: Mapping[str, Any],
    ) -> dict[str, str]:
        return {
            output_key: str(status.get(status_key) or "").strip()[:512]
            for output_key, status_key in _OBSERVATION_IDENTITY_FIELDS
        }

    @classmethod
    def _observation_identity(
        cls,
        status: Mapping[str, Any],
    ) -> tuple[tuple[str, ...], dict[str, str]] | None:
        identity = cls._status_identity(status)
        required = (
            "provider_id",
            "robot_id",
            "runtime_generation_id",
            "provider_component_epoch",
            "transport_owner_component_epoch",
        )
        if any(not identity[key] for key in required):
            return None
        if (
            int(status.get("providerPublicationSequence") or 0) <= 0
            or int(status.get("observationGeneration") or 0) <= 0
        ):
            return None
        return tuple(identity.values()), identity

    def _material_facets(self, status: Mapping[str, Any]) -> dict[str, Any]:
        facets = {
            output_key: status.get(status_key)
            for output_key, status_key in _MATERIAL_STATUS_FIELDS
        }
        adapter_status = status.get("adapterStatus")
        if isinstance(adapter_status, Mapping):
            facets.update(
                {
                    f"adapter.{key}": adapter_status.get(key)
                    for key in self._material_adapter_status_keys
                }
            )
        return facets

    @staticmethod
    def _adapter_evidence(status: Mapping[str, Any]) -> dict[str, str]:
        """Retain already-bounded vendor details without interpreting them."""
        adapter_status = status.get("adapterStatus")
        if not isinstance(adapter_status, Mapping):
            return {}
        return {
            str(key)[:128]: str(value)[:512]
            for key, value in list(adapter_status.items())[:32]
        }

    @staticmethod
    def _terminal(
        status: Mapping[str, Any],
    ) -> tuple[tuple[str, int, str], dict[str, Any]] | None:
        value = status.get("lastTerminalTransaction")
        if not isinstance(value, Mapping):
            return None
        terminal = dict(value)
        key = (
            str(terminal.get("requestId") or ""),
            int(terminal.get("generation") or 0),
            str(terminal.get("phase") or ""),
        )
        if (
            not key[0]
            or key[1] <= 0
            or key[2] not in _TERMINAL_PHASES
        ):
            return None
        return key, terminal

    @staticmethod
    def _stable_terminal_scope(
        identity: Mapping[str, str],
    ) -> tuple[str, ...] | None:
        keys = (
            "provider_id",
            "robot_id",
            "runtime_generation_id",
            "provider_component_epoch",
        )
        if any(not identity[key] for key in keys):
            return None
        return tuple(identity[key] for key in keys)

    @staticmethod
    def _bounded_transaction(value: Mapping[str, Any]) -> dict[str, Any]:
        keys = (
            "generation",
            "requestId",
            "requestSequence",
            "actionId",
            "commandSource",
            "authoritySessionGeneration",
            "phase",
            "resultReason",
        )
        return {
            key: (
                str(value[key])[:512]
                if isinstance(value[key], str)
                else value[key]
            )
            for key in keys
            if key in value
        }

    @classmethod
    def _correlation(
        cls,
        status: Mapping[str, Any],
        new_terminal: Mapping[str, Any] | None,
    ) -> dict[str, Any]:
        pending = status.get("pendingTransaction")
        if isinstance(pending, Mapping):
            command_source = str(pending.get("commandSource") or "")
            return {
                "classification": (
                    "correlated_manyforge_transaction"
                    if command_source.startswith("composer.")
                    else "correlated_provider_transaction"
                ),
                "transaction": cls._bounded_transaction(pending),
            }
        if new_terminal is not None:
            command_source = str(new_terminal.get("commandSource") or "")
            return {
                "classification": (
                    "correlated_manyforge_terminal"
                    if command_source.startswith("composer.")
                    else "correlated_provider_terminal"
                ),
                "transaction": cls._bounded_transaction(new_terminal),
            }
        command = status.get("commandInFlight")
        if isinstance(command, Mapping):
            return {
                "classification": "correlated_manyforge_command",
                "command": {
                    key: (
                        str(command[key])[:512]
                        if isinstance(command[key], str)
                        else command[key]
                    )
                    for key in ("operation", "stage", "startedAtNs")
                    if key in command
                },
            }
        return {"classification": "external_or_unknown"}

    @staticmethod
    def _source_details(
        status: Mapping[str, Any],
        identity: Mapping[str, str],
    ) -> dict[str, Any]:
        return {
            **identity,
            "provider_publication_sequence": int(
                status.get("providerPublicationSequence") or 0
            ),
            "snapshot_generation": int(
                status.get("snapshotGeneration") or 0
            ),
            "observation_generation": int(
                status.get("observationGeneration") or 0
            ),
            "source_steady_time_ns": int(
                status.get("sourceSteadyTimeNs") or 0
            ),
            "causal_attribution": "not_inferred",
        }

    def _emit(self, action: str, details: dict[str, Any]) -> None:
        try:
            self._event_sink(action, details=details)
        except Exception:
            # History is diagnostic. A broken sink must not change lifecycle,
            # alarm, admission, or command correctness.
            _LOGGER.exception(
                "Failed to record robot lifecycle history",
                extra={"robot_lifecycle_history_action": action},
            )

    def observe(self, status: Mapping[str, Any]) -> None:
        """Record at most one baseline, one delta, and one terminal per sample."""
        resolved_identity = self._observation_identity(status)
        status_identity = self._status_identity(status)
        provider_sequence = int(
            status.get("providerPublicationSequence") or 0
        )
        terminal = self._terminal(status)
        facets = (
            self._material_facets(status)
            if resolved_identity is not None
            else {}
        )
        adapter_evidence = self._adapter_evidence(status)

        # Keep reduction and sink writes in one NRT critical section. Composer
        # status callbacks may interleave after the client publication lock;
        # releasing here before writing could invert source sequence N/N+1 in
        # durable history.
        with self._lock:
            new_terminal: dict[str, Any] | None = None
            terminal_key: tuple[str, int, str] | None = None
            terminal_scope = self._stable_terminal_scope(status_identity)
            if terminal_scope is not None:
                if terminal_scope != self._terminal_scope:
                    self._terminal_scope = terminal_scope
                    self._last_terminal_provider_publication_sequence = 0
                    self._seen_terminal_keys.clear()
                if (
                    provider_sequence
                    > self._last_terminal_provider_publication_sequence
                ):
                    self._last_terminal_provider_publication_sequence = (
                        provider_sequence
                    )
                    if terminal is not None:
                        terminal_key, terminal_value = terminal
                        if terminal_key not in self._seen_terminal_keys:
                            new_terminal = terminal_value

            if resolved_identity is not None:
                identity_key, identity = resolved_identity
                identity_changed = identity_key != self._identity_key
                if identity_changed:
                    self._identity_key = identity_key
                    self._last_provider_publication_sequence = 0
                    self._last_facets = {}
                if provider_sequence > self._last_provider_publication_sequence:
                    if identity_changed:
                        self._emit(
                            "observation_baseline",
                            {
                                **self._source_details(status, identity),
                                "correlation": {
                                    "classification": "observation_baseline"
                                },
                                "facets": dict(facets),
                                "adapter_status": adapter_evidence,
                            },
                        )
                    else:
                        changes = {
                            key: {
                                "previous": self._last_facets.get(key),
                                "current": facets.get(key),
                            }
                            for key in sorted(
                                set(self._last_facets).union(facets)
                            )
                            if self._last_facets.get(key) != facets.get(key)
                        }
                        if changes:
                            self._emit(
                                "observed_transition",
                                {
                                    **self._source_details(status, identity),
                                    "correlation": self._correlation(
                                        status,
                                        new_terminal,
                                    ),
                                    "changed_facets": list(changes),
                                    "changes": changes,
                                    "adapter_status": adapter_evidence,
                                },
                            )
                    self._last_provider_publication_sequence = (
                        provider_sequence
                    )
                    self._last_facets = facets

            if new_terminal is not None and terminal_key is not None:
                self._emit(
                    "transaction_terminal",
                    {
                        **self._source_details(status, status_identity),
                        "transaction": self._bounded_transaction(
                            new_terminal
                        ),
                    }
                )
                self._seen_terminal_keys.append(terminal_key)


class RosLifecycleTransport:
    """One isolated rclpy context for lifecycle status and services."""

    def __init__(
        self,
        config: RobotLifecycleConfig,
        snapshot_sink: Callable[[dict[str, Any]], None],
    ) -> None:
        import rclpy
        from manyforge_robot_lifecycle_msgs.msg import RobotLifecycleSnapshot
        from manyforge_robot_lifecycle_msgs.srv import (
            CloseRobotLifecycleSession,
            OpenRobotLifecycleSession,
            ReconcileRobotLifecycleCommand,
            SubmitRobotLifecycleCommand,
        )
        from rclpy.executors import SingleThreadedExecutor
        from rclpy.qos import (
            DurabilityPolicy,
            HistoryPolicy,
            QoSProfile,
            ReliabilityPolicy,
        )

        self._rclpy = rclpy
        self._service_types = (
            OpenRobotLifecycleSession,
            CloseRobotLifecycleSession,
            SubmitRobotLifecycleCommand,
            ReconcileRobotLifecycleCommand,
        )
        self._context = rclpy.context.Context()
        rclpy.init(args=None, context=self._context)
        self._node = rclpy.create_node(
            f"manyforge_composer_robot_lifecycle_{uuid.uuid4().hex[:8]}",
            context=self._context,
        )
        qos = QoSProfile(
            history=HistoryPolicy.KEEP_LAST,
            depth=1,
            reliability=ReliabilityPolicy.RELIABLE,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
        )
        self._subscription = self._node.create_subscription(
            RobotLifecycleSnapshot,
            config.status_topic,
            lambda message: self._receive_message(message, snapshot_sink),
            qos,
        )
        self._open_client = self._node.create_client(
            OpenRobotLifecycleSession,
            config.open_session_service,
        )
        self._close_client = self._node.create_client(
            CloseRobotLifecycleSession,
            config.close_session_service,
        )
        self._submit_client = self._node.create_client(
            SubmitRobotLifecycleCommand,
            config.submit_command_service,
        )
        self._reconcile_client = self._node.create_client(
            ReconcileRobotLifecycleCommand,
            config.reconcile_command_service,
        )
        self._service_timeout_s = config.service_timeout_s
        self._executor = SingleThreadedExecutor(context=self._context)
        self._executor.add_node(self._node)
        self._thread = threading.Thread(
            target=self._executor.spin,
            name="manyforge-robot-lifecycle-ros",
            daemon=True,
        )
        self._thread.start()
        self._closed = False

    @staticmethod
    def _receive_message(
        message: Any,
        snapshot_sink: Callable[[dict[str, Any]], None],
    ) -> None:
        try:
            snapshot_sink(snapshot_message_payload(message))
        except Exception:
            # A malformed provider publication is treated as absent/stale by
            # the product client. Never let one bad DDS sample terminate the
            # executor thread and silently freeze all later status updates.
            _LOGGER.exception("Rejected malformed robot lifecycle snapshot")

    def _call(
        self,
        client: Any,
        request: Any,
    ) -> Any:
        if not client.wait_for_service(timeout_sec=self._service_timeout_s):
            raise RuntimeError("robot_lifecycle_service_unavailable")
        future = client.call_async(request)
        completed = threading.Event()
        future.add_done_callback(lambda _future: completed.set())
        if not completed.wait(self._service_timeout_s):
            try:
                future.cancel()
            except Exception:
                pass
            raise RuntimeError("robot_lifecycle_service_timeout")
        exception = future.exception()
        if exception is not None:
            raise RuntimeError("robot_lifecycle_service_error") from exception
        response = future.result()
        if response is None:
            raise RuntimeError("robot_lifecycle_service_empty_response")
        return response

    @staticmethod
    def _base_request(request: Any, payload: Mapping[str, Any]) -> None:
        request.robot_id = str(payload["robot_id"])
        request.command_source = str(payload["command_source"])
        request.caller_context_id = str(payload["caller_context_id"])
        request.caller_assurance = str(payload["caller_assurance"])

    def open_session(self, payload: Mapping[str, Any]) -> dict[str, Any]:
        service_type = self._service_types[0]
        request = service_type.Request()
        self._base_request(request, payload)
        request.session_id = str(payload["session_id"])
        request.session_generation = int(payload["session_generation"])
        request.authority_component_epoch = str(
            payload["authority_component_epoch"]
        )
        request.lease_duration_ns = int(payload["lease_duration_ns"])
        response = self._call(self._open_client, request)
        return {
            "accepted": bool(response.accepted),
            "reason": str(response.reason),
            "snapshotGeneration": int(response.snapshot_generation),
        }

    def close_session(self, payload: Mapping[str, Any]) -> dict[str, Any]:
        service_type = self._service_types[1]
        request = service_type.Request()
        self._base_request(request, payload)
        request.session_id = str(payload["session_id"])
        request.session_generation = int(payload["session_generation"])
        response = self._call(self._close_client, request)
        return {
            "accepted": bool(response.accepted),
            "reason": str(response.reason),
            "snapshotGeneration": int(response.snapshot_generation),
        }

    def submit_command(self, payload: Mapping[str, Any]) -> dict[str, Any]:
        service_type = self._service_types[2]
        request = service_type.Request()
        self._base_request(request, payload)
        request.session_id = str(payload["session_id"])
        request.session_generation = int(payload["session_generation"])
        request.request_id = str(payload["request_id"])
        request.request_sequence = int(payload["request_sequence"])
        request.action_id = str(payload["action_id"])
        request.expected_snapshot_generation = int(
            payload["expected_snapshot_generation"]
        )
        request.confirmed = bool(payload["confirmed"])
        response = self._call(self._submit_client, request)
        return {
            "accepted": bool(response.accepted),
            "disposition": str(response.disposition),
            "reason": str(response.reason),
            "transactionGeneration": int(response.transaction_generation),
            "transactionPhase": str(response.transaction_phase),
            "deadlineSteadyTimeNs": int(response.deadline_steady_time_ns),
            "snapshotGeneration": int(response.snapshot_generation),
        }

    def reconcile_command(
        self,
        payload: Mapping[str, Any],
    ) -> dict[str, Any]:
        service_type = self._service_types[3]
        request = service_type.Request()
        self._base_request(request, payload)
        request.session_id = str(payload["session_id"])
        request.session_generation = int(payload["session_generation"])
        request.transaction_generation = int(
            payload["transaction_generation"]
        )
        request.request_id = str(payload["request_id"])
        request.reason = str(payload["reason"])
        response = self._call(self._reconcile_client, request)
        return {
            "accepted": bool(response.accepted),
            "reason": str(response.reason),
            "snapshotGeneration": int(response.snapshot_generation),
        }

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        try:
            self._executor.shutdown(timeout_sec=1.0)
        finally:
            self._thread.join(timeout=1.0)
            self._node.destroy_node()
            self._context.shutdown()


class RobotLifecycleClient:
    """Serialized product operations over one lifecycle transport."""

    def __init__(
        self,
        config: RobotLifecycleConfig,
        *,
        authority_component_epoch: str,
        authority_health_probe: Callable[[], AuthorityHealthEvidence],
        status_sink: Callable[[dict[str, Any]], None] | None = None,
        transport_factory: Callable[
            [RobotLifecycleConfig, Callable[[dict[str, Any]], None]],
            LifecycleTransport,
        ] = RosLifecycleTransport,
        clock_ns: Callable[[], int] = time.monotonic_ns,
    ) -> None:
        if (
            not authority_component_epoch
            or len(authority_component_epoch) > 512
        ):
            raise ValueError("authority component epoch is invalid")
        self.config = config
        self.provider_id = config.provider_instance_id
        self.authority_provider_id = (
            f"{config.robot_id}:control-authority:local-operator"
        )
        self._authority_component_epoch = authority_component_epoch
        self._authority_health_probe = authority_health_probe
        self._clock_ns = clock_ns
        self._status_sink = status_sink
        self._snapshot_lock = threading.Condition()
        self._command_lock = threading.Lock()
        self._session_lock = threading.RLock()
        # Status reads must remain available while a session ROS RPC is
        # boundedly waiting. Session mutation always takes _session_lock
        # first, then this short-lived evidence lock; status takes only this
        # lock and therefore never waits behind transport I/O.
        self._authority_status_lock = threading.Lock()
        self._health_probe_lock = threading.Lock()
        self._snapshot: dict[str, Any] | None = None
        self._snapshot_received_ns = 0
        self._snapshot_delivery_sequence = 0
        self._provider_component_epoch = ""
        self._snapshot_terminal_reason = ""
        self._command_in_flight: dict[str, Any] | None = None
        self._last_composite_result: dict[str, Any] | None = None
        self._caller_context_id = (
            f"composer-authority:{self._authority_component_epoch}"
        )
        self._command_source = "composer.local_operator"
        self._session_id = (
            f"composer-session:{self._authority_component_epoch}:"
            f"{uuid.uuid4().hex}"
        )
        self._session_generation = 1
        self._session_established = False
        self._authority_status_session_established = False
        self._request_sequence = 0
        self._authority_health_sequence = 0
        self._authority_health_observed_ns = 0
        self._authority_health_healthy = False
        self._authority_health_reason = "authority_health_not_observed"
        self._authority_heartbeat_last_attempt_ns = 0
        self._authority_heartbeat_last_renewed_ns = 0
        self._authority_heartbeat_renewal_count = 0
        self._authority_heartbeat_failure_count = 0
        self._status_publish_lock = threading.Lock()
        self._last_status_signature = ""
        self._last_status_published_ns = 0
        self._closed = False
        self._transport = transport_factory(config, self._receive_snapshot)

    def _receive_snapshot(self, payload: dict[str, Any]) -> None:
        if payload.get("schemaVersion") != _SNAPSHOT_SCHEMA:
            return
        if payload.get("robotId") != self.config.robot_id:
            return
        if (
            payload.get("providerInstanceId")
            != self.config.provider_instance_id
        ):
            return
        provider_epoch = str(payload.get("providerComponentEpoch") or "")
        if (
            payload.get("runtimeGenerationId")
            != self.config.runtime_generation_id
            or payload.get("cellSupervisorComponentEpoch")
            != self.config.cell_supervisor_component_epoch
            or not provider_epoch
        ):
            with self._snapshot_lock:
                self._snapshot_terminal_reason = (
                    "robot_lifecycle_component_identity_mismatch"
                )
                self._snapshot_lock.notify_all()
            self._publish_status()
            return
        if self._provider_component_epoch:
            if provider_epoch != self._provider_component_epoch:
                with self._snapshot_lock:
                    self._snapshot_terminal_reason = (
                        "robot_lifecycle_provider_epoch_changed"
                    )
                    self._snapshot_lock.notify_all()
                self._publish_status()
                return
        else:
            self._provider_component_epoch = provider_epoch
        generation = int(payload.get("snapshotGeneration") or 0)
        provider_publication_sequence = int(
            payload.get("providerPublicationSequence") or 0
        )
        if generation <= 0 or provider_publication_sequence <= 0:
            return
        now_ns = self._clock_ns()
        accepted = False
        terminal_reason = ""
        with self._snapshot_lock:
            current = self._snapshot or {}
            current_provider_publication_sequence = int(
                current.get("providerPublicationSequence") or 0
            )
            if (
                provider_publication_sequence
                <= current_provider_publication_sequence
            ):
                return
            current_generation = int(
                current.get("snapshotGeneration") or 0
            )
            if generation < current_generation:
                terminal_reason = (
                    "robot_lifecycle_provider_generation_regressed"
                )
            observation_generation = int(
                payload.get("observationGeneration") or 0
            )
            current_observation_generation = int(
                current.get("observationGeneration") or 0
            )
            if (
                not terminal_reason
                and current_observation_generation > 0
                and observation_generation
                < current_observation_generation
            ):
                terminal_reason = (
                    "robot_lifecycle_observation_generation_regressed"
                )
            source_ns = int(payload.get("sourceSteadyTimeNs") or 0)
            current_source_ns = int(
                current.get("sourceSteadyTimeNs") or 0
            )
            if (
                not terminal_reason
                and current_source_ns > 0
                and source_ns < current_source_ns
            ):
                terminal_reason = (
                    "robot_lifecycle_observation_time_regressed"
                )
            if terminal_reason:
                self._snapshot_terminal_reason = terminal_reason
                self._snapshot_lock.notify_all()
            else:
                # The provider publication sequence is the source-owned proof
                # that its status callback progressed. A repeated DDS sample
                # keeps the same sequence and is ignored. Robot observation
                # timestamps may legitimately remain unchanged during one
                # bounded lifecycle primitive; that closes motion freshness
                # without declaring the provider dead.
                self._snapshot_delivery_sequence += 1
                self._snapshot = {
                    **payload,
                    "providerDeliverySequence": (
                        self._snapshot_delivery_sequence
                    ),
                    "providerDeliveryReceivedSteadyTimeNs": now_ns,
                }
                self._snapshot_received_ns = now_ns
                self._snapshot_lock.notify_all()
                accepted = True
        if accepted or terminal_reason:
            self._publish_status()

    def _current_snapshot_health(
        self,
    ) -> tuple[dict[str, Any] | None, bool, bool]:
        """Return delivery health separately from motion-state freshness.

        Provider delivery is component-liveness evidence.  Observation
        freshness is the stricter, robot-state deadline used by command and
        execution admission.  A live provider may therefore be healthy while
        its latest observation has just expired; that state must fence motion
        without falsely declaring the provider process lost.
        """
        now_ns = self._clock_ns()
        with self._snapshot_lock:
            snapshot = dict(self._snapshot) if self._snapshot else None
            received_ns = self._snapshot_received_ns
        provider_healthy = bool(
            snapshot
            and not self._snapshot_terminal_reason
            and received_ns > 0
            and now_ns - received_ns <= self.config.status_delivery_max_age_ns
        )
        fresh = bool(
            provider_healthy
            and int(snapshot.get("freshUntilSteadyTimeNs") or 0) > now_ns
            and snapshot.get("observationFresh") is True
        )
        return snapshot, provider_healthy, fresh

    def _current_snapshot(self) -> tuple[dict[str, Any] | None, bool]:
        snapshot, _provider_healthy, fresh = (
            self._current_snapshot_health()
        )
        return snapshot, fresh

    def _authority_health_status(self) -> dict[str, Any]:
        now_ns = self._clock_ns()
        with self._authority_status_lock:
            observed_ns = self._authority_health_observed_ns
            age_ns = (
                max(0, now_ns - observed_ns) if observed_ns > 0 else None
            )
            fresh = bool(
                self._authority_health_healthy
                and age_ns is not None
                and age_ns <= self.config.authority_health_max_age_ns
            )
            reason = (
                self._authority_health_reason
                if fresh
                else "authority_critical_progress_stale"
                if observed_ns > 0 and self._authority_health_healthy
                else self._authority_health_reason
            )
            return {
                "componentEpoch": self._authority_component_epoch,
                "sequence": self._authority_health_sequence,
                "observedAtSteadyTimeNs": observed_ns,
                "ageNs": age_ns,
                "healthy": fresh,
                "reason": reason,
                "sessionEstablished": (
                    self._authority_status_session_established
                ),
                "lastAttemptSteadyTimeNs": (
                    self._authority_heartbeat_last_attempt_ns
                ),
                "lastRenewedSteadyTimeNs": (
                    self._authority_heartbeat_last_renewed_ns
                ),
                "renewalCount": self._authority_heartbeat_renewal_count,
                "failureCount": self._authority_heartbeat_failure_count,
            }

    @staticmethod
    def _capability(
        snapshot: Mapping[str, Any],
        action_id: str,
    ) -> Mapping[str, Any] | None:
        return next(
            (
                value
                for value in snapshot.get("capabilities", [])
                if value.get("actionId") == action_id
            ),
            None,
        )

    def status(self) -> dict[str, Any]:
        snapshot, provider_healthy, fresh = self._current_snapshot_health()
        authority_health = self._authority_health_status()
        if snapshot is None:
            return {
                "schemaVersion": "manyforge.robot_lifecycle.ui_status.v1",
                "configured": True,
                "providerId": self.provider_id,
                "authorityProviderId": self.authority_provider_id,
                "robotId": self.config.robot_id,
                "providerHealthy": False,
                "fresh": False,
                "readiness": "waiting",
                "canExecute": False,
                "authorityHealthy": authority_health["healthy"],
                "authorityHealth": authority_health,
                "userMessage": "Waiting for the robot lifecycle provider.",
                "commandInFlight": self._command_in_flight,
                "lastCompositeResult": self._last_composite_result,
            }
        if self._snapshot_terminal_reason:
            return {
                **snapshot,
                "schemaVersion": "manyforge.robot_lifecycle.ui_status.v1",
                "configured": True,
                "providerId": self.provider_id,
                "authorityProviderId": self.authority_provider_id,
                "robotId": self.config.robot_id,
                "providerHealthy": False,
                "fresh": False,
                "readiness": "offline",
                "canExecute": False,
                "authorityHealthy": authority_health["healthy"],
                "authorityHealth": authority_health,
                "userMessage": self._snapshot_terminal_reason,
                "commandInFlight": self._command_in_flight,
                "lastCompositeResult": self._last_composite_result,
            }
        fault = (
            snapshot.get("faultPresent") == "true"
            or snapshot.get("recoveryRequired") == "true"
            or snapshot.get("reconciliationRequired") is True
        )
        warning = snapshot.get("warningPresent") == "true"
        authority_expected = bool(
            authority_health["sessionEstablished"]
            or snapshot.get("motionAdmitted") is True
        )
        authority_healthy = authority_health["healthy"] is True
        authority_gate_healthy = bool(
            not authority_expected or authority_healthy
        )
        can_execute = bool(
            fresh
            and authority_gate_healthy
            and snapshot.get("motionAdmitted") is True
            and not fault
            and not warning
        )
        readiness = (
            "offline"
            if not provider_healthy
            else "waiting"
            if not fresh
            else "degraded"
            if not authority_gate_healthy
            else "fault"
            if fault
            else "degraded"
            if warning
            else "ready"
            if can_execute
            else "waiting"
        )
        user_message = (
            "Robot lifecycle provider delivery is stale."
            if not provider_healthy
            else "Robot observation is not fresh; motion remains fenced."
            if not fresh
            else (
                "The selected headless control authority is unhealthy; "
                "motion remains fenced."
            )
            if not authority_gate_healthy
            else "Robot fault or uncertain lifecycle outcome requires attention."
            if fault
            else "Robot warning must be cleared before enabling motion."
            if warning
            else "Robot is enabled and trajectory execution is admitted."
            if can_execute
            else "Robot is connected but trajectory execution is not enabled."
        )
        result = dict(snapshot)
        result.update(
            {
                "schemaVersion": "manyforge.robot_lifecycle.ui_status.v1",
                "configured": True,
                "providerId": self.provider_id,
                "authorityProviderId": self.authority_provider_id,
                "providerHealthy": provider_healthy,
                "authorityHealthy": authority_healthy,
                "authorityHealth": authority_health,
                "fresh": fresh,
                "readiness": readiness,
                "canExecute": can_execute,
                "userMessage": user_message,
                "commandInFlight": self._command_in_flight,
                "lastCompositeResult": self._last_composite_result,
            }
        )
        return result

    def runtime_status(self) -> dict[str, Any]:
        status = self.status()
        return {
            "readiness": status["readiness"],
            "providerHealthy": status["providerHealthy"],
            "canExecute": status["canExecute"],
            "userMessage": status["userMessage"],
            "components": [
                {
                    "id": self.provider_id,
                    "label": f"Robot lifecycle ({self.config.robot_id})",
                    "kind": "robot_lifecycle",
                    # A healthy provider with a disabled robot is still
                    # waiting for execution admission, not execution-ready.
                    "status": status["readiness"],
                    "requiredFor": ["execute"],
                    "detail": status["userMessage"],
                },
                {
                    "id": self.authority_provider_id,
                    "label": "Headless local control authority",
                    "kind": "control_authority",
                    "status": (
                        "ready"
                        if status["authorityHealthy"]
                        else "waiting"
                        if status["authorityHealth"]["sequence"] == 0
                        else "offline"
                    ),
                    "requiredFor": ["execute"],
                    "detail": status["authorityHealth"]["reason"],
                },
            ],
        }

    @staticmethod
    def _status_signature(status: Mapping[str, Any]) -> str:
        ignored = {
            "snapshotGeneration",
            "observationGeneration",
            "sourceSteadyTimeNs",
            "receivedSteadyTimeNs",
            "freshUntilSteadyTimeNs",
            "authorityValidUntilSteadyTimeNs",
        }
        material = {
            key: value for key, value in status.items() if key not in ignored
        }
        authority_health = material.get("authorityHealth")
        if isinstance(authority_health, Mapping):
            material["authorityHealth"] = {
                key: value
                for key, value in authority_health.items()
                if key
                not in {
                    "sequence",
                    "observedAtSteadyTimeNs",
                    "ageNs",
                    "lastAttemptSteadyTimeNs",
                    "lastRenewedSteadyTimeNs",
                    "renewalCount",
                    "failureCount",
                }
            }
        return json.dumps(
            material,
            sort_keys=True,
            separators=(",", ":"),
            default=str,
        )

    def _publish_status(self, *, force: bool = False) -> None:
        if self._status_sink is None:
            return
        status = self.status()
        signature = self._status_signature(status)
        now_ns = self._clock_ns()
        with self._status_publish_lock:
            if (
                not force
                and signature == self._last_status_signature
                and now_ns - self._last_status_published_ns
                < _STATUS_HEARTBEAT_NS
            ):
                return
            self._last_status_signature = signature
            self._last_status_published_ns = now_ns
        self._status_sink(status)

    def _base_payload(self) -> dict[str, Any]:
        return {
            "robot_id": self.config.robot_id,
            "command_source": self._command_source,
            "caller_context_id": self._caller_context_id,
            "caller_assurance": self.config.caller_assurance,
        }

    @property
    def authority_heartbeat_period_s(self) -> float:
        return self.config.authority_heartbeat_period_ns / 1_000_000_000

    def _sample_authority_health(self) -> tuple[bool, str]:
        with self._health_probe_lock:
            probe_started_ns = self._clock_ns()
            try:
                evidence = self._authority_health_probe()
            except Exception as exc:
                reason = (
                    "authority_health_probe_failed:"
                    f"{str(exc) or type(exc).__name__}"
                )
                with self._authority_status_lock:
                    self._authority_health_healthy = False
                    self._authority_health_reason = reason[:1024]
                return False, reason
            # Sample receiver time after the probe. A same-process probe
            # legitimately stamps its evidence during the call.
            now_ns = self._clock_ns()
            with self._authority_status_lock:
                current_sequence = self._authority_health_sequence
            if not isinstance(evidence, AuthorityHealthEvidence):
                reason = "authority_health_evidence_invalid"
            elif evidence.component_epoch != self._authority_component_epoch:
                reason = "authority_component_epoch_mismatch"
            elif evidence.sequence <= current_sequence:
                reason = "authority_health_sequence_not_increasing"
            elif evidence.observed_at_ns <= 0 or evidence.observed_at_ns > now_ns:
                reason = "authority_health_timestamp_invalid"
            elif (
                now_ns - probe_started_ns
                > self.config.authority_health_max_age_ns
            ):
                reason = "authority_critical_progress_probe_overrun"
            elif (
                now_ns - evidence.observed_at_ns
                > self.config.authority_health_max_age_ns
            ):
                reason = "authority_critical_progress_stale"
            elif not evidence.healthy:
                reason = evidence.reason or "authority_health_unhealthy"
            else:
                reason = ""
            evidence_belongs_to_epoch = bool(
                isinstance(evidence, AuthorityHealthEvidence)
                and evidence.component_epoch == self._authority_component_epoch
                and evidence.sequence > current_sequence
                and evidence.observed_at_ns > 0
                and evidence.observed_at_ns <= now_ns
            )
            with self._authority_status_lock:
                # Invalid or foreign-epoch evidence fails closed but cannot
                # advance/poison this component epoch's replay fence.
                if evidence_belongs_to_epoch:
                    self._authority_health_sequence = evidence.sequence
                    self._authority_health_observed_ns = evidence.observed_at_ns
                self._authority_health_healthy = not reason
                self._authority_health_reason = reason or "healthy"
            return not reason, reason

    def _renewal_margin_ns(self) -> int:
        return max(
            self.config.authority_heartbeat_period_ns * 2,
            self.config.authority_lease_ns // 2,
        )

    def _set_session_established_locked(self, established: bool) -> None:
        """Update session ownership and its non-blocking status projection.

        The caller owns _session_lock. The mirrored status fact deliberately
        lives under a separate, short critical section so health endpoints do
        not contend with ROS service waits.
        """
        self._session_established = established
        with self._authority_status_lock:
            self._authority_status_session_established = established

    def _open_or_renew_session_locked(
        self,
        snapshot: Mapping[str, Any],
        *,
        allow_new_session: bool,
    ) -> bool:
        now_ns = self._clock_ns()
        owns_current_session = (
            snapshot.get("authoritySessionId") == self._session_id
            and int(snapshot.get("authoritySessionGeneration") or 0)
            == self._session_generation
        )
        current_session_fresh = bool(
            owns_current_session
            and snapshot.get("authorityFresh") is True
            and int(snapshot.get("authorityValidUntilSteadyTimeNs") or 0)
            > now_ns
        )
        created_new_session = False
        if (
            current_session_fresh
            and int(snapshot.get("authorityValidUntilSteadyTimeNs") or 0)
            > now_ns + self._renewal_margin_ns()
        ):
            self._set_session_established_locked(True)
            return False
        if current_session_fresh:
            # Renewal advances freshness only. It deliberately retains the
            # authority epoch, session generation, and replay domain.
            session_id = self._session_id
            session_generation = self._session_generation
        elif allow_new_session:
            created_new_session = True
            self._session_generation += 1
            self._session_id = (
                f"composer-session:{self._authority_component_epoch}:"
                f"{uuid.uuid4().hex}"
            )
            session_id = self._session_id
            session_generation = self._session_generation
        else:
            self._set_session_established_locked(False)
            raise RuntimeError("authority_session_lost")
        payload = self._base_payload()
        payload.update(
            {
                "session_id": session_id,
                "session_generation": session_generation,
                "authority_component_epoch": (
                    self._authority_component_epoch
                ),
                "lease_duration_ns": self.config.authority_lease_ns,
            }
        )
        response = self._transport.open_session(payload)
        if not response.get("accepted"):
            self._set_session_established_locked(False)
            raise RuntimeError(
                str(response.get("reason") or "authority_session_rejected")
            )
        if created_new_session:
            # Request sequences are scoped to one authority-session
            # generation. A new generation starts at sequence one; renewing
            # the same generation preserves its replay domain.
            self._request_sequence = 0
        minimum_valid_until_ns = now_ns + self.config.authority_lease_ns // 2
        self._wait_for_snapshot(
            lambda value: (
                value.get("authorityFresh") is True
                and value.get("authoritySessionId") == session_id
                and int(value.get("authoritySessionGeneration") or 0)
                == session_generation
                and int(value.get("authorityValidUntilSteadyTimeNs") or 0)
                >= minimum_valid_until_ns
            ),
            timeout_s=1.0,
            reason="authority_session_status_timeout",
        )
        self._set_session_established_locked(True)
        with self._authority_status_lock:
            self._authority_heartbeat_last_renewed_ns = self._clock_ns()
            self._authority_heartbeat_renewal_count += 1
        return True

    def _ensure_session(self, snapshot: Mapping[str, Any]) -> None:
        healthy, reason = self._sample_authority_health()
        if not healthy:
            raise RuntimeError(reason)
        with self._session_lock:
            if self._closed:
                raise RuntimeError("robot_lifecycle_client_closed")
            self._open_or_renew_session_locked(
                snapshot,
                allow_new_session=True,
            )

    def _revoke_owned_session_locked(self, *, reason: str) -> None:
        snapshot, _fresh = self._current_snapshot()
        owns = bool(
            snapshot is not None
            and snapshot.get("authoritySessionId") == self._session_id
            and int(snapshot.get("authoritySessionGeneration") or 0)
            == self._session_generation
        )
        self._set_session_established_locked(False)
        if not owns:
            return
        payload = self._base_payload()
        payload.update(
            {
                "session_id": self._session_id,
                "session_generation": self._session_generation,
            }
        )
        try:
            self._transport.close_session(payload)
        except Exception:
            _LOGGER.warning(
                "Failed to close unhealthy robot authority session",
                extra={
                    "robot_id": self.config.robot_id,
                    "reason": reason,
                },
                exc_info=True,
            )

    def authority_heartbeat_tick(self) -> dict[str, Any]:
        """Renew one established authority epoch from critical-path evidence.

        This method never creates a session. An expired/replaced session needs
        a later explicit lifecycle command and therefore cannot auto-rearm
        motion after a health fault.
        """
        now_ns = self._clock_ns()
        with self._session_lock:
            if self._closed:
                return {"status": "closed", "renewed": False}
            with self._authority_status_lock:
                self._authority_heartbeat_last_attempt_ns = now_ns
        healthy, reason = self._sample_authority_health()
        with self._session_lock:
            if self._closed:
                return {"status": "closed", "renewed": False}
            if not healthy:
                with self._authority_status_lock:
                    self._authority_heartbeat_failure_count += 1
                self._revoke_owned_session_locked(reason=reason)
                result = {
                    "status": "unhealthy",
                    "reason": reason,
                    "renewed": False,
                }
            elif not self._session_established:
                result = {"status": "inactive", "renewed": False}
            else:
                snapshot, provider_healthy, _observation_fresh = (
                    self._current_snapshot_health()
                )
                if snapshot is None or not provider_healthy:
                    with self._authority_status_lock:
                        self._authority_heartbeat_failure_count += 1
                    self._set_session_established_locked(False)
                    result = {
                        "status": "provider_status_stale",
                        "renewed": False,
                    }
                elif snapshot.get("transportOwnerProgressHealthy") is not True:
                    reason = str(
                        snapshot.get("transportOwnerProgressReason")
                        or "transport_owner_progress_unhealthy"
                    )
                    received_ns = int(
                        snapshot.get(
                            "transportOwnerProgressReceivedSteadyTimeNs"
                        )
                        or 0
                    )
                    progress_age_ns = (
                        now_ns - received_ns
                        if 0 < received_ns <= now_ns
                        else self.config.authority_health_max_age_ns + 1
                    )
                    if (
                        reason in _RECOVERABLE_OWNER_PROGRESS_REASONS
                        and progress_age_ns
                        <= self.config.authority_health_max_age_ns
                    ):
                        # Motion admission is already closed by the provider.
                        # Retain (but do not renew) this authority epoch while
                        # the outer component-progress budget decides whether
                        # the same owner actually stopped progressing.
                        result = {
                            "status": "transport_owner_progress_pending",
                            "reason": reason,
                            "progressAgeNs": progress_age_ns,
                            "renewed": False,
                        }
                    else:
                        with self._authority_status_lock:
                            self._authority_heartbeat_failure_count += 1
                        self._revoke_owned_session_locked(reason=reason)
                        result = {
                            "status": "transport_owner_progress_unhealthy",
                            "reason": reason,
                            "renewed": False,
                        }
                else:
                    try:
                        renewed = self._open_or_renew_session_locked(
                            snapshot,
                            allow_new_session=False,
                        )
                        result = {
                            "status": "renewed" if renewed else "healthy",
                            "renewed": renewed,
                        }
                    except Exception as exc:
                        with self._authority_status_lock:
                            self._authority_heartbeat_failure_count += 1
                        self._set_session_established_locked(False)
                        result = {
                            "status": "renewal_failed",
                            "reason": str(exc) or type(exc).__name__,
                            "renewed": False,
                        }
        self._publish_status()
        return result

    def _wait_for_snapshot(
        self,
        predicate: Callable[[Mapping[str, Any]], bool],
        *,
        timeout_s: float,
        reason: str,
    ) -> dict[str, Any]:
        deadline = time.monotonic() + timeout_s
        with self._snapshot_lock:
            while True:
                snapshot = self._snapshot
                if snapshot is not None and predicate(snapshot):
                    return dict(snapshot)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError(reason)
                self._snapshot_lock.wait(timeout=min(remaining, 0.1))

    def _wait_terminal(
        self,
        request_id: str,
        *,
        timeout_s: float,
    ) -> dict[str, Any]:
        deadline = time.monotonic() + timeout_s
        with self._snapshot_lock:
            while True:
                snapshot = self._snapshot
                terminal = (
                    snapshot.get("lastTerminalTransaction")
                    if snapshot is not None
                    else None
                )
                if (
                    isinstance(terminal, Mapping)
                    and terminal.get("requestId") == request_id
                    and terminal.get("phase") in _TERMINAL_PHASES
                ):
                    return dict(terminal)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError(
                        "robot_lifecycle_terminal_status_timeout"
                    )
                self._snapshot_lock.wait(timeout=min(remaining, 0.1))

    def _submit(self, action_id: str, *, confirmed: bool = True) -> dict[str, Any]:
        snapshot, fresh = self._current_snapshot()
        if snapshot is None or not fresh:
            raise RuntimeError("robot_lifecycle_status_unavailable_or_stale")
        if snapshot.get("reconciliationRequired") is True:
            raise RuntimeError("robot_lifecycle_reconciliation_required")
        capability = self._capability(snapshot, action_id)
        if capability is None or not capability.get("supported"):
            raise RuntimeError(f"robot_lifecycle_action_unsupported:{action_id}")
        if not capability.get("available"):
            reason = capability.get("availabilityReason") or "unavailable"
            raise RuntimeError(
                f"robot_lifecycle_action_unavailable:{action_id}:{reason}"
            )
        self._ensure_session(snapshot)
        # Establishing or renewing a session changes the authoritative
        # snapshot generation.
        snapshot, fresh = self._current_snapshot()
        if snapshot is None or not fresh:
            raise RuntimeError("robot_lifecycle_status_unavailable_after_session")
        snapshot = self._wait_for_snapshot(
            lambda value: (
                value.get("authorityFresh") is True
                and value.get("authoritySessionId") == self._session_id
                and int(value.get("authoritySessionGeneration") or 0)
                == self._session_generation
            ),
            timeout_s=1.0,
            reason="authority_session_status_timeout",
        )
        snapshot, fresh = self._current_snapshot()
        if snapshot is None or not fresh:
            raise RuntimeError("robot_lifecycle_status_unavailable_after_session")
        self._request_sequence += 1
        request_id = (
            f"composer:{self._session_generation}:"
            f"{self._request_sequence}:{uuid.uuid4().hex[:12]}"
        )
        payload = self._base_payload()
        payload.update(
            {
                "session_id": self._session_id,
                "session_generation": self._session_generation,
                "request_id": request_id,
                "request_sequence": self._request_sequence,
                "action_id": action_id,
                "expected_snapshot_generation": int(
                    snapshot["snapshotGeneration"]
                ),
                "confirmed": confirmed,
            }
        )
        response = self._transport.submit_command(payload)
        if not response.get("accepted"):
            raise RuntimeError(
                str(response.get("reason") or "lifecycle_command_rejected")
            )
        capability_timeout_ns = int(capability.get("timeoutNs") or 0)
        terminal_timeout_s = max(
            self.config.transaction_timeout_s,
            capability_timeout_ns / 1_000_000_000 + 1.0,
        )
        terminal = self._wait_terminal(
            request_id,
            timeout_s=terminal_timeout_s,
        )
        if terminal.get("phase") != "succeeded":
            raise RuntimeError(
                str(
                    terminal.get("resultReason")
                    or f"lifecycle_command_{terminal.get('phase')}"
                )
            )
        return {
            "actionId": action_id,
            "requestId": request_id,
            "transaction": terminal,
        }

    def _run_target(
        self,
        operation: str,
        action_id: str,
        *,
        completion_check: Callable[[], None] | None = None,
    ) -> dict[str, Any]:
        if not self._command_lock.acquire(blocking=False):
            raise RuntimeError("robot_lifecycle_command_in_progress")
        started_ns = time.monotonic_ns()
        self._command_in_flight = {
            "operation": operation,
            "startedAtNs": started_ns,
            "stage": action_id,
        }
        self._publish_status()
        try:
            target = self._submit(action_id)
            if completion_check is not None:
                completion_check()
            result = {
                "operation": operation,
                "success": True,
                "startedAtNs": started_ns,
                "completedAtNs": time.monotonic_ns(),
                "targetActionId": action_id,
                # Preserve the established response shape while making clear
                # that the sole public transaction is the target intent.
                "stages": [target],
            }
            self._last_composite_result = result
            return result
        except Exception as exc:
            self._last_composite_result = {
                "operation": operation,
                "success": False,
                "startedAtNs": started_ns,
                "completedAtNs": time.monotonic_ns(),
                "targetActionId": action_id,
                "stages": [],
                "reason": str(exc) or type(exc).__name__,
            }
            raise
        finally:
            self._command_in_flight = None
            self._command_lock.release()
            self._publish_status()

    def enable(self) -> dict[str, Any]:
        def require_motion_admission() -> None:
            snapshot = self._wait_for_snapshot(
                lambda value: value.get("motionAdmitted") is True,
                timeout_s=1.5,
                reason="motion_admission_not_open_after_enable",
            )
            if snapshot.get("motionAdmitted") is not True:
                raise RuntimeError(
                    "motion_admission_closed:"
                    + ",".join(snapshot.get("admissionReasonCodes", []))
                )

        return self._run_target(
            "enable",
            _PREPARE_FOR_MOTION_ACTION,
            completion_check=require_motion_admission,
        )

    def disable(self) -> dict[str, Any]:
        return self._run_target(
            "disable",
            _MAKE_NONCOMMANDABLE_ACTION,
        )

    def recovery_action(self, action_id: str) -> dict[str, Any]:
        if action_id not in {
            "robot.clear_fault",
            "robot.clear_warning",
            "shutdown_controller",
        }:
            raise ValueError("robot lifecycle recovery action is unsupported")
        if not self._command_lock.acquire(blocking=False):
            raise RuntimeError("robot_lifecycle_command_in_progress")
        try:
            return self._submit(action_id)
        finally:
            self._command_lock.release()

    def reconcile(self) -> dict[str, Any]:
        if not self._command_lock.acquire(blocking=False):
            raise RuntimeError("robot_lifecycle_command_in_progress")
        try:
            snapshot, fresh = self._current_snapshot()
            if snapshot is None or not fresh:
                raise RuntimeError(
                    "robot_lifecycle_status_unavailable_or_stale"
                )
            pending = snapshot.get("pendingTransaction")
            if (
                snapshot.get("reconciliationRequired") is not True
                or not isinstance(pending, Mapping)
            ):
                raise RuntimeError("robot_lifecycle_reconciliation_not_required")
            self._ensure_session(snapshot)
            payload = self._base_payload()
            payload.update(
                {
                    "session_id": self._session_id,
                    "session_generation": self._session_generation,
                    "transaction_generation": int(pending["generation"]),
                    "request_id": str(pending["requestId"]),
                    "reason": "operator_confirmed_observed_postcondition",
                }
            )
            response = self._transport.reconcile_command(payload)
            if not response.get("accepted"):
                raise RuntimeError(
                    str(
                        response.get("reason")
                        or "robot_lifecycle_reconciliation_rejected"
                    )
                )
            return response
        finally:
            self._command_lock.release()

    def close(self) -> None:
        with self._session_lock:
            if self._closed:
                return
            self._closed = True
            try:
                snapshot, _fresh = self._current_snapshot()
                if (
                    snapshot is not None
                    and snapshot.get("authoritySessionId") == self._session_id
                ):
                    payload = self._base_payload()
                    payload.update(
                        {
                            "session_id": self._session_id,
                            "session_generation": self._session_generation,
                        }
                    )
                    try:
                        self._transport.close_session(payload)
                    except Exception:
                        pass
                self._set_session_established_locked(False)
            finally:
                self._transport.close()


class UnavailableRobotLifecycleClient:
    """Fail-closed status object retained when ROS ingress cannot start."""

    def __init__(
        self,
        config: RobotLifecycleConfig,
        reason: str,
    ) -> None:
        self.config = config
        self.provider_id = config.provider_instance_id
        self.authority_provider_id = (
            f"{config.robot_id}:control-authority:local-operator"
        )
        self.reason = reason[:1024]

    def status(self) -> dict[str, Any]:
        return {
            "schemaVersion": "manyforge.robot_lifecycle.ui_status.v1",
            "configured": True,
            "providerId": self.provider_id,
            "authorityProviderId": self.authority_provider_id,
            "robotId": self.config.robot_id,
            "providerHealthy": False,
            "fresh": False,
            "readiness": "offline",
            "canExecute": False,
            "userMessage": self.reason,
            "capabilities": [],
            "commandInFlight": None,
            "lastCompositeResult": None,
        }

    def runtime_status(self) -> dict[str, Any]:
        status = self.status()
        return {
            "readiness": "offline",
            "providerHealthy": False,
            "canExecute": False,
            "userMessage": status["userMessage"],
            "components": [
                {
                    "id": self.provider_id,
                    "label": f"Robot lifecycle ({self.config.robot_id})",
                    "kind": "robot_lifecycle",
                    "status": "offline",
                    "requiredFor": ["execute"],
                    "detail": status["userMessage"],
                }
            ],
        }

    def enable(self) -> dict[str, Any]:
        raise RuntimeError(self.reason)

    def disable(self) -> dict[str, Any]:
        raise RuntimeError(self.reason)

    def recovery_action(self, _action_id: str) -> dict[str, Any]:
        raise RuntimeError(self.reason)

    def reconcile(self) -> dict[str, Any]:
        raise RuntimeError(self.reason)

    def close(self) -> None:
        return
