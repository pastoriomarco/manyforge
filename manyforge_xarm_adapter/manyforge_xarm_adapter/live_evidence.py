"""Pure Lite6 observation-evidence planning and reduction.

Probe construction, bounded-output classification, and textual postconditions
remain same-role Python qualification logic.  The historical subprocess
runner, directory creation, raw/evidence writers, and product CLI are removed
rather than left callable below an unavailable fence.  The read-only graph
predicate intentionally records the former topology for I10 baseline
reconstruction; I10 must bind it to the selected measured-state owner before
claiming the reduced final topology.

Exact source is retained at ManyForge commit
``08222188293df8e12f90dc72e204c21f22f92ea1``, blob
``7d58213cd9a8e27422700516095da97b3b19dd3b``.  Six historical test cohorts
are blob ``2a4ca4e3bd6cb5df70c6b0d1ba63f540d589c909``: optional versus required
timeouts, manifest-selected controller probes, unexpected graph participants,
claimed interfaces, and namespace-sensitive observation.  Their effect-free
logic remains below; live capture and durable evidence return at I10/R.
"""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import sys
from typing import Any, Final


EVIDENCE_SCHEMA: Final = "manyforge.xarm_adapter.live_evidence.v1"
MAX_PROBE_OUTPUT_BYTES: Final = 1024 * 1024
UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATE: Final = "I10/R"
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: live Lite6 observation capture requires the "
    f"{RESTORATION_GATE} retained-product and ROS adapter gate"
)
HISTORICAL_SOURCE_BLOB: Final = (
    "7d58213cd9a8e27422700516095da97b3b19dd3b"
)


@dataclass(frozen=True)
class Probe:
    """One bounded, read-only ROS graph or state observation request."""

    label: str
    argv: tuple[str, ...]
    postcondition: str
    required: bool = True


@dataclass(frozen=True)
class CommandOutcome:
    """Supplied result from a future bounded probe runner."""

    return_code: int
    stdout: bytes
    stderr: bytes
    timed_out: bool
    duration_ms: float


def _sections(
    manifest: dict[str, object],
) -> tuple[dict[str, object], dict[str, object]]:
    binding = manifest.get("binding")
    controller = manifest.get("controller_binding")
    if not isinstance(binding, dict):
        raise ValueError("binding manifest has no endpoint binding")
    if not isinstance(controller, dict):
        raise ValueError("binding manifest has no controller binding")
    return binding, controller


def _namespaced_node(binding: dict[str, object], name: str) -> str:
    namespace = str(binding.get("ros_namespace", "")).strip("/")
    return "/" + "/".join(part for part in (namespace, name) if part)


def read_only_probes(manifest: dict[str, object]) -> tuple[Probe, ...]:
    """Build the historical command-free observation plan without running it."""

    binding, _ = _sections(manifest)
    diagnostics = str(binding["diagnostics_topic"])
    robot_state = str(binding["robot_state_topic"])
    joint_state = str(binding["joint_state_topic"])
    validated = str(binding["validated_joint_state_topic"])
    return (
        Probe("nodes", ("ros2", "node", "list", "--no-daemon"), "read_only_nodes"),
        Probe("actions", ("ros2", "action", "list"), "empty"),
        Probe(
            "services",
            ("ros2", "service", "list", "--no-daemon"),
            "no_xarm_command_services",
        ),
        Probe(
            "diagnostics",
            ("ros2", "topic", "echo", "--once", diagnostics),
            "read_only_diagnostics",
        ),
        Probe(
            "robot_state",
            ("ros2", "topic", "echo", "--once", robot_state),
            "nonempty",
        ),
        Probe(
            "joint_state",
            ("ros2", "topic", "echo", "--once", joint_state),
            "nonempty",
        ),
        Probe(
            "validated_joint_state",
            ("ros2", "topic", "echo", "--once", validated),
            "nonempty",
            required=False,
        ),
    )


def supervised_preflight_probes(
    manifest: dict[str, object],
) -> tuple[Probe, ...]:
    """Build controller/hardware observation probes without executing them."""

    _, controller = _sections(manifest)
    manager = str(controller["controller_manager"])
    activity = str(controller["controller_manager_activity_topic"])
    return (
        Probe("nodes", ("ros2", "node", "list", "--no-daemon"), "nonempty"),
        Probe(
            "hardware_components",
            (
                "ros2",
                "control",
                "list_hardware_components",
                "--controller-manager",
                manager,
            ),
            "active_production_hardware_component",
        ),
        Probe(
            "hardware_interfaces",
            (
                "ros2",
                "control",
                "list_hardware_interfaces",
                "--controller-manager",
                manager,
            ),
            "claimed_production_interfaces",
        ),
        Probe(
            "controllers",
            (
                "ros2",
                "control",
                "list_controllers",
                "--controller-manager",
                manager,
                "--verbose",
            ),
            "active_production_controller",
        ),
        Probe(
            "controller_activity",
            ("ros2", "topic", "echo", "--once", activity),
            "active_controller_activity",
        ),
    )


def evaluate_postcondition(
    name: str,
    output: str,
    manifest: dict[str, object],
) -> tuple[bool, str]:
    """Evaluate one historical observation postcondition without effects."""

    binding, controller = _sections(manifest)
    if name == "nonempty":
        return (bool(output.strip()), "output_observed")
    if name == "empty":
        return (not output.strip(), "no_endpoint_observed")
    if name == "read_only_nodes":
        expected = {
            str(binding["driver_node"]),
            _namespaced_node(binding, "lite6_robot_state_publisher"),
            _namespaced_node(binding, "manyforge_xarm_read_only_observer"),
        }
        actual = {line.strip() for line in output.splitlines() if line.strip()}
        missing = sorted(expected - actual)
        unexpected = sorted(actual - expected)
        return (
            not missing and not unexpected,
            (
                "expected_read_only_graph"
                if not missing and not unexpected
                else f"missing={missing};unexpected={unexpected}"
            ),
        )
    if name == "no_xarm_command_services":
        forbidden_tokens = (
            "clean_error",
            "clean_warn",
            "motion_enable",
            "set_mode",
            "set_state",
            "set_servo_angle",
            "set_servo_cartesian",
            "set_joint_velocity",
            "set_position",
            "set_tool_position",
            "set_cgpio",
            "gripper",
            "controller_manager",
        )
        found = sorted(
            token for token in forbidden_tokens if token in output.lower()
        )
        allowed_nodes = {
            str(binding["driver_node"]),
            _namespaced_node(binding, "lite6_robot_state_publisher"),
            _namespaced_node(binding, "manyforge_xarm_read_only_observer"),
        }
        allowed_leaves = {
            "describe_parameters",
            "get_logger_levels",
            "get_parameter_types",
            "get_parameters",
            "get_type_description",
            "list_parameters",
            "set_logger_levels",
            "set_parameters",
            "set_parameters_atomically",
        }
        unexpected = []
        for service in (
            line.strip() for line in output.splitlines() if line.strip()
        ):
            parent, separator, leaf = service.rpartition("/")
            if (
                not separator
                or parent not in allowed_nodes
                or leaf not in allowed_leaves
            ):
                unexpected.append(service)
        unexpected.sort()
        passed = not found and not unexpected
        return (
            passed,
            (
                "only_introspection_services"
                if passed
                else f"forbidden={found};unexpected={unexpected}"
            ),
        )
    if name == "read_only_diagnostics":
        required = (
            "key: access_mode",
            "value: read_only",
            "key: motion_admission",
            "value: closed",
            "key: command_endpoints",
            "value: suppressed",
        )
        missing = [token for token in required if token not in output]
        return (
            not missing,
            "read_only_fail_closed" if not missing else f"missing={missing}",
        )
    if name == "active_production_hardware_component":
        expected = str(controller["hardware_component_name"])
        component_start = output.find(expected)
        component_block = output[component_start:] if component_start >= 0 else ""
        next_component = component_block.find("Hardware Component", len(expected))
        if next_component >= 0:
            component_block = component_block[:next_component]
        active = component_start >= 0 and "active" in component_block.lower()
        return (
            active,
            (
                "active_hardware_component_observed"
                if active
                else f"missing_or_inactive={expected}"
            ),
        )
    if name == "active_production_controller":
        expected = str(controller["trajectory_controller_name"]).rsplit(
            "/", 1
        )[-1]
        expected_type = str(controller["trajectory_controller_type"])
        matched = [
            line.lower() for line in output.splitlines() if expected in line
        ]
        active = any("active" in line for line in matched)
        exact_type = expected_type in output
        return (
            active and exact_type,
            (
                "active_exact_trajectory_controller_observed"
                if active and exact_type
                else f"missing_or_inactive={expected};type_observed={exact_type}"
            ),
        )
    if name == "active_controller_activity":
        expected = str(controller["trajectory_controller_name"]).rsplit(
            "/", 1
        )[-1]
        active = expected in output and "state: active" in output.lower()
        return (
            active,
            (
                "active_controller_activity_observed"
                if active
                else f"missing_or_inactive={expected}"
            ),
        )
    if name == "claimed_production_interfaces":
        expected_commands = controller["claimed_command_interface_names"]
        expected_states = controller["state_interface_names"]
        if not isinstance(expected_commands, list) or not isinstance(
            expected_states, list
        ):
            raise ValueError("controller interface expectations must be lists")
        lower_output = output.lower()
        command_heading = lower_output.find("command interfaces")
        state_heading = lower_output.find("state interfaces")
        if (
            command_heading < 0
            or state_heading < 0
            or state_heading <= command_heading
        ):
            return (False, "command_or_state_interface_section_missing")
        command_lines = [
            line.strip()
            for line in output[command_heading:state_heading].splitlines()
        ]
        state_lines = [
            line.strip() for line in output[state_heading:].splitlines()
        ]
        missing_commands = [
            value
            for value in expected_commands
            if not any(
                str(value) in line and "[claimed]" in line.lower()
                for line in command_lines
            )
        ]
        missing_states = [
            value
            for value in expected_states
            if not any(str(value) in line for line in state_lines)
        ]
        passed = not missing_commands and not missing_states
        return (
            passed,
            (
                "all_interfaces_observed_and_commands_claimed"
                if passed
                else (
                    f"unclaimed_or_missing_commands={missing_commands};"
                    f"missing_states={missing_states}"
                )
            ),
        )
    raise ValueError(f"unsupported evidence postcondition: {name}")


def assess_probe(
    *,
    probe: Probe,
    outcome: CommandOutcome,
    manifest: dict[str, object],
) -> dict[str, object]:
    """Reduce a supplied bounded outcome without running or writing anything."""

    combined = outcome.stdout + (
        b"\n--- stderr ---\n" + outcome.stderr if outcome.stderr else b""
    )
    truncated = len(combined) > MAX_PROBE_OUTPUT_BYTES
    retained = combined[:MAX_PROBE_OUTPUT_BYTES]
    decoded = outcome.stdout.decode("utf-8", errors="replace")
    postcondition_passed, reason = evaluate_postcondition(
        probe.postcondition,
        decoded,
        manifest,
    )
    passed = (
        postcondition_passed
        and outcome.return_code == 0
        and not outcome.timed_out
        and not truncated
    )
    return {
        "schema": EVIDENCE_SCHEMA,
        "label": probe.label,
        "argv": list(probe.argv),
        "required": probe.required,
        "timeout_is_success": False,
        "return_code": outcome.return_code,
        "timed_out": outcome.timed_out,
        "duration_ms": outcome.duration_ms,
        "output_sha256": hashlib.sha256(retained).hexdigest(),
        "output_size_bytes": len(retained),
        "output_truncated": truncated,
        "postcondition": probe.postcondition,
        "postcondition_passed": passed,
        "postcondition_reason": reason,
    }


def main() -> int:
    """Fail before arguments, subprocesses, ROS, files, or robot access."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
