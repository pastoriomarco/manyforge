"""Stored values follow Manager's committed Scenario Applies; none ever revives."""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from typing import Any

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_values import ProgramValueStore
from manyforge_behavior.program import Program
from manyforge_headless_api.models import ProgramRunStartRequest
import manyforge_program_runner.persistent_keys as persistent_keys
from manyforge_program_runner.cycle_journal import ProgramCycleJournal
from manyforge_program_runner.runtime import (
    ProgramRunnerConfig,
    ProgramRunnerError,
    ProgramRuntime,
)
from manyforge_program_runner.tests.test_persistent_values import (
    _PROJECT,
    _SCENARIO,
    _Headless as _ValuesHeadless,
    _key,
    _program,
    _request_key,
    _rows,
)
from manyforge_program_runner.tests.test_runtime import (
    _A_TEXT,
    _DIGEST_A,
    _Admission,
    _Host,
    _ObservationOwner,
    _PreparedRunAdmission,
    _bypass_scene_preparation,
)


_PALLET = _program(_key("pallet_index", initial=0), _key("layer", initial=0))
_WITHOUT_PALLET = _program(_key("layer", initial=0))


class _Headless(_ValuesHeadless):
    """Committed Applies whose current one is also the Applied Program."""

    def program_applied(self) -> dict[str, Any]:
        applied = super().program_applied()
        assert not isinstance(self.current, Exception)
        applied["source"] = {**applied["source"], "record_digest": self.current["record_digest"]}
        applied["project_path"] = self.current["origin"]["project_path"]
        return applied


class _StoreHost(_Host):
    """Builds the run's value store exactly as a run does at start."""

    def __init__(self, *args: Any, headless: _Headless, register: Any, **kwargs: Any):
        super().__init__(*args, **kwargs)
        self.headless = headless
        self.register = register
        self.stores: list[ProgramValueStore] = []

    def start_run(self, admission: object, **kwargs: Any):
        current = self.headless.current
        assert not isinstance(current, Exception)
        program_run_id = kwargs["program_run_id"]
        self.stores.append(
            ProgramValueStore.from_program(
                Program.from_dict(
                    deepcopy(current["program"]), build_node_environment().catalog
                ),
                project_path=_PROJECT,
                register=self.register,
                program_run_id=program_run_id,
            )
        )
        return super().start_run(admission, **kwargs)


def _runtime(
    tmp_path: Path, headless: _Headless, name: str
) -> tuple[ProgramRuntime, _StoreHost]:
    """One Program-runner process; the register file outlives it."""

    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            application_start_id=0x3F9A00000000002A,
            persistent_key_register_directory=tmp_path,
        ),
        headless_client=headless,  # type: ignore[arg-type]
        execution_transport=headless,  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(tmp_path / f"{name}.sqlite3"),
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _ObservationOwner()
    host = _StoreHost(
        owner,
        owner_id=runtime.owner_id,
        headless=headless,
        register=runtime._persistent_keys,  # noqa: SLF001
    )
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001
    runtime.start()
    return runtime, host


def _start(runtime: ProgramRuntime, headless: _Headless) -> dict[str, Any]:
    assert not isinstance(headless.current, Exception)
    return runtime.start_run(
        ProgramRunStartRequest.model_validate(
            {
                "request_key": _request_key("request.start"),
                "source_label": "operator_ui",
                "scenario_id": _SCENARIO,
                "source_document_digest": _DIGEST_A,
                "expected_applied_record_digest": headless.current["record_digest"],
                "draft_program_revision": _DIGEST_A,
            }
        )
    )


def _audit(runtime: ProgramRuntime, run: dict[str, Any]) -> list[dict[str, Any]]:
    controls = runtime._runtime_controls[run["capture"]["program_run_id"]]  # noqa: SLF001
    return [
        {key: value for key, value in event.items() if key != "event_revision"}
        for event in controls.snapshot()["audit"]
    ]


@pytest.fixture
def _no_scene(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda *_args, **_kwargs: _PreparedRunAdmission(),
    )
    _bypass_scene_preparation(monkeypatch)


def _store_pallet_nine(tmp_path: Path) -> tuple[_Headless, ProgramRuntime]:
    """pallet_index is declared and holds 9; the runner saw that Apply."""

    headless = _Headless()
    headless.apply(_PALLET)
    runtime, host = _runtime(tmp_path, headless, "first")
    _start(runtime, headless)
    host.stores[0].write("pallet_index", 9)
    host.stores[0].write("layer", 4)
    runtime.shutdown()
    return headless, runtime


def _pallet_after_rerun(tmp_path: Path, headless: _Headless, name: str) -> Any:
    runtime, host = _runtime(tmp_path, headless, name)
    try:
        run = _start(runtime, headless)
        return host.stores[0].read("pallet_index"), runtime, run
    finally:
        runtime.shutdown()


@pytest.mark.usefixtures("_no_scene")
def test_a_missed_delete_apply_keeps_the_value_and_reports_the_fault(
    tmp_path: Path,
) -> None:
    headless, _first = _store_pallet_nine(tmp_path)
    missed = headless.apply(_WITHOUT_PALLET)  # committed; nobody woke the runner
    headless.apply(_PALLET)  # pallet_index re-added with initial: 0

    pallet, runtime, run = _pallet_after_rerun(tmp_path, headless, "second")

    # Preserved, never dropped for what cannot be proven ...
    assert pallet == 9
    # ... and the missed Apply is a named fault in the run and on the page.
    assert {
        "kind": "persistent_key_apply_not_observed",
        "project_path": _PROJECT,
        "code": "applied_program_not_observed",
        "record_digest": missed,
    } in _audit(runtime, run)
    page, _host = _runtime(tmp_path, headless, "page")
    try:
        assert page.read_persistent_values()["apply_observation_fault"] == {
            "code": "applied_program_not_observed",
            "record_digests": [missed],
        }
    finally:
        page.shutdown()


@pytest.mark.usefixtures("_no_scene")
def test_values_survive_a_workbench_restart_when_the_program_still_declares_them(
    tmp_path: Path,
) -> None:
    _headless, _first = _store_pallet_nine(tmp_path)
    # stop/start: Manager starts a new history and applies the saved Scenario.
    restarted = _Headless(history="manager-state.2")
    restarted.apply(_WITHOUT_PALLET)

    runtime, host = _runtime(tmp_path, restarted, "restarted")
    try:
        reconciled = runtime.reconcile_persistent_values()
        assert reconciled["history_changed"] is True
        assert reconciled["dropped"] == [
            {
                "project_path": _PROJECT,
                "key": "pallet_index",
                "reason": "key_not_declared_persistent",
            }
        ]
        assert reconciled["unobserved_applies"] == [
            # The new history's first Apply was the startup one, never seen.
            {
                "record_digest": restarted.applies[0]["record_digest"],
                "project_path": _PROJECT,
            }
        ]
        _start(runtime, restarted)
        # The value the saved Program still declares survived stop/start.
        assert host.stores[0].read("layer") == 4
        assert _stored_values(tmp_path, runtime) == {"layer": 4}
    finally:
        runtime.shutdown()


@pytest.mark.usefixtures("_no_scene")
def test_a_notified_delete_apply_drops_only_the_removed_key(tmp_path: Path) -> None:
    headless, _first = _store_pallet_nine(tmp_path)
    headless.apply(_WITHOUT_PALLET)
    notified, _host = _runtime(tmp_path, headless, "notified")
    assert notified.reconcile_persistent_values()["dropped"] == [
        {
            "project_path": _PROJECT,
            "key": "pallet_index",
            "reason": "key_not_declared_persistent",
        }
    ]
    # Delivered twice, or late after a newer Apply: nothing more happens.
    assert notified.reconcile_persistent_values()["outcome"] == "duplicate"
    notified.shutdown()
    headless.apply(_PALLET)

    pallet, runtime, run = _pallet_after_rerun(tmp_path, headless, "second")
    assert pallet == 0
    assert runtime._persistent_keys.values(_PROJECT)["layer"]["value"] == 4  # noqa: SLF001
    assert not [
        event for event in _audit(runtime, run)
        if event["kind"] == "persistent_key_value_dropped"
    ]


@pytest.mark.usefixtures("_no_scene")
def test_a_late_notification_never_cleans_against_outdated_declarations(
    tmp_path: Path,
) -> None:
    headless, _first = _store_pallet_nine(tmp_path)
    late = deepcopy(headless.current)
    headless.apply(
        _program(_key("pallet_index", initial=0), _key("layer", initial=0), _key("extra"))
    )
    runtime, host = _runtime(tmp_path, headless, "second")
    _start(runtime, headless)
    host.stores[0].write("extra", 1)
    runtime.shutdown()

    # A late notification whose read still returns the older activation
    # (narrower: no `extra`) must not clean up against it.
    newer = headless.current
    headless.current = late
    stale, _host = _runtime(tmp_path, headless, "stale")
    assert stale.reconcile_persistent_values()["outcome"] == "stale"
    stale.shutdown()
    headless.current = newer
    assert _stored_values(tmp_path) == {"extra": 1, "layer": 4, "pallet_index": 9}


def _stored_values(tmp_path: Path, live: ProgramRuntime | None = None) -> dict[str, Any]:
    """What a next start reads: the file, once ``live``'s writer has written it."""

    if live is not None:
        assert live._persistent_keys.wait_written(5.0)  # noqa: SLF001
    register = persistent_keys.PersistentKeyRegister(tmp_path, application_start_id=_A_TEXT)
    return {key: row["value"] for key, row in register.values(_PROJECT).items()}


@pytest.mark.usefixtures("_no_scene")
def test_a_program_without_persistent_keys_drops_its_scenarios_rows_at_run_start(
    tmp_path: Path,
) -> None:
    headless, _first = _store_pallet_nine(tmp_path)
    headless.apply(_program(_key("scratch", persistence="run")))
    runtime, _host = _runtime(tmp_path, headless, "zero")
    try:
        run = _start(runtime, headless)
        assert _stored_values(tmp_path, runtime) == {}
        assert {
            (event["key"], event["reason"])
            for event in _audit(runtime, run)
            if event["kind"] == "persistent_key_value_dropped"
        } == {
            ("layer", "key_not_declared_persistent"),
            ("pallet_index", "key_not_declared_persistent"),
        }
    finally:
        runtime.shutdown()


def _with_scene(document: dict[str, Any], *resource_ids: str) -> dict[str, Any]:
    """The same Program declaring other Scene resources."""

    changed = deepcopy(document)
    if changed.get("scene"):
        template = changed["scene"][0]
    else:
        example = Program(
            Path(__file__).resolve().parents[2]
            / "examples/pick_and_place_minimal.program.yaml",
            build_node_environment().catalog,
        )
        template = example.to_dict()["scene"][0]
    changed["scene"] = [
        {**deepcopy(template), "scene_resource_id": resource_id}
        for resource_id in resource_ids
    ]
    changed["program_revision"] = compute_content_identity(
        changed,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v3",
    )
    return changed


@pytest.mark.usefixtures("_no_scene")
def test_a_restored_value_a_new_rule_refuses_is_dropped_without_blocking_the_run(
    tmp_path: Path,
) -> None:
    keys = (_key("target", "scene_resource"), _key("layer", initial=0))
    headless = _Headless()
    headless.apply(_with_scene(_program(*keys), "table"))
    runtime, host = _runtime(tmp_path, headless, "first")
    _start(runtime, headless)
    host.stores[0].write("target", "table")
    host.stores[0].write("layer", 4)
    runtime.shutdown()
    # The stored structure is unchanged (still a scene_resource), but the
    # newly applied Program no longer declares that resource, as an added or
    # narrowed `allowed_frames` refuses a stored pose.
    headless.apply(_with_scene(_program(*keys), "bench"))

    runtime, host = _runtime(tmp_path, headless, "second")
    try:
        run = _start(runtime, headless)
        assert not host.stores[0].contains("target")
        assert host.stores[0].read("layer") == 4
        (dropped,) = [
            event
            for event in _audit(runtime, run)
            if event["kind"] == "persistent_key_value_dropped"
        ]
        assert (dropped["key"], dropped["reason"]) == ("target", "value_invalid")
        assert "scene resource" in dropped["detail"]
        assert _stored_values(tmp_path, runtime) == {"layer": 4}
        (edit,) = [
            edit
            for edit in runtime._persistent_keys.edits(_PROJECT)  # noqa: SLF001
            if edit["action"] == "dropped"
        ]
        assert edit["program_run_id"] == run["capture"]["program_run_id"]
    finally:
        runtime.shutdown()


@pytest.mark.usefixtures("_no_scene")
def test_a_cleanup_whose_file_write_fails_is_made_and_written_later(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    headless, _first = _store_pallet_nine(tmp_path)
    headless.apply(_WITHOUT_PALLET)
    runtime, host = _runtime(tmp_path, headless, "fault")
    register = runtime._persistent_keys  # noqa: SLF001
    replace = persistent_keys.os.replace
    monkeypatch.setattr(
        persistent_keys.os,
        "replace",
        lambda *_args: (_ for _ in ()).throw(OSError("read-only file system")),
    )
    try:
        # Memory is the authority: the notification follows the Apply and
        # the disk refuses nothing.
        assert runtime.reconcile_persistent_values()["dropped"] == [
            {
                "project_path": _PROJECT,
                "key": "pallet_index",
                "reason": "key_not_declared_persistent",
            }
        ]
        listed = runtime.read_persistent_values()
        assert _rows(listed)["layer"]["value"] == 4
        _start(runtime, headless)
        assert host.stores[0].read("layer") == 4
        assert register.wait_written(5.0)
        assert register.write_failures >= 1
    finally:
        monkeypatch.setattr(persistent_keys.os, "replace", replace)
        runtime.shutdown()
    # The shutdown wrote the latest state.
    assert _stored_values(tmp_path) == {"layer": 4}


def test_another_register_version_refuses_the_runner_by_name(tmp_path: Path) -> None:
    path = tmp_path / "persistent-keys.v4.json"
    path.write_text('{"schema_version": "manyforge.persistent_key_register.v1"}')
    with pytest.raises(ProgramRunnerError) as refused:
        ProgramRuntime(
            ProgramRunnerConfig(
                headless_url="http://headless.test",
                application_start_id=0x3F9A00000000002A,
                persistent_key_register_directory=tmp_path,
            ),
            headless_client=_Headless(),  # type: ignore[arg-type]
            execution_transport=_Headless(),  # type: ignore[arg-type]
            admission_client=_Admission(),  # type: ignore[arg-type]
        )
    assert refused.value.code == "persistent_key_register_version_unsupported"
    assert path.read_text() == '{"schema_version": "manyforge.persistent_key_register.v1"}'
