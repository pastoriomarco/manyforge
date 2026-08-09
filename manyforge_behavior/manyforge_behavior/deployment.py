"""Typed gate for the removed v0 Python deployment owner.

The former :mod:`manyforge_behavior.deployment` implementation parsed
``manyforge.deployment.v0`` and became the semantic owner for one
``robot_id/group_id``, adapter and runtime-resource selection, launcher module
selection, gripper transport, pose-source policy, authority, and Program/Kernel
cross-validation.  Those meanings cannot be retained beside the composable-cell
contracts: I6 replaces them atomically with component instances, command-target
and capability addresses, and the native deployment bindings.

No v0 runtime parser, compatibility selector, launcher factory, partial
runtime projection, or `robot_config` geometry excerpt remains here. Composer
now receives the exact active retained viewer product through Headless. Every
old Python deployment entry point therefore fails before reading a path or
caller-owned mapping.

The exact historical implementation remains available at manyforge commit
``08222188293df8e12f90dc72e204c21f22f92ea1`` (tree
``bc749b9879d3c8f36fe7bf3036f9cc1f2ef1e60f``).
"""

from __future__ import annotations

from typing import Final, NoReturn


DEPLOYMENT_ARTIFACT_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_deployment_artifact_unavailable"
)
DEPLOYMENT_ARTIFACT_RESTORATION_GATES: Final = ("I6", "N")


class DeploymentArtifactUnavailableError(RuntimeError):
    """Raised before legacy artifact, filesystem, launcher, or runtime access."""


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
    "DEPLOYMENT_ARTIFACT_RESTORATION_GATES",
    "DEPLOYMENT_ARTIFACT_UNAVAILABLE_REASON_CODE",
    "Deployment",
    "DeploymentArtifactUnavailableError",
]
