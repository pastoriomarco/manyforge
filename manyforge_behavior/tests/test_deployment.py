"""Focused cutover tests for the unavailable v0 deployment owner.

Historical acceptance disposition
---------------------------------

The exact pre-cutover corpus and implementation are preserved at manyforge
commit ``08222188293df8e12f90dc72e204c21f22f92ea1`` (tree
``bc749b9879d3c8f36fe7bf3036f9cc1f2ef1e60f``).  The former cases map in
dependency order as follows:

* **never restore:** ``manyforge.deployment.v0`` itself, top-level
  ``robot_config`` and Program ``robot`` as runtime selectors, ambiguous
  ``robot_id/group_id`` dispatch, ``runtime_resources``, Python
  runner-module/factory selection, ``manyforge.skill.v0``, and the local
  skill/catalog compatibility path;
* **I5/H0 then I6/N:** model endpoint, assistant mode/tool/prompt authoring and
  Composer deployment-client behavior, using headless truth and the new native
  artifact projection rather than a second deployment owner;
* **I6/N:** strict specs-approved program/deployment loading, component and
  command-target binding, native adapter/capability selection, bounded
  cross-artifact validation, two-same-type-instance addressing, and all seven
  native deployment inputs;
* **I8/P5:** cooperative-interrupt rejection, execution-stop capability, and
  deadline/settlement cases, now owned by resource-scoped Kernel Stop rather
  than deployment or runner policy;
* **I9/I:** pose-source, simulator-control, Isaac bridge, reset, viewer, and
  five registered Isaac deployment cases;
* **I10/R and I11/T:** real Lite6 admission plus generalized articulated and
  fixed-pneumatic tool/gripper semantics; and
* **retained independently:** a bounded read-only authored URDF-reference
  projection keeps Composer's neutral structural viewer available without
  accepting the enclosing v0 deployment; the transport-neutral Lite6 model
  closure, digest, runtime-capture, and source-provenance cases now live in
  ``test_lite6_model_artifacts.py``.

The old test classes ``TestDeploymentLoading``, ``TestDeploymentValidation``,
``TestAssistantModeValidation``, ``TestCrossArtifactValidation``,
``TestAdapterCapabilityKeys``, and ``TestValidateCapabilities`` are historical
acceptance input for those gates, not evidence for a partial Python v0 loader.
"""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

import pytest
import yaml

from manyforge_behavior import deployment
from manyforge_behavior.deployment import (
    AuthoredStructuralDeployment,
    DEPLOYMENT_ARTIFACT_RESTORATION_GATES,
    DEPLOYMENT_ARTIFACT_UNAVAILABLE_REASON_CODE,
    Deployment,
    DeploymentArtifactUnavailableError,
    StructuralDeploymentProjectionError,
)


class _PoisonArtifact:
    """Fails if the unavailability boundary inspects a former input."""

    def __getattribute__(self, name: str) -> object:
        raise AssertionError(f"deployment gate accessed former input: {name}")

    def __iter__(self):
        raise AssertionError("deployment gate iterated former input")


@pytest.mark.parametrize(
    "entrypoint",
    [
        pytest.param(lambda value: Deployment(value), id="path-loader"),
        pytest.param(
            lambda value: Deployment.from_dict(value),
            id="mapping-loader",
        ),
    ],
)
def test_deployment_entrypoints_fail_before_reading_legacy_input(
    entrypoint: Callable[[object], object],
) -> None:
    with pytest.raises(DeploymentArtifactUnavailableError) as raised:
        entrypoint(_PoisonArtifact())

    message = str(raised.value)
    assert DEPLOYMENT_ARTIFACT_UNAVAILABLE_REASON_CODE in message
    assert "manyforge.deployment.v0" in message
    assert "I6/N" in message


def test_deployment_gate_names_exact_restoration_boundary() -> None:
    assert DEPLOYMENT_ARTIFACT_RESTORATION_GATES == ("I6", "N")


def test_structural_projection_extracts_only_authored_model_reference(
    tmp_path: Path,
) -> None:
    artifact = tmp_path / "cell.deployment.yaml"
    artifact.write_text(
        yaml.safe_dump(
            {
                "api_version": "manyforge.deployment.v0",
                "robot_config": {
                    "robot_id": "preview-arm",
                    "group_id": "must-not-be-projected",
                    "urdf_path": "models/preview.urdf",
                    "srdf_path": "models/preview.srdf",
                    "package_paths": ["models", "vendor/models"],
                },
                "runtime_resources": {"must": "not be projected"},
                "launch": {"runner": {"module": "must.not.load"}},
                "pose_sources": {"must": "not be projected"},
            }
        ),
        encoding="utf-8",
    )

    projection = AuthoredStructuralDeployment.from_path(artifact)

    assert projection.source_path == artifact.resolve()
    assert projection.model.authored_model_id == "preview-arm"
    assert projection.model.urdf_path == "models/preview.urdf"
    assert projection.model.srdf_path == "models/preview.srdf"
    assert projection.model.package_paths == ("models", "vendor/models")
    for runtime_name in (
        "robot_id",
        "group_id",
        "runtime_resources",
        "launch",
        "pose_sources",
    ):
        assert not hasattr(projection, runtime_name)
        assert not hasattr(projection.model, runtime_name)


@pytest.mark.parametrize(
    ("robot_config", "message"),
    [
        ({"robot_id": "arm"}, "robot_config.urdf_path"),
        (
            {
                "robot_id": "arm",
                "urdf_path": "arm.urdf",
                "package_paths": ["models"] * 129,
            },
            "128-item limit",
        ),
        (
            {
                "robot_id": "arm",
                "urdf_path": "arm.urdf",
                "package_paths": ["models", "models"],
            },
            "must not contain duplicates",
        ),
    ],
)
def test_structural_projection_rejects_unbounded_or_ambiguous_references(
    tmp_path: Path,
    robot_config: dict[str, object],
    message: str,
) -> None:
    artifact = tmp_path / "cell.deployment.yaml"
    artifact.write_text(
        yaml.safe_dump({"robot_config": robot_config}),
        encoding="utf-8",
    )

    with pytest.raises(StructuralDeploymentProjectionError, match=message):
        AuthoredStructuralDeployment.from_path(artifact)


def test_structural_projection_rejects_oversize_artifact(tmp_path: Path) -> None:
    artifact = tmp_path / "cell.deployment.yaml"
    artifact.write_bytes(b"x" * 1_048_577)

    with pytest.raises(StructuralDeploymentProjectionError, match="1048576-byte"):
        AuthoredStructuralDeployment.from_path(artifact)


@pytest.mark.parametrize(
    "removed_name",
    [
        "EXPECTED_API_VERSION",
        "RobotConfig",
        "GripperConfig",
        "AdapterConfig",
        "OrchestratorConfig",
        "LaunchConfig",
        "RuntimeResources",
        "DeploymentValidationError",
    ],
)
def test_deployment_module_does_not_export_old_semantic_contracts(
    removed_name: str,
) -> None:
    assert not hasattr(deployment, removed_name)
