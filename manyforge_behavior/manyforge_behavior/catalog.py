"""Node catalog loader and validator for ManyForge behavior trees."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import yaml


SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+$")
EXPECTED_API_VERSION = "manyforge.node_catalog.v0"
_BLACKBOARD_ROLES = {
    "namespace_writer",
    "namespace_reader",
    "key_writer",
    "key_reader",
    "field_reader",
    "literal_or_key_reader",
}
_BLACKBOARD_VALUE_TYPES = {
    "string",
    "float",
    "int",
    "bool",
    "pose",
    "float_list",
    "string_list",
    "dict",
}
_INTERRUPT_SCOPES = {
    "active_node",
    "active_subtree",
    "cycle",
    "kernel_motion",
}
_STABILITY_LEVELS = {
    "stable",
    "experimental",
    "deprecated",
}


@dataclass
class CatalogParameter:
    name: str
    type: str
    required: bool
    default: Any = None
    enum_values: list[str] = field(default_factory=list)
    constraints: dict[str, Any] = field(default_factory=dict)
    parameterizable: bool = False
    ui_editor: Optional[str] = None
    blackboard_role: Optional[str] = None
    blackboard_contract: Optional[str] = None
    blackboard_value_type: Optional[str] = None
    description: str = ""


@dataclass
class CatalogEntry:
    id: str
    name: str
    category: str
    description: str
    kind: str  # leaf, decorator, composite, macro
    children_min: int
    children_max: int  # -1 means unbounded
    stability: str = "stable"
    parameters: list[CatalogParameter] = field(default_factory=list)
    bb_reader_params: list[str] = field(default_factory=list)
    bb_writer_params: list[str] = field(default_factory=list)
    required_capabilities: list[str] = field(default_factory=list)
    profiles_min: str = "v0.0"
    runners: dict[str, Any] = field(default_factory=dict)
    interruptible: bool = False
    default_interrupt_scope: Optional[str] = None


class CatalogValidationError(Exception):
    pass


class NodeCatalog:
    """Loads and validates a node catalog from YAML."""

    def __init__(self, path: str | Path):
        self._path = Path(path)
        self._entries: dict[str, CatalogEntry] = {}
        self._version: str = ""
        self._load_and_validate()

    def _load_and_validate(self) -> None:
        with open(self._path) as f:
            data = yaml.safe_load(f)

        if not isinstance(data, dict):
            raise CatalogValidationError("Catalog must be a YAML mapping")

        # Validate api_version
        api_version = data.get("api_version")
        if api_version != EXPECTED_API_VERSION:
            raise CatalogValidationError(
                f"Expected api_version '{EXPECTED_API_VERSION}', got '{api_version}'"
            )

        # Validate catalog_version
        version = data.get("catalog_version", "")
        if not SEMVER_RE.match(str(version)):
            raise CatalogValidationError(
                f"catalog_version must match X.Y.Z, got '{version}'"
            )
        self._version = str(version)

        # Parse entries
        raw_entries = data.get("entries", [])
        if not isinstance(raw_entries, list):
            raise CatalogValidationError("entries must be a list")

        seen_ids: set[str] = set()
        for raw in raw_entries:
            entry = self._parse_entry(raw)
            if entry.id in seen_ids:
                raise CatalogValidationError(f"Duplicate entry id: '{entry.id}'")
            seen_ids.add(entry.id)
            self._validate_entry(entry)
            self._entries[entry.id] = entry

    def _parse_entry(self, raw: dict[str, Any]) -> CatalogEntry:
        children = raw.get("children", {})
        max_children = children.get("max", 0)
        if max_children == "unbounded":
            max_children = -1

        params = []
        for p in raw.get("parameters", []):
            params.append(
                CatalogParameter(
                    name=p["name"],
                    type=p["type"],
                    required=p.get("required", False),
                    default=p.get("default"),
                    enum_values=p.get("enum_values", []),
                    constraints=p.get("constraints", {}),
                    parameterizable=p.get("parameterizable", False),
                    ui_editor=p.get("ui_editor"),
                    blackboard_role=p.get("blackboard_role"),
                    blackboard_contract=p.get("blackboard_contract"),
                    blackboard_value_type=p.get("blackboard_value_type"),
                    description=p.get("description", ""),
                )
            )

        entry_id = raw["id"]
        bb_reader_params = self._parse_blackboard_param_names(
            raw.get("bb_reader_params", []),
            field_name="bb_reader_params",
            entry_id=entry_id,
        )
        bb_writer_params = self._parse_blackboard_param_names(
            raw.get("bb_writer_params", []),
            field_name="bb_writer_params",
            entry_id=entry_id,
        )

        return CatalogEntry(
            id=entry_id,
            name=raw.get("name", entry_id),
            category=raw.get("category", ""),
            description=raw.get("description", ""),
            stability=raw.get("stability", "stable"),
            kind=raw.get("kind", "leaf"),
            children_min=children.get("min", 0),
            children_max=max_children,
            parameters=params,
            bb_reader_params=bb_reader_params,
            bb_writer_params=bb_writer_params,
            required_capabilities=raw.get("required_capabilities", []),
            profiles_min=raw.get("profiles", {}).get("min_profile", "v0.0"),
            runners=raw.get("runners", {}),
            interruptible=bool(raw.get("interruptible", False)),
            default_interrupt_scope=raw.get("default_interrupt_scope"),
        )

    @staticmethod
    def _parse_blackboard_param_names(
        raw_value: Any,
        *,
        field_name: str,
        entry_id: str,
    ) -> list[str]:
        if raw_value in (None, []):
            return []
        if not isinstance(raw_value, list) or not all(
            isinstance(item, str) and item.strip() for item in raw_value
        ):
            raise CatalogValidationError(
                f"Entry '{entry_id}': {field_name} must be a list of non-empty strings"
            )
        return list(raw_value)

    def _validate_entry(self, entry: CatalogEntry) -> None:
        # Kind/children consistency
        if entry.stability not in _STABILITY_LEVELS:
            allowed = ", ".join(sorted(_STABILITY_LEVELS))
            raise CatalogValidationError(
                f"Entry '{entry.id}': stability must be one of: {allowed}"
            )
        if entry.kind == "leaf" and entry.children_max != 0:
            raise CatalogValidationError(
                f"Entry '{entry.id}': leaf must have children.max=0"
            )
        if entry.kind == "decorator":
            if entry.children_min != 1 or entry.children_max != 1:
                raise CatalogValidationError(
                    f"Entry '{entry.id}': decorator must have children min=1, max=1"
                )

        # Enum parameters must have enum_values
        for p in entry.parameters:
            if p.type == "enum" and not p.enum_values:
                raise CatalogValidationError(
                    f"Entry '{entry.id}', param '{p.name}': "
                    "enum type requires non-empty enum_values"
                )
            if p.blackboard_role is not None and p.blackboard_role not in _BLACKBOARD_ROLES:
                allowed = ", ".join(sorted(_BLACKBOARD_ROLES))
                raise CatalogValidationError(
                    f"Entry '{entry.id}', param '{p.name}': "
                    f"blackboard_role must be one of: {allowed}"
                )
            if (
                p.blackboard_value_type is not None
                and p.blackboard_value_type not in _BLACKBOARD_VALUE_TYPES
            ):
                allowed = ", ".join(sorted(_BLACKBOARD_VALUE_TYPES))
                raise CatalogValidationError(
                    f"Entry '{entry.id}', param '{p.name}': "
                    f"blackboard_value_type must be one of: {allowed}"
                )

        declared_params = {p.name for p in entry.parameters}
        for field_name, param_names in (
            ("bb_reader_params", entry.bb_reader_params),
            ("bb_writer_params", entry.bb_writer_params),
        ):
            if len(param_names) != len(set(param_names)):
                raise CatalogValidationError(
                    f"Entry '{entry.id}': {field_name} contains duplicate parameter names"
                )
            for param_name in param_names:
                if param_name not in declared_params:
                    raise CatalogValidationError(
                        f"Entry '{entry.id}': {field_name} references unknown parameter '{param_name}'"
                    )

        # Must have py_trees runner
        if "py_trees" not in entry.runners:
            raise CatalogValidationError(
                f"Entry '{entry.id}': must have a 'py_trees' runner"
            )
        if entry.default_interrupt_scope is not None and entry.default_interrupt_scope not in _INTERRUPT_SCOPES:
            allowed = ", ".join(sorted(_INTERRUPT_SCOPES))
            raise CatalogValidationError(
                f"Entry '{entry.id}': default_interrupt_scope must be one of: {allowed}"
            )
        if entry.default_interrupt_scope is not None and not entry.interruptible:
            raise CatalogValidationError(
                f"Entry '{entry.id}': default_interrupt_scope requires interruptible=true"
            )

    def get(self, node_id: str) -> CatalogEntry:
        """Get entry by id. Raises KeyError if not found."""
        return self._entries[node_id]

    def filter_by_profile(self, profile: str) -> list[CatalogEntry]:
        """Return entries available for the given profile."""
        return [
            e
            for e in self._entries.values()
            if _profile_satisfied(e.profiles_min, profile)
        ]

    @property
    def entries(self) -> list[CatalogEntry]:
        return list(self._entries.values())

    @property
    def version(self) -> str:
        return self._version


def _profile_satisfied(min_profile: str, target: str) -> bool:
    """Check if target profile satisfies min_profile requirement.

    Profile ordering: v0.0 < v0.1 < ... < v1.
    """
    if min_profile == target:
        return True
    # Parse version numbers
    min_parts = _parse_profile(min_profile)
    target_parts = _parse_profile(target)
    if min_parts is None or target_parts is None:
        return min_profile == target
    return target_parts >= min_parts


def _parse_profile(profile: str) -> Optional[tuple[int, int]]:
    """Parse 'vX.Y' into (X, Y). Returns None if unparseable."""
    m = re.match(r"^v(\d+)\.(\d+)$", profile)
    if m:
        return (int(m.group(1)), int(m.group(2)))
    m = re.match(r"^v(\d+)$", profile)
    if m:
        return (int(m.group(1)), 0)
    return None
