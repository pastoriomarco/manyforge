from __future__ import annotations

import json
import os
import tempfile
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
    [[ ${FAKE_INSPECT_FAIL:-false} != true ]] || exit 1
    [[ ${FAKE_CONTAINER_PRESENT:-true} == true || -e ${FAKE_DOCKER_STARTED:-/nonexistent} ]] || exit 1
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
    elif [[ ${1:-} == --format ]]; then
      printf '{"image":"sha256:test","mounts":[],"state":{"status":"exited","running":false,"exit_code":0,"finished_at":"time"}}\\n'
    else
      printf '[{"State":{"Status":"exited","Running":false}}]\\n'
    fi
    ;;
  container)
    [[ ${FAKE_LIST_FAIL:-false} != true ]] || exit 1
    if [[ ${FAKE_CONTAINER_LISTED:-false} == true ]]; then
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


@pytest.mark.parametrize("ros2_control", [False, True])
def test_start_preserves_explicit_ros_topology_only_when_requested(
    tmp_path: Path, ros2_control: bool
) -> None:
    environment = _fake_docker_environment(tmp_path)
    workspace = tmp_path / "workspace"
    launcher = workspace / "scripts" / LAUNCHER.name
    launcher.parent.mkdir(parents=True)
    launcher.write_text(LAUNCHER.read_text(encoding="utf-8"), encoding="utf-8")
    docker = tmp_path / "bin/docker"
    docker.write_text(
        """#!/usr/bin/env bash
set -euo pipefail
printf '%s\\n' "$*" >> "$FAKE_DOCKER_CALLS"
case "$1" in
  image) printf 'sha256:test\\n' ;;
  run) : > "$FAKE_DOCKER_STARTED" ;;
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
        MANYFORGE_DEV_IMAGE="current-ros-image",
    )
    arguments = ["start", *([
        "--ros2-control", "--bootstrap-project", "examples/first.scenario",
    ] if ros2_control else [])]
    completed = subprocess.run(
        ["/bin/bash", str(launcher), *arguments], cwd=workspace,
        env=environment, capture_output=True, text=True, check=False,
    )
    assert completed.returncode == 0, completed.stderr
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text().splitlines()
    command = next(call for call in calls if call.startswith("run "))
    assert os.access(workspace, os.W_OK)
    assert f"-v {workspace}:/workspace" in command
    assert "--network host" in command
    assert f"--label manyforge.dev.ros2-control={str(ros2_control).lower()}" in command
    assert command.endswith(
        "sha256:test python3 scripts/dev-workbench.py serve" +
        (" --ros2-control --bootstrap-project examples/first.scenario" if ros2_control else "")
    )
    for name in ("ROS_DOMAIN_ID", "RMW_IMPLEMENTATION", "ROS_STATIC_PEERS"):
        assert (f"--env {name}" in command) is ros2_control
    if ros2_control:
        assert f"--user {os.getuid()}:{os.getgid()}" in command
        assert "--env HOME=/runner/home" in command
        assert "--env MANYFORGE_BUILD_DIR=/runner/build" in command
        cache_root = tmp_path / "cache/images/test"
        for directory in ("home", "build", "node_modules"):
            assert (cache_root / directory).is_dir()
            assert (cache_root / directory).stat().st_uid == os.getuid()
    else:
        assert "--user " not in command
        assert "--env HOME=" not in command
        assert "--env MANYFORGE_BUILD_DIR=" not in command
        assert not (tmp_path / "cache/images/test/home").exists()
    assert "launch-isaac" not in command
    assert "ROS_STATIC_PEERS=orin;laptop" not in command


def test_ros_start_refuses_an_existing_native_workbench(tmp_path: Path) -> None:
    environment = _fake_docker_environment(tmp_path, FAKE_ROS_LABEL="false")

    completed = subprocess.run(
        ["/bin/bash", str(LAUNCHER), "start", "--ros2-control",
         "--bootstrap-project", "examples/first.scenario"],
        cwd=REPOSITORY_ROOT, env=environment, capture_output=True, text=True,
        check=False,
    )
    assert completed.returncode == 69
    assert "different runtime capabilities" in completed.stderr
    calls = Path(environment["FAKE_DOCKER_CALLS"]).read_text()
    assert "run " not in calls


@pytest.mark.parametrize("arguments,error", [
    (["--ros2-control"], "requires --bootstrap-project or --project"),
    (["--bootstrap-project", "first"], "requires --ros2-control"),
    (["--ros2-control", "--bootstrap-project", "first", "--project", "second"],
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
    assert (evidence / "docker-logs-reference.txt").read_text() == "docker logs " + "d" * 64 + "\n"
    assert (evidence / "manyforge-state/manager/journal.sqlite3").read_text() == "manager evidence"


@pytest.mark.parametrize("operation", ["stop", "reuse_exited"])
def test_g_export_failure_keeps_container_and_never_starts_replacement(tmp_path, operation):
    environment = _g_environment(tmp_path, FAKE_COPY_FAIL="true", FAKE_INITIAL_EXITED="true")
    completed = _invoke_g(environment, *(["stop"] if operation == "stop" else ["start", "--ros2-control"]))
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
        environment.update(MANYFORGE_DEV_RUNTIME_STATE_ROOT=persistent,
            MANYFORGE_DEV_OPERATOR_INSTALL_ROOT=str(install), MANYFORGE_DEV_OPERATOR_CELL_ID="fixture_cell",
            MANYFORGE_DEV_OPERATOR_ADMISSION="canonical-empty-admission.fixture_cell.json",
            MANYFORGE_DEV_OPERATOR_PACKAGE_SHA256="b" * 64, ROS_DOMAIN_ID="17", ROS_STATIC_PEERS="one;two")
        completed = _invoke_g(environment, "start", "--ros2-control", launcher=launcher)
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
    completed = _invoke_g(environment, operation, *(["--ros2-control"] if operation == "start" else []))
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
