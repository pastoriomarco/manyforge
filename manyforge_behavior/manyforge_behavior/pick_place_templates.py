"""Pure expansion of Pick/Place inputs into ordinary nodes."""

from __future__ import annotations

from collections.abc import Mapping
from copy import deepcopy
from typing import Any, Sequence

from manyforge_behavior.program import ProgramValidationError, motion_tcp_selection


class PickPlaceTemplateError(ValueError):
    """The requested template cannot be represented by ordinary nodes."""


_COMMON_REQUIRED = frozenset(
    {
        "target_role",
        "tool_target_role",
        "base_frame_role",
        "carrier_frame_role",
        "selected_object_id",
        "tool_position",
    }
)
_COMMON_OPTIONAL = frozenset(
    {
        "planning_profile",
        "tcp_endpoint",
        "motion_options",
        "tool_max_effort",
        "tool_command_duration_s",
        "tool_completion_timeout_s",
    }
)
_SPECIFIC = {
    "pick": (
        frozenset({"contact_policy_id"}),
        frozenset({"synthesis", "no_feasible_plan_policy"}),
    ),
    "place": (
        frozenset({"place_pose"}),
        frozenset({"approach_offset_m"}),
    ),
}
_STATIC_FIELDS = frozenset(
    {
        "target_role",
        "tool_target_role",
        "base_frame_role",
        "tcp_endpoint",
        "carrier_frame_role",
        "contact_policy_id",
        "planning_profile",
    }
)
_WRAPPER_FIELDS = frozenset(
    {
        "metadata",
        "recovery_policy",
        "operator_stop_policy",
        "execution_groups",
    }
)


def build_pick_place_subtree(
    kind: str,
    *,
    instance_id: str,
    parameters: Mapping[str, Any],
    value_sources: Mapping[str, Mapping[str, Any]] | None,
    result_key: str,
    touch_body_roles: Sequence[str] = (),
    child_prefix: str | None = None,
    wrapper_fields: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    """Return one visible Sequence that uses only maintained ordinary nodes."""

    if kind not in _SPECIFIC:
        raise PickPlaceTemplateError("Pick/Place template kind must be pick or place")
    values = deepcopy(dict(parameters))
    sources = deepcopy(dict(value_sources or {}))
    required_specific, optional_specific = _SPECIFIC[kind]
    permitted = _COMMON_REQUIRED | _COMMON_OPTIONAL | required_specific | optional_specific
    missing = sorted((_COMMON_REQUIRED | required_specific) - (set(values) | set(sources)))
    unknown = sorted((set(values) | set(sources)) - permitted)
    duplicate = sorted(set(values) & set(sources))
    if missing or unknown or duplicate:
        raise PickPlaceTemplateError(
            "Pick/Place template parameters are not exactly representable: "
            f"missing={missing}, unknown={unknown}, duplicate={duplicate}"
        )
    dynamic_static = sorted(set(sources) & _STATIC_FIELDS)
    if dynamic_static:
        raise PickPlaceTemplateError(
            "Deployment-captured Pick/Place selectors cannot be dynamic: "
            f"{dynamic_static}"
        )
    try:
        tcp_selection = motion_tcp_selection(values)
    except ProgramValidationError as exc:
        raise PickPlaceTemplateError(str(exc)) from exc
    if "motion_options" in sources:
        raise PickPlaceTemplateError(
            "dynamic legacy motion_options cannot be split safely across ordinary nodes"
        )
    touch_roles = list(touch_body_roles)
    if kind == "pick" and not touch_roles:
        raise PickPlaceTemplateError("Pick requires at least one touch body role")
    if kind == "place" and touch_roles:
        raise PickPlaceTemplateError("Place does not accept touch body roles")
    if (
        any(type(role) is not str or not role for role in touch_roles)
        or touch_roles != sorted(set(touch_roles))
    ):
        raise PickPlaceTemplateError(
            "touch body roles must be a canonical non-empty string set"
        )
    wrapper_values = deepcopy(dict(wrapper_fields or {}))
    unknown_wrapper_fields = sorted(set(wrapper_values) - _WRAPPER_FIELDS)
    if unknown_wrapper_fields:
        raise PickPlaceTemplateError(
            f"unsupported Sequence wrapper fields: {unknown_wrapper_fields}"
        )
    generated_prefix = child_prefix or instance_id

    def literal(name: str, default: Any = None) -> Any:
        return deepcopy(values[name] if name in values else default)

    planning_profile = literal("planning_profile", "first_feasible")
    motion_options = literal("motion_options", {})
    if type(motion_options) is not dict:
        raise PickPlaceTemplateError("motion_options must be one object")
    common_motion = {
        "target_role": literal("target_role"),
        "base_frame_role": literal("base_frame_role"),
        **deepcopy(tcp_selection),
        "planning_profile": planning_profile,
    }
    for name in (
        "acceleration_scale",
        "max_tcp_linear_speed_mps",
        "joint_position_tol_rad",
        "translation_tol_m",
        "rotation_tol_rad",
        "planning_timeout_s",
        "execution_timeout_s",
        "cache_mode",
    ):
        if name in motion_options:
            common_motion[name] = deepcopy(motion_options[name])

    plan_parameters = {
        name: literal(name)
        for name in (
            "target_role",
            "tool_target_role",
            "base_frame_role",
            "carrier_frame_role",
        )
    }
    plan_parameters.update(deepcopy(tcp_selection))
    plan_parameters.update(
        planning_profile=planning_profile,
        motion_options=deepcopy(motion_options),
        output_key=result_key,
    )
    plan_sources: dict[str, dict[str, Any]] = {}
    planning_inputs = ["selected_object_id", "tool_position"]
    planning_inputs.extend(
        ("synthesis", "no_feasible_plan_policy")
        if kind == "pick"
        else ("place_pose", "approach_offset_m")
    )
    for name in planning_inputs:
        if name in sources:
            plan_sources[name] = deepcopy(sources[name])
        elif name in values:
            plan_parameters[name] = literal(name)
    if kind == "pick":
        plan_parameters["contact_policy_id"] = literal("contact_policy_id")
    plan_sources = dict(sorted(plan_sources.items()))

    def motion_parameters(phase: str) -> dict[str, Any]:
        result = deepcopy(common_motion)
        result["velocity_scale"] = motion_options.get(
            f"{phase}_velocity_scale", 0.35
        )
        return result

    approach_parameters = motion_parameters("approach")
    descend_parameters = motion_parameters("descend")
    retreat_parameters = motion_parameters("retreat")
    if kind == "pick":
        descend_parameters["contact_policy_id"] = literal("contact_policy_id")

    gripper_parameters = {"target_role": literal("tool_target_role")}
    gripper_sources: dict[str, dict[str, Any]] = {
        "position": {
            "kind": "blackboard_field",
            "reference": result_key,
            "field": "tool_position",
        }
    }
    for source_name, destination in (
        ("tool_max_effort", "max_effort"),
        ("tool_command_duration_s", "command_duration_s"),
        ("tool_completion_timeout_s", "completion_timeout_s"),
    ):
        if source_name in sources:
            gripper_sources[destination] = deepcopy(sources[source_name])
        elif source_name in values:
            gripper_parameters[destination] = literal(source_name)
    gripper_sources = dict(sorted(gripper_sources.items()))

    tool_suffix = "close" if kind == "pick" else "open"
    scene_suffix = "attach" if kind == "pick" else "detach"
    children: list[dict[str, Any]] = [
        {
            "node_id": f"planning.plan_{kind}_sequence",
            "instance_id": f"{generated_prefix}.plan",
            "parameters": plan_parameters,
            **({"value_sources": plan_sources} if plan_sources else {}),
            "children": [],
        },
        {
            "node_id": "motion.move_to_pose",
            "instance_id": f"{generated_prefix}.approach",
            "parameters": approach_parameters,
            "value_sources": {
                "goal": {
                    "kind": "blackboard_field",
                    "reference": result_key,
                    "field": "approach_pose",
                }
            },
            "children": [],
        },
        {
            "node_id": "motion.follow_cartesian_path",
            "instance_id": f"{generated_prefix}.descend",
            "parameters": descend_parameters,
            "value_sources": dict(
                sorted(
                    {
                        "path": {
                            "kind": "blackboard_field",
                            "reference": result_key,
                            "field": "descend_path",
                        },
                        **(
                            {
                                "contact_scene_resource_id": {
                                    "kind": "blackboard_field",
                                    "reference": result_key,
                                    "field": "selected_object_id",
                                }
                            }
                            if kind == "pick"
                            else {}
                        ),
                    }.items()
                )
            ),
            "children": [],
        },
        {
            "node_id": "gripper.command_position",
            "instance_id": f"{generated_prefix}.{tool_suffix}",
            "parameters": gripper_parameters,
            "value_sources": gripper_sources,
            "children": [],
        },
        {
            "node_id": (
                "scene.attach_carried_object"
                if kind == "pick"
                else "scene.detach_carried_object"
            ),
            "instance_id": f"{generated_prefix}.{scene_suffix}",
            "parameters": {
                "target_role": literal("tool_target_role"),
                **(
                    {
                        "carrier_frame_role": literal("carrier_frame_role"),
                        "touch_body_roles": deepcopy(touch_roles),
                    }
                    if kind == "pick"
                    else {}
                ),
            },
            "value_sources": {
                "scene_resource_id": {
                    "kind": "blackboard_field",
                    "reference": result_key,
                    "field": "selected_object_id",
                }
            },
            "children": [],
        },
        {
            "node_id": "motion.follow_cartesian_path",
            "instance_id": f"{generated_prefix}.retreat",
            "parameters": retreat_parameters,
            "value_sources": {
                "path": {
                    "kind": "blackboard_field",
                    "reference": result_key,
                    "field": "retreat_path",
                }
            },
            "children": [],
        },
    ]
    wrapper = {
        "node_id": "sequence",
        "instance_id": instance_id,
        "parameters": {},
        "children": children,
    }
    wrapper.update(wrapper_values)
    return wrapper


PICK_PLACE_RESULT_FIELDS = [
    {"field": "approach_pose", "type": "stored_pose"},
    {"field": "descend_path", "type": "cartesian_path"},
    {"field": "pick_pose", "type": "stored_pose"},
    {"field": "retreat_path", "type": "cartesian_path"},
    {"field": "retreat_pose", "type": "stored_pose"},
    {"field": "selected_object_id", "type": "scene_resource"},
    {"field": "tool_position", "type": "coordinate_position"},
]


__all__ = [
    "PICK_PLACE_RESULT_FIELDS",
    "PickPlaceTemplateError",
    "build_pick_place_subtree",
]
