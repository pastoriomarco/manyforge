"""Typed deployment-artifact gate during the I6/N addressing cutover.

The former :mod:`manyforge_behavior.deployment` implementation parsed
``manyforge.deployment.v0`` and became the semantic owner for one
``robot_id/group_id``, adapter and runtime-resource selection, launcher module
selection, gripper transport, pose-source policy, authority, and Program/Kernel
cross-validation.  Those meanings cannot be retained beside the composable-cell
contracts: I6 replaces them atomically with component instances, command-target
and capability addresses, and the native deployment bindings.

No v0 runtime parser, compatibility selector, launcher factory, or partial
runtime projection remains here.  The one retained reader is a bounded,
read-only extraction of an authored URDF reference for Composer's neutral
structural viewer.  It does not validate or accept the enclosing deployment,
and it exposes no runtime address or policy.  Same-role Python authoring
clients may consume the specs-approved native artifact projection at I6/N;
until then every runtime deployment entry point fails before reading a path or
caller-owned mapping.

The exact historical implementation remains available at manyforge commit
``08222188293df8e12f90dc72e204c21f22f92ea1`` (tree
``bc749b9879d3c8f36fe7bf3036f9cc1f2ef1e60f``).
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Final, NoReturn

import yaml


DEPLOYMENT_ARTIFACT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_deployment_artifact_unavailable"
)
DEPLOYMENT_ARTIFACT_RESTORATION_GATES: Final = ("I6", "N")
_MAX_STRUCTURAL_ARTIFACT_BYTES: Final = 1_048_576
_MAX_MODEL_ID_CHARS: Final = 256
_MAX_PATH_CHARS: Final = 4_096
_MAX_PACKAGE_PATHS: Final = 128


class DeploymentArtifactUnavailableError(RuntimeError):
    """Raised before legacy artifact, filesystem, launcher, or runtime access."""


class StructuralDeploymentProjectionError(ValueError):
    """Raised when an authored model reference cannot be projected safely."""


def _required_bounded_text(
    value: object,
    *,
    path: str,
    max_chars: int,
) -> str:
    if not isinstance(value, str):
        raise StructuralDeploymentProjectionError(f"{path} must be a string")
    normalized = value.strip()
    if not normalized or len(normalized) > max_chars:
        raise StructuralDeploymentProjectionError(
            f"{path} must contain 1-{max_chars} characters"
        )
    if "\x00" in normalized:
        raise StructuralDeploymentProjectionError(f"{path} must not contain NUL")
    return normalized


def _optional_bounded_path(value: object, *, path: str) -> str:
    if value is None or value == "":
        return ""
    return _required_bounded_text(value, path=path, max_chars=_MAX_PATH_CHARS)


@dataclass(frozen=True, slots=True)
class AuthoredRobotModelProjection:
    """Neutral authored-model reference with no runtime addressing meaning."""

    authored_model_id: str
    urdf_path: str
    srdf_path: str = ""
    package_paths: tuple[str, ...] = ()


@dataclass(frozen=True, slots=True)
class AuthoredStructuralDeployment:
    """Read-only structural excerpt; not an accepted deployment artifact."""

    source_path: Path
    model: AuthoredRobotModelProjection

    @classmethod
    def from_path(cls, path: str | Path) -> "AuthoredStructuralDeployment":
        source_path = Path(path).resolve()
        try:
            with source_path.open("rb") as stream:
                encoded = stream.read(_MAX_STRUCTURAL_ARTIFACT_BYTES + 1)
        except OSError as exc:
            raise StructuralDeploymentProjectionError(
                f"cannot read authored structural artifact: {source_path}"
            ) from exc
        if len(encoded) > _MAX_STRUCTURAL_ARTIFACT_BYTES:
            raise StructuralDeploymentProjectionError(
                "authored structural artifact exceeds the 1048576-byte limit"
            )
        try:
            decoded = encoded.decode("utf-8")
        except UnicodeDecodeError as exc:
            raise StructuralDeploymentProjectionError(
                "authored structural artifact must be UTF-8"
            ) from exc
        try:
            raw = yaml.safe_load(decoded)
        except yaml.YAMLError as exc:
            raise StructuralDeploymentProjectionError(
                "authored structural artifact is not valid YAML"
            ) from exc
        if not isinstance(raw, dict):
            raise StructuralDeploymentProjectionError(
                "authored structural artifact must be a mapping"
            )

        # This is deliberately an excerpt reader for the retained structural
        # shell.  It does not inspect api_version or any runtime section and
        # therefore cannot confer deployment validity on the enclosing file.
        raw_model = raw.get("robot_config")
        if not isinstance(raw_model, dict):
            raise StructuralDeploymentProjectionError(
                "robot_config must be a mapping for authored structural viewing"
            )
        authored_model_id = _required_bounded_text(
            raw_model.get("robot_id"),
            path="robot_config.robot_id",
            max_chars=_MAX_MODEL_ID_CHARS,
        )
        urdf_path = _required_bounded_text(
            raw_model.get("urdf_path"),
            path="robot_config.urdf_path",
            max_chars=_MAX_PATH_CHARS,
        )
        srdf_path = _optional_bounded_path(
            raw_model.get("srdf_path"),
            path="robot_config.srdf_path",
        )
        package_paths_raw = raw_model.get("package_paths", [])
        if not isinstance(package_paths_raw, list):
            raise StructuralDeploymentProjectionError(
                "robot_config.package_paths must be a list"
            )
        if len(package_paths_raw) > _MAX_PACKAGE_PATHS:
            raise StructuralDeploymentProjectionError(
                "robot_config.package_paths exceeds the 128-item limit"
            )
        package_paths = tuple(
            _required_bounded_text(
                item,
                path=f"robot_config.package_paths[{index}]",
                max_chars=_MAX_PATH_CHARS,
            )
            for index, item in enumerate(package_paths_raw)
        )
        if len(package_paths) != len(set(package_paths)):
            raise StructuralDeploymentProjectionError(
                "robot_config.package_paths must not contain duplicates"
            )
        return cls(
            source_path=source_path,
            model=AuthoredRobotModelProjection(
                authored_model_id=authored_model_id,
                urdf_path=urdf_path,
                srdf_path=srdf_path,
                package_paths=package_paths,
            ),
        )


def _raise_deployment_artifact_unavailable() -> NoReturn:
    raise DeploymentArtifactUnavailableError(
        f"{DEPLOYMENT_ARTIFACT_UNAVAILABLE_REASON_CODE}: "
        "manyforge.deployment.v0 and singleton robot/group/runtime addressing "
        "were removed; resource-addressed native deployment artifacts return "
        "at I6/N"
    )


class Deployment:
    """Unavailable former v0 loader name retained only as an import-time gate."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        _raise_deployment_artifact_unavailable()

    @classmethod
    def from_dict(cls, *_args: object, **_kwargs: object) -> NoReturn:
        """Reject an old or caller-authored mapping before inspecting it."""

        _raise_deployment_artifact_unavailable()


__all__ = [
    "AuthoredRobotModelProjection",
    "AuthoredStructuralDeployment",
    "DEPLOYMENT_ARTIFACT_RESTORATION_GATES",
    "DEPLOYMENT_ARTIFACT_UNAVAILABLE_REASON_CODE",
    "Deployment",
    "DeploymentArtifactUnavailableError",
    "StructuralDeploymentProjectionError",
]
