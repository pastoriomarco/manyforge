"""Deterministic standard-library fakes shared by the I1a prototypes."""

from __future__ import annotations

import hashlib
import json
from threading import Lock

from .contracts import (
    RuntimeEpoch,
    RuntimeUnitId,
    RuntimeUnitObservation,
    RuntimeUnitSpec,
)


class FakeClock:
    def __init__(self, start_ns: int = 0) -> None:
        if start_ns < 0:
            raise ValueError("start_ns must be non-negative")
        self._now_ns = start_ns
        self._lock = Lock()

    def now_ns(self) -> int:
        with self._lock:
            return self._now_ns

    def advance_ns(self, amount_ns: int) -> int:
        if amount_ns < 0:
            raise ValueError("amount_ns must be non-negative")
        with self._lock:
            self._now_ns += amount_ns
            return self._now_ns


class FakeProcessAdapter:
    """Idempotent process fake whose observed epochs come from actual starts."""

    def __init__(self, clock: FakeClock) -> None:
        self._clock = clock
        self._lock = Lock()
        self._epoch_sequence = 0
        self._process_sequence = 10_000
        self._observations: dict[RuntimeUnitId, RuntimeUnitObservation] = {}
        self._spec_fingerprints: dict[RuntimeUnitId, str] = {}

    @staticmethod
    def _fingerprint(spec: RuntimeUnitSpec) -> str:
        payload = [
            {
                "command": list(spec.command),
                "dependencies": [str(item) for item in spec.dependencies],
                "kind": spec.kind,
                "runtime_unit_id": str(spec.runtime_unit_id),
            }
        ]
        encoded = json.dumps(
            payload, sort_keys=True, separators=(",", ":")
        ).encode("utf-8")
        return hashlib.sha256(encoded).hexdigest()

    def start(self, spec: RuntimeUnitSpec) -> RuntimeUnitObservation:
        with self._lock:
            existing = self._observations.get(spec.runtime_unit_id)
            if existing is not None and existing.health not in {
                "absent",
                "crashed",
                "dead",
                "failed",
                "missing",
                "stopped",
            }:
                return existing
            self._epoch_sequence += 1
            self._process_sequence += 1
            observation = RuntimeUnitObservation(
                runtime_unit_id=spec.runtime_unit_id,
                runtime_epoch=RuntimeEpoch(
                    f"{spec.runtime_unit_id}.epoch.{self._epoch_sequence}"
                ),
                health="ready",
                observed_monotonic_ns=self._clock.now_ns(),
                process_id=self._process_sequence,
            )
            self._observations[spec.runtime_unit_id] = observation
            self._spec_fingerprints[spec.runtime_unit_id] = self._fingerprint(spec)
            return observation

    def stop(self, runtime_unit_id: RuntimeUnitId) -> RuntimeUnitObservation:
        with self._lock:
            previous = self._observations.pop(runtime_unit_id, None)
            self._spec_fingerprints.pop(runtime_unit_id, None)
            epoch = (
                previous.runtime_epoch
                if previous is not None
                else RuntimeEpoch(f"{runtime_unit_id}.absent")
            )
            return RuntimeUnitObservation(
                runtime_unit_id=runtime_unit_id,
                runtime_epoch=epoch,
                health="stopped",
                observed_monotonic_ns=self._clock.now_ns(),
                detail="idempotent fake stop",
            )

    def observe(
        self, runtime_unit_id: RuntimeUnitId
    ) -> RuntimeUnitObservation | None:
        with self._lock:
            return self._observations.get(runtime_unit_id)

    def observe_all(self) -> tuple[RuntimeUnitObservation, ...]:
        with self._lock:
            return tuple(
                sorted(
                    self._observations.values(),
                    key=lambda item: str(item.runtime_unit_id),
                )
            )

    def observed_spec_fingerprint(
        self, runtime_unit_id: RuntimeUnitId
    ) -> str | None:
        with self._lock:
            return self._spec_fingerprints.get(runtime_unit_id)

    def crash(self, runtime_unit_id: RuntimeUnitId) -> None:
        self.set_health(runtime_unit_id, "crashed", detail="injected fake crash")

    def set_health(
        self,
        runtime_unit_id: RuntimeUnitId,
        health: str,
        *,
        detail: str = "injected fake health",
    ) -> None:
        with self._lock:
            previous = self._observations.get(runtime_unit_id)
            if previous is None:
                return
            self._observations[runtime_unit_id] = RuntimeUnitObservation(
                runtime_unit_id=runtime_unit_id,
                runtime_epoch=previous.runtime_epoch,
                health=health,
                observed_monotonic_ns=self._clock.now_ns(),
                process_id=previous.process_id,
                detail=detail,
            )

    def drift_spec(
        self,
        runtime_unit_id: RuntimeUnitId,
        observed_spec: RuntimeUnitSpec,
    ) -> None:
        """Replace only the observed process-spec identity."""

        with self._lock:
            if runtime_unit_id not in self._observations:
                raise KeyError(str(runtime_unit_id))
            self._spec_fingerprints[runtime_unit_id] = self._fingerprint(
                observed_spec
            )
