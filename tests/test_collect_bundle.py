"""The host-side bug-report bundle: one bounded tar.gz of one run, read-only."""

from __future__ import annotations

import importlib.util
import json
import os
from datetime import datetime, timezone
from pathlib import Path
import sqlite3
import subprocess
import sys
import tarfile

import pytest

REPOSITORY = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("collect_bundle", REPOSITORY / "scripts/lib/collect_bundle.py")
assert spec and spec.loader
collect_bundle = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = collect_bundle
spec.loader.exec_module(collect_bundle)

sys.path.insert(0, str(REPOSITORY / "manyforge_headless_api/tests"))
from test_history_export import history_file  # noqa: E402

OLD_SESSION, NEW_SESSION = "a" * 32, "b" * 32
OLD_START, NEW_START = "1" * 16, "2" * 16
OLD_CONTAINER, NEW_CONTAINER = "c" * 64, "d" * 64
OLD_WALL, NEW_WALL = 1_790_000_000, 1_790_000_600


def _marker(session: str, wall: int, container: str) -> dict:
    return {"schema_version": "manyforge.workbench_session.v3", "session_id": session,
            "started_at_wall": datetime.fromtimestamp(wall, timezone.utc).isoformat(),
            "disposition": "stopped", "shutdown": "clean", "effects": "closed", "launcher_claimed": True,
            "previous_session": None, "process_boundary": {"kind": "docker", "container_id": container},
            "application_end": None, "manager_exit": {"exit_status": 0, "signal": None}}


def _journal(folder: Path, start: str) -> None:
    path = folder / "execution-state/operation-journal.sqlite3"
    path.parent.mkdir(parents=True)
    database = sqlite3.connect(path)
    database.execute("PRAGMA journal_mode=WAL")
    database.execute("CREATE TABLE journal_metadata (singleton INTEGER PRIMARY KEY, application_start_id TEXT)")
    database.execute("INSERT INTO journal_metadata VALUES(1, ?)", (start,))
    database.commit()
    database.close()


def state_root(tmp_path: Path) -> Path:
    root = tmp_path / "runtime/workbench"
    old = root / "previous-runs" / OLD_SESSION
    old.mkdir(parents=True)
    (old / "session.json").write_text(json.dumps(_marker(OLD_SESSION, OLD_WALL, OLD_CONTAINER)))
    _journal(old, OLD_START)
    (old / "execution-run/ros").mkdir(parents=True)
    (old / "execution-run/ros/host.log").write_bytes(b"ros log\n")
    (old / "execution-run/assistant-audit.jsonl").write_text('{"audit":1}\n')
    reports = root / "session-reports"
    (reports / "pending").mkdir(parents=True)
    (reports / (OLD_SESSION + ".json")).write_text(json.dumps(
        {"session_id": OLD_SESSION, "application_start_id": OLD_START}))
    current = root / "current"
    current.mkdir()
    (current / "session.json").write_text(json.dumps(_marker(NEW_SESSION, NEW_WALL, NEW_CONTAINER)))
    # The current run's start is known only from its own journal.
    _journal(current, NEW_START)
    (current / "workbench-startup-timing.json").write_text("{}")
    for name, container in (("20261124T000000Z.000000001", OLD_CONTAINER), ("20261124T000001Z", "e" * 64)):
        folder = root / "evidence" / name
        folder.mkdir(parents=True)
        (folder / "docker-inspect.json").write_text(json.dumps({"container_id": container}))
        (folder / "docker.log").write_bytes(b"2026-11-24T00:00:00.000000000Z console line\n")
    (root / "history").mkdir()
    history_file(root / "history", [(OLD_START, 1, 10), (NEW_START, 1, 20), (OLD_START, 1, 30)])
    traces = root / "motion-traces"
    traces.mkdir()
    for index, wall in enumerate((OLD_WALL + 10, NEW_WALL + 10)):
        trace = traces / f"trace-{index:064x}.json"
        trace.write_text("{}")
        os.utime(trace, (wall, wall))
    # The archive is sealed read-only, as the launcher leaves it.
    for directory, _, names in os.walk(old, topdown=False):
        for name in names:
            os.chmod(Path(directory) / name, 0o444)
        os.chmod(directory, 0o555)
    return root


def _tree(root: Path) -> dict[str, tuple[int, int]]:
    return {str(path.relative_to(root)): (path.stat().st_size, path.stat().st_mtime_ns)
            for path in sorted(root.rglob("*"))}


def _members(bundle: Path) -> dict[str, bytes]:
    with tarfile.open(bundle) as tar:
        names = tar.getnames()
        prefix = names[0].split("/")[0] + "/"
        assert all(name.startswith(prefix) for name in names)
        return {name[len(prefix):]: tar.extractfile(name).read() for name in names}


def test_last_bundles_the_current_run_without_touching_the_state_root(tmp_path: Path) -> None:
    root = state_root(tmp_path)
    before = _tree(root)
    out = tmp_path / "bundle.tar.gz"
    assert collect_bundle.main([str(root), "--last", "--out", str(out)]) == 0
    assert _tree(root) == before
    members = _members(out)
    manifest = json.loads(members["manifest.json"])
    assert (manifest["run_folder"], manifest["session_id"], manifest["application_start_id"]) == (
        "current", NEW_SESSION, NEW_START)
    assert set(members) == {
        "manifest.json", "run/session.json", "run/workbench-startup-timing.json",
        "run/execution-state/operation-journal.sqlite3", "history.ndjson",
        "motion-traces/trace-" + f"{1:064x}" + ".json"}
    assert [json.loads(line)["sequence"] for line in members["history.ndjson"].decode().splitlines()] == [2]
    # The live database is copied through SQLite's backup.
    copy = tmp_path / "copy.sqlite3"
    copy.write_bytes(members["run/execution-state/operation-journal.sqlite3"])
    assert sqlite3.connect(copy).execute("SELECT application_start_id FROM journal_metadata").fetchone() == (NEW_START,)
    assert manifest["history"] == {"file": "history/history.v9.sqlite3", "rows": 1, "stopped_at": None}


def test_start_bundles_an_archived_run_with_its_report_and_evidence(tmp_path: Path) -> None:
    root = state_root(tmp_path)
    out = tmp_path / "bundle.tar.gz"
    assert collect_bundle.main([str(root), "--start", OLD_START, "--out", str(out)]) == 0
    members = _members(out)
    manifest = json.loads(members["manifest.json"])
    assert manifest["run_folder"] == "previous-runs/" + OLD_SESSION
    assert set(members) == {
        "manifest.json", "run/session.json", "session-reports/" + OLD_SESSION + ".json",
        "evidence/20261124T000000Z.000000001/docker-inspect.json", "evidence/20261124T000000Z.000000001/docker.log",
        "history.ndjson", "run/execution-state/operation-journal.sqlite3",
        "run/execution-run/assistant-audit.jsonl", "run/execution-run/ros/host.log",
        "motion-traces/trace-" + f"{0:064x}" + ".json"}
    assert [json.loads(line)["sequence"] for line in members["history.ndjson"].decode().splitlines()] == [1, 3]
    # Priority order: identity, report and evidence first, then History.
    order = [row["path"] for row in manifest["included"]]
    assert order.index("run/session.json") < order.index("evidence/20261124T000000Z.000000001/docker.log") \
        < order.index("history.ndjson") < order.index("run/execution-state/operation-journal.sqlite3") \
        < order.index("run/execution-run/assistant-audit.jsonl") < order.index("run/execution-run/ros/host.log")


def test_max_bytes_skips_what_does_not_fit_and_lists_it(tmp_path: Path) -> None:
    root = state_root(tmp_path)
    out = tmp_path / "bundle.tar.gz"
    assert collect_bundle.main([str(root), "--start", OLD_START, "--out", str(out), "--max-bytes", "600"]) == 0
    members = _members(out)
    manifest = json.loads(members["manifest.json"])
    included = sum(row["bytes"] for row in manifest["included"])
    assert included <= 600
    assert sum(len(data) for name, data in members.items() if name != "manifest.json") == included
    skipped = {row["path"] for row in manifest["skipped"]}
    assert "run/execution-state/operation-journal.sqlite3" in skipped
    assert all(row["reason"].startswith("max_bytes") for row in manifest["skipped"])
    assert "run/session.json" in members


def test_refusals(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = state_root(tmp_path)
    assert collect_bundle.main([str(root), "--out", str(root / "bundle.tar.gz")]) == 66
    assert "outside the runtime state root" in capsys.readouterr().err
    assert collect_bundle.main([str(root), "--start", "3" * 16, "--out", str(tmp_path / "b.tar.gz")]) == 66
    assert f"known: {OLD_START}, {NEW_START}" in capsys.readouterr().err
    assert not (tmp_path / "b.tar.gz").exists()
    (tmp_path / "taken.tar.gz").write_bytes(b"keep")
    assert collect_bundle.main([str(root), "--out", str(tmp_path / "taken.tar.gz")]) == 73
    assert (tmp_path / "taken.tar.gz").read_bytes() == b"keep"
    assert collect_bundle.main([str(tmp_path / "absent"), "--out", str(tmp_path / "c.tar.gz")]) == 66


@pytest.mark.parametrize("command,variable", [
    (["bash", str(REPOSITORY / "scripts/compose-dev.sh")], "MANYFORGE_DEV_RUNTIME_STATE_ROOT"),
    (["bash", str(REPOSITORY / "manyforge")], "MANYFORGE_RUNTIME_STATE_ROOT"),
])
def test_launchers_collect_from_their_runtime_state_root(tmp_path: Path, command: list[str], variable: str) -> None:
    root = state_root(tmp_path)
    out = tmp_path / "bundle.tar.gz"
    environment = {key: value for key, value in os.environ.items() if not key.startswith("MANYFORGE_")}
    completed = subprocess.run(
        [*command, "collect", "--start", OLD_START, "--out", str(out)], cwd=tmp_path,
        env={**environment, variable: str(root)}, capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    assert json.loads(_members(out)["manifest.json"])["application_start_id"] == OLD_START


def _listed() -> list[str]:
    wall = lambda seconds: datetime.fromtimestamp(seconds, timezone.utc).isoformat()  # noqa: E731
    return [f"session={OLD_SESSION} start={OLD_START} started_at_wall={wall(OLD_WALL)} shutdown=clean "
            f"container={OLD_CONTAINER} evidence=evidence/20261124T000000Z.000000001",
            f"session={NEW_SESSION} start={NEW_START} started_at_wall={wall(NEW_WALL)} shutdown=clean "
            f"container={NEW_CONTAINER} evidence=-"]


def test_list_prints_one_line_per_run_oldest_first_and_writes_nothing(
        tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = state_root(tmp_path)
    before = _tree(root)
    assert collect_bundle.main([str(root), "--list"]) == 0
    assert capsys.readouterr().out.splitlines() == _listed()
    assert _tree(root) == before and not list(tmp_path.glob("*.tar.gz"))
    # Unknown values print as "-".
    (root / "current/session.json").write_text("{}")
    assert collect_bundle.main([str(root), "--list"]) == 0
    assert capsys.readouterr().out.splitlines()[-1] == (
        f"session=- start={NEW_START} started_at_wall=- shutdown=- container=- evidence=-")


def test_list_refusals(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = state_root(tmp_path)
    for extra in (["--out", str(tmp_path / "b.tar.gz")], ["--max-bytes", "10"], ["--start", OLD_START]):
        with pytest.raises(SystemExit) as refused:
            collect_bundle.main([str(root), "--list", *extra])
        assert refused.value.code == 2
    assert collect_bundle.main([str(tmp_path / "absent"), "--list"]) == 66
    assert "no runtime state root" in capsys.readouterr().err


@pytest.mark.parametrize("command,variable", [
    (["bash", str(REPOSITORY / "scripts/compose-dev.sh")], "MANYFORGE_DEV_RUNTIME_STATE_ROOT"),
    (["bash", str(REPOSITORY / "manyforge")], "MANYFORGE_RUNTIME_STATE_ROOT"),
])
def test_launchers_list_the_runs_of_their_runtime_state_root(tmp_path: Path, command: list[str], variable: str) -> None:
    root = state_root(tmp_path)
    environment = {key: value for key, value in os.environ.items() if not key.startswith("MANYFORGE_")}
    completed = subprocess.run(
        [*command, "collect", "--list"], cwd=tmp_path,
        env={**environment, variable: str(root)}, capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    assert completed.stdout.splitlines() == _listed()
