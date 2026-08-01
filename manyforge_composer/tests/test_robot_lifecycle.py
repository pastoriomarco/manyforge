"""Focused thin lifecycle-client coverage during the I7/L0 cutover."""

from __future__ import annotations

from collections.abc import Iterator, Mapping
from pathlib import Path
import threading
from typing import Any

import pytest
from fastapi import HTTPException
from pydantic import ValidationError

from manyforge_composer.backend.robot_lifecycle import (
    LifecycleObservation,
    LifecycleTerminalObservation,
    RobotLifecycleClient,
    RobotLifecycleTransitionRecorder,
    reduce_lifecycle_observation,
    validate_real_robot_runtime_record,
)
from manyforge_composer.backend.routes_robot_lifecycle import (
    RobotLifecycleConfirmationRequest,
    _confirmed,
    create_robot_lifecycle_router,
)


def _observation(
    revision: int,
    *,
    enabled: bool,
    terminal: LifecycleTerminalObservation | None = None,
    epoch: str = "owner-1",
) -> LifecycleObservation:
    return LifecycleObservation(
        component_id="arm-a",
        owner_epoch=epoch,
        source_revision=revision,
        observed_at_ns=100,
        fresh_until_ns=200,
        material_facets={"enabled": enabled},
        adapter_evidence={"vendor_state": "ready"},
        last_terminal=terminal,
    )


def _observation_with_facets(material_facets: dict[str, Any]) -> LifecycleObservation:
    return LifecycleObservation(
        component_id="arm-a",
        owner_epoch="owner-1",
        source_revision=1,
        observed_at_ns=100,
        fresh_until_ns=200,
        material_facets=material_facets,
    )


def test_observation_reducer_reports_freshness_without_deriving_admission() -> None:
    reduced = reduce_lifecycle_observation(
        _observation(1, enabled=True),
        now_ns=201,
    )

    assert reduced["observation_fresh"] is False
    assert reduced["material_facets"] == {"enabled": True}
    assert "can_execute" not in reduced
    assert "motion_admitted" not in reduced


def test_observation_deep_freezes_inputs_and_reducer_returns_fresh_copies() -> None:
    facets = {
        "enabled": True,
        "nested": {"states": ["ready"]},
    }
    adapter = {"detail": "bounded"}
    observation = LifecycleObservation(
        component_id="arm-a",
        owner_epoch="owner-1",
        source_revision=1,
        observed_at_ns=100,
        fresh_until_ns=200,
        material_facets=facets,
        adapter_evidence=adapter,
    )
    facets["enabled"] = False
    facets["nested"]["states"][0] = "mutated"
    adapter["detail"] = "changed"

    assert observation.material_facets["enabled"] is True
    assert observation.material_facets["nested"]["states"] == ("ready",)
    assert observation.adapter_evidence["detail"] == "bounded"
    with pytest.raises(TypeError):
        observation.material_facets["nested"]["new"] = True

    reduced = reduce_lifecycle_observation(observation, now_ns=150)
    assert reduced["material_facets"]["nested"]["states"] == ["ready"]
    reduced["material_facets"]["nested"]["states"][0] = "caller mutation"
    assert reduce_lifecycle_observation(observation, now_ns=150)[
        "material_facets"
    ]["nested"]["states"] == ["ready"]


def test_observation_rejects_missing_or_ambiguous_owner_identity_and_numbers() -> None:
    values: dict[str, Any] = {
        "component_id": "arm-a",
        "owner_epoch": "owner-1",
        "source_revision": 1,
        "observed_at_ns": 100,
        "fresh_until_ns": 200,
    }

    for field_name in ("component_id", "owner_epoch"):
        invalid = dict(values)
        invalid[field_name] = "   "
        with pytest.raises(ValueError, match="must not be empty"):
            LifecycleObservation(**invalid)

        invalid[field_name] = object()
        with pytest.raises(TypeError, match="non-empty string"):
            LifecycleObservation(**invalid)

    for field_name in ("source_revision", "observed_at_ns", "fresh_until_ns"):
        invalid = dict(values)
        invalid[field_name] = True
        with pytest.raises(TypeError, match="integer"):
            LifecycleObservation(**invalid)

    with pytest.raises(TypeError, match="now_ns.*integer"):
        reduce_lifecycle_observation(
            LifecycleObservation(**values),
            now_ns=True,
        )


def test_observation_rejects_poison_without_stringification_or_iteration() -> None:
    class PoisonMapping(Mapping[str, Any]):
        def __getitem__(self, key: str) -> Any:
            raise AssertionError(f"mapping lookup attempted: {key}")

        def __iter__(self) -> Iterator[str]:
            raise AssertionError("mapping iteration attempted")

        def __len__(self) -> int:
            raise AssertionError("mapping length attempted")

    class Poison:
        def __hash__(self) -> int:
            return 7

        def __str__(self) -> str:
            raise AssertionError("arbitrary stringification attempted")

    with pytest.raises(TypeError, match="material_facets.*built-in mapping"):
        LifecycleObservation(
            component_id="arm-a",
            owner_epoch="owner-1",
            source_revision=1,
            observed_at_ns=100,
            fresh_until_ns=200,
            material_facets=PoisonMapping(),
        )
    with pytest.raises(TypeError, match="non-string mapping key"):
        LifecycleObservation(
            component_id="arm-a",
            owner_epoch="owner-1",
            source_revision=1,
            observed_at_ns=100,
            fresh_until_ns=200,
            material_facets={Poison(): "value"},  # type: ignore[dict-item]
        )
    with pytest.raises(TypeError, match="non-JSON value"):
        LifecycleObservation(
            component_id="arm-a",
            owner_epoch="owner-1",
            source_revision=1,
            observed_at_ns=100,
            fresh_until_ns=200,
            material_facets={"value": Poison()},
        )
    with pytest.raises(TypeError, match="adapter evidence key"):
        LifecycleObservation(
            component_id="arm-a",
            owner_epoch="owner-1",
            source_revision=1,
            observed_at_ns=100,
            fresh_until_ns=200,
            adapter_evidence={Poison(): "value"},  # type: ignore[dict-item]
        )
    with pytest.raises(TypeError, match="adapter evidence 'value'"):
        LifecycleObservation(
            component_id="arm-a",
            owner_epoch="owner-1",
            source_revision=1,
            observed_at_ns=100,
            fresh_until_ns=200,
            adapter_evidence={"value": Poison()},  # type: ignore[dict-item]
        )


def test_observation_rejects_cycles_depth_and_collection_overflow() -> None:
    cyclic: dict[str, Any] = {}
    cyclic["self"] = cyclic
    with pytest.raises(ValueError, match="container cycle"):
        _observation_with_facets(cyclic)

    deep: dict[str, Any] = {}
    cursor = deep
    for _ in range(10):
        child: dict[str, Any] = {}
        cursor["child"] = child
        cursor = child
    with pytest.raises(ValueError, match="nesting depth"):
        _observation_with_facets(deep)

    with pytest.raises(ValueError, match="exceeds 64 mapping entries"):
        _observation_with_facets(
            {"nested": {str(index): index for index in range(65)}}
        )
    with pytest.raises(ValueError, match="exceeds 256 sequence entries"):
        _observation_with_facets({"items": list(range(257))})
    with pytest.raises(ValueError, match="bounded value node count"):
        _observation_with_facets(
            {"rows": [[0] * 256 for _ in range(17)]}
        )


def test_observation_rejects_unbounded_or_ambiguous_scalars() -> None:
    with pytest.raises(ValueError, match="oversized integer"):
        _observation_with_facets({"value": 1 << 65})
    with pytest.raises(ValueError, match="non-finite"):
        _observation_with_facets({"value": float("nan")})
    with pytest.raises(ValueError, match="32768 UTF-8 bytes"):
        _observation_with_facets({"value": "x" * (32 * 1024 + 1)})
    with pytest.raises(ValueError, match="adapter evidence.*512 UTF-8 bytes"):
        LifecycleObservation(
            component_id="arm-a",
            owner_epoch="owner-1",
            source_revision=1,
            observed_at_ns=100,
            fresh_until_ns=200,
            adapter_evidence={"detail": "x" * 513},
        )


def test_transition_recorder_is_source_ordered_and_terminal_deduplicated() -> None:
    events: list[tuple[str, dict[str, object]]] = []

    def sink(action: str, *, details: dict[str, object]) -> None:
        events.append((action, details))

    terminal = LifecycleTerminalObservation(
        request_id="request-1",
        action_id="component.enable",
        result_revision=2,
        phase="succeeded",
    )
    recorder = RobotLifecycleTransitionRecorder(sink)
    recorder.observe(_observation(1, enabled=False))
    recorder.observe(_observation(2, enabled=True, terminal=terminal))
    recorder.observe(_observation(2, enabled=True, terminal=terminal))
    recorder.observe(_observation(1, enabled=False))

    assert [action for action, _ in events] == [
        "observation_baseline",
        "observed_transition",
        "request_terminal",
    ]


def test_transition_recorder_emits_exact_sorted_material_and_adapter_delta() -> None:
    events: list[tuple[str, dict[str, Any]]] = []
    recorder = RobotLifecycleTransitionRecorder(
        lambda action, *, details: events.append((action, details)),
        material_adapter_status_keys=("vendor_state",),
    )
    first = LifecycleObservation(
        component_id="arm-a",
        owner_epoch="owner-1",
        source_revision=1,
        observed_at_ns=100,
        fresh_until_ns=200,
        material_facets={"zeta": {"state": ["old"]}, "alpha": "old"},
        adapter_evidence={"vendor_state": "2", "detail": "first"},
    )
    second = LifecycleObservation(
        component_id="arm-a",
        owner_epoch="owner-1",
        source_revision=2,
        observed_at_ns=110,
        fresh_until_ns=210,
        material_facets={"zeta": {"state": ["new"]}, "alpha": "new"},
        adapter_evidence={"vendor_state": "4", "detail": "second"},
    )

    recorder.observe(first)
    recorder.observe(second)

    assert [action for action, _ in events] == [
        "observation_baseline",
        "observed_transition",
    ]
    transition = events[-1][1]
    assert transition["changed_facets"] == [
        "adapter.vendor_state",
        "alpha",
        "zeta",
    ]
    assert transition["changes"] == {
        "adapter.vendor_state": {"previous": "2", "current": "4"},
        "alpha": {"previous": "old", "current": "new"},
        "zeta": {
            "previous": {"state": ["old"]},
            "current": {"state": ["new"]},
        },
    }
    assert transition["adapter_evidence"] == {
        "vendor_state": "4",
        "detail": "second",
    }
    assert transition["causal_attribution"] == "not_inferred"
    transition["adapter_evidence"]["detail"] = "caller mutation"
    transition["changes"]["zeta"]["current"]["state"][0] = "caller mutation"
    assert second.adapter_evidence["detail"] == "second"
    assert second.material_facets["zeta"]["state"] == ("new",)


def test_transition_recorder_resets_baseline_on_owner_epoch_change() -> None:
    actions: list[str] = []
    recorder = RobotLifecycleTransitionRecorder(
        lambda action, *, details: actions.append(action)
    )
    recorder.observe(_observation(5, enabled=False, epoch="owner-1"))
    recorder.observe(_observation(1, enabled=True, epoch="owner-2"))

    assert actions == ["observation_baseline", "observation_baseline"]


def test_history_sink_failure_is_contained() -> None:
    def failing_sink(_action: str, *, details: dict[str, object]) -> None:
        del details
        raise RuntimeError("history unavailable")

    recorder = RobotLifecycleTransitionRecorder(failing_sink)
    recorder.observe(_observation(1, enabled=False))
    recorder.observe(_observation(2, enabled=True))


def test_transition_recorder_serializes_reduction_and_sink() -> None:
    entered_first_transition = threading.Event()
    release_first_transition = threading.Event()
    second_started = threading.Event()
    second_finished = threading.Event()
    events: list[tuple[str, int]] = []

    def sink(action: str, *, details: dict[str, Any]) -> None:
        revision = details["source_revision"]
        if action == "observed_transition" and revision == 2:
            entered_first_transition.set()
            assert release_first_transition.wait(timeout=1.0)
        events.append((action, revision))

    recorder = RobotLifecycleTransitionRecorder(sink)
    recorder.observe(_observation(1, enabled=False))

    first = threading.Thread(
        target=recorder.observe,
        args=(_observation(2, enabled=True),),
    )

    def record_second() -> None:
        second_started.set()
        recorder.observe(_observation(3, enabled=False))
        second_finished.set()

    second = threading.Thread(target=record_second)
    first.start()
    assert entered_first_transition.wait(timeout=1.0)
    second.start()
    assert second_started.wait(timeout=1.0)
    assert second_finished.wait(timeout=0.05) is False
    release_first_transition.set()
    first.join(timeout=1.0)
    second.join(timeout=1.0)

    assert first.is_alive() is False
    assert second.is_alive() is False
    assert events == [
        ("observation_baseline", 1),
        ("observed_transition", 2),
        ("observed_transition", 3),
    ]


def test_confirmation_is_strict_and_false_confirmation_is_structured() -> None:
    with pytest.raises(ValidationError):
        RobotLifecycleConfirmationRequest.model_validate({"confirmed": 1})
    with pytest.raises(ValidationError):
        RobotLifecycleConfirmationRequest.model_validate(
            {"confirmed": True, "unexpected": "value"}
        )
    with pytest.raises(HTTPException) as exc_info:
        _confirmed(
            RobotLifecycleConfirmationRequest(confirmed=False),
            "enable component",
        )
    assert exc_info.value.status_code == 409


def test_clients_and_routes_fail_before_state_or_transport_effects() -> None:
    class PoisonState:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"state was accessed: {name}")

    with pytest.raises(
        RuntimeError,
        match="composable_cell_robot_lifecycle_client_unavailable",
    ):
        RobotLifecycleClient(PoisonState())
    with pytest.raises(
        RuntimeError,
        match="composable_cell_robot_lifecycle_client_unavailable",
    ):
        create_robot_lifecycle_router(PoisonState())


def test_real_robot_record_validation_is_gated_before_path_access() -> None:
    with pytest.raises(
        RuntimeError,
        match="composable_cell_real_robot_record_validator_unavailable",
    ):
        validate_real_robot_runtime_record(
            deployment_path=Path("/definitely/not/read/deployment.yaml"),
            execution_config={},
        )
