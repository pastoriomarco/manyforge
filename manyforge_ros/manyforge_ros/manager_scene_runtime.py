"""Strict same-user client for Manager-owned scene-runtime observations.

The client is deliberately mechanical.  Manager assigns receive time,
freshness, epoch order, acceptance, retention, and publication generation;
ROS producers provide only source identity, sequence, lifetime intent, quality,
eligibility, and a bounded canonical observation payload.
"""

from __future__ import annotations

from contextlib import contextmanager
from dataclasses import dataclass, field
import json
import math
import os
from pathlib import Path
import socket
import stat
import struct
from typing import Any, Iterator, Mapping
import unicodedata

import rfc8785


MANAGER_SOCKET_FILE = "manager-command.sock"
# Stays v1 for the coordinated unreleased 128/130 cut (agreed with Track 2).
MANAGER_COMMAND_VERSION = "manyforge.manager_command.v1"
MANAGER_RESPONSE_VERSION = "manyforge.manager_command_response.v1"
MAX_MANAGER_REQUEST_BYTES = 128 * 1024 + 4 * 1024
MAX_MANAGER_RESPONSE_BYTES = 64 * 1024
MAX_OBSERVATION_PAYLOAD_BYTES = 64 * 1024
MAX_OBSERVATION_LIFETIME_NS = 60_000_000_000
MAXIMUM_SAFE_INTEGER = 9_007_199_254_740_991
DEFAULT_TIMEOUT_S = 2.0

_MAX_IDENTITY_BYTES = 256
_MAX_ERROR_MESSAGE_BYTES = 4096
_RUNTIME_STATES = {
    "open",
    "closed",
    "resetting",
    "seeding",
    "replaying",
    "failed",
    "retired",
}
_OBSERVATION_DISPOSITIONS = {
    "apply_now",
    "buffered",
    "replaced_buffered",
    "observed_only",
    "duplicate",
}
_SCENE_APPLICATION_STATUSES = {
    "already_applied",
    "buffered",
    "committed",
    "deferred_before_planning",
    "deferred_between_nodes",
    "not_requested",
    "skipped_attached",
    "unchanged",
}
_SCENE_UPDATE_STATUSES = {"freshness_only", "geometry_changed", "no_op"}


class SceneRuntimeManagerError(RuntimeError):
    """One stable fail-closed transport, protocol, or owner rejection."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class SceneRuntimeIdentity:
    retained_bundle_id: str
    graph_revision: str
    deployment_generation: int
    program_generation: int
    runtime_generation: int

    def __post_init__(self) -> None:
        _bounded_text(self.retained_bundle_id, label="retained_bundle_id")
        _bounded_text(self.graph_revision, label="graph_revision")
        _safe_integer(
            self.deployment_generation,
            label="deployment_generation",
            allow_zero=True,
        )
        _safe_integer(
            self.program_generation,
            label="program_generation",
            allow_zero=True,
        )
        _safe_integer(self.runtime_generation, label="runtime_generation")

    @classmethod
    def from_value(cls, value: object) -> SceneRuntimeIdentity:
        record = _exact_object(
            value,
            {
                "deployment_generation",
                "graph_revision",
                "program_generation",
                "retained_bundle_id",
                "runtime_generation",
            },
            label="scene-runtime identity",
        )
        try:
            return cls(
                retained_bundle_id=record["retained_bundle_id"],
                graph_revision=record["graph_revision"],
                deployment_generation=record["deployment_generation"],
                program_generation=record["program_generation"],
                runtime_generation=record["runtime_generation"],
            )
        except (TypeError, ValueError) as exc:
            raise _response_invalid("scene-runtime identity is invalid") from exc

    def to_value(self) -> dict[str, object]:
        return {
            "deployment_generation": self.deployment_generation,
            "graph_revision": self.graph_revision,
            "program_generation": self.program_generation,
            "retained_bundle_id": self.retained_bundle_id,
            "runtime_generation": self.runtime_generation,
        }


@dataclass(frozen=True)
class SceneProjectionRevision:
    retained_bundle_id: str
    graph_revision: str
    scene_id: str
    revision_id: int
    content_hash: str

    @classmethod
    def from_value(cls, value: object) -> SceneProjectionRevision:
        record = _exact_object(
            value,
            {"graph_revision", "retained_bundle_id", "scene_revision"},
            label="scene projection revision",
        )
        scene = _exact_object(
            record["scene_revision"],
            {"content_hash", "revision_id", "scene_id"},
            label="scene revision",
        )
        try:
            return cls(
                retained_bundle_id=_sha256(
                    record["retained_bundle_id"], label="retained_bundle_id"
                ),
                graph_revision=_sha256(
                    record["graph_revision"], label="graph_revision"
                ),
                scene_id=_sha256(scene["scene_id"], label="scene_id"),
                revision_id=_safe_integer(
                    scene["revision_id"], label="scene revision_id"
                ),
                content_hash=_sha256(
                    scene["content_hash"], label="scene content_hash"
                ),
            )
        except (TypeError, ValueError) as exc:
            raise _response_invalid("scene projection revision is invalid") from exc


@dataclass(frozen=True)
class SceneRuntimeOwnerSnapshot:
    cell_id: str
    owner_epoch: str
    runtime_identity: SceneRuntimeIdentity
    initialization_generation: int
    initialization_state: str


@dataclass(frozen=True)
class PoseObserverSession:
    cell_id: str
    runtime_identity: SceneRuntimeIdentity
    source_id: str
    source_epoch: str
    source_epoch_order: int


@dataclass(frozen=True, order=True)
class PoseObserverBinding:
    object_id: str
    policy_id: str
    eligible: bool

    def __post_init__(self) -> None:
        _bounded_text(self.object_id, label="object_id")
        _bounded_text(self.policy_id, label="policy_id")
        if type(self.eligible) is not bool:
            raise TypeError("eligible must be an exact bool")

    def to_value(self) -> dict[str, object]:
        return {
            "eligible": self.eligible,
            "object_id": self.object_id,
            "policy_id": self.policy_id,
        }


@dataclass(frozen=True)
class PoseObservationIntent:
    object_id: str
    policy_id: str
    lifetime_ns: int
    quality: str
    eligible: bool
    payload: Mapping[str, Any]
    _canonical_payload: bytes = field(init=False, repr=False, compare=True)

    def __post_init__(self) -> None:
        _bounded_text(self.object_id, label="object_id")
        _bounded_text(self.policy_id, label="policy_id")
        _safe_integer(self.lifetime_ns, label="lifetime_ns")
        if self.lifetime_ns > MAX_OBSERVATION_LIFETIME_NS:
            raise ValueError("lifetime_ns exceeds the Manager observation bound")
        if self.quality not in {"good", "unknown"}:
            raise ValueError("quality must be good or unknown")
        if type(self.eligible) is not bool:
            raise TypeError("eligible must be an exact bool")
        if not isinstance(self.payload, Mapping):
            raise TypeError("payload must be an object mapping")
        try:
            encoded = rfc8785.dumps(dict(self.payload))
        except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
            raise ValueError("payload is not canonically encodable") from exc
        if len(encoded) > MAX_OBSERVATION_PAYLOAD_BYTES:
            raise ValueError("payload exceeds the Manager observation byte bound")
        object.__setattr__(self, "_canonical_payload", encoded)

    def payload_value(self) -> dict[str, Any]:
        value = _strict_json_object(self._canonical_payload)
        return value


@dataclass(frozen=True)
class PoseObservationResult:
    disposition: str
    fresh: bool
    initialization_generation: int
    scene_runtime_source_generation: int
    source_epoch_order: int
    source_sequence: int
    scene_application_status: str
    scene_projection_revision: SceneProjectionRevision | None
    scene_request_id: str | None
    scene_update_status: str | None


class ManagerSceneRuntimeClient:
    """Exchange one bounded canonical packet at a time with Manager."""

    def __init__(
        self,
        run_root: Path,
        *,
        timeout_s: float = DEFAULT_TIMEOUT_S,
    ) -> None:
        if (
            isinstance(timeout_s, bool)
            or not isinstance(timeout_s, (int, float))
            or not math.isfinite(float(timeout_s))
            or timeout_s <= 0.0
            or timeout_s > 5.0
        ):
            raise ValueError("timeout_s must be finite and in (0, 5]")
        self._run_root = Path(run_root)
        self._socket_path = self._run_root / MANAGER_SOCKET_FILE
        with self._open_root(expected_identity=None) as (_descriptor, root):
            self._root_identity = (root.st_dev, root.st_ino)
        if len(os.fsencode(self._socket_path)) >= 104:
            raise SceneRuntimeManagerError(
                "manager_scene_runtime_transport_invalid",
                "Manager command socket path exceeds the AF_UNIX bound",
            )
        self._timeout_s = float(timeout_s)

    def read_owner(self) -> SceneRuntimeOwnerSnapshot:
        response = self._exchange(
            {
                "api_version": MANAGER_COMMAND_VERSION,
                "command": "scene_runtime_owner_read",
            }
        )
        record = _exact_object(
            response,
            {
                "api_version",
                "cell_id",
                "initialization_generation",
                "initialization_state",
                "ok",
                "runtime_identity",
                "scene_runtime_owner_epoch",
            },
            label="scene-runtime owner response",
        )
        try:
            cell_id = _bounded_text(record["cell_id"], label="cell_id")
            owner_epoch = _bounded_text(
                record["scene_runtime_owner_epoch"],
                label="scene_runtime_owner_epoch",
            )
            generation = _safe_integer(
                record["initialization_generation"],
                label="initialization_generation",
                allow_zero=True,
            )
            state = record["initialization_state"]
            if state not in _RUNTIME_STATES:
                raise ValueError("initialization_state is invalid")
            identity = SceneRuntimeIdentity.from_value(record["runtime_identity"])
        except (TypeError, ValueError) as exc:
            raise _response_invalid("scene-runtime owner response is invalid") from exc
        return SceneRuntimeOwnerSnapshot(
            cell_id=cell_id,
            owner_epoch=owner_epoch,
            runtime_identity=identity,
            initialization_generation=generation,
            initialization_state=state,
        )

    def open_observer(
        self,
        owner: SceneRuntimeOwnerSnapshot,
        *,
        bindings: tuple[PoseObserverBinding, ...],
        source_id: str,
        source_epoch: str,
    ) -> PoseObserverSession:
        if not isinstance(owner, SceneRuntimeOwnerSnapshot):
            raise TypeError("owner must be a SceneRuntimeOwnerSnapshot")
        source_id = _bounded_text(source_id, label="source_id")
        source_epoch = _bounded_text(source_epoch, label="source_epoch")
        if (
            not bindings
            or len(bindings) > 128
            or any(not isinstance(binding, PoseObserverBinding) for binding in bindings)
        ):
            raise ValueError("bindings must be one bounded non-empty tuple")
        normalized_bindings = tuple(sorted(bindings))
        if normalized_bindings != bindings or len(set(bindings)) != len(bindings):
            raise ValueError("bindings must be unique and canonically ordered")
        response = self._exchange(
            {
                "api_version": MANAGER_COMMAND_VERSION,
                "bindings": [binding.to_value() for binding in bindings],
                "command": "scene_runtime_observer_open",
                "expected_runtime_identity": owner.runtime_identity.to_value(),
                "source_epoch": source_epoch,
                "source_id": source_id,
            }
        )
        record = _exact_object(
            response,
            {
                "api_version",
                "bindings",
                "cell_id",
                "ok",
                "runtime_identity",
                "source_epoch",
                "source_epoch_order",
                "source_id",
            },
            label="scene-runtime observer-open response",
        )
        try:
            response_identity = SceneRuntimeIdentity.from_value(
                record["runtime_identity"]
            )
            epoch_order = _safe_integer(
                record["source_epoch_order"], label="source_epoch_order"
            )
            response_bindings = _pose_observer_bindings(record["bindings"])
        except (TypeError, ValueError) as exc:
            raise _response_invalid("observer-open response is invalid") from exc
        if (
            record["cell_id"] != owner.cell_id
            or response_identity != owner.runtime_identity
            or record["source_id"] != source_id
            or record["source_epoch"] != source_epoch
            or response_bindings != bindings
        ):
            raise SceneRuntimeManagerError(
                "manager_scene_runtime_response_identity_mismatch",
                "observer-open response crossed its requested identity",
            )
        return PoseObserverSession(
            cell_id=owner.cell_id,
            runtime_identity=response_identity,
            source_id=source_id,
            source_epoch=source_epoch,
            source_epoch_order=epoch_order,
        )

    def observe(
        self,
        session: PoseObserverSession,
        *,
        source_sequence: int,
        intent: PoseObservationIntent,
    ) -> PoseObservationResult:
        if not isinstance(session, PoseObserverSession):
            raise TypeError("session must be a PoseObserverSession")
        if not isinstance(intent, PoseObservationIntent):
            raise TypeError("intent must be a PoseObservationIntent")
        source_sequence = _safe_integer(
            source_sequence, label="source_sequence"
        )
        response = self._exchange(
            {
                "api_version": MANAGER_COMMAND_VERSION,
                "command": "scene_runtime_observe",
                "eligible": intent.eligible,
                "expected_runtime_identity": session.runtime_identity.to_value(),
                "lifetime_ns": intent.lifetime_ns,
                "object_id": intent.object_id,
                "payload": intent.payload_value(),
                "policy_id": intent.policy_id,
                # Wire name `payload_quality` (good|unknown); distinct from I/O quality.
                "payload_quality": intent.quality,
                "source_epoch": session.source_epoch,
                "source_id": session.source_id,
                "source_sequence": source_sequence,
            }
        )
        record = _exact_object(
            response,
            {
                "api_version",
                "disposition",
                "fresh",
                "initialization_generation",
                "object_id",
                "ok",
                "policy_id",
                "scene_application_status",
                "scene_projection_revision",
                "scene_request_id",
                "scene_runtime_source_generation",
                "scene_update_status",
                "source_epoch",
                "source_epoch_order",
                "source_id",
                "source_sequence",
            },
            label="scene-runtime observation response",
        )
        try:
            disposition = record["disposition"]
            if disposition not in _OBSERVATION_DISPOSITIONS:
                raise ValueError("observation disposition is invalid")
            if type(record["fresh"]) is not bool:
                raise TypeError("fresh must be an exact bool")
            initialization_generation = _safe_integer(
                record["initialization_generation"],
                label="initialization_generation",
                allow_zero=True,
            )
            publication_generation = _safe_integer(
                record["scene_runtime_source_generation"],
                label="scene_runtime_source_generation",
            )
            epoch_order = _safe_integer(
                record["source_epoch_order"], label="source_epoch_order"
            )
            response_sequence = _safe_integer(
                record["source_sequence"], label="source_sequence"
            )
            application_status = record["scene_application_status"]
            if application_status not in _SCENE_APPLICATION_STATUSES:
                raise ValueError("scene application status is invalid")
            projection = (
                None
                if record["scene_projection_revision"] is None
                else SceneProjectionRevision.from_value(
                    record["scene_projection_revision"]
                )
            )
            request_id = (
                None
                if record["scene_request_id"] is None
                else _bounded_text(
                    record["scene_request_id"], label="scene_request_id"
                )
            )
            update_status = record["scene_update_status"]
            if (
                update_status is not None
                and update_status not in _SCENE_UPDATE_STATUSES
            ):
                raise ValueError("scene update status is invalid")
            if application_status in {
                "buffered",
                "deferred_before_planning",
                "deferred_between_nodes",
                "not_requested",
            }:
                if (
                    projection is not None
                    or request_id is not None
                    or update_status is not None
                ):
                    raise ValueError("non-applied observation has scene result data")
            elif application_status == "committed":
                if (
                    projection is None
                    or request_id is None
                    or update_status is None
                ):
                    raise ValueError("committed observation lacks scene result data")
            elif (
                projection is None
                or request_id is None
                or update_status is not None
            ):
                # already_applied, skipped_attached and unchanged carry the
                # unchanged current projection and no update status.
                raise ValueError("idempotent or skipped observation result is invalid")
            if projection is not None and (
                projection.retained_bundle_id
                != session.runtime_identity.retained_bundle_id
                or projection.graph_revision
                != session.runtime_identity.graph_revision
            ):
                raise ValueError("scene projection crossed the retained runtime")
            if not intent.eligible or intent.quality != "good" or not record["fresh"]:
                if application_status != "not_requested":
                    raise ValueError("ineligible observation reports scene application")
            elif disposition in {"buffered", "replaced_buffered"}:
                if application_status != "buffered":
                    raise ValueError("buffered observation reports another application")
            elif application_status == "buffered":
                raise ValueError("unbuffered observation reports a buffered application")
            # Application timing belongs to the active Deployment/Manager.
            # The acquisition binding's policy_id is a stable source key, not
            # permission to reconstruct the selected application policy here.
        except (TypeError, ValueError) as exc:
            raise _response_invalid("observation response is invalid") from exc
        if (
            record["object_id"] != intent.object_id
            or record["policy_id"] != intent.policy_id
            or record["source_id"] != session.source_id
            or record["source_epoch"] != session.source_epoch
            or epoch_order != session.source_epoch_order
            or response_sequence != source_sequence
        ):
            raise SceneRuntimeManagerError(
                "manager_scene_runtime_response_identity_mismatch",
                "observation response crossed its requested identity",
            )
        return PoseObservationResult(
            disposition=disposition,
            fresh=record["fresh"],
            initialization_generation=initialization_generation,
            scene_runtime_source_generation=publication_generation,
            source_epoch_order=epoch_order,
            source_sequence=response_sequence,
            scene_application_status=application_status,
            scene_projection_revision=projection,
            scene_request_id=request_id,
            scene_update_status=update_status,
        )

    @contextmanager
    def _open_root(
        self,
        *,
        expected_identity: tuple[int, int] | None,
    ) -> Iterator[tuple[int, os.stat_result]]:
        try:
            before = self._run_root.lstat()
            canonical = self._run_root.resolve(strict=True)
            descriptor = os.open(
                self._run_root,
                os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW,
            )
        except (OSError, RuntimeError) as exc:
            raise SceneRuntimeManagerError(
                "manager_scene_runtime_transport_invalid",
                "Manager run root is unavailable",
            ) from exc
        try:
            try:
                opened = os.fstat(descriptor)
                after = self._run_root.lstat()
            except OSError as exc:
                raise SceneRuntimeManagerError(
                    "manager_scene_runtime_transport_invalid",
                    "Manager run root changed while opening",
                ) from exc
            identity = (before.st_dev, before.st_ino)
            required = stat.S_IRUSR | stat.S_IWUSR | stat.S_IXUSR
            if (
                expected_identity is not None
                and identity != expected_identity
            ):
                raise SceneRuntimeManagerError(
                    "manager_scene_runtime_transport_invalid",
                    "Manager run root identity changed",
                )
            if (
                not self._run_root.is_absolute()
                or canonical != self._run_root
                or any(
                    not stat.S_ISDIR(metadata.st_mode)
                    or metadata.st_uid != os.geteuid()
                    or metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                    or metadata.st_mode & required != required
                    or (metadata.st_dev, metadata.st_ino) != identity
                    for metadata in (before, opened, after)
                )
            ):
                raise SceneRuntimeManagerError(
                    "manager_scene_runtime_transport_invalid",
                    "Manager run root is not one safe real same-user directory",
                )
            yield descriptor, opened
            try:
                final = self._run_root.lstat()
            except OSError as exc:
                raise SceneRuntimeManagerError(
                    "manager_scene_runtime_transport_invalid",
                    "Manager run root changed during use",
                ) from exc
            if (
                not stat.S_ISDIR(final.st_mode)
                or final.st_uid != os.geteuid()
                or final.st_mode & (stat.S_IWGRP | stat.S_IWOTH)
                or final.st_mode & required != required
                or (final.st_dev, final.st_ino) != identity
            ):
                raise SceneRuntimeManagerError(
                    "manager_scene_runtime_transport_invalid",
                    "Manager run root changed during use",
                )
        finally:
            os.close(descriptor)

    @contextmanager
    def _socket_address(self) -> Iterator[str]:
        with self._open_root(
            expected_identity=self._root_identity
        ) as (descriptor, _root):
            try:
                metadata = os.stat(
                    MANAGER_SOCKET_FILE,
                    dir_fd=descriptor,
                    follow_symlinks=False,
                )
            except OSError as exc:
                raise SceneRuntimeManagerError(
                    "manager_scene_runtime_unavailable",
                    "Manager command socket is unavailable",
                ) from exc
            if (
                not stat.S_ISSOCK(metadata.st_mode)
                or metadata.st_uid != os.geteuid()
                or metadata.st_nlink != 1
                or stat.S_IMODE(metadata.st_mode) != 0o600
            ):
                raise SceneRuntimeManagerError(
                    "manager_scene_runtime_transport_invalid",
                    "Manager command endpoint is not the exact same-user socket",
                )
            yield f"/proc/self/fd/{descriptor}/{MANAGER_SOCKET_FILE}"

    def _exchange(self, command: dict[str, Any]) -> dict[str, Any]:
        try:
            request = rfc8785.dumps(command)
        except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
            raise SceneRuntimeManagerError(
                "manager_scene_runtime_request_invalid",
                "Manager scene-runtime request is not canonically encodable",
            ) from exc
        if not request or len(request) > MAX_MANAGER_REQUEST_BYTES:
            raise SceneRuntimeManagerError(
                "manager_scene_runtime_request_invalid",
                "Manager scene-runtime request exceeds its byte bound",
            )
        try:
            with self._socket_address() as socket_address:
                with socket.socket(
                    socket.AF_UNIX, socket.SOCK_SEQPACKET
                ) as channel:
                    channel.settimeout(self._timeout_s)
                    channel.connect(socket_address)
                    credentials = channel.getsockopt(
                        socket.SOL_SOCKET,
                        socket.SO_PEERCRED,
                        struct.calcsize("3i"),
                    )
                    _pid, peer_uid, _gid = struct.unpack("3i", credentials)
                    if peer_uid != os.geteuid():
                        raise SceneRuntimeManagerError(
                            "manager_scene_runtime_transport_invalid",
                            "Manager command peer is not same-user",
                        )
                    sent = channel.send(request)
                    if sent != len(request):
                        raise OSError(
                            "short Manager scene-runtime request send"
                        )
                    payload, _ancillary, flags, _address = channel.recvmsg(
                        MAX_MANAGER_RESPONSE_BYTES + 1
                    )
        except SceneRuntimeManagerError:
            raise
        except (OSError, TimeoutError) as exc:
            raise SceneRuntimeManagerError(
                "manager_scene_runtime_unavailable",
                "Manager scene-runtime exchange failed",
            ) from exc
        if (
            not payload
            or len(payload) > MAX_MANAGER_RESPONSE_BYTES
            or flags & socket.MSG_TRUNC
        ):
            raise _response_invalid(
                "Manager scene-runtime response is empty, truncated, or oversized"
            )
        response = _strict_json_object(payload)
        try:
            canonical = rfc8785.dumps(response)
        except (rfc8785.CanonicalizationError, UnicodeError, ValueError) as exc:
            raise _response_invalid(
                "Manager scene-runtime response cannot be canonicalized"
            ) from exc
        if canonical != payload:
            raise _response_invalid(
                "Manager scene-runtime response is not exact canonical JSON"
            )
        if response.get("api_version") != MANAGER_RESPONSE_VERSION:
            raise _response_invalid(
                "Manager scene-runtime response version is unsupported"
            )
        if type(response.get("ok")) is not bool:
            raise _response_invalid(
                "Manager scene-runtime response has no exact boolean result"
            )
        if response["ok"]:
            return response
        failure = _exact_object(
            response,
            {"api_version", "error", "ok"},
            label="Manager scene-runtime failure response",
        )
        error = _exact_object(
            failure["error"], {"code", "message"}, label="Manager owner error"
        )
        try:
            code = _bounded_text(error["code"], label="owner error code")
            message = _bounded_text(
                error["message"],
                label="owner error message",
                maximum_bytes=_MAX_ERROR_MESSAGE_BYTES,
            )
        except (TypeError, ValueError) as exc:
            raise _response_invalid("Manager owner error is invalid") from exc
        raise SceneRuntimeManagerError(code, message)


def _strict_json_object(payload: bytes) -> dict[str, Any]:
    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate object member {key!r}")
            result[key] = value
        return result

    def reject_constant(value: str) -> None:
        raise ValueError(f"unsupported JSON constant {value}")

    try:
        text = payload.decode("utf-8", errors="strict")
        value = json.loads(
            text,
            object_pairs_hook=unique_object,
            parse_constant=reject_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as exc:
        raise _response_invalid(
            "Manager scene-runtime response is not strict UTF-8 JSON"
        ) from exc
    if not isinstance(value, dict):
        raise _response_invalid("Manager scene-runtime response is not an object")
    return value


def _pose_observer_bindings(value: object) -> tuple[PoseObserverBinding, ...]:
    if not isinstance(value, list) or not value or len(value) > 128:
        raise ValueError("observer bindings must be one bounded non-empty array")
    result: list[PoseObserverBinding] = []
    for item in value:
        record = _exact_object(
            item,
            {"eligible", "object_id", "policy_id"},
            label="observer binding",
        )
        result.append(
            PoseObserverBinding(
                object_id=record["object_id"],
                policy_id=record["policy_id"],
                eligible=record["eligible"],
            )
        )
    bindings = tuple(result)
    if bindings != tuple(sorted(bindings)) or len(set(bindings)) != len(bindings):
        raise ValueError("observer bindings are not unique and ordered")
    return bindings


def _exact_object(
    value: object,
    fields: set[str],
    *,
    label: str,
) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != fields:
        raise _response_invalid(f"{label} has an unknown or missing field")
    return value


def _bounded_text(
    value: object,
    *,
    label: str,
    maximum_bytes: int = _MAX_IDENTITY_BYTES,
) -> str:
    if (
        not isinstance(value, str)
        or not value
        or unicodedata.normalize("NFC", value) != value
        or len(value.encode("utf-8")) > maximum_bytes
    ):
        raise ValueError(f"{label} must be bounded non-empty NFC text")
    return value


def _sha256(value: object, *, label: str) -> str:
    text = _bounded_text(value, label=label)
    if (
        len(text) != 71
        or not text.startswith("sha256:")
        or any(character not in "0123456789abcdef" for character in text[7:])
    ):
        raise ValueError(f"{label} must be one lowercase SHA-256 digest")
    return text


def _safe_integer(
    value: object,
    *,
    label: str,
    allow_zero: bool = False,
) -> int:
    minimum = 0 if allow_zero else 1
    if type(value) is not int or value < minimum or value > MAXIMUM_SAFE_INTEGER:
        raise ValueError(f"{label} is not an exact safe integer")
    return value


def _response_invalid(message: str) -> SceneRuntimeManagerError:
    return SceneRuntimeManagerError(
        "manager_scene_runtime_response_invalid",
        message,
    )
