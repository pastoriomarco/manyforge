"""Tests for the runtime-requirements / compatibility scaffold.

These helpers are pure and ROS-free (the deployment argument is read via
attribute-or-mapping, so we use dicts and `SimpleNamespace` fakes here). The
typed exception that the kernel factory raises is also checked here so the
structured 409 contract surfaced by `routes_program.load_program` cannot
silently drift away from its JSON shape.
"""
from __future__ import annotations

from types import SimpleNamespace

import pytest

from manyforge_composer.backend.runtime_requirements import (
    CompatibilityResultDTO,
    DeploymentRequirementsDTO,
    RuntimeCapabilitiesDTO,
    RuntimeFactoryRequirementsError,
    assess_runtime_capabilities,
    check_compatibility,
    derive_deployment_requirements,
    detect_current_lane,
)


# ---------------------------------------------------------------------------
# detect_current_lane


class TestDetectCurrentLane:
    @pytest.mark.parametrize(
        "value, expected",
        [
            ("manyforge-only", "manyforge-only"),
            ("isaac-sim", "isaac-sim"),
            ("observed-ros2", "observed-ros2"),
            ("ISAAC-SIM", "isaac-sim"),  # case insensitive
            ("  manyforge-only  ", "manyforge-only"),  # whitespace stripped
        ],
    )
    def test_known_lane_values_resolve(self, monkeypatch, value, expected):
        monkeypatch.setenv("MANYFORGE_LANE", value)
        assert detect_current_lane() == expected

    def test_unset_returns_unknown(self, monkeypatch):
        monkeypatch.delenv("MANYFORGE_LANE", raising=False)
        assert detect_current_lane() == "unknown"

    def test_bogus_value_returns_unknown(self, monkeypatch):
        monkeypatch.setenv("MANYFORGE_LANE", "production")
        # Honest about what it can't tell — don't guess.
        assert detect_current_lane() == "unknown"


# ---------------------------------------------------------------------------
# derive_deployment_requirements


class TestDeriveDeploymentRequirements:
    def test_none_deployment_is_unknown(self):
        req = derive_deployment_requirements(None)
        assert req.execution_backend == "unknown"
        assert req.ros_adapter_required is False
        assert req.isaac_supervisor_required is False

    def test_explicit_requires_block_wins_over_derivation(self):
        # If a deployment declares `requires:` directly, that intent must NOT
        # be overridden by inferred fields. Documented contract.
        deployment = {
            "execution_adapter_id": "execution.roboplan_native",  # would derive "native"
            "requires": {
                "execution_backend": "ros2_control",
                "ros_adapter_required": True,
                "isaac_supervisor_required": True,
                "action_servers": ["/some/action"],
                "topics": ["/some/topic"],
            },
        }
        req = derive_deployment_requirements(deployment)
        assert req.execution_backend == "ros2_control"
        assert req.ros_adapter_required is True
        assert req.isaac_supervisor_required is True
        assert req.action_servers == ("/some/action",)
        assert req.topics == ("/some/topic",)

    def test_ros2_control_inferred_from_adapter_id(self):
        deployment = SimpleNamespace(
            execution_adapter_id="ros2_control",
            robot_config=None,
        )
        req = derive_deployment_requirements(deployment)
        assert req.execution_backend == "ros2_control"
        assert req.ros_adapter_required is True

    def test_native_backend_does_not_require_ros(self):
        deployment = SimpleNamespace(
            execution_adapter_id="execution.roboplan_native",
            robot_config=None,
        )
        req = derive_deployment_requirements(deployment)
        assert req.execution_backend == "execution.roboplan_native"
        assert req.ros_adapter_required is False

    def test_action_servers_and_topics_collected_from_robot_config(self):
        deployment = SimpleNamespace(
            execution_adapter_id="ros2_control",
            robot_config=SimpleNamespace(
                arm_action_name="/isaac_arm_controller/follow_joint_trajectory",
                gripper_action_name="/robotiq_gripper_controller/gripper_cmd",
                isaac_joint_state_topic="/isaac_joint_states",
                controller_state_topic="/isaac_arm_controller/controller_state",
            ),
        )
        req = derive_deployment_requirements(deployment)
        assert "/isaac_arm_controller/follow_joint_trajectory" in req.action_servers
        assert "/robotiq_gripper_controller/gripper_cmd" in req.action_servers
        assert "/isaac_joint_states" in req.topics
        assert "/isaac_arm_controller/controller_state" in req.topics
        # Isaac topics on a ros2_control deployment imply the supervisor.
        assert req.isaac_supervisor_required is True

    def test_ros2_control_without_isaac_topics_does_not_require_supervisor(self):
        deployment = SimpleNamespace(
            execution_adapter_id="ros2_control",
            robot_config=SimpleNamespace(
                arm_action_name="/some_real_robot/follow_joint_trajectory",
                gripper_action_name=None,
                isaac_joint_state_topic=None,
                controller_state_topic=None,
            ),
        )
        req = derive_deployment_requirements(deployment)
        assert req.ros_adapter_required is True
        assert req.isaac_supervisor_required is False


# ---------------------------------------------------------------------------
# assess_runtime_capabilities


class TestAssessRuntimeCapabilities:
    def test_pass_through(self):
        cap = assess_runtime_capabilities(
            has_kernel_factory=True,
            ros_adapter_available=True,
            isaac_supervisor_available=False,
            lane="isaac-sim",
        )
        assert cap.has_kernel_factory is True
        assert cap.ros_adapter_available is True
        assert cap.isaac_supervisor_available is False
        assert cap.lane == "isaac-sim"

    def test_lane_defaults_to_detect_current_lane(self, monkeypatch):
        monkeypatch.setenv("MANYFORGE_LANE", "manyforge-only")
        cap = assess_runtime_capabilities(
            has_kernel_factory=False,
            ros_adapter_available=False,
            isaac_supervisor_available=False,
        )
        assert cap.lane == "manyforge-only"


# ---------------------------------------------------------------------------
# check_compatibility


class TestCheckCompatibility:
    def _ros2_req(self, supervisor_required: bool = True) -> DeploymentRequirementsDTO:
        return DeploymentRequirementsDTO(
            execution_backend="ros2_control",
            ros_adapter_required=True,
            isaac_supervisor_required=supervisor_required,
        )

    def _cap(
        self,
        *,
        lane: str = "isaac-sim",
        has_kf: bool = True,
        ros: bool = True,
        sup: bool = True,
    ) -> RuntimeCapabilitiesDTO:
        return RuntimeCapabilitiesDTO(
            lane=lane,
            ros_adapter_available=ros,
            isaac_supervisor_available=sup,
            has_kernel_factory=has_kf,
        )

    def test_compatible_when_everything_available(self):
        result = check_compatibility(self._ros2_req(), self._cap())
        assert result.status == "compatible"
        assert result.missing == ()
        assert result.expected_lane == "isaac-sim"

    def test_wrong_lane_when_kernel_factory_missing(self):
        # Fundamental lane mismatch: ros2_control on a manyforge-only image.
        result = check_compatibility(
            self._ros2_req(),
            self._cap(lane="manyforge-only", has_kf=False, ros=False, sup=False),
        )
        assert result.status == "wrong_lane"
        assert "ros_adapter" in result.missing
        assert result.expected_lane == "isaac-sim"
        assert result.current_lane == "manyforge-only"
        assert result.suggested_command is not None
        assert "isaac-sim" in result.suggested_command

    def test_missing_components_when_supervisor_unavailable(self):
        # Kernel factory is there but the Isaac supervisor isn't streaming.
        result = check_compatibility(
            self._ros2_req(supervisor_required=True),
            self._cap(sup=False),
        )
        assert result.status == "missing_components"
        assert "isaac_supervisor" in result.missing
        assert result.suggested_command is not None

    def test_native_deployment_is_always_compatible(self):
        req = DeploymentRequirementsDTO(execution_backend="execution.roboplan_native")
        result = check_compatibility(
            req,
            self._cap(lane="manyforge-only", has_kf=False, ros=False, sup=False),
        )
        # No ros2_control requirement means manyforge-only is fine.
        assert result.status == "compatible"

    def test_ros2_control_without_supervisor_requirement_is_compatible(self):
        req = self._ros2_req(supervisor_required=False)
        result = check_compatibility(req, self._cap(sup=False))
        # Kernel factory + ros adapter present, supervisor not required.
        assert result.status == "compatible"


# ---------------------------------------------------------------------------
# RuntimeFactoryRequirementsError


class TestRuntimeFactoryRequirementsError:
    def test_to_detail_round_trips_all_fields(self):
        exc = RuntimeFactoryRequirementsError(
            reason_code="ros_adapter_missing",
            message="ROS adapter not in this image",
            missing=("ros_adapter", "isaac_supervisor"),
            expected_lane="isaac-sim",
            current_lane="manyforge-only",
            suggested_command="scripts/launch.sh --lane isaac-sim restart",
            original_error="ImportError: No module named 'manyforge_ros'",
        )
        detail = exc.to_detail()
        assert detail["reason_code"] == "ros_adapter_missing"
        assert detail["message"] == "ROS adapter not in this image"
        assert detail["missing"] == ["ros_adapter", "isaac_supervisor"]
        assert detail["expected_lane"] == "isaac-sim"
        assert detail["current_lane"] == "manyforge-only"
        assert detail["suggested_command"].startswith("scripts/launch.sh")
        assert detail["original_error"] == "ImportError: No module named 'manyforge_ros'"

    def test_extends_runtime_error_so_legacy_catches_still_work(self):
        # The kernel factory raise sites are caught BOTH as RuntimeError
        # (legacy except clauses) and as the typed class (structured surfacing).
        # The structured catch must come first; the RuntimeError fallback
        # must not be removed because other paths still raise plain ones.
        exc = RuntimeFactoryRequirementsError(
            reason_code="x",
            message="x",
        )
        assert isinstance(exc, RuntimeError)


# ---------------------------------------------------------------------------
# DTO round-trip safety


class TestDTORoundTrips:
    def test_deployment_requirements_to_dict(self):
        req = DeploymentRequirementsDTO(
            execution_backend="ros2_control",
            ros_adapter_required=True,
            isaac_supervisor_required=False,
            action_servers=("/a",),
            topics=("/t",),
        )
        d = req.to_dict()
        assert d["execution_backend"] == "ros2_control"
        assert d["ros_adapter_required"] is True
        assert d["action_servers"] == ["/a"]
        assert d["topics"] == ["/t"]

    def test_runtime_capabilities_to_dict(self):
        cap = RuntimeCapabilitiesDTO(
            lane="isaac-sim",
            ros_adapter_available=True,
            isaac_supervisor_available=False,
            has_kernel_factory=True,
        )
        d = cap.to_dict()
        assert d["lane"] == "isaac-sim"
        assert d["has_kernel_factory"] is True

    def test_compatibility_result_to_dict(self):
        result = CompatibilityResultDTO(
            status="missing_components",
            missing=("isaac_supervisor",),
            expected_lane="isaac-sim",
            current_lane="isaac-sim",
            suggested_command="scripts/launch.sh restart",
        )
        d = result.to_dict()
        assert d["status"] == "missing_components"
        assert d["missing"] == ["isaac_supervisor"]
