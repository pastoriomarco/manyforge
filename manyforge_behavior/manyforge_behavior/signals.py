"""Runtime signal registry and provider contracts for external IO adapters."""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Any, Iterable, Protocol


@dataclass
class SignalUpdate:
    """One externally observed signal sample."""

    adapter: str
    signal: str
    value: Any
    observed_at_ns: int = field(default_factory=time.monotonic_ns)
    stale_after_ns: int | None = None
    mirror_key: str | None = None
    clear_on_stale: bool = True
    metadata: dict[str, Any] = field(default_factory=dict)

    @property
    def expires_at_ns(self) -> int | None:
        if self.stale_after_ns is None:
            return None
        return self.observed_at_ns + self.stale_after_ns


@dataclass
class SignalRecord:
    """Current runtime view of one signal."""

    adapter: str
    signal: str
    value: Any
    observed_at_ns: int
    stale_after_ns: int | None
    mirror_key: str | None
    clear_on_stale: bool
    metadata: dict[str, Any] = field(default_factory=dict)
    provider_id: str | None = None
    freshness: str = "fresh"

    @property
    def expires_at_ns(self) -> int | None:
        if self.stale_after_ns is None:
            return None
        return self.observed_at_ns + self.stale_after_ns

    def to_dict(self) -> dict[str, Any]:
        return {
            "adapter": self.adapter,
            "signal": self.signal,
            "value": self.value,
            "observedAtNs": self.observed_at_ns,
            "staleAfterNs": self.stale_after_ns,
            "expiresAtNs": self.expires_at_ns,
            "mirrorKey": self.mirror_key,
            "clearOnStale": self.clear_on_stale,
            "providerId": self.provider_id,
            "freshness": self.freshness,
            "metadata": dict(self.metadata),
        }


@dataclass
class SignalProviderHealth:
    """Operational state of one registered signal provider."""

    adapter_id: str
    connected: bool = True
    last_poll_ns: int | None = None
    last_success_ns: int | None = None
    last_error: str | None = None

    def to_dict(self, *, signal_count: int) -> dict[str, Any]:
        return {
            "adapterId": self.adapter_id,
            "connected": self.connected,
            "lastPollNs": self.last_poll_ns,
            "lastSuccessNs": self.last_success_ns,
            "lastError": self.last_error,
            "signalCount": signal_count,
        }


class SignalProvider(Protocol):
    """External provider that can poll one adapter-facing signal surface."""

    adapter_id: str

    def poll_signal_updates(self, now_ns: int) -> Iterable[SignalUpdate]:
        """Return zero or more signal updates observed at this poll."""


class RuntimeSignalRegistry:
    """Thread-safe store for external signal values and provider health."""

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._providers: dict[str, SignalProvider] = {}
        self._provider_health: dict[str, SignalProviderHealth] = {}
        self._signals: dict[tuple[str, str], SignalRecord] = {}
        self._change_counter = 0

    def _bump_change_counter_locked(self) -> None:
        self._change_counter += 1

    def register_provider(self, provider: SignalProvider) -> None:
        with self._lock:
            changed = provider.adapter_id not in self._providers
            self._providers[provider.adapter_id] = provider
            self._provider_health[provider.adapter_id] = SignalProviderHealth(
                adapter_id=provider.adapter_id
            )
            if changed:
                self._bump_change_counter_locked()

    def unregister_provider(self, adapter_id: str) -> None:
        with self._lock:
            changed = (
                adapter_id in self._providers
                or adapter_id in self._provider_health
                or any(record.adapter == adapter_id for record in self._signals.values())
            )
            self._providers.pop(adapter_id, None)
            self._provider_health.pop(adapter_id, None)
            self._signals = {
                key: record
                for key, record in self._signals.items()
                if record.adapter != adapter_id
            }
            if changed:
                self._bump_change_counter_locked()

    def publish(self, update: SignalUpdate, *, provider_id: str | None = None) -> None:
        with self._lock:
            key = (update.adapter, update.signal)
            existing = self._signals.get(key)
            record = SignalRecord(
                adapter=update.adapter,
                signal=update.signal,
                value=update.value,
                observed_at_ns=update.observed_at_ns,
                stale_after_ns=update.stale_after_ns,
                mirror_key=update.mirror_key,
                clear_on_stale=update.clear_on_stale,
                metadata=dict(update.metadata),
                provider_id=provider_id,
                freshness="fresh",
            )
            self._signals[key] = record
            if existing != record:
                self._bump_change_counter_locked()

    def publish_many(
        self,
        updates: Iterable[SignalUpdate],
        *,
        provider_id: str | None = None,
    ) -> None:
        for update in updates:
            self.publish(update, provider_id=provider_id)

    def poll_providers(self, *, now_ns: int | None = None) -> None:
        now_ns = now_ns or time.monotonic_ns()
        with self._lock:
            providers = list(self._providers.items())

        for adapter_id, provider in providers:
            try:
                updates = list(provider.poll_signal_updates(now_ns))
                for update in updates:
                    if update.adapter != adapter_id:
                        raise ValueError(
                            "signal update adapter mismatch: "
                            f"provider '{adapter_id}' published '{update.adapter}'"
                        )
            except Exception as exc:
                with self._lock:
                    health = self._provider_health.setdefault(
                        adapter_id,
                        SignalProviderHealth(adapter_id=adapter_id),
                    )
                    health.connected = False
                    health.last_poll_ns = now_ns
                    health.last_error = f"{type(exc).__name__}: {exc}"
                continue

            with self._lock:
                health = self._provider_health.setdefault(
                    adapter_id,
                    SignalProviderHealth(adapter_id=adapter_id),
                )
                health.connected = True
                health.last_poll_ns = now_ns
                health.last_success_ns = now_ns
                health.last_error = None
            self.publish_many(updates, provider_id=adapter_id)

        self.expire(now_ns=now_ns)

    def expire(self, *, now_ns: int | None = None) -> None:
        now_ns = now_ns or time.monotonic_ns()
        with self._lock:
            changed = False
            for record in self._signals.values():
                expires_at_ns = record.expires_at_ns
                if (
                    expires_at_ns is not None
                    and now_ns >= expires_at_ns
                    and record.freshness != "stale"
                ):
                    record.freshness = "stale"
                    changed = True
            if changed:
                self._bump_change_counter_locked()

    def change_counter(self, *, now_ns: int | None = None) -> int:
        """Return the monotonic counter for mirrored signal/provider changes."""
        self.expire(now_ns=now_ns)
        with self._lock:
            return self._change_counter

    def get_signal(
        self,
        adapter: str,
        signal: str,
        *,
        now_ns: int | None = None,
    ) -> SignalRecord | None:
        self.expire(now_ns=now_ns)
        with self._lock:
            record = self._signals.get((adapter, signal))
            if record is None:
                return None
            return SignalRecord(
                adapter=record.adapter,
                signal=record.signal,
                value=record.value,
                observed_at_ns=record.observed_at_ns,
                stale_after_ns=record.stale_after_ns,
                mirror_key=record.mirror_key,
                clear_on_stale=record.clear_on_stale,
                metadata=dict(record.metadata),
                provider_id=record.provider_id,
                freshness=record.freshness,
            )

    def snapshot(self, *, now_ns: int | None = None) -> dict[str, Any]:
        self.expire(now_ns=now_ns)
        with self._lock:
            provider_counts: dict[str, int] = {}
            for record in self._signals.values():
                provider_counts[record.adapter] = provider_counts.get(record.adapter, 0) + 1

            return {
                "providerCount": len(self._provider_health),
                "signalCount": len(self._signals),
                "providers": {
                    adapter_id: health.to_dict(
                        signal_count=provider_counts.get(adapter_id, 0)
                    )
                    for adapter_id, health in sorted(self._provider_health.items())
                },
                "signals": {
                    f"{adapter}:{signal}": record.to_dict()
                    for (adapter, signal), record in sorted(self._signals.items())
                },
            }
