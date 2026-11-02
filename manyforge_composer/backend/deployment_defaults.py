"""Deployment defaults: the backend is their only owner (CONTRACTS §11).

A defaulted field may be absent from the Deployment document; its effective
value is then the default here. ``GET /api/deployment/effective`` reports each
one as ``{value, default, set}``; writing the field adds it to the Draft and
"Reset to default" removes it. :func:`with_defaults` fills the fields the
compiler requires into the document right before it is compiled for preview or
Apply, so a document without them still compiles; the others are optional in
the compiled schema, where absent means the same default.

Path syntax: the Draft mutation path elements joined by ``.`` (list rows by
index), e.g. ``components.0.configuration.runtime_configuration.execution_tick_hz``
or ``policies.interrupt_rules.2.active_at_start``.
"""

from __future__ import annotations

from copy import deepcopy
from typing import Any, Iterator

# Robot defaults: per robot (a ros2_control component no attachment mounts), generated from
# manyforge_core/resources/robot_defaults.tsv, the one source the C++ consumers
# share: ROBOT_RUNTIME_DEFAULTS are required by the compiler, so they are
# filled before compile; ROBOT_OPTIONAL_DEFAULTS (and, on lane `real`,
# REAL_ROBOT_OPTIONAL_DEFAULTS) are optional in the compiled schema (absent =
# this default).
from .robot_defaults import (
    COMPONENT_DEFAULTS,
    REAL_ROBOT_OPTIONAL_DEFAULTS,
    ROBOT_OPTIONAL_DEFAULTS,
    ROBOT_RUNTIME_DEFAULTS,
)

# COMPONENT_DEFAULTS (same source): per component with its own lifecycle
# (robots, tools, pose sources, AI models, sensors); optional (absent = this
# default, the host's kDefaultLifecycleBudgetNs). An I/O device authors its
# own in `io_provider_devices`; a machine owner and an I/O member have none.
# Per point-cloud source (a `kind: sensor` component, runtime_kind `sensor`):
# the bridge's former defaults. The compiler requires them, so they are
# filled before compile.
SENSOR_POINTCLOUD_DEFAULTS: dict[str, Any] = {
    "max_points": 200000,
    "minimum_period_s": 0.1,
}
# Per global interrupt rule (CONTRACTS §8); optional in the compiled schema.
RULE_DEFAULTS: dict[str, Any] = {"active_at_start": False}
# Per rule with `on_trip: stop` only.
STOP_RULE_DEFAULTS: dict[str, Any] = {"after_stop": "hold"}
# Moving takeover (`policies`): with a total preparation time, an absent
# planning budget is this fraction of it (the kernel derives 4/5 of the total).
TAKEOVER_TOTAL_FIELD = "motion_takeover_preparation_budget_s"
TAKEOVER_PLANNING_FIELD = "motion_takeover_planning_budget_s"
TAKEOVER_PLANNING_FRACTION = 0.8


def _defaulted(
    document: dict[str, Any],
) -> Iterator[tuple[str, dict[str, Any], str, Any, bool]]:
    """Yield (path, owning object, field, default, filled before compile)."""

    mounted = {
        row["child"].get("component_id")
        for row in document.get("attachments") or ()
        if isinstance(row, dict) and isinstance(row.get("child"), dict)
    }
    for index, component in enumerate(document.get("components") or ()):
        if not isinstance(component, dict):
            continue
        configuration = component.get("configuration")
        if _has_own_lifecycle_budget(component):
            for field, default in COMPONENT_DEFAULTS.items():
                yield f"components.{index}.configuration.{field}", configuration, field, default, False
        if not isinstance(configuration, dict):
            continue
        runtime = configuration.get("runtime_configuration")
        if not isinstance(runtime, dict):
            continue
        prefix = f"components.{index}.configuration.runtime_configuration"
        # A sensor keeps its own defaults wherever it is mounted.
        if configuration.get("runtime_kind") == "sensor":
            for field, default in SENSOR_POINTCLOUD_DEFAULTS.items():
                yield f"{prefix}.{field}", runtime, field, default, True
            continue
        # Robot defaults: a mounted ros2_control component is a tool.
        if (configuration.get("runtime_kind") != "ros2_control"
                or component.get("component_id") in mounted):
            continue
        rows = [(ROBOT_RUNTIME_DEFAULTS, True), (ROBOT_OPTIONAL_DEFAULTS, False)]
        if component.get("lane") == "real":
            rows.append((REAL_ROBOT_OPTIONAL_DEFAULTS, False))
        for defaults, filled in rows:
            for field, default in defaults.items():
                yield f"{prefix}.{field}", runtime, field, default, filled
    policies = document.get("policies")
    total = policies.get(TAKEOVER_TOTAL_FIELD) if isinstance(policies, dict) else None
    if isinstance(total, (int, float)) and not isinstance(total, bool):
        yield (
            f"policies.{TAKEOVER_PLANNING_FIELD}",
            policies,
            TAKEOVER_PLANNING_FIELD,
            total * TAKEOVER_PLANNING_FRACTION,
            False,
        )
    rules = policies.get("interrupt_rules") if isinstance(policies, dict) else None
    for index, rule in enumerate(rules or ()):
        if not isinstance(rule, dict):
            continue
        defaults = dict(RULE_DEFAULTS)
        if rule.get("on_trip") == "stop":
            defaults.update(STOP_RULE_DEFAULTS)
        for field, default in defaults.items():
            yield f"policies.interrupt_rules.{index}.{field}", rule, field, default, False


def _has_own_lifecycle_budget(component: dict[str, Any]) -> bool:
    """Whether `configuration.lifecycle_command_timeout_s` belongs to this row:
    every component with `target_settings` (robots, tools, pose sources, AI
    models, sensors), except an I/O device."""

    configuration = component.get("configuration")
    return (
        "target_settings" in component
        and isinstance(configuration, dict)
        and configuration.get("runtime_kind") != "io_provider"
    )


def effective_fields(document: dict[str, Any] | None) -> dict[str, Any]:
    """`{fields: {<path>: {value, default, set}}}` over one Deployment document."""

    fields: dict[str, Any] = {}
    for path, owner, field, default, _filled in _defaulted(document or {}):
        present = field in owner
        fields[path] = {
            "value": deepcopy(owner[field]) if present else default,
            "default": default,
            "set": present,
        }
    return {"fields": fields}


def with_defaults(document: dict[str, Any]) -> dict[str, Any]:
    """A copy of the document with every absent required default filled in."""

    filled = deepcopy(document)
    for _path, owner, field, default, fill in _defaulted(filled):
        if fill:
            owner.setdefault(field, default)
    return filled
