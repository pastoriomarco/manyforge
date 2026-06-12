"""Order-tolerant startup supervisor for the Isaac commanded-sim demo.

This node deliberately lives in ``manyforge_ros_bringup`` because it is demo
orchestration, not ManyForge core behavior.  It asks Composer to load the
Isaac light-scene program/deployment as soon as Composer is reachable so the
backend-authored scene and native viewer are available even before Isaac starts.
It keeps the arm command gate closed until Isaac Sim, the
topic_based_ros2_control arm controller, and the gripper action bridge are all
reachable; once they are ready it requests a Composer reset so Isaac receives
the configured home command through the normal execution backend path.
"""

from __future__ import annotations

import json
import time
import urllib.error
import urllib.request
from dataclasses import dataclass

import rclpy
from control_msgs.action import FollowJointTrajectory, GripperCommand
from control_msgs.msg import JointTrajectoryControllerState
from rclpy.action import ActionClient
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import JointState
from std_msgs.msg import Bool, String


@dataclass(frozen=True)
class _Readiness:
    ready: bool
    missing: list[str]


class IsaacDemoSupervisor(Node):
    """Monitor Isaac demo dependencies and trigger Composer load on readiness."""

    def __init__(self) -> None:
        super().__init__("manyforge_isaac_demo_supervisor")
        self.declare_parameter("composer_url", "http://127.0.0.1:9000")
        self.declare_parameter(
            "program_path",
            "/workspace/examples/isaac_manipulator_scene_light.program.yaml",
        )
        self.declare_parameter(
            "deployment_path",
            "/workspace/examples/isaac_manipulator_scene_light_ros2_control.deployment.yaml",
        )
        self.declare_parameter("isaac_joint_state_topic", "/isaac_joint_states")
        self.declare_parameter(
            "controller_state_topic", "/isaac_arm_controller/controller_state"
        )
        self.declare_parameter(
            "arm_action_name", "/isaac_arm_controller/follow_joint_trajectory"
        )
        self.declare_parameter(
            "gripper_action_name", "/robotiq_gripper_controller/gripper_cmd"
        )
        self.declare_parameter(
            # Supervisor-owned veto topic. Deliberately distinct from the
            # backend's enable topic: each gate authority is the SINGLE
            # writer of its own transient-local topic and the command-gate
            # node ANDs them (fail-closed). Sharing one topic between
            # several latched writers makes the effective gate value
            # reader/timing-dependent.
            "supervisor_enable_topic", "/manyforge/isaac_supervisor_command_enable"
        )
        self.declare_parameter(
            "status_topic", "/manyforge/isaac_demo_supervisor/status"
        )
        self.declare_parameter("auto_load", True)
        self.declare_parameter("poll_period_s", 1.0)
        self.declare_parameter("freshness_timeout_s", 3.0)
        self.declare_parameter("health_http_timeout_s", 1.0)
        self.declare_parameter("load_http_timeout_s", 30.0)
        self.declare_parameter("close_gate_after_not_ready_s", 1.0)
        self.declare_parameter("reload_after_disconnect_s", 3.0)
        self.declare_parameter("retry_load_period_s", 2.0)

        self._composer_url = str(self.get_parameter("composer_url").value).rstrip("/")
        self._program_path = str(self.get_parameter("program_path").value)
        self._deployment_path = str(self.get_parameter("deployment_path").value)
        self._auto_load = bool(self.get_parameter("auto_load").value)
        self._freshness_timeout_s = float(
            self.get_parameter("freshness_timeout_s").value
        )
        self._health_http_timeout_s = float(
            self.get_parameter("health_http_timeout_s").value
        )
        self._load_http_timeout_s = float(
            self.get_parameter("load_http_timeout_s").value
        )
        self._close_gate_after_not_ready_s = float(
            self.get_parameter("close_gate_after_not_ready_s").value
        )
        self._reload_after_disconnect_s = float(
            self.get_parameter("reload_after_disconnect_s").value
        )
        self._retry_load_period_s = float(
            self.get_parameter("retry_load_period_s").value
        )

        isaac_joint_state_topic = str(
            self.get_parameter("isaac_joint_state_topic").value
        )
        controller_state_topic = str(
            self.get_parameter("controller_state_topic").value
        )
        arm_action_name = str(self.get_parameter("arm_action_name").value)
        gripper_action_name = str(self.get_parameter("gripper_action_name").value)
        supervisor_enable_topic = str(
            self.get_parameter("supervisor_enable_topic").value
        )
        status_topic = str(self.get_parameter("status_topic").value)
        poll_period_s = float(self.get_parameter("poll_period_s").value)
        if poll_period_s <= 0.0:
            raise ValueError("poll_period_s must be > 0")

        gate_qos = QoSProfile(
            depth=1,
            durability=DurabilityPolicy.TRANSIENT_LOCAL,
            reliability=ReliabilityPolicy.RELIABLE,
        )
        self._command_gate_pub = self.create_publisher(
            Bool, supervisor_enable_topic, gate_qos
        )
        self._status_pub = self.create_publisher(String, status_topic, 1)
        self.create_subscription(
            JointState, isaac_joint_state_topic, self._on_isaac_joint_state, 10
        )
        self.create_subscription(
            JointTrajectoryControllerState,
            controller_state_topic,
            self._on_controller_state,
            10,
        )
        self._arm_client = ActionClient(self, FollowJointTrajectory, arm_action_name)
        self._gripper_client = ActionClient(self, GripperCommand, gripper_action_name)

        self._last_isaac_joint_state_s: float | None = None
        self._last_controller_state_s: float | None = None
        self._load_complete = False
        self._last_load_attempt_s = 0.0
        self._last_ready = False
        self._not_ready_since_s: float | None = None
        self._home_reset_complete = False
        self._last_status_key: tuple[bool, tuple[str, ...], bool, bool] | None = None

        self._publish_command_gate(False)
        self.create_timer(poll_period_s, self._tick)
        self.get_logger().info(
            f"Isaac demo supervisor active: auto_load={self._auto_load} "
            f"composer={self._composer_url}"
        )

    def _on_isaac_joint_state(self, _msg: JointState) -> None:
        self._last_isaac_joint_state_s = time.monotonic()

    def _on_controller_state(self, _msg: JointTrajectoryControllerState) -> None:
        self._last_controller_state_s = time.monotonic()

    def _tick(self) -> None:
        readiness = self._readiness()
        now = time.monotonic()
        composer_ready = "composer" not in readiness.missing

        if self._auto_load and not self._load_complete and composer_ready:
            if now - self._last_load_attempt_s >= self._retry_load_period_s:
                self._last_load_attempt_s = now
                if self._load_composer_artifacts():
                    self._load_complete = True
                    self._home_reset_complete = False

        if not readiness.ready:
            if self._last_ready:
                self._not_ready_since_s = now
                self.get_logger().warning(
                    "Isaac demo became not ready: "
                    f"{', '.join(readiness.missing)}"
                )
            elif self._not_ready_since_s is None:
                self._not_ready_since_s = now
            if (
                self._not_ready_since_s is not None
                and now - self._not_ready_since_s >= self._close_gate_after_not_ready_s
            ):
                self._publish_command_gate(False)
            if (
                self._load_complete
                and "composer" in readiness.missing
                and self._not_ready_since_s is not None
                and now - self._not_ready_since_s >= self._reload_after_disconnect_s
            ):
                self._load_complete = False
                self._home_reset_complete = False
            self._last_ready = False
            self._publish_status(readiness)
            return

        self._not_ready_since_s = None
        if not self._last_ready:
            self.get_logger().info("Isaac demo dependencies are ready")
            # Withdraw the supervisor veto on recovery. Before the topic
            # split this node never re-enabled and silently relied on the
            # other gate publishers' latches winning the race — the exact
            # split-brain that stranded the gate closed across restarts.
            self._publish_command_gate(True)
        self._last_ready = True

        if self._auto_load and self._load_complete and not self._home_reset_complete:
            if now - self._last_load_attempt_s >= self._retry_load_period_s:
                self._last_load_attempt_s = now
                if self._reset_composer_cycle():
                    self._home_reset_complete = True
        self._publish_status(readiness)

    def _readiness(self) -> _Readiness:
        missing: list[str] = []
        now = time.monotonic()
        if not self._composer_healthy():
            missing.append("composer")
        if not self._is_fresh(self._last_isaac_joint_state_s, now):
            missing.append("isaac_joint_states")
        if not self._is_fresh(self._last_controller_state_s, now):
            missing.append("controller_state")
        if not self._arm_client.wait_for_server(timeout_sec=0.0):
            missing.append("arm_action")
        if not self._gripper_client.wait_for_server(timeout_sec=0.0):
            missing.append("gripper_action")
        return _Readiness(ready=not missing, missing=missing)

    def _is_fresh(self, timestamp_s: float | None, now_s: float) -> bool:
        return (
            timestamp_s is not None
            and now_s - timestamp_s <= self._freshness_timeout_s
        )

    def _composer_healthy(self) -> bool:
        try:
            with urllib.request.urlopen(
                f"{self._composer_url}/api/health",
                timeout=self._health_http_timeout_s,
            ) as response:
                return 200 <= response.status < 300
        except (OSError, urllib.error.URLError):
            return False

    def _load_composer_artifacts(self) -> bool:
        payload = json.dumps(
            {
                "path": self._program_path,
                "deploymentPath": self._deployment_path,
                "forceDiscardOverrides": True,
                "deferInitialRuntimeReset": True,
            }
        ).encode("utf-8")
        request = urllib.request.Request(
            f"{self._composer_url}/api/program/load",
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(
                request, timeout=self._load_http_timeout_s
            ) as response:
                ok = 200 <= response.status < 300
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            self.get_logger().warning(
                f"Composer artifact load failed: HTTP {exc.code} {detail}"
            )
            return False
        except (OSError, urllib.error.URLError) as exc:
            self.get_logger().warning(f"Composer artifact load failed: {exc}")
            return False
        if ok:
            self.get_logger().info("Composer Isaac program/deployment loaded")
        return ok

    def _reset_composer_cycle(self) -> bool:
        request = urllib.request.Request(
            f"{self._composer_url}/api/cycle/reset",
            data=b"",
            method="POST",
        )
        try:
            with urllib.request.urlopen(
                request, timeout=self._load_http_timeout_s
            ) as response:
                ok = 200 <= response.status < 300
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", errors="replace")
            self.get_logger().warning(
                f"Composer cycle reset failed: HTTP {exc.code} {detail}"
            )
            return False
        except (OSError, urllib.error.URLError) as exc:
            self.get_logger().warning(f"Composer cycle reset failed: {exc}")
            return False
        if ok:
            self.get_logger().info("Composer cycle reset completed for Isaac home command")
        return ok

    def _publish_command_gate(self, enabled: bool) -> None:
        msg = Bool()
        msg.data = enabled
        self._command_gate_pub.publish(msg)

    def _publish_status(self, readiness: _Readiness) -> None:
        status_key = (
            readiness.ready,
            tuple(readiness.missing),
            self._load_complete,
            self._home_reset_complete,
        )
        msg = String()
        status_payload = self._status_payload(readiness)
        msg.data = json.dumps(status_payload, sort_keys=True)
        self._status_pub.publish(msg)
        self._post_runtime_status(status_payload)
        if status_key != self._last_status_key:
            self.get_logger().info(f"Isaac demo supervisor status: {msg.data}")
            self._last_status_key = status_key

    def _status_payload(self, readiness: _Readiness) -> dict:
        missing = set(readiness.missing)
        execution_ready = readiness.ready and self._load_complete and self._home_reset_complete
        if execution_ready:
            runtime_readiness = "ready"
            user_message = "Isaac Sim execution path is ready."
        elif not self._load_complete:
            runtime_readiness = "waiting"
            user_message = "Loading Composer program/deployment for Isaac preview."
        elif not readiness.ready:
            runtime_readiness = "waiting"
            user_message = "Rendering/planning are available; execution is waiting for Isaac readiness."
        else:
            runtime_readiness = "waiting"
            user_message = "Isaac is ready; resetting simulator to the backend-authored home pose."

        def component(component_id: str, label: str, kind: str, ready: bool, detail: str) -> dict:
            return {
                "id": component_id,
                "label": label,
                "kind": kind,
                "status": "ready" if ready else "waiting",
                "requiredFor": ["execute"],
                "detail": detail,
            }

        return {
            "ready": readiness.ready,
            "missing": readiness.missing,
            "auto_load": self._auto_load,
            "load_complete": self._load_complete,
            "home_reset_complete": self._home_reset_complete,
            "readiness": runtime_readiness,
            "canExecute": execution_ready,
            "userMessage": user_message,
            "components": [
                component(
                    "isaac_supervisor",
                    "Isaac supervisor",
                    "simulator",
                    True,
                    "Order-tolerant demo supervisor is running",
                ),
                component(
                    "isaac_stream",
                    "Isaac joint stream",
                    "simulator",
                    "isaac_joint_states" not in missing,
                    "Fresh /isaac_joint_states received"
                    if "isaac_joint_states" not in missing
                    else "Waiting for /isaac_joint_states",
                ),
                component(
                    "controller_state",
                    "Arm controller state",
                    "controller",
                    "controller_state" not in missing,
                    "Fresh controller state received"
                    if "controller_state" not in missing
                    else "Waiting for /isaac_arm_controller/controller_state",
                ),
                component(
                    "arm_controller",
                    "Arm trajectory action",
                    "controller",
                    "arm_action" not in missing,
                    "FollowJointTrajectory action available"
                    if "arm_action" not in missing
                    else "Waiting for arm FollowJointTrajectory action",
                ),
                component(
                    "gripper_action",
                    "Gripper action",
                    "gripper",
                    "gripper_action" not in missing,
                    "GripperCommand action available"
                    if "gripper_action" not in missing
                    else "Waiting for GripperCommand action",
                ),
                {
                    "id": "command_gate",
                    "label": "Isaac command gate",
                    "kind": "safety",
                    "status": "ready" if execution_ready else "waiting",
                    "requiredFor": ["execute"],
                    "detail": "Open after home reset" if execution_ready else "Closed until Isaac is ready and reset home completes",
                },
            ],
        }

    def _post_runtime_status(self, payload: dict) -> None:
        request = urllib.request.Request(
            f"{self._composer_url}/api/runtime/external/isaac_demo_supervisor",
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(
                request, timeout=self._health_http_timeout_s
            ) as response:
                if not (200 <= response.status < 300):
                    self.get_logger().debug(
                        f"Runtime status post returned HTTP {response.status}"
                    )
        except (OSError, urllib.error.URLError):
            # Composer readiness is tracked separately; avoid log spam while it
            # is still starting or restarting.
            return


def main() -> None:
    rclpy.init()
    node = IsaacDemoSupervisor()
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
