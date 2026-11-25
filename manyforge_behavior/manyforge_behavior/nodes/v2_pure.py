"""Pure v2 py_trees adapters over Program-run orchestration values."""

from __future__ import annotations

from collections.abc import Mapping
import time
from typing import Any

import py_trees

from manyforge_behavior.failure_details import (
    NodeFailureOrigin,
    exception_failure_details,
)
from manyforge_behavior.geometry import rpy_deg_to_xyzw
from manyforge_behavior.nodes.geometry import (
    offset_pose,
    position_within_bounds,
    quaternion_angular_distance_rad,
    translation_distance_m,
)
from manyforge_behavior.nodes.interruption import InterruptedOutcome
from manyforge_behavior.nodes.logic import boolean_condition_matches, compare_values
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopState
from manyforge_behavior.orchestration_ports import (
    FrameTransformError,
    FrameTransformPort,
    NodeAttemptIdentity,
    NodeAttemptSource,
)
from manyforge_behavior.program_clock import AttemptTime, ProgramClock
from manyforge_behavior.run_sender import RunRequest, RunSender
from manyforge_behavior.orchestration_values import (
    ExpressionEvaluationError,
    NodeAttemptInputResolver,
    ProgramValueError,
    PublicationReceipt,
    input_failure_reason,
)

# The node itself failed: its inputs, an expression, a value it read.
_NODE_FAILURE = NodeFailureOrigin("program_node")
# The frame transform owner refused or could not answer: the model-query
# owner (`model_queries`) that serves the `model.kinematics` API.
_FRAME_TRANSFORM_FAILURE = NodeFailureOrigin("owner", owner_api="model.kinematics")


class _TransformPending(Exception):
    """The node's frame transform is on the run's sender: it is RUNNING."""


def _read_failure_reason(error: ProgramValueError) -> str:
    if isinstance(error, ExpressionEvaluationError):
        return ExpressionEvaluationError.reason_code
    return "blackboard_read_failed"


class PublishedWriteHold:
    """A node whose write of a published Program value returns its result
    only after the host answered that value (OD-213 part 3).

    The store commits a write at once and returns the receipt of its
    publication (``PublicationReceipt``; None when nothing is sent). The node
    keeps the receipt of its update's writes in ``_written``; ``_after_write``
    holds the update's result (RUNNING) until the receipt is answered and
    ``_held_write_result`` returns it on a later tick. A Stop (INVALID) of a
    node holding such a result keeps it (``_abandon_write``) and the Forced
    Program End ends the attempt with it (``settle_abandoned_request``), never
    `interrupted`: a committed value is never withdrawn. The tick never waits.
    """

    _written: PublicationReceipt | None = None
    _held_write: tuple[PublicationReceipt, py_trees.common.Status] | None = None
    _abandoned_write: tuple[PublicationReceipt, py_trees.common.Status] | None = None

    def _held_write_result(self) -> py_trees.common.Status | None:
        """The result held for a write's publication: RUNNING until the host
        answered it, then the result; None when nothing is held."""

        held = self._held_write
        if held is None:
            return None
        receipt, status = held
        if not receipt.answered:
            return py_trees.common.Status.RUNNING
        self._held_write = None
        return status

    def _after_write(self, status: py_trees.common.Status) -> py_trees.common.Status:
        """Return ``status`` once this update's writes were answered by the
        host: until then the node is RUNNING (``_held_write_result``), as
        when the write sent its frame itself (OD-213 part 3)."""

        receipt = self._written
        if receipt is None or status is py_trees.common.Status.RUNNING:
            return status  # A later result waits for it (frames keep order).
        self._written = None
        if receipt.answered:
            return status
        self._held_write = (receipt, status)
        return py_trees.common.Status.RUNNING

    def _abandon_write(self, new_status: py_trees.common.Status) -> bool:
        """``terminate``: a Stop (INVALID) of a node holding a write's result
        keeps it; the write completes and ends the attempt with that result
        (``settle_abandoned_request``), never `interrupted`. True then."""

        held, self._held_write = self._held_write, None
        if held is None or new_status is not py_trees.common.Status.INVALID:
            return False
        self._abandoned_write = held
        return True

    def _forget_written(self) -> None:
        """A new attempt: nothing of the previous one's writes is held."""

        self._written = None
        self._held_write = None
        self._abandoned_write = None

    def settle_abandoned_request(self) -> bool:
        """Forced Program End: a stopped attempt's written value ends it with
        its result once the host answered it. True once nothing is pending."""

        held = self._abandoned_write
        if held is None:
            return True
        receipt, status = held
        if not receipt.answered:
            return False
        self._abandoned_write = None
        self.status = status
        return True


class _SnapshotAdapter(PublishedWriteHold, py_trees.behaviour.Behaviour):
    """Capture one immutable parameter snapshot at node-attempt start."""

    node_id: str
    # Keys read into the one attempt snapshot; a missing key fails the attempt.
    blackboard_read_parameters: tuple[str, ...] = ()

    def __init__(
        self,
        *,
        name: str,
        parameters: Mapping[str, Any],
        value_sources: Mapping[str, Mapping[str, Any]],
        input_resolver: NodeAttemptInputResolver,
        attempt_source: NodeAttemptSource,
        frame_transforms: FrameTransformPort | None = None,
        run_sender: RunSender | None = None,
    ) -> None:
        super().__init__(name=name)
        self._node_instance_id = name
        self._parameters = dict(parameters)
        self._value_sources = {
            key: dict(source) for key, source in value_sources.items()
        }
        self._input_resolver = input_resolver
        self._attempt_source = attempt_source
        self._frame_transforms = frame_transforms
        # The run's sender (OD-380 D2): the attempt's one frame transform
        # leaves the tick and is read on a later tick; None: in the tick.
        self._sender = run_sender
        self._transform: RunRequest | None = None
        self._identity: NodeAttemptIdentity | None = None
        self._inputs: Mapping[str, Any] | None = None
        self._blackboard_inputs: Mapping[str, Any] | None = None
        self._input_error: str | None = None
        # Read by the Program run for the failed tip; set only for failures
        # that carry a stable reason (inputs, expression evaluation, an
        # owner's frame transform).
        self.failure_reason_code: str | None = None
        self.failure_message = ""
        # Bounded structured details (`bounded_failure_details`), for example
        # an input's problem, path and source; None when there are none.
        self.failure_details: Mapping[str, Any] | None = None
        # Where it failed: kind and owner API; the run names the instance.
        self.failure_origin: NodeFailureOrigin | None = None

    def _fail(
        self,
        reason_code: str,
        message: str,
        origin: NodeFailureOrigin,
        details: Mapping[str, Any] | None = None,
    ) -> py_trees.common.Status:
        self.failure_reason_code = reason_code
        self.failure_message = message
        self.failure_details = details
        self.failure_origin = origin
        self.feedback_message = f"{reason_code}: {message}"
        return py_trees.common.Status.FAILURE

    def _withdraw_transform(self) -> None:
        if self._transform is not None:
            self._transform.withdraw()  # A sent read completes; nobody reads it.
            self._transform = None

    def terminate(self, new_status: py_trees.common.Status) -> None:
        self._withdraw_transform()
        self._abandon_write(new_status)

    def initialise(self) -> None:
        self._withdraw_transform()
        self._forget_written()
        self._identity = None
        self._inputs = None
        self._blackboard_inputs = None
        self._input_error = None
        self.failure_reason_code = None
        self.failure_message = ""
        self.failure_details = None
        self.failure_origin = None
        try:
            self._identity = self._attempt_source.begin(self._node_instance_id)
            snapshot = self._input_resolver.snapshot(
                node_id=self.node_id,
                node_instance_id=self._node_instance_id,
                parameters=self._parameters,
                value_sources=self._value_sources,
                blackboard_read_parameters=self.blackboard_read_parameters,
            )
            self._inputs = snapshot.parameters
            self._blackboard_inputs = snapshot.blackboard
        except (KeyError, ProgramValueError, TypeError, ValueError) as exc:
            # The inputs fail before the node's own work starts, whatever
            # outcomes its catalog entry declares.
            self._input_error = str(exc)
            self._fail(
                input_failure_reason(exc),
                str(exc),
                _NODE_FAILURE,
                exception_failure_details(exc),
            )

    def _resolved_inputs(self) -> Mapping[str, Any] | None:
        return self._inputs if self._input_error is None else None

    def _write(self, key: str, value: Any) -> None:
        """Write one key, attributed to this run, node and attempt."""

        assert self._identity is not None  # Set with the resolved inputs.
        receipt = self._input_resolver.value_store.write(
            key,
            value,
            metadata={
                "node_id": self.node_id,
                "node_instance_id": self._node_instance_id,
                "program_run_id": self._identity.program_run_id,
                "node_attempt_id": self._identity.node_attempt_id,
            },
        )
        if receipt is not None:
            self._written = receipt

    def _expressed_in(
        self, pose: Mapping[str, Any], frame: Mapping[str, Any]
    ) -> Mapping[str, Any]:
        """``pose`` stated in ``frame``: unchanged when it already is, otherwise
        one owner-proven transform (the frames are never composed here).

        With the run's sender the transform is sent and `_TransformPending`
        raised until its answer, which a later tick reads (the attempt's
        inputs, so the request, stay the same). A Halt withdraws it while it
        is not sent; the node sends it again when ticked after Execute.
        """

        if dict(pose["frame_ref"]) == dict(frame):
            return pose
        if self._frame_transforms is None or self._identity is None:
            raise FrameTransformError(
                "frame_transform_unavailable",
                "this Program run has no native frame transform owner",
            )
        if self._sender is None:
            return self._frame_transforms.express_pose(self._identity, pose, frame).pose
        request = self._transform
        if request is None or request.withdrawn:
            frames, identity = self._frame_transforms, self._identity
            self._transform = self._sender.submit(
                lambda: frames.express_pose(identity, pose, frame)
            )
            raise _TransformPending()
        if not request.answered:
            raise _TransformPending()
        self._transform = None
        return request.result().pose


class LogicCheckCondition(_SnapshotAdapter):
    """Succeed when the attempt's one boolean condition is true."""

    node_id = "logic.check_condition"

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        return (
            py_trees.common.Status.SUCCESS
            if boolean_condition_matches(inputs["condition"], expected=True)
            else py_trees.common.Status.FAILURE
        )


class LogicSetValue(_SnapshotAdapter):
    node_id = "logic.set_value"

    def update(self) -> py_trees.common.Status:
        held = self._held_write_result()
        if held is not None:
            return held
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        try:
            self._write(inputs["key"], inputs["value"])
        except ProgramValueError as exc:
            self.feedback_message = f"blackboard_write_failed: {exc}"
            return py_trees.common.Status.FAILURE
        return self._after_write(py_trees.common.Status.SUCCESS)


class _KeyComparison(_SnapshotAdapter):
    def _matches(self, inputs: Mapping[str, Any], actual: object) -> bool | None:
        try:
            return compare_values(
                actual,
                inputs["operator"],
                inputs["value"],
                value_type=self._input_resolver.key_type(inputs["key"]),
                tolerance=inputs.get("tolerance"),
            )
        except (ProgramValueError, TypeError, ValueError) as exc:
            self.feedback_message = f"comparison_failed: {exc}"
            return None


class LogicCheckValue(_KeyComparison):
    node_id = "logic.check_value"
    blackboard_read_parameters = ("key",)

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None or self._blackboard_inputs is None:
            return py_trees.common.Status.FAILURE
        if self._matches(inputs, self._blackboard_inputs["key"]):
            return py_trees.common.Status.SUCCESS
        return py_trees.common.Status.FAILURE


class _ProgramTimed(InterruptedOutcome, _SnapshotAdapter):
    """A cancellable leaf that measures its attempt on the run's Program clock.

    Program time stands still while the run is halted, so a Halt never
    counts and Execute continues the same attempt. `held_policy_parameter`
    names the node's `pause | count` choice: with `pause` the time an
    operator Stop actually holds the node is left out too; with `count`, or
    for a node with no hold scope (it keeps running through a Stop), held
    time counts. A Program Stop, Reset, cancel or a parent ending the node
    reports `interrupted`, never a failure."""

    held_policy_parameter: str

    def __init__(
        self,
        *,
        program_clock: ProgramClock,
        operator_stop_state: OperatorMotionStopState | None = None,
        operator_stop_hold_scope: str | None = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(**kwargs)
        self._attempt_time = AttemptTime(
            program_clock,
            operator_stop_state=operator_stop_state,
            operator_stop_hold_scope=operator_stop_hold_scope,
        )

    def initialise(self) -> None:
        self._clear_interruption()
        super().initialise()
        if self._inputs is not None:
            self._attempt_time.start(
                exclude_held=self._inputs.get(self.held_policy_parameter) == "pause"
            )

    def terminate(self, new_status: py_trees.common.Status) -> None:
        self._record_interruption(new_status)


# A watched key without a value yet.
_EMPTY_KEY: Any = object()


class LogicWaitForValue(_ProgramTimed, _KeyComparison):
    """Fixed inputs come from the attempt snapshot; the watched key is read
    fresh on every tick, or the wait could never see the write it awaits.
    Without `timeout_s` it waits until the value matches or the run stops.
    `timeout_s` counts Program time from the attempt's start, with held time
    as `timeout_while_held` selects (`pause` by default)."""

    node_id = "logic.wait_for_value"
    held_policy_parameter = "timeout_while_held"
    # The watched key and the value this attempt last read (none while the
    # key is empty), with the UTC time it was first read: display only.
    wait_observation: Mapping[str, Any] | None = None

    def initialise(self) -> None:
        self.wait_observation = None
        super().initialise()

    def _observe(self, key: str, value: Any = _EMPTY_KEY) -> None:
        previous = self.wait_observation
        if previous is not None and previous["key"] == key:
            last = previous.get("value", _EMPTY_KEY)
            if last is value or (type(last) is type(value) and last == value):
                return
        observation = {"kind": "key", "key": key, "observed_at_utc_ns": time.time_ns()}
        if value is not _EMPTY_KEY:
            observation["value"] = value
        self.wait_observation = observation

    def _timed_out(self, inputs: Mapping[str, Any]) -> bool:
        timeout_s = inputs.get("timeout_s")
        return timeout_s is not None and self._attempt_time.elapsed() >= float(timeout_s)

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        try:
            # A computed watched key is recomputed from a fresh snapshot.
            actual = self._input_resolver.value_store.read(inputs["key"])
        except KeyError:
            # The key is still empty; keep waiting.
            self._observe(inputs["key"])
        except ProgramValueError as exc:
            return self._fail(
                _read_failure_reason(exc),
                str(exc),
                _NODE_FAILURE,
                exception_failure_details(exc),
            )
        else:
            self._observe(inputs["key"], actual)
            matched = self._matches(inputs, actual)
            if matched is None:
                return py_trees.common.Status.FAILURE
            if matched:
                return py_trees.common.Status.SUCCESS
        if self._timed_out(inputs):
            self.feedback_message = (
                f"wait_timed_out: {inputs['key']!r} did not match within "
                f"{float(inputs['timeout_s']):g} s"
            )
            return py_trees.common.Status.FAILURE
        return py_trees.common.Status.RUNNING


class ProgramTimer(_ProgramTimed):
    """`timer`: a delay of `duration_s` seconds of Program time, with held
    time as `duration_while_held` selects (`pause` by default). It succeeds
    on the first tick at or after the end of the delay. The wait itself
    cannot fail; like any node, an attempt whose inputs cannot be resolved
    fails before the wait starts (`node_input_unavailable`)."""

    node_id = "timer"
    held_policy_parameter = "duration_while_held"

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        duration_s = float(inputs["duration_s"])
        elapsed = self._attempt_time.elapsed()
        if elapsed >= duration_s:
            return py_trees.common.Status.SUCCESS
        self.feedback_message = f"{duration_s - elapsed:.3f} s remaining"
        return py_trees.common.Status.RUNNING


_IDENTITY_XYZW = (0.0, 0.0, 0.0, 1.0)


class GeometryOffsetPose(_SnapshotAdapter):
    """Offset one pose; the result stays in the input pose's frame.

    Without `offset_frame` the offset is measured along the pose's own axes.
    With it, along that declared frame's axes, whose orientation relative to
    the pose's frame is read once from the owner when the frames differ.
    """

    node_id = "geometry.offset_pose"

    def update(self) -> py_trees.common.Status:
        held = self._held_write_result()
        if held is not None:
            return held
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        try:
            pose = inputs["pose"]
            frame = inputs.get("offset_frame")
            axes = (
                None
                if frame is None
                else self._expressed_in(
                    {
                        "frame_ref": frame,
                        "position_m": [0.0, 0.0, 0.0],
                        "orientation": {"xyzw": list(_IDENTITY_XYZW)},
                    },
                    pose["frame_ref"],
                )["orientation"]["xyzw"]
            )
            result = offset_pose(pose, inputs["offset"], axes_xyzw=axes)
            self._write(inputs["output_key"], result)
        except _TransformPending:
            return py_trees.common.Status.RUNNING
        except FrameTransformError as exc:
            return self._fail(
                exc.reason_code, str(exc), _FRAME_TRANSFORM_FAILURE, exc.details
            )
        except (ProgramValueError, TypeError, ValueError) as exc:
            self.feedback_message = f"pose_offset_failed: {exc}"
            return py_trees.common.Status.FAILURE
        return self._after_write(py_trees.common.Status.SUCCESS)


class GeometryCheckPoseBounds(_SnapshotAdapter):
    """The pose is expressed in the box frame `frame_ref`, then compared."""

    node_id = "geometry.check_pose_bounds"

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        try:
            pose = self._expressed_in(inputs["pose"], inputs["frame_ref"])
            inside = position_within_bounds(
                pose["position_m"],
                inputs["min_position_m"],
                inputs["max_position_m"],
                inclusive=inputs["inclusive"],
            )
        except _TransformPending:
            return py_trees.common.Status.RUNNING
        except FrameTransformError as exc:
            return self._fail(
                exc.reason_code, str(exc), _FRAME_TRANSFORM_FAILURE, exc.details
            )
        except (TypeError, ValueError) as exc:
            self.feedback_message = f"pose_bounds_failed: {exc}"
            return py_trees.common.Status.FAILURE
        return py_trees.common.Status.SUCCESS if inside else py_trees.common.Status.FAILURE


def _stored_pose_parts(value: object) -> tuple[object, object]:
    """Position and xyzw orientation of a stored pose in either form."""

    if not isinstance(value, Mapping):
        raise ProgramValueError("stored pose is not a mapping")
    orientation = value.get("orientation")
    if isinstance(orientation, Mapping) and "xyzw" in orientation:
        quaternion = orientation["xyzw"]
    elif isinstance(orientation, Mapping) and "rpy_deg" in orientation:
        quaternion = rpy_deg_to_xyzw(orientation["rpy_deg"])
    else:
        raise ProgramValueError("stored pose has no xyzw or rpy_deg orientation")
    return value.get("position_m"), quaternion


class GeometryCheckPoseDistance(_SnapshotAdapter):
    """Pose B is expressed in pose A's frame, then both are compared."""

    node_id = "geometry.check_pose_distance"
    blackboard_read_parameters = ("pose_key_a", "pose_key_b")

    def update(self) -> py_trees.common.Status:
        inputs = self._resolved_inputs()
        if inputs is None:
            return py_trees.common.Status.FAILURE
        try:
            if self._blackboard_inputs is None:
                raise ProgramValueError("pose snapshot is absent")
            first = self._blackboard_inputs["pose_key_a"]
            second = self._expressed_in(
                self._blackboard_inputs["pose_key_b"], first["frame_ref"]
            )
            first_position, first_orientation = _stored_pose_parts(first)
            second_position, second_orientation = _stored_pose_parts(second)
            translation = translation_distance_m(
                first_position,  # type: ignore[arg-type]
                second_position,  # type: ignore[arg-type]
            )
            rotation = quaternion_angular_distance_rad(
                first_orientation,  # type: ignore[arg-type]
                second_orientation,  # type: ignore[arg-type]
            )
        except _TransformPending:
            return py_trees.common.Status.RUNNING
        except FrameTransformError as exc:
            return self._fail(
                exc.reason_code, str(exc), _FRAME_TRANSFORM_FAILURE, exc.details
            )
        except (KeyError, ProgramValueError, TypeError, ValueError) as exc:
            self.feedback_message = f"pose_comparison_failed: {exc}"
            return py_trees.common.Status.FAILURE
        return (
            py_trees.common.Status.SUCCESS
            if translation <= inputs["translation_threshold_m"]
            and rotation <= inputs["rotation_threshold_rad"]
            else py_trees.common.Status.FAILURE
        )


__all__ = [
    "GeometryCheckPoseBounds",
    "GeometryCheckPoseDistance",
    "GeometryOffsetPose",
    "LogicCheckCondition",
    "LogicCheckValue",
    "LogicSetValue",
    "LogicWaitForValue",
    "ProgramTimer",
]
