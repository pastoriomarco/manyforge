"""Collision rules: the row shape of a motion node's and a Deployment's
``collision_rules``.

A rule holds every pair made of one member of ``a`` and one member of ``b``
and gives those pairs one value: ``clearance_m``, ``contact`` or ``release``.
The row without selections holds every robot x environment pair and has a
clearance only. Pick and place sequence rows name the ``leg`` they apply to.

A move's row may name a generated tool's ``spring_slider`` instead of ``a``
and the contact numbers (OD-371): ``a`` is then the slider and the objects it
carries, and the stroke and step come from the slider (``sent_collision_rules``
spells the row out with the Program Binding's slider rows).
"""

from __future__ import annotations

from collections.abc import Mapping
import math
import re
from typing import Any, Final

MAX_COLLISION_RULES: Final = 64
MAX_COLLISION_RULE_SELECTIONS: Final = 16
MAX_COLLISION_RULE_ID_BYTES: Final = 128
MAX_COLLISION_CLEARANCE_M: Final = 0.05
# A contact row without end_from_fraction may touch from this fraction of the
# move on (the execution host applies it); 0 means during the whole move.
DEFAULT_CONTACT_END_FROM_FRACTION: Final = 0.8
COLLISION_RULE_LEGS: Final = ("approach", "descend", "retreat")

_ID_RE: Final = re.compile(r"^[a-z][a-z0-9_.-]*$")
_CATALOG_ID_RE: Final = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")

# Selection kind -> its exact keys. A scene_object names its object by
# exactly one of scene_resource_id and blackboard_key; with blackboard_key an
# optional field reads one field of a structured blackboard value. An
# attached_object may name the collision body role that carries the objects.
_CELL_SELECTION_FIELDS: Final[Mapping[str, frozenset[str]]] = {
    "component": frozenset({"kind", "component_id"}),
    "collision_body": frozenset({"kind", "component_id", "collision_body_id"}),
    "link": frozenset({"kind", "component_id", "link"}),
    "attached_object": frozenset({"kind", "component_id"}),
    "scene_objects": frozenset({"kind"}),
}
_SPRING_SLIDER_FIELDS: Final = frozenset({"component_id", "element_id"})
_MOVE_ONLY_SELECTION_KINDS: Final = frozenset({"scene_object", "active_tcp_contact_bodies"})
# An SRDF group of a component's model, by its source name: the execution
# host resolves it for the Deployment's rules.
_MODEL_GROUP_FIELDS: Final = frozenset({"kind", "component_id", "group"})

_VALUE_KEYS: Final = ("clearance_m", "contact", "release")
_RELEASE_BOOLEANS: Final = (
    "enforce_max_start_penetration",
    "require_strictly_decreasing_penetration",
    "require_separation_by_end",
    "allow_recontact",
)
_RELEASE_NUMBERS: Final = (
    "max_start_penetration_m",
    "min_release_progress_m",
    "release_tolerance_m",
)
_RELEASE_KEYS: Final = frozenset(
    {"release_evidence_mode", *_RELEASE_BOOLEANS, *_RELEASE_NUMBERS}
)
# Values of the keys a release leaves out. Lightweight evidence (binary
# contact) measures no depth, so its depth settings are off.
_RELEASE_DEFAULTS: Final[Mapping[str, Mapping[str, Any]]] = {
    "signed_penetration": {
        "enforce_max_start_penetration": True,
        "max_start_penetration_m": 0.0,
        "min_release_progress_m": 0.0,
        "release_tolerance_m": 1e-5,
        "require_strictly_decreasing_penetration": True,
        "require_separation_by_end": True,
        "allow_recontact": False,
    },
    "binary_contact": {
        "enforce_max_start_penetration": False,
        "max_start_penetration_m": 0.0,
        "min_release_progress_m": 0.0,
        "release_tolerance_m": 0.0,
        "require_strictly_decreasing_penetration": False,
        "require_separation_by_end": True,
        "allow_recontact": False,
    },
}


class CollisionRuleError(ValueError):
    """A ``collision_rules`` list does not have the rule shape.

    ``reason`` is the stable code, ``path`` the failing field.
    """

    def __init__(self, reason: str, path: str, message: str) -> None:
        super().__init__(f"{path}: {message}")
        self.reason = reason
        self.path = path
        self.message = message


def _number(value: Any) -> float | None:
    if type(value) not in {int, float}:
        return None
    number = float(value)
    return number if math.isfinite(number) else None


def _identifier(value: Any, pattern: re.Pattern[str]) -> bool:
    return (
        type(value) is str
        and len(value.encode("utf-8")) <= MAX_COLLISION_RULE_ID_BYTES
        and pattern.fullmatch(value) is not None
    )


def _check_selection(selection: Any, *, path: str, move_rules: bool) -> None:
    def refuse(field: str, message: str) -> None:
        raise CollisionRuleError(
            "collision_rule_selection_invalid", f"{path}{field}", message
        )

    if type(selection) is not dict:
        refuse("", "must be a mapping")
    kind = selection.get("kind")
    if kind in _CELL_SELECTION_FIELDS:
        fields = _CELL_SELECTION_FIELDS[kind]
        if kind == "attached_object" and "collision_body_role_id" in selection:
            fields |= {"collision_body_role_id"}
    elif kind == "model_group" and not move_rules:
        fields = _MODEL_GROUP_FIELDS
    elif kind == "model_group":
        refuse(".kind", "model_group is available on the Deployment's rules only")
    elif kind == "active_tcp_contact_bodies" and move_rules:
        fields = frozenset({"kind"})
    elif kind == "scene_object" and move_rules:
        named = {"scene_resource_id", "blackboard_key"} & set(selection)
        if len(named) != 1:
            refuse("", "names its object by exactly one of scene_resource_id and blackboard_key")
        fields = frozenset({"kind", *named})
        if "blackboard_key" in named and "field" in selection:
            fields |= {"field"}
    elif kind in _MOVE_ONLY_SELECTION_KINDS:
        refuse(".kind", f"{kind} is available on a move's rules only")
    else:
        refuse(".kind", "is not a collision rule selection kind")
    if set(selection) != fields:
        refuse("", f"must have exactly the fields {sorted(fields)}")
    for name in sorted(fields - {"kind"}):
        value = selection[name]
        if name in {"link", "group"}:
            valid = (
                type(value) is str
                and 0 < len(value.encode("utf-8")) <= 256
                and "\x00" not in value
            )
        elif name in {"collision_body_id", "collision_body_role_id", "scene_resource_id", "field"}:
            valid = _identifier(value, _CATALOG_ID_RE)
        else:
            valid = _identifier(value, _ID_RE)
        if not valid:
            refuse(f".{name}", "is not a valid identifier")


def _check_value(row: Mapping[str, Any], *, path: str, has_selections: bool) -> None:
    def refuse(field: str, message: str) -> None:
        raise CollisionRuleError("collision_rule_value_invalid", f"{path}{field}", message)

    present = [key for key in _VALUE_KEYS if key in row]
    if len(present) != 1:
        refuse("", "has exactly one of clearance_m, contact and release")
    key = present[0]
    value = row[key]
    if not has_selections and key != "clearance_m":
        refuse(f".{key}", "the row without selections has a clearance only")
    if key == "clearance_m":
        clearance = _number(value)
        if clearance is None or not 0.0 <= clearance <= MAX_COLLISION_CLEARANCE_M:
            refuse(".clearance_m", f"must be a number from 0 to {MAX_COLLISION_CLEARANCE_M}")
        return
    if type(value) is not dict:
        refuse(f".{key}", "must be a mapping")
    if key == "contact":
        unknown = set(value) - {"max_penetration_m", "end_from_fraction", "max_compression_step_m"}
        if unknown:
            refuse(".contact", f"contains unknown fields {sorted(unknown)}")
        if "max_penetration_m" in value:
            depth = _number(value["max_penetration_m"])
            if depth is None or depth < 0.0:
                refuse(".contact.max_penetration_m", "must be a number of at least 0")
        if "end_from_fraction" in value:
            fraction = _number(value["end_from_fraction"])
            if fraction is None or not 0.0 <= fraction <= 1.0:
                refuse(".contact.end_from_fraction", "must be a number from 0 to 1")
        if "max_compression_step_m" in value:
            # A spring (OD-356): the step limit comes with its stroke.
            step = _number(value["max_compression_step_m"])
            if step is None or step <= 0.0:
                refuse(".contact.max_compression_step_m", "must be a number above 0")
            if "max_penetration_m" not in value:
                refuse(".contact.max_compression_step_m", "needs max_penetration_m (the stroke)")
        return
    unknown = set(value) - _RELEASE_KEYS
    if unknown:
        refuse(".release", f"contains unknown fields {sorted(unknown)}")
    evidence = value.get("release_evidence_mode", "signed_penetration")
    if evidence not in _RELEASE_DEFAULTS:
        refuse(
            ".release.release_evidence_mode",
            "must be signed_penetration or binary_contact",
        )
    for name in _RELEASE_BOOLEANS:
        if name in value and type(value[name]) is not bool:
            refuse(f".release.{name}", "must be a boolean")
    for name in _RELEASE_NUMBERS:
        if name in value:
            number = _number(value[name])
            if number is None or number < 0.0:
                refuse(f".release.{name}", "must be a number of at least 0")
    if evidence == "binary_contact" and (
        value.get("enforce_max_start_penetration", False)
        or value.get("require_strictly_decreasing_penetration", False)
        or any(value.get(name, 0.0) != 0.0 for name in _RELEASE_NUMBERS)
    ):
        refuse(".release", "binary_contact forbids signed-distance fields")


def _check_spring_slider_row(row: Mapping[str, Any], *, path: str, move_rules: bool) -> None:
    """A row that switches a generated tool's spring slider on: the slider,
    ``b``, and a ``contact`` that may hold only ``end_from_fraction``."""

    if not move_rules:
        raise CollisionRuleError(
            "collision_rule_selection_invalid",
            f"{path}.spring_slider",
            "spring_slider is available on a move's rules only",
        )
    slider = row["spring_slider"]
    if type(slider) is not dict or set(slider) != _SPRING_SLIDER_FIELDS:
        raise CollisionRuleError(
            "collision_rule_selection_invalid",
            f"{path}.spring_slider",
            f"must have exactly the fields {sorted(_SPRING_SLIDER_FIELDS)}",
        )
    if not _identifier(slider["component_id"], _ID_RE):
        raise CollisionRuleError(
            "collision_rule_selection_invalid",
            f"{path}.spring_slider.component_id",
            "is not a valid identifier",
        )
    if not _identifier(slider["element_id"], _CATALOG_ID_RE):
        raise CollisionRuleError(
            "collision_rule_selection_invalid",
            f"{path}.spring_slider.element_id",
            "is not a valid identifier",
        )
    if "a" in row or "b" not in row:
        raise CollisionRuleError(
            "collision_rule_selection_invalid",
            path,
            "a spring_slider row has b and no a",
        )
    if "clearance_m" in row or "release" in row:
        raise CollisionRuleError(
            "collision_rule_value_invalid",
            path,
            "a spring_slider row has at most a contact with end_from_fraction",
        )
    contact = row.get("contact", {})
    if type(contact) is not dict or set(contact) - {"end_from_fraction"}:
        raise CollisionRuleError(
            "collision_rule_value_invalid",
            f"{path}.contact",
            "a spring_slider row's contact holds end_from_fraction only",
        )
    if "end_from_fraction" in contact:
        fraction = _number(contact["end_from_fraction"])
        if fraction is None or not 0.0 <= fraction <= 1.0:
            raise CollisionRuleError(
                "collision_rule_value_invalid",
                f"{path}.contact.end_from_fraction",
                "must be a number from 0 to 1",
            )


def validate_collision_rules(
    value: Any, *, path: str, move_rules: bool, legs: bool = False
) -> None:
    """Check one ``collision_rules`` list; raises ``CollisionRuleError``.

    ``move_rules`` admits the selections only a move has (``scene_object``,
    ``active_tcp_contact_bodies``) and the ``spring_slider`` row. ``legs`` is set for the pick and place
    sequences: every row then names its ``leg``; elsewhere ``leg`` is refused.
    A Deployment's list (``move_rules`` false) starts with the row without
    selections.
    """

    if type(value) is not list:
        raise CollisionRuleError("collision_rule_value_invalid", path, "must be a list")
    if len(value) > MAX_COLLISION_RULES:
        raise CollisionRuleError(
            "collision_rules_bound_exceeded",
            path,
            f"has more than {MAX_COLLISION_RULES} rows",
        )
    rule_ids: set[str] = set()
    without_selections = False
    for index, row in enumerate(value):
        row_path = f"{path}[{index}]"
        if type(row) is not dict:
            raise CollisionRuleError(
                "collision_rule_value_invalid", row_path, "must be a mapping"
            )
        allowed = {"rule_id", "a", "b", "spring_slider", *_VALUE_KEYS} | (
            {"leg"} if legs else set()
        )
        unknown = set(row) - allowed
        if unknown:
            raise CollisionRuleError(
                "collision_rule_value_invalid",
                row_path,
                f"contains unknown fields {sorted(unknown)}",
            )
        rule_id = row.get("rule_id")
        if (
            type(rule_id) is not str
            or not rule_id
            or len(rule_id.encode("utf-8")) > MAX_COLLISION_RULE_ID_BYTES
            or "\x00" in rule_id
        ):
            raise CollisionRuleError(
                "collision_rule_id_invalid",
                f"{row_path}.rule_id",
                f"must be a non-empty text of at most {MAX_COLLISION_RULE_ID_BYTES} bytes",
            )
        if rule_id in rule_ids:
            raise CollisionRuleError(
                "collision_rule_id_duplicate",
                f"{row_path}.rule_id",
                f"{rule_id!r} is used by another row",
            )
        rule_ids.add(rule_id)
        leg: str | None = None
        if legs:
            leg = row.get("leg")
            if leg not in COLLISION_RULE_LEGS:
                raise CollisionRuleError(
                    "collision_rule_value_invalid",
                    f"{row_path}.leg",
                    "must be approach, descend or retreat",
                )
        spring_slider = "spring_slider" in row
        if spring_slider:
            _check_spring_slider_row(row, path=row_path, move_rules=move_rules)
        has_a, has_b = "a" in row or spring_slider, "b" in row
        if has_a != has_b:
            raise CollisionRuleError(
                "collision_rule_selection_invalid",
                row_path,
                "has both a and b, or neither",
            )
        sides = ("b",) if spring_slider else ("a", "b") if has_a else ()
        for side in sides:
            selections = row[side]
            if (
                type(selections) is not list
                or not 1 <= len(selections) <= MAX_COLLISION_RULE_SELECTIONS
            ):
                raise CollisionRuleError(
                    "collision_rule_selection_invalid",
                    f"{row_path}.{side}",
                    f"must list 1 to {MAX_COLLISION_RULE_SELECTIONS} selections",
                )
            for selection_index, selection in enumerate(selections):
                _check_selection(
                    selection,
                    path=f"{row_path}.{side}[{selection_index}]",
                    move_rules=move_rules,
                )
        if not has_a:
            if without_selections:
                raise CollisionRuleError(
                    "collision_rule_selection_invalid",
                    row_path,
                    "is a second row without selections",
                )
            without_selections = True
            if not move_rules and index != 0:
                raise CollisionRuleError(
                    "collision_rule_selection_invalid",
                    row_path,
                    "the row without selections is the first row",
                )
        if not spring_slider:
            _check_value(row, path=row_path, has_selections=has_a)
    if not move_rules and not without_selections:
        raise CollisionRuleError(
            "collision_rule_selection_invalid",
            path,
            "starts with the row without selections",
        )


def release_with_defaults(release: Mapping[str, Any]) -> dict[str, Any]:
    """One release value with every key spelled out."""

    evidence = release.get("release_evidence_mode", "signed_penetration")
    return {
        "release_evidence_mode": evidence,
        **{
            name: release.get(name, default)
            for name, default in _RELEASE_DEFAULTS[evidence].items()
        },
    }


def sent_collision_rules(
    rules: list[Mapping[str, Any]],
    *,
    leg: str | None = None,
    spring_sliders: Mapping[tuple[str, str], Mapping[str, Any]] | None = None,
) -> list[dict[str, Any]]:
    """The rows a motion sends: ``leg`` removed (with ``leg`` given, only that
    leg's rows), every release spelled out and every ``spring_slider`` row
    spelled out from ``spring_sliders`` (the Program Binding's
    ``collision_rule_spring_sliders`` by component and element id): ``a`` is the
    slider's body and the objects it carries, the contact has the slider's
    stroke, step and axis. The node's input snapshot has already named every
    ``scene_object`` by its Scene resource id."""

    sent: list[dict[str, Any]] = []
    for index, row in enumerate(rules):
        if leg is not None and row.get("leg") != leg:
            continue
        item: dict[str, Any] = {"rule_id": row["rule_id"]}
        if "spring_slider" in row:
            component_id = row["spring_slider"]["component_id"]
            element_id = row["spring_slider"]["element_id"]
            slider = (spring_sliders or {}).get((component_id, element_id))
            if slider is None:
                raise CollisionRuleError(
                    "collision_rule_spring_slider_unresolved",
                    f"collision_rules[{index}].spring_slider",
                    f"{component_id}/{element_id} is no spring slider of a generated tool",
                )
            item["a"] = [
                {"kind": "collision_body", "component_id": component_id,
                 "collision_body_id": element_id},
                {"kind": "attached_object", "component_id": component_id,
                 "collision_body_role_id": element_id},
            ]
            item["b"] = [dict(selection) for selection in row["b"]]
            item["contact"] = {
                **{name: float(value) for name, value in row.get("contact", {}).items()},
                "max_penetration_m": float(slider["stroke_m"]),
                "max_compression_step_m": float(slider["max_compression_step_m"]),
                "spring_group": {
                    "component_id": component_id,
                    "element_id": element_id,
                    "axis": [float(value) for value in slider["axis"]],
                },
            }
            sent.append(item)
            continue
        if "a" in row:
            item["a"] = [dict(selection) for selection in row["a"]]
            item["b"] = [dict(selection) for selection in row["b"]]
        if "clearance_m" in row:
            item["clearance_m"] = float(row["clearance_m"])
        elif "contact" in row:
            item["contact"] = {
                name: float(value) for name, value in row["contact"].items()
            }
        else:
            item["release"] = release_with_defaults(row["release"])
        sent.append(item)
    return sent
