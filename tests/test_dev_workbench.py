from __future__ import annotations

from copy import deepcopy
import importlib.util
from pathlib import Path
import socket
from types import SimpleNamespace
from typing import Any

import pytest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "manyforge_dev_workbench",
    REPOSITORY_ROOT / "scripts/dev-workbench.py",
)
assert SPEC is not None and SPEC.loader is not None
dev_workbench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dev_workbench)


def test_status_reads_the_backend_derived_scenario_without_reducing_it_again(
    monkeypatch: Any,
) -> None:
    response = {
        "schema_version": "manyforge.current_scenario.v1",
        "ready": True,
        "current_scenario": {"scenario_id": "lite6_pick_and_place"},
        "unavailable_reasons": [],
    }

    class _Client:
        def __init__(self, *_args: Any, **_kwargs: Any) -> None:
            pass

        def __enter__(self) -> "_Client":
            return self

        def __exit__(self, *_args: Any) -> None:
            pass

        def request(self, method: str, path: str) -> dict[str, Any]:
            assert (method, path) == ("GET", "/api/scenarios/current")
            return response

    monkeypatch.setattr(dev_workbench, "HeadlessHttpClient", _Client)

    status = dev_workbench.query_status(
        scenario_id="lite6_pick_and_place", composer_port=9020
    )

    assert status["current_scenario"] == response["current_scenario"]
    assert status["classification"] == {
        "kind": "development_workbench",
        "package_kind": "maintained_qualified",
        "physical_execution_authorized": False,
    }


def test_status_rejects_a_different_requested_scenario(monkeypatch: Any) -> None:
    class _Client:
        def __init__(self, *_args: Any, **_kwargs: Any) -> None:
            pass

        def __enter__(self) -> "_Client":
            return self

        def __exit__(self, *_args: Any) -> None:
            pass

        def request(self, _method: str, _path: str) -> dict[str, Any]:
            return {
                "schema_version": "manyforge.current_scenario.v1",
                "ready": True,
                "current_scenario": {"scenario_id": "other"},
                "unavailable_reasons": [],
            }

    monkeypatch.setattr(dev_workbench, "HeadlessHttpClient", _Client)

    status = dev_workbench.query_status(
        scenario_id="lite6_pick_and_place", composer_port=9020
    )

    assert status["ready"] is False
    assert status["current_scenario"] is None
    assert status["unavailable_reasons"] == ["requested_scenario_mismatch"]


def test_status_command_reads_the_current_scenario_without_package_mutation(
    monkeypatch: Any,
    capsys: Any,
) -> None:
    observed: list[str | None] = []

    def query_status(
        *, scenario_id: str | None, composer_port: int
    ) -> dict[str, Any]:
        assert composer_port == 9020
        observed.append(scenario_id)
        return {
            "schema_version": "manyforge.current_scenario.v1",
            "ready": True,
            "current_scenario": {"scenario_id": "another-native-scenario"},
            "unavailable_reasons": [],
        }

    monkeypatch.setattr(dev_workbench, "query_status", query_status)

    def unexpected_import() -> None:
        raise AssertionError("status mutated package cache")

    monkeypatch.setattr(dev_workbench, "_import_package", unexpected_import)

    assert dev_workbench.main(["status"]) == 0

    assert observed == [None]
    assert "another-native-scenario" in capsys.readouterr().out


@pytest.mark.parametrize(
    "state", ["idle_after_reset", "terminal_awaiting_reset"]
)
def test_startup_accepts_contained_idle_and_resettable_terminal_cycles(
    state: str,
) -> None:
    client = SimpleNamespace(program_cycle_status=lambda: {"state": state})

    assert dev_workbench._startup_cycle_state(client) == state


def test_startup_rejects_an_uncontained_program_cycle() -> None:
    client = SimpleNamespace(program_cycle_status=lambda: {"state": "running"})

    with pytest.raises(
        dev_workbench.WorkbenchError,
        match="Program cycle is not contained at startup",
    ):
        dev_workbench._startup_cycle_state(client)


def test_package_cache_contains_scenario_content_only(
    monkeypatch: Any, tmp_path: Path
) -> None:
    scenario_root = tmp_path / "maintained-scenario-package"
    package_root = scenario_root / "retained-package"
    qualified_root = tmp_path / "qualified"
    qualified_package = qualified_root / "maintained-package"
    qualified_package.mkdir(parents=True)
    (qualified_package / "scenario.json").write_text("{}", encoding="utf-8")
    monkeypatch.setattr(dev_workbench, "SCENARIO_ROOT", scenario_root)
    monkeypatch.setattr(dev_workbench, "PACKAGE_ROOT", package_root)
    monkeypatch.setattr(dev_workbench, "CACHE_ROOT", tmp_path)
    monkeypatch.setattr(dev_workbench, "_qualified_root", lambda: qualified_root)

    dev_workbench._import_package()

    assert (package_root / "scenario.json").read_text(encoding="utf-8") == "{}"
    assert not (scenario_root / "manyforge_manager_daemon").exists()
    assert not (scenario_root / "headless-python").exists()
    assert not (scenario_root / "manyforge_native_execution_host").exists()
    assert not (scenario_root / "manyforge_roboplan_candidate_worker").exists()


def test_scenario_load_rejects_a_stale_workspace_deployment(
    monkeypatch: Any, tmp_path: Path
) -> None:
    workspace = tmp_path / "workspace"
    deployment = workspace / "examples/example.deployment.yaml"
    deployment.parent.mkdir(parents=True)
    deployment.write_text(
        '{"deployment_artifact_id":"example",'
        '"deployment_revision":"sha256:' + ('1' * 64) + '"}',
        encoding="utf-8",
    )
    monkeypatch.setattr(dev_workbench, "WORKSPACE", workspace)

    with pytest.raises(
        dev_workbench.WorkbenchError,
        match="package is stale relative to its workspace Deployment",
    ):
        dev_workbench._require_current_workspace_deployment(
            {
                "deployment_artifact_id": "example",
                "deployment_revision": "sha256:" + ("2" * 64),
            }
        )


def test_backend_build_uses_current_source_and_only_production_targets(
    monkeypatch: Any, tmp_path: Path
) -> None:
    build = tmp_path / "headless"
    manager = build / "manyforge_manager_daemon"
    extension = build / "manyforge_headless_py.fixture.so"
    observed: list[tuple[str, ...]] = []

    def run(command: tuple[str, ...], **_kwargs: Any) -> None:
        observed.append(command)
        if command[0] == "cmake" and command[1] == "--build":
            manager.parent.mkdir(parents=True, exist_ok=True)
            manager.write_bytes(b"manager")
            manager.chmod(0o755)
            extension.write_bytes(b"headless")

    monkeypatch.setattr(dev_workbench, "HEADLESS_BUILD", build)
    monkeypatch.setattr(dev_workbench, "MANAGER_BINARY", manager)
    monkeypatch.setattr(dev_workbench, "_run", run)
    monkeypatch.setattr(
        dev_workbench.subprocess,
        "run",
        lambda *_args, **_kwargs: SimpleNamespace(
            returncode=0, stdout="/pybind11/cmake\n"
        ),
    )
    monkeypatch.setenv("MANYFORGE_DEV_BUILD_JOBS", "3")

    assert dev_workbench._ensure_current_backend() == extension

    assert len(observed) == 2
    assert "-DBUILD_TESTING=OFF" in observed[0]
    assert observed[1][-5:] == (
        "manyforge_manager_daemon",
        "manyforge_headless_py",
        "--",
        "-k",
        "0",
    )
    assert "--parallel" in observed[1]
    assert observed[1][observed[1].index("--parallel") + 1] == "3"


def test_native_build_uses_current_source_and_only_runtime_targets(
    monkeypatch: Any, tmp_path: Path
) -> None:
    build = tmp_path / "roboplan"
    native_host = build / "manyforge_native_execution_host"
    candidate_worker = build / "manyforge_roboplan_candidate_worker"
    observed: list[tuple[str, ...]] = []

    def run(command: tuple[str, ...], **_kwargs: Any) -> None:
        observed.append(command)
        if command[0] == "cmake" and command[1] == "--build":
            build.mkdir(parents=True, exist_ok=True)
            for path in (native_host, candidate_worker):
                path.write_bytes(b"current-native")
                path.chmod(0o755)

    monkeypatch.setattr(dev_workbench, "ROBOPLAN_BUILD", build)
    monkeypatch.setattr(dev_workbench, "NATIVE_EXECUTION_HOST", native_host)
    monkeypatch.setattr(
        dev_workbench, "NATIVE_CANDIDATE_WORKER", candidate_worker
    )
    monkeypatch.setattr(dev_workbench, "_run", run)
    monkeypatch.setenv("MANYFORGE_DEV_BUILD_JOBS", "3")

    assert dev_workbench._ensure_current_native_backend() == (
        native_host,
        candidate_worker,
    )

    assert len(observed) == 2
    assert "-DBUILD_TESTING=OFF" in observed[0]
    assert observed[1][-5:] == (
        "manyforge_native_execution_host",
        "manyforge_roboplan_candidate_worker",
        "--",
        "-k",
        "0",
    )
    assert observed[1][observed[1].index("--parallel") + 1] == "3"


def test_startup_requires_the_existing_manager_baseline_to_be_fully_converged() -> None:
    status = {
        "source": {
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
        },
        "status": {
            "manager_ready": True,
            "runtime": {"converged": True},
        },
    }
    diff = {
        "source": {
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
        },
        "converged": True,
    }

    assert dev_workbench._manager_projection_ready(status, diff) is True
    assert dev_workbench._manager_projection_ready(
        status,
        {**diff, "converged": False},
    ) is False
    assert dev_workbench._manager_projection_ready(
        {**status, "status": {**status["status"], "manager_ready": False}},
        diff,
    ) is False


class _LifecycleClient:
    def __init__(self) -> None:
        self.state = "declared"
        self.generation = 1
        self.active: dict[str, Any] | None = None
        self.lifecycle_requests: list[dict[str, Any]] = []
        self.route_requests: list[dict[str, Any]] = []

    @staticmethod
    def _bindings() -> list[dict[str, str]]:
        return [
            {
                "control_binding_id": "binding.arm",
                "control_binding_revision": "binding.revision.1",
                "control_route_id": "route.arm",
            }
        ]

    def _desired(self) -> dict[str, Any]:
        return {
            "control_domain_id": "domain.arm",
            "control_domain_revision": "domain.revision.1",
            "controller_topology_id": "topology.arm",
            "topology_revision": "topology.revision.1",
            "activation_set_id": "arm_direct",
            "binding_routes": self._bindings(),
        }

    def cclc_component(self, component_id: str) -> dict[str, Any]:
        assert component_id == "component.arm"
        return {
            "controllable_component_id": component_id,
            "component_lifecycle_state": self.state,
            "component_lifecycle_generation": self.generation,
            "graph_revision": "graph.1",
            "domains": [
                {
                    "control_domain_id": "domain.arm",
                    "control_domain_revision": "domain.revision.1",
                    "controller_topology_id": "topology.arm",
                    "topology_revision": "topology.revision.1",
                    "current_control_bindings": self._bindings(),
                    "active_control_configuration": deepcopy(self.active),
                }
            ],
        }

    def cclc_owner_identity(self) -> dict[str, Any]:
        return {"request_owner_epoch": "cclc-owner.1"}

    def authoring_admission(self) -> dict[str, Any]:
        return {"authoring_allowed": False, "admission_generation": 7}

    def submit_component_lifecycle(
        self, request: dict[str, Any]
    ) -> dict[str, Any]:
        self.lifecycle_requests.append(deepcopy(request))
        return {
            "acceptance": {
                "lifecycle_transaction_id": f"lifecycle.{request['intent']}"
            }
        }

    def component_lifecycle_status(
        self, transaction_id: str
    ) -> dict[str, Any]:
        intent = transaction_id.removeprefix("lifecycle.")
        self.state = "prepared" if intent == "prepare" else "active"
        self.generation += 1
        return {"status": {"terminal_result": "succeeded"}}

    def cclc_route_templates(self, component_id: str) -> dict[str, Any]:
        assert component_id == "component.arm"
        return {
            "retained_bundle_id": "bundle.1",
            "graph_revision": "graph.1",
            "templates": [
                {
                    "desired_configuration": self._desired(),
                    "expected_active_control_configuration": (
                        dev_workbench._expected_active_configuration(self.active)
                    ),
                    "component_expectations": [
                        {
                            "controllable_component_id": component_id,
                            "expected_component_lifecycle_generation": (
                                self.generation
                            ),
                            "expected_component_lifecycle_state": "active",
                        }
                    ],
                }
            ],
        }

    def submit_route_selection(
        self, request: dict[str, Any]
    ) -> dict[str, Any]:
        self.route_requests.append(deepcopy(request))
        return {
            "acceptance": {
                "route_selection_transaction_id": "route-selection.1"
            }
        }

    def route_selection_status(self, transaction_id: str) -> dict[str, Any]:
        assert transaction_id == "route-selection.1"
        self.active = {
            **self._desired(),
            "active_control_configuration_id": "active-configuration.1",
            "active_control_configuration_generation": 1,
            "state": "reconciled",
        }
        return {"status": {"terminal_result": "succeeded"}}


def test_simulated_scenario_start_converges_only_missing_lifecycle_and_routes() -> None:
    client = _LifecycleClient()

    dev_workbench._converge_component(
        client,  # type: ignore[arg-type]
        "component.arm",
        scenario_id="scenario.1",
        processes=[],
    )

    assert client.state == "active"
    assert [row["intent"] for row in client.lifecycle_requests] == [
        "prepare",
        "enable",
    ]
    assert len(client.route_requests) == 1
    assert client.route_requests[0]["desired_configuration"] == client._desired()
    assert client.route_requests[0]["expected_admission_generation"] == 7

    dev_workbench._converge_component(
        client,  # type: ignore[arg-type]
        "component.arm",
        scenario_id="scenario.1",
        processes=[],
    )

    assert len(client.lifecycle_requests) == 2
    assert len(client.route_requests) == 1

    first_request_id = client.route_requests[0]["request_context"]["request_id"]
    client.active = {
        **client.active,
        "active_control_configuration_id": "active-configuration.2",
        "active_control_configuration_generation": 2,
        "activation_set_id": "different-route",
    }

    dev_workbench._converge_component(
        client,  # type: ignore[arg-type]
        "component.arm",
        scenario_id="scenario.1",
        processes=[],
    )

    assert len(client.lifecycle_requests) == 2
    assert len(client.route_requests) == 2
    assert (
        client.route_requests[1]["request_context"]["request_id"]
        != first_request_id
    )


class _ControlManagementClient(_LifecycleClient):
    def __init__(self, mode: str) -> None:
        super().__init__()
        self.mode = mode

    def execution_identity(self) -> dict[str, object]:
        return {"identity": {"control_management_mode": self.mode}}


def test_direct_control_scenario_does_not_require_cclc() -> None:
    client = _ControlManagementClient("direct")

    mode = dev_workbench._converge_control_management(
        client,  # type: ignore[arg-type]
        ["component.arm"],
        scenario_id="scenario.1",
        processes=[],
    )

    assert mode == "direct"
    assert client.lifecycle_requests == []
    assert client.route_requests == []


def test_lifecycle_control_scenario_converges_cclc() -> None:
    client = _ControlManagementClient("component_lifecycle")

    mode = dev_workbench._converge_control_management(
        client,  # type: ignore[arg-type]
        ["component.arm"],
        scenario_id="scenario.1",
        processes=[],
    )

    assert mode == "component_lifecycle"
    assert [row["intent"] for row in client.lifecycle_requests] == [
        "prepare",
        "enable",
    ]
    assert len(client.route_requests) == 1


def test_stale_socket_cleanup_removes_only_the_named_socket(tmp_path: Path) -> None:
    socket_path = tmp_path / "manager-command.sock"
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        listener.bind(str(socket_path))

        dev_workbench._remove_stale_socket(socket_path, "Manager command socket")

        assert not socket_path.exists()
    finally:
        listener.close()


def test_stale_socket_cleanup_rejects_non_socket_state(tmp_path: Path) -> None:
    state_path = tmp_path / "manager-command.sock"
    state_path.write_text("persistent state", encoding="utf-8")

    try:
        dev_workbench._remove_stale_socket(state_path, "Manager command socket")
    except dev_workbench.WorkbenchError as error:
        assert str(error) == f"Manager command socket is not a socket: {state_path}"
    else:
        raise AssertionError("non-socket state was removed")

    assert state_path.read_text(encoding="utf-8") == "persistent state"


def test_launcher_pins_the_image_and_separates_its_cache() -> None:
    launcher = (REPOSITORY_ROOT / "scripts/compose-dev.sh").read_text(
        encoding="utf-8"
    )

    assert "docker image inspect -f '{{.Id}}'" in launcher
    assert 'cache_root="$cache_base/images/$image_key"' in launcher
    assert '"$image_id" \\' in launcher


def test_cleanup_uses_bounded_reverse_owner_order(monkeypatch: Any) -> None:
    workbench = dev_workbench.Workbench(
        scenario_id="lite6_pick_and_place",
        headless_port=1,
        runner_port=2,
        composer_port=3,
        frontend_port=4,
    )
    workbench.processes = [
        ("Manager", SimpleNamespace()),
        ("Program runner", SimpleNamespace()),
        ("Headless API", SimpleNamespace()),
        ("Composer backend", SimpleNamespace()),
        ("Vite HMR", SimpleNamespace()),
    ]
    observed: list[str] = []
    monkeypatch.setattr(
        workbench,
        "_terminate",
        lambda process: observed.append(
            next(
                name
                for name, candidate in workbench.processes
                if candidate is process
            )
        ),
    )
    monkeypatch.setattr(workbench, "_stop_manager", lambda: observed.append("Manager"))
    monkeypatch.setattr(
        workbench,
        "_wait_for_program_runner_manager_expiry",
        lambda: observed.append("Program runner fenced"),
    )

    workbench.close()

    assert observed == [
        "Vite HMR",
        "Composer backend",
        "Headless API",
        "Manager",
        "Program runner fenced",
        "Program runner",
    ]
