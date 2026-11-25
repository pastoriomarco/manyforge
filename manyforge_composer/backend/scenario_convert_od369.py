"""Convert Scenarios saved before OD-369 to the current file format.

The conversion of `scripts/convert_scenarios_od369.py` (its command-line
wrapper) and of the Load dialog's Upgrade and load (:mod:`.scenario_upgrade`).

A Scenario has one identity, its folder name (`<name>.scenario`), and one
description, in `scenario.yaml`. The conversion removes the fields that used
to name or describe its parts:

- `program.yaml`: `program_id`, `name`, `description`;
- `deployment.yaml`: `deployment_artifact_id`, `name`, `description`.

It also renames the retired Program profile in `program_profile`:
`composable-cell-v1` becomes `manyforge-cell-v1`, and removes the lane labels,
the Program's `compatibility.lanes` and the Deployment's `compatibility.lane`
(Scenario lane labels removed, 0.4.0: each component's own lane is the only
lane).

A Scenario folder is rewritten the way Save writes it: its members and
`scenario.yaml` with the member digests and the checked-content digest
recomputed by the Composer's own functions. In a standalone `*.program.yaml`
file only the top-level lines of those fields change, and its
`program_revision`, when present, is recomputed for the new content. A
Scenario already in the current format is left alone. Its Deployment must
already be in the current format (run the OD-46 conversion first).
"""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
from typing import Any

from manyforge_behavior.canonical_artifact import (
    compute_content_identity,
    load_strict_json_yaml_bytes,
)

from . import scenario_artifacts as sa

DROPPED = {
    "program_draft": ("program_id", "name", "description"),
    "deployment_draft": ("deployment_artifact_id", "name", "description"),
}
RETIRED_PROFILE = "composable-cell-v1"
CURRENT_PROFILE = "manyforge-cell-v1"
RENAMED = f"program_profile {RETIRED_PROFILE} -> {CURRENT_PROFILE}"
LANES_REMOVED = "compatibility.lanes removed"
LANE_REMOVED = "compatibility.lane removed"


def _drop_lanes(document: Any, field: str = "lanes") -> bool:
    """Remove ``compatibility.<field>``: the Program's ``lanes``, the
    Deployment's ``lane``."""
    compatibility = document.get("compatibility") if isinstance(document, dict) else None
    if not isinstance(compatibility, dict) or field not in compatibility:
        return False
    del compatibility[field]
    return True


def _strip(value: Any, fields: tuple[str, ...]) -> list[str]:
    if not isinstance(value, dict):
        return []
    found = [field for field in fields if field in value]
    for field in found:
        del value[field]
    return found


def convert_scenario_files(
    project: Path, files: dict[str, bytes]
) -> tuple[list[str], dict[str, bytes]]:
    """Convert the files of one Scenario folder, given by name (`project`
    only names them in messages). Returns what was done and the new content of
    every file Save writes, or ``([], {})`` when the Scenario is current.
    Writes nothing."""

    manifest = load_strict_json_yaml_bytes(
        files[sa.SCENARIO_MANIFEST], source=str(project / sa.SCENARIO_MANIFEST)
    )
    values: dict[str, Any] = {}
    changes: list[str] = []
    for member, filename in sa.SCENARIO_PROJECT_MEMBERS.items():
        values[member] = sa._load_project_member(
            files[filename], source=str(project / filename), nullable=True
        )
        changes += [f"removed {filename}: {field}"
                    for field in _strip(values[member], DROPPED[member])]
    program = values["program_draft"]
    if isinstance(program, dict) and program.get("program_profile") == RETIRED_PROFILE:
        program["program_profile"] = CURRENT_PROFILE
        changes.append(f"program.yaml: {RENAMED}")
    if _drop_lanes(program):
        changes.append(f"program.yaml: {LANES_REMOVED}")
    if _drop_lanes(values["deployment_draft"], "lane"):
        changes.append(f"deployment.yaml: {LANE_REMOVED}")
    if not changes:
        return [], {}
    document = {
        "api_version": sa.SCENARIO_API_VERSION,
        "scenario_id": manifest["scenario_id"],
        "name": manifest["name"],
        "description": manifest["description"],
        **values,
        "saved_status": {
            **manifest["saved_status"],
            "checked_content_digest": sa.scenario_content_digest(
                values["program_draft"], values["deployment_draft"]
            ),
        },
    }
    # The same encoding, member digests and manifest as Save.
    member_bytes, manifest_raw = sa._project_payload(
        document, source_snapshot=manifest["source_snapshot"]
    )
    return changes, {**member_bytes, sa.SCENARIO_MANIFEST: manifest_raw}


def convert_scenario(project: Path, *, dry_run: bool) -> list[str]:
    """Convert one Scenario folder in place; returns what was done."""

    names = (sa.SCENARIO_MANIFEST, *sa.SCENARIO_PROJECT_MEMBERS.values())
    files = {name: (project / name).read_bytes() for name in names}
    changes, outputs = convert_scenario_files(project, files)
    if not dry_run:
        for filename, raw in sorted(outputs.items()):
            if filename != sa.SCENARIO_MANIFEST:
                (project / filename).write_bytes(raw)
        if sa.SCENARIO_MANIFEST in outputs:
            (project / sa.SCENARIO_MANIFEST).write_bytes(outputs[sa.SCENARIO_MANIFEST])
    return changes


def convert_program_file(path: Path, *, dry_run: bool) -> list[str]:
    raw = path.read_bytes()
    before = load_strict_json_yaml_bytes(raw, source=str(path))
    removed = [field for field in DROPPED["program_draft"] if field in before]
    rename = before.get("program_profile") == RETIRED_PROFILE
    expected = deepcopy({key: value for key, value in before.items() if key not in removed})
    lanes = _drop_lanes(expected)
    if not removed and not rename and not lanes:
        return []
    if rename:
        expected["program_profile"] = CURRENT_PROFILE
    if "program_revision" in expected:
        expected["program_revision"] = compute_content_identity(
            expected, own_field="program_revision",
            identity_domain="program-revision", api_version=expected["api_version"],
        )
    replaced = {key: expected[key] for key in ("program_profile", "program_revision")
                if key in expected and expected[key] != before[key]}
    kept: list[str] = []
    dropping = False
    # The `lanes:` entry of the top-level `compatibility:` block: its line and
    # the deeper lines (or same-indent list items) under it.
    in_compatibility = False
    lanes_indent: int | None = None
    for line in raw.decode("utf-8").splitlines(keepends=True):
        if line[:1] not in (" ", "\t", "\n", "\r", "") or line.startswith("#"):
            key = line.split(":", 1)[0]
            dropping = key in removed
            in_compatibility = lanes and key == "compatibility"
            lanes_indent = None
            if key in replaced:
                line = f"{key}: {replaced[key]}" + line[len(line.rstrip("\r\n")):]
        elif in_compatibility and line.strip():
            indent = len(line) - len(line.lstrip(" "))
            body = line.lstrip(" ")
            if lanes_indent is not None and (
                indent > lanes_indent or (indent == lanes_indent and body.startswith("- "))
            ):
                continue
            lanes_indent = indent if body.split(":", 1)[0] == "lanes" else None
            if lanes_indent is not None:
                continue
        if not dropping:
            kept.append(line)
    text = "".join(kept)
    after = load_strict_json_yaml_bytes(text.encode("utf-8"), source=str(path))
    if after != expected:
        raise ValueError("the top-level fields could not be changed line by line")
    if not dry_run:
        path.write_text(text, encoding="utf-8")
    return ([f"removed {field}" for field in removed] + ([RENAMED] if rename else [])
            + ([LANES_REMOVED] if lanes else []))
