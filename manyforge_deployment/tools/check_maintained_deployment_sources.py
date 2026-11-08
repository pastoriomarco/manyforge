#!/usr/bin/env python3
"""Validate the maintained model families and their component model descriptors."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
import sys
from pathlib import Path
from typing import Any, Final

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    load_strict_json_yaml_bytes,
)

API_VERSION: Final = "manyforge.maintained_deployment_sources.v1"
_ID = re.compile(r"^[a-z][a-z0-9_.-]{0,127}$")

_EXPECTED_ASSISTANT_PROFILES_SOURCE: Final = (
    "manyforge_deployment/resources/maintained/maintained_assistant_profiles.v1.json"
)
_EXPECTED_ASSISTANT_PROFILE_CONTRACTS: Final = {
    "assistant_scene_authoring": {
        "mode_profile_id": "scene_authoring",
        "endpoint_aliases": ("assistant_model",),
        "historical_profiles": ("thor_gemma4_12b_gguf",),
        "provider_kind": "nemoclaw",
    },
    "semantic_shape_selection": {
        "mode_profile_id": "scene_authoring",
        "endpoint_aliases": ("assistant_model", "selection_model"),
        "historical_profiles": (
            "thor_cosmos_reason2_8b",
            "thor_manyforge_qwen36",
        ),
        "provider_kind": "direct",
    },
    "vision_bin_selection": {
        "mode_profile_id": None,
        "endpoint_aliases": ("vision_model",),
        "historical_profiles": ("thor_manyforge_qwen36",),
        "provider_kind": None,
    },
    "vision_pose_selection": {
        "mode_profile_id": "scene_authoring",
        "endpoint_aliases": ("assistant_model", "vision_model"),
        "historical_profiles": (
            "thor_cosmos_reason2_8b",
            "thor_cosmos_reason2_8b",
        ),
        "provider_kind": "direct",
    },
}
_EXPECTED_ASSISTANT_REQUEST_CONTRACTS: Final = {
    ("assistant_scene_authoring", "assistant_model"): {
        "default_timeout_s": 180.0,
        "max_output_tokens": 8192,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": False,
        "supported_image_transports": [],
        "tool_call_family": "openai_chat_tools",
    },
    ("semantic_shape_selection", "assistant_model"): {
        "default_timeout_s": 180.0,
        "max_output_tokens": 8192,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": True,
        "supported_image_transports": ["http_url", "inline_data_url"],
        "tool_call_family": "openai_chat_tools",
    },
    ("semantic_shape_selection", "selection_model"): {
        "default_timeout_s": 30.0,
        "max_output_tokens": 384,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": False,
        "supported_image_transports": [],
        "tool_call_family": "openai_chat_tools",
    },
    ("vision_bin_selection", "vision_model"): {
        "default_timeout_s": 30.0,
        "max_output_tokens": 512,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": True,
        "supported_image_transports": ["http_url", "inline_data_url"],
        "tool_call_family": "openai_chat_tools",
    },
    ("vision_pose_selection", "assistant_model"): {
        "default_timeout_s": 180.0,
        "max_output_tokens": 8192,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": True,
        "supported_image_transports": ["http_url", "inline_data_url"],
        "tool_call_family": "openai_chat_tools",
    },
    ("vision_pose_selection", "vision_model"): {
        "default_timeout_s": 120.0,
        "max_output_tokens": 512,
        "supports_response_tools": True,
        "supports_json_object": True,
        "supports_image_input": True,
        "supported_image_transports": ["http_url", "inline_data_url"],
        "tool_call_family": "openai_chat_tools",
    },
}
_TEMPLATE_VARIABLE_TYPES: Final = frozenset(
    {"bool", "float64", "int64", "string", "string_list"}
)
_EXPECTED_ASSISTANT_PROMPT_CONTRACTS: Final = {
    ("semantic_shape_selection", "select_scene_object_v1"): {
        "version": "1",
        "template_text_sha256": (
            "f5acfe169fa25ab07f94ec2711cf8872a96a869def7275a9b3be8f46418d83ef"
        ),
        "variables": {
            "candidate_object_ids": "string_list",
            "scene_context_json": "string",
            "selection_request_text": "string",
        },
        "response_contract": {
            "response_mode": "tool_call",
            "tool_name": "select_scene_object",
            "tool_description": (
                "Choose one candidate object from the current runtime scene context."
            ),
        },
    },
    ("vision_bin_selection", "choose_bin_target_v1"): {
        "version": "1",
        "template_text_sha256": (
            "8a1f6703c7aadaa2c427c43932a4b7c6bfe45468a02a221f130917a90f5e6314"
        ),
        "variables": {},
        "response_contract": {"response_mode": "json_object"},
    },
    ("vision_pose_selection", "infer_grasp_pose_v1"): {
        "version": "1",
        "template_text_sha256": (
            "29888cd8ca46c597e6fae283d3f7e2512092fe9f1a9874781c46da5754516c49"
        ),
        "variables": {},
        "response_contract": {"response_mode": "json_object"},
    },
}
_EXPECTED_MODEL_FAMILIES: Final = {
    "robotiq_2f_140": {
        "frames": {"mount": "robotiq_2f_140_mount", "tcp": "grasp_frame"},
        "groups": {"gripper": "gripper"},
        "resources": {"fingers": ("finger_joint",)},
        "collisions": {
            "gripper_collision": (
                "left_inner_finger",
                "left_inner_finger_pad",
                "left_inner_knuckle",
                "left_outer_finger",
                "left_outer_knuckle",
                "right_inner_finger",
                "right_inner_finger_pad",
                "right_inner_knuckle",
                "right_outer_finger",
                "right_outer_knuckle",
                "robotiq_base_link",
            ),
        },
        "root": "mount",
        "source": {
            "accepted_package_uri_prefix": "package://manyforge_example_models/models/ur10e_robotiq_2f_140/",
            "historical_srdf_path": "manyforge_example_models/models/robotiq_2f_140/robotiq_2f_140.srdf",
            "historical_urdf_path": "manyforge_example_models/models/robotiq_2f_140/robotiq_2f_140.urdf",
            "resource_root": "manyforge_example_models/models/ur10e_robotiq_2f_140",
            "srdf_path": "manyforge_example_models/models/robotiq_2f_140/robotiq_2f_140.structural.srdf",
            "srdf_projection_kind": "remove_hpp_gripper_extension",
            "urdf_path": "manyforge_example_models/models/robotiq_2f_140/robotiq_2f_140.structural.urdf",
            "urdf_projection_kind": "remove_unsupported_collision_materials",
        },
    },
    "ur5_roboplan": {
        "frames": {
            "base": "base_link",
            "flange": "tool0",
            "gripper_tcp": "tool0",
            "root": "world",
            "tcp": "tool0",
        },
        "groups": {"arm": "arm"},
        "resources": {
            "arm_joints": (
                "shoulder_pan_joint",
                "shoulder_lift_joint",
                "elbow_joint",
                "wrist_1_joint",
                "wrist_2_joint",
                "wrist_3_joint",
            )
        },
        "collisions": {
            "arm_collision": (
                "base_link",
                "forearm_link",
                "shoulder_link",
                "upper_arm_link",
                "wrist_1_link",
                "wrist_2_link",
                "wrist_3_link",
            ),
            "gripper_collision": ("ee_link", "tool0"),
            "tool_collision": ("ee_link", "tool0"),
        },
        "root": "root",
        "source": {
            "accepted_package_uri_prefix": "package://roboplan_example_models/models/ur_robot_model/",
            "historical_srdf_path": ".docker/roboplan/roboplan_example_models/models/ur_robot_model/ur5_gripper.srdf",
            "historical_urdf_path": ".docker/roboplan/roboplan_example_models/models/ur_robot_model/ur5_gripper.urdf",
            "resource_root": ".docker/roboplan/roboplan_example_models/models/ur_robot_model",
            "srdf_path": "manyforge_deployment/resources/maintained/model_sources/ur5_roboplan/ur5_gripper.structural.srdf",
            "srdf_projection_kind": "remove_hpp_gripper_extension",
            "urdf_path": "manyforge_deployment/resources/maintained/model_sources/ur5_roboplan/ur5_gripper.structural.urdf",
            "urdf_projection_kind": "remove_unused_xacro_namespace",
        },
    },
}


class SourceDescriptionError(ValueError):
    """The maintained authoring description is malformed or stale."""


def _reject_duplicate_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for name, value in pairs:
        if name in result:
            raise SourceDescriptionError(f"duplicate JSON member {name!r}")
        result[name] = value
    return result


def _object(value: object, *, path: str) -> dict[str, Any]:
    if type(value) is not dict:
        raise SourceDescriptionError(f"{path} must be an object")
    return value


def _array(value: object, *, path: str) -> list[Any]:
    if type(value) is not list:
        raise SourceDescriptionError(f"{path} must be an array")
    return value


def _exact(
    value: object,
    *,
    required: frozenset[str],
    path: str,
) -> dict[str, Any]:
    row = _object(value, path=path)
    missing = sorted(required - set(row))
    unknown = sorted(set(row) - required)
    if missing or unknown:
        raise SourceDescriptionError(
            f"{path} has wrong fields; missing={missing}, unknown={unknown}"
        )
    return row


def _offset_pose(
    value: object, *, path: str
) -> tuple[tuple[float, ...], tuple[float, ...]]:
    """`{position_m: [3], orientation_xyzw: [4]}`, finite, unit quaternion."""

    pose = _exact(value, required=frozenset({"position_m", "orientation_xyzw"}), path=path)
    parts = []
    for field, length in (("position_m", 3), ("orientation_xyzw", 4)):
        numbers = pose[field]
        if (
            type(numbers) is not list
            or len(numbers) != length
            or any(
                type(item) not in {int, float} or not math.isfinite(item)
                for item in numbers
            )
        ):
            raise SourceDescriptionError(
                f"{path}.{field} must hold {length} finite numbers"
            )
        parts.append(tuple(float(item) for item in numbers))
    if abs(sum(item * item for item in parts[1]) - 1.0) > 1e-6:
        raise SourceDescriptionError(f"{path}.orientation_xyzw must be a unit quaternion")
    return parts[0], parts[1]


def _model_payload(
    value: object, *, path: str
) -> tuple[float, tuple[float, ...]]:
    """A model's own payload: mass in [0, 100] kg, centre of mass [3] in [-2, 2] m."""

    payload = _exact(value, required=frozenset({"mass_kg", "center_of_mass_m"}), path=path)
    mass = payload["mass_kg"]
    if type(mass) not in {int, float} or not math.isfinite(mass) or not 0 <= mass <= 100:
        raise SourceDescriptionError(f"{path}.mass_kg must be a finite number in [0, 100]")
    center = payload["center_of_mass_m"]
    if (
        type(center) is not list
        or len(center) != 3
        or any(
            type(item) not in {int, float} or not math.isfinite(item) or not -2 <= item <= 2
            for item in center
        )
    ):
        raise SourceDescriptionError(
            f"{path}.center_of_mass_m must hold 3 finite numbers in [-2, 2]"
        )
    return float(mass), tuple(float(item) for item in center)


def _text(value: object, *, path: str) -> str:
    if type(value) is not str or not value.strip() or len(value) > 4096:
        raise SourceDescriptionError(f"{path} must be bounded non-empty text")
    return value


def _identifier(value: object, *, path: str) -> str:
    text = _text(value, path=path)
    if _ID.fullmatch(text) is None:
        raise SourceDescriptionError(f"{path} must be a portable identifier")
    return text


def _source_path(value: object, *, root: Path, path: str) -> str:
    text = _text(value, path=path)
    candidate = Path(text)
    if (
        candidate.is_absolute()
        or ".." in candidate.parts
        or text != candidate.as_posix()
    ):
        raise SourceDescriptionError(f"{path} must be a root-local normalized path")
    resolved = root / candidate
    if resolved.is_symlink() or not resolved.is_file():
        raise SourceDescriptionError(f"{path} does not select a plain source file")
    return text


def _directory_path(value: object, *, root: Path, path: str) -> str:
    text = _text(value, path=path)
    candidate = Path(text)
    if (
        candidate.is_absolute()
        or ".." in candidate.parts
        or text != candidate.as_posix()
    ):
        raise SourceDescriptionError(f"{path} must be a root-local normalized path")
    resolved = root / candidate
    if resolved.is_symlink() or not resolved.is_dir():
        raise SourceDescriptionError(f"{path} does not select a plain source directory")
    return text


def _source_symbol(value: object, *, path: str) -> str:
    text = _text(value, path=path)
    if len(text) > 1024 or any(
        character.isspace() or not character.isprintable() for character in text
    ):
        raise SourceDescriptionError(f"{path} must be one bounded source symbol")
    return text


def _canonical_unique(
    values: list[str], *, path: str, sort_values: bool = True
) -> None:
    if len(values) != len(set(values)) or (sort_values and values != sorted(values)):
        raise SourceDescriptionError(f"{path} must be unique and canonical-sorted")


def _validate_assistant_profiles(source: Path) -> set[str]:
    try:
        value = json.loads(
            source.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_pairs,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise SourceDescriptionError(
            f"cannot read maintained assistant profiles: {error}"
        ) from error
    top = _exact(
        value,
        required=frozenset({"api_version", "mode_profiles", "profiles"}),
        path="assistant_profiles",
    )
    if top["api_version"] != "manyforge.maintained_assistant_profiles.v1":
        raise SourceDescriptionError(
            "assistant_profiles.api_version is not the maintained v1 contract"
        )

    mode_profiles = _object(
        top["mode_profiles"], path="assistant_profiles.mode_profiles"
    )
    if set(mode_profiles) != {"scene_authoring"}:
        raise SourceDescriptionError(
            "assistant_profiles.mode_profiles changes the exact maintained set"
        )
    modes = _object(
        mode_profiles["scene_authoring"],
        path="assistant_profiles.mode_profiles.scene_authoring",
    )
    if set(modes) != {"composer-assistant"}:
        raise SourceDescriptionError(
            "assistant scene-authoring profile changes the reserved mode set"
        )
    for mode_id, raw_mode in modes.items():
        mode_path = f"assistant_profiles.mode_profiles.scene_authoring.{mode_id}"
        mode = _exact(
            raw_mode,
            required=frozenset({"enabled", "description", "grant_policy", "catalog"}),
            path=mode_path,
        )
        if mode["enabled"] is not True:
            raise SourceDescriptionError(f"{mode_path}.enabled must remain true")
        _text(mode["description"], path=f"{mode_path}.description")
        grant = _exact(
            mode["grant_policy"],
            required=frozenset({"session_kind", "approval_mode", "review_policy"}),
            path=f"{mode_path}.grant_policy",
        )
        if (
            grant["session_kind"] is not None
            or grant["approval_mode"] is not None
            or grant["review_policy"] != "required"
        ):
            raise SourceDescriptionError(
                f"{mode_path}.grant_policy changes reserved offline semantics"
            )
        catalog = _exact(
            mode["catalog"],
            required=frozenset({"skills", "tools", "nodes"}),
            path=f"{mode_path}.catalog",
        )
        for kind in ("skills", "tools", "nodes"):
            ids = [
                _identifier(item, path=f"{mode_path}.catalog.{kind}[{index}]")
                for index, item in enumerate(
                    _array(catalog[kind], path=f"{mode_path}.catalog.{kind}")
                )
            ]
            _canonical_unique(ids, path=f"{mode_path}.catalog.{kind}")

    profile_ids: list[str] = []
    site_overlay_value_refs: set[str] = set()
    for index, raw_profile in enumerate(
        _array(top["profiles"], path="assistant_profiles.profiles")
    ):
        profile_path = f"assistant_profiles.profiles[{index}]"
        profile = _exact(
            raw_profile,
            required=frozenset(
                {
                    "profile_id",
                    "audit_policy",
                    "prompt_templates",
                    "model_endpoints",
                    "provider_alias",
                    "mode_profile_id",
                }
            ),
            path=profile_path,
        )
        profile_id = _identifier(
            profile["profile_id"], path=f"{profile_path}.profile_id"
        )
        profile_ids.append(profile_id)
        expected = _EXPECTED_ASSISTANT_PROFILE_CONTRACTS.get(profile_id)
        if expected is None:
            raise SourceDescriptionError(
                f"{profile_path}.profile_id is outside the maintained set"
            )
        audit = _exact(
            profile["audit_policy"],
            required=frozenset(
                {
                    "store_full_prompts",
                    "store_full_responses",
                    "redact_argument_values",
                }
            ),
            path=f"{profile_path}.audit_policy",
        )
        if audit != {
            "store_full_prompts": False,
            "store_full_responses": False,
            "redact_argument_values": True,
        }:
            raise SourceDescriptionError(
                f"{profile_path}.audit_policy weakens the historical redaction policy"
            )
        templates = _object(
            profile["prompt_templates"],
            path=f"{profile_path}.prompt_templates",
        )
        for template_id, raw_template in templates.items():
            _identifier(template_id, path=f"{profile_path}.prompt_templates key")
            template = _exact(
                raw_template,
                required=frozenset(
                    {
                        "version",
                        "template_text",
                        "variables",
                        "response_contract",
                    }
                ),
                path=f"{profile_path}.prompt_templates.{template_id}",
            )
            expected_template = _EXPECTED_ASSISTANT_PROMPT_CONTRACTS.get(
                (profile_id, template_id)
            )
            if expected_template is None:
                raise SourceDescriptionError(
                    f"{profile_path}.prompt_templates.{template_id} is outside "
                    "the maintained historical set"
                )
            version = _text(
                template["version"],
                path=f"{profile_path}.prompt_templates.{template_id}.version",
            )
            if not version.isascii() or any(
                not (character.isalnum() or character in "_.-") for character in version
            ):
                raise SourceDescriptionError(
                    f"{profile_path}.prompt_templates.{template_id}.version "
                    "must be a portable token"
                )
            template_text = _text(
                template["template_text"],
                path=f"{profile_path}.prompt_templates.{template_id}.template_text",
            )
            variables_path = f"{profile_path}.prompt_templates.{template_id}.variables"
            variables = _object(template["variables"], path=variables_path)
            for variable_id, variable_type in variables.items():
                _identifier(variable_id, path=f"{variables_path} key")
                if variable_type not in _TEMPLATE_VARIABLE_TYPES:
                    raise SourceDescriptionError(
                        f"{variables_path}.{variable_id} is outside the closed type set"
                    )
            response_path = (
                f"{profile_path}.prompt_templates.{template_id}.response_contract"
            )
            raw_response = _object(template["response_contract"], path=response_path)
            response_mode = _text(
                raw_response.get("response_mode"),
                path=f"{response_path}.response_mode",
            )
            if response_mode == "json_object":
                response = _exact(
                    raw_response,
                    required=frozenset({"response_mode"}),
                    path=response_path,
                )
            elif response_mode == "tool_call":
                response = _exact(
                    raw_response,
                    required=frozenset(
                        {"response_mode", "tool_name", "tool_description"}
                    ),
                    path=response_path,
                )
                _identifier(response["tool_name"], path=f"{response_path}.tool_name")
                _text(
                    response["tool_description"],
                    path=f"{response_path}.tool_description",
                )
            else:
                raise SourceDescriptionError(
                    f"{response_path}.response_mode is outside the closed set"
                )
            if (
                version != expected_template["version"]
                or hashlib.sha256(template_text.encode("utf-8")).hexdigest()
                != expected_template["template_text_sha256"]
                or variables != expected_template["variables"]
                or response != expected_template["response_contract"]
            ):
                raise SourceDescriptionError(
                    f"{profile_path}.prompt_templates.{template_id} changes "
                    "the exact maintained prompt/response contract"
                )

        expected_template_ids = {
            template_id
            for (expected_profile_id, template_id) in (
                _EXPECTED_ASSISTANT_PROMPT_CONTRACTS
            )
            if expected_profile_id == profile_id
        }
        if set(templates) != expected_template_ids:
            raise SourceDescriptionError(
                f"{profile_path}.prompt_templates changes the maintained set"
            )

        endpoint_aliases: list[str] = []
        historical_profiles: list[str] = []
        for endpoint_index, raw_endpoint in enumerate(
            _array(profile["model_endpoints"], path=f"{profile_path}.model_endpoints")
        ):
            endpoint_path = f"{profile_path}.model_endpoints[{endpoint_index}]"
            endpoint = _exact(
                raw_endpoint,
                required=frozenset(
                    {
                        "model_endpoint_alias",
                        "model_id",
                        "site_overlay_value_ref",
                        "endpoint_value",
                        "historical_profile_ref",
                        "request_contract",
                    }
                ),
                path=endpoint_path,
            )
            endpoint_alias = _identifier(
                endpoint["model_endpoint_alias"],
                path=f"{endpoint_path}.model_endpoint_alias",
            )
            endpoint_aliases.append(endpoint_alias)
            _identifier(endpoint["model_id"], path=f"{endpoint_path}.model_id")
            site_overlay_value_ref = _identifier(
                endpoint["site_overlay_value_ref"],
                path=f"{endpoint_path}.site_overlay_value_ref",
            )
            if site_overlay_value_ref in site_overlay_value_refs:
                raise SourceDescriptionError(
                    f"{endpoint_path}.site_overlay_value_ref is duplicated"
                )
            site_overlay_value_refs.add(site_overlay_value_ref)
            endpoint_value = _text(
                endpoint["endpoint_value"], path=f"{endpoint_path}.endpoint_value"
            )
            if not endpoint_value.startswith("http://127.0.0.1:"):
                raise SourceDescriptionError(
                    f"{endpoint_path}.endpoint_value is not the maintained local endpoint"
                )
            historical_profiles.append(
                _identifier(
                    endpoint["historical_profile_ref"],
                    path=f"{endpoint_path}.historical_profile_ref",
                )
            )
            request_path = f"{endpoint_path}.request_contract"
            request = _exact(
                endpoint["request_contract"],
                required=frozenset(
                    {
                        "default_timeout_s",
                        "max_output_tokens",
                        "supports_response_tools",
                        "supports_json_object",
                        "supports_image_input",
                        "supported_image_transports",
                        "tool_call_family",
                    }
                ),
                path=request_path,
            )
            timeout = request["default_timeout_s"]
            tokens = request["max_output_tokens"]
            if (
                type(timeout) not in {int, float}
                or not math.isfinite(timeout)
                or timeout <= 0.0
                or type(tokens) is not int
                or not 0 < tokens <= 1_048_576
            ):
                raise SourceDescriptionError(
                    f"{request_path} has invalid bounded request limits"
                )
            for field in (
                "supports_response_tools",
                "supports_json_object",
                "supports_image_input",
            ):
                if type(request[field]) is not bool:
                    raise SourceDescriptionError(
                        f"{request_path}.{field} must be boolean"
                    )
            transports = [
                _identifier(
                    item,
                    path=(
                        f"{request_path}.supported_image_transports"
                        f"[{transport_index}]"
                    ),
                )
                for transport_index, item in enumerate(
                    _array(
                        request["supported_image_transports"],
                        path=f"{request_path}.supported_image_transports",
                    )
                )
            ]
            _canonical_unique(
                transports,
                path=f"{request_path}.supported_image_transports",
            )
            if any(
                transport not in {"http_url", "inline_data_url"}
                for transport in transports
            ):
                raise SourceDescriptionError(
                    f"{request_path}.supported_image_transports is outside the closed set"
                )
            if request["supports_image_input"] != bool(transports):
                raise SourceDescriptionError(
                    f"{request_path}.supports_image_input contradicts its transports"
                )
            _identifier(
                request["tool_call_family"],
                path=f"{request_path}.tool_call_family",
            )
            normalized_request = dict(request)
            normalized_request["supported_image_transports"] = transports
            if normalized_request != _EXPECTED_ASSISTANT_REQUEST_CONTRACTS.get(
                (profile_id, endpoint_alias)
            ):
                raise SourceDescriptionError(
                    f"{request_path} changes the exact maintained request contract"
                )
        _canonical_unique(endpoint_aliases, path=f"{profile_path}.model_endpoints")
        if (
            tuple(endpoint_aliases) != expected["endpoint_aliases"]
            or tuple(historical_profiles) != expected["historical_profiles"]
            or profile["mode_profile_id"] != expected["mode_profile_id"]
        ):
            raise SourceDescriptionError(
                f"{profile_path} changes the maintained endpoint/mode mapping"
            )

        provider = profile["provider_alias"]
        if provider is None:
            provider_kind = None
        else:
            provider_path = f"{profile_path}.provider_alias"
            provider_row = _exact(
                provider,
                required=frozenset(
                    {
                        "provider_alias_id",
                        "provider_kind",
                        "bridge_external_resource_alias",
                        "site_overlay_value_ref",
                        "endpoint_value",
                    }
                ),
                path=provider_path,
            )
            _identifier(
                provider_row["provider_alias_id"],
                path=f"{provider_path}.provider_alias_id",
            )
            _identifier(
                provider_row["bridge_external_resource_alias"],
                path=f"{provider_path}.bridge_external_resource_alias",
            )
            site_overlay_value_ref = _identifier(
                provider_row["site_overlay_value_ref"],
                path=f"{provider_path}.site_overlay_value_ref",
            )
            if site_overlay_value_ref in site_overlay_value_refs:
                raise SourceDescriptionError(
                    f"{provider_path}.site_overlay_value_ref is duplicated"
                )
            site_overlay_value_refs.add(site_overlay_value_ref)
            _text(
                provider_row["endpoint_value"], path=f"{provider_path}.endpoint_value"
            )
            provider_kind = provider_row["provider_kind"]
            if provider_kind not in {"direct", "nemoclaw"}:
                raise SourceDescriptionError(
                    f"{provider_path}.provider_kind is outside the closed v1 set"
                )
        if provider_kind != expected["provider_kind"]:
            raise SourceDescriptionError(
                f"{profile_path}.provider_alias changes the maintained provider kind"
            )

    if profile_ids != sorted(profile_ids) or set(profile_ids) != set(
        _EXPECTED_ASSISTANT_PROFILE_CONTRACTS
    ):
        raise SourceDescriptionError(
            "assistant_profiles.profiles must contain the canonical maintained set"
        )
    return set(profile_ids)


def _validate_model_semantics(
    value: object,
    *,
    family_id: str,
    path: str,
) -> None:
    semantics = _exact(
        value,
        required=frozenset(
            {
                "collision_body_role_bindings",
                "frame_role_bindings",
                "instance_cardinality",
                "model_group_role_bindings",
                "resource_role_bindings",
                "root_frame_role_id",
            }
        )
        | (
            frozenset({"payload"})
            if type(value) is dict and "payload" in value
            else frozenset()
        ),
        path=path,
    )
    payload = (
        _model_payload(semantics["payload"], path=f"{path}.payload")
        if "payload" in semantics
        else None
    )
    if semantics["instance_cardinality"] != "multiple_per_cell":
        raise SourceDescriptionError(
            f"{path}.instance_cardinality must preserve multi-instance model use"
        )

    frame_roles: list[str] = []
    frame_bindings: dict[str, str] = {}
    frame_offsets: dict[str, tuple[tuple[float, ...], tuple[float, ...]]] = {}
    for index, raw_binding in enumerate(
        _array(semantics["frame_role_bindings"], path=f"{path}.frame_role_bindings")
    ):
        binding_path = f"{path}.frame_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"frame_role_id", "source_link_name"})
            | (
                frozenset({"pose_in_source_link"})
                if type(raw_binding) is dict and "pose_in_source_link" in raw_binding
                else frozenset()
            ),
            path=binding_path,
        )
        frame_role = _identifier(
            binding["frame_role_id"], path=f"{binding_path}.frame_role_id"
        )
        frame_roles.append(frame_role)
        frame_bindings[frame_role] = _source_symbol(
            binding["source_link_name"], path=f"{binding_path}.source_link_name"
        )
        if "pose_in_source_link" in binding:
            frame_offsets[frame_role] = _offset_pose(
                binding["pose_in_source_link"],
                path=f"{binding_path}.pose_in_source_link",
            )
    _canonical_unique(frame_roles, path=f"{path}.frame_role_bindings")
    # An offset frame is a generated link named after its role: its
    # parent is a source link, never another offset frame.
    for frame_role in frame_offsets:
        if frame_bindings[frame_role] in frame_offsets:
            raise SourceDescriptionError(
                f"{path}.frame_role_bindings: offset frame {frame_role!r} chains "
                "onto another offset frame"
            )

    group_roles: list[str] = []
    group_bindings: dict[str, str] = {}
    for index, raw_binding in enumerate(
        _array(
            semantics["model_group_role_bindings"],
            path=f"{path}.model_group_role_bindings",
        )
    ):
        binding_path = f"{path}.model_group_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"model_group_role_id", "source_group_name"}),
            path=binding_path,
        )
        group_role = _identifier(
            binding["model_group_role_id"],
            path=f"{binding_path}.model_group_role_id",
        )
        group_roles.append(group_role)
        group_bindings[group_role] = _source_symbol(
            binding["source_group_name"], path=f"{binding_path}.source_group_name"
        )
    _canonical_unique(group_roles, path=f"{path}.model_group_role_bindings")

    resource_roles: list[str] = []
    resource_bindings: dict[str, tuple[str, ...]] = {}
    for index, raw_binding in enumerate(
        _array(
            semantics["resource_role_bindings"],
            path=f"{path}.resource_role_bindings",
        )
    ):
        binding_path = f"{path}.resource_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"resource_role_id", "source_joint_names"}),
            path=binding_path,
        )
        resource_role = _identifier(
            binding["resource_role_id"], path=f"{binding_path}.resource_role_id"
        )
        resource_roles.append(resource_role)
        joint_names = [
            _source_symbol(
                joint, path=f"{binding_path}.source_joint_names[{joint_index}]"
            )
            for joint_index, joint in enumerate(
                _array(
                    binding["source_joint_names"],
                    path=f"{binding_path}.source_joint_names",
                )
            )
        ]
        if not joint_names or len(joint_names) != len(set(joint_names)):
            raise SourceDescriptionError(
                f"{binding_path}.source_joint_names must be non-empty and unique"
            )
        resource_bindings[resource_role] = tuple(joint_names)
    _canonical_unique(resource_roles, path=f"{path}.resource_role_bindings")

    collision_roles: list[str] = []
    collision_bindings: dict[str, tuple[str, ...]] = {}
    for index, raw_binding in enumerate(
        _array(
            semantics["collision_body_role_bindings"],
            path=f"{path}.collision_body_role_bindings",
        )
    ):
        binding_path = f"{path}.collision_body_role_bindings[{index}]"
        binding = _exact(
            raw_binding,
            required=frozenset({"collision_body_role_id", "source_link_names"}),
            path=binding_path,
        )
        collision_role = _identifier(
            binding["collision_body_role_id"],
            path=f"{binding_path}.collision_body_role_id",
        )
        collision_roles.append(collision_role)
        link_names = [
            _source_symbol(link, path=f"{binding_path}.source_link_names[{link_index}]")
            for link_index, link in enumerate(
                _array(
                    binding["source_link_names"],
                    path=f"{binding_path}.source_link_names",
                )
            )
        ]
        if not link_names:
            raise SourceDescriptionError(
                f"{binding_path}.source_link_names must not be empty"
            )
        _canonical_unique(link_names, path=f"{binding_path}.source_link_names")
        collision_bindings[collision_role] = tuple(link_names)
    _canonical_unique(collision_roles, path=f"{path}.collision_body_role_bindings")

    root_frame_role = _identifier(
        semantics["root_frame_role_id"], path=f"{path}.root_frame_role_id"
    )
    # A family added as data has no pin; its roles are checked against its
    # URDF/SRDF by `manyforge component check` (its component.yaml).
    expected = _EXPECTED_MODEL_FAMILIES.get(family_id)
    if expected is None:
        if root_frame_role not in frame_roles:
            raise SourceDescriptionError(
                f"{path}.root_frame_role_id names no frame role binding"
            )
        return
    if (
        frame_bindings != expected["frames"]
        or frame_offsets != expected.get("offsets", {})
        or payload != expected.get("payload")
        or group_bindings != expected["groups"]
        or resource_bindings != expected["resources"]
        or collision_bindings != expected["collisions"]
        or root_frame_role != expected["root"]
        or root_frame_role not in frame_roles
    ):
        raise SourceDescriptionError(
            f"{path} changes the accepted source-local family role contract"
        )
















def _validate_component_models(
    value: object, root: Path, model_ids: set[str]
) -> None:
    """`component_models`: sorted, distinct descriptor paths of machines,
    model-form I/O devices (an `io_device` with `lanes`) and sensors, each a
    distinct id."""
    if value is None:
        return
    paths = _array(value, path="$.component_models")
    if paths != sorted(paths) or len(set(paths)) != len(paths):
        raise SourceDescriptionError("$.component_models must be sorted and distinct")
    for index, raw in enumerate(paths):
        path = f"$.component_models[{index}]"
        relative = _source_path(raw, root=root, path=path)
        try:
            descriptor = load_strict_json_yaml_bytes(
                (root / relative).read_bytes(), source=str(root / relative)
            )
        except (OSError, StrictArtifactError) as error:
            raise SourceDescriptionError(f"{path} cannot be read: {error}") from error
        descriptor = _object(descriptor, path=path)
        model_id = _identifier(descriptor.get("id"), path=f"{path}.id")
        kind = descriptor.get("kind")
        if model_id in model_ids or not (
            kind in ("machine", "sensor")
            or (kind == "io_device" and isinstance(descriptor.get("lanes"), dict))
        ):
            raise SourceDescriptionError(
                f"{path} must name one distinct machine, model-form io_device or sensor"
            )
        model_ids.add(model_id)


def validate(source: Path, root: Path) -> None:
    try:
        value = json.loads(
            source.read_text(encoding="utf-8"),
            object_pairs_hook=_reject_duplicate_pairs,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise SourceDescriptionError(
            f"cannot read maintained source description: {error}"
        ) from error
    top = _exact(
        value,
        required=frozenset({"api_version", "model_families"})
        # Machines and model-form I/O devices the compiler reads without a
        # URDF (`manyforge component register`).
        | frozenset(
            {"component_models"}
            if type(value) is dict and "component_models" in value
            else ()
        ),
        path="$",
    )
    if top["api_version"] != API_VERSION:
        raise SourceDescriptionError(f"$.api_version must be {API_VERSION}")
    # The assistant profiles the compiler resolves by `assistant_profile_id`.
    _validate_assistant_profiles(root / _EXPECTED_ASSISTANT_PROFILES_SOURCE)

    family_ids: list[str] = []
    model_ids: set[str] = set()
    _validate_component_models(top.get("component_models"), root, model_ids)
    for index, raw_family in enumerate(
        _array(top["model_families"], path="$.model_families")
    ):
        path = f"$.model_families[{index}]"
        family = _exact(
            raw_family,
            required=frozenset(
                {
                    "catalog_source",
                    "model_family_id",
                    "model_asset_id",
                    "semantics",
                    "source",
                }
            )
            # A descriptor-registered family may record its TCPs, the default
            # first (`manyforge component register`), and names its component
            # model descriptor when a component may select it.
            | frozenset(
                name
                for name in ("component_descriptor", "tcp_frame_role_ids")
                if type(raw_family) is dict and name in raw_family
            ),
            path=path,
        )
        family_id = _identifier(
            family["model_family_id"], path=f"{path}.model_family_id"
        )
        family_ids.append(family_id)
        model_asset_id = _identifier(
            family["model_asset_id"], path=f"{path}.model_asset_id"
        )
        if model_asset_id != family_id:
            raise SourceDescriptionError(
                f"{path}.model_asset_id must equal its model_family_id"
            )
        expected_catalog_source = (
            f"manyforge_deployment/resources/maintained/model_assets/"
            f"{model_asset_id}.model_asset.json"
        )
        catalog_source = _source_path(
            family["catalog_source"], root=root, path=f"{path}.catalog_source"
        )
        if catalog_source != expected_catalog_source:
            raise SourceDescriptionError(
                f"{path}.catalog_source is not the shared family source"
            )
        _validate_model_semantics(
            family["semantics"], family_id=family_id, path=f"{path}.semantics"
        )
        if "component_descriptor" in family:
            descriptor_path = _source_path(
                family["component_descriptor"],
                root=root,
                path=f"{path}.component_descriptor",
            )
            try:
                descriptor = load_strict_json_yaml_bytes(
                    (root / descriptor_path).read_bytes(),
                    source=str(root / descriptor_path),
                )
            except (OSError, StrictArtifactError) as error:
                raise SourceDescriptionError(
                    f"{path}.component_descriptor cannot be read: {error}"
                ) from error
            model_id = _identifier(
                _object(descriptor, path=f"{path}.component_descriptor").get("id"),
                path=f"{path}.component_descriptor.id",
            )
            if model_id in model_ids or not isinstance(descriptor.get("lanes"), dict):
                raise SourceDescriptionError(
                    f"{path}.component_descriptor must name one distinct model "
                    "with its lanes"
                )
            model_ids.add(model_id)
        if "tcp_frame_role_ids" in family:
            tcp_path = f"{path}.tcp_frame_role_ids"
            tcps = [
                _identifier(role, path=f"{tcp_path}[{index}]")
                for index, role in enumerate(
                    _array(family["tcp_frame_role_ids"], path=tcp_path)
                )
            ]
            frame_roles = {
                row["frame_role_id"]
                for row in family["semantics"]["frame_role_bindings"]
            }
            if (
                not tcps
                or len(set(tcps)) != len(tcps)
                or not set(tcps) <= frame_roles
            ):
                raise SourceDescriptionError(
                    f"{tcp_path} must name distinct frame role bindings, "
                    "the default first"
                )
        model_source = _exact(
            family["source"],
            required=frozenset(
                {
                    "accepted_package_uri_prefix",
                    "historical_srdf_path",
                    "historical_urdf_path",
                    "resource_root",
                    "srdf_path",
                    "srdf_projection_kind",
                    "urdf_path",
                    "urdf_projection_kind",
                }
            ),
            path=f"{path}.source",
        )
        prefix = _text(
            model_source["accepted_package_uri_prefix"],
            path=f"{path}.source.accepted_package_uri_prefix",
        )
        if not prefix.startswith("package://") or not prefix.endswith("/"):
            raise SourceDescriptionError(
                f"{path}.source.accepted_package_uri_prefix is invalid"
            )
        resource_root = _directory_path(
            model_source["resource_root"],
            root=root,
            path=f"{path}.source.resource_root",
        )
        srdf_path = _source_path(
            model_source["srdf_path"], root=root, path=f"{path}.source.srdf_path"
        )
        historical_srdf_path = _source_path(
            model_source["historical_srdf_path"],
            root=root,
            path=f"{path}.source.historical_srdf_path",
        )
        projection_kind = _text(
            model_source["srdf_projection_kind"],
            path=f"{path}.source.srdf_projection_kind",
        )
        pinned_family = family_id in _EXPECTED_MODEL_FAMILIES
        expected_source = (
            _EXPECTED_MODEL_FAMILIES[family_id]["source"]
            if pinned_family
            else {"srdf_projection_kind": "identity", "urdf_projection_kind": "identity"}
        )
        if projection_kind != expected_source["srdf_projection_kind"]:
            raise SourceDescriptionError(
                f"{path}.source.srdf_projection_kind changes the accepted projection"
            )
        if (projection_kind == "identity") != (srdf_path == historical_srdf_path):
            raise SourceDescriptionError(
                f"{path}.source SRDF projection paths contradict their declared kind"
            )
        urdf_path = _source_path(
            model_source["urdf_path"], root=root, path=f"{path}.source.urdf_path"
        )
        historical_urdf_path = _source_path(
            model_source["historical_urdf_path"],
            root=root,
            path=f"{path}.source.historical_urdf_path",
        )
        urdf_projection_kind = _text(
            model_source["urdf_projection_kind"],
            path=f"{path}.source.urdf_projection_kind",
        )
        if urdf_projection_kind != expected_source["urdf_projection_kind"]:
            raise SourceDescriptionError(
                f"{path}.source.urdf_projection_kind changes the accepted projection"
            )
        if (urdf_projection_kind == "identity") != (urdf_path == historical_urdf_path):
            raise SourceDescriptionError(
                f"{path}.source URDF projection paths contradict their declared kind"
            )
        actual_source = {
            "accepted_package_uri_prefix": prefix,
            "historical_srdf_path": historical_srdf_path,
            "historical_urdf_path": historical_urdf_path,
            "resource_root": resource_root,
            "srdf_path": srdf_path,
            "srdf_projection_kind": projection_kind,
            "urdf_path": urdf_path,
            "urdf_projection_kind": urdf_projection_kind,
        }
        if pinned_family and actual_source != expected_source:
            raise SourceDescriptionError(
                f"{path}.source changes the accepted model-family source closure"
            )
    if (
        family_ids != sorted(family_ids)
        or len(set(family_ids)) != len(family_ids)
        or not set(_EXPECTED_MODEL_FAMILIES) <= set(family_ids)
    ):
        raise SourceDescriptionError(
            "$.model_families must contain the canonical-sorted maintained families"
        )



def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--workspace-root", type=Path, required=True)
    args = parser.parse_args()
    try:
        validate(args.input.resolve(), args.workspace_root.resolve())
    except SourceDescriptionError as error:
        print(f"check_maintained_deployment_sources: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
