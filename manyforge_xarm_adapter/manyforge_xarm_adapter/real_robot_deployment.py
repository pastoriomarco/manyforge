"""Typed I10/R gate for the removed real-Lite6 deployment resolver.

The historical implementation did not contain a separable, owner-neutral
deployment generator.  Its complete resolution path coupled the removed
``manyforge.real_robot_cell.v1`` descriptor and experimental supervised
runtime binding to ``manyforge.deployment.v0`` ``robot_config``/``adapters``,
the old lifecycle-provider/session policy, and one singleton Lite6 address.
Keeping any of that resolver live would preserve the contract that I3 and I6
replace and would guess the I10 boundary before it exists.

The generic mechanics in that file (YAML/JSON reads, SHA-256 streaming, and
atomic writes) are not xArm generator semantics and already have responsible
owners elsewhere.  They are therefore not duplicated here.  Same-role
artifact compilation/retention returns at I3; component/resource addressing
returns at I6/N; lifecycle and admission semantics return at I7/L0 and I8/P5;
and the real Lite6 deployment client returns only at I10/R.

Exact reimplementation input is retained in Git:

* ManyForge commit ``08222188293df8e12f90dc72e204c21f22f92ea1``;
* tree ``bc749b9879d3c8f36fe7bf3036f9cc1f2ef1e60f``;
* this file's historical blob ``20b4a9d4d7930dc550f6931a4f32c17c585e8212``;
* the matching tests' blob ``bd97dc8bd64179f3db1fb299b97ea28a4064798e``;
* xarm_ros2 reference ``c3172e57c1c1bb2982fc008304ad6bc886357c87``.

The historical responsibilities map as follows: cell/template and retained
artifact closure to I3 and I6/N; runtime/provider/controller binding to I6/N,
I7/L0, and I8/P5; and real launch, topology, commissioning, evidence, and
product behavior to I10/R. Pneumatic actuation and full tool-program behavior
remain assigned to I11/T.
"""

from __future__ import annotations

import sys
from typing import Final, NoReturn


REAL_ROBOT_DEPLOYMENT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_real_robot_deployment_unavailable"
)
REAL_ROBOT_DEPLOYMENT_RESTORATION_GATES: Final = (
    "I3",
    "I6",
    "I7",
    "I8",
    "I10",
    "R",
)
HISTORICAL_SOURCE_COMMIT: Final = (
    "08222188293df8e12f90dc72e204c21f22f92ea1"
)
HISTORICAL_SOURCE_TREE: Final = (
    "bc749b9879d3c8f36fe7bf3036f9cc1f2ef1e60f"
)
HISTORICAL_SOURCE_BLOB: Final = (
    "20b4a9d4d7930dc550f6931a4f32c17c585e8212"
)
XARM_ROS2_REFERENCE_COMMIT: Final = (
    "c3172e57c1c1bb2982fc008304ad6bc886357c87"
)


class RealRobotDeploymentUnavailableError(RuntimeError):
    """Raised before old-schema, filesystem, ROS, process, or hardware access."""


def _raise_real_robot_deployment_unavailable() -> NoReturn:
    raise RealRobotDeploymentUnavailableError(
        f"{REAL_ROBOT_DEPLOYMENT_UNAVAILABLE_REASON_CODE}: the old singleton "
        "cell/runtime/deployment resolver was removed; compiler-owned "
        "artifacts and resource addressing must pass I3 and I6-I8 before the "
        "real-Lite6 client returns at I10/R"
    )


def resolve_real_robot_deployment(
    *_args: object,
    **_kwargs: object,
) -> NoReturn:
    """Reject the historical resolver without inspecting caller input."""

    _raise_real_robot_deployment_unavailable()


def main() -> int:
    """Return the stable unavailable result without parsing arguments."""

    print(
        f"{REAL_ROBOT_DEPLOYMENT_UNAVAILABLE_REASON_CODE}: "
        "real-Lite6 deployment returns at I10/R after I3 and I6-I8",
        file=sys.stderr,
    )
    return 78


__all__ = [
    "HISTORICAL_SOURCE_BLOB",
    "HISTORICAL_SOURCE_COMMIT",
    "HISTORICAL_SOURCE_TREE",
    "REAL_ROBOT_DEPLOYMENT_RESTORATION_GATES",
    "REAL_ROBOT_DEPLOYMENT_UNAVAILABLE_REASON_CODE",
    "RealRobotDeploymentUnavailableError",
    "XARM_ROS2_REFERENCE_COMMIT",
    "main",
    "resolve_real_robot_deployment",
]


if __name__ == "__main__":
    raise SystemExit(main())
