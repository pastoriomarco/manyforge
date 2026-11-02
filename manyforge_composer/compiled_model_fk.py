"""Pure Pinocchio FK for an explicit compiler-retained URDF.

This module is a structural consumer used to qualify one retained compiled
model.  It has no Composer runtime state, scene authority, model discovery,
fallback path, or viewer wire-format responsibility.  Callers provide the
exact retained URDF, independent joint coordinates, and frames to evaluate.
Pinocchio owns expansion of URDF mimic joints.
"""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any


class CompiledModelFkError(RuntimeError):
    """The explicit retained model could not be loaded or evaluated."""


@dataclass(frozen=True)
class CompiledModelDescriptor:
    """Exact immutable input needed by the structural FK consumer."""

    model_key: str
    urdf_path: Path


@dataclass(frozen=True)
class RigidTransform:
    """One frame transform relative to the requested reference frame."""

    translation: tuple[float, float, float]
    rotation_xyzw: tuple[float, float, float, float]


class CompiledModelFk:
    """Loaded read-only model with per-call mutable Pinocchio scratch data."""

    def __init__(
        self,
        descriptor: CompiledModelDescriptor,
        model: Any,
        joint_q_indices: Mapping[str, tuple[int, int]],
    ) -> None:
        self._descriptor = descriptor
        self._model = model
        self._joint_q_indices = dict(joint_q_indices)

    @property
    def descriptor(self) -> CompiledModelDescriptor:
        return self._descriptor

    def compute_link_transforms(
        self,
        *,
        joint_coordinates: Mapping[str, float],
        link_names: Sequence[str],
        reference_frame: str,
    ) -> dict[str, RigidTransform]:
        """Evaluate frames from explicit independent URDF-native coordinates.

        Scalar revolute coordinates are radians and scalar prismatic
        coordinates are metres.  This structural consumer does not infer,
        translate, or publish a runtime state contract.
        """

        try:
            import pinocchio as pin
        except Exception as exc:  # pragma: no cover - deployment dependency
            raise CompiledModelFkError("Pinocchio is unavailable") from exc

        if not reference_frame or not self._model.existFrame(reference_frame):
            raise CompiledModelFkError(
                "reference frame is absent from the compiled model: "
                f"{reference_frame!r}"
            )
        requested_links = tuple(str(name) for name in link_names)
        if not requested_links:
            raise CompiledModelFkError("at least one compiled link frame is required")
        missing_links = [
            name
            for name in requested_links
            if not name or not self._model.existFrame(name)
        ]
        if missing_links:
            raise CompiledModelFkError(
                f"compiled link frames are absent from the model: {missing_links!r}"
            )

        q = pin.neutral(self._model)
        for joint_name, raw_position in joint_coordinates.items():
            index = self._joint_q_indices.get(str(joint_name))
            if index is None:
                raise CompiledModelFkError(
                    "independent joint is absent from the compiled model: "
                    f"{joint_name!r}"
                )
            q_index, nq = index
            if nq != 1:
                raise CompiledModelFkError(
                    "compiled joint is not a scalar independent coordinate: "
                    f"{joint_name!r}"
                )
            try:
                position = float(raw_position)
            except (TypeError, ValueError) as exc:
                raise CompiledModelFkError(
                    f"compiled joint position is not numeric: {joint_name!r}"
                ) from exc
            if not math.isfinite(position):
                raise CompiledModelFkError(
                    f"compiled joint position is not finite: {joint_name!r}"
                )
            q[q_index] = position

        data = self._model.createData()
        try:
            pin.forwardKinematics(self._model, data, q)
            pin.updateFramePlacements(self._model, data)
            reference_inverse = data.oMf[
                self._model.getFrameId(reference_frame)
            ].inverse()
            transforms: dict[str, RigidTransform] = {}
            for link_name in requested_links:
                placement = reference_inverse * data.oMf[
                    self._model.getFrameId(link_name)
                ]
                quaternion = pin.Quaternion(placement.rotation)
                quaternion.normalize()
                coefficients = quaternion.coeffs()  # x, y, z, w
                transforms[link_name] = RigidTransform(
                    translation=tuple(float(value) for value in placement.translation),
                    rotation_xyzw=tuple(
                        float(coefficients[index]) for index in range(4)
                    ),
                )
            return transforms
        except CompiledModelFkError:
            raise
        except Exception as exc:
            raise CompiledModelFkError(
                f"compiled model FK failed for {self._descriptor.model_key!r}"
            ) from exc


def load_compiled_model_fk(
    descriptor: CompiledModelDescriptor,
) -> CompiledModelFk:
    """Load exactly one retained URDF with native Pinocchio mimic semantics."""

    if not descriptor.model_key:
        raise CompiledModelFkError("compiled model key must not be empty")
    urdf_path = Path(descriptor.urdf_path)
    if not urdf_path.is_file():
        raise CompiledModelFkError(
            f"compiled URDF is not a regular file: {str(urdf_path)!r}"
        )
    try:
        import pinocchio as pin
    except Exception as exc:  # pragma: no cover - deployment dependency
        raise CompiledModelFkError("Pinocchio is unavailable") from exc

    try:
        model = pin.buildModelFromUrdf(str(urdf_path), True)
    except Exception as exc:
        raise CompiledModelFkError(
            f"compiled URDF could not be loaded: {str(urdf_path)!r}"
        ) from exc

    joint_q_indices: dict[str, tuple[int, int]] = {}
    for joint_id, raw_name in enumerate(model.names):
        if joint_id == 0:
            continue
        try:
            joint_q_indices[str(raw_name)] = (
                int(model.idx_qs[joint_id]),
                int(model.nqs[joint_id]),
            )
        except (IndexError, TypeError, ValueError):
            continue
    return CompiledModelFk(descriptor, model, joint_q_indices)
