"""Closed authoring contract for one source-referenced Deployment intent."""

from __future__ import annotations

from copy import deepcopy
import math
import os
from pathlib import Path, PurePosixPath
import re
from typing import Any, Iterable, NoReturn

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    canonical_json_bytes,
)

from .stop_fallback_sequence import (
    FALLBACK_STOP_MODES_FIELD,
    fallback_sequence_error,
)


DEPLOYMENT_INTENT_API_VERSION = "manyforge.deployment_intent.v2"
_PORTABLE_ID = re.compile(r"^[a-z][a-z0-9._-]{0,127}$")
_PACKAGE_NAME = re.compile(r"^[a-z][a-z0-9_-]{0,127}$")
_FRAME_PREFIX = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_/]*/")
_MAX_COMPONENTS = 128
_MAX_ATTACHMENTS = 256
_MAX_PACKAGE_ROOTS = 128
_MAX_TARGETS = 128
_MAX_PATH_BYTES = 4096


class DeploymentIntentError(ValueError):
    """A Deployment intent is structurally invalid."""


def _fail(message: str) -> NoReturn:
    raise DeploymentIntentError(message)


def _require_object(value: Any, label: str) -> dict[str, Any]:
    if type(value) is not dict:
        _fail(f"{label} must be an object")
    return value


def _require_exact_fields(
    value: dict[str, Any],
    *,
    required: set[str],
    optional: set[str] = frozenset(),
    label: str,
) -> None:
    actual = set(value)
    missing = required - actual
    extra = actual - required - optional
    if missing:
        _fail(f"{label} is missing {sorted(missing)[0]}")
    if extra:
        _fail(f"{label} has unexpected field {sorted(extra)[0]}")


def _require_id(value: Any, label: str) -> str:
    if type(value) is not str or _PORTABLE_ID.fullmatch(value) is None:
        _fail(f"{label} must be a portable identifier")
    return value


def _require_text(value: Any, label: str, maximum_bytes: int) -> str:
    if (
        type(value) is not str
        or not value.strip()
        or len(value.encode("utf-8")) > maximum_bytes
    ):
        _fail(f"{label} must be bounded non-empty text")
    return value


def _require_frame_prefix(value: Any, label: str) -> str:
    if type(value) is not str:
        _fail(f"{label} must be a string")
    if value and _FRAME_PREFIX.fullmatch(value) is None:
        _fail(
            f"{label} must be empty or use only ASCII letters, digits, '_' and '/', "
            "without a leading '/' and with a trailing '/'"
        )
    return value


def _normalized_project_path(value: Any, label: str) -> str:
    if type(value) is not str or not value or len(value.encode("utf-8")) > _MAX_PATH_BYTES:
        _fail(f"{label} must be one bounded project-relative path")
    if "\\" in value or value.startswith("/"):
        _fail(f"{label} must use project-relative POSIX syntax")
    path = PurePosixPath(value)
    if str(path) != value or value in {".", ".."}:
        _fail(f"{label} must be normalized")
    if any(part in {"", "."} for part in path.parts):
        _fail(f"{label} must be normalized")
    return value


def _normalized_external_path(value: Any, label: str) -> str:
    if type(value) is not str or not value or len(value.encode("utf-8")) > _MAX_PATH_BYTES:
        _fail(f"{label} must be one bounded absolute path")
    path = Path(value)
    if not path.is_absolute() or os.path.normpath(value) != value:
        _fail(f"{label} must be a normalized absolute path")
    return value


def validate_source_reference(value: Any, *, label: str) -> dict[str, str]:
    reference = _require_object(value, label)
    if set(reference) == {"project_path"}:
        return {
            "project_path": _normalized_project_path(
                reference["project_path"], f"{label}.project_path"
            )
        }
    if set(reference) == {"external_path"}:
        return {
            "external_path": _normalized_external_path(
                reference["external_path"], f"{label}.external_path"
            )
        }
    _fail(f"{label} must contain exactly project_path or external_path")


def _validate_pose(value: Any, label: str) -> dict[str, list[float]]:
    pose = _require_object(value, label)
    _require_exact_fields(
        pose,
        required={"position_m", "orientation_xyzw"},
        label=label,
    )
    result: dict[str, list[float]] = {}
    for field, length in (("position_m", 3), ("orientation_xyzw", 4)):
        coordinates = pose[field]
        if (
            type(coordinates) is not list
            or len(coordinates) != length
            or any(
                type(item) not in {int, float} or not math.isfinite(float(item))
                for item in coordinates
            )
        ):
            _fail(f"{label}.{field} must contain {length} finite numbers")
        result[field] = [float(item) for item in coordinates]
    if math.sqrt(sum(item * item for item in result["orientation_xyzw"])) <= 1e-12:
        _fail(f"{label}.orientation_xyzw must be non-zero")
    return result


_MAX_CUSTOM_TCPS = 256


def _finite_numbers(value: Any, length: int, label: str) -> list[float]:
    if (
        type(value) is not list
        or len(value) != length
        or any(type(item) not in {int, float} or not math.isfinite(float(item)) for item in value)
    ):
        _fail(f"custom_tcp_invalid: {label} must contain {length} finite numbers")
    return [float(item) for item in value]


def _validate_authoring_pose(value: Any, label: str) -> dict[str, Any]:
    """`{position_m, orientation: {rpy_deg} | {xyzw}}`; xyzw must be unit."""
    pose = _require_object(value, label)
    _require_exact_fields(pose, required={"position_m", "orientation"}, label=label)
    orientation = _require_object(pose["orientation"], f"{label}.orientation")
    key = "rpy_deg" if "rpy_deg" in orientation else "xyzw"
    _require_exact_fields(orientation, required={key}, label=f"{label}.orientation")
    values = _finite_numbers(
        orientation[key], 3 if key == "rpy_deg" else 4, f"{label}.orientation.{key}"
    )
    if key == "xyzw" and abs(sum(item * item for item in values) - 1.0) > 1e-6:
        _fail(f"custom_tcp_invalid: {label}.orientation.xyzw must be a unit quaternion")
    return {
        "position_m": _finite_numbers(pose["position_m"], 3, f"{label}.position_m"),
        "orientation": {key: values},
    }


def _validate_custom_tcps(value: Any, label: str) -> list[dict[str, Any]]:
    """`custom_tcps` rows `{tcp_id, parent_frame, pose}`, sorted by tcp_id.

    `parent_frame` names a frame of the component's model (a descriptor frame
    role or a URDF link); the compiler resolves it and refuses chaining and
    name collisions. The pose is the authoring pose shape.
    """
    if type(value) is not list or len(value) > _MAX_CUSTOM_TCPS:
        _fail(f"{label} must be a bounded list")
    rows: list[dict[str, Any]] = []
    for index, raw in enumerate(value):
        row_label = f"{label}[{index}]"
        row = _require_object(raw, row_label)
        _require_exact_fields(
            row, required={"tcp_id", "parent_frame", "pose"}, label=row_label
        )
        pose = _validate_authoring_pose(row["pose"], f"{row_label}.pose")
        rows.append(
            {
                "tcp_id": _require_id(row["tcp_id"], f"{row_label}.tcp_id"),
                "parent_frame": _require_text(
                    row["parent_frame"], f"{row_label}.parent_frame", 1024
                ),
                "pose": pose,
            }
        )
    rows.sort(key=lambda row: row["tcp_id"])
    if len({row["tcp_id"] for row in rows}) != len(rows):
        _fail(f"custom_tcp_duplicate: {label} repeats a tcp_id")
    return rows


def _validate_target_settings(value: Any, label: str) -> dict[str, dict[str, Any]]:
    settings = _require_object(value, label)
    if len(settings) > _MAX_TARGETS:
        _fail(f"{label} exceeds the target bound")
    result: dict[str, dict[str, Any]] = {}
    for target_id, selected in settings.items():
        _require_id(target_id, f"{label} key")
        row = deepcopy(_require_object(selected, f"{label}.{target_id}"))
        speed = row.get("maximum_tcp_linear_speed_mps")
        if speed is not None and (
            type(speed) not in {int, float}
            or not math.isfinite(float(speed))
            or float(speed) <= 0.0
        ):
            _fail(
                f"{label}.{target_id}.maximum_tcp_linear_speed_mps "
                "must be positive and finite"
            )
        if speed is not None:
            row["maximum_tcp_linear_speed_mps"] = float(speed)
        if "controlled_stop" in row:
            row["controlled_stop"] = _validate_controlled_stop(
                row["controlled_stop"], f"{label}.{target_id}.controlled_stop"
            )
        result[target_id] = row
    return dict(sorted(result.items()))


def _positive_finite(value: Any, label: str, *, maximum: float | None = None) -> float:
    if (
        type(value) not in {int, float}
        or not math.isfinite(float(value))
        or float(value) <= 0.0
        or (maximum is not None and float(value) > maximum)
    ):
        suffix = "" if maximum is None else f" and no greater than {maximum:g}"
        _fail(f"{label} must be positive and finite{suffix}")
    return float(value)


def _nonnegative_finite(value: Any, label: str) -> float:
    if (
        type(value) not in {int, float}
        or not math.isfinite(float(value))
        or float(value) < 0.0
    ):
        _fail(f"{label} must be nonnegative and finite")
    return float(value)


def _validate_controlled_stop(value: Any, label: str) -> dict[str, Any]:
    """Normalize one target-level authored Stop choice.

    Source-joint names are authoring identities.  Generated resource and control
    binding identities deliberately do not cross this boundary; preparation
    expands this one profile over the exact compiled routes.
    """

    profile = _require_object(value, label)
    _require_exact_fields(
        profile,
        required={
            "stop_mode",
            "allow_natural_completion_on_short_path",
            "jerk_limits_enabled",
            "deceleration_time_s",
            "per_joint_limits",
        },
        optional={"max_stop_time_s", FALLBACK_STOP_MODES_FIELD},
        label=label,
    )
    mode = profile["stop_mode"]
    if mode not in {"soft_on_path", "spring_back_hold", "spring_back_fast"}:
        _fail(f"{label}.stop_mode is unsupported")
    natural = profile["allow_natural_completion_on_short_path"]
    jerk_enabled = profile["jerk_limits_enabled"]
    if type(natural) is not bool:
        _fail(f"{label}.allow_natural_completion_on_short_path must be boolean")
    if type(jerk_enabled) is not bool:
        _fail(f"{label}.jerk_limits_enabled must be boolean")
    limits = profile["per_joint_limits"]
    if type(limits) is not list or not 1 <= len(limits) <= 256:
        _fail(f"{label}.per_joint_limits must be a bounded non-empty list")
    normalized_limits: list[dict[str, Any]] = []
    seen: set[str] = set()
    for index, raw_limit in enumerate(limits):
        limit_label = f"{label}.per_joint_limits[{index}]"
        limit = _require_object(raw_limit, limit_label)
        _require_exact_fields(
            limit,
            required={
                "source_joint_name",
                "max_deceleration_rad_s2",
                "settle_velocity_threshold_rad_s",
            },
            optional={"max_jerk_rad_s3"},
            label=limit_label,
        )
        joint = _require_text(
            limit["source_joint_name"], f"{limit_label}.source_joint_name", 256
        )
        if joint in seen:
            _fail(f"{label} contains duplicate source joint {joint!r}")
        seen.add(joint)
        normalized_limit: dict[str, Any] = {
            "source_joint_name": joint,
            "max_deceleration_rad_s2": _positive_finite(
                limit["max_deceleration_rad_s2"],
                f"{limit_label}.max_deceleration_rad_s2",
            ),
            "settle_velocity_threshold_rad_s": _nonnegative_finite(
                limit["settle_velocity_threshold_rad_s"],
                f"{limit_label}.settle_velocity_threshold_rad_s",
            ),
        }
        if "max_jerk_rad_s3" in limit:
            normalized_limit["max_jerk_rad_s3"] = _positive_finite(
                limit["max_jerk_rad_s3"], f"{limit_label}.max_jerk_rad_s3"
            )
        normalized_limits.append(normalized_limit)
    # Keep authored order.  The Deployment compiler pairs each row with the
    # arm resource of its source joint by name and refuses rows that miss,
    # repeat, or name a joint the model's arm does not have.
    result: dict[str, Any] = {
        "stop_mode": mode,
        "allow_natural_completion_on_short_path": natural,
        "jerk_limits_enabled": jerk_enabled,
        "deceleration_time_s": _positive_finite(
            profile["deceleration_time_s"],
            f"{label}.deceleration_time_s",
            maximum=2.0,
        ),
        "per_joint_limits": normalized_limits,
    }
    # Per-profile override: the robot's controlled-Stop
    # deadline from the Stop request; absent: deceleration time + 0.5 s.
    if "max_stop_time_s" in profile:
        result["max_stop_time_s"] = _positive_finite(
            profile["max_stop_time_s"], f"{label}.max_stop_time_s"
        )
    # Ordered fallbacks after the authored mode (stop_fallback_sequence.py).
    # Absent and [] are equivalent (primary, then hard); [] is not emitted.
    if FALLBACK_STOP_MODES_FIELD in profile:
        fallbacks = profile[FALLBACK_STOP_MODES_FIELD]
        error = fallback_sequence_error(mode, fallbacks)
        if error is not None:
            _fail(f"{label}.{FALLBACK_STOP_MODES_FIELD} {error}")
        if fallbacks:
            result[FALLBACK_STOP_MODES_FIELD] = list(fallbacks)
    return result


IO_DEVICE_PROFILE_ID = "io_device_v0"
IO_PROVIDER_RUNTIME_KIND = "io_provider"
POSE_SOURCE_PROFILE_ID = "pose_source_v0"
POSE_SOURCE_RUNTIME_KIND = "pose_source"
AI_MODEL_RUNTIME_KIND = "ai_model"
# Runtime kinds of model-free components (I/O device, pose source, AI model).
MODEL_FREE_RUNTIME_KINDS = frozenset(
    {IO_PROVIDER_RUNTIME_KIND, POSE_SOURCE_RUNTIME_KIND, AI_MODEL_RUNTIME_KIND}
)


def _is_model_free_component(component: dict[str, Any]) -> bool:
    configuration = component.get("configuration")
    return (
        isinstance(configuration, dict)
        and configuration.get("runtime_kind") in MODEL_FREE_RUNTIME_KINDS
    )


def _validate_model_free_component(
    component: dict[str, Any], label: str
) -> dict[str, Any]:
    """Validate a model-free component envelope (runtime_kind io_provider,
    pose_source or ai_model).

    It has no model source, root pose, frame prefix or Stop fields and empty
    target settings; its runtime configuration stays opaque here (the closed
    runtime-kind schema checked by the activation compiler owns it).
    """

    _require_exact_fields(
        component,
        required={
            "component_id",
            "component_profile_id",
            "target_settings",
            "configuration",
        },
        label=label,
    )
    if component["target_settings"] != {}:
        _fail(f"{label}.target_settings must be empty for a model-free component")
    return {
        "component_id": _require_id(component["component_id"], f"{label}.component_id"),
        "component_profile_id": _require_id(
            component["component_profile_id"], f"{label}.component_profile_id"
        ),
        "target_settings": {},
        "configuration": deepcopy(
            _require_object(component["configuration"], f"{label}.configuration")
        ),
    }


SENSOR_RUNTIME_KIND = "sensor"


def _is_sensor_component(component: dict[str, Any]) -> bool:
    configuration = component.get("configuration")
    return (
        isinstance(configuration, dict)
        and configuration.get("runtime_kind") == SENSOR_RUNTIME_KIND
    )


def _validate_sensor_component(component: dict[str, Any], label: str) -> dict[str, Any]:
    """A sensor component (`kind: sensor` model, e.g. the point-cloud
    source): its model (no lane), empty target settings and an optional
    lifecycle time; its runtime configuration (topic, source frame, limits)
    is checked by the compiler."""

    _require_exact_fields(
        component,
        required={
            "component_id",
            "component_profile_id",
            "target_settings",
            "configuration",
        },
        label=label,
    )
    if component["target_settings"] != {}:
        _fail(f"{label}.target_settings must be empty for a sensor")
    configuration = _require_object(component["configuration"], f"{label}.configuration")
    _require_exact_fields(
        configuration,
        required={"runtime_kind", "runtime_configuration"},
        optional={"lifecycle_command_timeout_s"},
        label=f"{label}.configuration",
    )
    _require_object(
        configuration["runtime_configuration"], f"{label}.configuration.runtime_configuration"
    )
    return {
        "component_id": _require_id(component["component_id"], f"{label}.component_id"),
        "component_profile_id": _require_id(
            component["component_profile_id"], f"{label}.component_profile_id"
        ),
        "target_settings": {},
        "configuration": deepcopy(configuration),
    }


def _is_machine_owner(component: dict[str, Any]) -> bool:
    """A machine's owner row: no owner, no model source and no
    runtime kind; it holds the machine's lane and address reference."""
    configuration = component.get("configuration")
    return (
        "owner" not in component
        and "model_source" not in component
        and not (isinstance(configuration, dict) and "runtime_kind" in configuration)
    )


def _validate_active(component: dict[str, Any], label: str) -> dict[str, Any]:
    """`active` is a boolean; only `false` is kept (the default is active).
    Inactive rows stay in the file; the compiler leaves them out."""
    if "active" not in component:
        return {}
    if type(component["active"]) is not bool:
        _fail(f"{label}.active must be a boolean")
    return {} if component["active"] else {"active": False}


def _validate_machine_owner(component: dict[str, Any], label: str) -> dict[str, Any]:
    _require_exact_fields(
        component,
        required={"component_id", "component_profile_id", "lane", "configuration"},
        optional={"active"},
        label=label,
    )
    configuration = _require_object(component["configuration"], f"{label}.configuration")
    _require_exact_fields(
        configuration, required=set(), optional={"device_address_ref"},
        label=f"{label}.configuration",
    )
    normalized_configuration: dict[str, Any] = {}
    if "device_address_ref" in configuration:
        normalized_configuration["device_address_ref"] = _require_id(
            configuration["device_address_ref"], f"{label}.configuration.device_address_ref"
        )
    return {
        "component_id": _require_id(component["component_id"], f"{label}.component_id"),
        "component_profile_id": _require_id(
            component["component_profile_id"], f"{label}.component_profile_id"
        ),
        "lane": _require_id(component["lane"], f"{label}.lane"),
        "configuration": normalized_configuration,
        **_validate_active(component, label),
    }


def _validate_io_member(component: dict[str, Any], label: str) -> dict[str, Any]:
    """A machine's I/O member: its signals come from its model."""
    _require_exact_fields(
        component,
        required={"component_id", "component_profile_id", "owner", "role"},
        optional={"active"},
        label=label,
    )
    return {
        "component_id": _require_id(component["component_id"], f"{label}.component_id"),
        "component_profile_id": _require_id(
            component["component_profile_id"], f"{label}.component_profile_id"
        ),
        "owner": _require_id(component["owner"], f"{label}.owner"),
        "role": _require_id(component["role"], f"{label}.role"),
        **_validate_active(component, label),
    }


def _validate_default_tcp(value: Any, label: str) -> dict[str, str]:
    """`default_tcp` `{command_target_id, frame_role_id}`; the compiler
    checks the pair against the component's TCPs."""
    row = _require_object(value, label)
    _require_exact_fields(
        row, required={"command_target_id", "frame_role_id"}, label=label
    )
    return {
        "command_target_id": _require_id(
            row["command_target_id"], f"{label}.command_target_id"
        ),
        "frame_role_id": _require_id(row["frame_role_id"], f"{label}.frame_role_id"),
    }


def validate_deployment_intent(value: Any, *, source: str) -> dict[str, Any]:
    """Validate and normalize the profile-independent authored envelope.

    Component-profile configuration, target-setting extensions, attachments and
    policy values are deliberately retained as closed-profile inputs. Their
    selected profile owns the second validation stage during preparation.
    """

    intent = _require_object(value, f"{source}: Deployment intent")
    required = {
        "api_version",
        "deployment_artifact_id",
        "name",
        "components",
        "attachments",
        "policies",
    }
    optional = {
        "description",
        "assistant_profile_id",
        "assistant_model_components",
        "compatibility",
    }
    _require_exact_fields(
        intent, required=required, optional=optional, label=f"{source}: Deployment intent"
    )
    if intent["api_version"] != DEPLOYMENT_INTENT_API_VERSION:
        _fail(f"{source}: unsupported Deployment intent version")
    result: dict[str, Any] = {
        "api_version": DEPLOYMENT_INTENT_API_VERSION,
        "deployment_artifact_id": _require_id(
            intent["deployment_artifact_id"], f"{source}: deployment_artifact_id"
        ),
        "name": _require_text(intent["name"], f"{source}: name", 256),
    }
    if "description" in intent:
        if type(intent["description"]) is not str or len(intent["description"].encode("utf-8")) > 4096:
            _fail(f"{source}: description is invalid")
        result["description"] = intent["description"]

    components = intent["components"]
    if type(components) is not list or len(components) > _MAX_COMPONENTS:
        _fail(f"{source}: components must be a bounded list")
    normalized_components: list[dict[str, Any]] = []
    component_ids: set[str] = set()
    # A child mounted through an attachment row takes its placement from the
    # parent interface and omits root_pose_in_cell. Preparation owns the row.
    mounted: set[Any] = set()
    if type(intent["attachments"]) is list:
        for row in intent["attachments"]:
            child = row.get("child") if type(row) is dict else None
            if type(child) is dict:
                mounted.add(child.get("component_id"))
    for index, raw_component in enumerate(components):
        label = f"{source}: components[{index}]"
        component = _require_object(raw_component, label)
        model_free = _is_model_free_component(component)
        sensor = _is_sensor_component(component)
        if model_free or sensor or _is_machine_owner(component) or (
            "owner" in component and "model_source" not in component
        ):
            if sensor:
                normalized = _validate_sensor_component(component, label)
            elif model_free:
                normalized = _validate_model_free_component(component, label)
            elif "owner" in component:
                normalized = _validate_io_member(component, label)
            else:
                normalized = _validate_machine_owner(component, label)
            if normalized["component_id"] in component_ids:
                _fail(f"{source}: duplicate component_id {normalized['component_id']!r}")
            component_ids.add(normalized["component_id"])
            normalized_components.append(normalized)
            continue
        is_mounted = component.get("component_id") in mounted
        # A machine's arm member: no lane of its own (it runs on its owner's).
        member = "owner" in component
        _require_exact_fields(
            component,
            required={
                "component_id",
                "component_profile_id",
                "model_source",
                "target_settings",
                "configuration",
            }
            | (set() if is_mounted else {"root_pose_in_cell"})
            | ({"owner", "role"} if member else set()),
            optional={"frame_prefix", "custom_tcps", "default_tcp"}
            | ({"active"} if member else {"lane"}),
            label=label,
        )
        component_id = _require_id(component["component_id"], f"{label}.component_id")
        if component_id in component_ids:
            _fail(f"{source}: duplicate component_id {component_id!r}")
        component_ids.add(component_id)
        model_source = _require_object(component["model_source"], f"{label}.model_source")
        _require_exact_fields(
            model_source,
            required={"urdf", "package_roots"},
            optional={"srdf"},
            label=f"{label}.model_source",
        )
        package_roots = _require_object(
            model_source["package_roots"], f"{label}.model_source.package_roots"
        )
        if len(package_roots) > _MAX_PACKAGE_ROOTS:
            _fail(f"{label}.model_source.package_roots exceeds the bound")
        normalized_roots: dict[str, dict[str, str]] = {}
        for package_name, reference in package_roots.items():
            if type(package_name) is not str or _PACKAGE_NAME.fullmatch(package_name) is None:
                _fail(f"{label}.model_source.package_roots has an invalid package name")
            normalized_roots[package_name] = validate_source_reference(
                reference,
                label=f"{label}.model_source.package_roots.{package_name}",
            )
        normalized_source: dict[str, Any] = {
            "urdf": validate_source_reference(
                model_source["urdf"], label=f"{label}.model_source.urdf"
            ),
            "package_roots": dict(sorted(normalized_roots.items())),
        }
        if "srdf" in model_source:
            normalized_source["srdf"] = validate_source_reference(
                model_source["srdf"], label=f"{label}.model_source.srdf"
            )
        frame_prefix = _require_frame_prefix(
            component.get("frame_prefix", ""), f"{label}.frame_prefix"
        )
        normalized: dict[str, Any] = {
            "component_id": component_id,
            "frame_prefix": frame_prefix,
            "component_profile_id": _require_id(
                component["component_profile_id"],
                f"{label}.component_profile_id",
            ),
            "model_source": normalized_source,
            "target_settings": _validate_target_settings(
                component["target_settings"], f"{label}.target_settings"
            ),
            "configuration": deepcopy(
                _require_object(component["configuration"], f"{label}.configuration")
            ),
        }
        if "lane" in component:
            # One of the model's `lanes.<lane>` rows; the compiler checks it.
            normalized["lane"] = _require_id(component["lane"], f"{label}.lane")
        if member:
            normalized["owner"] = _require_id(component["owner"], f"{label}.owner")
            normalized["role"] = _require_id(component["role"], f"{label}.role")
            normalized.update(_validate_active(component, label))
        if not is_mounted:
            normalized["root_pose_in_cell"] = _validate_pose(
                component["root_pose_in_cell"], f"{label}.root_pose_in_cell"
            )
        if "custom_tcps" in component:
            normalized["custom_tcps"] = _validate_custom_tcps(
                component["custom_tcps"], f"{label}.custom_tcps"
            )
        if "default_tcp" in component:
            normalized["default_tcp"] = _validate_default_tcp(
                component["default_tcp"], f"{label}.default_tcp"
            )
        normalized_components.append(normalized)
    normalized_components.sort(key=lambda row: row["component_id"])
    result["components"] = normalized_components

    attachments = intent["attachments"]
    if type(attachments) is not list or len(attachments) > _MAX_ATTACHMENTS:
        _fail(f"{source}: attachments must be a bounded list")
    if any(type(row) is not dict for row in attachments):
        _fail(f"{source}: every attachment must be an object")
    result["attachments"] = deepcopy(attachments)
    result["policies"] = deepcopy(
        _require_object(intent["policies"], f"{source}: policies")
    )
    if "assistant_profile_id" in intent:
        assistant = intent["assistant_profile_id"]
        if assistant is not None:
            assistant = _require_id(assistant, f"{source}: assistant_profile_id")
        result["assistant_profile_id"] = assistant
    if "assistant_model_components" in intent:
        # Profile model alias -> ai_model component id; the compiler resolves it.
        mapping = _require_object(
            intent["assistant_model_components"], f"{source}: assistant_model_components"
        )
        result["assistant_model_components"] = {
            _require_id(alias, f"{source}: assistant_model_components key"): _require_id(
                component_id, f"{source}: assistant_model_components.{alias}"
            )
            for alias, component_id in sorted(mapping.items())
        }
    if "compatibility" in intent:
        result["compatibility"] = deepcopy(
            _require_object(intent["compatibility"], f"{source}: compatibility")
        )
    try:
        canonical_json_bytes(result)
    except StrictArtifactError as error:
        _fail(f"{source}: {error}")
    return result


def iter_source_references(
    intent: dict[str, Any],
) -> Iterable[tuple[tuple[str | int, ...], dict[str, str], bool]]:
    """Yield (document path, reference, expects_directory) in stable order."""

    normalized = validate_deployment_intent(intent, source="<Deployment intent>")
    for component_index, component in enumerate(normalized["components"]):
        if "model_source" not in component:
            continue  # A model-free component references no source.
        base: tuple[str | int, ...] = (
            "components",
            component_index,
            "model_source",
        )
        model_source = component["model_source"]
        yield base + ("urdf",), deepcopy(model_source["urdf"]), False
        if "srdf" in model_source:
            yield base + ("srdf",), deepcopy(model_source["srdf"]), False
        for package_name, reference in model_source["package_roots"].items():
            yield (
                base + ("package_roots", package_name),
                deepcopy(reference),
                True,
            )


def resolve_source_reference(reference: dict[str, str], *, project_root: Path) -> Path:
    """Resolve a validated reference without asserting existence or authority."""

    normalized = validate_source_reference(reference, label="source reference")
    if "project_path" in normalized:
        return Path(os.path.abspath(project_root / normalized["project_path"]))
    return Path(normalized["external_path"])


def rebase_deployment_intent_sources(
    intent: dict[str, Any],
    *,
    source_project_root: Path,
    destination_project_root: Path,
) -> dict[str, Any]:
    """Preserve every referenced target while changing the project location."""

    normalized = validate_deployment_intent(intent, source="<Deployment intent>")
    result = deepcopy(normalized)
    for path, reference, _expects_directory in iter_source_references(normalized):
        if "project_path" not in reference:
            continue
        target = resolve_source_reference(reference, project_root=source_project_root)
        relative = os.path.relpath(target, start=destination_project_root)
        replacement = {"project_path": Path(relative).as_posix()}
        current: Any = result
        for segment in path[:-1]:
            current = current[segment]
        current[path[-1]] = validate_source_reference(
            replacement, label="rebased source reference"
        )
    return result
