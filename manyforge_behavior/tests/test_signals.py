"""Tests for the runtime signal registry."""

from __future__ import annotations

from dataclasses import dataclass

from manyforge_behavior.signals import RuntimeSignalRegistry, SignalUpdate


@dataclass
class _StaticProvider:
    adapter_id: str
    updates: list[SignalUpdate]

    def poll_signal_updates(self, now_ns: int):
        return list(self.updates)


@dataclass
class _FailingProvider:
    adapter_id: str

    def poll_signal_updates(self, now_ns: int):
        raise RuntimeError("signal bus offline")


@dataclass
class _MismatchedProvider:
    adapter_id: str

    def poll_signal_updates(self, now_ns: int):
        return [
            SignalUpdate(
                adapter="wrong_adapter",
                signal="fixture_present",
                value=True,
            )
        ]


def test_runtime_signal_registry_polls_provider_and_records_health():
    registry = RuntimeSignalRegistry()
    registry.register_provider(
        _StaticProvider(
            adapter_id="plc",
            updates=[
                SignalUpdate(
                    adapter="plc",
                    signal="cell_ready",
                    value=True,
                    observed_at_ns=100,
                    stale_after_ns=1_000,
                    mirror_key="cell/ready",
                )
            ],
        )
    )

    registry.poll_providers(now_ns=200)
    snapshot = registry.snapshot(now_ns=200)

    assert snapshot["providerCount"] == 1
    assert snapshot["signalCount"] == 1
    provider = snapshot["providers"]["plc"]
    assert provider["connected"] is True
    assert provider["lastPollNs"] == 200
    signal = snapshot["signals"]["plc:cell_ready"]
    assert signal["value"] is True
    assert signal["freshness"] == "fresh"
    assert signal["mirrorKey"] == "cell/ready"


def test_runtime_signal_registry_marks_signal_stale_after_timeout():
    registry = RuntimeSignalRegistry()
    registry.publish(
        SignalUpdate(
            adapter="plc",
            signal="fixture_present",
            value=True,
            observed_at_ns=1_000,
            stale_after_ns=100,
            mirror_key="fixture/present",
        )
    )

    fresh = registry.get_signal("plc", "fixture_present", now_ns=1_050)
    assert fresh is not None
    assert fresh.freshness == "fresh"

    stale = registry.get_signal("plc", "fixture_present", now_ns=1_101)
    assert stale is not None
    assert stale.freshness == "stale"


def test_runtime_signal_registry_marks_provider_disconnected_on_poll_error():
    registry = RuntimeSignalRegistry()
    registry.register_provider(_FailingProvider(adapter_id="roxsie"))

    registry.poll_providers(now_ns=500)
    snapshot = registry.snapshot(now_ns=500)

    provider = snapshot["providers"]["roxsie"]
    assert provider["connected"] is False
    assert provider["lastPollNs"] == 500
    assert "signal bus offline" in provider["lastError"]


def test_runtime_signal_registry_rejects_provider_adapter_mismatch():
    registry = RuntimeSignalRegistry()
    registry.register_provider(_MismatchedProvider(adapter_id="roxsie"))

    registry.poll_providers(now_ns=750)
    snapshot = registry.snapshot(now_ns=750)

    provider = snapshot["providers"]["roxsie"]
    assert provider["connected"] is False
    assert "adapter mismatch" in provider["lastError"]
