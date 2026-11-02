"""Payload-neutral viewer demand and bounded fanout primitives.

No versioned viewer schema, manifest/frame encoder, runtime sampler, or route is
defined here. These mechanics are retained as I9 client-port inputs and cannot
make the old Composer WebSocket contract current again.
"""
from __future__ import annotations

import asyncio
import hashlib
import json
import math
import re
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Final, Iterable

from fastapi import WebSocket

VIEWER_TRANSPORT_RESTORATION_GATE: Final = "I9"

_ALLOWED_VISIBILITY: Final = frozenset({"focused", "visible", "collapsed", "hidden"})
_ACTIVE_VISIBILITY: Final = frozenset({"focused", "visible"})
_LAYER_TOKEN: Final = re.compile(r"^[a-z][a-z0-9_.-]{0,63}$")
_MAX_LAYER_COUNT: Final = 32


def _bounded_viewer_layers(layers: Iterable[str]) -> frozenset[str]:
    if type(layers) in {str, bytes}:
        raise TypeError("viewer layers must be an iterable of strings")
    try:
        iterator = iter(layers)
    except TypeError as exc:
        raise TypeError("viewer layers must be an iterable of strings") from exc

    normalized: set[str] = set()
    for index, raw_layer in enumerate(iterator):
        if index >= _MAX_LAYER_COUNT:
            raise ValueError(
                f"viewer layer iterable exceeds {_MAX_LAYER_COUNT} entries"
            )
        if type(raw_layer) is not str:
            raise TypeError("viewer layers must contain only strings")
        layer = raw_layer.strip()
        if _LAYER_TOKEN.fullmatch(layer) is None:
            raise ValueError("viewer layers must be bounded portable tokens")
        normalized.add(layer)
    return frozenset(normalized)


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
        layers: Iterable[str] = ("default",),
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
        normalized_layers = _bounded_viewer_layers(layers)

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
class ViewerTransportItem:
    """One opaque already-built payload generation."""

    sequence: int
    payload: str | bytes


@dataclass
class _FanoutClient:
    client_id: str
    websocket: WebSocket
    event: asyncio.Event = field(default_factory=asyncio.Event)
    pending: ViewerTransportItem | None = None
    sent: int = 0
    replaced: int = 0
    send_errors: int = 0
    send_timeouts: int = 0
    closed: bool = False


class BoundedViewerFanout:
    """Capacity-one payload-neutral fanout with independent client writers.

    Producers perform no socket I/O.  Each client owns exactly one replaceable
    latest slot, and the cross-thread ingress is itself capacity one.
    """

    def __init__(self, *, send_timeout_s: float = 2.0) -> None:
        if not math.isfinite(send_timeout_s) or send_timeout_s <= 0.0:
            raise ValueError("send_timeout_s must be finite and positive")
        self._send_timeout_s = float(send_timeout_s)
        self._clients: dict[str, _FanoutClient] = {}
        self._lock = asyncio.Lock()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._published = 0
        self._retired_sent = 0
        self._retired_replaced = 0
        self._retired_send_errors = 0
        self._retired_send_timeouts = 0
        self._ingress_lock = threading.Lock()
        self._ingress_pending: ViewerTransportItem | None = None
        self._ingress_drain_scheduled = False
        self._ingress_replacements = 0
        self._ingress_schedule_failures = 0

    async def attach(self, websocket: WebSocket, *, client_id: str) -> None:
        client = str(client_id).strip()
        if not client:
            raise ValueError("client_id must not be empty")
        loop = asyncio.get_running_loop()
        async with self._lock:
            if self._loop is not None and self._loop is not loop:
                raise RuntimeError("viewer fanout cannot span event loops")
            if client in self._clients:
                raise ValueError(f"viewer client already attached: {client}")
            self._loop = loop
            self._clients[client] = _FanoutClient(
                client_id=client,
                websocket=websocket,
            )

    async def detach(self, client_id: str) -> None:
        async with self._lock:
            client = self._clients.pop(client_id, None)
            if client is None:
                return
            self._retired_sent += client.sent
            self._retired_replaced += client.replaced
            self._retired_send_errors += client.send_errors
            self._retired_send_timeouts += client.send_timeouts
            client.closed = True
            client.event.set()

    async def publish(self, item: ViewerTransportItem) -> None:
        async with self._lock:
            self._published += 1
            for client in self._clients.values():
                self._offer_locked(client, item)

    async def offer_to_client(
        self,
        client_id: str,
        item: ViewerTransportItem,
    ) -> None:
        async with self._lock:
            client = self._clients.get(client_id)
            if client is not None:
                self._offer_locked(client, item)

    @staticmethod
    def _offer_locked(
        client: _FanoutClient,
        item: ViewerTransportItem,
    ) -> None:
        if client.closed:
            return
        if client.pending is not None:
            client.replaced += 1
        client.pending = item
        client.event.set()

    def publish_threadsafe(self, item: ViewerTransportItem) -> bool:
        loop = self._loop
        if loop is None or loop.is_closed():
            return False
        schedule = False
        with self._ingress_lock:
            if self._ingress_pending is not None:
                self._ingress_replacements += 1
            self._ingress_pending = item
            if not self._ingress_drain_scheduled:
                self._ingress_drain_scheduled = True
                schedule = True
        if not schedule:
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
                item = self._ingress_pending
                self._ingress_pending = None
                if item is None:
                    self._ingress_drain_scheduled = False
                    return
            await self.publish(item)

    async def send_loop(self, client_id: str) -> None:
        while True:
            async with self._lock:
                client = self._clients.get(client_id)
                if client is None or client.closed:
                    return
                event = client.event
            await event.wait()
            async with self._lock:
                current = self._clients.get(client_id)
                if current is not client or client.closed:
                    return
                item = client.pending
                client.pending = None
                client.event.clear()
            if item is None:
                continue
            try:
                if isinstance(item.payload, bytes):
                    operation = client.websocket.send_bytes(item.payload)
                else:
                    operation = client.websocket.send_text(item.payload)
                await asyncio.wait_for(operation, timeout=self._send_timeout_s)
                async with self._lock:
                    current = self._clients.get(client_id)
                    if current is client:
                        client.sent += 1
            except asyncio.TimeoutError:
                async with self._lock:
                    current = self._clients.get(client_id)
                    if current is client:
                        client.send_timeouts += 1
                raise
            except Exception:
                async with self._lock:
                    current = self._clients.get(client_id)
                    if current is client:
                        client.send_errors += 1
                raise

    async def status(self) -> dict[str, Any]:
        async with self._lock:
            clients = {
                client_id: {
                    "pending": client.pending is not None,
                    "sent": client.sent,
                    "replaced": client.replaced,
                    "send_errors": client.send_errors,
                    "send_timeouts": client.send_timeouts,
                }
                for client_id, client in self._clients.items()
            }
            with self._ingress_lock:
                ingress_pending = self._ingress_pending is not None
                ingress_replacements = self._ingress_replacements
                ingress_schedule_failures = self._ingress_schedule_failures
            return {
                "client_count": len(clients),
                "published": self._published,
                "sent_total": (
                    self._retired_sent
                    + sum(client.sent for client in self._clients.values())
                ),
                "replaced_total": (
                    self._retired_replaced
                    + sum(client.replaced for client in self._clients.values())
                ),
                "send_errors_total": (
                    self._retired_send_errors
                    + sum(client.send_errors for client in self._clients.values())
                ),
                "send_timeouts_total": (
                    self._retired_send_timeouts
                    + sum(client.send_timeouts for client in self._clients.values())
                ),
                "ingress_pending": ingress_pending,
                "ingress_replacements": ingress_replacements,
                "ingress_schedule_failures": ingress_schedule_failures,
                "clients": clients,
            }


__all__ = [
    "BoundedViewerFanout",
    "ViewerDemandRegistry",
    "ViewerDemandSummary",
    "ViewerInputUnchanged",
    "ViewerLeaseGrant",
    "ViewerTransportItem",
    "freeze_viewer_render_signature",
    "freeze_viewer_signature",
    "viewer_effective_period_s",
]
