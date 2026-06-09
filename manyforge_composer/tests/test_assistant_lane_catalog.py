"""Structured-metadata contract tests for the assistant lane catalog.

Tests assert on the stable fields callers depend on (``runtime``,
``routing``, ``isolation``, ``aliases``, ``id``) — NOT on description
prose. Per the 2026-06-03 external review:

    > I would make tests assert metadata, not prose. Better than
    > checking that the description contains a phrase.

Each lane carries structured fields that encode its operational
properties (where it runs, how it routes, what isolation applies).
Tests that depend on those properties query the structured fields;
docs and UI may render the description, which can be polished
without breaking the contract.
"""
from __future__ import annotations

import pytest

from manyforge_composer.backend.assistant_provider import (
    DeterministicSceneAuthoringProvider,
    HermesAssistantProvider,
    LANE_ID_ALIASES,
    LANE_REGISTRY,
    NemoClawAssistantProvider,
    _LANE_ISOLATIONS,
    _LANE_ROUTINGS,
    _LANE_RUNTIMES,
    assistant_provider_catalog,
    build_assistant_provider,
)


# ---------------------------------------------------------------------------
# LANE_REGISTRY: per-lane structured metadata
# ---------------------------------------------------------------------------


def test_lane_registry_direct_runs_in_nemoclaw_sandbox() -> None:
    """The Direct lane's 'direct' name refers to routing, NOT host
    execution — components still run inside the NemoClaw sandbox.
    This invariant is the safety property the legacy ``nemoclaw``
    alias was carrying. Pin it here."""
    entry = LANE_REGISTRY["direct"]
    assert entry["runtime"] == "nemoclaw"
    assert entry["routing"] == "direct-vllm"
    assert entry["isolation"] == "sandbox"
    assert "nemoclaw" in entry["aliases"]


def test_lane_registry_openclaw_runs_in_nemoclaw_sandbox() -> None:
    entry = LANE_REGISTRY["openclaw"]
    assert entry["runtime"] == "nemoclaw"
    assert entry["routing"] == "openclaw-gateway"
    assert entry["isolation"] == "sandbox"
    assert entry["aliases"] == ()


def test_lane_registry_hermes_runs_in_nemoclaw_sandbox() -> None:
    entry = LANE_REGISTRY["hermes"]
    assert entry["runtime"] == "nemoclaw"
    assert entry["routing"] == "hermes-sessions"
    assert entry["isolation"] == "sandbox"
    assert entry["aliases"] == ()


# ---------------------------------------------------------------------------
# Cross-lane invariants
# ---------------------------------------------------------------------------


def test_all_lanes_share_nemoclaw_sandbox_isolation() -> None:
    """All currently-registered lanes run inside the NemoClaw sandbox
    with sandbox isolation. If a future lane breaks this invariant,
    surface the change as an intentional decision rather than letting
    it land silently."""
    for lane_id, entry in LANE_REGISTRY.items():
        assert entry["runtime"] == "nemoclaw", (
            f"LANE_REGISTRY[{lane_id!r}].runtime is not 'nemoclaw'; "
            "if you are intentionally adding a non-sandboxed lane, "
            "update this invariant and the security review notes."
        )
        assert entry["isolation"] == "sandbox", (
            f"LANE_REGISTRY[{lane_id!r}].isolation is not 'sandbox'; "
            "same caveat as runtime."
        )


def test_lane_routings_are_unique() -> None:
    """Two lanes must not share a routing — the routing field is what
    distinguishes lanes operationally."""
    seen: set[str] = set()
    for lane_id, entry in LANE_REGISTRY.items():
        routing = entry["routing"]
        assert routing not in seen, (
            f"LANE_REGISTRY[{lane_id!r}].routing={routing!r} duplicates "
            "another lane; routings must be unique."
        )
        seen.add(routing)


def test_lane_metadata_uses_only_allowed_enum_values() -> None:
    """Every entry must pick from the module's frozenset enums. The
    module-level validator at import already enforces this; this test
    pins the contract so a future reviewer can find it without
    chasing through the validator code."""
    for lane_id, entry in LANE_REGISTRY.items():
        assert entry["runtime"] in _LANE_RUNTIMES, (
            f"LANE_REGISTRY[{lane_id!r}].runtime not in _LANE_RUNTIMES"
        )
        assert entry["routing"] in _LANE_ROUTINGS, (
            f"LANE_REGISTRY[{lane_id!r}].routing not in _LANE_ROUTINGS"
        )
        assert entry["isolation"] in _LANE_ISOLATIONS, (
            f"LANE_REGISTRY[{lane_id!r}].isolation not in _LANE_ISOLATIONS"
        )


def test_lane_aliases_resolve_via_lane_id_aliases() -> None:
    """Every alias declared on a LANE_REGISTRY entry must also appear
    in LANE_ID_ALIASES pointing back to its canonical lane id. The two
    structures must stay in sync."""
    for lane_id, entry in LANE_REGISTRY.items():
        for alias in entry["aliases"]:
            assert LANE_ID_ALIASES.get(alias) == lane_id, (
                f"LANE_REGISTRY[{lane_id!r}].aliases declares {alias!r} "
                f"but LANE_ID_ALIASES[{alias!r}] does not map back to "
                f"{lane_id!r}; these two structures must agree."
            )


# ---------------------------------------------------------------------------
# Catalog API surface (assistant_provider_catalog)
# ---------------------------------------------------------------------------


def _catalog_by_id(configured: object | None) -> dict[str, dict]:
    return {row["id"]: row for row in assistant_provider_catalog(configured)}  # type: ignore[arg-type]


def test_catalog_exposes_one_row_per_engine_lane() -> None:
    """The catalog has exactly one row per engine-backed lane id.
    No duplicate row for ``nemoclaw`` even though it's an alias."""
    rows = _catalog_by_id(None)
    for lane_id in LANE_REGISTRY:
        assert lane_id in rows, f"catalog missing row for lane {lane_id!r}"
    # Aliases must NOT appear as separate rows.
    for alias in LANE_ID_ALIASES:
        assert alias not in rows, (
            f"alias {alias!r} appears as a separate catalog row; aliases "
            "should be carried in the canonical row's 'aliases' field."
        )


def test_catalog_carries_structured_metadata_per_lane() -> None:
    """Each engine-backed catalog row carries the same structured
    metadata as the underlying LANE_REGISTRY entry."""
    rows = _catalog_by_id(None)
    for lane_id, entry in LANE_REGISTRY.items():
        row = rows[lane_id]
        assert row["runtime"] == entry["runtime"]
        assert row["routing"] == entry["routing"]
        assert row["isolation"] == entry["isolation"]
        assert row["aliases"] == list(entry["aliases"])


def test_catalog_direct_row_configured_for_canonical_id() -> None:
    """``configured_id="direct"`` → direct row marked configured."""
    provider = NemoClawAssistantProvider(
        endpoint_url="http://nemoclaw.local/assistant",
        provider_id="direct",
        request_fn=lambda payload: {"message": "unused"},
    )
    rows = _catalog_by_id(provider)
    assert rows["direct"]["configured"] is True
    assert rows["openclaw"]["configured"] is False
    assert rows["hermes"]["configured"] is False


def test_catalog_direct_row_configured_for_legacy_alias() -> None:
    """``configured_id="nemoclaw"`` (the legacy alias) → direct row
    STILL marked configured. No duplicate row appears for the alias."""
    provider = NemoClawAssistantProvider(
        endpoint_url="http://nemoclaw.local/assistant",
        provider_id="nemoclaw",
        request_fn=lambda payload: {"message": "unused"},
    )
    rows = _catalog_by_id(provider)
    assert rows["direct"]["configured"] is True
    assert "nemoclaw" not in rows


def test_catalog_openclaw_row_configured_for_canonical_id() -> None:
    provider = NemoClawAssistantProvider(
        endpoint_url="http://openclaw.local/assistant",
        provider_id="openclaw",
        request_fn=lambda payload: {"message": "unused"},
    )
    rows = _catalog_by_id(provider)
    assert rows["openclaw"]["configured"] is True
    assert rows["direct"]["configured"] is False


def test_catalog_hermes_row_active_after_phase_4() -> None:
    # Phase 4 landed: the Hermes lane is no longer inert. The row sources
    # ``inert`` from LANE_REGISTRY so it tracks activation state.
    rows = _catalog_by_id(None)
    assert rows["hermes"].get("inert") is False
    assert LANE_REGISTRY["hermes"]["inert"] is False


def test_build_assistant_provider_routes_hermes_after_phase_4() -> None:
    # No longer raises "inert"; returns the Hermes provider speaking the same
    # envelope, with the hermes provider_id preserved for audit.
    provider = build_assistant_provider("hermes", endpoint_url="http://127.0.0.1:8300/v1/manyforge/assistant")
    assert isinstance(provider, HermesAssistantProvider)
    assert isinstance(provider, NemoClawAssistantProvider)  # shares the transport
    assert provider.provider_id == "hermes"
