"""Owner-neutral ROS pose-observation adapter mechanics.

This module deliberately does **not** own pose-source identity, freshness,
latest-state caching, scene-apply policy, status reduction, or a Composer wire
payload.  It only normalizes bounded ROS/simulator observations and offers a
depth-one callback adapter.  The I5/H0 headless owner now supplies the retained
pose-source lifetime/state boundary; concrete producer identity,
receiver-local ordering/freshness, and Isaac/ROS wiring remain I9/I work.

Relocation traceability
-----------------------
The former mixed implementation is pinned at Git blob
``582fe0b822726e063c4793926ce6f969144af6bf``.  Its exact 14-case test source
is blob ``453edb0ba3aee414bbe44d26a310465350d0c8f4``.  The current file retains
the pose/quaternion/covariance/message/TF transforms, simulator read-only
capture mapping, and bounded ROS subscription mechanics from those sources.
The old ``PoseSourceLatestState`` family, status constants, deployment-v0
``apply_policy`` bindings, duck-typed deployment constructors, polling loop,
frame relabeling, and Composer-shaped ``to_dict`` payload are intentionally
absent.  The H0 lifetime cases are carried by the current C++ owner; the
historical producer/adapter cases remain acceptance input at I and are not a
runnable legacy fallback.
"""

from __future__ import annotations

from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass
from itertools import islice
from math import hypot, isfinite
from numbers import Real
from typing import Any

from manyforge_ros.scene_mirror_probe import PoseSnapshot


_MAX_DETAIL_CHARS = 512
_MAX_SOURCE_TEXT_CHARS = 1024
_MAX_BATCH_ITEMS = 4096


class ObservationNormalizationError(ValueError):
    """One bounded, adapter-local normalization failure.

    The code describes translation failure only.  A future semantic owner
    decides whether it means stale, rejected, faulted, or any other runtime
    status.
    """

    def __init__(self, code: str, detail: str) -> None:
        self.code = _bounded_source_text(code, label="error code", limit=128)
        self.detail = _bounded_detail(detail)
        super().__init__(f"{self.code}: {self.detail}")


@dataclass(frozen=True)
class AdapterFailure:
    """Failure to normalize one source-local item."""

    source_key: str
    code: str
    detail: str


@dataclass(frozen=True)
class NormalizedJointState:
    """ROS JointState values without assuming all coordinates are angular."""

    topic: str
    names: tuple[str, ...]
    positions: tuple[float, ...]
    source_stamp_s: float | None


@dataclass(frozen=True)
class DetectionSelector:
    """Source-local Detection3D association and quality filter."""

    detection_id: str = ""
    class_id: str = ""
    selection: str = ""
    confidence_min: float | None = None
    max_position_variance_m2: float | None = None

    def __post_init__(self) -> None:
        for field_name in ("detection_id", "class_id", "selection"):
            raw_value = getattr(self, field_name)
            if not isinstance(raw_value, str):
                raise ValueError(f"{field_name} must be a string")
            if len(raw_value) > _MAX_SOURCE_TEXT_CHARS:
                raise ValueError(f"{field_name} exceeds the adapter bound")
            value = raw_value.strip()
            if len(value) > _MAX_SOURCE_TEXT_CHARS:
                raise ValueError(f"{field_name} exceeds the adapter bound")
            object.__setattr__(self, field_name, value)
        association_count = sum(
            bool(value) for value in (self.detection_id, self.class_id, self.selection)
        )
        if association_count != 1:
            raise ValueError(
                "selector requires exactly one detection_id, class_id, or selection"
            )
        if self.selection and self.selection not in ("first", "best_score"):
            raise ValueError("selection must be 'first' or 'best_score'")
        if self.confidence_min is not None:
            try:
                confidence_min = _finite_number(
                    self.confidence_min,
                    label="confidence_min",
                )
            except ValueError as exc:
                raise ValueError(str(exc)) from exc
            object.__setattr__(self, "confidence_min", confidence_min)
        if self.max_position_variance_m2 is not None:
            try:
                max_position_variance_m2 = _finite_number(
                    self.max_position_variance_m2,
                    label="max_position_variance_m2",
                )
            except ValueError as exc:
                raise ValueError(str(exc)) from exc
            if max_position_variance_m2 < 0.0:
                raise ValueError("max_position_variance_m2 must be finite and >= 0")
            object.__setattr__(
                self,
                "max_position_variance_m2",
                max_position_variance_m2,
            )


@dataclass(frozen=True)
class NormalizedDetectionPose:
    """One selected, quality-checked Detection3D pose."""

    detection_id: str
    class_id: str
    confidence: float
    max_position_variance_m2: float
    pose: PoseSnapshot


@dataclass(frozen=True)
class SimulationEntityPoseRequest:
    """Read-only request for one simulator entity pose.

    ``target_frame_id`` requests a real TF transform.  It never relabels a
    pose whose source frame differs.  A producer with a known nonconforming
    header may instead declare both ``source_header_frame_id`` and
    ``source_coordinate_frame_id``.  The adapter then requires the exact
    reported header before assigning the explicitly declared coordinate frame
    and performing the normal target-frame transform.
    """

    entity: str
    target_frame_id: str = ""
    source_header_frame_id: str = ""
    source_coordinate_frame_id: str = ""
    local_position_xyz: tuple[float, float, float] = (0.0, 0.0, 0.0)
    local_orientation_xyzw: tuple[float, float, float, float] = (
        0.0,
        0.0,
        0.0,
        1.0,
    )

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "entity",
            _bounded_source_text(self.entity, label="entity"),
        )
        object.__setattr__(
            self,
            "target_frame_id",
            _bounded_optional_source_text(
                self.target_frame_id,
                label="target_frame_id",
            ),
        )
        for field_name in (
            "source_header_frame_id",
            "source_coordinate_frame_id",
        ):
            object.__setattr__(
                self,
                field_name,
                _bounded_optional_source_text(
                    getattr(self, field_name),
                    label=field_name,
                ),
            )
        if bool(self.source_header_frame_id) != bool(
            self.source_coordinate_frame_id
        ):
            raise ValueError(
                "source_header_frame_id and source_coordinate_frame_id "
                "must be declared together"
            )
        object.__setattr__(
            self,
            "local_position_xyz",
            _finite_numeric_sequence(
                self.local_position_xyz,
                length=3,
                label="local_position_xyz",
            ),
        )
        object.__setattr__(
            self,
            "local_orientation_xyzw",
            _normalize_quaternion(
                _finite_numeric_sequence(
                    self.local_orientation_xyzw,
                    length=4,
                    label="local_orientation_xyzw",
                ),
                label="local_orientation_xyzw quaternion",
            ),
        )


@dataclass(frozen=True)
class SimulationEntityPoseObservation:
    """One source-local simulator observation."""

    entity: str
    pose: PoseSnapshot


@dataclass(frozen=True)
class SimulationEntityPoseBatch:
    """One read-only capture result with no status or retained latest state."""

    observations: tuple[SimulationEntityPoseObservation, ...]
    failures: tuple[AdapterFailure, ...]


@dataclass(frozen=True)
class TfPoseRequest:
    """One exact target/source TF lookup pair."""

    target_frame_id: str
    source_frame_id: str

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "target_frame_id",
            _bounded_source_text(self.target_frame_id, label="target_frame_id"),
        )
        object.__setattr__(
            self,
            "source_frame_id",
            _bounded_source_text(self.source_frame_id, label="source_frame_id"),
        )


@dataclass(frozen=True)
class TfPoseObservation:
    """One normalized TF lookup result."""

    request: TfPoseRequest
    pose: PoseSnapshot


@dataclass(frozen=True)
class TfPoseBatch:
    """One stateless group of TF lookup results."""

    observations: tuple[TfPoseObservation, ...]
    failures: tuple[AdapterFailure, ...]


class DepthOneSubscriptionAdapter:
    """Normalize ROS messages and synchronously hand them to another owner.

    The ROS subscription depth is one, but the adapter has no semantic cache,
    latest-value accessor, clock, freshness rule, or status.  The callback
    recipient owns receive timestamps, sequencing, retention, and policy.
    """

    def __init__(
        self,
        *,
        node: Any,
        message_type: Any,
        topic: str,
        normalize: Callable[[Any], Any],
        on_observation: Callable[[Any], None],
        on_error: Callable[[ObservationNormalizationError], None] | None = None,
        qos_profile_factory: Callable[[int], Any] | None = None,
    ) -> None:
        self._node = node
        self._normalize = normalize
        self._on_observation = on_observation
        self._on_error = on_error
        normalized_topic = _bounded_source_text(topic, label="topic")
        if qos_profile_factory is None:
            from rclpy.qos import QoSProfile

            qos_profile = QoSProfile(depth=1)
        else:
            qos_profile = qos_profile_factory(1)
        self._subscription = node.create_subscription(
            message_type,
            normalized_topic,
            self._on_message,
            qos_profile,
        )

    def close(self) -> None:
        """Destroy only this adapter's transport subscription."""

        self._node.destroy_subscription(self._subscription)

    def _on_message(self, message: Any) -> None:
        try:
            observation = self._normalize(message)
        except ObservationNormalizationError as exc:
            if self._on_error is not None:
                self._on_error(exc)
            return
        except ValueError as exc:
            if self._on_error is not None:
                self._on_error(
                    ObservationNormalizationError("invalid_message", str(exc))
                )
            return
        self._on_observation(observation)


def normalize_joint_state_message(message: Any, *, topic: str) -> NormalizedJointState:
    """Translate one ROS JointState without inventing coordinate units."""

    normalized_topic = _bounded_source_text(topic, label="topic")
    raw_names = _bounded_rows(
        getattr(message, "name", ()),
        label="joint names",
        error_code="invalid_joint_state",
    )
    raw_positions = _bounded_rows(
        getattr(message, "position", ()),
        label="joint positions",
        error_code="invalid_joint_state",
    )
    try:
        names = tuple(
            _bounded_source_text(name, label="joint name") for name in raw_names
        )
    except ValueError as exc:
        raise ObservationNormalizationError(
            "invalid_joint_state", str(exc)
        ) from exc
    try:
        positions = tuple(
            _finite_number(value, label="joint position") for value in raw_positions
        )
    except ValueError as exc:
        raise ObservationNormalizationError(
            "invalid_joint_state", str(exc)
        ) from exc
    if not names or len(names) != len(positions):
        raise ObservationNormalizationError(
            "invalid_joint_state",
            "joint names and positions must be non-empty and equal in length",
        )
    if len(set(names)) != len(names):
        raise ObservationNormalizationError(
            "invalid_joint_state", "joint names must be unique"
        )
    return NormalizedJointState(
        topic=normalized_topic,
        names=names,
        positions=positions,
        source_stamp_s=_stamp_to_float(
            getattr(getattr(message, "header", None), "stamp", None)
        ),
    )


def normalize_pose_stamped_message(
    message: Any,
    *,
    fallback_frame_id: str = "",
) -> PoseSnapshot:
    """Translate one ROS PoseStamped and normalize its quaternion."""

    return _pose_msg_to_snapshot(
        getattr(message, "pose", None),
        header=getattr(message, "header", None),
        fallback_frame_id=fallback_frame_id,
    )


def normalize_pose_array_message(
    message: Any,
    *,
    fallback_frame_id: str = "",
) -> tuple[PoseSnapshot, ...]:
    """Translate a PoseArray in source order without assigning object identity."""

    header = getattr(message, "header", None)
    poses = _bounded_rows(
        getattr(message, "poses", ()) or (),
        label="pose array",
        error_code="invalid_pose_array",
    )
    return tuple(
        _pose_msg_to_snapshot(
            pose,
            header=header,
            fallback_frame_id=fallback_frame_id,
        )
        for pose in poses
    )


def normalize_detection3d_array_message(
    message: Any,
    *,
    selector: DetectionSelector,
    fallback_frame_id: str = "",
    target_frame_id: str = "",
    tf_buffer: Any | None = None,
    lookup_time: Any | None = None,
) -> NormalizedDetectionPose:
    """Select and normalize one Detection3D observation.

    Association and quality checks stay source-local.  No scene object,
    capability, apply policy, or runtime status is assigned here.
    """

    detections = _bounded_rows(
        getattr(message, "detections", ()) or (),
        label="detection array",
        error_code="invalid_detection_array",
    )
    match = _select_detection3d_match(detections, selector)
    if match is None:
        raise ObservationNormalizationError(
            "detection_not_found", "no detection matched the configured selector"
        )
    if selector.confidence_min is not None and match.score < selector.confidence_min:
        raise ObservationNormalizationError(
            "confidence_below_min",
            f"score {match.score:.6g} < confidence_min {selector.confidence_min:.6g}",
        )
    covariance_max = _pose_position_covariance_max(match.pose_with_covariance)
    if (
        selector.max_position_variance_m2 is not None
        and covariance_max > selector.max_position_variance_m2
    ):
        raise ObservationNormalizationError(
            "covariance_exceeded",
            f"position covariance {covariance_max:.6g} > "
            f"max_position_variance_m2 {selector.max_position_variance_m2:.6g}",
        )
    pose = _pose_msg_to_snapshot(
        getattr(match.pose_with_covariance, "pose", None),
        header=_preferred_header(match.header, getattr(message, "header", None)),
        fallback_frame_id=fallback_frame_id,
    )
    pose = transform_pose_snapshot(
        pose,
        target_frame_id=target_frame_id,
        buffer=tf_buffer,
        lookup_time=lookup_time,
    )
    return NormalizedDetectionPose(
        detection_id=match.detection_id,
        class_id=match.class_id,
        confidence=match.score,
        max_position_variance_m2=covariance_max,
        pose=pose,
    )


def capture_simulation_entity_poses(
    client: Any,
    requests: Sequence[SimulationEntityPoseRequest],
    *,
    tf_buffer: Any | None = None,
    lookup_time: Any | None = None,
) -> SimulationEntityPoseBatch:
    """Perform one explicit read-only simulator capture with no local state."""

    request_tuple = _bounded_rows(
        requests,
        label="simulation entity requests",
    )
    if not request_tuple:
        raise ValueError("requests must not be empty")
    if not all(
        isinstance(request, SimulationEntityPoseRequest) for request in request_tuple
    ):
        raise ValueError("simulation entity requests contain an invalid row")
    entities = tuple(request.entity for request in request_tuple)
    if len(set(entities)) != len(entities):
        raise ValueError("simulation entity requests must be unique")
    try:
        snapshot = client.capture(
            entity_names=entities,
            include_info=False,
            include_state=True,
            include_bounds=False,
        )
    except Exception as exc:
        detail = _bounded_detail(exc)
        return SimulationEntityPoseBatch(
            observations=(),
            failures=tuple(
                AdapterFailure(request.entity, "source_error", detail)
                for request in request_tuple
            ),
        )

    try:
        snapshot_entities = _bounded_rows(
            getattr(snapshot, "entities", ()),
            label="simulation snapshot entities",
        )
        snapshot_errors = _bounded_rows(
            getattr(snapshot, "errors", ()),
            label="simulation snapshot errors",
        )
        by_entity: dict[str, Any] = {}
        for entity in snapshot_entities:
            name = _bounded_source_text(
                getattr(entity, "name", None),
                label="simulation snapshot entity name",
            )
            if name in by_entity:
                raise ValueError(
                    f"simulation snapshot contains duplicate entity {name}"
                )
            by_entity[name] = entity
        if len(snapshot_errors) + len(request_tuple) > _MAX_BATCH_ITEMS:
            raise ValueError(
                "simulation snapshot plus requested results exceed the adapter bound"
            )
    except (AttributeError, TypeError, ValueError) as exc:
        return _simulation_capture_failure_batch(
            request_tuple,
            code="invalid_source_snapshot",
            detail=exc,
        )

    observations: list[SimulationEntityPoseObservation] = []
    failures: list[AdapterFailure] = [
        AdapterFailure("", "source_error", _bounded_detail(error))
        for error in snapshot_errors
    ]
    for request in request_tuple:
        entity = by_entity.get(request.entity)
        if entity is None:
            if len(failures) >= _MAX_BATCH_ITEMS:
                return _simulation_capture_failure_batch(
                    request_tuple,
                    code="invalid_source_snapshot",
                    detail="simulation snapshot produced too many failure rows",
                )
            failures.append(
                AdapterFailure(
                    request.entity,
                    "missing_entity",
                    "entity missing from simulation snapshot",
                )
            )
            continue
        try:
            entity_errors = _bounded_rows(
                getattr(entity, "errors", ()),
                label=f"errors for simulation entity {request.entity}",
            )
        except ValueError as exc:
            return _simulation_capture_failure_batch(
                request_tuple,
                code="invalid_source_snapshot",
                detail=exc,
            )
        if entity_errors:
            if len(failures) >= _MAX_BATCH_ITEMS:
                return _simulation_capture_failure_batch(
                    request_tuple,
                    code="invalid_source_snapshot",
                    detail="simulation snapshot produced too many failure rows",
                )
            failures.append(
                AdapterFailure(
                    request.entity,
                    "source_error",
                    _bounded_error_summary(entity_errors),
                )
            )
            continue
        entity_pose = getattr(entity, "pose", None)
        if entity_pose is None:
            if len(failures) >= _MAX_BATCH_ITEMS:
                return _simulation_capture_failure_batch(
                    request_tuple,
                    code="invalid_source_snapshot",
                    detail="simulation snapshot produced too many failure rows",
                )
            failures.append(
                AdapterFailure(
                    request.entity,
                    "pose_missing",
                    "entity state did not include pose",
                )
            )
            continue
        if not isinstance(entity_pose, PoseSnapshot):
            if len(failures) >= _MAX_BATCH_ITEMS:
                return _simulation_capture_failure_batch(
                    request_tuple,
                    code="invalid_source_snapshot",
                    detail="simulation snapshot produced too many failure rows",
                )
            failures.append(
                AdapterFailure(
                    request.entity,
                    "frame_or_pose_invalid",
                    "entity pose is not a normalized PoseSnapshot",
                )
            )
            continue
        try:
            pose = compose_pose_with_local_offset(
                entity_pose,
                request.local_position_xyz,
                request.local_orientation_xyzw,
            )
            if request.source_header_frame_id:
                if pose.frame_id != request.source_header_frame_id:
                    raise ObservationNormalizationError(
                        "source_frame_mismatch",
                        "simulation entity header changed: expected "
                        f"{request.source_header_frame_id}, observed "
                        f"{pose.frame_id}",
                    )
                pose = PoseSnapshot(
                    frame_id=request.source_coordinate_frame_id,
                    stamp_sec=pose.stamp_sec,
                    position_xyz=pose.position_xyz,
                    orientation_xyzw=pose.orientation_xyzw,
                )
            pose = transform_pose_snapshot(
                pose,
                target_frame_id=request.target_frame_id,
                buffer=tf_buffer,
                lookup_time=lookup_time,
            )
        except ValueError as exc:
            if len(failures) >= _MAX_BATCH_ITEMS:
                return _simulation_capture_failure_batch(
                    request_tuple,
                    code="invalid_source_snapshot",
                    detail="simulation snapshot produced too many failure rows",
                )
            failures.append(
                AdapterFailure(
                    request.entity,
                    "frame_or_pose_invalid",
                    _bounded_detail(exc),
                )
            )
            continue
        observations.append(
            SimulationEntityPoseObservation(entity=request.entity, pose=pose)
        )
    return SimulationEntityPoseBatch(tuple(observations), tuple(failures))


def capture_tf_poses(
    buffer: Any,
    requests: Sequence[TfPoseRequest],
    *,
    lookup_time: Any | None = None,
) -> TfPoseBatch:
    """Perform exact TF lookups without retaining a latest-state cache."""

    request_tuple = _bounded_rows(requests, label="TF requests")
    if not request_tuple:
        raise ValueError("requests must not be empty")
    if not all(isinstance(request, TfPoseRequest) for request in request_tuple):
        raise ValueError("TF requests contain an invalid row")
    if len(set(request_tuple)) != len(request_tuple):
        raise ValueError("TF requests must be unique")
    resolved_lookup_time = _tf_lookup_time() if lookup_time is None else lookup_time
    observations: list[TfPoseObservation] = []
    failures: list[AdapterFailure] = []
    for request in request_tuple:
        source_key = f"{request.target_frame_id}<-{request.source_frame_id}"
        try:
            transform = buffer.lookup_transform(
                request.target_frame_id,
                request.source_frame_id,
                resolved_lookup_time,
            )
            pose = normalize_transform_stamped(transform)
        except Exception as exc:
            failures.append(
                AdapterFailure(source_key, "tf_lookup_failed", _bounded_detail(exc))
            )
            continue
        observations.append(TfPoseObservation(request=request, pose=pose))
    return TfPoseBatch(tuple(observations), tuple(failures))


def transform_pose_snapshot(
    pose: PoseSnapshot,
    *,
    target_frame_id: str,
    buffer: Any | None,
    lookup_time: Any | None = None,
) -> PoseSnapshot:
    """Transform a pose through TF; never relabel a different source frame."""

    try:
        target_frame = _bounded_optional_source_text(
            target_frame_id,
            label="target frame",
        )
    except ValueError as exc:
        raise ObservationNormalizationError(
            "frame_unresolved", str(exc)
        ) from exc
    normalized_pose = _validated_pose_snapshot(pose, label="source pose")
    if not target_frame or normalized_pose.frame_id == target_frame:
        return normalized_pose
    if buffer is None:
        raise ObservationNormalizationError(
            "frame_unresolved",
            f"no TF buffer available for {target_frame} <- "
            f"{normalized_pose.frame_id}",
        )
    try:
        transform = buffer.lookup_transform(
            target_frame,
            normalized_pose.frame_id,
            _tf_lookup_time() if lookup_time is None else lookup_time,
        )
    except Exception as exc:
        raise ObservationNormalizationError(
            "frame_unresolved",
            f"TF lookup failed for {target_frame} <- "
            f"{normalized_pose.frame_id}: {exc}",
        ) from exc

    tf = getattr(transform, "transform", None)
    translation = getattr(tf, "translation", None)
    rotation = getattr(tf, "rotation", None)
    if translation is None or rotation is None:
        raise ObservationNormalizationError(
            "invalid_transform", "TransformStamped transform is incomplete"
        )
    transform_translation = _finite_vector3(
        translation,
        label="TransformStamped translation",
    )
    transform_orientation = _normalize_quaternion(
        _quaternion_from_message(rotation),
        label="TransformStamped orientation quaternion",
    )
    transformed_position = _vector_add(
        transform_translation,
        _rotate_vector(transform_orientation, normalized_pose.position_xyz),
    )
    transformed_position = _finite_numeric_sequence(
        transformed_position,
        length=3,
        label="transformed pose position_xyz",
    )
    transformed_orientation = _normalize_quaternion(
        _quaternion_multiply(
            transform_orientation,
            normalized_pose.orientation_xyzw,
        ),
        label="transformed pose orientation quaternion",
    )
    return PoseSnapshot(
        frame_id=target_frame,
        stamp_sec=normalized_pose.stamp_sec,
        position_xyz=transformed_position,
        orientation_xyzw=transformed_orientation,
    )


def compose_pose_with_local_offset(
    pose: PoseSnapshot,
    position_xyz: tuple[float, float, float],
    orientation_xyzw: tuple[float, float, float, float],
) -> PoseSnapshot:
    """Compose a source pose with a fixed source-local offset."""

    normalized_pose = _validated_pose_snapshot(pose, label="source pose")
    normalized_position = _finite_numeric_sequence(
        position_xyz,
        length=3,
        label="fixed-offset position_xyz",
    )
    offset_orientation = _normalize_quaternion(
        _finite_numeric_sequence(
            orientation_xyzw,
            length=4,
            label="fixed-offset orientation_xyzw",
        ),
        label="fixed-offset orientation quaternion",
    )
    composed_position = _vector_add(
        normalized_pose.position_xyz,
        _rotate_vector(
            normalized_pose.orientation_xyzw,
            normalized_position,
        ),
    )
    composed_position = _finite_numeric_sequence(
        composed_position,
        length=3,
        label="composed position_xyz",
    )
    return PoseSnapshot(
        frame_id=normalized_pose.frame_id,
        stamp_sec=normalized_pose.stamp_sec,
        position_xyz=composed_position,
        orientation_xyzw=_normalize_quaternion(
            _quaternion_multiply(
                normalized_pose.orientation_xyzw,
                offset_orientation,
            ),
            label="composed orientation quaternion",
        ),
    )


def normalize_transform_stamped(transform: Any) -> PoseSnapshot:
    """Translate one ROS TransformStamped into a finite PoseSnapshot."""

    header = getattr(transform, "header", None)
    tf = getattr(transform, "transform", None)
    translation = getattr(tf, "translation", None)
    rotation = getattr(tf, "rotation", None)
    if translation is None or rotation is None:
        raise ObservationNormalizationError(
            "invalid_transform", "TransformStamped transform is incomplete"
        )
    position_xyz = _finite_vector3(
        translation,
        label="TransformStamped translation",
    )
    orientation_xyzw = _normalize_quaternion(
        _quaternion_from_message(rotation),
        label="TransformStamped orientation quaternion",
    )
    try:
        frame_id = _bounded_source_text(
            getattr(header, "frame_id", None),
            label="TransformStamped header.frame_id",
        )
    except ValueError as exc:
        raise ObservationNormalizationError(
            "invalid_transform", str(exc)
        ) from exc
    return PoseSnapshot(
        frame_id=frame_id,
        stamp_sec=_stamp_to_float(getattr(header, "stamp", None)),
        position_xyz=position_xyz,
        orientation_xyzw=orientation_xyzw,
    )


@dataclass(frozen=True)
class _Detection3DMatch:
    detection_id: str
    class_id: str
    score: float
    pose_with_covariance: Any
    header: Any


def _select_detection3d_match(
    detections: Iterable[Any],
    selector: DetectionSelector,
) -> _Detection3DMatch | None:
    best: _Detection3DMatch | None = None
    result_row_count = 0
    for detection in detections:
        raw_detection_id = getattr(detection, "id", "") or ""
        try:
            detection_id = _bounded_optional_source_text(
                raw_detection_id,
                label="detection id",
            )
        except ValueError as exc:
            raise ObservationNormalizationError(
                "invalid_detection_array", str(exc)
            ) from exc
        if (
            not selector.selection
            and selector.detection_id
            and detection_id != selector.detection_id
        ):
            continue
        results = _bounded_rows(
            getattr(detection, "results", ()) or (),
            label="detection result rows",
            error_code="invalid_detection_array",
        )
        result_row_count += len(results)
        if result_row_count > _MAX_BATCH_ITEMS:
            raise ObservationNormalizationError(
                "invalid_detection_array",
                "aggregate detection result rows exceed the adapter bound",
            )
        for result in results:
            hypothesis = getattr(result, "hypothesis", None)
            raw_class_id = getattr(hypothesis, "class_id", "") or ""
            try:
                class_id = _bounded_optional_source_text(
                    raw_class_id,
                    label="detection class_id",
                )
            except ValueError as exc:
                raise ObservationNormalizationError(
                    "invalid_detection_array", str(exc)
                ) from exc
            if (
                not selector.selection
                and selector.class_id
                and class_id != selector.class_id
            ):
                continue
            try:
                score = _finite_number(
                    getattr(hypothesis, "score"),
                    label="detection score",
                )
            except (AttributeError, ValueError) as exc:
                raise ObservationNormalizationError(
                    "invalid_detection_array", str(exc)
                ) from exc
            candidate = _Detection3DMatch(
                detection_id=detection_id,
                class_id=class_id,
                score=score,
                pose_with_covariance=getattr(result, "pose", None),
                header=getattr(detection, "header", None),
            )
            if selector.selection == "first":
                return candidate
            if best is None or candidate.score > best.score:
                best = candidate
    return best


def _pose_position_covariance_max(pose_with_covariance: Any) -> float:
    covariance_raw = getattr(pose_with_covariance, "covariance", None)
    if covariance_raw is None:
        covariance: tuple[Any, ...] = ()
    else:
        covariance = _bounded_rows(
            covariance_raw,
            label="pose covariance",
            limit=36,
            error_code="invalid_covariance",
        )
    if len(covariance) != 36:
        raise ObservationNormalizationError(
            "invalid_covariance", "pose covariance must contain 36 values"
        )
    try:
        diagonal = [
            _finite_number(
                covariance[index],
                label=f"pose covariance[{index}]",
            )
            for index in (0, 7, 14)
        ]
    except ValueError as exc:
        raise ObservationNormalizationError(
            "invalid_covariance",
            str(exc),
        ) from exc
    if any(value < 0.0 for value in diagonal):
        raise ObservationNormalizationError(
            "invalid_covariance",
            "pose covariance position diagonal contains negative values",
        )
    return max(diagonal)


def _pose_msg_to_snapshot(
    pose: Any,
    *,
    header: Any,
    fallback_frame_id: str,
) -> PoseSnapshot:
    raw_frame_id = getattr(header, "frame_id", "") or fallback_frame_id
    try:
        frame_id = _bounded_source_text(raw_frame_id, label="pose header.frame_id")
    except ValueError as exc:
        raise ObservationNormalizationError(
            "invalid_pose", str(exc)
        ) from exc
    position = getattr(pose, "position", None)
    orientation = getattr(pose, "orientation", None)
    if position is None or orientation is None:
        raise ObservationNormalizationError("invalid_pose", "pose is incomplete")
    position_xyz = _finite_vector3(position, label="pose position")
    orientation_xyzw = _normalize_quaternion(
        _quaternion_from_message(orientation),
        label="pose orientation quaternion",
    )
    return PoseSnapshot(
        frame_id=frame_id,
        stamp_sec=_stamp_to_float(getattr(header, "stamp", None)),
        position_xyz=position_xyz,
        orientation_xyzw=orientation_xyzw,
    )


def _finite_vector3(value: Any, *, label: str) -> tuple[float, float, float]:
    try:
        vector = (
            _finite_number(getattr(value, "x", None), label=f"{label}.x"),
            _finite_number(getattr(value, "y", None), label=f"{label}.y"),
            _finite_number(getattr(value, "z", None), label=f"{label}.z"),
        )
    except ValueError as exc:
        raise ObservationNormalizationError(
            "invalid_pose", str(exc)
        ) from exc
    return vector


def _quaternion_from_message(value: Any) -> tuple[float, float, float, float]:
    try:
        return (
            _finite_number(getattr(value, "x", None), label="quaternion.x"),
            _finite_number(getattr(value, "y", None), label="quaternion.y"),
            _finite_number(getattr(value, "z", None), label="quaternion.z"),
            _finite_number(getattr(value, "w", None), label="quaternion.w"),
        )
    except ValueError as exc:
        raise ObservationNormalizationError(
            "invalid_quaternion", str(exc)
        ) from exc


def _normalize_quaternion(
    quaternion: tuple[float, ...],
    *,
    label: str,
) -> tuple[float, float, float, float]:
    if len(quaternion) != 4:
        raise ObservationNormalizationError(
            "invalid_quaternion", f"{label} must contain four values"
        )
    if not all(isfinite(value) for value in quaternion):
        raise ObservationNormalizationError(
            "invalid_quaternion", f"{label} contains non-finite values"
        )
    norm = hypot(*quaternion)
    if not isfinite(norm) or norm <= 1e-9:
        raise ObservationNormalizationError(
            "invalid_quaternion", f"{label} has invalid norm"
        )
    return tuple(value / norm for value in quaternion)  # type: ignore[return-value]


def _quaternion_multiply(
    lhs: tuple[float, float, float, float],
    rhs: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    lx, ly, lz, lw = lhs
    rx, ry, rz, rw = rhs
    return (
        lw * rx + lx * rw + ly * rz - lz * ry,
        lw * ry - lx * rz + ly * rw + lz * rx,
        lw * rz + lx * ry - ly * rx + lz * rw,
        lw * rw - lx * rx - ly * ry - lz * rz,
    )


def _quaternion_conjugate(
    quaternion: tuple[float, float, float, float],
) -> tuple[float, float, float, float]:
    x, y, z, w = quaternion
    return (-x, -y, -z, w)


def _rotate_vector(
    quaternion: tuple[float, float, float, float],
    vector: tuple[float, float, float],
) -> tuple[float, float, float]:
    vx, vy, vz = vector
    rotated = _quaternion_multiply(
        _quaternion_multiply(quaternion, (vx, vy, vz, 0.0)),
        _quaternion_conjugate(quaternion),
    )
    return (rotated[0], rotated[1], rotated[2])


def _vector_add(
    lhs: tuple[float, float, float],
    rhs: tuple[float, float, float],
) -> tuple[float, float, float]:
    return (lhs[0] + rhs[0], lhs[1] + rhs[1], lhs[2] + rhs[2])


def _stamp_to_float(stamp: Any) -> float | None:
    if stamp is None:
        return None
    sec = getattr(stamp, "sec", None)
    nanosec = getattr(stamp, "nanosec", None)
    if type(sec) is not int or type(nanosec) is not int:
        raise ObservationNormalizationError(
            "invalid_timestamp",
            "timestamp sec and nanosec must be exact integers",
        )
    if not 0 <= nanosec < 1_000_000_000:
        raise ObservationNormalizationError(
            "invalid_timestamp",
            "timestamp nanosec must be in [0, 1000000000)",
        )
    try:
        value = float(sec) + float(nanosec) * 1e-9
    except (TypeError, ValueError, OverflowError) as exc:
        raise ObservationNormalizationError(
            "invalid_timestamp", "timestamp must be finite"
        ) from exc
    if not isfinite(value):
        raise ObservationNormalizationError(
            "invalid_timestamp", "timestamp must be finite"
        )
    return value


def _preferred_header(primary: Any, fallback: Any) -> Any:
    primary_stamp = getattr(primary, "stamp", None)
    primary_sec = getattr(primary_stamp, "sec", None)
    primary_nanosec = getattr(primary_stamp, "nanosec", None)
    has_primary_stamp = primary_stamp is not None and not (
        type(primary_sec) is int
        and type(primary_nanosec) is int
        and primary_sec == 0
        and primary_nanosec == 0
    )
    primary_frame = getattr(primary, "frame_id", "")
    if primary_frame not in (None, "") or has_primary_stamp:
        return primary
    return fallback


def _tf_lookup_time() -> Any:
    from rclpy.time import Time

    return Time()


def _bounded_rows(
    value: Any,
    *,
    label: str,
    limit: int = _MAX_BATCH_ITEMS,
    error_code: str | None = None,
) -> tuple[Any, ...]:
    detail: str | None = None
    if isinstance(value, (str, bytes, bytearray)):
        detail = f"{label} must be a row sequence"
        rows: tuple[Any, ...] = ()
    else:
        try:
            rows = tuple(islice(iter(value), limit + 1))
        except TypeError:
            rows = ()
            detail = f"{label} must be a row sequence"
    if detail is None and len(rows) > limit:
        detail = f"{label} exceed the adapter bound of {limit}"
    if detail is not None:
        if error_code is not None:
            raise ObservationNormalizationError(error_code, detail)
        raise ValueError(detail)
    return rows


def _bounded_optional_source_text(value: object, *, label: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    if len(value) > _MAX_SOURCE_TEXT_CHARS:
        raise ValueError(f"{label} exceeds the adapter bound")
    text = value.strip()
    if len(text) > _MAX_SOURCE_TEXT_CHARS:
        raise ValueError(f"{label} exceeds the adapter bound")
    return text


def _finite_number(value: object, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError(f"{label} must be a numeric value, not text or boolean")
    try:
        number = float(value)
    except (TypeError, ValueError, OverflowError) as exc:
        raise ValueError(f"{label} must be numeric") from exc
    if not isfinite(number):
        raise ValueError(f"{label} must be finite")
    return number


def _finite_numeric_sequence(
    value: object,
    *,
    length: int,
    label: str,
) -> tuple[float, ...]:
    rows = _bounded_rows(value, label=label, limit=length)
    if len(rows) != length:
        raise ValueError(f"{label} must contain {length} values")
    return tuple(
        _finite_number(item, label=f"{label}[{index}]")
        for index, item in enumerate(rows)
    )


def _validated_pose_snapshot(pose: PoseSnapshot, *, label: str) -> PoseSnapshot:
    if not isinstance(pose, PoseSnapshot):
        raise ObservationNormalizationError(
            "invalid_pose", f"{label} must be a PoseSnapshot"
        )
    try:
        frame_id = _bounded_source_text(pose.frame_id, label=f"{label} frame_id")
        position_xyz = _finite_numeric_sequence(
            pose.position_xyz,
            length=3,
            label=f"{label} position_xyz",
        )
        stamp_sec = (
            None
            if pose.stamp_sec is None
            else _finite_number(pose.stamp_sec, label=f"{label} stamp_sec")
        )
        orientation_values = _finite_numeric_sequence(
            pose.orientation_xyzw,
            length=4,
            label=f"{label} orientation_xyzw",
        )
    except ValueError as exc:
        raise ObservationNormalizationError("invalid_pose", str(exc)) from exc
    orientation_xyzw = _normalize_quaternion(
        orientation_values,
        label=f"{label} orientation quaternion",
    )
    return PoseSnapshot(
        frame_id=frame_id,
        stamp_sec=stamp_sec,
        position_xyz=position_xyz,  # type: ignore[arg-type]
        orientation_xyzw=orientation_xyzw,
    )


def _simulation_capture_failure_batch(
    requests: tuple[Any, ...],
    *,
    code: str,
    detail: object,
) -> SimulationEntityPoseBatch:
    bounded_detail = _bounded_detail(detail)
    return SimulationEntityPoseBatch(
        observations=(),
        failures=tuple(
            AdapterFailure(request.entity, code, bounded_detail)
            for request in requests
        ),
    )


def _bounded_error_summary(errors: tuple[Any, ...]) -> str:
    summary = ""
    for error in errors:
        text = _bounded_detail(error)
        separator = "; " if summary else ""
        remaining = _MAX_DETAIL_CHARS - len(summary) - len(separator)
        if remaining <= 0:
            break
        summary += separator + text[:remaining]
        if len(text) > remaining:
            break
    return summary


def _bounded_source_text(value: object, *, label: str, limit: int = 1024) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    if len(value) > limit:
        raise ValueError(f"{label} exceeds {limit} characters")
    text = value.strip()
    if not text:
        raise ValueError(f"{label} must be non-empty")
    if len(text) > limit:
        raise ValueError(f"{label} exceeds {limit} characters")
    return text


def _bounded_detail(value: object) -> str:
    text = str(value)
    if len(text) <= _MAX_DETAIL_CHARS:
        return text
    return text[: _MAX_DETAIL_CHARS - 3] + "..."
