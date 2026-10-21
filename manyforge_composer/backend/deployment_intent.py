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
    # Q28/Q51 (`main`'s per-profile override): the robot's controlled-Stop
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


def _is_io_provider_component(component: dict[str, Any]) -> bool:
    configuration = component.get("configuration")
    return (
        isinstance(configuration, dict)
        and configuration.get("runtime_kind") == IO_PROVIDER_RUNTIME_KIND
    )


def _validate_io_provider_component(
    component: dict[str, Any], label: str
) -> dict[str, Any]:
    """Validate a model-free I/O device envelope (runtime_kind io_provider).

    It has no model source, root pose, frame prefix or Stop fields and empty
    target settings; its runtime configuration stays opaque here (the closed
    io_provider execution schema owns it).
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
        _fail(f"{label}.target_settings must be empty for an I/O device")
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
    optional = {"description", "assistant_profile_id", "compatibility", "isaac_world"}
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

    if "isaac_world" in intent:
        result["isaac_world"] = _require_id(intent["isaac_world"], f"{source}: isaac_world")

    components = intent["components"]
    if type(components) is not list or len(components) > _MAX_COMPONENTS:
        _fail(f"{source}: components must be a bounded list")
    normalized_components: list[dict[str, Any]] = []
    component_ids: set[str] = set()
    for index, raw_component in enumerate(components):
        label = f"{source}: components[{index}]"
        component = _require_object(raw_component, label)
        if _is_io_provider_component(component):
            normalized = _validate_io_provider_component(component, label)
            if normalized["component_id"] in component_ids:
                _fail(f"{source}: duplicate component_id {normalized['component_id']!r}")
            component_ids.add(normalized["component_id"])
            normalized_components.append(normalized)
            continue
        _require_exact_fields(
            component,
            required={
                "component_id",
                "component_profile_id",
                "model_source",
                "root_pose_in_cell",
                "target_settings",
                "configuration",
            },
            optional={"frame_prefix"},
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
        normalized_components.append(
            {
                "component_id": component_id,
                "frame_prefix": frame_prefix,
                "component_profile_id": _require_id(
                    component["component_profile_id"],
                    f"{label}.component_profile_id",
                ),
                "model_source": normalized_source,
                "root_pose_in_cell": _validate_pose(
                    component["root_pose_in_cell"], f"{label}.root_pose_in_cell"
                ),
                "target_settings": _validate_target_settings(
                    component["target_settings"], f"{label}.target_settings"
                ),
                "configuration": deepcopy(
                    _require_object(component["configuration"], f"{label}.configuration")
                ),
            }
        )
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
            continue  # A model-free I/O device references no source.
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
