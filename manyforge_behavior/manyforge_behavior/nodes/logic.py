"""Logic / blackboard behavior nodes for ManyForge orchestration."""

from __future__ import annotations

import time
from typing import Any

import py_trees

from manyforge_behavior.blackboard_namespace import (
    read_blackboard_binding_value,
    register_blackboard_binding_read,
)
from manyforge_behavior.kernel_client import (
    GripperCommandRequest,
    GripperCommandState,
    KernelClient,
    resolve_gripper_commanded_state,
)


_DEFAULT_GRASP_EVIDENCE_LABELS: dict[str, str] = {
    "operator_confirm": "operator_confirm",
    "discrete_signal": "discrete_signal",
    "perception": "perception_grasp_confirmed",
    "force_or_current": "force_or_current",
}


class CheckKeyBoolValue(py_trees.behaviour.Behaviour):
    """Condition: succeeds if blackboard key holds the expected bool value."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._key = kwargs.get("key", "")
        self._expected = kwargs.get("expected_value", True)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        register_blackboard_binding_read(self.blackboard, self._key)

    def update(self) -> py_trees.common.Status:
        try:
            value = read_blackboard_binding_value(self.blackboard, self._key)
        except KeyError:
            return py_trees.common.Status.FAILURE
        if value == self._expected:
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.FAILURE


class SetKeyBoolValue(py_trees.behaviour.Behaviour):
    """Set a blackboard key to a boolean value. Always succeeds."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._key = kwargs.get("key", "")
        self._value = kwargs.get("value", True)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key=self._key, access=py_trees.common.Access.WRITE
        )

    def update(self) -> py_trees.common.Status:
        self.blackboard.set(self._key, self._value)
        return py_trees.common.Status.SUCCESS


class WaitForKeyBool(py_trees.behaviour.Behaviour):
    """Block until a blackboard key holds the expected boolean value."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._key = kwargs.get("key", "")
        self._expected = kwargs.get("expected_value", True)

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        register_blackboard_binding_read(self.blackboard, self._key)

    def update(self) -> py_trees.common.Status:
        try:
            value = read_blackboard_binding_value(self.blackboard, self._key)
        except KeyError:
            return py_trees.common.Status.RUNNING
        if value == self._expected:
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.RUNNING


class CheckSignalBool(CheckKeyBoolValue):
    """Condition: succeeds if a declared boolean signal matches the expected level."""


class SetSignalBool(SetKeyBoolValue):
    """Write a declared boolean signal level and succeed."""


class WaitForSignalBool(WaitForKeyBool):
    """Block until a declared boolean signal reaches the expected level."""


class WaitForSignalEdge(py_trees.behaviour.Behaviour):
    """Block until a declared boolean signal produces the configured edge."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._key = kwargs.get("key", "")
        self._edge = kwargs.get("edge", "rising")
        self._last_value: bool | None = None

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        register_blackboard_binding_read(self.blackboard, self._key)

    def initialise(self) -> None:
        self._last_value = None

    def update(self) -> py_trees.common.Status:
        try:
            value = read_blackboard_binding_value(self.blackboard, self._key)
        except KeyError:
            return py_trees.common.Status.RUNNING

        if not isinstance(value, bool):
            self.feedback_message = (
                f"Signal '{self._key}' must be boolean for edge detection, "
                f"got {type(value).__name__}"
            )
            return py_trees.common.Status.FAILURE

        if self._last_value is None:
            self._last_value = value
            return py_trees.common.Status.RUNNING

        previous = self._last_value
        self._last_value = value

        if self._edge == "rising":
            matched = (previous is False and value is True)
        elif self._edge == "falling":
            matched = (previous is True and value is False)
        else:
            matched = previous != value

        return (
            py_trees.common.Status.SUCCESS
            if matched
            else py_trees.common.Status.RUNNING
        )


class CheckGripperState(py_trees.behaviour.Behaviour):
    """Condition: succeeds if the declared gripper state matches the expectation."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._state_key = kwargs.get("state_key", "")
        self._expected_state = kwargs.get("expected_state", "closed")

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        register_blackboard_binding_read(self.blackboard, self._state_key)

    def update(self) -> py_trees.common.Status:
        try:
            value = read_blackboard_binding_value(self.blackboard, self._state_key)
        except KeyError:
            return py_trees.common.Status.FAILURE
        if not isinstance(value, bool):
            self.feedback_message = (
                f"Gripper state key '{self._state_key}' must be boolean, "
                f"got {type(value).__name__}"
            )
            return py_trees.common.Status.FAILURE
        expected = self._expected_state == "closed"
        return (
            py_trees.common.Status.SUCCESS
            if value == expected
            else py_trees.common.Status.FAILURE
        )


class SetGripperState(py_trees.behaviour.Behaviour):
    """Set the declared logical gripper state and succeed."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._state_key = kwargs.get("state_key", "")
        self._state = kwargs.get("state", "closed")

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key=self._state_key, access=py_trees.common.Access.WRITE
        )

    def update(self) -> py_trees.common.Status:
        self.blackboard.set(self._state_key, self._state == "closed")
        return py_trees.common.Status.SUCCESS


class CommandGripper(py_trees.behaviour.Behaviour):
    """Command an actuated gripper and block until the command completes."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._gripper_id = kwargs.get("gripper_id", "")
        self._position = kwargs.get("position")
        self._max_effort = kwargs.get("max_effort")
        raw_duration_s = kwargs.get("duration_s")
        self._duration_s_override = (
            None if raw_duration_s is None else float(raw_duration_s)
        )
        raw_timeout_s = kwargs.get("timeout_s")
        self._timeout_s_override = (
            None if raw_timeout_s is None else float(raw_timeout_s)
        )
        self._started = False
        self._deadline = 0.0

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(
            key="kernel", access=py_trees.common.Access.READ
        )
        self.blackboard.register_key(
            key="gripper_configs", access=py_trees.common.Access.READ
        )
        self.blackboard.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.WRITE
        )

    def initialise(self) -> None:
        self._started = False
        self._deadline = 0.0
        try:
            self.blackboard.set("gripper_fault_details", None)
        except (KeyError, AttributeError):
            pass

    def _record_gripper_fault(
        self,
        reason_code: str | None,
        message: str,
        *,
        config: dict[str, Any] | None = None,
        request: GripperCommandRequest | None = None,
        status: Any | None = None,
    ) -> None:
        # Cycle digest diagnostics read this key when the tree fails: the
        # gripper runtime has no kernel-snapshot error surface, so without it
        # the digest would carry only the prose feedback message.
        if not reason_code:
            return
        try:
            details: dict[str, Any] = {
                "reason_code": str(reason_code),
                "gripper_id": self._gripper_id,
                "surface": "command_gripper",
                "message": message,
            }
            semantic = self._semantic_fields(config or {})
            for key, value in semantic.items():
                if value not in (None, "", [], {}):
                    details[key] = value
            if request is not None:
                commanded_state = request.commanded_state or resolve_gripper_commanded_state(
                    target_position=request.target_position,
                    srdf_presets=request.srdf_presets,
                    open_state_ref=request.open_state_ref,
                    closed_state_ref=request.closed_state_ref,
                )
                details["commanded_state"] = commanded_state
                details["commanded_position"] = request.target_position
            if status is not None:
                for attr in (
                    "actuation_kind",
                    "state_authority",
                    "completion_policy",
                    "grasp_confirmation_policy",
                    "transport_kind",
                    "commanded_state",
                    "evidence_state",
                ):
                    value = getattr(status, attr, "")
                    if value not in (None, "", [], {}):
                        details[attr] = value
                evidence = getattr(status, "evidence", None)
                if evidence:
                    details["evidence"] = list(evidence)
            self.blackboard.set("gripper_fault_details", details)
        except (KeyError, AttributeError):
            pass

    def _load_gripper_config(self) -> dict[str, Any] | None:
        try:
            configs = self.blackboard.get("gripper_configs")
        except (KeyError, AttributeError):
            return None
        if not isinstance(configs, dict):
            return None
        config = configs.get(self._gripper_id)
        if not isinstance(config, dict):
            return None
        return config

    @staticmethod
    def _transport_kind(config: dict[str, Any]) -> str:
        transport = config.get("transport")
        if isinstance(transport, dict):
            kind = str(transport.get("kind", "")).strip()
            if kind:
                return kind
        command_mode = str(config.get("command_mode", "")).strip()
        if command_mode == "simulated_joint":
            return "native_kinematic"
        if command_mode == "ros_gripper_command":
            return "ros_gripper_command"
        return command_mode

    @classmethod
    def _semantic_fields(cls, config: dict[str, Any]) -> dict[str, Any]:
        command_mode = str(config.get("command_mode", "")).strip()
        transport_kind = cls._transport_kind(config)
        if command_mode == "simulated_joint":
            default_actuation_kind = "position_commanded"
            default_state_authority = "native_backend"
            default_completion_policy = "position_reached"
        else:
            default_actuation_kind = ""
            default_state_authority = ""
            default_completion_policy = ""
        srdf_presets = config.get("srdf_presets")
        if not isinstance(srdf_presets, dict):
            srdf_presets = {}
        return {
            "actuation_kind": str(
                config.get("actuation_kind") or default_actuation_kind
            ),
            "state_authority": str(
                config.get("state_authority") or default_state_authority
            ),
            "completion_policy": str(
                config.get("completion_policy") or default_completion_policy
            ),
            "grasp_confirmation_policy": str(
                config.get("grasp_confirmation_policy") or ""
            ),
            "transport_kind": transport_kind,
            "open_state_ref": str(config.get("open_state_ref") or ""),
            "closed_state_ref": str(config.get("closed_state_ref") or ""),
            "srdf_presets": {
                str(key): float(value)
                for key, value in srdf_presets.items()
                if isinstance(value, (int, float))
            },
        }

    @classmethod
    def _timeout_reason_code(cls, config: dict[str, Any]) -> str:
        semantic = cls._semantic_fields(config)
        if semantic["completion_policy"] in {
            "dwell_elapsed",
            "request_ack_then_dwell",
        }:
            return "gripper_dwell_timeout"
        return "gripper_result_timeout"

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        config = self._load_gripper_config()
        if config is None:
            self.feedback_message = (
                f"Gripper '{self._gripper_id}' is not configured in the deployment"
            )
            return py_trees.common.Status.FAILURE

        command_mode = str(config.get("command_mode", ""))
        if command_mode not in {
            "simulated_joint",
            "ros_gripper_command",
        }:
            self.feedback_message = (
                "command_gripper currently supports command_mode=simulated_joint "
                f"or ros_gripper_command, got '{command_mode}'"
            )
            self._record_gripper_fault(
                "invalid_gripper_semantics",
                self.feedback_message,
                config=config,
            )
            return py_trees.common.Status.FAILURE

        if self._position is None:
            self.feedback_message = "command_gripper.position is required"
            return py_trees.common.Status.FAILURE
        try:
            target_position = float(self._position)
        except (TypeError, ValueError):
            self.feedback_message = "command_gripper.position must be a number"
            return py_trees.common.Status.FAILURE
        try:
            max_effort = None if self._max_effort is None else float(self._max_effort)
        except (TypeError, ValueError):
            self.feedback_message = "command_gripper.max_effort must be a number"
            return py_trees.common.Status.FAILURE

        duration_s = (
            self._duration_s_override
            if self._duration_s_override is not None
            else float(config["motion_time_s"])
        )
        if duration_s < 0.0:
            self.feedback_message = "command_gripper.duration_s must be >= 0"
            return py_trees.common.Status.FAILURE
        timeout_s = (
            self._timeout_s_override
            if self._timeout_s_override is not None
            else duration_s + max(1.0, duration_s * 0.2)
        )
        if timeout_s < 0.0:
            self.feedback_message = (
                "command_gripper.timeout_s must be >= 0; 0 disables the timeout"
            )
            return py_trees.common.Status.FAILURE

        if not self._started:
            semantic = self._semantic_fields(config)
            request = GripperCommandRequest(
                gripper_id=self._gripper_id,
                group_id=str(config["group_id"]),
                joint_name=str(config["joint_name"]),
                target_position=target_position,
                duration_s=duration_s,
                max_effort=max_effort,
                **semantic,
            )
            result = kernel.command_gripper(
                request
            )
            self._started = True
            self._deadline = (
                float("inf") if timeout_s == 0.0 else time.monotonic() + timeout_s
            )
            if not result.accepted:
                self.feedback_message = result.error or "Gripper command was rejected"
                self._record_gripper_fault(
                    getattr(result, "reason_code", None),
                    self.feedback_message,
                    config=config,
                    request=request,
                )
                return py_trees.common.Status.FAILURE

        kernel.update_gripper_commands()
        status = kernel.get_gripper_status(self._gripper_id)
        if status.state == GripperCommandState.COMPLETED:
            return py_trees.common.Status.SUCCESS
        if status.state == GripperCommandState.FAILED:
            self.feedback_message = status.error or "Gripper command failed"
            self._record_gripper_fault(
                getattr(status, "reason_code", None),
                self.feedback_message,
                config=config,
                status=status,
            )
            return py_trees.common.Status.FAILURE

        now = time.monotonic()
        if now >= self._deadline:
            self.feedback_message = (
                f"Timed out waiting for gripper '{self._gripper_id}' to reach "
                f"position {target_position}"
            )
            self._record_gripper_fault(
                self._timeout_reason_code(config),
                self.feedback_message,
                config=config,
                status=status,
            )
            return py_trees.common.Status.FAILURE

        evidence = ",".join(getattr(status, "evidence", []) or [])
        authority = getattr(status, "state_authority", "") or "n/a"
        evidence_state = getattr(status, "evidence_state", "") or "n/a"
        self.feedback_message = (
            f"progress={status.progress:.2f} "
            f"position={status.current_position if status.current_position is not None else 'n/a'} "
            f"authority={authority} "
            f"evidence={evidence or 'n/a'} "
            f"evidence_state={evidence_state}"
        )
        return py_trees.common.Status.RUNNING


class ConfirmGripperGrasp(py_trees.behaviour.Behaviour):
    """Record verified grasp evidence for the latest gripper command."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._gripper_id = kwargs.get("gripper_id", "")
        self._policy = kwargs.get("grasp_confirmation_policy", "")
        self._source_key = kwargs.get("source_key", "")
        self._expected_value = kwargs.get("expected_value", True)
        self._evidence_label = kwargs.get("evidence_label", "")
        self._evidence_state = kwargs.get("evidence_state", "confirmed")

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        self.blackboard.register_key(key="kernel", access=py_trees.common.Access.READ)
        self.blackboard.register_key(
            key="gripper_configs", access=py_trees.common.Access.READ
        )
        self.blackboard.register_key(
            key="gripper_fault_details", access=py_trees.common.Access.WRITE
        )
        if self._source_key:
            register_blackboard_binding_read(self.blackboard, self._source_key)

    def initialise(self) -> None:
        try:
            self.blackboard.set("gripper_fault_details", None)
        except (KeyError, AttributeError):
            pass

    def _record_grasp_confirmation_fault(
        self,
        reason_code: str | None,
        message: str,
        *,
        policy: str,
        evidence_label: str,
    ) -> None:
        if not reason_code:
            return
        try:
            self.blackboard.set(
                "gripper_fault_details",
                {
                    "reason_code": str(reason_code),
                    "surface": "confirm_gripper_grasp",
                    "message": message,
                    "gripper_id": self._gripper_id,
                    "grasp_confirmation_policy": policy,
                    "evidence_label": evidence_label,
                    "evidence_state": self._evidence_state,
                    "source_key": self._source_key,
                },
            )
        except (KeyError, AttributeError):
            pass

    def _load_policy(self) -> str:
        policy = str(self._policy or "").strip()
        if policy:
            return policy
        try:
            configs = self.blackboard.get("gripper_configs")
        except (KeyError, AttributeError):
            return ""
        if not isinstance(configs, dict):
            return ""
        config = configs.get(self._gripper_id)
        if not isinstance(config, dict):
            return ""
        return str(config.get("grasp_confirmation_policy", "") or "").strip()

    def _source_is_confirmed(self, *, policy: str, evidence_label: str) -> bool:
        if not self._source_key:
            return True
        try:
            value = read_blackboard_binding_value(self.blackboard, self._source_key)
        except KeyError:
            self.feedback_message = (
                f"grasp confirmation source '{self._source_key}' is missing"
            )
            self._record_grasp_confirmation_fault(
                "stale_gripper_state",
                self.feedback_message,
                policy=policy,
                evidence_label=evidence_label,
            )
            return False
        if not isinstance(value, bool):
            self.feedback_message = (
                f"grasp confirmation source '{self._source_key}' must be boolean, "
                f"got {type(value).__name__}"
            )
            self._record_grasp_confirmation_fault(
                "invalid_gripper_semantics",
                self.feedback_message,
                policy=policy,
                evidence_label=evidence_label,
            )
            return False
        if value != bool(self._expected_value):
            self.feedback_message = (
                f"grasp confirmation source '{self._source_key}' is {value}, "
                f"expected {bool(self._expected_value)}"
            )
            self._record_grasp_confirmation_fault(
                "grasp_confirmation_lost",
                self.feedback_message,
                policy=policy,
                evidence_label=evidence_label,
            )
            return False
        return True

    def update(self) -> py_trees.common.Status:
        kernel: KernelClient = self.blackboard.kernel
        policy = self._load_policy()
        if not policy:
            self.feedback_message = (
                f"gripper '{self._gripper_id}' has no grasp_confirmation_policy"
            )
            self._record_grasp_confirmation_fault(
                "invalid_gripper_semantics",
                self.feedback_message,
                policy=policy,
                evidence_label=str(self._evidence_label or ""),
            )
            return py_trees.common.Status.FAILURE
        if policy == "none_open_loop":
            self.feedback_message = (
                "none_open_loop is an explicit assumption and cannot be "
                "converted into verified grasp evidence"
            )
            self._record_grasp_confirmation_fault(
                "invalid_gripper_semantics",
                self.feedback_message,
                policy=policy,
                evidence_label=str(self._evidence_label or ""),
            )
            return py_trees.common.Status.FAILURE
        evidence_label = str(self._evidence_label or "").strip()
        if not evidence_label:
            evidence_label = _DEFAULT_GRASP_EVIDENCE_LABELS.get(policy, "")
        if not self._source_is_confirmed(
            policy=policy,
            evidence_label=evidence_label,
        ):
            return py_trees.common.Status.FAILURE
        confirm = getattr(kernel, "confirm_gripper_grasp", None)
        if not callable(confirm):
            self.feedback_message = (
                "active backend does not support gripper grasp confirmation"
            )
            self._record_grasp_confirmation_fault(
                "invalid_gripper_semantics",
                self.feedback_message,
                policy=policy,
                evidence_label=evidence_label,
            )
            return py_trees.common.Status.FAILURE
        result = confirm(
            gripper_id=self._gripper_id,
            grasp_confirmation_policy=policy,
            evidence_label=evidence_label,
            evidence_state=str(self._evidence_state or "confirmed"),
        )
        if not result.accepted:
            self.feedback_message = result.error or "Gripper grasp confirmation failed"
            self._record_grasp_confirmation_fault(
                getattr(result, "reason_code", None),
                self.feedback_message,
                policy=policy,
                evidence_label=evidence_label,
            )
            return py_trees.common.Status.FAILURE

        self.feedback_message = (
            f"confirmed gripper '{self._gripper_id}' "
            f"policy={policy} evidence={evidence_label}"
        )
        return py_trees.common.Status.SUCCESS


class WaitForGripperState(py_trees.behaviour.Behaviour):
    """Block until the declared logical gripper state matches the expectation."""

    def __init__(self, name: str, **kwargs: Any):
        super().__init__(name=name)
        self._state_key = kwargs.get("state_key", "")
        self._expected_state = kwargs.get("expected_state", "closed")

    def setup(self, **kwargs: Any) -> None:
        self.blackboard = self.attach_blackboard_client()
        register_blackboard_binding_read(self.blackboard, self._state_key)

    def update(self) -> py_trees.common.Status:
        try:
            value = read_blackboard_binding_value(self.blackboard, self._state_key)
        except KeyError:
            return py_trees.common.Status.RUNNING
        if not isinstance(value, bool):
            self.feedback_message = (
                f"Gripper state key '{self._state_key}' must be boolean, "
                f"got {type(value).__name__}"
            )
            return py_trees.common.Status.FAILURE
        expected = self._expected_state == "closed"
        return (
            py_trees.common.Status.SUCCESS
            if value == expected
            else py_trees.common.Status.RUNNING
        )
