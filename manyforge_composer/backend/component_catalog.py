"""Read-only catalog of the components a Deployment Draft can add.

Add component composes from components that already exist: each complete
component row of the curated example Scenarios shipped with ManyForge
(``examples/*.scenario/deployment.yaml``). A row that names another component
of its Scenario (a pneumatic gripper's I/O device) is offered together with
it. Model-source paths are rebased to the current Deployment Draft's project
folder (absolute when the Draft has none). Tags come from the component-type
catalog of the row's model family, for Program role compatibility; a family
tagged ``tool`` is a tool, mounted on a robot's ``flange`` by an attachment row.

Machines come from their models instead (the description's
``component_models``): one ``kind: machine`` entry per machine model, snake_case,
after the others, with ready owner and member rows;
adding one posts the owner row plus one member row per member. Example rows of
a machine (owner or member), and rows whose assembly reaches one, are not
offered on their own.

Lane-free adapter components, sensors (``kind: sensor``, e.g. the point-cloud
source) and the AI assistant (``kind: assistant``), also come from their models
in ``component_models``: one ordinary entry of the model's kind per model with
a ready row under the model id's last segment (without a sensor's ``topic`` and
``source_frame_id`` or the assistant's model and profile, which the Add panel
asks for); they have no lane, so their profile facts' ``lanes`` are empty.

Passive tools (a ``kind: tool`` model without a ``gripper`` section, e.g. the
generic ``manyforge.passive_tool``, OD-336) come from their models too: one
``tool`` entry per model with a ready row under the model family id, whose
model source is the model's own files; a project points it at its own URDF,
meshes and TCP. The Tool made from parameters (``manyforge.tool``, OD-357) is
one of them: its ready row holds its starter parameters inline (the model
folder's ``new_tool.tool.yaml``), edited on its component page. They have no
lane and are mounted like every tool.

Owner-less model-form I/O devices (an ``io_device`` with ``lanes`` that no
machine names as a member, e.g. a PLC's bank) come from their models too: one
``io_device`` entry per model with a ready row under the model id's last
segment on the Mock lane. They run on every lane (Mock and Isaac: the host's
Mock I/O), so their profile facts' ``lanes`` are Mock, Isaac and the model's.

Nothing here compiles or changes a Draft: the Add component Draft route adds
the rows (and a tool's attachment row), and the Apply compiler checks them.
"""

from __future__ import annotations

from copy import deepcopy
from functools import cache
import json
import logging
from pathlib import Path
from typing import Any

import yaml

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    canonical_json_bytes,
    load_strict_json_yaml_bytes,
)

from .deployment_defaults import default_collision_rules
from .deployment_intent import (
    DEPLOYMENT_INTENT_API_VERSION,
    DeploymentIntentError,
    iter_source_references,
    rebase_deployment_intent_sources,
    resolve_source_reference,
    validate_deployment_intent,
)

_LOGGER = logging.getLogger(__name__)
_REPOSITORY = Path(__file__).resolve().parents[2]
# The Tool made from parameters (OD-357) and the parameters it starts from.
GENERATED_TOOL_PROFILE_ID = "manyforge.tool"
GENERATED_TOOL_STARTER = "new_tool.tool.yaml"
_EXAMPLES = _REPOSITORY / "examples"
_MAINTAINED = _REPOSITORY / "manyforge_deployment/resources/maintained"
_SOURCES = _MAINTAINED / "maintained_deployment_sources.v1.json"
_COMPONENT_TYPES = _MAINTAINED / "catalog/component_type"

# runtime_kind -> catalog kind; every other row carries a model: a tool when
# its component type is tagged ``tool``, otherwise a robot.
_RUNTIME_KINDS = {
    "io_provider": "io_device",
    "pose_source": "pose_source",
    "ai_model": "ai_model",
    "sensor": "sensor",
    "assistant": "assistant",
}
_KIND_ORDER = (
    "robot", "tool", "io_device", "pose_source", "ai_model", "sensor", "assistant",
)
# Model kinds of lane-free adapter components; each is its runtime_kind.
_LANE_FREE_KINDS = ("sensor", "assistant")


def _kind(row: dict[str, Any], tags: list[str]) -> str:
    configuration = row.get("configuration")
    runtime_kind = (
        configuration.get("runtime_kind") if isinstance(configuration, dict) else None
    )
    if runtime_kind in _RUNTIME_KINDS:
        return _RUNTIME_KINDS[runtime_kind]
    return "tool" if "tool" in tags else "robot"


def _referenced_components(value: Any) -> set[str]:
    """Every ``controllable_component_id`` value inside one row."""

    if isinstance(value, list):
        return set().union(*map(_referenced_components, value))
    if not isinstance(value, dict):
        return set()
    found = set().union(*map(_referenced_components, value.values()))
    if isinstance(value.get("controllable_component_id"), str):
        found.add(value["controllable_component_id"])
    return found


def _assembly(component_id: str, rows: dict[str, dict[str, Any]]) -> list[str]:
    """The component and every component of its Scenario it names, transitively."""

    members: list[str] = []
    pending = [component_id]
    while pending:
        current = pending.pop()
        if current in members:
            continue
        members.append(current)
        pending.extend(
            sorted(_referenced_components(rows[current]) & rows.keys())
        )
    return sorted(members)


def _machine_rows(rows: dict[str, dict[str, Any]]) -> set[str]:
    """The ids of a Scenario's machine rows: members (`owner`) and their owners."""

    members = {cid for cid, row in rows.items() if "owner" in row}
    return members | {rows[cid]["owner"] for cid in members}


def _descriptor(path: str) -> dict[str, Any]:
    return yaml.safe_load((_REPOSITORY / path).read_text(encoding="utf-8"))


@cache
def tool_profile_ids() -> frozenset[str]:
    """The component models that are tools (``kind: tool``): the only ones a
    Deployment Draft may hold unmounted (OD-366)."""

    sources = json.loads(_SOURCES.read_text(encoding="utf-8"))
    return frozenset(
        model["id"]
        for family_row in sources["model_families"]
        if "component_descriptor" in family_row
        and (model := _descriptor(family_row["component_descriptor"])).get("kind") == "tool"
    )


def profile_facts() -> dict[str, dict[str, Any]]:
    """Every component model's catalog facts (``componentProfiles``)."""

    return _profiles()[2]


def _arm_row(
    model: dict[str, Any], family_row: dict[str, Any], project_root: Path | None,
    component_id: str,
) -> dict[str, Any]:
    """A machine's arm member row from its model, as a new robot is added:
    the model's sources, an identity base pose, its default joints, TCP
    speed and controlled Stop, its measured-joints stream, all under its
    default id. Without
    `owner`, `role` and lane."""

    source = family_row["source"]
    package = source["accepted_package_uri_prefix"].removeprefix("package://").split("/")[0]
    defaults = model.get("defaults") or {}
    row: dict[str, Any] = {
        "component_id": component_id,
        "component_profile_id": model["id"],
        "model_source": {
            "urdf": {"project_path": source["urdf_path"]},
            "srdf": {"project_path": source["srdf_path"]},
            "package_roots": {package: {"project_path": package}},
        },
        "root_pose_in_cell": {"position_m": [0.0, 0.0, 0.0],
                              "orientation_xyzw": [0.0, 0.0, 0.0, 1.0]},
        "frame_prefix": "",
        "target_settings": {"arm": {
            "maximum_tcp_linear_speed_mps": defaults.get(
                "maximum_tcp_linear_speed_mps", model["limits"]["max_tcp_linear_speed_mps"]),
            # The arm's controlled Stop: the model's per-joint deceleration,
            # the rest as every maintained example sets it.
            "controlled_stop": {
                "stop_mode": "soft_on_path",
                "deceleration_time_s": 1.0,
                "jerk_limits_enabled": True,
                "allow_natural_completion_on_short_path": True,
                "per_joint_limits": [
                    {"source_joint_name": joint,
                     "max_deceleration_rad_s2": defaults["controlled_stop_max_deceleration_rad_s2"],
                     "settle_velocity_threshold_rad_s": 0.01}
                    for joint in model["roles"]["joints"]["arm_joints"]
                ],
            },
        }},
        "configuration": {
            "runtime_kind": "ros2_control",
            # The fault Stop a runtime collision Stop uses (without it the
            # Stop cancels the goal and is never retried, OD-373): the
            # controlled Stop's mode and per-joint deceleration.
            "default_fault_stop_profile": {
                "profile_id": "fault_soft_on_path",
                "stop_mode": "soft_on_path",
                "deceleration_time_s": 1.0,
                "per_joint_max_deceleration_rad_s2": {
                    joint: defaults["controlled_stop_max_deceleration_rad_s2"]
                    for joint in model["roles"]["joints"]["arm_joints"]
                },
                "require_backend_confirmation": True,
            },
            "runtime_configuration": {
                "component_streams": [
                    {"budget_ns": 250000000, "controllable_component_id": component_id,
                     "stream_id": "measured_joints"}
                ],
                **({"initial_joint_positions_rad": list(defaults["initial_joint_positions_rad"])}
                   if "initial_joint_positions_rad" in defaults else {}),
            },
        },
    }
    [located] = _located_rows([row], [], scenario_root=_REPOSITORY, project_root=project_root)
    return located


def _machine_entries(
    sources: dict[str, Any], project_root: Path | None
) -> list[dict[str, Any]]:
    """`{kind: machine, model_id, title, lanes[],
    default_owner_id, owner_row, members: [{role, model_id, title, row}]}`.
    Rows are ready Deployment rows under the default ids (owner:
    `default_owner_id`, member: `<default_owner_id>_<role>`, inner references
    too), without `owner`, the members' `role` and the owner's `lane`; a
    machine runs on its robot member's lanes."""

    models: dict[str, dict[str, Any]] = {}
    families: dict[str, dict[str, Any]] = {}
    for family_row in sources["model_families"]:
        if "component_descriptor" in family_row:
            descriptor = _descriptor(family_row["component_descriptor"])
            models[descriptor["id"]] = descriptor
            families[descriptor["id"]] = family_row
    for path in sources.get("component_models", []):
        descriptor = _descriptor(path)
        models[descriptor["id"]] = descriptor
    entries = []
    for model_id, machine in sorted(models.items()):
        if machine.get("kind") != "machine":
            continue
        arm = next(
            (member for member in machine["members"]
             if models.get(member["model"], {}).get("kind") == "robot"),
            None,
        )
        owner_id = machine["default_owner_id"]
        members = []
        for member in machine["members"]:
            model = models[member["model"]]
            member_id = f"{owner_id}_{member['role']}"
            members.append({
                "role": member["role"],
                "model_id": member["model"],
                "title": model.get("title"),
                "row": (_arm_row(model, families[member["model"]], project_root, member_id)
                        if model.get("kind") == "robot"
                        else {"component_id": member_id,
                              "component_profile_id": member["model"]}),
            })
        entries.append({
            "kind": "machine",
            "model_id": model_id,
            "title": machine["title"],
            "lanes": sorted(models[arm["model"]].get("lanes") or {}) if arm else [],
            "default_owner_id": owner_id,
            "owner_row": {"component_id": owner_id, "component_profile_id": model_id,
                          "configuration": {}},
            "members": members,
        })
    return entries


def _lane_free_models(sources: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    """(descriptor path, descriptor) of every lane-free component model
    (`kind: sensor` or `kind: assistant`), sensors first."""

    models = []
    for path in sources.get("component_models", []):
        descriptor = _descriptor(path)
        if descriptor.get("kind") in _LANE_FREE_KINDS:
            models.append((path, descriptor))
    return sorted(
        models,
        key=lambda item: (_LANE_FREE_KINDS.index(item[1]["kind"]), item[1]["id"]),
    )


# The lanes where the host's Mock I/O serves a model-form I/O device.
_MOCK_IO_LANES = ("isaac", "mock")


def _owner_less_io_models(sources: dict[str, Any]) -> list[tuple[str, dict[str, Any]]]:
    """(descriptor path, descriptor) of every model-form I/O device that no
    machine model names as a member, by id."""

    descriptors = [(path, _descriptor(path)) for path in sources.get("component_models", [])]
    members = {
        member["model"]
        for _path, descriptor in descriptors
        if descriptor.get("kind") == "machine"
        for member in descriptor.get("members", [])
    }
    return sorted(
        ((path, descriptor) for path, descriptor in descriptors
         if descriptor.get("kind") == "io_device" and "lanes" in descriptor
         and descriptor["id"] not in members),
        key=lambda item: item[1]["id"],
    )


def _owner_less_io_entries(sources: dict[str, Any]) -> list[dict[str, Any]]:
    """One `io_device` entry per owner-less model-form I/O device: its ready
    row (default id the model id's last segment) on the Mock lane."""

    entries = []
    for path, model in _owner_less_io_models(sources):
        component_id = model["id"].rsplit(".", 1)[-1]
        entries.append({
            "entryId": f"model/{model['id']}",
            "kind": "io_device",
            "componentId": component_id,
            "componentProfileId": model["id"],
            "modelFamilyId": None,
            "tags": [],
            "lane": "mock",
            "description": model["title"],
            "sources": [path],
            "components": [{
                "component_id": component_id,
                "component_profile_id": model["id"],
                "configuration": {},
                "lane": "mock",
            }],
        })
    return entries


def _lane_free_entries(sources: dict[str, Any]) -> list[dict[str, Any]]:
    """One ordinary catalog entry per lane-free model: its ready row (default
    id the model id's last segment, runtime_kind its kind) without the
    runtime settings the Add panel asks for; it has no lane."""

    entries = []
    for path, model in _lane_free_models(sources):
        component_id = model["id"].rsplit(".", 1)[-1]
        entries.append({
            "entryId": f"model/{model['id']}",
            "kind": model["kind"],
            "componentId": component_id,
            "componentProfileId": model["id"],
            "modelFamilyId": None,
            "tags": [],
            "lane": None,
            "description": model["title"],
            "sources": [path],
            "components": [{
                "component_id": component_id,
                "component_profile_id": model["id"],
                "target_settings": {},
                "configuration": {
                    "runtime_kind": model["kind"], "runtime_configuration": {},
                },
            }],
        })
    return entries


def _passive_tool_entries(
    sources: dict[str, Any], project_root: Path | None, model_facts: dict[str, dict[str, Any]],
) -> list[dict[str, Any]]:
    """One ``tool`` entry per passive tool model (no ``gripper`` section): its
    ready row under the model family id with the model's own sources, no lane
    and no streams; the Add panel mounts it on a robot's ``flange``."""

    entries = []
    for family_row in sources["model_families"]:
        if "component_descriptor" not in family_row:
            continue
        path = family_row["component_descriptor"]
        model = _descriptor(path)
        if model.get("kind") != "tool" or "gripper" in model:
            continue
        family = family_row["model_family_id"]
        source = family_row["source"]
        # The package folder: the resource root less the prefix's own path
        # (`package://passive_tool/` names the model folder itself).
        package, _, inner = (
            source["accepted_package_uri_prefix"].removeprefix("package://").partition("/")
        )
        package_root = source["resource_root"].rstrip("/")
        if inner.rstrip("/"):
            package_root = package_root.removesuffix("/" + inner.rstrip("/"))
        model_source: dict[str, Any] = {
            "urdf": {"project_path": source["urdf_path"]},
            "srdf": {"project_path": source["srdf_path"]},
            "package_roots": {package: {"project_path": package_root}},
        }
        if model["id"] == GENERATED_TOOL_PROFILE_ID:
            starter = _REPOSITORY / source["resource_root"] / GENERATED_TOOL_STARTER
            model_source = {"parameters": yaml.safe_load(starter.read_text(encoding="utf-8"))}
        row = {
            "component_id": family,
            "component_profile_id": model["id"],
            "frame_prefix": "",
            "model_source": model_source,
            "target_settings": {},
            "configuration": {
                "runtime_kind": "ros2_control",
                "runtime_configuration": {"component_streams": []},
            },
        }
        # Validated as mounted: the Add panel writes its real mount row.
        mount = {
            "parent": {"component_id": "catalog_robot", "interface": "flange"},
            "child": {"component_id": family, "interface": "mount"},
        }
        [located] = _located_rows(
            [row], [mount], scenario_root=_REPOSITORY, project_root=project_root
        )
        entries.append({
            "entryId": f"model/{model['id']}",
            "kind": "tool",
            "componentId": family,
            "componentProfileId": model["id"],
            "modelFamilyId": family,
            "tags": model_facts.get(model["id"], {}).get("tags", []),
            "lane": None,
            "description": model["title"],
            "sources": [path],
            "components": [located],
        })
    return sorted(entries, key=lambda entry: entry["componentProfileId"])


def _model_payload(family_row: dict[str, Any] | None) -> dict[str, Any] | None:
    """The optional ``payload`` of a family's model asset, display-shaped.

    Mass (kg) and centre of mass (m) in the asset's root link, as the model
    asset carries them; None when the asset defines none (a payload of 0).
    """

    if family_row is None:
        return None
    asset = json.loads(
        (_REPOSITORY / family_row["catalog_source"]).read_text(encoding="utf-8")
    )
    payload = asset.get("payload")
    if payload is None:
        return None
    return {
        "massKg": payload["mass_kg"],
        "centerOfMassM": list(payload["center_of_mass_m"]),
    }


def _tcp_targets(component_type: dict[str, Any]) -> list[dict[str, Any]]:
    """The command targets with TCPs: their tagged TCP roles and default.

    A target that tags any frame role as a TCP declares one of them as its
    default (the component-type codec checks it). A component with TCPs on
    more than one target chooses its own default in the Deployment (B5).
    """

    targets = []
    for target in component_type.get("command_targets", []):
        roles = sorted(
            role["frame_role_id"]
            for role in target.get("frame_roles", [])
            if role.get("tcp") is True
        )
        if roles:
            targets.append({
                "commandTargetId": target["command_target_id"],
                "tcpFrameRoleIds": roles,
                "defaultTcpFrameRoleId": target["default_tcp_frame_role_id"],
            })
    return sorted(targets, key=lambda target: target["commandTargetId"])


def _collision_body_targets(component_type: dict[str, Any]) -> list[dict[str, Any]]:
    """The command targets with collision bodies: the body ids a TCP's
    `contact_bodies` and a collision rule can name, the contact bodies of
    each model TCP, and the target's grip body (the gripper body a carried
    object is held by), when it has one."""

    targets = []
    for target in component_type.get("command_targets", []):
        bodies = sorted(
            role["collision_body_role_id"]
            for role in target.get("collision_body_roles", [])
        )
        if not bodies:
            continue
        targets.append({
            "commandTargetId": target["command_target_id"],
            "collisionBodyRoleIds": bodies,
            "tcpContactBodyRoleIds": {
                role["frame_role_id"]: sorted(role.get("contact_body_role_ids", []))
                for role in target.get("frame_roles", [])
                if role.get("tcp") is True
            },
            "gripBodyRoleId": target.get("grip_body_role_id"),
        })
    return sorted(targets, key=lambda target: target["commandTargetId"])


def _has_motion_target(component_type: dict[str, Any]) -> bool | None:
    targets = component_type.get("command_targets")
    if not isinstance(targets, list):
        return None
    capabilities = []
    for target in targets:
        if not isinstance(target, dict) or not isinstance(target.get("capabilities"), list):
            return None
        for capability in target["capabilities"]:
            if not isinstance(capability, dict) or not isinstance(capability.get("capability_id"), str):
                return None
            capabilities.append(capability["capability_id"])
    return "execution.trajectory.execute" in capabilities


# Model-free components name these constant ids (no model, no family).
_MODEL_FREE_PROFILES = ("ai_model_v0", "io_device_v0", "pose_source_v0")


def _title(descriptor: dict[str, Any]) -> dict[str, str]:
    """The model's readable title (`component.yaml` `title`), when it declares one."""

    title = descriptor.get("title")
    return {"title": title} if isinstance(title, str) and title else {}


def _profiles() -> tuple[list[str], dict[str, str], dict[str, dict[str, Any]]]:
    """Every component model, model -> model family, model -> type facts.

    The models are the descriptors the maintained families name
    (``component_descriptor``), keyed by the descriptor's ``id``. The facts
    of a model are its lanes (``lanes.<lane>`` of its descriptor), its
    component type's tags, the ids of its attachment interfaces (a robot's
    ``flange``), its targets with TCPs, its targets with collision bodies and,
    when its model asset defines one, the model's payload (read-only package
    data).
    """

    sources = json.loads(_SOURCES.read_text(encoding="utf-8"))
    families: dict[str, str] = {}
    facts: dict[str, dict[str, Any]] = {}
    for family_row in sources["model_families"]:
        if "component_descriptor" not in family_row:
            continue
        descriptor = yaml.safe_load(
            (_REPOSITORY / family_row["component_descriptor"]).read_text(encoding="utf-8")
        )
        model = descriptor["id"]
        family = family_row["model_family_id"]
        families[model] = family
        path = _COMPONENT_TYPES / f"{family}.component.json"
        if not path.is_file():
            continue  # no component type generated yet: no facts to offer
        component_type = json.loads(path.read_text(encoding="utf-8"))
        model_facts: dict[str, Any] = {
            **_title(descriptor),
            "lanes": sorted(descriptor.get("lanes") or {}),
            "tags": sorted(component_type.get("tags", [])),
            "attachmentInterfaces": sorted(
                interface["attachment_interface_id"]
                for interface in component_type.get("attachment_interfaces", [])
            ),
            "tcpTargets": _tcp_targets(component_type),
            "frameTargets": sorted([
                {"commandTargetId": target["command_target_id"],
                 "frameRoleIds": sorted(row["frame_role_id"]
                                        for row in target["frame_roles"])}
                for target in component_type.get("command_targets", [])
                if isinstance(target.get("frame_roles"), list)
            ], key=lambda target: target["commandTargetId"]),
            "collisionBodyTargets": _collision_body_targets(component_type),
        }
        has_motion_target = _has_motion_target(component_type)
        if has_motion_target is not None:
            model_facts["hasMotionTarget"] = has_motion_target
        payload = _model_payload(family_row)
        if payload is not None:
            model_facts["payload"] = payload
        facts[model] = model_facts
    # An owner-less model-form I/O device: its own lanes plus Mock I/O's.
    for _path, descriptor in _owner_less_io_models(sources):
        facts[descriptor["id"]] = {
            **_title(descriptor),
            "lanes": sorted({*descriptor["lanes"], *_MOCK_IO_LANES}),
            "tags": [],
            "attachmentInterfaces": [],
            "tcpTargets": [],
        }
    # Lane-free models have no model family, component type or lane.
    for _path, descriptor in _lane_free_models(sources):
        facts[descriptor["id"]] = {
            **_title(descriptor),
            "lanes": [],
            "tags": [],
            "attachmentInterfaces": [],
            "tcpTargets": [],
        }
    return sorted([*families, *_MODEL_FREE_PROFILES]), families, facts


def _located_rows(
    rows: list[dict[str, Any]],
    attachments: list[dict[str, Any]],
    *,
    scenario_root: Path,
    project_root: Path | None,
) -> list[dict[str, Any]]:
    """The rows with their model sources usable from the Draft's project.

    ``attachments`` are the Scenario's rows that mount one of ``rows``: a
    mounted tool has no ``root_pose_in_cell``, and only its mount row makes
    that valid.
    """

    intent = {
        "api_version": DEPLOYMENT_INTENT_API_VERSION,
        "components": rows,
        "attachments": attachments,
        "collision_rules": default_collision_rules(),
        "policies": {},
    }
    if project_root is not None:
        return rebase_deployment_intent_sources(
            intent,
            source_project_root=scenario_root,
            destination_project_root=project_root,
        )["components"]
    located = validate_deployment_intent(intent, source="<component catalog>")
    for path, reference, _directory in iter_source_references(located):
        if "project_path" not in reference:
            continue
        current: Any = located
        for segment in path[:-1]:
            current = current[segment]
        current[path[-1]] = {
            "external_path": str(
                resolve_source_reference(reference, project_root=scenario_root)
            )
        }
    return located["components"]


def component_catalog(project_root: str | None) -> dict[str, Any]:
    """The addable component rows of the curated example Scenarios."""

    profiles, families, model_facts = _profiles()
    destination = None if project_root is None else Path(project_root)
    entries: dict[bytes, dict[str, Any]] = {}
    for deployment_path in sorted(_EXAMPLES.glob("*.scenario/deployment.yaml")):
        scenario_root = deployment_path.parent
        source = f"examples/{scenario_root.name}"
        try:
            document = load_strict_json_yaml_bytes(
                deployment_path.read_bytes(), source=source
            )
            if (
                not isinstance(document, dict)
                or document.get("api_version") != DEPLOYMENT_INTENT_API_VERSION
            ):
                continue
            intent = validate_deployment_intent(document, source=source)
            # The entry describes its example by the Scenario's one description.
            description = load_strict_json_yaml_bytes(
                (scenario_root / "scenario.yaml").read_bytes(), source=source
            )["description"]
        except (OSError, StrictArtifactError, DeploymentIntentError) as error:
            _LOGGER.warning(
                "component_catalog_source_skipped",
                extra={"source": source, "error": str(error)},
            )
            continue
        rows = {row["component_id"]: row for row in intent["components"]}
        machine_rows = _machine_rows(rows)
        for component_id, row in rows.items():
            members = _assembly(component_id, rows)
            # A machine is added whole, from its machine entry.
            if machine_rows & set(members):
                continue
            located = _located_rows(
                [deepcopy(rows[member]) for member in members],
                [
                    deepcopy(attachment)
                    for attachment in intent["attachments"]
                    if isinstance(attachment.get("child"), dict)
                    and attachment["child"].get("component_id") in members
                ],
                scenario_root=scenario_root,
                project_root=destination,
            )
            key = canonical_json_bytes([component_id, located])
            if key in entries:
                entries[key]["sources"].append(source)
                continue
            profile = row["component_profile_id"]
            family = families.get(profile)
            tags = model_facts.get(profile, {}).get("tags", [])
            entries[key] = {
                "entryId": f"{scenario_root.name}/{component_id}",
                "kind": _kind(row, tags),
                "componentId": component_id,
                "componentProfileId": profile,
                "modelFamilyId": family,
                "tags": tags,
                # The entry's own placement lane, as for the model entries.
                "lane": row.get("lane"),
                "description": description,
                "sources": [source],
                "components": located,
            }
    ordered = sorted(
        entries.values(),
        key=lambda entry: (
            _KIND_ORDER.index(entry["kind"]),
            entry["componentProfileId"],
            entry["componentId"],
            entry["entryId"],
        ),
    )
    sources = json.loads(_SOURCES.read_text(encoding="utf-8"))
    ordered += _passive_tool_entries(sources, destination, model_facts)
    ordered += _owner_less_io_entries(sources)
    ordered += _lane_free_entries(sources)
    offered = {entry["componentProfileId"] for entry in ordered}
    # One entry per machine model (owner plus member rows).
    ordered += _machine_entries(sources, destination)
    return {
        "schemaVersion": "manyforge.composer_component_catalog.v1",
        "projectRoot": project_root,
        "entries": ordered,
        "profilesWithoutEntry": [
            profile for profile in profiles if profile not in offered
        ],
        # Facts of every component model, for the Draft's rows too: its
        # lanes, tool or robot, the robots a tool can mount on, its TCPs per
        # target, and the model's own payload when it defines one.
        "componentProfiles": model_facts,
    }
