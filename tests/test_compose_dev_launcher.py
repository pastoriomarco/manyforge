from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = REPOSITORY_ROOT / "scripts" / "compose-dev.sh"


def _fake_docker_environment(tmp_path: Path, **settings: str) -> dict[str, str]:
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    docker = fake_bin / "docker"
    docker.write_text(
        """#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$*" >> "$FAKE_DOCKER_CALLS"
command=${1:-}
shift || true
case "$command" in
  inspect)
    [[ ${FAKE_CONTAINER_PRESENT:-true} == true ]] || exit 1
    if [[ ${1:-} == -f ]]; then
      if [[ -e "$FAKE_DOCKER_STOPPED" && ${FAKE_STILL_RUNNING:-false} != true ]]; then
        printf 'false\\n'
      else
        printf 'true\\n'
      fi
    elif [[ ${1:-} == --format ]]; then
      printf '{"image":"sha256:test","mounts":[],"state":{"status":"exited","running":false,"exit_code":0,"finished_at":"time"}}\\n'
    else
      printf '[{"State":{"Status":"exited","Running":false}}]\\n'
    fi
    ;;
  stop)
    [[ ${FAKE_STOP_FAIL:-false} != true ]] || exit 1
    : > "$FAKE_DOCKER_STOPPED"
    ;;
  cp)
    [[ ${FAKE_COPY_FAIL:-false} != true ]] || exit 1
    /bin/tar --create --file - --directory "$FAKE_CONTAINER_ROOT" manyforge-state
    ;;
  logs)
    [[ ${FAKE_LOGS_FAIL:-false} != true ]] || exit 1
    printf 'preserved log\\n'
    ;;
  rm)
    [[ ${FAKE_REMOVE_FAIL:-false} != true ]] || exit 1
    : > "$FAKE_DOCKER_REMOVED"
    ;;
  *)
    printf 'unexpected fake docker command: %s\\n' "$command" >&2
    exit 98
    ;;
esac
""",
        encoding="utf-8",
    )
    docker.chmod(0o755)

    container_root = tmp_path / "container"
    state = container_root / "manyforge-state"
    (state / "installed-targets").mkdir(parents=True)
    (state / "manager").mkdir()
    (state / "manager" / "journal.sqlite3").write_text(
        "manager evidence", encoding="utf-8"
    )
    (state / "installed-targets" / "reproducible-binary").write_text(
        "large cache", encoding="utf-8"
    )
    for reproducible in ("raw-artifacts", "prepared-activations"):
        (state / "artifacts" / reproducible).mkdir(parents=True)
        (state / "artifacts" / reproducible / "large.blob").write_text(
            "reproducible input", encoding="utf-8"
        )
    planning = state / "execution-run" / "manyforge-retained-planning-0123"
    planning.mkdir(parents=True)
    (planning / "robot.stl").write_text("mesh", encoding="utf-8")
    (state / "execution-run" / "resolved-stop-base-plans").mkdir()
    (state / "execution-run" / "resolved-stop-base-plans" / "plan.json").write_text(
        "stop evidence", encoding="utf-8"
    )
    (state / "artifacts" / "programs").mkdir()
    (state / "artifacts" / "programs" / "program.json").write_text(
        "program evidence", encoding="utf-8"
    )

    environment = {
        **os.environ,
        "PATH": f"{fake_bin}:{os.environ['PATH']}",
        "MANYFORGE_DEV_CACHE_BASE": str(tmp_path / "cache"),
        "FAKE_CONTAINER_ROOT": str(container_root),
        "FAKE_DOCKER_CALLS": str(tmp_path / "docker-calls"),
        "FAKE_DOCKER_STOPPED": str(tmp_path / "docker-stopped"),
        "FAKE_DOCKER_REMOVED": str(tmp_path / "docker-removed"),
        **settings,
    }
    return environment


def _run_launcher(
    tmp_path: Path, *arguments: str, **settings: str
) -> tuple[subprocess.CompletedProcess[str], dict[str, str]]:
    environment = _fake_docker_environment(tmp_path, **settings)
    completed = subprocess.run(
        ["/bin/bash", str(LAUNCHER), *arguments],
        cwd=REPOSITORY_ROOT,
        env=environment,
        capture_output=True,
        text=True,
        check=False,
    )
    return completed, environment


def test_stop_with_evidence_preserves_quiescent_state_and_removes_container(
    tmp_path: Path,
) -> None:
    completed, environment = _run_launcher(tmp_path, "stop", "--keep-evidence")

    assert completed.returncode == 0, completed.stderr
    prefix = "Evidence preserved at "
    evidence_line = next(
        line for line in completed.stdout.splitlines() if line.startswith(prefix)
    )
    evidence_path = Path(evidence_line.removeprefix(prefix))
    assert evidence_path.parent == tmp_path / "cache" / "evidence"
    assert re.fullmatch(r"\d{8}T\d{6}Z", evidence_path.name)
    assert (evidence_path / "manyforge-state/manager/journal.sqlite3").read_text(
        encoding="utf-8"
    ) == "manager evidence"
    assert not (evidence_path / "manyforge-state/installed-targets").exists()
    assert not (evidence_path / "manyforge-state/artifacts/raw-artifacts").exists()
    assert not (
        evidence_path / "manyforge-state/artifacts/prepared-activations"
    ).exists()
    assert (evidence_path / "manyforge-state/artifacts/programs/program.json").exists()
    assert not (
        evidence_path / "manyforge-state/execution-run/manyforge-retained-planning-0123"
    ).exists()
    assert (
        evidence_path / "manyforge-state/execution-run/resolved-stop-base-plans/plan.json"
    ).exists()
    assert (evidence_path / "docker.log").read_text(encoding="utf-8") == (
        "preserved log\n"
    )
    inspection = (evidence_path / "docker-inspect.json").read_text(encoding="utf-8")
    assert '"status":"exited"' in inspection
    assert '"image":"sha256:test"' in inspection
    assert '"mounts":[]' in inspection
    assert "environment" not in inspection.lower()
    assert Path(environment["FAKE_DOCKER_REMOVED"]).exists()

    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text(
        encoding="utf-8"
    ).splitlines()
    assert calls[-1].startswith("rm ")
    copy_index = next(
        index for index, call in enumerate(calls) if call.startswith("cp ")
    )
    logs_index = next(
        index for index, call in enumerate(calls) if call.startswith("logs ")
    )
    assert copy_index < logs_index


def test_stop_with_evidence_refuses_to_copy_if_container_is_still_running(
    tmp_path: Path,
) -> None:
    completed, environment = _run_launcher(
        tmp_path, "stop", "--keep-evidence", FAKE_STILL_RUNNING="true"
    )

    assert completed.returncode == 69
    assert "still running; no evidence was copied" in completed.stderr
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text(encoding="utf-8")
    assert "cp " not in calls
    assert "rm " not in calls


def test_stop_with_evidence_does_not_mask_docker_stop_failure(tmp_path: Path) -> None:
    completed, environment = _run_launcher(
        tmp_path, "stop", "--keep-evidence", FAKE_STOP_FAIL="true"
    )

    assert completed.returncode == 69
    assert "Failed to stop the development workbench" in completed.stderr
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text(encoding="utf-8")
    assert "cp " not in calls
    assert "rm " not in calls


@pytest.mark.parametrize("failure_setting", ["FAKE_COPY_FAIL", "FAKE_LOGS_FAIL"])
def test_stop_with_evidence_keeps_stopped_container_and_partial_copy_on_failure(
    tmp_path: Path, failure_setting: str
) -> None:
    completed, environment = _run_launcher(
        tmp_path, "stop", "--keep-evidence", **{failure_setting: "true"}
    )

    assert completed.returncode == 69
    assert "Partial evidence:" in completed.stderr
    assert "stopped container was retained" in completed.stderr
    assert not Path(environment["FAKE_DOCKER_REMOVED"]).exists()
    partials = list((tmp_path / "cache" / "evidence").glob("*.partial"))
    assert len(partials) == 1


def test_stop_with_evidence_reports_remove_failure_after_completed_snapshot(
    tmp_path: Path,
) -> None:
    completed, environment = _run_launcher(
        tmp_path, "stop", "--keep-evidence", FAKE_REMOVE_FAIL="true"
    )

    assert completed.returncode == 69
    assert "Evidence was preserved at " in completed.stderr
    assert "could not be removed" in completed.stderr
    assert not Path(environment["FAKE_DOCKER_REMOVED"]).exists()
    evidence_entries = list((tmp_path / "cache" / "evidence").iterdir())
    assert len(evidence_entries) == 1
    assert not evidence_entries[0].name.endswith(".partial")


def test_stop_with_evidence_fails_clearly_when_container_is_absent(
    tmp_path: Path,
) -> None:
    completed, _environment = _run_launcher(
        tmp_path,
        "stop",
        "--keep-evidence",
        FAKE_CONTAINER_PRESENT="false",
    )

    assert completed.returncode == 69
    assert "container does not exist" in completed.stderr
    assert not (tmp_path / "cache" / "evidence").exists()


def test_plain_stop_keeps_its_best_effort_behavior(tmp_path: Path) -> None:
    completed, _environment = _run_launcher(
        tmp_path,
        "stop",
        FAKE_STOP_FAIL="true",
        FAKE_REMOVE_FAIL="true",
    )

    assert completed.returncode == 0
    assert completed.stdout == (
        "Development workbench is stopped. Build and npm caches were retained.\n"
    )
