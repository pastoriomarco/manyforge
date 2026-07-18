#!/usr/bin/env python3
"""Regenerate operator-facing public contract docs in `docs/reference/`.

Produces public documentation and machine-readable contracts derived from
code-as-source-of-truth:

  - docs/reference/REST_API.md
        Composer REST surface, generated from the FastAPI app's
        OpenAPI schema (manyforge_composer.backend.app:app).

  - docs/reference/MCP_RUNTIME_CONTRACT.md
        ManyForge MCP tools (names, descriptions, input schemas),
        generated from the MCP server module
        (manyforge_composer.backend.mcp_server).

  - docs/reference/ASSISTANT_PROVIDER_CONTRACT.md
        Assistant-provider HTTP wire format, generated from the bridge
        FastAPI app's OpenAPI schema (manyforge_assistant_bridge.bridge:app)
        plus the envelope-validation contract documented in protocol.py.

  - docs/reference/VIEWER_DATA_PLANE_V1.schema.json
        JSON Schema for every viewer v1 server and client message, generated
        from the strict Pydantic wire models.

  - manyforge_composer/frontend/src/types/viewerProtocol.generated.ts
        Frontend wire types generated from the same Pydantic models. This is
        the only TypeScript declaration surface for viewer protocol messages.

  - manyforge_composer/frontend/src/types/viewerProtocol.fixture.generated.ts
        Frontend-local copy of the generated preservation fixture. Keeping it
        inside the frontend source boundary makes isolated worktree builds
        deterministic without duplicating hand-authored fixture data.

  - docs/reference/VIEWER_DATA_PLANE_V1.fixture.json
        One generated, representative v1 manifest/frame generation and its
        legacy materialized view. Backend and frontend parity tests consume the
        same fixture so neither side can silently reinterpret the wire order.

  - docs/reference/VIEWER_QUALIFICATION_ARTIFACT_V1.schema.json
        JSON Schema for reproducible viewer qualification captures.

Why this script exists:
  These docs replace links to the private `manyforge_specs` repo so
  deployment users have everything they need on disk. Hand-copying the
  spec content would introduce a second source of truth and drift.
  Generation from code keeps the deployment-side docs synchronised with
  the actual runtime code without manual reconciliation.

Usage:
    cd $REPO_ROOT
    python3 scripts/regen-public-contracts.py

The script imports manyforge modules; run it from a Python environment
that can import them (the standard Composer dev container, or the host
venv used for the bridge).

When extending: add new generators that read code-level facts (decorator
metadata, type hints, FastAPI routes, schemas) and produce concise
operator-facing markdown. NEVER copy normative content from
manyforge_specs into the generator's output.
"""
from __future__ import annotations

import argparse
import enum
import hashlib
import json
import re
import sys
import time
import textwrap
import types
from pathlib import Path
from typing import Any, Callable, Literal, Union, get_args, get_origin, get_type_hints


REPO_ROOT = Path(__file__).resolve().parents[1]
REF_DIR = REPO_ROOT / "docs" / "reference"
VIEWER_SCHEMA_PATH = REF_DIR / "VIEWER_DATA_PLANE_V1.schema.json"
VIEWER_QUALIFICATION_SCHEMA_PATH = (
    REF_DIR / "VIEWER_QUALIFICATION_ARTIFACT_V1.schema.json"
)
VIEWER_QUALIFICATION_MATRIX_SCHEMA_PATH = (
    REF_DIR / "VIEWER_QUALIFICATION_MATRIX_V1.schema.json"
)
VIEWER_FIXTURE_PATH = REF_DIR / "VIEWER_DATA_PLANE_V1.fixture.json"
VIEWER_TYPES_PATH = (
    REPO_ROOT
    / "manyforge_composer"
    / "frontend"
    / "src"
    / "types"
    / "viewerProtocol.generated.ts"
)
VIEWER_TYPES_FIXTURE_PATH = (
    REPO_ROOT
    / "manyforge_composer"
    / "frontend"
    / "src"
    / "types"
    / "viewerProtocol.fixture.generated.ts"
)
DEFAULT_CATALOG_PATH = str(
    REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml"
)

# The repository is a mixed colcon/Python workspace: Composer is importable
# from the repository root, while manyforge_behavior uses a nested package
# root. Make the documented direct invocation deterministic instead of relying
# on an ambient PYTHONPATH from a particular test or launch wrapper.
for source_root in (REPO_ROOT, REPO_ROOT / "manyforge_behavior"):
    source_text = str(source_root)
    if source_text not in sys.path:
        sys.path.insert(0, source_text)


def _build_composer_state():
    """Construct a minimal ComposerState suitable for OpenAPI / MCP introspection.

    The factories `create_app(state)` and `create_mcp_server(state)` both
    require a real `ComposerState` instance. We use the same `MockKernelClient`
    that the test suite uses — the kernel is never invoked here; we only
    introspect the FastAPI/MCP shape after construction.
    """
    from manyforge_behavior.kernel_client import MockKernelClient
    from manyforge_behavior.node_environment import build_node_environment
    from manyforge_composer.backend.state import ComposerState
    return ComposerState(
        node_environment=build_node_environment(DEFAULT_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
HEADER_TEMPLATE = (
    "<!--\n"
    "  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.\n"
    "  Regenerate with:\n"
    "    cd {repo_root}\n"
    "    python3 scripts/regen-public-contracts.py\n"
    "  Last regenerated: {timestamp}\n"
    "  Generator source: scripts/regen-public-contracts.py:{generator_name}\n"
    "-->\n\n"
)


def _stamp(generator_name: str) -> str:
    return HEADER_TEMPLATE.format(
        repo_root=str(REPO_ROOT),
        timestamp=time.strftime("%Y-%m-%d %H:%M:%S %Z"),
        generator_name=generator_name,
    )


# `--check` mode compares freshly-generated content to what's on disk. The
# embedded `Last regenerated: <now>` timestamp would otherwise always
# differ, defeating the check. Normalize the timestamp line on both sides
# before comparing.
_TIMESTAMP_LINE_RE = re.compile(r"  Last regenerated: .*\n")


def _strip_timestamp(content: str) -> str:
    return _TIMESTAMP_LINE_RE.sub("  Last regenerated: <normalized>\n", content)


def _normalized_generated_content(path: Path, content: str) -> str:
    return _strip_timestamp(content) if path.suffix == ".md" else content


# ---------------------------------------------------------------------------
# Viewer data-plane wire contract generator
# ---------------------------------------------------------------------------

def _viewer_wire_models() -> tuple[type[Any], ...]:
    from manyforge_composer.backend.models import (
        ViewerDiagnosticsV1,
        ViewerFrameV1,
        ViewerClientDiagnosticsControlV1,
        ViewerLeaseAckV1,
        ViewerLeaseControlV1,
        ViewerManifestV1,
        ViewerResyncControlV1,
    )

    return (
        ViewerManifestV1,
        ViewerFrameV1,
        ViewerDiagnosticsV1,
        ViewerLeaseAckV1,
        ViewerClientDiagnosticsControlV1,
        ViewerLeaseControlV1,
        ViewerResyncControlV1,
    )


def gen_viewer_json_schema() -> str:
    """Generate one schema bundle for all viewer v1 wire messages."""
    from pydantic import TypeAdapter

    models = _viewer_wire_models()
    adapter = TypeAdapter(Union[models])  # type: ignore[arg-type]
    schema = adapter.json_schema(
        by_alias=False,
        ref_template="#/$defs/{model}",
    )
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema"
    schema["$id"] = "https://manyforge.dev/schemas/viewer-data-plane-v1.schema.json"
    schema["title"] = "ManyForge Viewer Data Plane v1"
    schema["description"] = (
        "Generated viewer manifest, frame, diagnostics, lease acknowledgement, "
        "and client control messages. Source of truth: backend Pydantic models."
    )
    return json.dumps(schema, indent=2, sort_keys=True) + "\n"


def _collect_pydantic_models(roots: tuple[type[Any], ...]) -> list[type[Any]]:
    from pydantic import BaseModel

    collected: dict[str, type[Any]] = {}

    def collect_annotation(annotation: Any) -> None:
        origin = get_origin(annotation)
        if origin is not None:
            for argument in get_args(annotation):
                collect_annotation(argument)
            return
        if (
            isinstance(annotation, type)
            and issubclass(annotation, BaseModel)
            and annotation.__name__ not in collected
        ):
            collected[annotation.__name__] = annotation
            for nested in get_type_hints(annotation, include_extras=True).values():
                collect_annotation(nested)

    for root in roots:
        collect_annotation(root)
    return [collected[name] for name in sorted(collected)]


def _typescript_literal(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=True)
    if isinstance(value, (int, float)):
        return repr(value)
    raise TypeError(f"unsupported Literal value for TypeScript: {value!r}")


def _typescript_type(annotation: Any) -> str:
    from pydantic import BaseModel

    origin = get_origin(annotation)
    arguments = get_args(annotation)
    if annotation is Any:
        return "unknown"
    if annotation is None or annotation is type(None):
        return "null"
    if annotation is str:
        return "string"
    if annotation in (int, float):
        return "number"
    if annotation is bool:
        return "boolean"
    if origin is Literal:
        return " | ".join(_typescript_literal(value) for value in arguments)
    if origin in (Union, types.UnionType):
        return " | ".join(_typescript_type(value) for value in arguments)
    if origin in (list, set, frozenset):
        return f"Array<{_typescript_type(arguments[0])}>"
    if origin is tuple:
        if len(arguments) == 2 and arguments[1] is Ellipsis:
            return f"Array<{_typescript_type(arguments[0])}>"
        return "[" + ", ".join(_typescript_type(value) for value in arguments) + "]"
    if origin is dict:
        key_type = _typescript_type(arguments[0])
        value_type = _typescript_type(arguments[1])
        if key_type not in ("string", "number"):
            key_type = "string"
        return f"Record<{key_type}, {value_type}>"
    if origin is not None and str(origin).endswith("Annotated"):
        return _typescript_type(arguments[0])
    if isinstance(annotation, type) and issubclass(annotation, BaseModel):
        return annotation.__name__
    if isinstance(annotation, type) and issubclass(annotation, enum.Enum):
        return " | ".join(_typescript_literal(member.value) for member in annotation)
    raise TypeError(f"unsupported viewer wire annotation: {annotation!r}")


def _typescript_property_name(name: str) -> str:
    if re.fullmatch(r"[A-Za-z_$][A-Za-z0-9_$]*", name):
        return name
    return json.dumps(name, ensure_ascii=True)


def gen_viewer_typescript() -> str:
    """Generate exact TypeScript interfaces from viewer Pydantic models."""
    roots = _viewer_wire_models()
    models = _collect_pydantic_models(roots)
    lines = [
        "// THIS FILE IS GENERATED. DO NOT EDIT.\n",
        "// Source: manyforge_composer/backend/models.py viewer v1 wire models.\n",
        "// Regenerate: python3 scripts/regen-public-contracts.py\n\n",
    ]
    for model in models:
        hints = get_type_hints(model, include_extras=True)
        lines.append(f"export interface {model.__name__} {{\n")
        for field_name, field_info in model.model_fields.items():
            # Viewer protocol producers serialize Pydantic field names, not
            # REST/UI aliases inherited by a few nested source DTOs.
            wire_name = field_name
            optional = "" if field_info.is_required() else "?"
            annotation = hints.get(field_name, field_info.annotation)
            lines.append(
                f"  {_typescript_property_name(str(wire_name))}{optional}: "
                f"{_typescript_type(annotation)};\n"
            )
        lines.append("}\n\n")
    lines.extend(
        [
            "export type ViewerServerMessageV1 =\n",
            "  | ViewerManifestV1\n",
            "  | ViewerFrameV1\n",
            "  | ViewerDiagnosticsV1\n",
            "  | ViewerLeaseAckV1;\n\n",
            "export type ViewerControlMessageV1 =\n",
            "  | ViewerLeaseControlV1\n",
            "  | ViewerClientDiagnosticsControlV1\n",
            "  | ViewerResyncControlV1;\n",
        ]
    )
    return "".join(lines)


def gen_viewer_qualification_json_schema() -> str:
    """Generate the checked contract for qualification evidence artifacts."""
    from scripts.qualification.viewer_qualification_contract import (
        ViewerQualificationArtifactV1,
    )

    schema = ViewerQualificationArtifactV1.model_json_schema(
        ref_template="#/$defs/{model}"
    )
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema"
    schema["$id"] = (
        "https://manyforge.dev/schemas/viewer-qualification-artifact-v1.schema.json"
    )
    return json.dumps(schema, indent=2, sort_keys=True) + "\n"


def gen_viewer_qualification_matrix_json_schema() -> str:
    """Generate the checked scenario/budget matrix contract."""
    from scripts.qualification.viewer_qualification_contract import (
        ViewerQualificationMatrixV1,
    )

    schema = ViewerQualificationMatrixV1.model_json_schema(
        ref_template="#/$defs/{model}"
    )
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema"
    schema["$id"] = (
        "https://manyforge.dev/schemas/viewer-qualification-matrix-v1.schema.json"
    )
    return json.dumps(schema, indent=2, sort_keys=True) + "\n"


def gen_viewer_preservation_fixture() -> str:
    """Generate one cross-language static/dynamic preservation fixture.

    The fixture deliberately exercises every ordered block and both authored
    and attached entities. Fixed epochs, revisions, timestamps, and values make
    it a useful golden rather than a sample whose changes can be explained away
    as clock or UUID churn.
    """
    from manyforge_composer.backend.models import ViewerFrameV1, ViewerManifestV1

    pose_identity = {
        "position_m": [0.0, 0.0, 0.0],
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    manifest = ViewerManifestV1.model_validate(
        {
            "type": "manifest",
            "schema_version": "manyforge.viewer.manifest.v1",
            "producer_epoch": "fixture-epoch-0001",
            "manifest_revision": 7,
            "content_hash": "0" * 64,
            "world_frame": "universe",
            "robots": [
                {
                    "robot_id": "robot-a",
                    "group_id": "arm",
                    "urdf_model_key": "fixture-model-key",
                    "base_frame": "base_link",
                    "tcp_frame": "tool0",
                    "base_pose": {"frame_id": "universe", **pose_identity},
                    "joint_names": ["joint_1", "joint_2"],
                    "link_order": ["base_link", "tool0"],
                }
            ],
            "entities": [
                {
                    "object_id": "fixture-box",
                    "shape": {
                        "type": "box",
                        "box_dimensions_m": [0.2, 0.3, 0.4],
                        "sphere_radius_m": None,
                        "cylinder_radius_m": None,
                        "cylinder_height_m": None,
                        "mesh_resource_uri": None,
                        "mesh_scale": None,
                        "mesh_asset_url": None,
                        "mesh_asset_status": None,
                    },
                    "attached": False,
                    "attached_to": None,
                    "source": "authored",
                    "persistable": True,
                    "viewer_frame_visible": True,
                    "support_contacts": [],
                },
                {
                    "object_id": "fixture-tool",
                    "shape": {
                        "type": "mesh",
                        "box_dimensions_m": None,
                        "sphere_radius_m": None,
                        "cylinder_radius_m": None,
                        "cylinder_height_m": None,
                        "mesh_resource_uri": "package://fixture/tool.stl",
                        "mesh_scale": [1.0, 1.0, 1.0],
                        "mesh_asset_url": "/api/scene/meshes/fixture-tool",
                        "mesh_asset_status": "ready",
                    },
                    "attached": True,
                    "attached_to": "tool0",
                    "source": "attached",
                    "persistable": False,
                    "viewer_frame_visible": False,
                    "support_contacts": [{"link": "tool0"}],
                },
            ],
            "frames": [
                {"name": "universe", "roles": ["world"], "viewer_visible": True},
                {"name": "tool0", "roles": ["tcp"], "viewer_visible": True},
            ],
            "grippers": [
                {
                    "gripper_id": "gripper-a",
                    "joint_name": "finger_joint",
                    "derived_joints": ["left_finger", "right_finger"],
                    "actuation_kind": "position",
                    "state_authority": "backend",
                    "completion_policy": "time_and_observation",
                    "grasp_confirmation_policy": "position",
                    "transport_kind": "native",
                    "limit_lower": 0.0,
                    "limit_upper": 0.8,
                    "motion_time_s": 0.5,
                    "srdf_presets": {"open": 0.0, "closed": 0.8},
                    "visualization": None,
                }
            ],
            "pose_sources": [
                {
                    "pose_source_id": "camera-a",
                    "kind": "foundation_pose",
                    "enabled": True,
                    "max_update_hz": 30.0,
                    "stale_after_s": 0.5,
                    "frame_id": "universe",
                    "description": "fixture source",
                    "objects": [
                        {
                            "object_id": "fixture-box",
                            "entity": "fixture-box",
                            "frame_id": "universe",
                            "apply_policy": "continuous",
                            "scene_admission": {"mode": "snapshot"},
                            "measured_state_ghost": {"enabled": True, "object": True},
                        }
                    ],
                }
            ],
            "orders": {
                "links": ["base_link", "tool0"],
                "entities": ["fixture-box", "fixture-tool"],
                "frames": ["universe", "tool0"],
                "grippers": ["gripper-a"],
                "pose_sources": ["camera-a"],
            },
            "object_axes_gizmo_size_m": 0.12,
            "frame_axes_gizmo_size_m": 0.08,
            "capabilities": {
                "measured_robot": True,
                "measured_objects": True,
                "measured_tcp": True,
                "point_cloud_channel": True,
                "snapshot_capture": True,
                "multi_robot_shape": True,
            },
        }
    ).model_dump(mode="json")
    manifest_hash_payload = {
        key: value
        for key, value in manifest.items()
        if key
        not in {
            "type",
            "schema_version",
            "producer_epoch",
            "manifest_revision",
            "content_hash",
        }
    }
    manifest["content_hash"] = hashlib.sha256(
        json.dumps(
            manifest_hash_payload,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("utf-8")
    ).hexdigest()
    frame = ViewerFrameV1.model_validate(
        {
            "type": "frame",
            "schema_version": "manyforge.viewer.frame.v1",
            "producer_epoch": "fixture-epoch-0001",
            "manifest_revision": 7,
            "frame_sequence": 19,
            "render_revision": 13,
            "timestamp_ns": 12_345_678_900,
            "source_sequence": 31,
            "stale": False,
            "robot_blocks": [
                {
                    "robot_id": "robot-a",
                    "joint_positions": [0.25, -0.5],
                    "link_transforms": [
                        pose_identity,
                        {
                            "position_m": [0.4, -0.2, 0.7],
                            "orientation_xyzw": [0.0, 0.0, 0.7071067811865476, 0.7071067811865476],
                        },
                    ],
                    "tcp_pose_in_base": {
                        "position_m": [0.4, -0.2, 0.7],
                        "orientation_xyzw": [0.0, 0.0, 0.7071067811865476, 0.7071067811865476],
                    },
                    "state_source": "composer_backend",
                    "source_timestamp_ns": 12_345_670_000,
                    "source_age_ns": 8_900,
                }
            ],
            "entity_transforms": [
                {
                    "position_m": [0.7, 0.1, 0.2],
                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                },
                {
                    "position_m": [0.4, -0.2, 0.8],
                    "orientation_xyzw": [0.0, 0.0, 0.7071067811865476, 0.7071067811865476],
                },
            ],
            "frame_transforms": [
                pose_identity,
                {
                    "position_m": [0.4, -0.2, 0.7],
                    "orientation_xyzw": [0.0, 0.0, 0.7071067811865476, 0.7071067811865476],
                },
            ],
            "gripper_states": [
                {
                    "gripper_id": "gripper-a",
                    "joint_name": "finger_joint",
                    "derived_joints": ["left_finger", "right_finger"],
                    "state": "executing",
                    "command_id": 4,
                    "generation": 9,
                    "revision": 11,
                    "current_position": 0.4,
                    "target_position": 0.8,
                    "commanded_position": 0.8,
                    "observed_position": 0.39,
                    "commanded_state": "closed",
                    "actuation_kind": "position",
                    "state_authority": "backend",
                    "completion_policy": "time_and_observation",
                    "grasp_confirmation_policy": "position",
                    "transport_kind": "native",
                    "evidence": ["command_accepted"],
                    "evidence_state": "pending",
                    "projection_state": "available",
                    "projection_reason": "",
                    "limit_lower": 0.0,
                    "limit_upper": 0.8,
                    "motion_time_s": 0.5,
                    "srdf_presets": {"open": 0.0, "closed": 0.8},
                    "visualization": None,
                    "error": None,
                    "reason_code": None,
                }
            ],
            "pose_source_states": [
                {
                    "pose_source_id": "camera-a",
                    "status": "fresh",
                    "received_at_ns": 12_345_670_000,
                    "measured_tcp": None,
                    "measured_robot": None,
                    "objects": [
                        {
                            "object_id": "fixture-box",
                            "status": "fresh",
                            "age_s": 0.01,
                            "update": {
                                "pose_source_id": "camera-a",
                                "object_id": "fixture-box",
                                "entity": "fixture-box",
                                "monotonic_s": 12.345,
                                "apply_policy": "continuous",
                                "pose": {
                                    "frame_id": "universe",
                                    "stamp_sec": 12.3,
                                    "position_xyz": [0.7, 0.1, 0.2],
                                    "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                                },
                            },
                            "rejection": None,
                            "attached_drift": None,
                        }
                    ],
                }
            ],
        }
    ).model_dump(mode="json")
    robot_manifest = manifest["robots"][0]
    robot_frame = frame["robot_blocks"][0]
    legacy_robot = {
        **robot_manifest,
        "joint_positions": dict(
            zip(robot_manifest["joint_names"], robot_frame["joint_positions"])
        ),
        "tcp_pose_in_base": robot_frame["tcp_pose_in_base"],
        "state_source": robot_frame["state_source"],
        "source_timestamp_ns": robot_frame["source_timestamp_ns"],
        "source_age_ns": robot_frame["source_age_ns"],
    }
    dynamic_pose_sources = {
        item["pose_source_id"]: item for item in frame["pose_source_states"]
    }
    legacy_pose_sources = []
    for static_source in manifest["pose_sources"]:
        dynamic_source = dynamic_pose_sources[static_source["pose_source_id"]]
        dynamic_objects = {
            item["object_id"]: item for item in dynamic_source["objects"]
        }
        legacy_pose_sources.append(
            {
                **static_source,
                "status": dynamic_source["status"],
                "received_at_ns": dynamic_source["received_at_ns"],
                "measured_tcp": dynamic_source["measured_tcp"],
                "measured_robot": dynamic_source["measured_robot"],
                "objects": [
                    {**item, **dynamic_objects[item["object_id"]]}
                    for item in static_source["objects"]
                ],
            }
        )
    diagnostics = {
        "mode": "viewer_data_plane_v1",
        "served_from_visual_snapshot": True,
        "total_ms": 0,
        "timings_ms": {},
        "slow_steps": {},
        "snapshot_sequence_number": frame["source_sequence"],
        "snapshot_stale": frame["stale"],
    }
    def legacy_shape(shape: dict[str, Any] | None) -> dict[str, Any] | None:
        if shape is None:
            return None
        converted: dict[str, Any] = {
            "type": shape["type"],
            "mesh_asset_url": shape["mesh_asset_url"],
            "mesh_asset_status": shape["mesh_asset_status"],
        }
        for key in (
            "box_dimensions_m",
            "sphere_radius_m",
            "cylinder_radius_m",
            "cylinder_height_m",
            "mesh_resource_uri",
            "mesh_scale",
        ):
            if shape[key] is not None:
                converted[key] = shape[key]
        return converted

    expected_legacy = {
        "scene": {
            "robot": legacy_robot,
            "objects": [
                {
                    **item,
                    "shape": legacy_shape(item["shape"]),
                    "pose_in_universe": frame["entity_transforms"][index],
                }
                for index, item in enumerate(manifest["entities"])
            ],
            "link_poses": dict(
                zip(robot_manifest["link_order"], robot_frame["link_transforms"])
            ),
            "grippers": frame["gripper_states"],
            "pose_sources": legacy_pose_sources,
            "visual_state_diagnostics": diagnostics,
            "object_axes_gizmo_size_m": manifest["object_axes_gizmo_size_m"],
            "timestamp_ns": frame["timestamp_ns"],
        },
        "frames": {
            "frames": [
                {
                    "name": item["name"],
                    "roles": item["roles"],
                    "viewerVisible": item["viewer_visible"],
                    "poseAvailable": frame["frame_transforms"][index] is not None,
                    "poseInWorld": frame["frame_transforms"][index],
                }
                for index, item in enumerate(manifest["frames"])
            ],
            "viewerAvailable": True,
            "frameGizmoSizeM": manifest["frame_axes_gizmo_size_m"],
            "visualStateDiagnostics": diagnostics,
            "timestampNs": frame["timestamp_ns"],
        },
    }
    return json.dumps(
        {
            "schema_version": "manyforge.viewer.preservation_fixture.v1",
            "manifest": manifest,
            "frame": frame,
            "expected_legacy": expected_legacy,
        },
        indent=2,
        sort_keys=True,
    ) + "\n"


def gen_viewer_typescript_fixture() -> str:
    """Generate the same preservation fixture inside the frontend boundary."""
    fixture = json.loads(gen_viewer_preservation_fixture())
    encoded = json.dumps(fixture, indent=2, sort_keys=True, ensure_ascii=True)
    return (
        "// THIS FILE IS GENERATED. DO NOT EDIT.\n"
        "// Source: scripts/regen-public-contracts.py:"
        "gen_viewer_typescript_fixture\n\n"
        "import type { ViewerFrameV1, ViewerManifestV1 } from "
        "'./viewerProtocol.generated';\n\n"
        "export const viewerProtocolFixture = "
        + encoded
        + " satisfies {\n"
        "  schema_version: 'manyforge.viewer.preservation_fixture.v1';\n"
        "  manifest: ViewerManifestV1;\n"
        "  frame: ViewerFrameV1;\n"
        "  expected_legacy: unknown;\n"
        "};\n"
    )


# ---------------------------------------------------------------------------
# REST API generator
# ---------------------------------------------------------------------------

def gen_rest_api() -> str:
    """Render the Composer REST surface from its FastAPI OpenAPI schema."""
    try:
        from manyforge_composer.backend.app import create_app
        state = _build_composer_state()
        app = create_app(state)
        spec = app.openapi()
    except Exception as exc:
        return _stamp("gen_rest_api") + textwrap.dedent(f"""
            # Composer REST API

            **AUTO-GENERATION FAILED** — could not import the Composer
            FastAPI app at module level. This is expected when the
            `manyforge_composer` package is not importable from the env
            running this script. Run from inside the dev container:

                docker compose run --rm dev bash
                python3 scripts/regen-public-contracts.py

            Error captured: `{type(exc).__name__}: {exc}`

            ## Live source of truth

            Run Composer and visit:

            - `http://localhost:9000/openapi.json` — raw OpenAPI 3.0 schema
            - `http://localhost:9000/docs` — interactive Swagger UI
            - `http://localhost:9000/redoc` — ReDoc-rendered reference

            ## Code source

            FastAPI app construction:
            `manyforge_composer/backend/app.py:build_app()`

            Per-router files under `manyforge_composer/backend/routes_*.py`.
        """).lstrip()

    body: list[str] = [_stamp("gen_rest_api"), "# Composer REST API\n"]
    body.append(
        f"FastAPI app, served by `python -m manyforge_composer ...` on the "
        f"port specified by `--port` (default `9000`). The live schema is "
        f"available at `/openapi.json` and `/docs` while Composer is "
        f"running.\n\n"
    )
    body.append(f"OpenAPI version: `{spec.get('openapi', '?')}`. "
                f"Title: `{spec.get('info', {}).get('title', '?')}`.\n\n")
    body.append("## Routes\n\n")
    paths = spec.get("paths", {})
    rows = []
    for path, methods in sorted(paths.items()):
        for method, op in sorted((methods or {}).items()):
            if method.upper() not in ("GET", "POST", "PUT", "DELETE", "PATCH"):
                continue
            summary = (op.get("summary") or op.get("description") or "").splitlines()
            summary_text = summary[0] if summary else ""
            tag = ", ".join(op.get("tags") or [])
            rows.append((method.upper(), path, tag, summary_text))
    body.append("| Method | Path | Tag | Summary |\n|---|---|---|---|\n")
    for method, path, tag, summary in rows:
        body.append(f"| `{method}` | `{path}` | {tag} | {summary} |\n")
    return "".join(body)


# ---------------------------------------------------------------------------
# MCP runtime contract generator
# ---------------------------------------------------------------------------

def gen_mcp_contract() -> str:
    """Render the ManyForge MCP tool surface."""
    try:
        from manyforge_composer.backend.mcp_server import create_mcp_server
        state = _build_composer_state()
        server = create_mcp_server(state)
        # FastMCP exposes the registered tools through its tool manager.
        # Layout has shifted across versions, so try a couple of shapes.
        tool_manager = getattr(server, "_tool_manager", None)
        tools = {}
        if tool_manager is not None:
            tools = getattr(tool_manager, "_tools", None) or {}
            if not tools:
                tools = getattr(tool_manager, "tools", None) or {}
        if not tools:
            tools = getattr(server, "_tools", None) or {}
        if not tools:
            raise RuntimeError("MCP server has no registered tools")
    except Exception as exc:
        return _stamp("gen_mcp_contract") + textwrap.dedent(f"""
            # ManyForge MCP Runtime Contract

            **AUTO-GENERATION FAILED** — could not introspect the MCP
            server module. Error: `{type(exc).__name__}: {exc}`.

            ## Live source of truth

            Start the MCP HTTP shim and query the tools/list endpoint:

                python -m manyforge_composer --mcp-http \\
                  --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml

            Then:

                curl -s -X POST http://localhost:9000/api/mcp \\
                  -H 'Content-Type: application/json' \\
                  -d '{{"method":"tools/list","jsonrpc":"2.0","id":1}}' \\
                  | python3 -m json.tool

            ## Code source

            Tool registrations: `manyforge_composer/backend/mcp_server.py`
            (each `@mcp.tool()` decorator registers one tool).
        """).lstrip()

    body: list[str] = [_stamp("gen_mcp_contract"), "# ManyForge MCP Runtime Contract\n"]
    body.append(
        "ManyForge ships an MCP (Model Context Protocol) server that exposes "
        "Composer's program/scene/tree/blackboard tools to LLM agents over a "
        "stdio transport. Agents can `tools/list` to discover the surface and "
        "`tools/call` to invoke individual tools. The server enforces a "
        "deployment-scoped catalog so only mode-allowed tools are reachable.\n\n"
    )
    body.append(f"Tool count: **{len(tools)}**.\n\n")
    body.append("## Tools (machine-introspected at regen time)\n\n")
    for name, tool in sorted(tools.items()):
        desc = (getattr(tool, "description", "") or "").strip()
        first_line = desc.split("\n", 1)[0]
        body.append(f"### `{name}`\n\n")
        body.append(f"{first_line}\n\n")
        # Show the input schema if available.
        schema = getattr(tool, "parameters", None) or getattr(tool, "input_schema", None)
        if schema:
            try:
                body.append("Input schema:\n```json\n")
                body.append(json.dumps(schema, indent=2, default=str))
                body.append("\n```\n\n")
            except Exception:
                pass
    body.append("## Code source\n\n"
                "All tools are registered with `@mcp.tool()` decorators in "
                "`manyforge_composer/backend/mcp_server.py`. The decorator "
                "captures the tool's name, description (from the function's "
                "docstring), and JSON-Schema input shape from the function's "
                "type hints.\n")
    return "".join(body)


# ---------------------------------------------------------------------------
# Assistant provider contract generator
# ---------------------------------------------------------------------------

def gen_assistant_provider_contract() -> str:
    """Render the assistant-provider HTTP wire format."""
    try:
        from manyforge_assistant_bridge import bridge as bridge_module
        spec = bridge_module.app.openapi()
    except Exception as exc:
        spec = None
        err = exc

    out: list[str] = [_stamp("gen_assistant_provider_contract"),
                      "# Assistant Provider Contract\n"]
    out.append(
        "Composer dispatches assistant requests to a *provider* HTTP service "
        "which speaks the `manyforge.assistant.provider_request.v0` envelope. "
        "Two reference providers ship with manyforge:\n\n"
        "- `nemoclaw` lane: `manyforge_assistant_bridge` on port 8100 — "
        "  runs the agent loop in-process; calls the model directly. "
        "  Same repo as this doc.\n"
        "- `openclaw` lane: `openclaw_assistant_bridge` on port 8200 — "
        "  dispatches into a NemoClaw sandbox running OpenClaw. Lives in "
        "  the platform repo (NemoClaw-Thor) under `manyforge/`.\n\n"
        "Both speak the same wire envelope; this doc covers the wire format "
        "the provider must accept.\n\n"
    )
    if spec:
        out.append("## Bridge HTTP routes (machine-introspected)\n\n")
        out.append("| Method | Path | Summary |\n|---|---|---|\n")
        for path, methods in sorted(spec.get("paths", {}).items()):
            for method, op in sorted((methods or {}).items()):
                if method.upper() not in ("GET", "POST", "PUT", "DELETE", "PATCH"):
                    continue
                summary = (op.get("summary") or "").splitlines()
                summary_text = summary[0] if summary else ""
                out.append(f"| `{method.upper()}` | `{path}` | {summary_text} |\n")
        out.append("\n")
    else:
        out.append(f"_Auto-introspection failed: `{type(err).__name__}: {err}`._\n\n")

    out.append(
        "## Envelope wire format\n\n"
        "The request body sent by Composer to the provider on "
        "`POST /v1/manyforge/assistant`:\n\n"
        "```json\n"
        "{\n"
        '  "version": "manyforge.assistant.provider_request.v0",\n'
        '  "schemaVersion": "0.1.0",\n'
        '  "requestId": "<uuid>",\n'
        '  "conversationId": "<uuid>",\n'
        '  "principal": "<operator id>",\n'
        '  "assistantMode": "composer-assistant" | "query" | ...,\n'
        '  "message": "<user prompt>",\n'
        '  "tools": [ {"id": "...", "description": "...", "parameters": {...}}, ... ],\n'
        '  "constraints": { "mutatesState": bool, "requiresReview": bool, "proposalStatus": "draft", ... },\n'
        '  "context": { "catalogHash": "<sha256>", "programRevision": "...", ... },\n'
        '  "programSnapshot": { ... },\n'
        '  "sceneSnapshot": { ... },\n'
        '  "nodeCatalog": [ ... ],\n'
        '  "skillCatalog": [ ... ]\n'
        "}\n"
        "```\n\n"
        "## Catalog-hash gate\n\n"
        "Every envelope carries a `context.catalogHash` computed by the "
        "shared protocol helper. Any provider implementation must validate "
        "the hash before executing tool calls; mismatch → HTTP 409 with "
        "`code: catalog_mismatch`. Hash function lives in:\n\n"
        "  `manyforge_assistant_bridge/protocol.py:compute_catalog_hash`\n\n"
        "Both the Composer manifest emitter and the bridge consumer compute "
        "the hash from the same canonical input shape, so byte-identical "
        "output is required across the wire.\n\n"
        "## Provider response\n\n"
        "On success, the provider returns:\n\n"
        "```json\n"
        "{\n"
        '  "requestId": "<echo>",\n'
        '  "conversationId": "<echo>",\n'
        '  "providerId": "nemoclaw" | "openclaw" | "...",\n'
        '  "message": "<final assistant text>",\n'
        '  "proposals": [ ... ],          // draft mutations the operator must accept\n'
        '  "toolCalls": [ ... ],          // executed callbacks (audit trail)\n'
        '  "warnings": [ ... ],\n'
        '  "draftMutated": bool,\n'
        '  "openclaw": { ... }            // optional provider-specific telemetry\n'
        "}\n"
        "```\n\n"
        "Errors return non-200 with:\n\n"
        "```json\n"
        "{ \"requestId\": \"<echo>\", \"code\": \"<short_id>\", \"detail\": \"<human-readable>\" }\n"
        "```\n\n"
        "## Code source\n\n"
        "- `manyforge_assistant_bridge/bridge.py` — direct lane provider (this repo).\n"
        "- `manyforge_assistant_bridge/protocol.py` — shared hashing helpers.\n"
        "- `manyforge_composer/backend/routes_assistant.py` — Composer-side "
        "  envelope assembly + tool-callback endpoints (`/api/assistant/chat`, "
        "  `/api/assistant/bridge/tools/<tool_id>`).\n"
    )
    return "".join(out)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--check", action="store_true",
                        help="Exit 1 if regen would change any file (CI mode).")
    args = parser.parse_args()

    REF_DIR.mkdir(parents=True, exist_ok=True)
    targets: dict[Path, Callable[[], str]] = {
        REF_DIR / "REST_API.md": gen_rest_api,
        REF_DIR / "MCP_RUNTIME_CONTRACT.md": gen_mcp_contract,
        REF_DIR / "ASSISTANT_PROVIDER_CONTRACT.md": gen_assistant_provider_contract,
        VIEWER_SCHEMA_PATH: gen_viewer_json_schema,
        VIEWER_FIXTURE_PATH: gen_viewer_preservation_fixture,
        VIEWER_QUALIFICATION_SCHEMA_PATH: gen_viewer_qualification_json_schema,
        VIEWER_QUALIFICATION_MATRIX_SCHEMA_PATH: (
            gen_viewer_qualification_matrix_json_schema
        ),
        VIEWER_TYPES_PATH: gen_viewer_typescript,
        VIEWER_TYPES_FIXTURE_PATH: gen_viewer_typescript_fixture,
    }
    drift = []
    for path, generator in targets.items():
        new_content = generator()
        old_content = path.read_text() if path.exists() else ""
        new_comparable = _normalized_generated_content(path, new_content)
        old_comparable = _normalized_generated_content(path, old_content)
        if args.check:
            if new_comparable != old_comparable:
                drift.append(str(path.relative_to(REPO_ROOT)))
        else:
            if new_comparable == old_comparable:
                print(f"  unchanged {path.relative_to(REPO_ROOT)}")
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(new_content)
            print(f"  wrote {path.relative_to(REPO_ROOT)} ({len(new_content)} chars)")
    if args.check and drift:
        print("ERROR: regen would change:", ", ".join(drift), file=sys.stderr)
        print("Run scripts/regen-public-contracts.py to update.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
