"""Reset with Reload initial scene resets the persistent keys' stored values."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from typing import Any

import pytest

from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_headless_api.models import ProgramCycleResetRequest
import manyforge_program_runner.persistent_keys as persistent_keys
from manyforge_program_runner.cycle_journal import ProgramCycleJournal
from manyforge_program_runner.persistent_keys import (
    PersistentKeyEdit,
    PersistentKeyRegister,
    PersistentKeyRegisterError,
)
from manyforge_program_runner.runtime import (
    ProgramRunnerConfig,
    ProgramRunnerError,
    ProgramRuntime,
)
from manyforge_program_runner.tests.test_persistent_values import (
    _DOCUMENT,
    _PROJECT,
    _Headless,
)
from manyforge_program_runner.tests.test_runtime import (
    _Admission,
    _Headless as _Transport,
    _program_cycle_claim,
    _retain_terminal_cycle,
)


_OWNER = "runner.keys"
_OTHER_PROJECT = "/projects/other.scenario"


def _declarations() -> dict[str, Any]:
    return dict(
        Program.from_dict(
            deepcopy(_DOCUMENT), build_node_environment().catalog
        ).blackboard_keys
    )


def _stored(register: PersistentKeyRegister) -> None:
    """layer (read_write, initial 0), counted (no initial), limit (read)."""

    declarations = _declarations()
    register.commit(_PROJECT, declarations["layer"], 3)
    register.commit(_PROJECT, declarations["counted"], 7)
    register.commit(_PROJECT, declarations["limit"], 0.7)


def _values(register: PersistentKeyRegister) -> dict[str, Any]:
    return {key: row["value"] for key, row in register.values(_PROJECT).items()}


# -- the register --------------------------------------------------------------


def _reconciled_register(tmp_path: Path) -> PersistentKeyRegister:
    register = PersistentKeyRegister(tmp_path / "persistent-keys.json")
    register.reconcile(
        persistent_keys.CommittedActivation(
            record_digest="sha256:" + "a" * 64,
            project_path=_PROJECT,
            declarations=_declarations(),
            committed_applies={"sha256:" + "a" * 64: _PROJECT},
        )
    )
    return register


def test_register_resets_every_persistent_key_in_one_audited_write(tmp_path) -> None:
    register = _reconciled_register(tmp_path)
    _stored(register)
    register.commit(_OTHER_PROJECT, _declarations()["layer"], 5)
    sequence = register.sequence

    forgotten = register.reset_to_initial(
        _PROJECT,
        _declarations(),
        edit=PersistentKeyEdit("reset.1", "program_reset", program_run_id="program-cycle.1"),
    )

    # Every persistent key, read keys included; run keys are never stored.
    assert forgotten == {"counted": 7, "layer": 3, "limit": 0.7}
    assert register.values(_PROJECT) == {}
    # Another Scenario project keeps its values.
    assert register.values(_OTHER_PROJECT)["layer"]["value"] == 5
    # One write; one audit per key, naming the Reset and the value it had.
    assert register.sequence == sequence + 1
    resets = register.edits(_PROJECT)[-3:]
    assert [
        (edit["action"], edit["key"], edit["request_id"], edit["source_label"], edit["program_run_id"])
        for edit in resets
    ] == [
        ("reset", "counted", "reset.1", "program_reset", "program-cycle.1"),
        ("reset", "layer", "reset.1", "program_reset", "program-cycle.1"),
        ("reset", "limit", "reset.1", "program_reset", "program-cycle.1"),
    ]
    assert resets[1]["detail"] == "Reset reloaded the initial scene; was 3"
    # Durable: a new register reads the same.
    assert PersistentKeyRegister(register.path).values(_PROJECT) == {}
    # Nothing stored: nothing written, nothing audited.
    assert register.reset_to_initial(
        _PROJECT, _declarations(), edit=PersistentKeyEdit("reset.2", "program_reset")
    ) == {}
    assert register.sequence == sequence + 1


def test_register_reset_that_cannot_be_stored_changes_nothing(
    tmp_path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = _reconciled_register(tmp_path)
    _stored(register)
    sequence = register.sequence
    original = register.path.read_bytes()

    def refuse(*_args: Any) -> None:
        raise OSError("read-only file system")

    monkeypatch.setattr(persistent_keys.os, "replace", refuse)
    with pytest.raises(PersistentKeyRegisterError):
        register.reset_to_initial(
            _PROJECT, _declarations(), edit=PersistentKeyEdit("reset.1", "program_reset")
        )
    assert _values(register) == {"counted": 7, "layer": 3, "limit": 0.7}
    assert register.sequence == sequence
    assert register.path.read_bytes() == original


# -- the Program Reset ---------------------------------------------------------


def _reset_runtime(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    *,
    idle: bool,
    reload_scene: bool,
    events: list[str],
) -> tuple[ProgramRuntime, ProgramCycleJournal, PersistentKeyRegister, ProgramCycleResetRequest]:
    journal = ProgramCycleJournal(tmp_path / "cycle.sqlite3", owner_epoch=_OWNER)
    reference = None if idle else _retain_terminal_cycle(journal, _program_cycle_claim(_OWNER))

    class Admission(_Admission):
        def get(self):
            return {"admission_generation": 1, "authoring_allowed": True}

    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_epoch=_OWNER,
            persistent_key_register_path=tmp_path / "persistent-keys.json",
        ),
        headless_client=_Headless(),  # type: ignore[arg-type]
        execution_transport=_Transport(),  # type: ignore[arg-type]
        admission_client=Admission(),  # type: ignore[arg-type]
        cycle_journal=journal,
    )
    register = runtime._persistent_keys  # noqa: SLF001
    assert register is not None

    def scene(_request, _authorization) -> None:
        # Keys first: the Scene is reloaded only after they were reset.
        events.append(f"scene:{sorted(_values(register))}")

    # The between-Runs blackboard source is not under test here.
    monkeypatch.setattr(runtime, "_establish_startup_bbkey_source", lambda: None)
    monkeypatch.setattr(runtime, "_follow_cell_bbkey_source", lambda: None)
    monkeypatch.setattr(runtime, "_prepare_scene_after_reset", scene)
    runtime.start()
    runtime._reconcile_persistent_keys(register)  # noqa: SLF001
    _stored(register)
    request = ProgramCycleResetRequest.model_validate({
        "request_id": "reset.keys", "request_owner_epoch": "composer.1",
        "source_label": "operator_ui", "correlation_id": None,
        "program_run_id": None if idle else "program-cycle.1",
        "expected_owner_epoch": runtime.owner_epoch, "expected_cycle_generation": 1,
        "expected_claim_generation": None if idle else 1, "terminal_reference": reference,
        "settlement_evidence_digest": reference, "reload_initial_scene": reload_scene,
    })
    return runtime, journal, register, request


@pytest.mark.parametrize("idle", [False, True])
def test_reset_with_reload_returns_persistent_keys_to_initial(
    tmp_path, monkeypatch: pytest.MonkeyPatch, idle: bool
) -> None:
    events: list[str] = []
    runtime, journal, register, request = _reset_runtime(
        tmp_path, monkeypatch, idle=idle, reload_scene=True, events=events
    )
    try:
        runtime.reset_cycle(request)
        assert journal.read().state == "idle_after_reset"
        # Not stored any more: layer starts from `initial: 0`, counted
        # (no `initial:`) empty, and the read key limit from `initial: 0.5`.
        assert _values(register) == {}
        assert events == ["scene:[]"]
        # Recorded with this Reset in the stored-value edits.
        assert [
            (edit["action"], edit["key"], edit["request_id"], edit["program_run_id"])
            for edit in register.edits(_PROJECT)[-3:]
        ] == [
            ("reset", key, "reset.keys", None if idle else "program-cycle.1")
            for key in ("counted", "layer", "limit")
        ]
        # The next run restores nothing: every key starts from `initial:`.
        assert register.restore(_PROJECT, _declarations()) == {}
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("idle", [False, True])
def test_reset_without_reload_keeps_persistent_keys(
    tmp_path, monkeypatch: pytest.MonkeyPatch, idle: bool
) -> None:
    events: list[str] = []
    runtime, journal, register, request = _reset_runtime(
        tmp_path, monkeypatch, idle=idle, reload_scene=False, events=events
    )
    edits = register.edits(_PROJECT)
    try:
        runtime.reset_cycle(request)
        assert journal.read().state == "idle_after_reset"
        assert _values(register) == {"counted": 7, "layer": 3, "limit": 0.7}
        assert register.edits(_PROJECT) == edits
        assert events == []
    finally:
        runtime.shutdown()


@pytest.mark.parametrize("idle", [False, True])
def test_a_key_reset_that_cannot_be_stored_refuses_the_reset_before_the_scene(
    tmp_path, monkeypatch: pytest.MonkeyPatch, idle: bool
) -> None:
    events: list[str] = []
    runtime, journal, register, request = _reset_runtime(
        tmp_path, monkeypatch, idle=idle, reload_scene=True, events=events
    )
    reset_to_initial = register.reset_to_initial

    def refuse(*_args: Any, **_kwargs: Any) -> dict[str, Any]:
        raise PersistentKeyRegisterError("read-only file system")

    monkeypatch.setattr(register, "reset_to_initial", refuse)
    try:
        with pytest.raises(ProgramRunnerError) as refused:
            runtime.reset_cycle(request)
        assert refused.value.code == "program_cycle_recovery_required"
        assert refused.value.details == {
            "cause_code": "program_cycle_reset_persistent_keys_failed"
        }
        failed = journal.read()
        assert failed.state == "recovery_required"
        assert failed.reset_phase == "restoring_scene"
        assert failed.failure["code"] == "program_cycle_reset_persistent_keys_failed"
        assert failed.failure["failed_phase"] == "restoring_scene"
        # Nothing half-applied: values kept, Scene untouched.
        assert _values(register) == {"counted": 7, "layer": 3, "limit": 0.7}
        assert events == []

        # Reset again: the same phase resumes, keys first, then the Scene.
        monkeypatch.setattr(register, "reset_to_initial", reset_to_initial)
        runtime.reset_cycle(request.model_copy(update={"request_id": "reset.keys.again"}))
        assert journal.read().state == "idle_after_reset"
        assert _values(register) == {}
        assert events == ["scene:[]"]
    finally:
        runtime.shutdown()


def test_the_refusal_names_the_storage_cause(tmp_path, monkeypatch: pytest.MonkeyPatch) -> None:
    events: list[str] = []
    runtime, _journal, register, request = _reset_runtime(
        tmp_path, monkeypatch, idle=True, reload_scene=True, events=events
    )

    def refuse(*_args: Any, **_kwargs: Any) -> dict[str, Any]:
        raise PersistentKeyRegisterError("read-only file system")

    monkeypatch.setattr(register, "reset_to_initial", refuse)
    try:
        with pytest.raises(ProgramRunnerError) as refused:
            runtime._reset_persistent_keys_to_initial(request)  # noqa: SLF001
        assert refused.value.code == "program_cycle_reset_persistent_keys_failed"
        assert refused.value.details == {"cause_code": "persistent_key_storage_unavailable"}
    finally:
        runtime.shutdown()
