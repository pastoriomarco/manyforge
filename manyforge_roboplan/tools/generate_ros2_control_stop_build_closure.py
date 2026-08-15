#!/usr/bin/env python3
"""Generate the exact ROS execution Stop build and runner closures.

The I8 native generator owns the closed File API hashing implementation. This
profile wrapper changes only the case-contract identity; passing this wrapper
as an explicit runner source input makes both layers part of the ROS runner's
attested closure without rotating the frozen native runner contract.
"""

from __future__ import annotations

import generate_native_stop_build_closure as closure


closure.CASE_API = "manyforge.ros2_control_stop_qualification_case_contract.v1"
closure.CASE_RUNNER_ID = "manyforge.ros2_control_stop_qualification_runner.v1"


if __name__ == "__main__":
    try:
        raise SystemExit(closure.main())
    except closure.ClosureError as error:
        raise SystemExit(
            f"ROS Stop build-closure generation failed: {error}"
        ) from error
