"""A statically reviewed NRT node package, not a runtime plugin loader.

The existing Program adapter supplies a captured read-only model port. This
package compares its actual result; it owns no model, state cache or effect.
"""

from dataclasses import replace
from math import isfinite
from pathlib import Path

PACKAGE_ID = "manyforge_example_nodes"
NODE_ID = "example.check_frame_height"
IMPLEMENTATION_ID = "example_check_frame_height_v1"
PACKAGE_ROOT = Path(__file__).resolve().parent


class FrameHeightQuery:
    def __init__(self, model_port):
        self._model_port = model_port

    def invoke(self, node_id, identity, inputs):
        if node_id != NODE_ID:
            raise ValueError("example package received an unknown node")
        minimum = inputs.parameters["minimum_height_m"]
        maximum = inputs.parameters["maximum_height_m"]
        if (
            any(
                type(value) not in (int, float) or not isfinite(value)
                for value in (minimum, maximum)
            )
            or minimum > maximum
        ):
            raise ValueError("frame-height range must be finite and ordered")
        query = replace(
            inputs,
            parameters={key: inputs.parameters[key] for key in ("target", "frame")},
        )
        result = self._model_port.invoke("model.get_frame_pose", identity, query)
        if not result.succeeded:
            return result
        pose = result.output
        if (
            not isinstance(pose, dict)
            or pose.get("frame_ref") != {"kind": "cell_frame"}
            or not isinstance(pose.get("position_m"), list)
            or len(pose["position_m"]) != 3
        ):
            raise ValueError("model owner returned an invalid cell-frame pose")
        height = pose["position_m"][2]
        if type(height) not in (int, float) or not isfinite(height):
            raise ValueError("model owner returned an invalid frame height")
        matches = minimum <= height <= maximum
        return replace(
            result,
            succeeded=matches,
            output=None,
            reason_code=None if matches else "frame_height_outside_range",
            message=("" if matches else "Observed frame is outside the selected height range."),
        )
