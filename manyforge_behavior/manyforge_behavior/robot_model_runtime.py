"""Runtime helpers for authority-derived robot model consumer views."""

from __future__ import annotations

import tempfile
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory
from typing import Any, Mapping

from manyforge_behavior.model_paths import ResolvedRobotModel
from manyforge_behavior.robot_model_authority import (
    RobotModelAuthority,
    RobotModelImportView,
    build_roboplan_import_view,
    load_robot_model_authority,
)


@dataclass(frozen=True)
class RoboPlanRuntimeModelView:
    """Resolved model paths for a RoboPlan/Pinocchio consumer.

    `urdf_path` / `srdf_path` may point at a private generated view. Public
    metadata remains canonical through `authority` and `import_view`.
    """

    authority: RobotModelAuthority
    import_view: RobotModelImportView
    urdf_path: str
    srdf_path: str
    package_paths: list[str]
    _temp_dir: TemporaryDirectory[str] | None = None

    @property
    def model_name(self) -> str:
        return self.authority.model_name

    @property
    def metadata(self) -> dict[str, Any]:
        return {
            "canonical_fingerprint": self.authority.canonical_fingerprint,
            "view_fingerprint": self.import_view.view_fingerprint,
            "view_changed": self.import_view.changed,
            "canonical_urdf_path": self.authority.urdf_path,
            "canonical_srdf_path": self.authority.srdf_path,
            "view_urdf_path": self.urdf_path,
            "view_srdf_path": self.srdf_path,
            "canonical_to_view_joint_names": dict(
                self.import_view.canonical_to_view_joint_names
            ),
            "view_to_canonical_joint_names": dict(
                self.import_view.view_to_canonical_joint_names
            ),
            "commandable_joint_names": list(
                self.authority.commandable_joint_names
            ),
            "group_commandable_joint_names": {
                group_id: list(self.authority.group_commandable_joints(group_id))
                for group_id in self.authority.groups
            },
            "grippers": {
                gripper_id: {
                    "group_id": profile.group_id,
                    "command_joint": profile.command_joint,
                    "derived_joints": list(profile.derived_joints),
                    "touch_links": list(profile.touch_links),
                    "mount_frame": profile.mount_frame,
                    "attach_frame": profile.attach_frame,
                    "tcp_frame": profile.tcp_frame,
                    "auxiliary_frames": list(profile.auxiliary_frames),
                    "presets": dict(profile.presets),
                }
                for gripper_id, profile in self.authority.grippers.items()
            },
        }


def prepare_roboplan_runtime_model_view(
    *,
    resolved_model: ResolvedRobotModel,
    grippers: Mapping[str, Any] | None = None,
) -> RoboPlanRuntimeModelView:
    """Build the authority and private RoboPlan import view for a model."""

    authority = load_robot_model_authority(
        urdf_path=resolved_model.urdf_path,
        srdf_path=resolved_model.srdf_path,
        package_paths=resolved_model.package_paths,
        grippers=grippers,
        model_name=resolved_model.model_name,
    )
    import_view = build_roboplan_import_view(authority)
    if not import_view.changed:
        return RoboPlanRuntimeModelView(
            authority=authority,
            import_view=import_view,
            urdf_path=resolved_model.urdf_path,
            srdf_path=resolved_model.srdf_path,
            package_paths=list(resolved_model.package_paths),
        )

    temp_dir = tempfile.TemporaryDirectory(prefix="manyforge-rma-roboplan-")
    stem = f"{Path(resolved_model.model_name).stem or 'robot'}_roboplan_view"
    view_urdf, view_srdf = import_view.write_to_directory(temp_dir.name, stem=stem)
    return RoboPlanRuntimeModelView(
        authority=authority,
        import_view=import_view,
        urdf_path=str(view_urdf),
        srdf_path=str(view_srdf),
        package_paths=list(resolved_model.package_paths),
        _temp_dir=temp_dir,
    )


def attach_robot_model_metadata(target: Any, view: RoboPlanRuntimeModelView) -> None:
    """Best-effort metadata attachment for Python wrapper diagnostics/lifetime."""

    try:
        attach = getattr(target, "set_robot_model_authority_metadata", None)
        if callable(attach):
            attach(view.metadata)
        setattr(target, "robot_model_authority_metadata", view.metadata)
        setattr(target, "_robot_model_authority_temp_dir", view._temp_dir)
    except Exception:
        # Metadata is diagnostic only; the factory has already received paths.
        pass
