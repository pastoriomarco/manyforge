"""Pure Lite6 controller-contract validation plus an unavailable live gate.

The retained functions project and compare an already selected controller
binding without ROS, subprocess, filesystem, or robot access.  The historical
module's ROS parameter client, runtime-binding loader, evidence writer, and CLI
runner were effectful I10 product behavior and are removed rather than hidden
behind the unavailable console.

Exact source is retained at ManyForge commit
``08222188293df8e12f90dc72e204c21f22f92ea1``, blob
``c9c5a5be68df40bcd023ea6cd3ffc71788e0e9db``.  Its three test cohorts are
blob ``c20a1e8e97339026baa437df9502ad1b04498a90``: projection/agreement and
drift rejection remain active below; live ROS lookup returns at I10/R against
the selected retained controller product.
"""

from __future__ import annotations

from datetime import datetime, timezone
import math
import sys
from typing import Any, Final


LIVE_CONTROLLER_PARAMETER_VALIDATION_SCHEMA: Final = (
    "manyforge.xarm_adapter.live_controller_parameter_validation.v1"
)
UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
RESTORATION_GATE: Final = "I10/R"
UNAVAILABLE_MESSAGE: Final = (
    f"{UNAVAILABLE_REASON_CODE}: live Lite6 controller-parameter lookup "
    f"requires the {RESTORATION_GATE} retained-product and ROS adapter gate"
)
HISTORICAL_SOURCE_BLOB: Final = (
    "c9c5a5be68df40bcd023ea6cd3ffc71788e0e9db"
)


def _mapping(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be a mapping")
    return value


def expected_live_controller_parameters(
    controller_binding: dict[str, Any],
) -> tuple[str, dict[str, bool | float]]:
    """Project a retained controller contract into ROS parameter names."""

    controller_name = controller_binding.get("trajectory_controller_name")
    if not isinstance(controller_name, str) or not controller_name.startswith(
        "/"
    ):
        raise ValueError(
            "controller binding trajectory_controller_name must be absolute"
        )
    joints = controller_binding.get("arm_joint_names")
    if (
        not isinstance(joints, list)
        or not joints
        or not all(isinstance(joint, str) and joint for joint in joints)
        or len(set(joints)) != len(joints)
    ):
        raise ValueError(
            "controller binding arm_joint_names must be unique strings"
        )
    state_interfaces = controller_binding.get("controller_state_interfaces")
    if not isinstance(state_interfaces, list) or "velocity" not in (
        state_interfaces
    ):
        raise ValueError("cancel deceleration requires controller velocity state")
    if controller_binding.get("decelerate_on_cancel") is not True:
        raise ValueError(
            "controller binding decelerate_on_cancel must be true"
        )
    limits = _mapping(
        controller_binding.get("max_deceleration_on_cancel_rad_s2"),
        "controller binding max_deceleration_on_cancel_rad_s2",
    )
    if set(limits) != set(joints):
        raise ValueError(
            "controller binding cancel-deceleration joints do not match"
        )

    expected: dict[str, bool | float] = {
        "constraints.decelerate_on_cancel": True,
    }
    for joint_name in joints:
        value = limits[joint_name]
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(value)
            or value <= 0.0
        ):
            raise ValueError(
                f"cancel deceleration for {joint_name} must be positive"
            )
        expected[
            f"constraints.{joint_name}.max_deceleration_on_cancel"
        ] = float(value)
    return controller_name, expected


def validate_live_controller_parameters(
    *,
    controller_binding: dict[str, Any],
    observed_parameters: dict[str, Any],
    validated_at: datetime,
) -> dict[str, Any]:
    """Compare supplied observations with the retained cancel contract."""

    controller_name, expected = expected_live_controller_parameters(
        controller_binding
    )
    mismatches: list[dict[str, Any]] = []
    for name, expected_value in expected.items():
        if name not in observed_parameters:
            mismatches.append(
                {
                    "parameter": name,
                    "expected": expected_value,
                    "observed": None,
                    "reason": "missing",
                }
            )
            continue
        observed_value = observed_parameters[name]
        if isinstance(expected_value, bool):
            matches = (
                isinstance(observed_value, bool)
                and observed_value is expected_value
            )
        else:
            matches = (
                not isinstance(observed_value, bool)
                and isinstance(observed_value, (int, float))
                and math.isfinite(float(observed_value))
                and math.isclose(
                    float(observed_value),
                    expected_value,
                    rel_tol=0.0,
                    abs_tol=1e-12,
                )
            )
        if not matches:
            mismatches.append(
                {
                    "parameter": name,
                    "expected": expected_value,
                    "observed": observed_value,
                    "reason": "value_mismatch",
                }
            )
    if mismatches:
        names = ", ".join(item["parameter"] for item in mismatches)
        raise ValueError(
            "loaded controller disagrees with the generated safety "
            f"contract: {names}"
        )

    if validated_at.tzinfo is None:
        raise ValueError("validated_at must be timezone-aware")
    return {
        "schema": LIVE_CONTROLLER_PARAMETER_VALIDATION_SCHEMA,
        "validatedAt": validated_at.astimezone(timezone.utc).isoformat(),
        "result": "pass",
        "controllerNode": controller_name,
        "controllerBindingGenerationId": controller_binding.get(
            "generation_id"
        ),
        "expectedParameters": expected,
        "observedParameters": {
            name: observed_parameters[name] for name in expected
        },
    }


def main() -> int:
    """Fail before arguments, ROS imports/queries, files, or robot access."""

    print(UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


if __name__ == "__main__":
    raise SystemExit(main())
