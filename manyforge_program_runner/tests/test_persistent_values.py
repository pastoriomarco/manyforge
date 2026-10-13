"""Operator edits of persistent blackboard values, with and without a run."""

from __future__ import annotations

import hashlib
import json
import os
import stat
from copy import deepcopy
from pathlib import Path
from typing import Any

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_values import ProgramValueError, ProgramValueStore
from manyforge_behavior.program import Program
from manyforge_behavior.program_run_owner import ProgramRunOwnerError
from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.program_binding_projection import (
    validate_committed_scenario_activation,
)
from manyforge_headless_api.models import (
    PersistentKeyValueResetRequest,
    PersistentKeyValueSetRequest,
    PersistentKeyValuesClearRequest,
)
import manyforge_program_runner.persistent_keys as persistent_keys
from manyforge_program_runner.cycle_journal import ProgramCycleJournal
from manyforge_program_runner.persistent_keys import PersistentKeyRegister
from manyforge_program_runner.runtime import (
    PERSISTENT_KEY_RECONCILIATION_VERSION,
    ProgramRunnerConfig,
    ProgramRunnerError,
    ProgramRuntime,
)
from manyforge_program_runner.runtime_controls import ProgramRuntimeControls
from manyforge_program_runner.tests.test_runtime import _Headless as _Transport


_ROOT = Path(__file__).resolve().parents[2]
_SCENARIO = "cell-pick"
_RUN = "program-cycle.1"


def _program(*keys: dict[str, Any]) -> dict[str, Any]:
    environment = build_node_environment()
    document = Program(
        _ROOT / "examples/pick_and_place_minimal.program.yaml", environment.catalog
    ).to_dict()
    document["blackboard"] = {
        "namespaces": [],
        "keys": sorted(keys, key=lambda row: row["key"]),
    }
    document["tree"] = {
        "node_id": "logic.check_condition",
        "instance_id": "check",
        "parameters": {"condition": True},
        "children": [],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    return document


def _key(key: str, value_type: str = "int64", **extra: Any) -> dict[str, Any]:
    return {
        "key": key,
        "type": value_type,
        "access": "read_write",
        "persistence": "persistent",
        **extra,
    }


_DOCUMENT = _program(
    _key("layer", initial=0),
    _key("counted"),
    _key("limit", "float64", access="read", initial=0.5),
    _key("scratch", persistence="run"),
)
_REVISION = _DOCUMENT["program_revision"]


class _Headless(_Transport):
    """Manager's committed Applies, as Headless projects the current one."""

    def __init__(self, history: str = "manager-state.1") -> None:
        super().__init__()
        # Manager state: a restart with new state is a new history.
        self.history = history
        self.applies: list[dict[str, Any]] = []
        self.current: dict[str, Any] | Exception = {}
        self.apply(_DOCUMENT)

    def apply(self, document: dict[str, Any] | None, scenario_id: str = _SCENARIO) -> str:
        """Commit one Scenario Apply; nothing tells the runner about it."""

        committed = {
            "apply_id": f"apply.{len(self.applies) + 1}",
            "apply_generation": 1,
            "record_digest": "sha256:"
            + hashlib.sha256(
                f"{self.history}:{len(self.applies) + 1}".encode()
            ).hexdigest(),
            "origin": {
                "kind": "scenario",
                "scenario_id": scenario_id,
                "source_document_digest": "sha256:"
                + hashlib.sha256(scenario_id.encode()).hexdigest(),
            },
        }
        self.applies.append(committed)
        self.current = {**committed, "program": deepcopy(document)}
        return committed["record_digest"]

    def apply_sealed_empty(self) -> str:
        """One named sealed-empty Apply stays in the complete history."""
        origin = {
            "kind": "sealed_canonical_empty",
            "cell_id": "cell.test",
            "deployment_artifact_id": "deployment.empty",
            "deployment_revision": "sha256:" + "a" * 64,
            "retained_bundle_id": "sha256:" + "b" * 64,
            "graph_revision": "sha256:" + "c" * 64,
            "admission_digest": "sha256:" + "d" * 64,
        }
        committed = {
            "apply_id": f"apply.{len(self.applies) + 1}",
            "apply_generation": 1,
            "record_digest": "sha256:"
            + hashlib.sha256(
                f"{self.history}:{len(self.applies) + 1}".encode()
            ).hexdigest(),
            "origin": origin,
        }
        self.applies.append(committed)
        self.current = {**deepcopy(committed), "program": None}
        return self.current["record_digest"]

    def committed_scenario_activation(self) -> dict[str, Any]:
        if isinstance(self.current, Exception):
            raise self.current
        # As the Headless client hands it to the runner: validated.
        return validate_committed_scenario_activation(
            {
                "schema_version": "manyforge.committed_scenario_activation.v2",
                "publication_generation": len(self.applies) + 1,
                **deepcopy(self.current),
                "committed_applies": sorted(
                    deepcopy(self.applies), key=lambda row: row["record_digest"]
                ),
            }
        )


class _Snapshot:
    def __init__(self, state: str, run_revision: int = 1) -> None:
        self.state = state
        self.run_revision = run_revision

    def to_dict(self) -> dict[str, Any]:
        return {
            "state": self.state,
            "capture": {"program_run_id": _RUN, "scenario_id": _SCENARIO},
        }


class _Owner:
    """One live run whose value store is built exactly as a run builds it."""

    def __init__(self, register: PersistentKeyRegister) -> None:
        self.store = ProgramValueStore.from_program(
            Program.from_dict(deepcopy(_DOCUMENT), build_node_environment().catalog),
            scenario_id=_SCENARIO,
            register=register,
        )
        self.state = "running"
        self.revision = 1
        self.writes: list[dict[str, Any]] = []

    def list_snapshots(self, *, active_only: bool, limit: int):
        assert active_only and limit >= 1
        return (_Snapshot(self.state, self.revision),), False

    def read(self, program_run_id: str) -> _Snapshot:
        assert program_run_id == _RUN
        return _Snapshot(self.state, self.revision)

    def note_runtime_control_change(self, program_run_id: str) -> None:
        assert program_run_id == _RUN
        self.revision += 1

    def _require_writable(self) -> None:
        if self.state not in {"running", "intervention_required"}:
            raise ProgramRunOwnerError(
                "runtime blackboard writes require a current Program run"
            )

    def write_runtime_blackboard(self, program_run_id: str, **values: Any) -> None:
        assert program_run_id == _RUN
        self._require_writable()
        self.writes.append(values)
        try:
            self.store.write(
                values["key"],
                values["value"],
                metadata={
                    "program_run_id": program_run_id,
                    "request_id": values["request_id"],
                    "value_source": "runtime_control",
                },
            )
        except ProgramValueError as error:
            raise ProgramRunOwnerError(str(error)) from error
        self.revision += 1

    def reset_runtime_blackboard(
        self, program_run_id: str, *, key: str, request_id: str
    ) -> None:
        assert program_run_id == _RUN
        self._require_writable()
        try:
            self.store.reset_to_initial(
                key,
                metadata={
                    "program_run_id": program_run_id,
                    "request_id": request_id,
                    "value_source": "runtime_control",
                },
            )
        except ProgramValueError as error:
            raise ProgramRunOwnerError(str(error)) from error
        self.revision += 1


class _NoRuns:
    def list_snapshots(self, *, active_only: bool, limit: int):
        return (), False


def _runtime(tmp_path: Path) -> tuple[ProgramRuntime, _Headless]:
    headless = _Headless()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch="program-runner.epoch.1",
            persistent_key_register_path=tmp_path / "persistent-keys.json",
        ),
        headless_client=headless,  # type: ignore[arg-type]
        execution_transport=headless,  # type: ignore[arg-type]
        admission_client=object(),  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3", owner_epoch="program-runner.epoch.1"
        ),
    )
    runtime._host.shutdown()  # noqa: SLF001
    runtime._owner = _NoRuns()  # type: ignore[assignment]  # noqa: SLF001
    runtime._started = True  # noqa: SLF001
    # The operator page reads the values (reconciling them) before editing.
    runtime.read_persistent_values(_SCENARIO)
    return runtime, headless


def _fence(runtime: ProgramRuntime) -> dict[str, Any]:
    """What the operator page sends: the live run it saw; without one nothing."""

    owner = runtime._owner  # noqa: SLF001
    if isinstance(owner, _Owner):
        return {"program_run_id": _RUN, "expected_run_revision": owner.revision}
    return {}


def _edit(runtime: ProgramRuntime, key: str, **extra: Any) -> dict[str, Any]:
    return {
        "request_id": f"request.{key}.{len(extra)}",
        "request_owner_epoch": "composer.epoch.1",
        "source_label": "operator_ui",
        "scenario_id": _SCENARIO,
        "key": key,
        "expected_program_revision": _REVISION,
        **_fence(runtime),
        **extra,
    }


def _set(
    runtime: ProgramRuntime, key: str, value: Any, **extra: Any
) -> PersistentKeyValueSetRequest:
    return PersistentKeyValueSetRequest.model_validate(
        _edit(runtime, key, value=value, **extra)
    )


def _reset(runtime: ProgramRuntime, key: str) -> PersistentKeyValueResetRequest:
    return PersistentKeyValueResetRequest.model_validate(_edit(runtime, key))


def _clear(request_id: str = "request.clear") -> PersistentKeyValuesClearRequest:
    return PersistentKeyValuesClearRequest.model_validate(
        {
            "request_id": request_id,
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "scenario_id": _SCENARIO,
        }
    )


def _rows(projection: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {row["key"]: row for row in projection["keys"]}


def _refusal(call) -> str:
    with pytest.raises(ProgramRunnerError) as refused:
        call()
    return refused.value.code


def test_values_are_listed_set_reset_and_cleared_without_a_run(tmp_path) -> None:
    runtime, _headless = _runtime(tmp_path)

    listed = runtime.read_persistent_values(_SCENARIO)
    assert listed["program_revision"] == _REVISION
    assert listed["program_run_id"] is None
    # Only persistent keys; run keys are not stored between runs.
    assert list(_rows(listed)) == ["counted", "layer", "limit"]
    assert _rows(listed)["layer"] == {
        "key": "layer",
        "type": "int64",
        "access": "read_write",
        "has_initial": True,
        "initial": 0,
        "stored": False,
        "value": None,
        "sequence": None,
    }

    written = runtime.set_persistent_value(_SCENARIO, _set(runtime, "layer", 3))
    assert written["action"] == "set"
    layer = _rows(written["values"])["layer"]
    assert (layer["stored"], layer["value"]) == (True, 3)
    # Committed before success is reported: a new register sees it.
    reopened = PersistentKeyRegister(tmp_path / "persistent-keys.json")
    assert reopened.values(_SCENARIO)["layer"]["value"] == 3

    reset = runtime.reset_persistent_value(_SCENARIO, _reset(runtime, "layer"))
    assert _rows(reset["values"])["layer"]["stored"] is False

    runtime.set_persistent_value(
        _SCENARIO, _set(runtime, "layer", 4, request_id="request.layer.again")
    )
    runtime.set_persistent_value(_SCENARIO, _set(runtime, "counted", 7))
    # A read key's stored value (the Program wrote it) is the Program's own.
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    limit = Program.from_dict(
        deepcopy(_DOCUMENT), build_node_environment().catalog
    ).blackboard_keys["limit"]
    register.commit(_SCENARIO, limit, 0.7)
    # Clear is one Reset per writable key: read keys keep their stored value.
    cleared = runtime.clear_persistent_values(_SCENARIO, _clear())
    assert cleared["removed"] == 2
    rows = _rows(runtime.read_persistent_values(_SCENARIO))
    assert (rows["layer"]["stored"], rows["layer"]["initial"]) == (False, 0)
    assert rows["counted"]["stored"] is False
    assert (rows["limit"]["stored"], rows["limit"]["value"]) == (True, 0.7)
    # Every accepted edit is audited with its request, in commit order.
    assert [
        (edit["action"], edit["key"], edit["request_id"])
        for edit in runtime.read_persistent_values(_SCENARIO)["edits"]
    ] == [
        ("set", "layer", "request.layer.1"),
        ("reset", "layer", "request.layer.0"),
        ("set", "layer", "request.layer.again"),
        ("set", "counted", "request.counted.1"),
        ("clear", None, "request.clear"),
    ]


def test_edits_are_validated_like_a_run_write_and_refused_by_name(tmp_path) -> None:
    runtime, headless = _runtime(tmp_path)

    assert _refusal(lambda: runtime.set_persistent_value(
        _SCENARIO, _set(runtime, "layer", 1.5)
    )) == "persistent_key_value_invalid"
    # Set and Reset are both operator value changes: a read key refuses both.
    assert _refusal(lambda: runtime.set_persistent_value(
        _SCENARIO, _set(runtime, "limit", 1.0)
    )) == "persistent_key_access_conflict"
    assert _refusal(lambda: runtime.reset_persistent_value(
        _SCENARIO, _reset(runtime, "limit")
    )) == "persistent_key_access_conflict"
    assert _refusal(lambda: runtime.set_persistent_value(
        _SCENARIO, _set(runtime, "scratch", 1)
    )) == "persistent_key_not_found"
    assert _refusal(lambda: runtime.set_persistent_value(
        _SCENARIO, _set(runtime, "layer", 1, expected_program_revision="sha256:" + "0" * 64)
    )) == "persistent_key_program_stale"
    assert _refusal(lambda: runtime.set_persistent_value(
        "other", _set(runtime, "layer", 1)
    )) == "persistent_key_path_invalid"

    headless.apply(_DOCUMENT, "other-scenario")
    assert _refusal(
        lambda: runtime.read_persistent_values(_SCENARIO)
    ) == "persistent_key_scenario_conflict"
    # Clear resets the Applied Program's writable keys, so it needs that Program.
    assert _refusal(
        lambda: runtime.clear_persistent_values(_SCENARIO, _clear())
    ) == "persistent_key_scenario_conflict"
    headless.current = HeadlessClientError(
        "program_applied_selection_not_active", "Apply in progress"
    )
    assert _refusal(
        lambda: runtime.read_persistent_values(_SCENARIO)
    ) == "persistent_key_activation_unavailable"
    # A Scenario applied without a Program declares no keys to edit.
    headless.apply(None)
    assert _refusal(
        lambda: runtime.read_persistent_values(_SCENARIO)
    ) == "persistent_key_program_unavailable"
    assert PersistentKeyRegister(tmp_path / "persistent-keys.json").values(
        _SCENARIO
    ) == {}


def test_a_live_run_owns_the_values_and_serializes_operator_edits(tmp_path) -> None:
    runtime, _headless = _runtime(tmp_path)
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    register.commit(
        _SCENARIO,
        Program.from_dict(
            deepcopy(_DOCUMENT), build_node_environment().catalog
        ).blackboard_keys["layer"],
        4,
    )
    owner = _Owner(register)
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    controls = ProgramRuntimeControls(
        {
            "program_run_id": _RUN,
            "program_id": _DOCUMENT["program_id"],
            "program_revision": _REVISION,
            "program_binding_id": "sha256:" + "b" * 64,
            "retained_bundle_id": "sha256:" + "a" * 64,
            "graph_revision": "sha256:" + "b" * 64,
            "node_environment_digest": "sha256:" + "a" * 64,
        }
    )
    runtime._runtime_controls[_RUN] = controls  # noqa: SLF001
    assert owner.store.read("layer") == 4

    written = runtime.set_persistent_value(_SCENARIO, _set(runtime, "layer", 5))
    # The run's own store took the write: visible value and register agree.
    assert owner.store.read("layer") == 5
    assert owner.writes == [
        {"key": "layer", "value": 5, "request_id": "request.layer.1"}
    ]
    assert _rows(written["values"])["layer"]["value"] == 5
    assert written["values"]["program_run_id"] == _RUN

    runtime.reset_persistent_value(_SCENARIO, _reset(runtime, "layer"))
    assert owner.store.read("layer") == 0
    assert owner.store.read_bundle("layer")["meta"]["value_source"] == "initial"
    assert "layer" not in register.values(_SCENARIO)
    runtime.set_persistent_value(_SCENARIO, _set(runtime, "counted", 2))
    runtime.reset_persistent_value(_SCENARIO, _reset(runtime, "counted"))
    # A key without `initial:` is empty again after Reset.
    assert not owner.store.contains("counted")
    assert [
        (event["kind"], event["key"]) for event in controls.snapshot()["audit"]
    ] == [
        ("persistent_value_written", "layer"),
        ("persistent_value_reset", "layer"),
        ("persistent_value_written", "counted"),
        ("persistent_value_reset", "counted"),
    ]
    # Clearing would bypass the live store; the run must end first.
    assert _refusal(
        lambda: runtime.clear_persistent_values(_SCENARIO, _clear())
    ) == "persistent_key_run_conflict"

    # Edits through the run are attributed to it and keep their request.
    assert [
        (
            edit["action"],
            edit["key"],
            edit["request_id"],
            edit["source_label"],
            edit["program_run_id"],
        )
        for edit in register.edits(_SCENARIO)
    ] == [
        ("set", "layer", "request.layer.1", "runtime_control", _RUN),
        ("reset", "layer", "request.layer.0", "runtime_control", _RUN),
        ("set", "counted", "request.counted.1", "runtime_control", _RUN),
        ("reset", "counted", "request.counted.0", "runtime_control", _RUN),
    ]

    owner.state = "full_stopped"
    with pytest.raises(ProgramRunnerError) as halted:
        runtime.set_persistent_value(
            _SCENARIO, _set(runtime, "layer", 9, request_id="request.layer.halted")
        )
    assert halted.value.code == "persistent_key_run_conflict"
    assert "require a current Program run" in str(halted.value)
    assert "layer" not in register.values(_SCENARIO)
    assert len(register.edits(_SCENARIO)) == 4


def test_a_runner_without_a_register_refuses_by_name(tmp_path) -> None:
    runtime, _headless = _runtime(tmp_path)
    runtime._persistent_keys = None  # noqa: SLF001
    assert _refusal(
        lambda: runtime.read_persistent_values(_SCENARIO)
    ) == "persistent_key_register_unavailable"


def test_a_failed_reset_leaves_the_stored_and_visible_value(tmp_path) -> None:
    register = PersistentKeyRegister(tmp_path / "persistent-keys.json")
    store = _Owner(register).store
    store.write("layer", 6)

    def refuse(*_args: Any, **_kwargs: Any) -> bool:
        raise OSError("read-only file system")

    register.remove = refuse  # type: ignore[method-assign]
    with pytest.raises(ProgramValueError, match="could not be reset"):
        store.reset_to_initial("layer")
    assert store.read("layer") == 6
    assert register.values(_SCENARIO)["layer"]["value"] == 6
    with pytest.raises(ProgramValueError, match="not persistent"):
        store.reset_to_initial("scratch")


def _layer() -> Any:
    return Program.from_dict(
        deepcopy(_DOCUMENT), build_node_environment().catalog
    ).blackboard_keys["layer"]


def _live(runtime: ProgramRuntime) -> _Owner:
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    owner = _Owner(register)
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._runtime_controls[_RUN] = ProgramRuntimeControls(  # noqa: SLF001
        {
            "program_run_id": _RUN,
            "program_id": _DOCUMENT["program_id"],
            "program_revision": _REVISION,
            "program_binding_id": "sha256:" + "b" * 64,
            "retained_bundle_id": "sha256:" + "a" * 64,
            "graph_revision": "sha256:" + "b" * 64,
            "node_environment_digest": "sha256:" + "a" * 64,
        }
    )
    return owner


def test_without_a_run_the_last_edit_wins_and_a_resend_never_applies(tmp_path) -> None:
    runtime, _headless = _runtime(tmp_path)
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None

    first = _set(runtime, "layer", 3, request_id="request.first")
    runtime.set_persistent_value(_SCENARIO, first)
    # Two different requests in sequence: the later one wins.
    runtime.set_persistent_value(
        _SCENARIO, _set(runtime, "layer", 8, request_id="request.second")
    )
    assert register.values(_SCENARIO)["layer"]["value"] == 8
    # Resending the first is refused by its identity: the value stays 8.
    assert _refusal(
        lambda: runtime.set_persistent_value(_SCENARIO, first)
    ) == "persistent_key_request_replayed"
    assert register.values(_SCENARIO)["layer"]["value"] == 8
    # A Reset brings no earlier state back, and is never applied twice.
    reset = _reset(runtime, "layer")
    runtime.reset_persistent_value(_SCENARIO, reset)
    for request in (first, reset):
        call = (
            runtime.set_persistent_value
            if isinstance(request, PersistentKeyValueSetRequest)
            else runtime.reset_persistent_value
        )
        assert _refusal(
            lambda: call(_SCENARIO, request)
        ) == "persistent_key_request_replayed"
    assert "layer" not in register.values(_SCENARIO)

    # A Clear after another edit is admitted; resending it is refused.
    runtime.set_persistent_value(_SCENARIO, _set(runtime, "counted", 1))
    clear = _clear()
    assert runtime.clear_persistent_values(_SCENARIO, clear)["removed"] == 1
    runtime.set_persistent_value(
        _SCENARIO, _set(runtime, "counted", 2, request_id="request.counted.again")
    )
    assert _refusal(
        lambda: runtime.clear_persistent_values(_SCENARIO, clear)
    ) == "persistent_key_request_replayed"
    assert register.values(_SCENARIO)["counted"]["value"] == 2

    # The same request identity while it is still in flight is not admitted.
    journal = runtime._journal  # noqa: SLF001
    assert journal is not None
    pending = _set(runtime, "layer", 4, request_id="request.pending")
    journal.record_request(
        owner_epoch=pending.request_owner_epoch,
        request_id=pending.request_id,
        kind="persistent_value_set",
        intent=pending.model_dump(mode="json"),
        cycle_generation=journal.read().cycle_generation,
    )
    assert _refusal(
        lambda: runtime.set_persistent_value(_SCENARIO, pending)
    ) == "execution_control_command_in_progress"
    assert "layer" not in register.values(_SCENARIO)
    # Refused and accepted edits leave nothing in flight behind them.
    assert [row["request_id"] for row in journal.pending_commands()] == [
        "request.pending"
    ]

    # A live run also fences on its revision; a resend is refused by its
    # identity there too.
    owner = _live(runtime)
    through_run = _set(runtime, "layer", 5, request_id="request.run")
    runtime.set_persistent_value(_SCENARIO, through_run)
    owner.store.write("layer", 6)
    assert _refusal(
        lambda: runtime.set_persistent_value(_SCENARIO, through_run)
    ) == "persistent_key_request_replayed"
    assert owner.store.read("layer") == 6
    # A new edit made on an older view of the run is stale.
    assert _refusal(
        lambda: runtime.set_persistent_value(
            _SCENARIO,
            _set(
                runtime,
                "layer",
                7,
                request_id="request.run.stale",
                expected_run_revision=owner.revision - 1,
            ),
        )
    ) == "persistent_key_values_stale"
    # An edit must name the live run that holds the values.
    assert _refusal(
        lambda: runtime.set_persistent_value(
            _SCENARIO,
            _set(runtime, "layer", 7, program_run_id=None, expected_run_revision=None),
        )
    ) == "persistent_key_values_stale"


def test_without_a_run_a_value_is_written_only_when_it_changes(tmp_path) -> None:
    runtime, _headless = _runtime(tmp_path)
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    path = tmp_path / "persistent-keys.json"

    # No stored row is never equal to `initial:`: setting it stores a row.
    runtime.set_persistent_value(
        _SCENARIO, _set(runtime, "layer", 0, request_id="request.initial")
    )
    assert register.values(_SCENARIO)["layer"]["value"] == 0
    stored = path.read_bytes()
    sequence = register.sequence

    # The same value again completes without rewriting the register.
    same = _set(runtime, "layer", 0, request_id="request.same")
    written = runtime.set_persistent_value(_SCENARIO, same)
    assert _rows(written["values"])["layer"]["value"] == 0
    assert path.read_bytes() == stored
    assert register.sequence == sequence
    assert [
        edit["request_id"]
        for edit in runtime.read_persistent_values(_SCENARIO)["edits"]
    ] == ["request.initial"]
    # It was still applied: a resend is refused by its identity.
    assert _refusal(
        lambda: runtime.set_persistent_value(_SCENARIO, same)
    ) == "persistent_key_request_replayed"

    # A changed value is written.
    runtime.set_persistent_value(
        _SCENARIO, _set(runtime, "layer", 1, request_id="request.changed")
    )
    assert register.values(_SCENARIO)["layer"]["value"] == 1
    assert register.sequence == sequence + 1
    assert [
        edit["request_id"]
        for edit in runtime.read_persistent_values(_SCENARIO)["edits"]
    ] == ["request.initial", "request.changed"]


def test_a_resend_stays_refused_after_many_node_writes_and_a_journal_reopen(
    tmp_path,
) -> None:
    runtime, _headless = _runtime(tmp_path)
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    first = _set(runtime, "layer", 3, request_id="request.first")
    runtime.set_persistent_value(_SCENARIO, first)
    runtime.set_persistent_value(
        _SCENARIO, _set(runtime, "layer", 8, request_id="request.second")
    )

    # A run's node writes carry no request: more of them than the register's
    # edit history holds never let the completed request back in.
    store = _Owner(register).store
    for value in range(100, 400):
        store.write("layer", value)
    assert len(register.edits(_SCENARIO)) == persistent_keys.MAX_EDITS
    assert not any(
        edit["request_id"] == "request.first" for edit in register.edits(_SCENARIO)
    )
    assert _refusal(
        lambda: runtime.set_persistent_value(_SCENARIO, first)
    ) == "persistent_key_request_replayed"
    assert register.values(_SCENARIO)["layer"]["value"] == 399

    # The completed identities survive a journal reopen (runner restart).
    journal = runtime._journal  # noqa: SLF001
    assert journal is not None
    journal.close()
    restarted, _headless = _runtime(tmp_path)
    assert _refusal(
        lambda: restarted.set_persistent_value(_SCENARIO, first)
    ) == "persistent_key_request_replayed"
    assert register.path == restarted._persistent_keys.path  # noqa: SLF001
    assert PersistentKeyRegister(register.path).values(_SCENARIO)["layer"][
        "value"
    ] == 399


def test_an_edit_names_the_live_run_with_its_revision_or_nothing() -> None:
    base = {
        "request_id": "request.1",
        "request_owner_epoch": "composer.epoch.1",
        "source_label": "operator_ui",
        "scenario_id": _SCENARIO,
        "key": "layer",
        "expected_program_revision": _REVISION,
    }
    for fence in (
        {"program_run_id": _RUN},
        {"expected_run_revision": 1},
        # The register sequence is no longer a fence.
        {"expected_sequence": 0},
    ):
        with pytest.raises(ValueError):
            PersistentKeyValueResetRequest.model_validate({**base, **fence})
    PersistentKeyValueResetRequest.model_validate(base)
    PersistentKeyValueResetRequest.model_validate(
        {**base, "program_run_id": _RUN, "expected_run_revision": 1}
    )


def test_a_program_without_persistent_keys_still_drops_the_scenarios_rows(
    tmp_path,
) -> None:
    runtime, headless = _runtime(tmp_path)
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    runtime.set_persistent_value(_SCENARIO, _set(runtime, "layer", 9))
    headless.apply(_program(_key("scratch", persistence="run")))

    reconciled = runtime.reconcile_persistent_values()

    assert reconciled["outcome"] == "reconciled"
    assert reconciled["dropped"] == [
        {
            "scenario_id": _SCENARIO,
            "key": "layer",
            "reason": "key_not_declared_persistent",
        }
    ]
    assert register.values(_SCENARIO) == {}
    # Re-declaring the key later never brings the dropped value back.
    headless.apply(_DOCUMENT)
    assert _rows(runtime.read_persistent_values(_SCENARIO))["layer"]["stored"] is False


def _fail_directory_sync(monkeypatch: pytest.MonkeyPatch) -> list[bool]:
    failing = [True]
    sync = persistent_keys.os.fsync

    def fsync(descriptor: int) -> None:
        if failing[0] and stat.S_ISDIR(os.fstat(descriptor).st_mode):
            raise OSError("I/O error")
        sync(descriptor)

    monkeypatch.setattr(persistent_keys.os, "fsync", fsync)
    return failing


def test_an_edit_that_may_not_be_durable_is_reported_as_such(
    tmp_path, monkeypatch: pytest.MonkeyPatch
) -> None:
    runtime, _headless = _runtime(tmp_path)
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    failing = _fail_directory_sync(monkeypatch)

    written = _set(runtime, "layer", 3)
    assert _refusal(
        lambda: runtime.set_persistent_value(_SCENARIO, written)
    ) == "persistent_key_storage_uncertain"
    # The file has the value, so the register holds it, and the request is
    # completed: a resend is refused at once, never applied twice ...
    assert register.values(_SCENARIO)["layer"]["value"] == 3
    assert _refusal(
        lambda: runtime.set_persistent_value(_SCENARIO, written)
    ) == "persistent_key_request_replayed"
    # ... and until it is reconciled with disk nothing is shown or changed.
    assert _refusal(
        lambda: runtime.set_persistent_value(
            _SCENARIO, _set(runtime, "layer", 4, request_id="request.layer.blocked")
        )
    ) == "persistent_key_storage_unavailable"
    listed = runtime.read_persistent_values(_SCENARIO)
    assert listed["storage_fault"]["code"] == "persistent_key_storage_unavailable"
    assert not any(row["stored"] for row in listed["keys"])
    failing[0] = False
    assert _refusal(
        lambda: runtime.set_persistent_value(_SCENARIO, written)
    ) == "persistent_key_request_replayed"
    runtime.set_persistent_value(
        _SCENARIO, _set(runtime, "layer", 4, request_id="request.layer.after")
    )
    assert PersistentKeyRegister(register.path).values(_SCENARIO)["layer"]["value"] == 4

    # Through a live run the run shows the change and advances its revision.
    owner = _live(runtime)
    failing[0] = True
    through_run = _set(runtime, "layer", 5, request_id="request.layer.run")
    assert _refusal(
        lambda: runtime.set_persistent_value(_SCENARIO, through_run)
    ) == "persistent_key_storage_uncertain"
    assert owner.store.read("layer") == 5
    assert register.values(_SCENARIO)["layer"]["value"] == 5
    assert _refusal(
        lambda: runtime.set_persistent_value(_SCENARIO, through_run)
    ) == "persistent_key_request_replayed"
    failing[0] = False
    assert _refusal(
        lambda: runtime.set_persistent_value(_SCENARIO, through_run)
    ) == "persistent_key_request_replayed"


def test_sealed_current_reconciliation_is_read_only_and_repeated(tmp_path, monkeypatch):
    runtime, headless = _runtime(tmp_path)
    runtime.set_persistent_value(_SCENARIO, _set(runtime, "layer", 9))
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    original_bytes = register.path.read_bytes()
    original_sequence = register.sequence
    original_activation = deepcopy(register.activation)
    digest = headless.apply_sealed_empty()
    projection = headless.committed_scenario_activation()

    def no_register_write(*_args, **_kwargs):
        raise AssertionError("sealed origin must not reconcile the Scenario register")

    monkeypatch.setattr(register, "reconcile", no_register_write)
    expected = {
        "schema_version": PERSISTENT_KEY_RECONCILIATION_VERSION,
        "outcome": "no_scenario", "record_digest": digest,
        "origin": projection["origin"], "dropped": [], "unobserved_applies": [],
        "history_changed": False,
    }
    assert runtime.reconcile_persistent_values() == expected
    assert runtime.reconcile_persistent_values() == expected
    assert register.path.read_bytes() == original_bytes
    assert register.sequence == original_sequence
    assert register.activation == original_activation
    assert register.values(_SCENARIO)["layer"]["value"] == 9
    assert _refusal(
        lambda: runtime._committed_activation()  # noqa: SLF001
    ) == "persistent_key_activation_unavailable"


def test_sealed_current_checks_expected_record_before_scenario_refusal(tmp_path):
    runtime, headless = _runtime(tmp_path)
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    original = register.path.read_bytes()
    old_digest = headless.current["record_digest"]
    actual = headless.apply_sealed_empty()
    assert _refusal(lambda: runtime._reconcile_persistent_keys(  # noqa: SLF001
        register, expected_record_digest=old_digest
    )) == "program_run_applied_selection_stale"
    assert _refusal(lambda: runtime._reconcile_persistent_keys(  # noqa: SLF001
        register, expected_record_digest=actual
    )) == "persistent_key_activation_unavailable"
    assert register.path.read_bytes() == original


@pytest.mark.parametrize(
    "defect",
    [
        "duplicate", "missing_current", "foreign_origin", "bad_sealed_digest",
        "sealed_program", "unknown_kind", "mixed_fields", "extra_field",
    ],
)
def test_full_typed_history_is_validated_before_sealed_noop(tmp_path, monkeypatch, defect):
    runtime, headless = _runtime(tmp_path)
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    original = register.path.read_bytes()
    headless.apply_sealed_empty()
    projection = headless.committed_scenario_activation()
    if defect == "duplicate":
        projection["committed_applies"].append(deepcopy(projection["committed_applies"][0]))
        projection["publication_generation"] += 1
    elif defect == "missing_current":
        projection["record_digest"] = "sha256:" + "e" * 64
    elif defect == "foreign_origin":
        projection["origin"]["cell_id"] = "cell.foreign"
    elif defect == "bad_sealed_digest":
        sealed = next(
            row for row in projection["committed_applies"]
            if row["origin"]["kind"] == "sealed_canonical_empty"
        )
        sealed["origin"]["admission_digest"] = "invalid"
    elif defect == "sealed_program":
        projection["program"] = deepcopy(_DOCUMENT)
    elif defect == "unknown_kind":
        projection["origin"]["kind"] = "unrecognized"
    elif defect == "mixed_fields":
        projection["origin"]["scenario_id"] = _SCENARIO
    else:
        projection["unexpected_field"] = True
    # A fake transport bypasses its validator; the runtime must still reject
    # the complete history before branching or touching its register.
    monkeypatch.setattr(headless, "committed_scenario_activation", lambda: projection)
    assert _refusal(
        runtime.reconcile_persistent_values
    ) == "persistent_key_activation_unavailable"
    assert register.path.read_bytes() == original


@pytest.mark.parametrize("stale", [False, True])
def test_unreadable_scenario_program_keeps_precedence_over_stale_record(tmp_path, stale):
    runtime, headless = _runtime(tmp_path)
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    original = register.path.read_bytes()
    headless.apply({"unreadable_program": True})
    expected = "sha256:" + "f" * 64 if stale else headless.current["record_digest"]
    assert _refusal(lambda: runtime._reconcile_persistent_keys(  # noqa: SLF001
        register, expected_record_digest=expected
    )) == "persistent_key_program_unavailable"
    assert register.path.read_bytes() == original


@pytest.mark.parametrize("schema", [
    "manyforge.committed_scenario_activation.v1",
    "manyforge.committed_scenario_activation.v2",
])
def test_originless_projection_is_a_named_refusal_with_a_stale_parser(
    tmp_path, monkeypatch, schema
):
    runtime, headless = _runtime(tmp_path)
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    original = register.path.read_bytes()
    projection = headless.committed_scenario_activation()
    projection["schema_version"] = schema
    del projection["origin"]
    monkeypatch.setattr(headless, "committed_scenario_activation", lambda: projection)
    # Simulate the old dependency accepting an origin-less projection.
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.validate_committed_scenario_activation",
        lambda value: value,
    )
    assert _refusal(
        runtime.reconcile_persistent_values
    ) == "persistent_key_activation_unavailable"
    assert _refusal(
        lambda: runtime._committed_activation()  # noqa: SLF001
    ) == "persistent_key_activation_unavailable"
    assert register.path.read_bytes() == original


@pytest.mark.parametrize("current_kind", ["scenario", "sealed_canonical_empty"])
@pytest.mark.parametrize("extra", [0, 1])
def test_complete_history_bound_counts_sealed_rows(tmp_path, current_kind, extra):
    runtime, headless = _runtime(tmp_path)
    runtime.set_persistent_value(_SCENARIO, _set(runtime, "layer", 9))
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    original = register.path.read_bytes()
    count = persistent_keys.MAX_RECONCILED_APPLIES + extra
    scenario_rows = 2 if current_kind == "scenario" else 1
    for _ in range(count - scenario_rows):
        headless.apply_sealed_empty()
    if current_kind == "scenario":
        headless.apply(_DOCUMENT)
    assert len(headless.committed_scenario_activation()["committed_applies"]) == count
    if extra:
        assert _refusal(
            runtime.reconcile_persistent_values
        ) == "persistent_key_activation_unavailable"
        assert register.path.read_bytes() == original
    else:
        result = runtime.reconcile_persistent_values()
        assert result["outcome"] == (
            "reconciled" if current_kind == "scenario" else "no_scenario"
        )
    assert register.values(_SCENARIO)["layer"]["value"] == 9


def test_distinct_scenario_a_empty_b_keeps_a_values_without_a_fake_scenario(tmp_path):
    runtime, headless = _runtime(tmp_path)
    runtime.set_persistent_value(_SCENARIO, _set(runtime, "layer", 9))
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None
    original = register.path.read_bytes()
    empty = headless.apply_sealed_empty()
    assert runtime.reconcile_persistent_values()["outcome"] == "no_scenario"
    assert register.path.read_bytes() == original
    other_scenario = "cell-other"
    current = headless.apply(_DOCUMENT, other_scenario)
    result = runtime.reconcile_persistent_values()
    assert result["scenario_id"] == other_scenario
    assert result["record_digest"] == current
    assert result["dropped"] == []
    assert result["unobserved_applies"] == []
    assert register.values(_SCENARIO)["layer"]["value"] == 9
    assert register.activation["scenario_id"] == other_scenario
    assert empty not in json.loads(register.path.read_text())["reconciled_applies"]
    assert _refusal(
        lambda: runtime.read_persistent_values(_SCENARIO)
    ) == "persistent_key_scenario_conflict"
