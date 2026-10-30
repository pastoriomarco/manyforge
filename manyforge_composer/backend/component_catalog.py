"""Read-only catalog of the components a Deployment Draft can add.

Add component composes from components that already exist: each complete
component row of the curated example Scenarios shipped with ManyForge
(``examples/*.scenario/deployment.yaml``). A row that names another component
of its Scenario (a pneumatic gripper's I/O device) is offered together with
it. Model-source paths are rebased to the current Deployment Draft's project
folder (absolute when the Draft has none). Tags come from the component-type
catalog of the row's model family, for Program role compatibility; a family
tagged ``tool`` is a tool, mounted on a robot's ``flange`` by an attachment row.

Nothing here compiles or changes a Draft: the Add component Draft route adds
the rows (and a tool's attachment row), and the Apply compiler checks them.
"""

from __future__ import annotations

from copy import deepcopy
import json
import logging
from pathlib import Path
from typing import Any

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    canonical_json_bytes,
    load_strict_json_yaml_bytes,
)

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
_EXAMPLES = _REPOSITORY / "examples"
_MAINTAINED = _REPOSITORY / "manyforge_deployment/resources/maintained"
_SOURCES = _MAINTAINED / "maintained_deployment_sources.v1.json"
_COMPONENT_TYPES = _MAINTAINED / "catalog/component_type"

# runtime_kind -> catalog kind; every other row carries a model: a tool when
# its component type is tagged ``tool``, otherwise a robot.
_MODEL_FREE_KINDS = {
    "io_provider": "io_device",
    "pose_source": "pose_source",
    "ai_model": "ai_model",
}
_KIND_ORDER = ("robot", "tool", "io_device", "pose_source", "ai_model")


def _kind(row: dict[str, Any], tags: list[str]) -> str:
    configuration = row.get("configuration")
    runtime_kind = (
        configuration.get("runtime_kind") if isinstance(configuration, dict) else None
    )
    if runtime_kind in _MODEL_FREE_KINDS:
        return _MODEL_FREE_KINDS[runtime_kind]
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


def _profiles() -> tuple[list[str], dict[str, str], dict[str, dict[str, Any]]]:
    """Every component profile, profile -> model family, family -> type facts.

    The facts of a component type are its tags, the ids of its attachment
    interfaces (a robot's ``flange``), its targets with TCPs and, when its
    model asset defines one, the model's payload (read-only package data).
    """

    sources = json.loads(_SOURCES.read_text(encoding="utf-8"))
    rows = sources["runtime_restoration_profiles"]
    families = {
        row["runtime_restoration_profile_id"]: row["model_family_id"]
        for row in rows
        if isinstance(row.get("model_family_id"), str)
    }
    family_rows = {
        row["model_family_id"]: row for row in sources.get("model_families", [])
    }
    facts: dict[str, dict[str, Any]] = {}
    for family in set(families.values()):
        path = _COMPONENT_TYPES / f"{family}.component.json"
        if path.is_file():
            component_type = json.loads(path.read_text(encoding="utf-8"))
            facts[family] = {
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
            }
            has_motion_target = _has_motion_target(component_type)
            if has_motion_target is not None:
                facts[family]["hasMotionTarget"] = has_motion_target
            payload = _model_payload(family_rows.get(family))
            if payload is not None:
                facts[family]["payload"] = payload
    return sorted(row["runtime_restoration_profile_id"] for row in rows), families, facts


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
        "deployment_artifact_id": "component_catalog",
        "name": "component catalog",
        "components": rows,
        "attachments": attachments,
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

    profiles, families, family_facts = _profiles()
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
        except (OSError, StrictArtifactError, DeploymentIntentError) as error:
            _LOGGER.warning(
                "component_catalog_source_skipped",
                extra={"source": source, "error": str(error)},
            )
            continue
        rows = {row["component_id"]: row for row in intent["components"]}
        compatibility = intent.get("compatibility")
        lane = (
            compatibility.get("lane")
            if isinstance(compatibility, dict)
            and isinstance(compatibility.get("lane"), str)
            else None
        )
        for component_id, row in rows.items():
            members = _assembly(component_id, rows)
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
            tags = family_facts.get(family, {}).get("tags", [])
            entries[key] = {
                "entryId": f"{scenario_root.name}/{component_id}",
                "kind": _kind(row, tags),
                "componentId": component_id,
                "componentProfileId": profile,
                "modelFamilyId": family,
                "tags": tags,
                "lane": lane,
                "description": intent.get("description"),
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
    offered = {entry["componentProfileId"] for entry in ordered}
    return {
        "schemaVersion": "manyforge.composer_component_catalog.v1",
        "projectRoot": project_root,
        "entries": ordered,
        "profilesWithoutEntry": [
            profile for profile in profiles if profile not in offered
        ],
        # Type facts of every profile with a component type, for the Draft's
        # rows too: tool or robot, the robots a tool can mount on, its TCPs
        # per target, and the model's own payload when it defines one.
        "componentProfiles": {
            profile: family_facts[families[profile]]
            for profile in profiles
            if families.get(profile) in family_facts
        },
    }
