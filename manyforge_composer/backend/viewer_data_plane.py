"""Bounded demand and transport primitives for the native scene viewer.

This module is deliberately independent of planning, collision, execution, and
scene mutation.  It owns only observer demand and already-built publication
delivery.  Producers never await a browser and every client has one replaceable
latest slot.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
import math
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Callable, Final, Iterable

from fastapi import WebSocket

VIEWER_MANIFEST_SCHEMA: Final = "manyforge.viewer.manifest.v1"
VIEWER_FRAME_SCHEMA: Final = "manyforge.viewer.frame.v1"
VIEWER_DIAGNOSTICS_SCHEMA: Final = "manyforge.viewer.diagnostics.v1"
VIEWER_CONTROL_SCHEMA: Final = "manyforge.viewer.control.v1"

_ALLOWED_VISIBILITY: Final = frozenset({"focused", "visible", "collapsed", "hidden"})
_ACTIVE_VISIBILITY: Final = frozenset({"focused", "visible"})
_ALLOWED_LAYERS: Final = frozenset(
    {
        "canonical",
        "measured_robot",
        "measured_objects",
        "measured_tcp",
        "candidates",
        "diagnostics",
    }
)


def viewer_effective_period_s(
    *,
    fastest_period_s: float,
    requested_rate_hz: float,
) -> float | None:
    """Return the clamped sampling period for active viewer demand.

    ``fastest_period_s`` is the server-side CPU/GPU ceiling. A client may ask
    for a lower rate, but never make the producer run faster than that ceiling.
    """
    if requested_rate_hz <= 0.0:
        return None
    return max(0.02, max(float(fastest_period_s), 1.0 / requested_rate_hz))


class ViewerInputUnchanged(RuntimeError):
    """Signals that cheap source sampling found no publishable change."""

    def __init__(self, stage_durations_ms: dict[str, float] | None = None) -> None:
        super().__init__("viewer_input_unchanged")
        self.stage_durations_ms = dict(stage_durations_ms or {})


def _canonical_json(value: Any) -> str:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
        allow_nan=False,
    )


def _content_hash(value: Any) -> str:
    return hashlib.sha256(_canonical_json(value).encode("utf-8")).hexdigest()


_RENDER_EPHEMERAL_FIELDS: Final[frozenset[str]] = frozenset(
    {
        "age_s",
        "monotonic_s",
        "received_at_ns",
        "source_age_ns",
        "source_sequence",
        "source_timestamp_ns",
        "stamp_sec",
        "timestamp_ns",
    }
)


def _render_content(value: Any) -> Any:
    """Remove evidence-only time fields from one render-content signature.

    The complete frame remains authoritative when render-significant content
    changes. Timestamp/age-only evidence belongs to the low-rate diagnostics
    domain and does not consume a transform-frame sequence. Status, stale
    state, transforms, gripper state, rejection evidence, and every other
    non-temporal value remain render-significant.
    """

    if hasattr(value, "model_dump"):
        value = value.model_dump(mode="python")
    if isinstance(value, dict):
        return {
            str(key): _render_content(item)
            for key, item in value.items()
            if str(key) not in _RENDER_EPHEMERAL_FIELDS
        }
    if isinstance(value, (list, tuple)):
        return [_render_content(item) for item in value]
    return value


def freeze_viewer_signature(value: Any) -> Any:
    """Convert observer inputs into a deterministic, hashable signature."""
    if hasattr(value, "model_dump"):
        value = value.model_dump(mode="python")
    if isinstance(value, dict):
        return tuple(
            (str(key), freeze_viewer_signature(item))
            for key, item in sorted(value.items(), key=lambda pair: str(pair[0]))
        )
    if isinstance(value, (list, tuple)):
        return tuple(freeze_viewer_signature(item) for item in value)
    if isinstance(value, (set, frozenset)):
        return tuple(sorted(freeze_viewer_signature(item) for item in value))
    return value


def freeze_viewer_render_signature(value: Any) -> Any:
    """Freeze only content whose change requires a transform-frame rebuild.

    Pose-source transport revisions, timestamps, and calculated ages belong to
    diagnostics. Including them in the worker build key would repeat FK and
    encoding even when every visible pose and authority state is unchanged.
    """

    return freeze_viewer_signature(_render_content(value))


@dataclass(frozen=True)
class ViewerDemandSummary:
    active: bool
    client_count: int
    active_client_count: int
    requested_rate_hz: float
    layers: frozenset[str]
    next_deadline_ns: int | None
    lease_revision: int


@dataclass(frozen=True)
class ViewerLeaseGrant:
    client_id: str
    visibility: str
    requested_rate_hz: float
    layers: frozenset[str]
    expires_at_ns: int
    lease_revision: int


@dataclass
class _ViewerLease:
    client_id: str
    visibility: str
    requested_rate_hz: float
    layers: frozenset[str]
    expires_at_ns: int
    connected: bool = True


class ViewerDemandRegistry:
    """Thread-safe viewer demand leases with deterministic expiry.

    A connected browser must renew its lease through control messages.  A
    disconnect retains demand only for ``disconnect_grace_s`` to avoid rebuild
    thrash during a brief network handover.  Hidden/collapsed clients remain
    connected for diagnostics but never request dynamic FK/frame production.
    """

    def __init__(
        self,
        *,
        lease_timeout_s: float = 15.0,
        disconnect_grace_s: float = 0.75,
        max_rate_hz: float = 30.0,
        clock_ns: Callable[[], int] = time.monotonic_ns,
        on_change: Callable[[], None] | None = None,
    ) -> None:
        if lease_timeout_s <= 0.0:
            raise ValueError("lease_timeout_s must be positive")
        if disconnect_grace_s < 0.0:
            raise ValueError("disconnect_grace_s must be non-negative")
        if max_rate_hz <= 0.0:
            raise ValueError("max_rate_hz must be positive")
        self._lease_timeout_ns = int(lease_timeout_s * 1e9)
        self._disconnect_grace_ns = int(disconnect_grace_s * 1e9)
        self._max_rate_hz = float(max_rate_hz)
        self._clock_ns = clock_ns
        self._on_change = on_change
        self._lock = threading.Lock()
        self._leases: dict[str, _ViewerLease] = {}
        self._revision = 0

    @property
    def max_rate_hz(self) -> float:
        return self._max_rate_hz

    def attach(
        self,
        client_id: str,
        *,
        visibility: str = "visible",
        requested_rate_hz: float = 15.0,
        layers: Iterable[str] = ("canonical", "diagnostics"),
    ) -> ViewerDemandSummary:
        return self.update(
            client_id,
            visibility=visibility,
            requested_rate_hz=requested_rate_hz,
            layers=layers,
            create=True,
        )

    def update(
        self,
        client_id: str,
        *,
        visibility: str,
        requested_rate_hz: float,
        layers: Iterable[str],
        create: bool = False,
    ) -> ViewerDemandSummary:
        client = str(client_id).strip()
        if not client:
            raise ValueError("client_id must not be empty")
        normalized_visibility = str(visibility).strip().lower()
        if normalized_visibility not in _ALLOWED_VISIBILITY:
            raise ValueError("unsupported viewer visibility")
        try:
            rate = float(requested_rate_hz)
        except (TypeError, ValueError) as exc:
            raise ValueError("requested_rate_hz must be numeric") from exc
        if not math.isfinite(rate) or rate < 0.0:
            raise ValueError("requested_rate_hz must be finite and non-negative")
        rate = min(rate, self._max_rate_hz)
        normalized_layers = frozenset(str(layer).strip() for layer in layers)
        if not normalized_layers.issubset(_ALLOWED_LAYERS):
            raise ValueError("unsupported viewer layer")
        if "canonical" not in normalized_layers:
            normalized_layers = frozenset({*normalized_layers, "canonical"})

        now_ns = self._clock_ns()
        changed = False
        with self._lock:
            self._prune_locked(now_ns)
            lease = self._leases.get(client)
            if lease is None and not create:
                raise KeyError(client)
            replacement = _ViewerLease(
                client_id=client,
                visibility=normalized_visibility,
                requested_rate_hz=rate,
                layers=normalized_layers,
                expires_at_ns=now_ns + self._lease_timeout_ns,
                connected=True,
            )
            changed = lease != replacement
            self._leases[client] = replacement
            if changed:
                self._revision += 1
            summary = self._summary_locked(now_ns)
        if changed:
            self._notify_change()
        return summary

    def detach(self, client_id: str) -> ViewerDemandSummary:
        client = str(client_id).strip()
        now_ns = self._clock_ns()
        changed = False
        with self._lock:
            self._prune_locked(now_ns)
            lease = self._leases.get(client)
            if lease is not None:
                if self._disconnect_grace_ns == 0:
                    del self._leases[client]
                else:
                    lease.connected = False
                    lease.expires_at_ns = now_ns + self._disconnect_grace_ns
                self._revision += 1
                changed = True
            summary = self._summary_locked(now_ns)
        if changed:
            self._notify_change()
        return summary

    def summary(self) -> ViewerDemandSummary:
        now_ns = self._clock_ns()
        changed = False
        with self._lock:
            before = len(self._leases)
            self._prune_locked(now_ns)
            changed = len(self._leases) != before
            if changed:
                self._revision += 1
            summary = self._summary_locked(now_ns)
        if changed:
            self._notify_change()
        return summary

    def grant(self, client_id: str) -> ViewerLeaseGrant:
        client = str(client_id).strip()
        now_ns = self._clock_ns()
        changed = False
        with self._lock:
            before = len(self._leases)
            self._prune_locked(now_ns)
            changed = len(self._leases) != before
            if changed:
                self._revision += 1
            lease = self._leases.get(client)
            if lease is None:
                raise KeyError(client)
            grant = ViewerLeaseGrant(
                client_id=client,
                visibility=lease.visibility,
                requested_rate_hz=lease.requested_rate_hz,
                layers=lease.layers,
                expires_at_ns=lease.expires_at_ns,
                lease_revision=self._revision,
            )
        if changed:
            self._notify_change()
        return grant

    def _prune_locked(self, now_ns: int) -> None:
        expired = [
            client_id
            for client_id, lease in self._leases.items()
            if lease.expires_at_ns <= now_ns
        ]
        for client_id in expired:
            del self._leases[client_id]

    def _summary_locked(self, now_ns: int) -> ViewerDemandSummary:
        leases = list(self._leases.values())
        active = [
            lease
            for lease in leases
            if lease.expires_at_ns > now_ns
            and lease.visibility in _ACTIVE_VISIBILITY
            and lease.requested_rate_hz > 0.0
        ]
        rate = max((lease.requested_rate_hz for lease in active), default=0.0)
        layers = frozenset(layer for lease in active for layer in lease.layers)
        next_deadline = min(
            (lease.expires_at_ns for lease in leases),
            default=None,
        )
        return ViewerDemandSummary(
            active=bool(active),
            client_count=len(leases),
            active_client_count=len(active),
            requested_rate_hz=rate,
            layers=layers,
            next_deadline_ns=next_deadline,
            lease_revision=self._revision,
        )

    def _notify_change(self) -> None:
        if self._on_change is not None:
            self._on_change()


@dataclass(frozen=True)
class ViewerPublication:
    producer_epoch: str
    manifest_revision: int
    frame_sequence: int
    manifest: dict[str, Any]
    frame: dict[str, Any]
    manifest_json: str
    frame_json: str
    encoded_bytes: int
    encode_duration_ms: float


@dataclass(frozen=True)
class ViewerSnapshotBuild:
    """One successfully shaped observer generation before protocol encoding."""

    snapshot: Any
    input_signature: tuple[Any, ...]
    stage_durations_ms: dict[str, float]
    canonical_fk_reused: bool = False
    measured_fk_reused: int = 0
    publication: ViewerPublication | None = None
    derived_attachment_offsets: dict[str, dict[str, Any]] = field(default_factory=dict)
    object_revision: int | None = None


class ViewerProtocolEncoder:
    """Encode self-applicable manifest/frame generations once per producer."""

    def __init__(self, *, producer_epoch: str | None = None) -> None:
        self._producer_epoch = producer_epoch or uuid.uuid4().hex
        self._manifest_revision = 0
        self._frame_sequence = 0
        self._render_revision = 0
        self._manifest_signature = ""
        self._render_signature = ""
        self._last_manifest: dict[str, Any] | None = None
        self._lock = threading.Lock()

    @property
    def producer_epoch(self) -> str:
        return self._producer_epoch

    def encode(
        self,
        *,
        manifest_payload: dict[str, Any],
        frame_payload: dict[str, Any],
    ) -> ViewerPublication | None:
        """Encode one render-significant generation.

        Timestamp/age-only observer evidence is deliberately not assigned a
        frame sequence. Source freshness transitions remain render-significant
        and therefore publish, while timer passage alone cannot create fake
        transform traffic or frame-gap accounting.
        """
        started = time.monotonic()
        with self._lock:
            normalized_manifest = dict(manifest_payload)
            normalized_manifest.pop("manifest_revision", None)
            normalized_manifest.pop("manifestRevision", None)
            normalized_manifest.pop("content_hash", None)
            normalized_manifest.pop("contentHash", None)
            signature = _content_hash(normalized_manifest)
            manifest_changed = signature != self._manifest_signature
            if manifest_changed:
                self._manifest_revision += 1
                self._manifest_signature = signature
                self._last_manifest = {
                    "type": "manifest",
                    "schema_version": VIEWER_MANIFEST_SCHEMA,
                    "producer_epoch": self._producer_epoch,
                    "manifest_revision": self._manifest_revision,
                    "content_hash": signature,
                    **normalized_manifest,
                }
            assert self._last_manifest is not None
            render_signature = _content_hash(
                {
                    "manifest_signature": self._manifest_signature,
                    "frame": _render_content(frame_payload),
                }
            )
            if not manifest_changed and render_signature == self._render_signature:
                return None
            self._render_revision += 1
            self._render_signature = render_signature
            self._frame_sequence += 1
            manifest = self._last_manifest
            frame = {
                "type": "frame",
                "schema_version": VIEWER_FRAME_SCHEMA,
                "producer_epoch": self._producer_epoch,
                "manifest_revision": self._manifest_revision,
                "frame_sequence": self._frame_sequence,
                "render_revision": self._render_revision,
                **frame_payload,
            }
            manifest_json = _canonical_json(manifest)
            frame_json = _canonical_json(frame)
            sequence = self._frame_sequence
            revision = self._manifest_revision
        duration_ms = (time.monotonic() - started) * 1000.0
        return ViewerPublication(
            producer_epoch=self._producer_epoch,
            manifest_revision=revision,
            frame_sequence=sequence,
            manifest=manifest,
            frame=frame,
            manifest_json=manifest_json,
            frame_json=frame_json,
            encoded_bytes=len(manifest_json.encode("utf-8")) + len(frame_json.encode("utf-8")),
            encode_duration_ms=duration_ms,
        )


def build_viewer_protocol_payloads(
    visual_state: Any,
    *,
    frame_roles: dict[str, Iterable[str]],
    frame_names: Iterable[str],
    frame_visibility: dict[str, bool],
    object_axes_gizmo_size_m: float,
    frame_axes_gizmo_size_m: float,
) -> tuple[dict[str, Any], dict[str, Any]]:
    """Shape one immutable visual snapshot without reading ComposerState."""
    robot = visual_state.robot
    link_order = sorted(visual_state.link_poses)
    objects = sorted(visual_state.objects, key=lambda item: item.object_id)
    grippers = sorted(visual_state.grippers, key=lambda item: item.gripper_id)
    pose_sources = sorted(
        visual_state.pose_sources,
        key=lambda item: item.pose_source_id,
    )

    robot_manifest: dict[str, Any] | None = None
    robot_frame: dict[str, Any] | None = None
    if robot is not None:
        robot_payload = robot.model_dump(mode="json")
        joint_order = list(robot.joint_names)
        robot_manifest = {
            key: value
            for key, value in robot_payload.items()
            if key
            not in {
                "joint_positions",
                "tcp_pose_in_base",
                "state_source",
                "source_timestamp_ns",
                "source_age_ns",
            }
        }
        robot_manifest["link_order"] = link_order
        robot_frame = {
            "robot_id": robot.robot_id,
            "joint_positions": [
                robot.joint_positions.get(name) for name in joint_order
            ],
            "link_transforms": [
                visual_state.link_poses[name] for name in link_order
            ],
            "tcp_pose_in_base": robot.tcp_pose_in_base,
            "state_source": robot.state_source,
            "source_timestamp_ns": robot.source_timestamp_ns,
            "source_age_ns": robot.source_age_ns,
        }

    entity_manifest: list[dict[str, Any]] = []
    entity_transforms: list[Any] = []
    for item in objects:
        payload = item.model_dump(mode="json")
        entity_transforms.append(payload.pop("pose_in_universe", None))
        shape = payload.get("shape")
        if isinstance(shape, dict):
            shape.setdefault("mesh_asset_url", None)
            shape.setdefault("mesh_asset_status", None)
        entity_manifest.append(payload)

    normalized_roles = {
        str(name): sorted(str(role) for role in roles)
        for name, roles in frame_roles.items()
    }
    ordered_frames = sorted(
        {str(name) for name in frame_names} | set(normalized_roles)
    )
    world_frame = str(visual_state.world_frame or "universe")
    identity_pose = {
        "position_m": [0.0, 0.0, 0.0],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    frame_manifest = [
        {
            "name": name,
            "roles": normalized_roles.get(name, []),
            # Structural URDF frames are hidden unless explicitly selected.
            # A missing visibility entry must retain that role-based default;
            # treating an incomplete map as `True` floods the viewer with every
            # robot and gripper link axis.
            "viewer_visible": bool(
                frame_visibility.get(name, bool(normalized_roles.get(name)))
            ),
        }
        for name in ordered_frames
    ]
    frame_transforms = [
        identity_pose
        if name == world_frame
        else visual_state.link_poses.get(name)
        for name in ordered_frames
    ]

    pose_source_manifest: list[dict[str, Any]] = []
    pose_source_states: list[dict[str, Any]] = []
    for source in pose_sources:
        payload = source.model_dump(mode="json")
        source_objects = payload.pop("objects", [])
        measured_tcp = payload.pop("measured_tcp", None)
        measured_robot = payload.pop("measured_robot", None)
        status = payload.pop("status", None)
        received_at_ns = payload.pop("received_at_ns", None)
        static_objects: list[dict[str, Any]] = []
        dynamic_objects: list[dict[str, Any]] = []
        for object_payload in source_objects:
            dynamic_objects.append(
                {
                    "object_id": object_payload.get("object_id"),
                    "status": object_payload.get("status"),
                    "age_s": object_payload.get("age_s"),
                    "update": object_payload.get("update"),
                    "rejection": object_payload.get("rejection"),
                    "attached_drift": object_payload.get("attached_drift"),
                }
            )
            static_objects.append(
                {
                    key: value
                    for key, value in object_payload.items()
                    if key
                    not in {
                        "status",
                        "age_s",
                        "update",
                        "rejection",
                        "attached_drift",
                    }
                }
            )
        payload["objects"] = static_objects
        pose_source_manifest.append(payload)
        pose_source_states.append(
            {
                "pose_source_id": source.pose_source_id,
                "status": status,
                "received_at_ns": received_at_ns,
                "measured_tcp": measured_tcp,
                "measured_robot": measured_robot,
                "objects": dynamic_objects,
            }
        )

    manifest = {
        "world_frame": world_frame,
        "robots": [] if robot_manifest is None else [robot_manifest],
        "entities": entity_manifest,
        "frames": frame_manifest,
        "grippers": [
            {
                key: value
                for key, value in item.model_dump(mode="json").items()
                if key
                not in {
                    "state",
                    "command_id",
                    "generation",
                    "revision",
                    "current_position",
                    "target_position",
                    "commanded_position",
                    "observed_position",
                    "commanded_state",
                    "evidence",
                    "evidence_state",
                    "projection_state",
                    "projection_reason",
                    "error",
                    "reason_code",
                }
            }
            for item in grippers
        ],
        "pose_sources": pose_source_manifest,
        "orders": {
            "links": link_order,
            "entities": [item.object_id for item in objects],
            "frames": ordered_frames,
            "grippers": [item.gripper_id for item in grippers],
            "pose_sources": [item.pose_source_id for item in pose_sources],
        },
        "object_axes_gizmo_size_m": float(object_axes_gizmo_size_m),
        "frame_axes_gizmo_size_m": float(frame_axes_gizmo_size_m),
        "capabilities": {
            "measured_robot": True,
            "measured_objects": True,
            "measured_tcp": True,
            "point_cloud_channel": True,
            "snapshot_capture": True,
            "multi_robot_shape": True,
        },
    }
    frame = {
        "timestamp_ns": visual_state.timestamp_ns,
        "source_sequence": visual_state.sequence_number,
        "stale": visual_state.stale,
        "robot_blocks": [] if robot_frame is None else [robot_frame],
        "entity_transforms": entity_transforms,
        "frame_transforms": frame_transforms,
        "gripper_states": [item.model_dump(mode="json") for item in grippers],
        "pose_source_states": pose_source_states,
    }
    return manifest, frame


@dataclass
class _FanoutClient:
    client_id: str
    websocket: WebSocket
    event: asyncio.Event = field(default_factory=asyncio.Event)
    pending: ViewerPublication | None = None
    control_pending: str | None = None
    diagnostics_pending: str | None = None
    last_manifest_identity: tuple[str, int] | None = None
    sent_frames: int = 0
    replaced_frames: int = 0
    replaced_controls: int = 0
    replaced_diagnostics: int = 0
    send_errors: int = 0
    send_timeouts: int = 0
    client_diagnostics: dict[str, Any] | None = None
    client_diagnostics_received_at_ns: int | None = None
    qualification_send_delay_s: float = 0.0
    closed: bool = False


class BoundedViewerFanout:
    """Capacity-one, non-blocking publication fanout.

    ``publish`` performs no socket I/O.  It atomically replaces each client's
    pending generation and wakes its independent writer.  A slow client can
    lose intermediate frames but always receives a manifest before the next
    frame for a new revision.
    """

    def __init__(self, *, send_timeout_s: float = 2.0) -> None:
        if send_timeout_s <= 0.0:
            raise ValueError("send_timeout_s must be positive")
        self._send_timeout_s = float(send_timeout_s)
        self._clients: dict[str, _FanoutClient] = {}
        self._lock = asyncio.Lock()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._published_frames = 0
        # The visual publisher runs outside the ASGI loop. Keep that crossing
        # capacity-one as well: one scheduled drain owns one replaceable latest
        # generation. Creating one asyncio task per producer frame would move
        # the unbounded queue in front of the otherwise bounded fanout.
        self._ingress_lock = threading.Lock()
        self._ingress_pending: ViewerPublication | None = None
        self._ingress_drain_scheduled = False
        self._ingress_replacements = 0
        self._ingress_schedule_failures = 0
        self._retired_replaced_frames = 0
        self._retired_replaced_controls = 0
        self._retired_replaced_diagnostics = 0
        self._retired_send_errors = 0
        self._retired_send_timeouts = 0

    async def attach(
        self,
        websocket: WebSocket,
        *,
        client_id: str,
        qualification_send_delay_s: float = 0.0,
    ) -> None:
        if (
            not math.isfinite(qualification_send_delay_s)
            or qualification_send_delay_s < 0.0
            or qualification_send_delay_s >= self._send_timeout_s
        ):
            raise ValueError("qualification send delay must remain below send timeout")
        loop = asyncio.get_running_loop()
        async with self._lock:
            if self._loop is not None and self._loop is not loop:
                raise RuntimeError("viewer fanout cannot span event loops")
            self._loop = loop
            if client_id in self._clients:
                raise ValueError(f"viewer client already attached: {client_id}")
            self._clients[client_id] = _FanoutClient(
                client_id=client_id,
                websocket=websocket,
                qualification_send_delay_s=qualification_send_delay_s,
            )

    async def detach(self, client_id: str) -> None:
        async with self._lock:
            client = self._clients.pop(client_id, None)
            if client is not None:
                self._retired_replaced_frames += client.replaced_frames
                self._retired_replaced_controls += client.replaced_controls
                self._retired_replaced_diagnostics += client.replaced_diagnostics
                self._retired_send_errors += client.send_errors
                self._retired_send_timeouts += client.send_timeouts
                client.closed = True
                client.event.set()

    async def publish(self, publication: ViewerPublication) -> None:
        async with self._lock:
            self._published_frames += 1
            for client in self._clients.values():
                if client.closed:
                    continue
                self._offer_locked(client, publication)

    async def offer_to_client(
        self,
        client_id: str,
        publication: ViewerPublication,
    ) -> None:
        """Offer one cached generation without rebroadcasting it to peers."""
        async with self._lock:
            client = self._clients.get(client_id)
            if client is not None and not client.closed:
                self._offer_locked(client, publication)

    @staticmethod
    def _offer_locked(
        client: _FanoutClient,
        publication: ViewerPublication,
    ) -> None:
        if client.pending is not None:
            client.replaced_frames += 1
        client.pending = publication
        client.event.set()

    def publish_threadsafe(self, publication: ViewerPublication) -> bool:
        loop = self._loop
        if loop is None or loop.is_closed():
            return False
        schedule_drain = False
        with self._ingress_lock:
            if self._ingress_pending is not None:
                self._ingress_replacements += 1
            self._ingress_pending = publication
            if not self._ingress_drain_scheduled:
                self._ingress_drain_scheduled = True
                schedule_drain = True
        if not schedule_drain:
            return True
        try:
            loop.call_soon_threadsafe(self._start_ingress_drain)
        except RuntimeError:
            with self._ingress_lock:
                self._ingress_pending = None
                self._ingress_drain_scheduled = False
                self._ingress_schedule_failures += 1
            return False
        return True

    def _start_ingress_drain(self) -> None:
        asyncio.create_task(self._drain_threadsafe_ingress())

    async def _drain_threadsafe_ingress(self) -> None:
        while True:
            with self._ingress_lock:
                publication = self._ingress_pending
                self._ingress_pending = None
                if publication is None:
                    self._ingress_drain_scheduled = False
                    return
            await self.publish(publication)

    async def send_loop(self, client_id: str) -> None:
        while True:
            async with self._lock:
                client = self._clients.get(client_id)
                if client is None or client.closed:
                    return
                event = client.event
            await event.wait()
            async with self._lock:
                client = self._clients.get(client_id)
                if client is None or client.closed:
                    return
                publication = client.pending
                control = client.control_pending
                diagnostics = client.diagnostics_pending
                qualification_send_delay_s = client.qualification_send_delay_s
                client.pending = None
                client.control_pending = None
                client.diagnostics_pending = None
                client.event.clear()
                last_manifest_identity = client.last_manifest_identity
            if publication is None and control is None and diagnostics is None:
                continue
            try:
                if control is not None:
                    await asyncio.wait_for(
                        client.websocket.send_text(control),
                        timeout=self._send_timeout_s,
                    )
                if publication is None:
                    if diagnostics is not None:
                        await asyncio.wait_for(
                            client.websocket.send_text(diagnostics),
                            timeout=self._send_timeout_s,
                        )
                    continue
                if qualification_send_delay_s > 0.0:
                    await asyncio.sleep(qualification_send_delay_s)
                    async with self._lock:
                        current = self._clients.get(client_id)
                        if current is not client or current.closed:
                            return
                manifest_identity = (
                    publication.producer_epoch,
                    publication.manifest_revision,
                )
                if last_manifest_identity != manifest_identity:
                    await asyncio.wait_for(
                        client.websocket.send_text(publication.manifest_json),
                        timeout=self._send_timeout_s,
                    )
                    async with self._lock:
                        current = self._clients.get(client_id)
                        if current is not None:
                            current.last_manifest_identity = manifest_identity
                await asyncio.wait_for(
                    client.websocket.send_text(publication.frame_json),
                    timeout=self._send_timeout_s,
                )
                if diagnostics is not None:
                    await asyncio.wait_for(
                        client.websocket.send_text(diagnostics),
                        timeout=self._send_timeout_s,
                    )
                async with self._lock:
                    current = self._clients.get(client_id)
                    if current is not None:
                        current.sent_frames += 1
            except asyncio.TimeoutError:
                async with self._lock:
                    current = self._clients.get(client_id)
                    if current is not None:
                        current.send_timeouts += 1
                raise
            except Exception:
                async with self._lock:
                    current = self._clients.get(client_id)
                    if current is not None:
                        current.send_errors += 1
                raise

    async def publish_control(
        self,
        client_id: str,
        payload: dict[str, Any],
    ) -> None:
        encoded = _canonical_json(payload)
        async with self._lock:
            client = self._clients.get(client_id)
            if client is None or client.closed:
                return
            if client.control_pending is not None:
                client.replaced_controls += 1
            client.control_pending = encoded
            client.event.set()

    async def publish_diagnostics(
        self,
        client_id: str,
        payload: dict[str, Any],
    ) -> None:
        """Offer one replaceable low-rate server diagnostics generation."""
        encoded = _canonical_json(payload)
        async with self._lock:
            client = self._clients.get(client_id)
            if client is None or client.closed:
                return
            if client.diagnostics_pending is not None:
                client.replaced_diagnostics += 1
            client.diagnostics_pending = encoded
            client.event.set()

    async def request_resync(self, client_id: str) -> None:
        async with self._lock:
            client = self._clients.get(client_id)
            if client is not None:
                client.last_manifest_identity = None
                if client.pending is not None:
                    client.event.set()

    async def update_client_diagnostics(
        self,
        client_id: str,
        diagnostics: dict[str, Any],
    ) -> None:
        """Store one bounded, explicitly client-reported metrics window."""
        async with self._lock:
            client = self._clients.get(client_id)
            if client is None or client.closed:
                return
            client.client_diagnostics = dict(diagnostics)
            client.client_diagnostics_received_at_ns = time.monotonic_ns()

    async def status(self) -> dict[str, Any]:
        async with self._lock:
            clients = {
                client_id: {
                    "pending": client.pending is not None,
                    "last_manifest_revision": (
                        client.last_manifest_identity[1]
                        if client.last_manifest_identity is not None
                        else 0
                    ),
                    "last_manifest_producer_epoch": (
                        client.last_manifest_identity[0]
                        if client.last_manifest_identity is not None
                        else None
                    ),
                    "sent_frames": client.sent_frames,
                    "replaced_frames": client.replaced_frames,
                    "replaced_controls": client.replaced_controls,
                    "replaced_diagnostics": client.replaced_diagnostics,
                    "send_errors": client.send_errors,
                    "send_timeouts": client.send_timeouts,
                    "client_diagnostics": client.client_diagnostics,
                    "client_diagnostics_received_at_ns": (
                        client.client_diagnostics_received_at_ns
                    ),
                    "qualification_send_delay_ms": (
                        client.qualification_send_delay_s * 1000.0
                    ),
                }
                for client_id, client in self._clients.items()
            }
            replaced_frames_total = sum(
                client.replaced_frames for client in self._clients.values()
            ) + self._retired_replaced_frames
            replaced_controls_total = sum(
                client.replaced_controls for client in self._clients.values()
            ) + self._retired_replaced_controls
            replaced_diagnostics_total = sum(
                client.replaced_diagnostics for client in self._clients.values()
            ) + self._retired_replaced_diagnostics
            send_errors_total = sum(
                client.send_errors for client in self._clients.values()
            ) + self._retired_send_errors
            send_timeouts_total = sum(
                client.send_timeouts for client in self._clients.values()
            ) + self._retired_send_timeouts
            with self._ingress_lock:
                ingress_pending = self._ingress_pending is not None
                ingress_replacements = self._ingress_replacements
                ingress_schedule_failures = self._ingress_schedule_failures
            return {
                "client_count": len(clients),
                "published_frames": self._published_frames,
                "replaced_frames_total": replaced_frames_total,
                "replaced_controls_total": replaced_controls_total,
                "replaced_diagnostics_total": replaced_diagnostics_total,
                "send_errors_total": send_errors_total,
                "send_timeouts_total": send_timeouts_total,
                "ingress_pending": ingress_pending,
                "ingress_replacements": ingress_replacements,
                "ingress_schedule_failures": ingress_schedule_failures,
                "clients": clients,
            }
