"""Direct C0.2o qualification of the sole I2 Python extension.

Collection performs no product import and needs no files.  Execution requires
absolute paths in the following variables:

* ``MANYFORGE_I2_BINDING_EXTENSION``: the exact candidate ELF extension;
* ``MANYFORGE_I2_BINDING_EVIDENCE_ROOT``: an existing, empty, lane-exclusive
  directory in which these tests create one raw evidence file per case;
* ``MANYFORGE_I2_BINDING_STAGED_ROOT``: the extension-only staging directory
  (used by the sole-module case);
* ``MANYFORGE_I2_BINDING_GENERATED_ROOT``: the four generated binding outputs
  (used by the generated-API case); and
* ``MANYFORGE_I2_BINDING_SOURCE_ROOT``: the exact candidate source tree (used
  by the forbidden-surface case); and
* ``MANYFORGE_I2_BINDING_EXECUTABLE``: the Python executable selected by the
  packet, including the linked ASan/UBSan host in that sanitizer lane.

No candidate-authored semantic trace is consumed.  Every semantic observation
is made here from calls to the supplied extension.  Evidence files are created
with O_EXCL/O_NOFOLLOW after proving the lane directory empty; precreation,
hard-link aliases, symlinks, and reuse fail closed.  Evidence contains derived
measurements and raw test-owned observations, never a producer pass flag.
"""

from __future__ import annotations

import ast
import concurrent.futures
import fnmatch
import gc
import hashlib
import importlib.util
import json
import os
import platform
import re
import stat
import subprocess
import sys
import sysconfig
import threading
import time
from pathlib import Path
from types import ModuleType
from typing import Any, Callable, NoReturn, get_args, get_origin

import pytest


_EXTENSION_ENV = "MANYFORGE_I2_BINDING_EXTENSION"
_EVIDENCE_ROOT_ENV = "MANYFORGE_I2_BINDING_EVIDENCE_ROOT"
_STAGED_ROOT_ENV = "MANYFORGE_I2_BINDING_STAGED_ROOT"
_GENERATED_ROOT_ENV = "MANYFORGE_I2_BINDING_GENERATED_ROOT"
_SOURCE_ROOT_ENV = "MANYFORGE_I2_BINDING_SOURCE_ROOT"
_EXECUTABLE_ENV = "MANYFORGE_I2_BINDING_EXECUTABLE"
_MODULE_NAME = "manyforge_core_py"
_NODE_PREFIX = "verification/composable_cell_i2/binding_contract_tests.py::test_"
_MAX_EXTENSION_BYTES = 1024 * 1024 * 1024
_MAX_GENERATED_BYTES = 64 * 1024 * 1024
_MAX_SOURCE_BYTES = 32 * 1024 * 1024

_ZERO_DIGEST = "sha256:" + ("0" * 64)
_CANONICAL_EMPTY_INPUT = json.dumps(
    {
        "api_version": "manyforge.deployment.v1",
        "deployment_artifact_id": "c1_build_authority",
        "deployment_revision": _ZERO_DIGEST,
        "cell_id": "c1_build_authority_cell",
        "name": "C1 build authority",
        "catalogs": {"records": []},
        "graph": {
            "runtime_units": [],
            "model_instances": [],
            "controllable_components": [],
            "control_domains": [],
            "control_bindings": [],
            "component_attachments": [],
        },
        "desired_control_configurations": [],
        "capability_provider_bindings": [],
    },
    sort_keys=True,
    separators=(",", ":"),
)
_CANONICAL_EMPTY_NORMALIZED = (
    '{"api_version":"manyforge.deployment.v1",'
    '"capability_provider_bindings":[],"catalogs":{"records":[]},'
    '"cell_id":"c1_build_authority_cell",'
    '"deployment_artifact_id":"c1_build_authority",'
    '"deployment_revision":"sha256:'
    '7b1480ba7bc20044f194dbbcc48f0f1d08fe16b469a46ca79b7c409419d11f2d",'
    '"desired_control_configurations":[],"graph":'
    '{"component_attachments":[],"control_bindings":[],"control_domains":[],'
    '"controllable_components":[],"model_instances":[],"runtime_units":[]},'
    '"name":"C1 build authority"}'
)
_EXPECTED_DEPLOYMENT_REVISION = (
    "sha256:7b1480ba7bc20044f194dbbcc48f0f1d08fe16b469a46ca79b7c409419d11f2d"
)
_EXPECTED_GRAPH_REVISION = (
    "sha256:e933b3c39ee2c8fe1e1f0f5c8133826d277e5664ff5448093624d5e348030704"
)
_MODEL_ASSET_SCHEMA_REVISION = (
    "sha256:0820a2fbe627f2ccc5b76ff8d5348137d796893d3bd67707f7171fb420ec671c"
)

_TRUSTED_PROJECTIONS = (
    "RevisionVerifiedDeploymentArtifact",
    "RevisionVerifiedDeploymentApplicabilityInputs",
    "GraphRevisionVerifiedCellGraph",
    "VerifiedSiteOverlayArtifactProjection",
    "QualifiedSelectedSiteOverlay",
    "ModelSourceRetrievalPlan",
    "ModelSourceIr",
    "SourceVerifiedModelAsset",
    "CompositionQualifiedCellIr",
    "ApplicabilityQualifiedCellIr",
    "CompilerQualifiedCellIr",
)
_PREDECESSORS = _TRUSTED_PROJECTIONS[:-1]
_EXPECTED_PROPERTIES: dict[str, tuple[str, ...]] = {
    "RevisionVerifiedDeploymentArtifact": (
        "applicability_inputs",
        "assistant_was_present",
        "catalog_records",
        "cell_id",
        "deployment_artifact_id",
        "deployment_revision",
        "description",
        "graph",
        "name",
        "normalized_authored_canonical_json",
        "normalized_authored_graph_canonical_json",
    ),
    "RevisionVerifiedDeploymentApplicabilityInputs": (
        "assistant_settings",
        "external_resources_was_present",
        "normalized_capability_provider_bindings_canonical_json",
        "normalized_desired_control_configurations_canonical_json",
        "normalized_external_resources_canonical_json",
        "normalized_site_overlay_ref_canonical_json",
        "site_overlay_ref_presence",
        "value",
    ),
    "GraphRevisionVerifiedCellGraph": ("graph", "normalized_canonical_json"),
    "VerifiedSiteOverlayArtifactProjection": (
        "external_resource_keys",
        "reference",
    ),
    "QualifiedSelectedSiteOverlay": (
        "external_resource_keys",
        "has_selection",
        "reference",
    ),
    "ModelSourceRetrievalPlan": (
        "artifacts",
        "qualified_asset",
        "total_bytes",
    ),
    "ModelSourceIr": (
        "artifact_references",
        "semantic_index",
        "srdf",
        "urdf",
    ),
    "SourceVerifiedModelAsset": (
        "qualified_asset",
        "retrieval_plan",
        "source_ir",
        "value",
        "verified_artifacts",
    ),
    "CompositionQualifiedCellIr": (
        "cell_frame",
        "component_types",
        "control_binding_coordinate_maps",
        "coordinates",
        "fixed_connections",
        "flattened_srdf",
        "flattened_urdf",
        "frame_paths",
        "frames",
        "graph",
        "model_assets",
        "model_occurrences",
        "planning_resource_rewrites",
        "reference_rewrites",
        "reverse_symbol_rewrites",
        "symbol_rewrites",
        "targets",
        "verified_graph",
    ),
    "ApplicabilityQualifiedCellIr": (
        "assistant_settings",
        "binding_interfaces",
        "capabilities",
        "capability_definitions",
        "command_schemas",
        "composition",
        "control_interface_contracts",
        "controller_topologies",
        "controllers",
        "deployment_inputs",
        "external_resources",
        "provider_endpoints",
        "provider_manifests",
        "routes",
        "runtime_profiles",
        "selected_site_overlay_ref",
        "stop_applicability",
        "stop_evidence_policies",
    ),
    "CompilerQualifiedCellIr": ("applicability", "catalog_record_refs"),
}
_EXPECTED_ENUMS = {
    "CapabilityEndpointSourceKind": {
        "kExternalResource": 1,
        "kRuntimeUnit": 0,
    },
    "CatalogSchemaQualification": {"kQualified": 1, "kUnqualified": 0},
    "ClosedSchemaPurpose": {"kArtifactDocument": 0, "kValue": 1},
    "ClosedSchemaRootShape": {
        "kArray": 2,
        "kObject": 1,
        "kScalar": 0,
        "kTaggedUnion": 3,
    },
    "CompositionCoordinateUnit": {
        "kFloatingMetersRadians": 4,
        "kMeters": 2,
        "kNone": 0,
        "kPlanarMetersRadians": 3,
        "kRadians": 1,
    },
    "ExternalResourceKind": {
        "kContentStore": 1,
        "kDataSource": 2,
        "kModelEndpoint": 0,
        "kServiceEndpoint": 3,
    },
    "ModelSourceArtifactKind": {
        "kColladaMesh": 3,
        "kJpegTexture": 5,
        "kPngTexture": 4,
        "kSrdfXmlEntrypoint": 1,
        "kStlMesh": 2,
        "kUrdfXmlEntrypoint": 0,
    },
    "OccurrenceReferenceKind": {
        "kJointChildLink": 1,
        "kJointMimic": 2,
        "kJointParentLink": 0,
        "kSrdfChainBaseLink": 6,
        "kSrdfChainTipLink": 7,
        "kSrdfDisabledCollisionFirstLink": 15,
        "kSrdfDisabledCollisionSecondLink": 16,
        "kSrdfEndEffectorGroup": 12,
        "kSrdfEndEffectorParentGroup": 13,
        "kSrdfEndEffectorParentLink": 11,
        "kSrdfGroupJoint": 5,
        "kSrdfGroupLink": 4,
        "kSrdfGroupStateGroup": 9,
        "kSrdfGroupStateJoint": 10,
        "kSrdfPassiveJoint": 14,
        "kSrdfSubgroup": 8,
        "kVisualMaterial": 3,
    },
    "OccurrenceSymbolKind": {
        "kCollision": 6,
        "kJoint": 4,
        "kLink": 3,
        "kMaterial": 2,
        "kModel": 0,
        "kRobot": 1,
        "kSrdfEndEffector": 9,
        "kSrdfGroup": 7,
        "kSrdfGroupState": 8,
        "kVisual": 5,
    },
    "SiteOverlayRefSourcePresence": {
        "kAbsent": 0,
        "kExplicitNull": 1,
        "kValue": 2,
    },
    "SourceArtifactReferenceKind": {
        "kMesh": 0,
        "kTexture": 1,
    },
    "SrdfExpansionProvenanceKind": {
        "kChainJoint": 3,
        "kChainLink": 2,
        "kDirectJoint": 1,
        "kDirectLink": 0,
        "kSubgroup": 4,
    },
    "UrdfCoordinateKind": {
        "kAngular": 1,
        "kFloating": 4,
        "kLinear": 2,
        "kNone": 0,
        "kPlanar": 3,
    },
    "UrdfJointType": {
        "kContinuous": 1,
        "kFixed": 3,
        "kFloating": 4,
        "kPlanar": 5,
        "kPrismatic": 2,
        "kRevolute": 0,
    },
}
_EXPECTED_CONSTRUCTIBLE_DTO_PROPERTIES: dict[str, tuple[str, ...]] = {
    "CapabilityProviderBindingDTO": (
        "capability_definition_revision",
        "capability_id",
        "capability_provider_binding_id",
        "capability_provider_binding_revision",
        "endpoint_bindings",
        "provider_instance_id",
        "provider_manifest_id",
        "provider_manifest_revision",
        "scope",
        "target",
    ),
    "CapabilityProviderEndpointBindingDTO": (
        "endpoint_role_id",
        "source_endpoint_role_id",
        "source_id",
        "source_kind",
    ),
    "CellGlobalCapabilityProviderTargetDTO": ("cell_id",),
    "ControlDomainCapabilityProviderTargetDTO": (
        "control_domain_id",
        "control_domain_revision",
        "controller_topology_id",
        "topology_revision",
    ),
    "DeploymentApplicabilityInputsDTO": (
        "capability_provider_bindings",
        "desired_control_configurations",
        "external_resources",
        "site_overlay_ref",
    ),
    "DesiredControlBindingRouteDTO": (
        "control_binding_id",
        "control_binding_revision",
        "control_route_id",
    ),
    "DesiredControlConfigurationDTO": (
        "activation_set_id",
        "binding_routes",
        "control_domain_id",
        "control_domain_revision",
        "controller_topology_id",
        "topology_revision",
    ),
    "ExternalResourceDTO": (
        "endpoint_role_id",
        "external_resource_alias",
        "provider_instance_id",
        "provider_manifest_id",
        "provider_manifest_revision",
        "resource_kind",
        "site_overlay_value_ref",
    ),
    "ResolvedModelSourceArtifactCandidate": (
        "logical_uri",
        "resolved_store_key",
        "stored_bytes",
    ),
    "SiteOverlayExternalResourceKeyDTO": (
        "resource_kind",
        "site_overlay_value_ref",
    ),
    "SiteOverlayRefDTO": (
        "non_secret_digest",
        "site_overlay_id",
        "site_overlay_revision",
    ),
    "TargetBindingCapabilityProviderTargetDTO": (
        "command_target_id",
        "component_type_revision",
        "control_binding_id",
        "control_binding_revision",
        "controllable_component_id",
    ),
}
_EXPECTED_CONSTRUCTOR_PARAMETERS: dict[str, tuple[str, ...]] = {
    "CapabilityProviderBindingDTO": (
        "capability_provider_binding_id",
        "capability_provider_binding_revision",
        "capability_id",
        "capability_definition_revision",
        "scope",
        "target",
        "provider_instance_id",
        "provider_manifest_id",
        "provider_manifest_revision",
        "endpoint_bindings",
    ),
    "CapabilityProviderEndpointBindingDTO": (
        "endpoint_role_id",
        "source_kind",
        "source_id",
        "source_endpoint_role_id",
    ),
    "CellGlobalCapabilityProviderTargetDTO": ("cell_id",),
    "ControlDomainCapabilityProviderTargetDTO": (
        "control_domain_id",
        "control_domain_revision",
        "controller_topology_id",
        "topology_revision",
    ),
    "DeploymentApplicabilityInputsDTO": (
        "site_overlay_ref",
        "desired_control_configurations",
        "capability_provider_bindings",
        "external_resources",
    ),
    "DesiredControlBindingRouteDTO": (
        "control_binding_id",
        "control_binding_revision",
        "control_route_id",
    ),
    "DesiredControlConfigurationDTO": (
        "control_domain_id",
        "control_domain_revision",
        "controller_topology_id",
        "topology_revision",
        "activation_set_id",
        "binding_routes",
    ),
    "ExternalResourceDTO": (
        "external_resource_alias",
        "resource_kind",
        "provider_instance_id",
        "provider_manifest_id",
        "provider_manifest_revision",
        "endpoint_role_id",
        "site_overlay_value_ref",
    ),
    "ResolvedModelSourceArtifactCandidate": (
        "logical_uri",
        "resolved_store_key",
        "stored_bytes",
    ),
    "SiteOverlayExternalResourceKeyDTO": (
        "site_overlay_value_ref",
        "resource_kind",
    ),
    "SiteOverlayRefDTO": (
        "site_overlay_id",
        "site_overlay_revision",
        "non_secret_digest",
    ),
    "TargetBindingCapabilityProviderTargetDTO": (
        "controllable_component_id",
        "component_type_revision",
        "command_target_id",
        "control_binding_id",
        "control_binding_revision",
    ),
}
_EXPECTED_UNION_ALIASES: dict[str, tuple[str, ...]] = {
    "CapabilityEndpointSourceIdDTO": (
        "RuntimeUnitId",
        "ExternalResourceAlias",
    ),
    "CapabilityProviderTargetDTO": (
        "CellGlobalCapabilityProviderTargetDTO",
        "TargetBindingCapabilityProviderTargetDTO",
        "ControlDomainCapabilityProviderTargetDTO",
    ),
    "CompositionConnectionSource": (
        "CompositionDirectModelAnchor",
        "CompositionDirectComponentAnchor",
        "CompositionModelMount",
        "CompositionComponentAttachment",
    ),
    "CompositionFramePathSegment": (
        "CompositionFixedPathSegment",
        "CompositionModelJointPathSegment",
    ),
    "CompositionResolvedFrame": (
        "CompositionCellFrame",
        "CompositionComponentModelFrame",
        "CompositionPassiveModelFrame",
    ),
    "LogicalEndpointSourceTemplate": (
        "RuntimeEndpointSourceTemplate",
        "ExternalEndpointSourceTemplate",
    ),
    "LogicalStopApplicabilityTemplate": (
        "LogicalNotStoppableApplicabilityTemplate",
        "LogicalStoppableApplicabilityTemplate",
    ),
    "LogicalStopScopeTemplate": (
        "LogicalResourceStopScopeTemplate",
        "LogicalControllerSetStopScopeTemplate",
        "LogicalControlDomainStopScopeTemplate",
    ),
    "QualifiedCapabilityOwner": (
        "ControlBindingCapabilityOwner",
        "CapabilityProviderCapabilityOwner",
    ),
    "SrdfGroupMemberIr": (
        "SrdfLinkGroupMemberIr",
        "SrdfJointGroupMemberIr",
        "SrdfChainGroupMemberIr",
        "SrdfSubgroupMemberIr",
    ),
    "UrdfGeometryIr": (
        "UrdfBoxGeometryIr",
        "UrdfCylinderGeometryIr",
        "UrdfSphereGeometryIr",
        "UrdfMeshGeometryIr",
    ),
}
_FORBIDDEN_EXPORTS = frozenset(
    {
        "StructurallyQualifiedCellGraph",
        "TrustedCellGraph",
        "TrustedCellGraphCandidate",
        "ParsedModelSourceIr",
        "ParsedUrdfJoint",
        "ParsedUrdfJointType",
        "ParsedSrdfChain",
        "ParsedSrdfDisabledCollision",
        "ParsedSrdfEndEffector",
        "ParsedSrdfGroup",
        "ParsedSrdfGroupState",
        "ParsedSrdfGroupStateJoint",
    }
)
_FORBIDDEN_MODULES = frozenset(
    {"manyforge_roboplan_py", "e0_nanobind", "e0_pybind"}
)
_ROS_PREFIXES = (
    "ament",
    "control_msgs",
    "controller_manager",
    "gazebo",
    "geometry_msgs",
    "ignition",
    "isaac",
    "launch_ros",
    "moveit",
    "rcl",
    "ros",
    "ros2",
    "sensor_msgs",
    "std_msgs",
    "tf2_ros",
    "xarm",
)
_FORBIDDEN_ELF_DEPENDENCY = re.compile(
    r"(?:ament|gazebo|geometry_msgs|ignition|isaac|moveit|pinocchio|rcl|ros|"
    r"sensor_msgs|tf2|xarm)",
    re.IGNORECASE,
)
_SANITIZER_FAILURE = re.compile(
    r"(?:AddressSanitizer|LeakSanitizer|ThreadSanitizer|"
    r"UndefinedBehaviorSanitizer|runtime error:|SUMMARY: .*Sanitizer)",
    re.IGNORECASE,
)

# Frozen copies of the manifest's actual-source scans.  The qualification test
# does not import scan rules from the candidate tree it is judging.
_SCAN_CONTRACTS = (
    (
        "stale_public_structure_header",
        "line",
        ("manyforge_core/python", "manyforge_deployment/include", "manyforge_roboplan"),
        r"manyforge_deployment/cell_structure\.hpp",
        ("*.cpp", "*.hpp", "CMakeLists.txt"),
        ("manyforge_core/python/tests/**",),
    ),
    (
        "public_or_python_structural_stage",
        "line",
        ("manyforge_core/python", "manyforge_deployment/include", "manyforge_roboplan"),
        r"StructurallyQualifiedCellGraph",
        ("*.cpp", "*.hpp", "*.py", "*.pyi", "CMakeLists.txt"),
        ("manyforge_core/python/tests/**",),
    ),
    (
        "deleted_roboplan_module",
        "line",
        ("manyforge_core", "manyforge_deployment", "manyforge_roboplan", "scripts"),
        r"manyforge_roboplan_py",
        ("*.cpp", "*.hpp", "*.py", "*.sh", "CMakeLists.txt"),
        ("manyforge_core/python/tests/**",),
    ),
    (
        "trusted_graph_alias",
        "line",
        ("manyforge_core", "manyforge_deployment", "manyforge_roboplan"),
        r"TrustedCellGraph(Candidate)?",
        ("*.cpp", "*.hpp", "*.py", "*.pyi"),
        ("manyforge_core/python/tests/**",),
    ),
    (
        "old_parsed_source_api",
        "line",
        ("manyforge_core/python", "manyforge_deployment/include"),
        r"Parsed(ModelSourceIr|Urdf|Srdf)|parsed_ir\(",
        ("*.cpp", "*.hpp", "*.py", "*.pyi"),
        ("manyforge_core/python/tests/**",),
    ),
    (
        "old_experimental_import",
        "line",
        ("manyforge_core", "manyforge_deployment", "manyforge_roboplan", "scripts"),
        r"manyforge\.experimental\.composable_cell\.i1a\.v0",
        ("*.cpp", "*.hpp", "*.py", "*.pyi", "*.sh", "CMakeLists.txt"),
        ("manyforge_core/python/tests/**",),
    ),
    (
        "trusted_stage_python_constructor",
        "file",
        ("manyforge_core/python",),
        r"(?s)py::class_<\s*(?:deployment::)?(?:CompositionQualifiedCellIr|ApplicabilityQualifiedCellIr|CompilerQualifiedCellIr)\b(?:(?!py::class_<).)*?\.def\(\s*py::init",
        ("*.cpp", "*.hpp"),
        ("manyforge_core/python/tests/**",),
    ),
    (
        "test_only_final_trust_bypass",
        "file",
        (
            "manyforge_deployment/include",
            "manyforge_deployment/src",
            "manyforge_deployment/test",
            "manyforge_core/python",
        ),
        r"(?is)(?:friend[^;]{0,300}(?:Test|Fixture|Proxy|Access)[^;]*(?:CompositionQualifiedCellIr|ApplicabilityQualifiedCellIr|CompilerQualifiedCellIr)[^;]*;|friend[^;]{0,300}(?:CompositionQualifiedCellIr|ApplicabilityQualifiedCellIr|CompilerQualifiedCellIr)[^;]*(?:Test|Fixture|Proxy|Access)[^;]*;|(?:TestOnly|ForTest|test_only)[A-Za-z0-9_:<> ]{0,160}(?:CompositionQualifiedCellIr|ApplicabilityQualifiedCellIr|CompilerQualifiedCellIr)|(?:CompositionQualifiedCellIr|ApplicabilityQualifiedCellIr|CompilerQualifiedCellIr)[A-Za-z0-9_:<> ]{0,160}(?:TestAccess|TestProxy|ForTest))",
        ("*.cpp", "*.hpp", "*.py", "*.pyi"),
        (),
    ),
    (
        "ros_or_backend_header_in_core_binding",
        "line",
        ("manyforge_core/python",),
        r'#include\s*[<"](?:rclcpp|rclpy|moveit|roboplan|pinocchio|coal|geometry_msgs|sensor_msgs)/',
        ("*.cpp", "*.hpp"),
        ("manyforge_core/python/tests/**",),
    ),
    (
        "direct_graph_to_retention_bypass",
        "file",
        (
            "manyforge_deployment/include",
            "manyforge_deployment/src",
            "manyforge_core/python",
            "manyforge_roboplan",
        ),
        r"(?s)(?:\b(?:retain|materializ|build_and_retain)[A-Za-z0-9_]*\s*\([^;{}]{0,1000}\bGraphRevisionVerifiedCellGraph\b|\bGraphRevisionVerifiedCellGraph\b[^;{}]{0,1000}\b(?:retain|materializ|build_and_retain)[A-Za-z0-9_]*\s*\()",
        ("*.cpp", "*.hpp", "*.py", "*.pyi"),
        ("manyforge_core/python/tests/**",),
    ),
    (
        "compatibility_fallback_or_alias",
        "file",
        ("manyforge_core/python", "manyforge_deployment/include"),
        r"(?i)(?:legacy|deprecated|compatib(?:ility|le))[\s_-]*(?:alias|fallback|overload)|fallback[\s_-]*import",
        ("*.cpp", "*.hpp", "*.py", "*.pyi", "CMakeLists.txt"),
        ("manyforge_core/python/tests/**",),
    ),
)

_LOADED_EXTENSION: ModuleType | None = None
_LOADED_PATH: Path | None = None
_LOADED_SHA256: str | None = None
_EVIDENCE_LOCK = threading.Lock()
_EVIDENCE_ROOT_IDENTITY: tuple[int, int] | None = None
_WRITTEN_EVIDENCE: set[str] = set()
_EVIDENCE_DIGESTS: dict[str, str] = {}


def _fail(message: str) -> NoReturn:
    raise AssertionError(message)


def _absolute_path(variable: str) -> Path:
    raw = os.environ.get(variable)
    if raw is None or not raw.strip():
        _fail(f"{variable} must name an explicit absolute qualification path")
    path = Path(raw)
    if not path.is_absolute():
        _fail(f"{variable} must be absolute: {raw!r}")
    return Path(os.path.abspath(path))


def _plain_directory(variable: str) -> Path:
    path = _absolute_path(variable)
    try:
        metadata = path.lstat()
    except OSError as error:
        _fail(f"{variable} is unavailable: {path}: {error}")
    if not stat.S_ISDIR(metadata.st_mode) or stat.S_ISLNK(metadata.st_mode):
        _fail(f"{variable} is not a plain directory: {path}")
    try:
        if path.resolve(strict=True) != path:
            _fail(f"{variable} traverses a symlink: {path}")
    except OSError as error:
        _fail(f"{variable} cannot be resolved: {path}: {error}")
    return path


def _plain_file(path: Path, *, maximum: int, allow_empty: bool = False) -> Path:
    try:
        metadata = path.lstat()
    except OSError as error:
        _fail(f"qualification file is unavailable: {path}: {error}")
    if (
        not stat.S_ISREG(metadata.st_mode)
        or stat.S_ISLNK(metadata.st_mode)
        or metadata.st_nlink != 1
    ):
        _fail(f"qualification file is not regular, unique, and non-symlink: {path}")
    minimum = 0 if allow_empty else 1
    if metadata.st_size < minimum or metadata.st_size > maximum:
        _fail(f"qualification file size is outside its bound: {path}")
    try:
        if path.resolve(strict=True) != path:
            _fail(f"qualification file traverses a symlink: {path}")
    except OSError as error:
        _fail(f"qualification file cannot be resolved: {path}: {error}")
    return path


def _extension_path() -> Path:
    path = _plain_file(
        _absolute_path(_EXTENSION_ENV), maximum=_MAX_EXTENSION_BYTES
    )
    suffix = sysconfig.get_config_var("EXT_SUFFIX")
    if not isinstance(suffix, str) or not suffix:
        _fail("active CPython has no exact extension suffix")
    if path.name != f"{_MODULE_NAME}{suffix}":
        _fail(f"candidate extension does not match active CPython ABI: {path.name}")
    with path.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            _fail(f"candidate extension is not ELF: {path}")
    return path


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _sha256_value(value: Any) -> str:
    return "sha256:" + hashlib.sha256(_canonical_bytes(value)).hexdigest()


def _canonical_bytes(value: Any) -> bytes:
    try:
        return json.dumps(
            value,
            ensure_ascii=False,
            allow_nan=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    except (TypeError, ValueError) as error:
        _fail(f"test-owned observation is not strict JSON: {error}")


def _load_extension() -> ModuleType:
    global _LOADED_EXTENSION, _LOADED_PATH, _LOADED_SHA256
    path = _extension_path()
    before = _sha256_file(path)
    if _LOADED_EXTENSION is not None:
        if _LOADED_PATH != path or _LOADED_SHA256 != before:
            _fail("candidate extension path or bytes changed during pytest")
        return _LOADED_EXTENSION
    existing = sys.modules.get(_MODULE_NAME)
    if existing is not None:
        if not isinstance(existing, ModuleType):
            _fail("manyforge_core_py was prepopulated with a non-module object")
        module = existing
    else:
        spec = importlib.util.spec_from_file_location(_MODULE_NAME, path)
        if spec is None or spec.loader is None:
            _fail(f"cannot form an import spec for {path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[_MODULE_NAME] = module
        try:
            spec.loader.exec_module(module)
        except BaseException:
            sys.modules.pop(_MODULE_NAME, None)
            raise
    origin = getattr(getattr(module, "__spec__", None), "origin", None)
    if (
        not isinstance(module.__file__, str)
        or Path(module.__file__).resolve() != path
        or not isinstance(origin, str)
        or Path(origin).resolve() != path
    ):
        _fail("loaded module __file__/__spec__.origin differs from the exact candidate")
    after = _sha256_file(path)
    if before != after:
        _fail("candidate extension bytes changed while loading")
    _LOADED_EXTENSION, _LOADED_PATH, _LOADED_SHA256 = module, path, after
    return module


def _extension_sha256() -> str:
    _load_extension()
    if _LOADED_SHA256 is None:
        _fail("candidate extension digest was not retained after import")
    return _LOADED_SHA256


def _callable(module: ModuleType, name: str) -> Callable[..., Any]:
    value = getattr(module, name, None)
    if not callable(value):
        _fail(f"candidate extension omits required production function {name}")
    return value


def _properties(value_type: type[Any]) -> tuple[str, ...]:
    return tuple(
        sorted(
            name
            for name, descriptor in vars(value_type).items()
            if not name.startswith("_") and isinstance(descriptor, property)
        )
    )


def _publishes_constructor_signature(value_type: type[Any]) -> bool:
    return bool(_documented_constructor_contracts(value_type))


def _split_signature_parameters(value: str) -> list[str]:
    result: list[str] = []
    start = 0
    stack: list[str] = []
    quote: str | None = None
    escaped = False
    pairs = {")": "(", "]": "[", "}": "{"}
    for index, character in enumerate(value):
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {"'", '"'}:
            quote = character
        elif character in "([{":
            stack.append(character)
        elif character in pairs:
            if stack and stack[-1] == pairs[character]:
                stack.pop()
        elif character == "," and not stack:
            result.append(value[start:index].strip())
            start = index + 1
    result.append(value[start:].strip())
    return [item for item in result if item]


def _has_top_level_default(parameter: str) -> bool:
    stack: list[str] = []
    quote: str | None = None
    escaped = False
    pairs = {")": "(", "]": "[", "}": "{"}
    for character in parameter:
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {"'", '"'}:
            quote = character
        elif character in "([{":
            stack.append(character)
        elif character in pairs:
            if stack and stack[-1] == pairs[character]:
                stack.pop()
        elif character == "=" and not stack:
            return True
    return False


def _documented_constructor_contracts(
    value_type: type[Any],
) -> tuple[tuple[tuple[str, ...], tuple[str, ...]], ...]:
    constructor = vars(value_type).get("__init__")
    document = getattr(constructor, "__doc__", "") or ""
    starts = list(re.finditer(r"(?m)(?:^\d+\.\s*)?__init__\(", document))
    contracts: list[tuple[tuple[str, ...], tuple[str, ...]]] = []
    for match in starts:
        open_index = document.find("(", match.start())
        depth = 0
        quote: str | None = None
        escaped = False
        close_index: int | None = None
        for index in range(open_index, len(document)):
            character = document[index]
            if quote is not None:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    quote = None
                continue
            if character in {"'", '"'}:
                quote = character
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0:
                    close_index = index
                    break
        if close_index is None:
            continue
        names: list[str] = []
        defaults: list[str] = []
        valid = True
        for parameter in _split_signature_parameters(
            document[open_index + 1 : close_index]
        ):
            name = re.split(r"[:=]", parameter, maxsplit=1)[0].strip()
            if name == "self":
                continue
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
                valid = False
                break
            names.append(name)
            if _has_top_level_default(parameter):
                defaults.append(name)
        if valid:
            contract = (tuple(names), tuple(defaults))
            if contract not in contracts:
                contracts.append(contract)
    return tuple(contracts)


def _documented_constructor_contract(
    value_type: type[Any],
) -> tuple[tuple[str, ...], tuple[str, ...]]:
    contracts = _documented_constructor_contracts(value_type)
    if len(contracts) != 1:
        _fail(
            f"expected one detailed constructor signature for {value_type.__name__}: "
            f"{contracts}"
        )
    return contracts[0]


def _small_observation(value: Any) -> Any:
    if value is None or isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, bytes):
        return {"type": "bytes", "sha256": hashlib.sha256(value).hexdigest(), "size": len(value)}
    if isinstance(value, (tuple, list)):
        return {"type": type(value).__name__, "length": len(value)}
    if isinstance(value, dict):
        return {"type": "dict", "length": len(value)}
    member_map = getattr(type(value), "__members__", None)
    if isinstance(member_map, dict):
        for name, member in member_map.items():
            if member == value:
                return {"type": type(value).__name__, "member": name}
    nested = getattr(value, "value", None)
    if isinstance(nested, (bool, int, float, str)):
        return {"type": type(value).__name__, "value": nested}
    return {"type": type(value).__name__}


def _write_case_evidence(
    case_id: str, measurements: dict[str, Any], observations: Any
) -> None:
    global _EVIDENCE_ROOT_IDENTITY
    filename = f"{case_id}.json"
    payload = {
        "schema_version": "manyforge.composable_cell_i2.direct_binding_case.v1",
        "case_id": case_id,
        "node_id": _NODE_PREFIX + case_id,
        "extension_sha256": _extension_sha256(),
        "measurements": measurements,
        "observations": observations,
    }
    encoded = _canonical_bytes(payload) + b"\n"
    with _EVIDENCE_LOCK:
        root = _plain_directory(_EVIDENCE_ROOT_ENV)
        identity = (root.stat().st_dev, root.stat().st_ino)
        existing = {entry.name for entry in root.iterdir()}
        if _EVIDENCE_ROOT_IDENTITY is None:
            if existing:
                _fail("binding evidence root was not empty before the first case")
            _EVIDENCE_ROOT_IDENTITY = identity
        elif identity != _EVIDENCE_ROOT_IDENTITY:
            _fail("binding evidence root identity changed during pytest")
        if existing != _WRITTEN_EVIDENCE:
            _fail("binding evidence root contains a candidate-precreated or aliased entry")
        for prior_name, prior_digest in _EVIDENCE_DIGESTS.items():
            prior = _plain_file(root / prior_name, maximum=_MAX_GENERATED_BYTES)
            if _sha256_file(prior) != prior_digest:
                _fail(f"test-owned evidence changed after publication: {prior}")
        path = root / filename
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(path, flags, 0o600)
        except OSError as error:
            _fail(f"cannot exclusively create test-owned case evidence {path}: {error}")
        try:
            view = memoryview(encoded)
            while view:
                written = os.write(descriptor, view)
                if written <= 0:
                    _fail(f"short write to test-owned case evidence: {path}")
                view = view[written:]
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        metadata = path.lstat()
        if (
            not stat.S_ISREG(metadata.st_mode)
            or stat.S_ISLNK(metadata.st_mode)
            or metadata.st_nlink != 1
            or metadata.st_size != len(encoded)
        ):
            _fail(f"test-owned case evidence was aliased or changed: {path}")
        _WRITTEN_EVIDENCE.add(filename)
        _EVIDENCE_DIGESTS[filename] = _sha256_file(path)
        if {entry.name for entry in root.iterdir()} != _WRITTEN_EVIDENCE:
            _fail("binding evidence root changed after exclusive publication")


def _canonical_empty_pipeline(module: ModuleType) -> dict[str, Any]:
    parsed = _callable(module, "parse_strict_json_model_yaml")(
        _CANONICAL_EMPTY_INPUT
    )
    deployment = _callable(module, "compute_deployment_artifact_revision")(
        parsed, ()
    )
    composition = _callable(module, "compose_cell_occurrences")(
        deployment.graph, (), ()
    )
    deployment_inputs = deployment.applicability_inputs
    selected_overlay = _callable(module, "select_site_overlay_for_deployment")(
        deployment_inputs.value.site_overlay_ref,
        deployment_inputs.value.external_resources,
        None,
    )
    applicability = _callable(module, "qualify_cell_applicability")(
        composition,
        deployment_inputs,
        (),
        (),
        (),
        (),
        (),
        (),
        (),
        selected_overlay,
    )
    compiler = _callable(module, "compile_cell_ir")(applicability)
    values = {
        "deployment": deployment,
        "deployment_inputs": deployment_inputs,
        "graph": deployment.graph,
        "selected_overlay": selected_overlay,
        "composition": composition,
        "applicability": applicability,
        "compiler": compiler,
    }
    expected_types = {
        "deployment": "RevisionVerifiedDeploymentArtifact",
        "deployment_inputs": "RevisionVerifiedDeploymentApplicabilityInputs",
        "graph": "GraphRevisionVerifiedCellGraph",
        "selected_overlay": "QualifiedSelectedSiteOverlay",
        "composition": "CompositionQualifiedCellIr",
        "applicability": "ApplicabilityQualifiedCellIr",
        "compiler": "CompilerQualifiedCellIr",
    }
    for key, type_name in expected_types.items():
        if type(values[key]).__name__ != type_name:
            _fail(f"canonical-empty production stage {key} returned {type(values[key]).__name__}")
    return values


def _sha256_prefixed(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _verified_site_overlay_fixture(module: ModuleType) -> dict[str, Any]:
    value_ref = "resources.robot.endpoint"
    identity = {
        "api_version": "manyforge.site_overlay.v1",
        "site_overlay_id": "i2_binding_overlay",
        "values": [
            {
                "resource_kind": "service_endpoint",
                "semantic_kind": "external_resource",
                "site_overlay_value_ref": value_ref,
                "value": {
                    "value": "https://robot.invalid",
                    "value_kind": "string",
                },
            }
        ],
    }
    canonical_identity = json.dumps(
        identity, sort_keys=True, separators=(",", ":")
    )
    revision = _sha256_prefixed(
        b"manyforge.content-identity.v1\x00"
        b"site-overlay-revision\x00"
        b"manyforge.site_overlay.v1\x00"
        + canonical_identity.encode()
    )
    normalized_root = dict(identity)
    normalized_root["site_overlay_revision"] = revision
    stored_bytes = json.dumps(
        normalized_root, sort_keys=True, separators=(",", ":")
    )
    reference = module.SiteOverlayRefDTO(
        site_overlay_id=module.SiteOverlayId("i2_binding_overlay"),
        site_overlay_revision=module.SiteOverlayRevision(revision),
        non_secret_digest=module.ContentDigest(
            _sha256_prefixed(stored_bytes.encode())
        ),
    )
    projection = _callable(
        module, "resolve_and_verify_site_overlay_artifact_projection"
    )(reference, stored_bytes)
    resource = module.ExternalResourceDTO(
        external_resource_alias=module.ExternalResourceAlias("robot_endpoint"),
        resource_kind=module.ExternalResourceKind.kServiceEndpoint,
        provider_instance_id=module.ProviderInstanceId("provider"),
        provider_manifest_id=module.ProviderManifestId("manifest"),
        provider_manifest_revision=module.ProviderManifestRevision(_ZERO_DIGEST),
        endpoint_role_id=module.EndpointRoleId("endpoint"),
        site_overlay_value_ref=module.SiteOverlayValueRef(value_ref),
    )
    selection = _callable(module, "select_site_overlay_for_deployment")(
        reference, (resource,), projection
    )
    if type(projection).__name__ != "VerifiedSiteOverlayArtifactProjection":
        _fail("ordinary site-overlay verifier returned the wrong type")
    if type(selection).__name__ != "QualifiedSelectedSiteOverlay":
        _fail("ordinary site-overlay selector returned the wrong type")
    return {
        "projection": projection,
        "reference": reference,
        "resource": resource,
        "selection": selection,
    }


def _constructible_dto_fixture(module: ModuleType) -> dict[str, Any]:
    route_a_kwargs = {
        "control_binding_id": module.ControlBindingId("binding_a"),
        "control_binding_revision": module.ControlBindingRevision(_ZERO_DIGEST),
        "control_route_id": module.ControlRouteId("route_a"),
    }
    route_b_kwargs = {
        "control_binding_id": module.ControlBindingId("binding_b"),
        "control_binding_revision": module.ControlBindingRevision(_ZERO_DIGEST),
        "control_route_id": module.ControlRouteId("route_b"),
    }
    route_a = module.DesiredControlBindingRouteDTO(**route_a_kwargs)
    route_b = module.DesiredControlBindingRouteDTO(**route_b_kwargs)

    configuration_kwargs = {
        "control_domain_id": module.ControlDomainId("domain"),
        "control_domain_revision": module.ControlDomainRevision(_ZERO_DIGEST),
        "controller_topology_id": module.ControllerTopologyId("topology"),
        "topology_revision": module.TopologyRevision(_ZERO_DIGEST),
        "activation_set_id": module.ActivationSetId("activation"),
        "binding_routes": (route_b, route_a),
    }
    configuration = module.DesiredControlConfigurationDTO(
        **configuration_kwargs
    )

    cell_target_kwargs = {"cell_id": module.CellId("cell")}
    cell_target = module.CellGlobalCapabilityProviderTargetDTO(
        **cell_target_kwargs
    )
    target_binding_kwargs = {
        "controllable_component_id": module.ControllableComponentId("component"),
        "component_type_revision": module.ComponentTypeRevision(_ZERO_DIGEST),
        "command_target_id": module.CommandTargetId("command_target"),
        "control_binding_id": module.ControlBindingId("binding_a"),
        "control_binding_revision": module.ControlBindingRevision(_ZERO_DIGEST),
    }
    target_binding = module.TargetBindingCapabilityProviderTargetDTO(
        **target_binding_kwargs
    )
    domain_target_kwargs = {
        "control_domain_id": module.ControlDomainId("domain"),
        "control_domain_revision": module.ControlDomainRevision(_ZERO_DIGEST),
        "controller_topology_id": module.ControllerTopologyId("topology"),
        "topology_revision": module.TopologyRevision(_ZERO_DIGEST),
    }
    domain_target = module.ControlDomainCapabilityProviderTargetDTO(
        **domain_target_kwargs
    )

    endpoint_runtime_kwargs = {
        "endpoint_role_id": module.EndpointRoleId("runtime_endpoint"),
        "source_kind": module.CapabilityEndpointSourceKind.kRuntimeUnit,
        "source_id": module.RuntimeUnitId("runtime"),
        "source_endpoint_role_id": module.EndpointRoleId("runtime_source"),
    }
    endpoint_external_kwargs = {
        "endpoint_role_id": module.EndpointRoleId("external_endpoint"),
        "source_kind": module.CapabilityEndpointSourceKind.kExternalResource,
        "source_id": module.ExternalResourceAlias("external_b"),
        "source_endpoint_role_id": module.EndpointRoleId("external_source"),
    }
    endpoint_runtime = module.CapabilityProviderEndpointBindingDTO(
        **endpoint_runtime_kwargs
    )
    endpoint_external = module.CapabilityProviderEndpointBindingDTO(
        **endpoint_external_kwargs
    )
    provider_binding_kwargs = {
        "capability_provider_binding_id": module.CapabilityProviderBindingId(
            "provider_binding"
        ),
        "capability_provider_binding_revision": (
            module.CapabilityProviderBindingRevision(_ZERO_DIGEST)
        ),
        "capability_id": module.CapabilityId("capability"),
        "capability_definition_revision": module.CapabilityDefinitionRevision(
            _ZERO_DIGEST
        ),
        "scope": module.CapabilityScope.kCellGlobal,
        "target": cell_target,
        "provider_instance_id": module.ProviderInstanceId("provider"),
        "provider_manifest_id": module.ProviderManifestId("manifest"),
        "provider_manifest_revision": module.ProviderManifestRevision(_ZERO_DIGEST),
        "endpoint_bindings": (endpoint_external, endpoint_runtime),
    }
    provider_binding = module.CapabilityProviderBindingDTO(
        **provider_binding_kwargs
    )

    overlay_reference_kwargs = {
        "site_overlay_id": module.SiteOverlayId("overlay"),
        "site_overlay_revision": module.SiteOverlayRevision(_ZERO_DIGEST),
        "non_secret_digest": module.ContentDigest(_ZERO_DIGEST),
    }
    overlay_reference = module.SiteOverlayRefDTO(**overlay_reference_kwargs)
    overlay_key_kwargs = {
        "site_overlay_value_ref": module.SiteOverlayValueRef(
            "resources.robot.endpoint"
        ),
        "resource_kind": module.ExternalResourceKind.kServiceEndpoint,
    }
    overlay_key = module.SiteOverlayExternalResourceKeyDTO(**overlay_key_kwargs)

    def external_kwargs(alias: str, value_ref: str) -> dict[str, Any]:
        return {
            "external_resource_alias": module.ExternalResourceAlias(alias),
            "resource_kind": module.ExternalResourceKind.kServiceEndpoint,
            "provider_instance_id": module.ProviderInstanceId("provider"),
            "provider_manifest_id": module.ProviderManifestId("manifest"),
            "provider_manifest_revision": module.ProviderManifestRevision(
                _ZERO_DIGEST
            ),
            "endpoint_role_id": module.EndpointRoleId("endpoint"),
            "site_overlay_value_ref": module.SiteOverlayValueRef(value_ref),
        }

    external_a_kwargs = external_kwargs(
        "external_a", "resources.external_a.endpoint"
    )
    external_b_kwargs = external_kwargs(
        "external_b", "resources.external_b.endpoint"
    )
    external_a = module.ExternalResourceDTO(**external_a_kwargs)
    external_b = module.ExternalResourceDTO(**external_b_kwargs)
    applicability_inputs_kwargs = {
        "site_overlay_ref": overlay_reference,
        "desired_control_configurations": (configuration,),
        "capability_provider_bindings": (provider_binding,),
        "external_resources": (external_b, external_a),
    }
    applicability_inputs = module.DeploymentApplicabilityInputsDTO(
        **applicability_inputs_kwargs
    )
    resolved_candidate_kwargs = {
        "logical_uri": "asset://i2-constructor/mesh.stl",
        "resolved_store_key": module.ContentAddressedStoreKey(
            "sha256/" + hashlib.sha256(b"i2-constructor").hexdigest()
        ),
        "stored_bytes": b"\x00i2-constructor\xff",
    }
    resolved_candidate = module.ResolvedModelSourceArtifactCandidate(
        **resolved_candidate_kwargs
    )

    kwargs_by_type = {
        "CapabilityProviderBindingDTO": provider_binding_kwargs,
        "CapabilityProviderEndpointBindingDTO": endpoint_runtime_kwargs,
        "CellGlobalCapabilityProviderTargetDTO": cell_target_kwargs,
        "ControlDomainCapabilityProviderTargetDTO": domain_target_kwargs,
        "DeploymentApplicabilityInputsDTO": applicability_inputs_kwargs,
        "DesiredControlBindingRouteDTO": route_a_kwargs,
        "DesiredControlConfigurationDTO": configuration_kwargs,
        "ExternalResourceDTO": external_a_kwargs,
        "ResolvedModelSourceArtifactCandidate": resolved_candidate_kwargs,
        "SiteOverlayExternalResourceKeyDTO": overlay_key_kwargs,
        "SiteOverlayRefDTO": overlay_reference_kwargs,
        "TargetBindingCapabilityProviderTargetDTO": target_binding_kwargs,
    }
    instances = {
        "CapabilityProviderBindingDTO": provider_binding,
        "CapabilityProviderEndpointBindingDTO": endpoint_runtime,
        "CellGlobalCapabilityProviderTargetDTO": cell_target,
        "ControlDomainCapabilityProviderTargetDTO": domain_target,
        "DeploymentApplicabilityInputsDTO": applicability_inputs,
        "DesiredControlBindingRouteDTO": route_a,
        "DesiredControlConfigurationDTO": configuration,
        "ExternalResourceDTO": external_a,
        "ResolvedModelSourceArtifactCandidate": resolved_candidate,
        "SiteOverlayExternalResourceKeyDTO": overlay_key,
        "SiteOverlayRefDTO": overlay_reference,
        "TargetBindingCapabilityProviderTargetDTO": target_binding,
    }
    ordering = {
        "binding_routes": [
            row.control_route_id.value for row in configuration.binding_routes
        ],
        "endpoint_bindings": [
            row.endpoint_role_id.value for row in provider_binding.endpoint_bindings
        ],
        "external_resources": [
            row.external_resource_alias.value
            for row in applicability_inputs.external_resources
        ],
    }
    return {
        "instances": instances,
        "kwargs": kwargs_by_type,
        "ordering": ordering,
    }


def _revisioned_document(
    identity: dict[str, Any], *, domain: str, revision_field: str
) -> str:
    canonical = json.dumps(identity, sort_keys=True, separators=(",", ":"))
    preimage = (
        b"manyforge.content-identity.v1"
        + b"\x00"
        + domain.encode()
        + b"\x00manyforge.model_asset.v1\x00"
        + canonical.encode()
    )
    result = dict(identity)
    result[revision_field] = _sha256_prefixed(preimage)
    return json.dumps(result, sort_keys=True, separators=(",", ":"))


def _verified_source_fixture(module: ModuleType) -> Any:
    urdf_uri = "asset://i2_binding_asset/urdf/model.urdf"
    srdf_uri = "asset://i2_binding_asset/srdf/model.srdf"
    mesh_uri = "asset://i2_binding_asset/meshes/base.stl"
    urdf = (
        '<robot name="i2_binding"><link name="base_link"><visual><geometry>'
        f'<mesh filename="{mesh_uri}"/></geometry></visual></link>'
        '<link name="tool0"/><joint name="joint1" type="revolute">'
        '<parent link="base_link"/><child link="tool0"/><axis xyz="0 0 1"/>'
        '<limit lower="-3" upper="3" effort="1" velocity="1"/>'
        "</joint></robot>"
    ).encode()
    srdf = (
        '<robot name="i2_binding"><group name="manipulator">'
        '<chain base_link="base_link" tip_link="tool0"/><joint name="joint1"/>'
        '</group><group_state name="home" group="manipulator">'
        '<joint name="joint1" value="0"/></group_state>'
        '<disable_collisions link1="base_link" link2="tool0" reason="Adjacent"/>'
        "</robot>"
    ).encode()
    blobs = {
        urdf_uri: ("application/xml", urdf),
        srdf_uri: ("application/xml", srdf),
        mesh_uri: ("model/stl", b"\x00\xffi2-binding-stl\x00\x80"),
    }
    artifacts = [
        {
            "logical_uri": uri,
            "media_type": media_type,
            "byte_size": len(data),
            "content_digest": _sha256_prefixed(data),
        }
        for uri, (media_type, data) in sorted(blobs.items())
    ]
    identity = {
        "api_version": "manyforge.model_asset.v1",
        "model_asset_id": "i2_binding_asset",
        "instance_cardinality": "multiple_per_cell",
        "expanded_source": {
            "urdf_logical_uri": urdf_uri,
            "srdf_logical_uri": srdf_uri,
            "artifacts": artifacts,
        },
        "root_frame_role_id": "base",
        "frame_role_bindings": [
            {"frame_role_id": "base", "source_link_name": "base_link"},
            {"frame_role_id": "tcp", "source_link_name": "tool0"},
        ],
        "model_group_role_bindings": [
            {"model_group_role_id": "arm", "source_group_name": "manipulator"}
        ],
        "resource_role_bindings": [
            {"resource_role_id": "arm_joints", "source_joint_names": ["joint1"]}
        ],
        "collision_body_role_bindings": [
            {
                "collision_body_role_id": "arm_collision",
                "source_link_names": ["base_link", "tool0"],
            }
        ],
    }
    document = _revisioned_document(
        identity,
        domain="catalog-record-revision/model_asset",
        revision_field="model_revision",
    )
    revision = json.loads(document)["model_revision"]
    catalog_key_text = "sha256/" + hashlib.sha256(b"i2-binding-catalog").hexdigest()
    catalog_key = module.ContentAddressedStoreKey(catalog_key_text)
    reference = module.ModelAssetCatalogRecordRefDTO(
        record_id=module.ModelAssetId("i2_binding_asset"),
        record_revision=module.ModelRevision(revision),
        schema_id=module.SchemaId("manyforge.model_asset.v1"),
        schema_revision=module.SchemaRevision(_MODEL_ASSET_SCHEMA_REVISION),
        content_digest=module.ContentDigest(_sha256_prefixed(document.encode())),
        content_addressed_store_key=catalog_key,
    )
    qualified = _callable(module, "resolve_and_qualify_model_asset")(
        reference, catalog_key, document
    )
    candidates = [
        module.ResolvedModelSourceArtifactCandidate(
            logical_uri=uri,
            resolved_store_key=module.ContentAddressedStoreKey(
                "sha256/" + hashlib.sha256(data).hexdigest()
            ),
            stored_bytes=data,
        )
        for uri, (_, data) in reversed(sorted(blobs.items()))
    ]
    value = _callable(module, "verify_model_source_closure")(
        qualified, candidates
    )
    if type(value).__name__ != "SourceVerifiedModelAsset":
        _fail("ordinary source-verification producer returned the wrong type")
    return value


def _compiler_observation(values: dict[str, Any]) -> dict[str, Any]:
    deployment = values["deployment"]
    graph = values["graph"].graph
    composition = values["composition"]
    applicability = values["applicability"]
    compiler = values["compiler"]
    return {
        "deployment_artifact_id": deployment.deployment_artifact_id.value,
        "deployment_revision": deployment.deployment_revision.value,
        "cell_id": graph.cell_id.value,
        "graph_revision": graph.graph_revision.value,
        "normalized_authored_canonical_json": deployment.normalized_authored_canonical_json,
        "composition": {
            "model_assets": len(composition.model_assets),
            "component_types": len(composition.component_types),
            "model_occurrences": len(composition.model_occurrences),
            "symbol_rewrites": len(composition.symbol_rewrites),
            "targets": len(composition.targets),
        },
        "applicability": {
            "selected_site_overlay_ref": _small_observation(
                applicability.selected_site_overlay_ref
            ),
            "runtime_profiles": len(applicability.runtime_profiles),
            "capability_definitions": len(applicability.capability_definitions),
            "binding_interfaces": len(applicability.binding_interfaces),
            "routes": len(applicability.routes),
            "capabilities": len(applicability.capabilities),
            "provider_endpoints": len(applicability.provider_endpoints),
            "external_resources": len(applicability.external_resources),
            "stop_applicability": len(applicability.stop_applicability),
        },
        "catalog_record_refs": len(compiler.catalog_record_refs),
    }


def _expected_compiler_observation() -> dict[str, Any]:
    return {
        "deployment_artifact_id": "c1_build_authority",
        "deployment_revision": _EXPECTED_DEPLOYMENT_REVISION,
        "cell_id": "c1_build_authority_cell",
        "graph_revision": _EXPECTED_GRAPH_REVISION,
        "normalized_authored_canonical_json": _CANONICAL_EMPTY_NORMALIZED,
        "composition": {
            "model_assets": 0,
            "component_types": 0,
            "model_occurrences": 0,
            "symbol_rewrites": 0,
            "targets": 0,
        },
        "applicability": {
            "selected_site_overlay_ref": None,
            "runtime_profiles": 0,
            "capability_definitions": 0,
            "binding_interfaces": 0,
            "routes": 0,
            "capabilities": 0,
            "provider_endpoints": 0,
            "external_resources": 0,
            "stop_applicability": 0,
        },
        "catalog_record_refs": 0,
    }


def _enum_observation(module: ModuleType) -> dict[str, dict[str, int]]:
    result: dict[str, dict[str, int]] = {}
    for enum_name in sorted(_EXPECTED_ENUMS):
        enum_type = getattr(module, enum_name, None)
        members = getattr(enum_type, "__members__", None)
        if not isinstance(members, dict):
            _fail(f"candidate binding omits enum inventory for {enum_name}")
        result[enum_name] = {
            name: int(getattr(member, "value"))
            for name, member in sorted(members.items())
        }
    return result


def _mutation_observations(instance: Any) -> tuple[int, list[dict[str, Any]]]:
    type_name = type(instance).__name__
    value_type = type(instance)
    observations: list[dict[str, Any]] = []
    for label, arguments in (("no_args", ()), ("one_positional", (None,))):
        try:
            value_type(*arguments)
        except TypeError as error:
            observations.append(
                {"operation": f"construct_{label}", "exception": type(error).__name__, "message": str(error)}
            )
        else:
            _fail(f"trusted projection is caller-constructible: {type_name} ({label})")
    properties = _properties(value_type)
    if properties != _EXPECTED_PROPERTIES[type_name]:
        _fail(f"trusted projection field inventory drifted: {type_name}: {properties}")
    write_attempts = 0
    for attribute in properties:
        current = getattr(instance, attribute)
        for operation in ("setattr", "delattr"):
            write_attempts += 1
            try:
                if operation == "setattr":
                    setattr(instance, attribute, current)
                else:
                    delattr(instance, attribute)
            except AttributeError as error:
                observations.append(
                    {
                        "operation": operation,
                        "attribute": attribute,
                        "exception": type(error).__name__,
                        "message": str(error),
                    }
                )
            else:
                _fail(f"trusted projection accepted {operation}: {type_name}.{attribute}")
    for operation in ("setattr", "delattr"):
        write_attempts += 1
        try:
            if operation == "setattr":
                setattr(instance, "qualification_only_field", 1)
            else:
                delattr(instance, "qualification_only_field")
        except AttributeError as error:
            observations.append(
                {
                    "operation": operation,
                    "attribute": "qualification_only_field",
                    "exception": type(error).__name__,
                    "message": str(error),
                }
            )
        else:
            _fail(f"trusted projection accepts dynamic fields: {type_name}")
    return write_attempts, observations


def _progress_probe(
    operation: Callable[[], Any], *, calls: int, expected_exception: type[BaseException] | None = None
) -> tuple[list[int], list[Any], int, int]:
    ready = threading.Event()
    begin = threading.Event()
    stop = threading.Event()
    counter = [0]

    def worker() -> None:
        ready.set()
        begin.wait()
        while not stop.is_set():
            counter[0] += 1
            time.sleep(0)

    thread = threading.Thread(target=worker, name="i2-binding-progress", daemon=True)
    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(10.0)
    thread.start()
    if not ready.wait(timeout=5.0):
        _fail("progress witness thread did not start")
    begin.set()
    observations: list[Any] = []
    deltas: list[int] = []
    try:
        for _ in range(calls):
            before = counter[0]
            if expected_exception is None:
                observations.append(operation())
            else:
                try:
                    operation()
                except expected_exception as error:
                    observations.append(
                        {"type": type(error).__name__, "message": str(error), "fields": dict(vars(error))}
                    )
                else:
                    _fail("released-GIL production failure path unexpectedly succeeded")
            deltas.append(counter[0] - before)
    finally:
        stop.set()
        sys.setswitchinterval(old_interval)
        thread.join(timeout=5.0)
    if thread.is_alive():
        _fail("progress witness thread did not stop")
    return deltas, observations, thread.ident or 0, threading.get_ident()


def _readelf(arguments: list[str], path: Path) -> str:
    try:
        result = subprocess.run(
            ["readelf", *arguments, str(path)],
            check=False,
            capture_output=True,
            text=True,
            timeout=30.0,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        _fail(f"readelf could not inspect the exact extension: {error}")
    if result.returncode != 0 or result.stderr:
        _fail(f"readelf failed for the exact extension: {result.stderr}")
    return result.stdout


def _module_is_ros(name: str) -> bool:
    return any(
        name == prefix
        or name.startswith(prefix + ".")
        or name.startswith(prefix + "_")
        for prefix in _ROS_PREFIXES
    )


def _generated_file(name: str) -> Path:
    root = _plain_directory(_GENERATED_ROOT_ENV)
    path = root / name
    if path.parent != root:
        _fail(f"generated output path escaped its exact root: {name}")
    return _plain_file(path, maximum=_MAX_GENERATED_BYTES)


def _strict_json(path: Path) -> dict[str, Any]:
    def no_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                _fail(f"generated JSON repeats key {key!r}: {path}")
            result[key] = value
        return result

    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=no_duplicates,
            parse_constant=lambda token: _fail(f"generated JSON has {token}: {path}"),
        )
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        _fail(f"generated output is not strict UTF-8 JSON: {path}: {error}")
    if not isinstance(value, dict):
        _fail(f"generated JSON root is not an object: {path}")
    return value


def _stub_semantics(tree: ast.Module) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and not node.name.startswith("_"):
            positional = [*node.args.posonlyargs, *node.args.args]
            default_offset = len(positional) - len(node.args.defaults)
            parameters = []
            for index, argument in enumerate(positional):
                default = None
                if index >= default_offset:
                    default = ast.unparse(node.args.defaults[index - default_offset])
                parameters.append({"name": argument.arg, "default": default})
            parameters.extend(
                {"name": argument.arg, "default": ast.unparse(default) if default else None}
                for argument, default in zip(node.args.kwonlyargs, node.args.kw_defaults)
            )
            row = result.setdefault(node.name, {"kind": "function", "signatures": []})
            row["signatures"].append(
                {"parameters": parameters, "returns": ast.unparse(node.returns) if node.returns else None}
            )
        elif isinstance(node, ast.ClassDef) and not node.name.startswith("_"):
            members: set[str] = set()
            signatures: dict[str, list[dict[str, Any]]] = {}
            for child in node.body:
                if isinstance(child, ast.AnnAssign) and isinstance(child.target, ast.Name):
                    if not child.target.id.startswith("_"):
                        members.add(child.target.id)
                elif isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    if child.name.startswith("_") and child.name != "__init__":
                        continue
                    members.add(child.name)
                    args = [*child.args.posonlyargs, *child.args.args]
                    if args and args[0].arg in {"self", "cls"}:
                        args = args[1:]
                    signatures.setdefault(child.name, []).append(
                        {"parameters": [argument.arg for argument in args]}
                    )
            result[node.name] = {
                "kind": "class",
                "members": sorted(members),
                "methods": signatures,
            }
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            if not node.target.id.startswith("_"):
                result[node.target.id] = {"kind": "constant", "annotation": ast.unparse(node.annotation)}
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and not target.id.startswith("_"):
                    result[target.id] = {"kind": "constant", "annotation": None}
    return result


def _scan_source() -> list[dict[str, Any]]:
    source_root = _plain_directory(_SOURCE_ROOT_ENV)
    results: list[dict[str, Any]] = []
    for scan_id, mode, roots, pattern, globs, excludes in _SCAN_CONTRACTS:
        expression = re.compile(pattern)
        matches: list[dict[str, Any]] = []
        seen_paths: set[str] = set()
        for relative_root in roots:
            root = source_root / relative_root
            if not root.is_dir() or root.is_symlink() or root.resolve() != root:
                _fail(f"forbidden scan root is missing or aliased: {relative_root}")
            for path in root.rglob("*"):
                if not path.is_file():
                    continue
                relative = path.relative_to(source_root).as_posix()
                if relative in seen_paths:
                    continue
                if not any(fnmatch.fnmatch(path.name, glob) for glob in globs):
                    continue
                if any(fnmatch.fnmatch(relative, excluded) for excluded in excludes):
                    continue
                seen_paths.add(relative)
                checked = _plain_file(path, maximum=_MAX_SOURCE_BYTES, allow_empty=True)
                try:
                    content = checked.read_text(encoding="utf-8")
                except (OSError, UnicodeError) as error:
                    _fail(f"candidate source is not readable UTF-8: {relative}: {error}")
                if mode == "line":
                    for line_number, line in enumerate(content.splitlines(), start=1):
                        if expression.search(line):
                            matches.append({"path": relative, "line": line_number, "text": line})
                elif expression.search(content):
                    matches.append({"path": relative})
        results.append({"id": scan_id, "matches": matches})
    return results


def test_ros_free_import_and_sole_module_inventory() -> None:
    extension = _extension_path()
    before_sha = _sha256_file(extension)
    before_modules = set(sys.modules)
    if any(_module_is_ros(name) for name in before_modules):
        _fail("qualification interpreter was already contaminated by ROS/backend modules")
    module = _load_extension()
    after_modules = set(sys.modules)
    imported = after_modules - before_modules
    ros_modules = sorted(name for name in imported if _module_is_ros(name))
    forbidden_modules = sorted(name for name in after_modules if name in _FORBIDDEN_MODULES)
    if ros_modules or forbidden_modules:
        _fail(f"extension imported forbidden modules: ROS={ros_modules}, other={forbidden_modules}")

    staged_root = _plain_directory(_STAGED_ROOT_ENV)
    suffix = sysconfig.get_config_var("EXT_SUFFIX")
    staged_files = [
        _plain_file(path, maximum=_MAX_EXTENSION_BYTES)
        for path in staged_root.rglob("*")
        if path.is_file()
    ]
    staged_extensions = [path for path in staged_files if path.name.endswith(suffix)]
    if staged_files != [extension]:
        _fail(f"extension-only staging inventory is not exact: {staged_files}")
    if staged_extensions != [extension]:
        _fail(f"staged production extension inventory is not exact: {staged_extensions}")
    symbols = sorted(set(re.findall(r"\bPyInit_[A-Za-z0-9_]+\b", _readelf(["--dyn-syms", "--wide"], extension))))
    if symbols != ["PyInit_manyforge_core_py"]:
        _fail(f"candidate ELF PyInit inventory drifted: {symbols}")
    dynamic = _readelf(["--dynamic", "--wide"], extension)
    needed = re.findall(r"Shared library: \[([^]]+)\]", dynamic)
    backend_needed = sorted(name for name in needed if _FORBIDDEN_ELF_DEPENDENCY.search(name))
    if backend_needed:
        _fail(f"candidate ELF links ROS/backend libraries: {backend_needed}")
    source_root = _plain_directory(_SOURCE_ROOT_ENV)
    registrations: list[dict[str, Any]] = []
    binding_root = source_root / "manyforge_core/python"
    if not binding_root.is_dir() or binding_root.is_symlink():
        _fail("candidate source omits the production binding root")
    registration_pattern = re.compile(r"PYBIND11_MODULE\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)")
    for source in binding_root.rglob("*"):
        if not source.is_file() or source.suffix not in {".cpp", ".hpp"}:
            continue
        relative = source.relative_to(source_root).as_posix()
        if relative.startswith("manyforge_core/python/tests/"):
            continue
        content = _plain_file(source, maximum=_MAX_SOURCE_BYTES, allow_empty=True).read_text(encoding="utf-8")
        for match in registration_pattern.finditer(content):
            registrations.append({"path": relative, "module": match.group(1)})
    if registrations != [{"path": "manyforge_core/python/bindings.cpp", "module": _MODULE_NAME}]:
        _fail(f"production PYBIND11_MODULE registration inventory drifted: {registrations}")
    if _sha256_file(extension) != before_sha:
        _fail("candidate extension bytes changed during import inspection")
    imported_module_count = len(staged_extensions)
    suffix_matches = extension.name.endswith(suffix)
    _write_case_evidence(
        "ros_free_import_and_sole_module_inventory",
        {
            "imported_module_count": imported_module_count,
            "forbidden_module_count": len(forbidden_modules),
            "ros_module_count": len(ros_modules),
            "extension_suffix_matches": suffix_matches,
        },
        {
            "module_file": module.__file__,
            "module_origin": module.__spec__.origin,
            "new_modules": sorted(imported),
            "forbidden_modules": forbidden_modules,
            "ros_modules": ros_modules,
            "staged_extensions": [str(path) for path in staged_extensions],
            "pyinit_symbols": symbols,
            "elf_needed": needed,
            "source_module_registrations": registrations,
        },
    )


def test_cpp_python_field_enum_default_ordering_and_error_parity() -> None:
    module = _load_extension()
    values = _canonical_empty_pipeline(module)
    actual = _compiler_observation(values)
    expected = _expected_compiler_observation()
    if actual != expected:
        _fail("canonical-empty Python projection differs from the frozen C++ golden")
    fields = {
        name: list(_properties(getattr(module, name))) for name in _TRUSTED_PROJECTIONS
    }
    expected_fields = {name: list(fields_) for name, fields_ in _EXPECTED_PROPERTIES.items()}
    if fields != expected_fields:
        _fail("trusted projection fields differ from the frozen C++ declarations")
    enums = _enum_observation(module)
    if enums != _EXPECTED_ENUMS:
        _fail("Python enum names/values differ from the frozen C++ declarations")
    dto_fixture = _constructible_dto_fixture(module)
    dto_fields = {
        name: list(_properties(getattr(module, name)))
        for name in sorted(_EXPECTED_CONSTRUCTIBLE_DTO_PROPERTIES)
    }
    expected_dto_fields = {
        name: list(fields_)
        for name, fields_ in sorted(_EXPECTED_CONSTRUCTIBLE_DTO_PROPERTIES.items())
    }
    if dto_fields != expected_dto_fields:
        _fail("constructible DTO fields differ from the frozen C++ declarations")
    constructor_rejections: dict[str, list[str]] = {}
    constructor_parameters: dict[str, list[str]] = {}
    constructor_defaults: dict[str, list[str]] = {}
    expected_required_constructor_arguments = {
        name: list(parameters)
        for name, parameters in sorted(_EXPECTED_CONSTRUCTOR_PARAMETERS.items())
    }
    for name, kwargs in sorted(dto_fixture["kwargs"].items()):
        value_type = getattr(module, name)
        parameters, defaults = _documented_constructor_contract(value_type)
        constructor_parameters[name] = list(parameters)
        constructor_defaults[name] = list(defaults)
        if parameters != _EXPECTED_CONSTRUCTOR_PARAMETERS[name] or defaults:
            _fail(
                f"constructor order/defaults differ from the C++ binding: "
                f"{name}: parameters={parameters}, defaults={defaults}"
            )
        if set(kwargs) != set(parameters):
            _fail(f"constructor fixture arguments differ from {name}'s signature")
        instance = value_type(**kwargs)
        if type(instance).__name__ != name:
            _fail(f"keyword construction returned the wrong DTO type: {name}")
        rejected: list[str] = []
        for omitted in parameters:
            incomplete = dict(kwargs)
            del incomplete[omitted]
            try:
                value_type(**incomplete)
            except TypeError:
                rejected.append(omitted)
            else:
                _fail(f"deployment DTO constructor made {name}.{omitted} optional")
        constructor_rejections[name] = rejected
    if (
        constructor_parameters != expected_required_constructor_arguments
        or constructor_rejections != expected_required_constructor_arguments
        or any(constructor_defaults.values())
    ):
        _fail("deployment DTO required-constructor argument inventory drifted")
    union_aliases: dict[str, list[str]] = {}
    expected_union_aliases = {
        name: list(alternatives)
        for name, alternatives in sorted(_EXPECTED_UNION_ALIASES.items())
    }
    for name in sorted(_EXPECTED_UNION_ALIASES):
        alias = getattr(module, name, None)
        alternatives = tuple(
            getattr(alternative, "__name__", "") for alternative in get_args(alias)
        )
        if alternatives != _EXPECTED_UNION_ALIASES[name]:
            _fail(f"closed Python union alternatives drifted: {name}: {alternatives}")
        union_aliases[name] = list(alternatives)
    expected_ordering = {
        "binding_routes": ["route_b", "route_a"],
        "endpoint_bindings": ["external_endpoint", "runtime_endpoint"],
        "external_resources": ["external_b", "external_a"],
    }
    if dto_fixture["ordering"] != expected_ordering:
        _fail("deployment DTO collection ordering differs from C++ input order")
    source = _verified_source_fixture(module)
    links = {link.name: link for link in source.source_ir.urdf.links}
    base_visual = links["base_link"].visuals[0]
    semantic_defaults = {
        "visual_origin": {
            "element_authored": base_visual.origin.element_authored,
            "authored_xyz_m": base_visual.origin.authored_xyz_m,
            "authored_rpy_rad": base_visual.origin.authored_rpy_rad,
            "xyz_m": list(base_visual.origin.xyz_m),
            "rpy_rad": list(base_visual.origin.rpy_rad),
        },
        "mesh_scale": {
            "authored_scale": base_visual.geometry.authored_scale,
            "scale": list(base_visual.geometry.scale),
        },
    }
    expected_semantic_defaults = {
        "visual_origin": {
            "element_authored": False,
            "authored_xyz_m": None,
            "authored_rpy_rad": None,
            "xyz_m": [0.0, 0.0, 0.0],
            "rpy_rad": [0.0, 0.0, 0.0],
        },
        "mesh_scale": {
            "authored_scale": None,
            "scale": [1.0, 1.0, 1.0],
        },
    }
    if semantic_defaults != expected_semantic_defaults:
        _fail("reachable model-source semantic defaults drifted")
    actual_artifact_order = [
        artifact.logical_uri for artifact in source.verified_artifacts
    ]
    expected_artifact_order = [
        "asset://i2_binding_asset/meshes/base.stl",
        "asset://i2_binding_asset/srdf/model.srdf",
        "asset://i2_binding_asset/urdf/model.urdf",
    ]
    if actual_artifact_order != expected_artifact_order:
        _fail("Python source-artifact ordering differs from the frozen C++ ordering")
    parsed_invalid = _callable(module, "parse_strict_json_model_yaml")(
        '{"api_version":"manyforge.cell_graph.v1"}'
    )
    error_type = getattr(module, "CellGraphCodecError", None)
    if not isinstance(error_type, type) or not issubclass(error_type, ValueError):
        _fail("structured CellGraphCodecError is missing or has the wrong base")
    try:
        _callable(module, "decode_cell_graph_candidate")(parsed_invalid)
    except error_type as error:
        error_observation = {"type": type(error).__name__, "message": str(error), "fields": dict(vars(error))}
    else:
        _fail("invalid graph did not produce the exact structured Python error")
    expected_error = {"type": "CellGraphCodecError", "message": "$: missing required field 'graph_revision'", "fields": {}}
    if error_observation != expected_error:
        _fail(f"structured-error parity drifted: {error_observation}")
    expected_vector = {
        "projection": expected,
        "trusted_fields": expected_fields,
        "deployment_dto_fields": expected_dto_fields,
        "constructor_parameters": expected_required_constructor_arguments,
        "constructor_defaults": {
            name: [] for name in expected_required_constructor_arguments
        },
        "required_constructor_arguments": expected_required_constructor_arguments,
        "enums": _EXPECTED_ENUMS,
        "union_aliases": expected_union_aliases,
        "deployment_collection_order": expected_ordering,
        "semantic_defaults": expected_semantic_defaults,
        "artifact_order": expected_artifact_order,
        "error": expected_error,
    }
    actual_vector = {
        "projection": actual,
        "trusted_fields": fields,
        "deployment_dto_fields": dto_fields,
        "constructor_parameters": constructor_parameters,
        "constructor_defaults": constructor_defaults,
        "required_constructor_arguments": constructor_rejections,
        "enums": enums,
        "union_aliases": union_aliases,
        "deployment_collection_order": dto_fixture["ordering"],
        "semantic_defaults": semantic_defaults,
        "artifact_order": actual_artifact_order,
        "error": error_observation,
    }
    expected_sha = _sha256_value(expected_vector)
    actual_sha = _sha256_value(actual_vector)
    _write_case_evidence(
        "cpp_python_field_enum_default_ordering_and_error_parity",
        {"cpp_vector_sha256": expected_sha, "python_vector_sha256": actual_sha, "vectors_equal": expected_sha == actual_sha},
        {"expected": expected_vector, "actual": actual_vector},
    )


def test_final_compiler_projection_nonconstructible_and_read_only() -> None:
    instance = _canonical_empty_pipeline(_load_extension())["compiler"]
    write_attempts, observations = _mutation_observations(instance)
    _write_case_evidence(
        "final_compiler_projection_nonconstructible_and_read_only",
        {"construct_attempts": 2, "construct_rejections": 2, "write_attempts": write_attempts, "write_rejections": write_attempts},
        {"types": [{"type_name": type(instance).__name__, "attempts": observations}]},
    )


def test_all_trusted_predecessor_projections_nonconstructible_and_read_only() -> None:
    module = _load_extension()
    empty = _canonical_empty_pipeline(module)
    source = _verified_source_fixture(module)
    overlay = _verified_site_overlay_fixture(module)
    instances = {
        "RevisionVerifiedDeploymentArtifact": empty["deployment"],
        "RevisionVerifiedDeploymentApplicabilityInputs": empty[
            "deployment_inputs"
        ],
        "GraphRevisionVerifiedCellGraph": empty["graph"],
        "VerifiedSiteOverlayArtifactProjection": overlay["projection"],
        "QualifiedSelectedSiteOverlay": overlay["selection"],
        "ModelSourceRetrievalPlan": source.retrieval_plan,
        "ModelSourceIr": source.source_ir,
        "SourceVerifiedModelAsset": source,
        "CompositionQualifiedCellIr": empty["composition"],
        "ApplicabilityQualifiedCellIr": empty["applicability"],
    }
    observations = []
    write_attempts = 0
    for name in _PREDECESSORS:
        attempts, rows = _mutation_observations(instances[name])
        write_attempts += attempts
        observations.append({"type_name": name, "attempts": rows})
    _write_case_evidence(
        "all_trusted_predecessor_projections_nonconstructible_and_read_only",
        {"projection_count": len(instances), "construct_attempts": 2 * len(instances), "construct_rejections": 2 * len(instances), "write_attempts": write_attempts, "write_rejections": write_attempts},
        {"types": observations},
    )


def test_detached_predecessor_and_final_value_lifetime() -> None:
    module = _load_extension()

    def detached(name: str) -> Any:
        if name in {
            "ModelSourceRetrievalPlan",
            "ModelSourceIr",
            "SourceVerifiedModelAsset",
        }:
            source = _verified_source_fixture(module)
            return {
                "ModelSourceRetrievalPlan": source.retrieval_plan,
                "ModelSourceIr": source.source_ir,
                "SourceVerifiedModelAsset": source,
            }[name]
        if name in {
            "VerifiedSiteOverlayArtifactProjection",
            "QualifiedSelectedSiteOverlay",
        }:
            overlay = _verified_site_overlay_fixture(module)
            return {
                "VerifiedSiteOverlayArtifactProjection": overlay["projection"],
                "QualifiedSelectedSiteOverlay": overlay["selection"],
            }[name]
        empty = _canonical_empty_pipeline(module)
        return {
            "RevisionVerifiedDeploymentArtifact": empty["deployment"],
            "RevisionVerifiedDeploymentApplicabilityInputs": empty[
                "compiler"
            ].applicability.deployment_inputs,
            "GraphRevisionVerifiedCellGraph": empty["compiler"].applicability.composition.verified_graph,
            "CompositionQualifiedCellIr": empty["compiler"].applicability.composition,
            "ApplicabilityQualifiedCellIr": empty["compiler"].applicability,
            "CompilerQualifiedCellIr": empty["compiler"],
        }[name]

    values = {name: detached(name) for name in _TRUSTED_PROJECTIONS}
    gc.collect()
    observations: list[dict[str, Any]] = []
    total_reads = 0
    for name, value in values.items():
        properties = _properties(type(value))
        first: dict[str, Any] = {}
        last: dict[str, Any] = {}
        for iteration in range(100):
            row = {attribute: _small_observation(getattr(value, attribute)) for attribute in properties}
            total_reads += len(properties)
            if iteration == 0:
                first = row
            if iteration == 99:
                last = row
        if first != last:
            _fail(f"detached projection changed after owner release: {name}")
        observations.append({"type_name": name, "iterations": 100, "first": first, "last": last})
    _write_case_evidence(
        "detached_predecessor_and_final_value_lifetime",
        {"detached_projection_count": len(values), "post_owner_read_iterations": total_reads, "failed_reads": 0},
        {"projections": observations},
    )


def test_blocking_compile_releases_gil_with_progress_witness() -> None:
    module = _load_extension()
    applicability = _canonical_empty_pipeline(module)["applicability"]
    compile_cell_ir = _callable(module, "compile_cell_ir")
    deltas, results, worker_thread, compiler_thread = _progress_probe(
        lambda: _small_observation(compile_cell_ir(applicability)), calls=64
    )
    progress = sum(deltas)
    if progress <= 0 or worker_thread == compiler_thread:
        _fail("actual blocking compile calls did not permit a distinct Python worker to progress")
    _write_case_evidence(
        "blocking_compile_releases_gil_with_progress_witness",
        {"compiler_calls": len(results), "worker_progress_events": progress, "distinct_worker_thread": worker_thread != compiler_thread},
        {"worker_thread_id": worker_thread, "compiler_thread_id": compiler_thread, "per_call_progress": deltas, "results": results},
    )


def test_concurrent_pure_compiles_are_deterministic() -> None:
    module = _load_extension()
    baseline = _canonical_empty_pipeline(module)
    applicability = baseline["applicability"]
    compile_cell_ir = _callable(module, "compile_cell_ir")
    workers = 8
    calls_per_worker = 64
    barrier = threading.Barrier(workers)

    def run_worker() -> dict[str, Any]:
        barrier.wait(timeout=10.0)
        start = time.monotonic_ns()
        result = None
        for _ in range(calls_per_worker):
            result = compile_cell_ir(applicability)
        end = time.monotonic_ns()
        if result is None:
            _fail("compile_cell_ir returned None in a concurrent worker")
        observation = _compiler_observation(
            {
                **baseline,
                "applicability": result.applicability,
                "composition": result.applicability.composition,
                "graph": result.applicability.composition.verified_graph,
                "compiler": result,
            }
        )
        return {"thread_id": threading.get_ident(), "start_ns": start, "end_ns": end, "result": observation}

    old_interval = sys.getswitchinterval()
    sys.setswitchinterval(10.0)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
            rows = list(executor.map(lambda unused: run_worker(), range(workers)))
    finally:
        sys.setswitchinterval(old_interval)
    if len({row["thread_id"] for row in rows}) < 2:
        _fail("concurrent compile qualification used fewer than two threads")
    overlap = any(
        left["thread_id"] != right["thread_id"]
        and max(left["start_ns"], right["start_ns"]) < min(left["end_ns"], right["end_ns"])
        for index, left in enumerate(rows)
        for right in rows[index + 1 :]
    )
    if not overlap:
        _fail("actual compile_cell_ir calls did not overlap across Python threads")
    digests = {_sha256_value(row["result"]) for row in rows}
    if len(digests) != 1:
        _fail("actual concurrent pure compiles produced different results")
    digest = next(iter(digests))
    _write_case_evidence(
        "concurrent_pure_compiles_are_deterministic",
        {"concurrent_calls": workers * calls_per_worker, "unique_result_digests": len(digests), "result_digest_sha256": digest},
        {"workers": rows, "calls_per_worker": calls_per_worker},
    )


def test_exception_translation_after_released_gil() -> None:
    module = _load_extension()
    parsed = _callable(module, "parse_strict_json_model_yaml")(
        '{"api_version":"manyforge.cell_graph.v1"}'
    )
    decode = _callable(module, "decode_cell_graph_candidate")
    error_type = getattr(module, "CellGraphCodecError", None)
    if not isinstance(error_type, type) or not issubclass(error_type, ValueError):
        _fail("production CellGraphCodecError is not an exact ValueError subtype")
    deltas, payloads, worker_thread, compiler_thread = _progress_probe(
        lambda: decode(parsed), calls=128, expected_exception=error_type
    )
    expected = {"type": "CellGraphCodecError", "message": "$: missing required field 'graph_revision'", "fields": {}}
    if any(payload != expected for payload in payloads):
        _fail("released-GIL production failures lost exact structured payload parity")
    progress = sum(deltas)
    if progress <= 0 or worker_thread == compiler_thread:
        _fail("production failure calls did not release the GIL to a distinct worker")
    _write_case_evidence(
        "exception_translation_after_released_gil",
        {"translated_exception_count": len(payloads), "released_gil_failure_count": sum(delta > 0 for delta in deltas), "exact_payload_match": True},
        {"worker_thread_id": worker_thread, "compiler_thread_id": compiler_thread, "per_call_progress": deltas, "payloads": payloads},
    )


_FRESH_CHILD = r'''
import gc, importlib.util, json, pathlib, sys
path = pathlib.Path(sys.argv[1]).resolve()
spec = importlib.util.spec_from_file_location("manyforge_core_py", path)
if spec is None or spec.loader is None:
    raise RuntimeError("cannot form exact candidate import spec")
module = importlib.util.module_from_spec(spec)
sys.modules["manyforge_core_py"] = module
spec.loader.exec_module(module)
if pathlib.Path(module.__file__).resolve() != path:
    raise RuntimeError("candidate __file__ drifted")
if pathlib.Path(module.__spec__.origin).resolve() != path:
    raise RuntimeError("candidate __spec__.origin drifted")
parsed = module.parse_strict_json_model_yaml(sys.argv[2])
deployment = module.compute_deployment_artifact_revision(parsed, ())
composition = module.compose_cell_occurrences(deployment.graph, (), ())
inputs = deployment.applicability_inputs
overlay = module.select_site_overlay_for_deployment(inputs.value.site_overlay_ref, inputs.value.external_resources, None)
applicability = module.qualify_cell_applicability(composition, inputs, (), (), (), (), (), (), (), overlay)
compiler = module.compile_cell_ir(applicability)
observation = {"module_file": module.__file__, "module_origin": module.__spec__.origin, "compiler_type": type(compiler).__name__, "cell_id": compiler.applicability.composition.graph.cell_id.value, "catalog_record_count": len(compiler.catalog_record_refs)}
del compiler, applicability, composition, deployment
gc.collect()
print(json.dumps(observation, sort_keys=True, separators=(",", ":")))
'''


def test_repeated_fresh_interpreter_import_use_and_clean_shutdown() -> None:
    extension = _extension_path()
    try:
        expected_executable = _absolute_path(_EXECUTABLE_ENV).resolve(strict=True)
        active_executable = Path(sys.executable).resolve(strict=True)
    except OSError as error:
        _fail(f"active qualification executable is unavailable: {error}")
    if active_executable != expected_executable:
        _fail(
            "fresh-process qualification is not running through the packet-selected "
            f"Python executable: {active_executable} != {expected_executable}"
        )
    before_sha = _sha256_file(extension)
    environment = dict(os.environ)
    environment.pop("PYTHONPATH", None)
    environment.pop("PYTHONHOME", None)
    runs: list[dict[str, Any]] = []
    for index in range(25):
        argv = [sys.executable, "-I", "-S", "-c", _FRESH_CHILD, str(extension), _CANONICAL_EMPTY_INPUT]
        start = time.monotonic_ns()
        try:
            result = subprocess.run(
                argv,
                check=False,
                capture_output=True,
                text=True,
                timeout=30.0,
                env=environment,
            )
            timed_out = False
        except subprocess.TimeoutExpired as error:
            _fail(f"fresh interpreter {index} timed out: {error}")
        end = time.monotonic_ns()
        combined = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 0 or _SANITIZER_FAILURE.search(combined):
            _fail(f"fresh interpreter {index} failed clean shutdown: {combined}")
        try:
            observation = json.loads(result.stdout)
        except json.JSONDecodeError as error:
            _fail(f"fresh interpreter {index} did not emit exact JSON: {error}")
        expected = {"module_file": str(extension), "module_origin": str(extension), "compiler_type": "CompilerQualifiedCellIr", "cell_id": "c1_build_authority_cell", "catalog_record_count": 0}
        if observation != expected:
            _fail(f"fresh interpreter {index} use observation drifted: {observation}")
        runs.append({"index": index, "argv": argv[:5] + [str(extension), "<embedded-canonical-empty>"], "start_ns": start, "end_ns": end, "exit_status": result.returncode, "timed_out": timed_out, "stdout": observation, "stderr": result.stderr})
    if _sha256_file(extension) != before_sha:
        _fail("candidate extension bytes changed across fresh interpreters")
    _write_case_evidence(
        "repeated_fresh_interpreter_import_use_and_clean_shutdown",
        {"fresh_interpreter_runs": len(runs), "clean_exit_count": sum(row["exit_status"] == 0 for row in runs), "nonzero_exit_count": sum(row["exit_status"] != 0 for row in runs)},
        {"runs": runs},
    )


def test_generated_stub_inventory_and_documentation_parity() -> None:
    module = _load_extension()
    extension = _extension_path()
    generated_root = _plain_directory(_GENERATED_ROOT_ENV)
    stub_path = _generated_file("manyforge_core_py.pyi")
    api_path = _generated_file("manyforge_core_py.api.json")
    docs_path = _generated_file("manyforge_core_py-api.md")
    inventory = _strict_json(api_path)
    if set(inventory) != {"schema_version", "module", "framework", "python_abi", "exports"}:
        _fail("generated API inventory root keys drifted")
    if inventory["schema_version"] != "manyforge.composable_cell_i2.binding_api_inventory.v1" or inventory["module"] != _MODULE_NAME or inventory["framework"] != "pybind11":
        _fail("generated API inventory identity drifted")
    abi = inventory["python_abi"]
    expected_abi = {"implementation": platform.python_implementation(), "version": platform.python_version(), "soabi": sysconfig.get_config_var("SOABI"), "extension_suffix": sysconfig.get_config_var("EXT_SUFFIX"), "gil_disabled": False}
    if abi != expected_abi:
        _fail(f"generated API inventory ABI differs from the active interpreter: {abi}")
    exports = inventory["exports"]
    if not isinstance(exports, list) or not exports:
        _fail("generated API inventory has no exports")
    by_name: dict[str, dict[str, Any]] = {}
    for row in exports:
        if not isinstance(row, dict) or set(row) != {"name", "kind", "constructible", "writable_attributes", "trusted_projection"}:
            _fail("generated API export row keys drifted")
        if row["name"] in by_name:
            _fail(f"generated API export is duplicated: {row['name']}")
        by_name[row["name"]] = row
    if list(by_name) != sorted(by_name):
        _fail("generated API inventory is not in canonical name order")
    runtime_names = {name for name in dir(module) if not name.startswith("_")}
    if set(by_name) != runtime_names:
        _fail("generated API inventory differs from runtime public exports")

    try:
        stub_tree = ast.parse(stub_path.read_text(encoding="utf-8"), filename=str(stub_path))
    except (OSError, UnicodeError, SyntaxError) as error:
        _fail(f"generated stub is not valid UTF-8 Python: {error}")
    stub = _stub_semantics(stub_tree)
    if set(stub) != runtime_names:
        _fail("generated stub semantic export inventory differs from runtime")
    docs = docs_path.read_text(encoding="utf-8")
    headings = set(re.findall(r"(?m)^### `([A-Za-z_][A-Za-z0-9_]*)`\s*$", docs))
    if headings != runtime_names:
        _fail("generated documentation headings differ from runtime exports")

    mismatches: list[str] = []
    for name, row in by_name.items():
        runtime = getattr(module, name)
        semantic = stub[name]
        section_match = re.search(rf"(?ms)^### `{re.escape(name)}`\s*$\n(.*?)(?=^### `|\Z)", docs)
        section = section_match.group(1) if section_match else ""
        if not section.strip():
            mismatches.append(f"{name}:empty_documentation")
        if row["kind"] in {"class", "enum"} and not isinstance(runtime, type):
            mismatches.append(f"{name}:runtime_kind")
        if row["kind"] == "function" and not callable(runtime):
            mismatches.append(f"{name}:runtime_kind")
        if row["kind"] == "class":
            runtime_constructible = _publishes_constructor_signature(runtime)
            stub_constructible = "__init__" in semantic.get("methods", {})
            if (
                row["constructible"] is not runtime_constructible
                or stub_constructible is not runtime_constructible
            ):
                mismatches.append(f"{name}:constructibility")
        if get_origin(runtime) is not None and (
            row["kind"] != "constant" or semantic["kind"] != "constant"
        ):
            mismatches.append(f"{name}:typing_alias_kind")
        if name in _TRUSTED_PROJECTIONS:
            fields = set(_properties(runtime))
            stub_members = set(semantic.get("members", ()))
            if fields != set(_EXPECTED_PROPERTIES[name]) or not fields.issubset(stub_members):
                mismatches.append(f"{name}:fields")
            if row["constructible"] is not False or row["writable_attributes"] or row["trusted_projection"] is not True:
                mismatches.append(f"{name}:trust_flags")
            if any(field not in section for field in fields):
                mismatches.append(f"{name}:field_documentation")
        members = getattr(runtime, "__members__", None)
        if isinstance(runtime, type) and isinstance(members, dict):
            stub_members = set(semantic.get("members", ()))
            if set(members) != stub_members or any(member not in section for member in members):
                mismatches.append(f"{name}:enum_members")
        if semantic["kind"] == "function":
            runtime_doc = getattr(runtime, "__doc__", "") or ""
            for signature in semantic["signatures"]:
                for parameter in signature["parameters"]:
                    parameter_name = parameter["name"]
                    if parameter_name not in runtime_doc or parameter_name not in section:
                        mismatches.append(f"{name}:parameter:{parameter_name}")
                    if parameter["default"] is not None and "=" not in runtime_doc:
                        mismatches.append(f"{name}:default:{parameter_name}")
                if signature["returns"] and "->" not in runtime_doc:
                    mismatches.append(f"{name}:return")
    if mismatches:
        _fail(f"generated stub/API/documentation semantic parity failed: {mismatches[:20]}")
    _write_case_evidence(
        "generated_stub_inventory_and_documentation_parity",
        {"generated_output_count": 4, "api_export_count": len(by_name), "parity_mismatches": len(mismatches)},
        {"extension": {"path": str(extension), "sha256": _sha256_file(extension)}, "stub_semantics": stub, "api_inventory": inventory, "documentation_headings": sorted(headings)},
    )


def test_complete_forbidden_surface_absence() -> None:
    module = _load_extension()
    forbidden_exports = sorted(name for name in _FORBIDDEN_EXPORTS if hasattr(module, name))
    forbidden_modules: list[str] = []
    for name in sorted(_FORBIDDEN_MODULES):
        if name in sys.modules:
            forbidden_modules.append(name)
            continue
        try:
            specification = importlib.util.find_spec(name)
        except (ImportError, ModuleNotFoundError, ValueError):
            specification = None
        if specification is not None:
            forbidden_modules.append(name)
    scans = _scan_source()
    scan_matches = sum(len(row["matches"]) for row in scans)
    if forbidden_exports or forbidden_modules or scan_matches:
        _fail(f"forbidden I2 surfaces remain: exports={forbidden_exports}, modules={forbidden_modules}, scans={[row for row in scans if row['matches']]}")
    _write_case_evidence(
        "complete_forbidden_surface_absence",
        {"forbidden_export_count": len(forbidden_exports), "forbidden_module_count": len(forbidden_modules), "forbidden_scan_match_count": scan_matches},
        {"exports": forbidden_exports, "modules": forbidden_modules, "scans": scans},
    )
