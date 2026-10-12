"""Deterministic Isaac world-load and stopped-state launch barrier."""

from __future__ import annotations

import json
import math
import time
from collections.abc import Sequence
from pathlib import PurePosixPath
from typing import Any, Final
from urllib.parse import unquote, urlsplit

import rclpy
from rclpy.node import Node
from rclpy.parameter import Parameter
from simulation_interfaces.msg import SimulationState, SimulatorFeatures
from simulation_interfaces.srv import (
    GetCurrentWorld,
    GetSimulationState,
    GetSimulatorFeatures,
    LoadWorld,
    SetSimulationState,
)


RESULT_SCHEMA: Final = "manyforge.isaac_scene_load_barrier.v1"
_MAX_GRAPH_ROWS: Final = 4096
_SERVICE_TYPES: Final = {
    "features": "simulation_interfaces/srv/GetSimulatorFeatures",
    "load_world": "simulation_interfaces/srv/LoadWorld",
    "current_world": "simulation_interfaces/srv/GetCurrentWorld",
    "get_state": "simulation_interfaces/srv/GetSimulationState",
    "set_state": "simulation_interfaces/srv/SetSimulationState",
}
_PREFERRED_SERVICE_LEAVES: Final = {
    _SERVICE_TYPES["features"]: "get_simulator_features",
    _SERVICE_TYPES["load_world"]: "load_world",
    _SERVICE_TYPES["current_world"]: "get_current_world",
    _SERVICE_TYPES["get_state"]: "get_simulation_state",
    _SERVICE_TYPES["set_state"]: "set_simulation_state",
}


def normalize_file_world_uri(value: str) -> str:
    """Return one canonical local ``file:`` URI without touching that host."""

    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > 4096:
        raise ValueError("world URI must be bounded non-empty text")
    if value.startswith("/"):
        path = value
    else:
        parsed = urlsplit(value)
        if (
            parsed.scheme != "file"
            or parsed.netloc not in {"", "localhost"}
            or parsed.query
            or parsed.fragment
        ):
            raise ValueError("world URI must be one absolute local file URI")
        path = unquote(parsed.path)
    if not path.startswith("/") or "\x00" in path:
        raise ValueError("world URI path must be absolute and NUL-free")
    pure_path = PurePosixPath(path)
    if ".." in pure_path.parts:
        raise ValueError("world URI path must not contain parent traversal")
    normalized = str(pure_path)
    if normalized == "/" or not normalized.endswith((".usd", ".usda", ".usdc")):
        raise ValueError("world URI must identify one USD file")
    return "file:" + normalized


def world_readback_matches(
    world: object,
    *,
    expected_uri: str,
    expected_name: str,
) -> bool:
    """Compare exact normalized URI and exact configured world name."""

    try:
        observed_uri = normalize_file_world_uri(world.world_resource.uri)
        observed_name = str(world.name)
    except (AttributeError, TypeError, ValueError):
        return False
    return observed_uri == normalize_file_world_uri(expected_uri) and (
        observed_name == expected_name
    )


def select_service_name(
    graph: object,
    *,
    service_type: str,
) -> str | None:
    """Select one exact typed service deterministically from a bounded graph."""

    if not isinstance(graph, (list, tuple)) or len(graph) > _MAX_GRAPH_ROWS:
        return None
    candidates: list[str] = []
    for row in graph:
        if not isinstance(row, (list, tuple)) or len(row) != 2:
            return None
        name, types = row
        if (
            isinstance(name, str)
            and isinstance(types, (list, tuple))
            and service_type in types
        ):
            candidates.append(name)
    if not candidates:
        return None
    preferred_leaf = _PREFERRED_SERVICE_LEAVES.get(service_type, "")
    return sorted(
        set(candidates),
        key=lambda name: (
            0
            if name.lower().rstrip("/").endswith("/" + preferred_leaf)
            or name.lower().rstrip("/") == preferred_leaf
            else 1,
            name,
        ),
    )[0]


class IsaacSceneLoadBarrier(Node):
    """Load and verify one exact world before command infrastructure starts."""

    def __init__(
        self,
        *,
        parameter_overrides: Sequence[Parameter] | None = None,
    ) -> None:
        super().__init__(
            "manyforge_isaac_scene_load_barrier",
            parameter_overrides=parameter_overrides,
        )
        self.declare_parameter("world_uri", "")
        self.declare_parameter("expected_world_name", "")
        self.declare_parameter("require_existing_world", False)
        self.declare_parameter("passive_join", False)
        self.declare_parameter("startup_timeout_s", 120.0)
        self.declare_parameter("service_timeout_s", 30.0)
        self.declare_parameter("required_topics", Parameter.Type.STRING_ARRAY)
        self.world_uri = normalize_file_world_uri(
            str(self.get_parameter("world_uri").value)
        )
        self.expected_world_name = str(
            self.get_parameter("expected_world_name").value
        )
        self.require_existing_world = bool(
            self.get_parameter("require_existing_world").value
        )
        self.passive_join = bool(self.get_parameter("passive_join").value)
        self.startup_timeout_s = float(
            self.get_parameter("startup_timeout_s").value
        )
        self.service_timeout_s = float(
            self.get_parameter("service_timeout_s").value
        )
        self.required_topics = tuple(self.get_parameter("required_topics").value)
        if (
            not self.expected_world_name
            or len(self.expected_world_name.encode("utf-8")) > 1024
        ):
            raise ValueError("expected_world_name must be bounded non-empty text")
        for name, value, maximum in (
            ("startup_timeout_s", self.startup_timeout_s, 600.0),
            ("service_timeout_s", self.service_timeout_s, 120.0),
        ):
            if not math.isfinite(value) or not 0.1 <= value <= maximum:
                raise ValueError(f"{name} must be finite and in [0.1, {maximum}]")
        if (
            len(self.required_topics) > 64
            or any(
                not isinstance(topic, str)
                or not topic.startswith("/")
                or len(topic.encode("utf-8")) > 1024
                for topic in self.required_topics
            )
        ):
            raise ValueError("required_topics must be a bounded absolute-name list")

    def run(self) -> dict[str, Any]:
        startup_deadline = time.monotonic() + self.startup_timeout_s
        services = self._wait_for_services(startup_deadline)
        if self.passive_join:
            return self._run_passive_join(services, startup_deadline)
        startup_error: Exception | None = None
        observed_features: set[int] = set()
        observed_topics: list[str] = []
        playback_requested = False
        try:
            features = self._call(
                services["features"],
                GetSimulatorFeatures,
                GetSimulatorFeatures.Request(),
                deadline=startup_deadline,
            ).features
            required_features = {
                int(SimulatorFeatures.WORLD_LOADING),
                int(SimulatorFeatures.WORLD_INFO_GETTING),
                int(SimulatorFeatures.SIMULATION_STATE_GETTING),
                int(SimulatorFeatures.SIMULATION_STATE_SETTING),
            }
            observed_features = {int(value) for value in features.features}
            missing_features = sorted(required_features - observed_features)
            if missing_features:
                raise RuntimeError(
                    "Isaac simulation adapter lacks required features: "
                    + ",".join(str(value) for value in missing_features)
                )

            if not self.require_existing_world:
                request = LoadWorld.Request()
                request.world_resource.uri = self.world_uri
                request.world_resource.resource_string = ""
                request.fail_on_unsupported_element = True
                request.ignore_missing_or_unsupported_assets = False
                loaded = self._call(
                    services["load_world"],
                    LoadWorld,
                    request,
                    deadline=startup_deadline,
                )
                self._require_ok(loaded.result, operation="LoadWorld")
                if not world_readback_matches(
                    loaded.world,
                    expected_uri=self.world_uri,
                    expected_name=self.expected_world_name,
                ):
                    raise RuntimeError(
                        "LoadWorld response differs from the selected world"
                    )

            current = self._call(
                services["current_world"],
                GetCurrentWorld,
                GetCurrentWorld.Request(),
                deadline=startup_deadline,
            )
            self._require_ok(current.result, operation="GetCurrentWorld")
            if not world_readback_matches(
                current.world,
                expected_uri=self.world_uri,
                expected_name=self.expected_world_name,
            ):
                raise RuntimeError(
                    "GetCurrentWorld readback differs from the required existing world"
                    if self.require_existing_world
                    else "GetCurrentWorld readback differs from LoadWorld"
                )
            if self.require_existing_world and self._read_state(
                services["get_state"], deadline=startup_deadline
            ) != int(SimulationState.STATE_STOPPED):
                raise RuntimeError("required existing Isaac world is not stopped")

            # Retained Isaac graphs materialize their ROS publishers on the
            # first simulation tick. Probe while playing; command owners are
            # still held behind this process's successful exit.
            if self.required_topics:
                # Record possible effects before dispatch; a lost response
                # still requires stopped cleanup for our playback request.
                playback_requested = True
                self._set_state(
                    services["set_state"],
                    int(SimulationState.STATE_PLAYING),
                    operation="SetSimulationState(playing readiness probe)",
                    deadline=startup_deadline,
                )
                if self._read_state(
                    services["get_state"], deadline=startup_deadline
                ) != int(
                    SimulationState.STATE_PLAYING
                ):
                    raise RuntimeError(
                        "Isaac world did not enter playing readiness probe"
                    )
                observed_topics = self._wait_for_required_topics(
                    startup_deadline
                )
        except Exception as error:
            startup_error = error

        if (
            self.require_existing_world
            and startup_error is not None
            and not playback_requested
        ):
            # Identity or stopped-state refusal never loads or changes the
            # user's existing scene. Default load-mode cleanup is unchanged.
            raise startup_error

        # Default load mode always converges to stopped. Existing-world mode
        # does so after our possible playback effect, including readiness
        # failures. Cleanup receives a fresh bounded service timeout.
        try:
            self._set_state(
                services["set_state"],
                int(SimulationState.STATE_STOPPED),
                operation="SetSimulationState(stopped handoff)",
            )
            state = self._read_state(services["get_state"])
            if state != int(SimulationState.STATE_STOPPED):
                raise RuntimeError(
                    "Isaac world did not converge to stopped handoff state"
                )
        except Exception as stop_error:
            if startup_error is not None:
                raise RuntimeError(
                    "Isaac startup failed and stopped-state cleanup also "
                    f"failed: startup={startup_error}; cleanup={stop_error}"
                ) from stop_error
            raise
        if startup_error is not None:
            raise startup_error

        return {
            "schema_version": RESULT_SCHEMA,
            "world_name": self.expected_world_name,
            "world_uri": self.world_uri,
            "simulation_state": "stopped",
            "services": services,
            "required_topics": list(self.required_topics),
            "readiness_probe": {
                "mode": "playing_then_stopped",
                "observed_topics": observed_topics,
            },
            "features": sorted(observed_features),
        }

    def _run_passive_join(
        self, services: dict[str, str], deadline: float
    ) -> dict[str, Any]:
        # Q84: this branch has no load, playback or stopped-state cleanup,
        # including failures. Robot holding remains downstream in CCLC.
        features = self._call(
            services["features"],
            GetSimulatorFeatures,
            GetSimulatorFeatures.Request(),
            deadline=deadline,
        ).features
        observed_features = {int(value) for value in features.features}
        if int(SimulatorFeatures.WORLD_INFO_GETTING) not in observed_features:
            raise RuntimeError("Isaac simulation adapter lacks world-info feature")
        current = self._call(
            services["current_world"],
            GetCurrentWorld,
            GetCurrentWorld.Request(),
            deadline=deadline,
        )
        self._require_ok(current.result, operation="GetCurrentWorld")
        if not world_readback_matches(
            current.world,
            expected_uri=self.world_uri,
            expected_name=self.expected_world_name,
        ):
            raise RuntimeError("GetCurrentWorld readback differs from passive join world")
        observed_topics = self._wait_for_required_topics(deadline)
        return {
            "schema_version": RESULT_SCHEMA,
            "world_name": self.expected_world_name,
            "world_uri": self.world_uri,
            "simulation_state": "unchanged",
            "services": services,
            "required_topics": list(self.required_topics),
            "readiness_probe": {
                "mode": "passive",
                "observed_topics": observed_topics,
            },
            "features": sorted(observed_features),
        }

    def _wait_for_services(self, deadline: float) -> dict[str, str]:
        service_types = (
            {key: _SERVICE_TYPES[key] for key in ("features", "current_world")}
            if self.passive_join
            else _SERVICE_TYPES
        )
        last_services: dict[str, str] = {}
        while time.monotonic() < deadline:
            graph = self.get_service_names_and_types()
            selected = {
                key: select_service_name(graph, service_type=value)
                for key, value in service_types.items()
            }
            last_services = {
                key: value for key, value in selected.items() if value is not None
            }
            if len(last_services) == len(service_types):
                return last_services
            time.sleep(0.05)
        missing_services = sorted(set(service_types) - set(last_services))
        raise TimeoutError(
            "Isaac simulation-control readiness timed out; missing services="
            f"{missing_services}"
        )

    def _wait_for_required_topics(self, deadline: float) -> list[str]:
        last_topics: set[str] = set()
        required = set(self.required_topics)
        while time.monotonic() < deadline:
            topic_rows = self.get_topic_names_and_types()
            last_topics = {
                name
                for name, types in topic_rows
                if isinstance(types, (list, tuple)) and types
            }
            if required.issubset(last_topics):
                return sorted(required)
            time.sleep(0.05)
        missing_topics = sorted(required - last_topics)
        raise TimeoutError(
            "Isaac loaded-world topic readiness timed out; missing topics="
            f"{missing_topics}"
        )

    def _set_state(
        self,
        service_name: str,
        target_state: int,
        *,
        operation: str,
        deadline: float | None = None,
    ) -> None:
        request = SetSimulationState.Request()
        request.state.state = target_state
        changed = self._call(
            service_name,
            SetSimulationState,
            request,
            deadline=deadline,
        )
        code = int(changed.result.result)
        if code not in {
            int(changed.result.RESULT_OK),
            int(SetSimulationState.Response.ALREADY_IN_TARGET_STATE),
        }:
            raise RuntimeError(
                f"{operation} failed: {code}: {changed.result.error_message}"
            )

    def _call(
        self,
        service_name: str,
        service_type: type,
        request: object,
        *,
        deadline: float | None = None,
    ) -> object:
        call_deadline = time.monotonic() + self.service_timeout_s
        if deadline is not None:
            call_deadline = min(call_deadline, deadline)
        client = self.create_client(service_type, service_name)
        try:
            wait_timeout = call_deadline - time.monotonic()
            if wait_timeout <= 0.0:
                raise TimeoutError(
                    f"service deadline expired before call: {service_name}"
                )
            if not client.wait_for_service(timeout_sec=wait_timeout):
                raise TimeoutError(f"service unavailable: {service_name}")
            future = client.call_async(request)
            response_timeout = call_deadline - time.monotonic()
            if response_timeout <= 0.0:
                raise TimeoutError(
                    f"service deadline expired before response: {service_name}"
                )
            rclpy.spin_until_future_complete(
                self,
                future,
                timeout_sec=response_timeout,
            )
            if not future.done():
                future.cancel()
                raise TimeoutError(f"service response timed out: {service_name}")
            response = future.result()
            if response is None:
                raise RuntimeError(f"service returned no response: {service_name}")
            return response
        finally:
            self.destroy_client(client)

    def _read_state(
        self,
        service_name: str,
        *,
        deadline: float | None = None,
    ) -> int:
        response = self._call(
            service_name,
            GetSimulationState,
            GetSimulationState.Request(),
            deadline=deadline,
        )
        self._require_ok(response.result, operation="GetSimulationState")
        return int(response.state.state)

    @staticmethod
    def _require_ok(result: object, *, operation: str) -> None:
        if int(result.result) != int(result.RESULT_OK):
            raise RuntimeError(
                f"{operation} failed: {result.result}: {result.error_message}"
            )


def main() -> int:
    rclpy.init()
    node: IsaacSceneLoadBarrier | None = None
    try:
        node = IsaacSceneLoadBarrier()
        result = node.run()
        print(
            json.dumps(result, sort_keys=True, separators=(",", ":")),
            flush=True,
        )
        return 0
    except Exception as error:
        print(
            json.dumps(
                {
                    "schema_version": RESULT_SCHEMA,
                    "success": False,
                    "error": str(error) or type(error).__name__,
                },
                sort_keys=True,
                separators=(",", ":"),
            ),
            flush=True,
        )
        return 2
    finally:
        if node is not None:
            node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())
