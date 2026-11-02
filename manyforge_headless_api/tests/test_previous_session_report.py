from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

from manyforge_headless_api import cli
from manyforge_headless_api.native import (
    MAX_PROGRAM_MODEL_ATTEMPT_BYTES,
    HeadlessApiConfig,
    HeadlessApiError,
    NativeHeadlessCore,
)


def _config(root: Path, reports: Path | None) -> HeadlessApiConfig:
    return HeadlessApiConfig(
        projection_root=root / "projection", install_root=root / "install",
        artifact_root=root / "artifacts", node_catalog_path=root / "nodes.json",
        node_environment_manifest_path=root / "environment.json",
        alarm_catalog_path=root / "alarms.json",
        historian_database_path=root / "history.sqlite3",
        manager_run_root=root / "manager-run", execution_run_root=root / "execution-run",
        cell_id="cell_a", workbench_previous_session_reports_root=reports,
    )


def _pending(root: Path, identity: str = "a" * 32) -> tuple[Path, Path]:
    reports = root / "session-reports"
    (reports / "pending").mkdir(parents=True, exist_ok=True)
    report = reports / "pending" / f"{identity}.json"
    report.write_text(json.dumps({"session_id": identity}), encoding="utf-8")
    return reports, report


@pytest.fixture
def native(monkeypatch: pytest.MonkeyPatch) -> SimpleNamespace:
    state = SimpleNamespace(records={}, calls=[], observed=[], fail=False, malformed_receipt=False, registration_status="registered")

    class Core:
        def __init__(self, *args: object) -> None:
            assert len(args) == 13  # No constructor vector or single-report side path.

        def append_workbench_previous_session_report(self, payload: str) -> str:
            value = json.loads(payload)
            identity = value["session_id"]
            state.calls.append(identity)
            old = state.records.get(identity)
            if state.fail or (old is not None and old != value):
                return json.dumps({"ok": False, "error": {
                    "code": "historian_storage_failure", "message": "injected append failure",
                }})
            state.records[identity] = value
            receipt = {"session_id": identity, "event_id": "event-" + identity,
                       "status": "duplicate" if old is not None else "stored"}
            return json.dumps({"ok": True, "value": {} if state.malformed_receipt else receipt})

        def observe_workbench_previous_session_report(self, payload: str) -> str:
            value = json.loads(payload)
            state.observed.append(value["session_id"])
            if state.registration_status == "query_failed":
                return json.dumps({"ok": False, "error": {
                    "code": "historian_storage_failure", "message": "injected query failure",
                }})
            assert value["session_id"] in state.records  # Replay precedes registration.
            return json.dumps({"ok": True, "value": {
                "session_id": value["session_id"], "status": state.registration_status,
            }})

    monkeypatch.setitem(sys.modules, "manyforge_headless_py", SimpleNamespace(HeadlessApiCore=Core))
    return state


def test_serve_passes_report_root_to_config(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    observed = []
    monkeypatch.setattr(cli, "create_app", lambda config: observed.append(config) or object())
    monkeypatch.setattr(cli.uvicorn, "run", lambda *_args, **_kwargs: None)
    args = ["serve", "--cell-id", "cell_a", "--workbench-previous-session-id", "a" * 32]
    for flag in (
        "projection-root", "install-root", "artifact-root", "node-catalog-path",
        "node-environment-manifest-path", "alarm-catalog-path", "historian-database-path",
        "manager-run-root", "execution-run-root", "workbench-previous-session-reports-root",
    ):
        args += [f"--{flag}", str(tmp_path / flag)]
    assert cli.main(args) == 0
    assert observed[0].workbench_previous_session_reports_root == tmp_path / "workbench-previous-session-reports-root"
    assert observed[0].workbench_previous_session_id == "a" * 32


def test_replays_every_pending_report_then_leaves_finalized_reports_unchanged(
    tmp_path: Path, native: SimpleNamespace,
) -> None:
    paths = [_pending(tmp_path, digit * 32)[1] for digit in "abcdef0123456789"]
    reports = paths[0].parent.parent
    expected = {path.name: path.read_bytes() for path in paths}
    NativeHeadlessCore(_config(tmp_path, reports))
    assert len(native.records) == len(paths)
    assert list((reports / "pending").iterdir()) == []
    assert {path.name: path.read_bytes() for path in reports.glob("*.json")} == expected
    NativeHeadlessCore(_config(tmp_path, reports))
    NativeHeadlessCore(_config(tmp_path, None))
    assert len(native.calls) == len(paths)


def test_crash_after_commit_replays_duplicate_before_marking_done(
    tmp_path: Path, native: SimpleNamespace, monkeypatch: pytest.MonkeyPatch,
) -> None:
    reports, report = _pending(tmp_path)
    from manyforge_headless_api import native as module
    real_rename = module.os.rename
    monkeypatch.setattr(module.os, "rename", lambda *_args, **_kwargs: (_ for _ in ()).throw(OSError("crash before done rename")))
    with pytest.raises(HeadlessApiError):
        NativeHeadlessCore(_config(tmp_path, reports))
    assert report.exists()
    assert len(native.records) == 1
    monkeypatch.setattr(module.os, "rename", real_rename)
    NativeHeadlessCore(_config(tmp_path, reports))
    assert not report.exists()
    assert (reports / report.name).exists()
    assert len(native.records) == 1
    assert len(native.calls) == 2


@pytest.mark.parametrize("failure", ["append", "receipt", "final_conflict"])
def test_failure_never_marks_pending_report_done(
    tmp_path: Path, native: SimpleNamespace, failure: str,
) -> None:
    reports, report = _pending(tmp_path)
    original = report.read_bytes()
    native.fail = failure == "append"
    native.malformed_receipt = failure == "receipt"
    if failure == "final_conflict":
        (reports / report.name).write_bytes(b'{"session_id":"different"}')
    with pytest.raises(HeadlessApiError):
        NativeHeadlessCore(_config(tmp_path, reports))
    assert report.read_bytes() == original
    if failure == "final_conflict":
        assert (reports / report.name).read_bytes() == b'{"session_id":"different"}'
        assert native.calls == []
    else:
        assert not (reports / report.name).exists()


@pytest.mark.parametrize("kind", ["empty", "oversize", "duplicate_key", "symlink", "directory", "wrong_identity", "unsafe_name"])
def test_invalid_pending_report_never_appends_or_finalizes(
    tmp_path: Path, native: SimpleNamespace, kind: str,
) -> None:
    reports, report = _pending(tmp_path)
    if kind == "empty":
        report.write_bytes(b"")
    elif kind == "oversize":
        report.write_bytes(b" " * (MAX_PROGRAM_MODEL_ATTEMPT_BYTES + 1))
    elif kind == "duplicate_key":
        report.write_bytes(b'{"session_id":"a","session_id":"b"}')
    elif kind == "symlink":
        report.unlink()
        target = tmp_path / "target.json"
        target.write_bytes(b"{}")
        report.symlink_to(target)
    elif kind == "directory":
        report.unlink()
        report.mkdir()
    elif kind == "wrong_identity":
        report.write_bytes(b'{"session_id":"different"}')
    elif kind == "unsafe_name":
        renamed = report.with_name("foreign.json")
        report.rename(renamed)
        report = renamed
    with pytest.raises(HeadlessApiError) as error:
        NativeHeadlessCore(_config(tmp_path, reports))
    assert error.value.code == "headless_native_initialization_failed"
    assert native.calls == []
    assert report.exists()
    assert not (reports / report.name).exists()


def test_duplicate_pending_and_final_report_is_verified_before_pending_is_removed(
    tmp_path: Path, native: SimpleNamespace,
) -> None:
    reports, report = _pending(tmp_path)
    payload = report.read_bytes()
    (reports / report.name).write_bytes(payload)
    NativeHeadlessCore(_config(tmp_path, reports))
    assert not report.exists()
    assert (reports / report.name).read_bytes() == payload
    assert len(native.calls) == 1


def test_unpublished_helper_temporary_is_preserved(
    tmp_path: Path, native: SimpleNamespace,
) -> None:
    reports, report = _pending(tmp_path)
    temporary = report.with_name("." + report.name + ".next")
    temporary.write_bytes(b"partial write")
    NativeHeadlessCore(_config(tmp_path, reports))
    assert temporary.read_bytes() == b"partial write"
    assert len(native.calls) == 1


def test_report_root_must_be_absolute(tmp_path: Path) -> None:
    with pytest.raises(ValueError, match="reports root must be absolute"):
        _config(tmp_path, Path("relative"))


@pytest.mark.parametrize("status", ["registered", "acknowledged", "quiet"])
def test_registers_only_current_report_after_durable_replay(tmp_path: Path, native: SimpleNamespace, status: str) -> None:
    reports, _ = _pending(tmp_path, "a" * 32)
    _pending(tmp_path, "b" * 32)
    native.registration_status = status
    core = NativeHeadlessCore(replace(_config(tmp_path, reports), workbench_previous_session_id="a" * 32))
    assert native.observed == ["a" * 32]
    assert core.workbench_previous_session_registration == {"session_id": "a" * 32, "status": status}
    NativeHeadlessCore(replace(_config(tmp_path, reports), workbench_previous_session_id="a" * 32))
    assert native.observed == ["a" * 32, "a" * 32]
    assert len(native.calls) == 2  # Finalized reports are not bulk-replayed.


def test_registration_without_current_identity_stays_absent(tmp_path: Path, native: SimpleNamespace) -> None:
    reports, _ = _pending(tmp_path)
    core = NativeHeadlessCore(_config(tmp_path, reports))
    assert native.observed == []
    assert core.workbench_previous_session_registration is None


@pytest.mark.parametrize("failure", ["missing", "wrong_identity", "symlink", "bad_receipt", "query_failure"])
def test_current_report_registration_never_claims_success_on_invalid_input(tmp_path: Path, native: SimpleNamespace, failure: str) -> None:
    reports, pending = _pending(tmp_path)
    NativeHeadlessCore(_config(tmp_path, reports))
    final = reports / pending.name
    if failure == "missing":
        final.unlink()
    elif failure == "wrong_identity":
        final.write_text(json.dumps({"session_id": "b" * 32}))
    elif failure == "symlink":
        target = tmp_path / "target.json"
        final.rename(target)
        final.symlink_to(target)
    else:
        native.registration_status = "query_failed" if failure == "query_failure" else "not_proven"
    with pytest.raises(HeadlessApiError):
        NativeHeadlessCore(replace(_config(tmp_path, reports), workbench_previous_session_id="a" * 32))


@pytest.mark.parametrize("identity", ["bad", "A" * 32, "../" + "a" * 32])
def test_config_refuses_invalid_current_archive_identity(tmp_path: Path, identity: str) -> None:
    with pytest.raises(ValueError):
        replace(_config(tmp_path, tmp_path / "reports"), workbench_previous_session_id=identity)


def test_config_requires_report_root_for_current_identity(tmp_path: Path) -> None:
    with pytest.raises(ValueError):
        replace(_config(tmp_path, None), workbench_previous_session_id="a" * 32)


def test_existing_health_projects_only_completed_native_registration() -> None:
    from manyforge_headless_api.service import HeadlessService
    receipt = {"session_id": "a" * 32, "status": "acknowledged"}
    core = SimpleNamespace(
        alarm_snapshot=lambda **_: {"health": {"state": "available"}, "reconciled": True},
        historian_health=lambda: {"available": True},
        workbench_previous_session_registration=receipt,
    )
    service = HeadlessService(core, SimpleNamespace())
    service.runtime = lambda: {"complete": True}
    service.node_environment = lambda: {"source": {"available": True, "freshness": "fresh", "known_gaps": []}}
    assert service.health()["workbench_previous_session_registration"] == receipt
    core.workbench_previous_session_registration = None
    assert "workbench_previous_session_registration" not in service.health()
