from __future__ import annotations

import json
import os
import tempfile
import re
import subprocess
import sqlite3
from pathlib import Path

import pytest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = REPOSITORY_ROOT / "scripts" / "compose-dev.sh"


def _copy_retention_helper(launcher: Path) -> None:
    target = launcher.parent / "lib/export_retention.py"
    target.parent.mkdir(exist_ok=True)
    target.write_text((REPOSITORY_ROOT / "scripts/lib/export_retention.py").read_text())
    (target.parent / "workbench_session.py").write_text(
        (REPOSITORY_ROOT / "scripts/lib/workbench_session.py").read_text()
    )


def _copy_writable_launcher(tmp_path: Path) -> Path:
    launcher = tmp_path / "workspace/scripts" / LAUNCHER.name
    launcher.parent.mkdir(parents=True)
    launcher.write_text(LAUNCHER.read_text(encoding="utf-8"), encoding="utf-8")
    _copy_retention_helper(launcher)
    return launcher


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
reference=${@: -1}
if [[ -n ${FAKE_OLDER_ID:-} && "$reference" == "$FAKE_OLDER_ID" ]]; then
  export FAKE_ACTUAL_ID=$FAKE_OLDER_ID
  export FAKE_ACTUAL_STATE_ROOT=${FAKE_OLDER_STATE_ROOT:-}
  export FAKE_LOGS_FAIL=${FAKE_OLDER_LOGS_FAIL:-false}
fi
case "$command" in
  inspect)
    [[ ${FAKE_INSPECT_FAIL:-false} != true ]] || exit 1
    [[ ${FAKE_CONTAINER_PRESENT:-true} == true || -e ${FAKE_DOCKER_STARTED:-/nonexistent} || ${1:-} == other-owned-container ]] || exit 1
    if [[ ${1:-} == --format && ${2:-} == '{{json .}}' ]]; then
      python3 -c 'import json,os;print(json.dumps({"Id":os.environ.get("FAKE_ACTUAL_ID","d"*64),"Image":os.environ["MANYFORGE_DEV_IMAGE"],"Config":{"Labels":{"manyforge.launch-instance":os.environ.get("FAKE_ACTUAL_INSTANCE",os.environ["MANYFORGE_DEV_LAUNCH_INSTANCE_ID"]),"manyforge.launch-input-digest":os.environ["MANYFORGE_DEV_LAUNCH_INPUT_DIGEST"]}},"State":{"Running":os.environ.get("FAKE_EXIT_AFTER_RUN")!="true" and os.environ.get("FAKE_INITIAL_EXITED")!="true" and not os.path.exists(os.environ["FAKE_DOCKER_STOPPED"])}}))'
    elif [[ ${1:-} == -f && ${2:-} == *Config.Labels* ]]; then
      printf '%s\\n' "${FAKE_ROS_LABEL:-true}"
    elif [[ ${1:-} == -f ]]; then
      if [[ ( -e "$FAKE_DOCKER_STOPPED" && ${FAKE_STILL_RUNNING:-false} != true ) || ${FAKE_INITIAL_EXITED:-false} == true || ${FAKE_EXIT_AFTER_RUN:-false} == true ]]; then
        printf 'false\\n'
      else
        printf 'true\\n'
      fi
    elif [[ ${1:-} == --format && ${2:-} == '{{.Id}}' ]]; then
      printf '%s\\n' "${FAKE_ACTUAL_ID:-dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd}"
    elif [[ ${1:-} == --format && ${2:-} == '{{json .Mounts}}' ]]; then
      python3 -c 'import json,os; root=os.environ.get("FAKE_ACTUAL_STATE_ROOT"); print(json.dumps(([{"Type":"bind","Source":root,"Destination":"/tmp/manyforge-state"}] if root else [])+[{"Type":"bind","Source":os.environ["FAKE_WORKSPACE"],"Destination":"/workspace"}]))'
    elif [[ ${1:-} == --format ]]; then
      python3 -c 'import json,os,sys;root=os.environ.get("FAKE_ACTUAL_STATE_ROOT");mounts=([{"Type":"bind","Source":root,"Destination":"/tmp/manyforge-state"}] if root else [])+[{"Type":"bind","Source":os.environ["FAKE_WORKSPACE"],"Destination":"/workspace"}]; print(json.dumps({"container_id":os.environ.get("FAKE_ACTUAL_ID","d"*64),"manyforge_export_kind":"console_tail" if "console_tail" in sys.argv[1] else "legacy_state","image":"sha256:test","mounts":mounts,"state":{"status":"exited","running":False,"exit_code":0,"finished_at":"time"}},separators=(",",":")))' "$2"
      [[ ${FAKE_SUMMARY_FAIL:-false} != true ]] || exit 1
    elif [[ -n ${FAKE_OLDER_ROW:-} && "$*" == *"$FAKE_OLDER_ID"* ]]; then
      printf '[%s]\\n' "$FAKE_OLDER_ROW"
    elif [[ -n ${FAKE_BOUND_STATE_ROOT:-} ]]; then
      [[ ${FAKE_RUNNING_INSPECT_FAIL:-false} != true ]] || exit 1
      python3 -c 'import json,os;print(json.dumps([{"Id":"other-owned-container","State":{"Running":os.environ.get("FAKE_BOUND_STATUS","running")=="running","Paused":os.environ.get("FAKE_BOUND_STATUS")=="paused","Restarting":os.environ.get("FAKE_BOUND_STATUS")=="restarting"},"Mounts":[{"Type":"bind","Source":os.environ["FAKE_BOUND_STATE_ROOT"],"Destination":"/tmp/manyforge-state"}]}]))'
    else
      printf '[{"State":{"Status":"exited","Running":false}}]\\n'
    fi
    ;;
  container)
    [[ ${FAKE_LIST_FAIL:-false} != true ]] || exit 1
    if [[ "$*" == *--quiet* ]]; then
      if [[ "$*" == *status=running* ]]; then
        [[ -z ${FAKE_BOUND_STATE_ROOT:-} ]] || printf 'other-owned-container\\n'
      elif [[ -n ${FAKE_OLDER_ID:-} ]]; then
        printf '%s\\n' "$FAKE_OLDER_ID"
      fi
    elif [[ ${FAKE_CONTAINER_LISTED:-false} == true ]]; then
      printf '%s %s\\n' "${FAKE_ACTUAL_ID:-dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd}" "$MANYFORGE_DEV_CONTAINER_NAME"
    fi
    ;;
  image) printf '%s\\n' "$MANYFORGE_DEV_IMAGE" ;;
  run)
    : > "$FAKE_DOCKER_STARTED"
    printf '%s\\n' "${FAKE_ACTUAL_ID:-dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd}"
    ;;
  exec)
    printf '{"ready":true}\\n'
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
    if [[ -n ${FAKE_LOG_BYTES:-} ]]; then
      python3 -c 'import os,sys;sys.stdout.buffer.write(b"x"*int(os.environ["FAKE_LOG_BYTES"])+b"END")'
    else
      printf 'preserved log\\n'
    fi
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

    if settings.get("FAKE_ACTUAL_STATE_ROOT"):
        Path(settings["FAKE_ACTUAL_STATE_ROOT"]).mkdir(parents=True, exist_ok=True)
    environment = {
        **os.environ,
        "PATH": f"{fake_bin}:{os.environ['PATH']}",
        "MANYFORGE_DEV_CACHE_BASE": str(tmp_path / "cache"),
        "FAKE_CONTAINER_ROOT": str(container_root),
        "FAKE_WORKSPACE": str(REPOSITORY_ROOT),
        "FAKE_DOCKER_CALLS": str(tmp_path / "docker-calls"),
        "FAKE_DOCKER_STOPPED": str(tmp_path / "docker-stopped"),
        "FAKE_DOCKER_REMOVED": str(tmp_path / "docker-removed"),
        "FAKE_DOCKER_STARTED": str(tmp_path / "docker-started"),
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
    assert re.fullmatch(r"\d{8}T\d{6}Z\.\d{9}", evidence_path.name)
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
    assert '"mounts":' in inspection
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
    assert "capture failed, container retained" in completed.stderr
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


def test_plain_stop_reports_failed_stop_without_deleting(tmp_path: Path) -> None:
    completed, _environment = _run_launcher(
        tmp_path,
        "stop",
        FAKE_STOP_FAIL="true",
        FAKE_REMOVE_FAIL="true",
    )

    assert completed.returncode == 69
    assert "Failed to stop" in completed.stderr


@pytest.mark.parametrize("explicit_ros_flag", [False, True])
@pytest.mark.parametrize("selection", [[], ["--bootstrap-project", "examples/first.scenario"],
                                       ["--project", "examples/authored.scenario"]])
@pytest.mark.parametrize("container_name", ["manyforge-development-workbench", "isolated-workbench"])
def test_start_preserves_ros_topology_by_default_and_refuses_removed_flag(
    tmp_path: Path, explicit_ros_flag: bool, selection: list[str], container_name: str
) -> None:
    environment = _fake_docker_environment(tmp_path)
    workspace = tmp_path / "workspace"
    launcher = workspace / "scripts" / LAUNCHER.name
    launcher.parent.mkdir(parents=True)
    launcher.write_text(LAUNCHER.read_text(encoding="utf-8"), encoding="utf-8")
    _copy_retention_helper(launcher)
    docker = tmp_path / "bin/docker"
    docker.write_text(
        """#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$*" >> "$FAKE_DOCKER_CALLS"
case "$1" in
  image) printf 'sha256:test\\n' ;;
  container) ;;
  run) : > "$FAKE_DOCKER_STARTED"; printf 'dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\\n' ;;
  inspect)
    [[ -e "$FAKE_DOCKER_STARTED" ]] || exit 1
    printf 'true\\n'
    ;;
  exec)
    if [[ "$*" == *"scripts/dev-workbench.py"* ]]; then
      printf '{"ready":true,"unavailable_reasons":[]}\\n'
    fi
    ;;
  *) exit 98 ;;
esac
""",
        encoding="utf-8",
    )
    environment.update(
        FAKE_DOCKER_STARTED=str(tmp_path / "docker-started"),
        ROS_STATIC_PEERS="orin;laptop",
        ROS_DOMAIN_ID="17",
        RMW_IMPLEMENTATION="rmw_fastrtps_cpp",
        MANYFORGE_ROS2_CONTROL_BUILD_TESTING="ON",
        MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS="/qualified/first.bash:/qualified/second.bash",
        MANYFORGE_DEV_IMAGE="current-ros-image",
        MANYFORGE_DEV_CONTAINER_NAME=container_name,
    )
    arguments = ["start", *(["--ros2-control"] if explicit_ros_flag else []), *selection]
    completed = subprocess.run(
        ["/bin/bash", str(launcher), *arguments], cwd=workspace,
        env=environment, capture_output=True, text=True, check=False,
    )
    if explicit_ros_flag:
        assert completed.returncode == 64
        assert "Usage:" in completed.stderr
        assert not Path(environment["FAKE_DOCKER_CALLS"]).exists()
        return
    assert completed.returncode == 0, completed.stderr
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text().splitlines()
    command = next(call for call in calls if call.startswith("run "))
    assert os.access(workspace, os.W_OK)
    assert f"-v {workspace}:/workspace" in command
    runtime_root = tmp_path / "cache/runtime" / container_name
    assert f"-v {runtime_root}:/tmp/manyforge-state" in command
    assert runtime_root.is_dir()
    selected = json.loads((runtime_root / "current/session.json").read_text())
    assert re.fullmatch(r"[0-9a-f]{32}", selected["session_id"])
    assert f"-e MANYFORGE_DEV_COLD_SESSION_ID={selected['session_id']}" in command
    assert "--network host" in command
    assert "--label manyforge.dev.ros2-control=true" in command
    assert command.endswith(
        "sha256:test python3 scripts/dev-workbench.py serve" +
        (" " + " ".join(selection) if selection else "")
    )
    for name in ("ROS_DOMAIN_ID", "RMW_IMPLEMENTATION", "ROS_STATIC_PEERS",
                 "MANYFORGE_ROS2_CONTROL_BUILD_TESTING", "MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS"):
        assert f"--env {name}" in command
    assert f"--user {os.getuid()}:{os.getgid()}" in command
    assert "--env HOME=/runner/home" in command
    assert "--env MANYFORGE_BUILD_DIR=/runner/build" in command
    cache_root = tmp_path / "cache/images/test"
    for directory in ("home", "build", "node_modules"):
        assert (cache_root / directory).is_dir()
        assert (cache_root / directory).stat().st_uid == os.getuid()
    assert "launch-isaac" not in command
    assert "ROS_STATIC_PEERS=orin;laptop" not in command


@pytest.mark.parametrize("same_root", [False, True])
def test_start_refuses_shared_live_state_but_allows_another_root(tmp_path: Path, same_root: bool) -> None:
    launcher = LAUNCHER if same_root else _copy_writable_launcher(tmp_path)
    state_root = tmp_path / "owned-state"
    environment = _fake_docker_environment(tmp_path,
        FAKE_WORKSPACE=str(launcher.parents[1]), MANYFORGE_DEV_IMAGE="sha256:test", FAKE_CONTAINER_PRESENT="false",
        MANYFORGE_DEV_RUNTIME_STATE_ROOT=str(state_root),
        FAKE_BOUND_STATE_ROOT=str(state_root if same_root else tmp_path / "another-state"))
    completed = subprocess.run(["bash", str(launcher), "start"], cwd=REPOSITORY_ROOT,
        env=environment, capture_output=True, text=True, check=False)
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text().splitlines()
    if same_root:
        assert completed.returncode == 69
        assert "Refusing overlapping live ManyForge state root" in completed.stderr
        assert not any(call.startswith("run ") for call in calls)
        assert not state_root.exists()
    else:
        assert completed.returncode == 0, completed.stderr
        assert f"-v {state_root}:/tmp/manyforge-state" in next(call for call in calls if call.startswith("run "))


def test_start_does_not_infer_unique_state_root_from_failed_inspection(tmp_path: Path) -> None:
    completed, environment = _run_launcher(tmp_path, "start", MANYFORGE_DEV_IMAGE="sha256:test",
        FAKE_CONTAINER_PRESENT="false", FAKE_BOUND_STATE_ROOT=str(tmp_path / "another-state"),
        FAKE_RUNNING_INSPECT_FAIL="true")
    assert completed.returncode == 69
    assert not any(call.startswith("run ") for call in Path(environment["FAKE_DOCKER_CALLS"]).read_text().splitlines())


@pytest.mark.parametrize("binding", ["same", "ancestor", "descendant", "prefix_sibling"])
def test_cold_start_requires_no_overlapping_live_bind_before_session_rotation(tmp_path, binding):
    launcher = _copy_writable_launcher(tmp_path)
    root = tmp_path / "runtime" / "selected"
    root.mkdir(parents=True)
    protected = root / "program-cycle.sqlite3"
    if binding != "prefix_sibling":
        protected.write_bytes(b"old cycle including unresolved effects")
    bound = {"same": root, "ancestor": root.parent,
             "descendant": root / "execution-state", "prefix_sibling": root.with_name("selected-other")}[binding]
    environment = _fake_docker_environment(tmp_path,
        FAKE_WORKSPACE=str(launcher.parents[1]), MANYFORGE_DEV_IMAGE="sha256:test",
        FAKE_CONTAINER_PRESENT="false", MANYFORGE_DEV_RUNTIME_STATE_ROOT=str(root),
        FAKE_BOUND_STATE_ROOT=str(bound))
    result = subprocess.run(["bash", str(launcher), "start"], env=environment,
        cwd=REPOSITORY_ROOT, capture_output=True, text=True, check=False)
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text().splitlines()
    if binding == "prefix_sibling":
        assert result.returncode == 0, result.stderr
        assert any(call.startswith("run ") for call in calls)
        # No overlapping mount is present. A genuinely fresh root needs no
        # predecessor proof; nonempty unknown legacy roots remain protected.
        assert (root / "current/session.json").is_file()
        assert not (root / "previous-sessions").exists()
    else:
        assert result.returncode == 69
        assert "overlapping live ManyForge state root" in result.stderr
        assert protected.read_bytes() == b"old cycle including unresolved effects"
        assert not (root / "current").exists()
        assert not any(call.startswith("run ") for call in calls)


def test_plain_container_removal_keeps_live_durable_state_files(tmp_path: Path) -> None:
    state_root = tmp_path / "cache/runtime/manyforge-development-workbench"
    journal = state_root / "execution-state/cclc-journal.sqlite3"
    journal.parent.mkdir(parents=True)
    journal.write_bytes(b"existing durable incident evidence")
    completed, _ = _run_launcher(tmp_path, "stop")
    assert completed.returncode == 0
    assert journal.read_bytes() == b"existing durable incident evidence"


@pytest.mark.parametrize("container_name", ["../foreign", "nested/name"])
def test_invalid_container_name_refuses_before_creating_state(tmp_path: Path, container_name: str) -> None:
    completed, environment = _run_launcher(tmp_path, "start", MANYFORGE_DEV_CONTAINER_NAME=container_name)
    assert completed.returncode == 64
    assert "Invalid Docker container name" in completed.stderr
    assert not (tmp_path / "cache").exists()
    assert not Path(environment["FAKE_DOCKER_CALLS"]).exists()


def test_ros_start_refuses_an_existing_native_workbench(tmp_path: Path) -> None:
    environment = _fake_docker_environment(tmp_path, FAKE_ROS_LABEL="false")

    completed = subprocess.run(
        ["/bin/bash", str(LAUNCHER), "start",
         "--bootstrap-project", "examples/first.scenario"],
        cwd=REPOSITORY_ROOT, env=environment, capture_output=True, text=True,
        check=False,
    )
    assert completed.returncode == 69
    assert "different runtime capabilities" in completed.stderr
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text()
    assert "run " not in calls


@pytest.mark.parametrize("arguments,error", [
    (["--bootstrap-project", "first", "--project", "second"],
     "excludes --project"),
])
def test_invalid_ros_selection_is_refused_before_docker(
    tmp_path: Path, arguments: list[str], error: str
) -> None:
    completed, environment = _run_launcher(tmp_path, "start", *arguments)
    assert completed.returncode == 64
    assert error in completed.stderr
    assert not Path(environment["FAKE_DOCKER_CALLS"]).exists()


def _g_environment(tmp_path, **settings):
    return _fake_docker_environment(tmp_path,
        MANYFORGE_DEV_CONTAINER_NAME="manyforge-g-fixture",
        MANYFORGE_DEV_IMAGE="sha256:" + "e" * 64,
        MANYFORGE_DEV_LAUNCH_INSTANCE_ID="isaac-fixture",
        MANYFORGE_DEV_LAUNCH_INPUT_DIGEST="a" * 64,
        MANYFORGE_DEV_EXPECTED_CONTAINER_ID="d" * 64,
        MANYFORGE_DEV_EVIDENCE_ROOT=str(tmp_path / "single-evidence-root"),
        **settings)


def _invoke_g(environment, *arguments, launcher=LAUNCHER):
    return subprocess.run(["bash", str(launcher), *arguments], cwd=launcher.parents[1],
                          env=environment, capture_output=True, text=True, check=False)


def test_g_plain_stop_exports_exact_container_logs_before_removal(tmp_path):
    environment = _g_environment(tmp_path)
    completed = _invoke_g(environment, "stop")
    assert completed.returncode == 0, completed.stderr
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text().splitlines()
    assert next(c for c in calls if c.startswith("cp ")).startswith("cp " + "d" * 64 + ":")
    assert calls[-1] == "rm " + "d" * 64
    evidence = next(Path(environment["MANYFORGE_DEV_EVIDENCE_ROOT"]).iterdir())
    assert not (evidence / "docker-logs-reference.txt").exists()
    assert (evidence / "manyforge-state/manager/journal.sqlite3").read_text() == "manager evidence"


@pytest.mark.parametrize("operation", ["stop", "reuse_exited"])
def test_g_export_failure_keeps_container_and_never_starts_replacement(tmp_path, operation):
    environment = _g_environment(tmp_path, FAKE_COPY_FAIL="true", FAKE_INITIAL_EXITED="true")
    completed = _invoke_g(environment, *(["stop"] if operation == "stop" else ["start"]))
    assert completed.returncode == 69, completed.stderr
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text().splitlines()
    assert any(c.startswith("cp ") for c in calls)
    assert not any(c.startswith(("rm ", "run ")) for c in calls)
    assert not Path(environment["FAKE_DOCKER_REMOVED"]).exists()
    assert len(list(Path(environment["MANYFORGE_DEV_EVIDENCE_ROOT"]).glob("*.partial"))) == 1


def test_g_failed_start_returns_exact_metadata_and_leaves_ros_client_for_owner_cleanup(tmp_path):
    environment = _g_environment(tmp_path, FAKE_CONTAINER_PRESENT="false", FAKE_EXIT_AFTER_RUN="true")
    # Fixture storage is owned scratch under HOME, representing persistent host
    # storage. Production rejects /tmp and tmpfs; no test-only bypass is used.
    with tempfile.TemporaryDirectory(prefix="manyforge-q93-fixture-", dir=Path.home()) as persistent:
        install = tmp_path / "qualified-package"
        install.mkdir()
        workspace = tmp_path / "workspace"
        launcher = workspace / "scripts/compose-dev.sh"
        launcher.parent.mkdir(parents=True)
        launcher.write_text(LAUNCHER.read_text())
        _copy_retention_helper(launcher)
        environment.update(MANYFORGE_DEV_RUNTIME_STATE_ROOT=persistent,
            MANYFORGE_DEV_OPERATOR_INSTALL_ROOT=str(install), MANYFORGE_DEV_OPERATOR_CELL_ID="fixture_cell",
            MANYFORGE_DEV_OPERATOR_ADMISSION="canonical-empty-admission.fixture_cell.json",
            MANYFORGE_DEV_OPERATOR_PACKAGE_SHA256="b" * 64, ROS_DOMAIN_ID="17", ROS_STATIC_PEERS="one;two")
        completed = _invoke_g(environment, "start", launcher=launcher)
    assert completed.returncode == 69, completed.stderr
    row = json.loads(completed.stdout)
    assert row["container_id"] == "d" * 64 and row["running"] is False
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text().splitlines()
    command = next(c for c in calls if c.startswith("run "))
    assert "--network host" in command
    assert "--log-driver json-file" in command and "--log-opt max-size=50m --log-opt max-file=3" in command
    assert "--env ROS_DOMAIN_ID" in command and "--env ROS_STATIC_PEERS" in command
    assert f"-v {persistent}:/tmp/manyforge-state" in command
    assert "--operator-runtime" in command and "--composer-port 9000 --composer-bind-host 0.0.0.0" in command
    assert "--project" not in command and "npm" not in command and "Vite" not in command
    assert not any(c.startswith(("stop ", "cp ", "rm ")) for c in calls)


@pytest.mark.parametrize("operation", ["stop", "start", "status"])
def test_g_refuses_foreign_occurrence_without_mutation(tmp_path, operation):
    environment = _g_environment(tmp_path, FAKE_ACTUAL_ID="f" * 64)
    completed = _invoke_g(environment, operation)
    assert completed.returncode != 0
    assert "Refusing foreign" in completed.stderr
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text().splitlines()
    assert not any(c.startswith(("run ", "stop ", "cp ", "rm ", "exec ")) for c in calls)


@pytest.mark.parametrize("expected_id", ["", "d" * 64])
def test_g_failed_inspect_does_not_claim_absence_for_listed_name_or_id(tmp_path, expected_id):
    environment = _g_environment(tmp_path, FAKE_INSPECT_FAIL="true", FAKE_CONTAINER_LISTED="true")
    environment["MANYFORGE_DEV_EXPECTED_CONTAINER_ID"] = expected_id
    completed = _invoke_g(environment, "status")
    assert completed.returncode == 75
    assert completed.stdout == ""
    assert "listed occurrence" in completed.stderr


def test_g_daemon_list_failure_is_unknown_not_absent(tmp_path):
    environment = _g_environment(tmp_path, FAKE_INSPECT_FAIL="true", FAKE_LIST_FAIL="true")
    completed = _invoke_g(environment, "status")
    assert completed.returncode != 0 and completed.stdout == ""


def test_g_exited_status_crosses_real_ssh_boundary_as_authenticated_facts(tmp_path, monkeypatch):
    from manyforge_operator.isaac import IsaacLauncher, IsaacLauncherConfig, RemoteNativeConfig

    environment = _g_environment(tmp_path, FAKE_INITIAL_EXITED="true")
    workspace = tmp_path / "workspace"
    launcher_path = workspace / "scripts/compose-dev.sh"
    launcher_path.parent.mkdir(parents=True)
    launcher_path.write_text(LAUNCHER.read_text())
    # Exercise _ssh's subprocess/nonzero policy with the real compose command;
    # the fixture SSH transport executes locally and the Docker daemon is fake.
    ssh = tmp_path / "ssh"
    ssh.write_text('#!/bin/bash\nexec bash -c "${@: -1}"\n')
    ssh.chmod(0o755)
    for name, value in environment.items():
        if name.startswith("FAKE_") or name == "PATH":
            monkeypatch.setenv(name, value)
    remote = RemoteNativeConfig(target="fixture", source_root=str(workspace),
        manager_executable="/qualified/manager", install_root="/qualified/package",
        node_catalog_path="/qualified/nodes", alarm_catalog_path="/qualified/alarms",
        state_root="/srv/state", simulation_control_config="/qualified/simulation.json",
        ros_environment_script="/qualified/topology.sh", package_aggregate_sha256="a" * 64,
        compiler_build_manifest_sha256="b" * 64, node_environment_sha256="c" * 64,
        container_image_id=environment["MANYFORGE_DEV_IMAGE"], container_name=environment["MANYFORGE_DEV_CONTAINER_NAME"],
        cache_root=str(tmp_path / "cache"), runtime_root="/srv/runtime", evidence_root=str(tmp_path / "evidence"))
    simulator = tmp_path / "isaac-sim.sh"
    simulator.write_text("#!/bin/bash\n")
    simulator.chmod(0o755)
    (tmp_path / "setup_ros_env.sh").write_text("# fixture\n")
    world = tmp_path / "world.usda"
    world.write_text("#usda 1.0\n")
    topology = tmp_path / "topology.sh"
    topology.write_text("# fixture\n")
    config = IsaacLauncherConfig(source_root=workspace, isaac_executable=simulator,
        world_path=world, remote=remote, state_root=tmp_path / "outer-state",
        instance_id="isaac-fixture", ssh_executable=ssh,
        local_ros_environment_script=topology)
    # Docker returns the exact digest forwarded by the production shell.
    outer = IsaacLauncher(config)
    status = outer._remote_container_operation("status")
    assert status["state"] == "degraded"
    assert status["container_running"] is False
    assert status["owned_resources"][0]["container_id"] == "d" * 64
    assert status["resource_observations"][0]["presence"] == "present"


@pytest.mark.parametrize("flag", [(), ("--keep-evidence",)])
def test_mounted_stop_captures_only_bounded_tail_and_summary(tmp_path, flag):
    root = tmp_path / "durable"
    root.mkdir()
    journal = root / "journal.sqlite3"
    journal.write_bytes(b"durable evidence")
    completed, environment = _run_launcher(tmp_path, "stop", *flag,
        FAKE_ACTUAL_STATE_ROOT=str(root), FAKE_LOG_BYTES="10000043")
    assert completed.returncode == 0, completed.stderr
    evidence = next((root / "evidence").iterdir())
    tail = (evidence / "docker.log").read_bytes()
    assert len(tail) == 10000000 and tail.endswith(b"END")
    summary = json.loads((evidence / "docker-inspect.json").read_text())
    assert summary["container_id"] == "d"*64 and summary["manyforge_export_kind"] == "console_tail"
    assert journal.read_bytes() == b"durable evidence"
    assert not (evidence / "manyforge-state").exists()
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text()
    assert "cp " not in calls and "rm " in calls


def test_plain_stop_stops_before_failed_capture_and_keeps_container(tmp_path):
    root = tmp_path / "durable"
    completed, environment = _run_launcher(tmp_path, "stop", FAKE_ACTUAL_STATE_ROOT=str(root), FAKE_LOGS_FAIL="true")
    assert completed.returncode == 69
    assert Path(environment["FAKE_DOCKER_STOPPED"]).exists()
    assert not Path(environment["FAKE_DOCKER_REMOVED"]).exists()
    receipt = json.loads(next((root / "evidence").glob("*.partial/docker-inspect.json")).read_text())
    assert receipt["container_id"] == "d"*64
    assert "capture failed, container retained" in completed.stderr


def test_failed_developer_start_uses_same_capture_path(tmp_path):
    launcher = _copy_writable_launcher(tmp_path)
    root = tmp_path / "cache/runtime/manyforge-development-workbench"
    environment = _fake_docker_environment(tmp_path, FAKE_WORKSPACE=str(launcher.parents[1]), FAKE_CONTAINER_PRESENT="false", FAKE_EXIT_AFTER_RUN="true",
        MANYFORGE_DEV_IMAGE="sha256:"+"e"*64, FAKE_ACTUAL_STATE_ROOT=str(root))
    completed = subprocess.run(["bash", str(launcher), "start"], cwd=REPOSITORY_ROOT,
        env=environment, capture_output=True, text=True, check=False)
    assert completed.returncode == 69
    assert Path(environment["FAKE_DOCKER_STOPPED"]).exists()
    assert Path(environment["FAKE_DOCKER_REMOVED"]).exists()
    assert list((root / "evidence").glob("*/docker.log"))
    assert "cp " not in Path(environment["FAKE_DOCKER_CALLS"]).read_text()


def test_legacy_transition_refuses_nonempty_target_before_removal(tmp_path):
    root = tmp_path / "target"
    root.mkdir()
    (root / "journal.sqlite3").write_bytes(b"unrelated existing state")
    completed, environment = _run_launcher(tmp_path, "start", FAKE_INITIAL_EXITED="true", MANYFORGE_DEV_RUNTIME_STATE_ROOT=str(root))
    assert completed.returncode == 69 and "legacy unmounted transition" in completed.stderr
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text()
    assert "rm " not in calls and "run " not in calls and "cp " not in calls
    assert (root / "journal.sqlite3").read_bytes() == b"unrelated existing state"


def _older_receipt(tmp_path, *, mounted=True, receipt=True, root=None):
    identity = "a"*64
    root = root or tmp_path / "older-root"
    evidence = root / "evidence" if mounted else tmp_path / "cache/evidence"
    mounts = [{"Type":"bind","Source":str(REPOSITORY_ROOT),"Destination":"/workspace"}]
    if mounted:
        mounts.insert(0, {"Type":"bind","Source":str(root),"Destination":"/tmp/manyforge-state"})
    if receipt:
        partial = evidence / "20261013T000000Z.partial"
        partial.mkdir(parents=True)
        (partial / "docker-inspect.json").write_text(json.dumps({"container_id":identity,"image":"sha256:test","mounts":mounts,"state":{"running":False}}))
    return {"FAKE_OLDER_ID":identity,"FAKE_OLDER_STATE_ROOT":str(root) if mounted else "",
        "FAKE_OLDER_ROW":json.dumps({"Id":identity,"Image":"sha256:test","Mounts":mounts,"State":{"Running":False}})}


@pytest.mark.parametrize("older_failure", [False, True])
def test_failed_capture_retries_receipt_matched_older_once_then_removes(tmp_path, older_failure):
    settings = _older_receipt(tmp_path)
    completed, environment = _run_launcher(tmp_path, "stop", FAKE_ACTUAL_STATE_ROOT=str(tmp_path / "current-root"),
        FAKE_LOGS_FAIL="true", FAKE_OLDER_LOGS_FAIL=str(older_failure).lower(), **settings)
    assert completed.returncode == 69
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text().splitlines()
    assert calls.count("logs "+"a"*64) == 1
    assert "rm "+"a"*64 in calls
    assert not any(c.startswith("rm ") and c != "rm "+"a"*64 for c in calls)
    assert "persistent state remains" in completed.stderr if older_failure else "capture succeeded" in completed.stderr


@pytest.mark.parametrize("case", ["historical", "legacy"])
def test_retained_cap_keeps_unknown_and_failed_legacy_exceptions(tmp_path, case):
    settings = _older_receipt(tmp_path, mounted=case=="historical", receipt=case!="historical")
    completed, environment = _run_launcher(tmp_path, "stop", FAKE_ACTUAL_STATE_ROOT=str(tmp_path / "current-root"),
        FAKE_LOGS_FAIL="true", FAKE_OLDER_LOGS_FAIL="true", **settings)
    assert completed.returncode == 69
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text().splitlines()
    assert "rm "+"a"*64 not in calls
    assert "manually export" in completed.stderr
    assert "unidentified stopped" in completed.stderr if case=="historical" else "sole open-incident journal" in completed.stderr


def test_older_default_root_retry_uses_aggregate_accounting_from_actual_mount(tmp_path):
    older_root = tmp_path / "cache/runtime/older-workbench"
    current_root = tmp_path / "cache/runtime/manyforge-development-workbench"
    settings = _older_receipt(tmp_path, root=older_root)
    (current_root / "execution-state").mkdir(parents=True)
    with sqlite3.connect(current_root / "execution-state/cclc-journal.sqlite3") as database:
        database.execute("CREATE TABLE cclc_component_incidents(acknowledged INTEGER)")
    for stamp in ("20261010T000000Z", "20261011T000000Z"):
        capture = current_root / "evidence" / stamp
        capture.mkdir(parents=True)
        (capture / "docker.log").write_bytes(b"other root's closed trace")
        (capture / "docker-inspect.json").write_text(json.dumps({
            "container_id":"c"*64,"image":"sha256:"+"e"*64,"manyforge_export_kind":"console_tail",
            "state":{"running":False},"mounts":[{"Type":"bind","Source":str(current_root),"Destination":"/tmp/manyforge-state"},
                {"Type":"bind","Source":str(REPOSITORY_ROOT),"Destination":"/workspace"}]}))
    completed, environment = _run_launcher(tmp_path, "stop", FAKE_ACTUAL_STATE_ROOT=str(current_root),
        FAKE_LOGS_FAIL="true", FAKE_OLDER_LOGS_FAIL="false",
        MANYFORGE_LOG_EVIDENCE_MIN_FREE_BYTES=str(2**60), **settings)
    assert completed.returncode == 69
    assert "capture succeeded" in completed.stderr
    assert "default-root aggregate log/evidence pressure" in completed.stderr
    assert "configured-root log/evidence pressure" not in completed.stderr
    assert "rm "+"a"*64 in Path(environment["FAKE_DOCKER_CALLS"]).read_text()
    assert list((current_root / "evidence").glob("*.partial"))
    assert (current_root / "evidence/20261010T000000Z/docker.log").read_bytes()==b"other root's closed trace"
    assert (current_root / "evidence/20261011T000000Z/docker.log").read_bytes()==b"other root's closed trace"


@pytest.mark.parametrize("status", ["paused", "restarting"])
def test_nonrunning_active_root_users_refuse_start_and_rotation(tmp_path,status):
    root=tmp_path/"cache/runtime/manyforge-development-workbench"
    root.mkdir(parents=True)
    (root/"protected-artifact").write_bytes(b"active root")
    completed,environment=_run_launcher(tmp_path,"start",FAKE_CONTAINER_PRESENT="false",
        MANYFORGE_DEV_IMAGE="sha256:"+"e"*64,FAKE_BOUND_STATE_ROOT=str(root),FAKE_BOUND_STATUS=status)
    assert completed.returncode==69 and "overlapping live ManyForge state root" in completed.stderr
    assert "run " not in Path(environment["FAKE_DOCKER_CALLS"]).read_text()
    assert (root/"protected-artifact").read_bytes()==b"active root"
    calls=Path(environment["FAKE_DOCKER_CALLS"]).read_text()
    assert "status=paused" in calls and "status=restarting" in calls


@pytest.mark.parametrize("mounted", [False,True])
def test_symlinked_cache_ancestor_can_capture_stop(tmp_path,mounted):
    actual=tmp_path/"actual-storage"
    actual.mkdir()
    alias=tmp_path/"ssd-alias"
    alias.symlink_to(actual,target_is_directory=True)
    root=alias/"cache/runtime/manyforge-development-workbench"
    root.mkdir(parents=True)
    settings={"FAKE_ACTUAL_STATE_ROOT":str(root)} if mounted else {}
    completed,environment=_run_launcher(tmp_path,"stop",MANYFORGE_DEV_CACHE_BASE=str(alias/"cache"),**settings)
    assert completed.returncode==0,completed.stderr
    assert Path(environment["FAKE_DOCKER_REMOVED"]).exists()
    evidence=actual/"cache/runtime/manyforge-development-workbench/evidence" if mounted else actual/"cache/evidence"
    assert list(evidence.glob("*/docker.log"))


def test_mounted_symlinked_evidence_descendant_stops_but_refuses_capture(tmp_path):
    root=tmp_path/"runtime"
    root.mkdir()
    outside=tmp_path/"foreign"
    outside.mkdir()
    (root/"evidence").symlink_to(outside,target_is_directory=True)
    completed,environment=_run_launcher(tmp_path,"stop",FAKE_ACTUAL_STATE_ROOT=str(root))
    assert completed.returncode==69
    assert "Container is stopped, capture failed, container retained" in completed.stderr
    assert Path(environment["FAKE_DOCKER_STOPPED"]).exists()
    assert not Path(environment["FAKE_DOCKER_REMOVED"]).exists()
    assert not list(outside.iterdir())


def test_repeated_legacy_capture_failures_replace_the_same_owned_partial(tmp_path):
    environment=_fake_docker_environment(tmp_path,FAKE_COPY_FAIL="true")
    for _ in range(3):
        completed=subprocess.run(["bash",str(LAUNCHER),"stop"],env=environment,capture_output=True,text=True)
        assert completed.returncode==69
        partials=list((tmp_path/"cache/evidence").glob("*.partial"))
        assert len(partials)==1 and partials[0].name==".legacy-"+"d"*64+".partial"
        assert json.loads((partials[0]/"docker-inspect.json").read_text())["container_id"]=="d"*64
    assert not Path(environment["FAKE_DOCKER_REMOVED"]).exists()


def test_symlinked_cache_ancestor_can_start_new_workbench(tmp_path):
    launcher = _copy_writable_launcher(tmp_path)
    actual=tmp_path/"actual-storage"
    actual.mkdir()
    alias=tmp_path/"ssd-alias"
    alias.symlink_to(actual,target_is_directory=True)
    environment=_fake_docker_environment(tmp_path,FAKE_WORKSPACE=str(launcher.parents[1]),FAKE_CONTAINER_PRESENT="false",
        MANYFORGE_DEV_CACHE_BASE=str(alias/"cache"),MANYFORGE_DEV_IMAGE="sha256:"+"e"*64)
    completed=subprocess.run(["bash",str(launcher),"start"],cwd=REPOSITORY_ROOT,
        env=environment,capture_output=True,text=True,check=False)
    assert completed.returncode==0,completed.stderr
    assert (actual/"cache/runtime/manyforge-development-workbench").is_dir()
    assert "run " in Path(environment["FAKE_DOCKER_CALLS"]).read_text()


def test_failed_inspect_never_replaces_existing_legacy_partial(tmp_path):
    partial=tmp_path/"cache/evidence"/(".legacy-"+"d"*64+".partial")
    partial.mkdir(parents=True)
    (partial/"copied-journal").write_bytes(b"old sole evidence")
    (partial/"docker-inspect.json").write_text(json.dumps({"container_id":"d"*64,"image":"sha256:test",
        "mounts":[{"Type":"bind","Source":str(REPOSITORY_ROOT),"Destination":"/workspace"}],"state":{"running":False}}))
    completed,environment=_run_launcher(tmp_path,"stop",FAKE_SUMMARY_FAIL="true")
    assert completed.returncode==69
    assert "Could not inspect the stopped-container capture" in completed.stderr
    assert Path(environment["FAKE_DOCKER_STOPPED"]).exists()
    assert (partial/"copied-journal").read_bytes()==b"old sole evidence"
    assert len(list(partial.parent.glob("*.partial")))==1
    assert not Path(environment["FAKE_DOCKER_REMOVED"]).exists()



def test_repeated_mounted_log_failures_reuse_one_partial_and_keep_previous_tail(tmp_path):
    root = tmp_path/'durable'
    environment = _fake_docker_environment(tmp_path, FAKE_ACTUAL_STATE_ROOT=str(root), FAKE_LOGS_FAIL='true')
    partial = None
    for attempt in range(3):
        completed = subprocess.run(['/bin/bash', str(LAUNCHER), 'stop'], cwd=REPOSITORY_ROOT,
            env=environment, capture_output=True, text=True, check=False)
        assert completed.returncode == 69, completed.stderr
        assert Path(environment['FAKE_DOCKER_STOPPED']).exists()
        assert not Path(environment['FAKE_DOCKER_REMOVED']).exists()
        partials = list((root/'evidence').glob('*.partial'))
        assert len(partials) == 1 and partials[0].name == '.mounted-'+'d'*64+'.partial'
        partial = partials[0]
        if attempt == 0:
            (partial/'docker.log').write_bytes(b'previous successful bounded tail')
        else:
            assert (partial/'docker.log').read_bytes() == b'previous successful bounded tail'
    environment['FAKE_LOGS_FAIL'] = 'false'
    completed = subprocess.run(['/bin/bash', str(LAUNCHER), 'stop'], cwd=REPOSITORY_ROOT,
        env=environment, capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    assert not partial.exists() and not list((root/'evidence').glob('*.partial'))
    evidence = next((root/'evidence').iterdir())
    assert (evidence/'docker.log').read_bytes() == b'preserved log\n'
    assert not (evidence/'.docker.log.next').exists()
    assert Path(environment['FAKE_DOCKER_REMOVED']).exists()



@pytest.mark.parametrize('user_file', [False, True])
def test_start_completes_mounted_stopped_predecessor_capture_and_preserves_durable_root(tmp_path, user_file):
    launcher = _copy_writable_launcher(tmp_path)
    root = tmp_path/'cache/runtime/manyforge-development-workbench'
    environment = _fake_docker_environment(tmp_path, FAKE_WORKSPACE=str(launcher.parents[1]),
        FAKE_ACTUAL_STATE_ROOT=str(root),
        FAKE_INITIAL_EXITED='true', MANYFORGE_DEV_IMAGE='sha256:test')
    docker = tmp_path/'bin/docker'
    fake = docker.read_text().replace('|| ${FAKE_INITIAL_EXITED:-false} == true',
        '|| ( ${FAKE_INITIAL_EXITED:-false} == true && ! -e "$FAKE_DOCKER_STARTED" )')
    fake = fake.replace('  run)\n    : > "$FAKE_DOCKER_STARTED"',
        '  run)\n    rm -f -- "$FAKE_DOCKER_STOPPED"\n    : > "$FAKE_DOCKER_STARTED"')
    docker.write_text(fake)
    durable = root/'execution-state/cclc-journal.sqlite3'
    durable.parent.mkdir()
    with sqlite3.connect(durable) as db:
        db.execute('CREATE TABLE cclc_component_incidents(acknowledged INTEGER)')
        db.execute('INSERT INTO cclc_component_incidents VALUES (0)')
    before = durable.read_bytes()
    partial = root/'evidence'/('.mounted-'+'d'*64+'.partial')
    partial.mkdir(parents=True)
    mounts = [{'Type':'bind','Source':str(root),'Destination':'/tmp/manyforge-state'},
        {'Type':'bind','Source':environment['FAKE_WORKSPACE'],'Destination':'/workspace'}]
    (partial/'docker-inspect.json').write_text(json.dumps({'container_id':'d'*64,
        'image':'sha256:test','mounts':mounts,'state':{'running':False},'manyforge_export_kind':'console_tail'}))
    (partial/'docker.log').write_bytes(b'previous partial tail')
    if user_file:
        (partial/'operator-note.txt').write_bytes(b'useful user evidence')
    completed = subprocess.run(['/bin/bash', str(launcher), 'start'], cwd=REPOSITORY_ROOT,
        env=environment, capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    calls = Path(environment['FAKE_DOCKER_CALLS']).read_text().splitlines()
    position = lambda command: next(index for index, call in enumerate(calls) if call.startswith(command+' '))
    assert position('stop') < position('logs') < position('rm') < position('run')
    archived = list((root / "previous-sessions").iterdir())
    assert len(archived) == 1
    archived_durable = archived[0] / "execution-state/cclc-journal.sqlite3"
    assert archived_durable.read_bytes() == before and not partial.exists()
    assert not durable.exists()
    assert {p.name for p in (root / "current").iterdir()} == {"session.json"}
    captured = next(p for p in (root/'evidence').iterdir() if not p.name.endswith('.partial'))
    assert (captured/'docker.log').read_bytes() == b'preserved log\n'
    assert {p.name for p in captured.iterdir()} == {'docker-inspect.json', 'docker.log'}
    assert json.loads((captured/'docker-inspect.json').read_text())['container_id'] == 'd'*64
    assert f'-v {root}:/tmp/manyforge-state' in calls[position('run')]



def test_owned_extra_entries_removed_in_place_without_adding_failed_capture_siblings(tmp_path):
    root = tmp_path/'durable'
    environment = _fake_docker_environment(tmp_path, FAKE_ACTUAL_STATE_ROOT=str(root), FAKE_LOGS_FAIL='true')
    partial = root/'evidence'/('.mounted-'+'d'*64+'.partial')
    partial.mkdir(parents=True)
    mounts = [{'Type':'bind','Source':str(root),'Destination':'/tmp/manyforge-state'},
        {'Type':'bind','Source':str(REPOSITORY_ROOT),'Destination':'/workspace'}]
    (partial/'docker-inspect.json').write_text(json.dumps({'container_id':'d'*64,
        'image':'sha256:test','mounts':mounts,'state':{'running':False},'manyforge_export_kind':'console_tail'}))
    (partial/'docker.log').write_bytes(b'previous partial tail')
    (partial/'operator-note.txt').write_bytes(b'user evidence')
    (partial/'user-directory').mkdir()
    (partial/'user-directory/note').write_bytes(b'user evidence')
    outside = tmp_path/'outside'
    outside.write_bytes(b'foreign target')
    (partial/'user-link').symlink_to(outside)
    for _ in range(3):
        completed = subprocess.run(['/bin/bash', str(LAUNCHER), 'stop'], cwd=REPOSITORY_ROOT,
            env=environment, capture_output=True, text=True, check=False)
        assert completed.returncode == 69, completed.stderr
        assert not Path(environment['FAKE_DOCKER_REMOVED']).exists()
        assert list((root/'evidence').glob('*.partial')) == [partial]
        assert (partial/'docker.log').read_bytes() == b'previous partial tail'
        assert {p.name for p in partial.iterdir()} <= {'docker-inspect.json', 'docker.log', '.docker.log.next'}
        assert outside.read_bytes() == b'foreign target'
    environment['FAKE_LOGS_FAIL'] = 'false'
    completed = subprocess.run(['/bin/bash', str(LAUNCHER), 'stop'], cwd=REPOSITORY_ROOT,
        env=environment, capture_output=True, text=True, check=False)
    assert completed.returncode == 0, completed.stderr
    assert not list((root/'evidence').glob('*.partial'))
    assert Path(environment['FAKE_DOCKER_REMOVED']).exists()
    assert outside.read_bytes() == b'foreign target'
