"""Node catalog loader and validator for ManyForge behavior trees."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

import yaml

from manyforge_behavior.capabilities import CAPABILITY_REGISTRY
from manyforge_behavior.catalog_schema import (
    CatalogSchema,
    CatalogSchemaError,
    CatalogSchemaRegistry,
    parse_schema,
    parse_schema_registry,
    schema_type_label,
    validate_value,
)


SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+$")
EXPECTED_API_VERSION = "manyforge.node_catalog.v0"
EXPECTED_API_VERSION_V1 = "manyforge.node_catalog.v1"
SUPPORTED_API_VERSIONS = {EXPECTED_API_VERSION_V1}
_V1_LEGACY_PARAMETER_KEYS = {"constraints", "enum_values", "type"}
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
_NODE_KINDS = {"leaf", "decorator", "composite", "macro", "authoring_macro"}


@dataclass
class CatalogEnumOption:
    value: str
    label: Optional[str] = None
    description: str = ""
    profiles_min: str = "v0.0"
    required_capabilities: list[str] = field(default_factory=list)


@dataclass
class CatalogVisibleWhen:
    param: str
    equals: Any


@dataclass
class CatalogParameter:
    name: str
    type: str
    required: bool
    schema: Optional[CatalogSchema] = None
    default: Any = None
    enum_values: list[str] = field(default_factory=list)
    enum_options: list[CatalogEnumOption] = field(default_factory=list)
    constraints: dict[str, Any] = field(default_factory=dict)
    parameterizable: bool = False
    profiles_min: str = "v0.0"
    visible_when: Optional[CatalogVisibleWhen] = None
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
    # Optional swap-compatibility family. When two entries declare the
    # same swap_class, tree_draft_change_node_kind treats them as
    # semantically interchangeable (subject to arity and param
    # validation). When they declare different families, the kind change
    # is rejected unless the caller passes force=true. Entries without a
    # declared swap_class fall back to structural validation only.
    # Free-form string; used as an equivalence key, not as a typed enum.
    swap_class: Optional[str] = None
    # Optional alias hints surfaced when params get dropped during a
    # swap. Maps an old-id parameter name to its analogous new-id
    # parameter name. Pure documentation; the swap handler does NOT
    # auto-rename, only reports the alias to the caller.
    swap_param_aliases: dict[str, str] = field(default_factory=dict)


class CatalogValidationError(Exception):
    pass


class NodeCatalog:
    """Loads and validates a node catalog from YAML."""

    def __init__(self, path: str | Path):
        self._path = Path(path)
        self._entries: dict[str, CatalogEntry] = {}
        self._version: str = ""
        self._api_version: str = ""
        self._schema_registry = CatalogSchemaRegistry()
        self._load_and_validate()

    def _load_and_validate(self) -> None:
        with open(self._path) as f:
            data = yaml.safe_load(f)

        if not isinstance(data, dict):
            raise CatalogValidationError("Catalog must be a YAML mapping")

        # Validate api_version
        api_version = data.get("api_version")
        if api_version not in SUPPORTED_API_VERSIONS:
            raise CatalogValidationError(
                "Expected api_version to be one of "
                f"{sorted(SUPPORTED_API_VERSIONS)}, got '{api_version}'"
            )
        self._api_version = str(api_version)

        # Validate catalog_version
        version = data.get("catalog_version", "")
        if not SEMVER_RE.match(str(version)):
            raise CatalogValidationError(
                f"catalog_version must match X.Y.Z, got '{version}'"
            )
        self._version = str(version)

        if self._api_version == EXPECTED_API_VERSION_V1:
            try:
                self._schema_registry = parse_schema_registry(data.get("schema_types", {}))
            except CatalogSchemaError as exc:
                raise CatalogValidationError(str(exc)) from exc

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
        entry_id = raw["id"]
        children = raw.get("children", {})
        max_children = children.get("max", 0)
        if max_children == "unbounded":
            max_children = -1

        params = []
        for p in raw.get("parameters", []):
            schema = self._parse_parameter_schema(p, entry_id=entry_id)
            if self._api_version == EXPECTED_API_VERSION_V1:
                legacy_keys = sorted(set(p).intersection(_V1_LEGACY_PARAMETER_KEYS))
                if legacy_keys:
                    raise CatalogValidationError(
                        f"Entry '{entry_id}', param '{p.get('name', '<unknown>')}': "
                        "v1 parameters must derive type, enum_values, and constraints "
                        f"from schema; remove legacy keys {legacy_keys}"
                    )
                param_type = None
            else:
                param_type = p.get("type")
            if schema is not None and (param_type is None or str(param_type).strip() == ""):
                try:
                    param_type = schema_type_label(schema, self._schema_registry)
                except CatalogSchemaError as exc:
                    raise CatalogValidationError(
                        f"Entry '{entry_id}', param '{p.get('name', '<unknown>')}': {exc}"
                    ) from exc
            if not isinstance(param_type, str) or not param_type.strip():
                raise CatalogValidationError(
                    f"Entry '{entry_id}', param '{p.get('name', '<unknown>')}': "
                    "type is required for v0 catalogs and must be derivable "
                    "from schema for v1 catalogs"
                )
            required = p.get("required", False)
            if not isinstance(required, bool):
                raise CatalogValidationError(
                    f"Entry '{entry_id}', param '{p.get('name', '<unknown>')}': "
                    "required must be a boolean"
                )
            enum_values, enum_options = self._parse_enum_metadata(
                p,
                entry_id=entry_id,
            )
            if schema is not None and param_type == "enum":
                schema_enum_values = self._schema_enum_values(schema)
                if enum_values and enum_values != schema_enum_values:
                    raise CatalogValidationError(
                        f"Entry '{entry_id}', param '{p.get('name', '<unknown>')}': "
                        "enum_options values must match schema enum values"
                    )
                enum_values = schema_enum_values
                if enum_options:
                    option_values = [option.value for option in enum_options]
                    if option_values != enum_values:
                        raise CatalogValidationError(
                            f"Entry '{entry_id}', param '{p.get('name', '<unknown>')}': "
                            "enum_options values must match schema enum values"
                        )
                else:
                    enum_options = [CatalogEnumOption(value=value) for value in enum_values]
            constraints = (
                self._schema_constraints(schema)
                if schema is not None
                else p.get("constraints", {})
            )
            params.append(
                CatalogParameter(
                    name=p["name"],
                    type=param_type,
                    required=required,
                    schema=schema,
                    default=p.get("default"),
                    enum_values=enum_values,
                    enum_options=enum_options,
                    constraints=constraints,
                    parameterizable=p.get("parameterizable", False),
                    profiles_min=self._parse_profile_min(
                        p.get("profiles"),
                        entry_id=entry_id,
                        param_name=p["name"],
                    ),
                    visible_when=self._parse_visible_when(
                        p.get("visible_when"),
                        entry_id=entry_id,
                        param_name=p["name"],
                    ),
                    ui_editor=p.get("ui_editor"),
                    blackboard_role=p.get("blackboard_role"),
                    blackboard_contract=p.get("blackboard_contract"),
                    blackboard_value_type=p.get("blackboard_value_type"),
                    description=p.get("description", ""),
                )
            )

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

        swap_class_raw = raw.get("swap_class")
        if swap_class_raw is not None and not isinstance(swap_class_raw, str):
            raise CatalogValidationError(
                f"Entry '{entry_id}': swap_class must be a string when declared"
            )
        swap_class = swap_class_raw.strip() if isinstance(swap_class_raw, str) and swap_class_raw.strip() else None
        swap_param_aliases_raw = raw.get("swap_param_aliases", {})
        if not isinstance(swap_param_aliases_raw, dict):
            raise CatalogValidationError(
                f"Entry '{entry_id}': swap_param_aliases must be a mapping"
            )
        swap_param_aliases: dict[str, str] = {}
        for k, v in swap_param_aliases_raw.items():
            if not isinstance(k, str) or not isinstance(v, str):
                raise CatalogValidationError(
                    f"Entry '{entry_id}': swap_param_aliases keys and values must be strings"
                )
            swap_param_aliases[k] = v

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
            profiles_min=self._parse_profile_min(raw.get("profiles"), entry_id=entry_id),
            runners=raw.get("runners", {}),
            interruptible=bool(raw.get("interruptible", False)),
            default_interrupt_scope=raw.get("default_interrupt_scope"),
            swap_class=swap_class,
            swap_param_aliases=swap_param_aliases,
        )

    def _parse_parameter_schema(
        self,
        raw_param: dict[str, Any],
        *,
        entry_id: str,
    ) -> CatalogSchema | None:
        if self._api_version != EXPECTED_API_VERSION_V1:
            return None
        param_name = raw_param.get("name", "<unknown>")
        has_schema = "schema" in raw_param
        has_schema_ref = "schema_ref" in raw_param
        if has_schema and has_schema_ref:
            raise CatalogValidationError(
                f"Entry '{entry_id}', param '{param_name}': declare only one of schema or schema_ref"
            )
        if not has_schema and not has_schema_ref:
            raise CatalogValidationError(
                f"Entry '{entry_id}', param '{param_name}': v1 parameters require schema or schema_ref"
            )
        try:
            if has_schema_ref:
                return parse_schema(
                    {"schema_ref": raw_param["schema_ref"]},
                    path=f"entries.{entry_id}.parameters.{param_name}.schema_ref",
                )
            return parse_schema(
                raw_param["schema"],
                path=f"entries.{entry_id}.parameters.{param_name}.schema",
            )
        except CatalogSchemaError as exc:
            raise CatalogValidationError(str(exc)) from exc

    def _schema_enum_values(self, schema: CatalogSchema) -> list[str]:
        try:
            concrete = self._schema_registry.resolve(schema)
        except CatalogSchemaError as exc:
            raise CatalogValidationError(str(exc)) from exc
        if concrete.schema_type != "enum":
            return []
        return list(concrete.enum_values)

    def _schema_constraints(self, schema: CatalogSchema | None) -> dict[str, Any]:
        if schema is None:
            return {}
        try:
            concrete = self._schema_registry.resolve(schema)
        except CatalogSchemaError as exc:
            raise CatalogValidationError(str(exc)) from exc
        if concrete.schema_type not in {"float", "int"}:
            return {}
        return dict(concrete.constraints)

    @staticmethod
    def _parse_profile_min(
        profiles: Any,
        *,
        entry_id: str,
        param_name: str | None = None,
    ) -> str:
        if profiles is None:
            return "v0.0"
        if not isinstance(profiles, dict):
            context = f"Entry '{entry_id}'"
            if param_name is not None:
                context += f", param '{param_name}'"
            raise CatalogValidationError(f"{context}: profiles must be a mapping")
        value = profiles.get("min_profile", "v0.0")
        if not isinstance(value, str) or not value.strip():
            context = f"Entry '{entry_id}'"
            if param_name is not None:
                context += f", param '{param_name}'"
            raise CatalogValidationError(
                f"{context}: profiles.min_profile must be a non-empty string"
            )
        return value

    @staticmethod
    def _parse_visible_when(
        raw_value: Any,
        *,
        entry_id: str,
        param_name: str,
    ) -> CatalogVisibleWhen | None:
        if raw_value is None:
            return None
        if not isinstance(raw_value, dict):
            raise CatalogValidationError(
                f"Entry '{entry_id}', param '{param_name}': visible_when must be a mapping"
            )
        ref_param = raw_value.get("param")
        if not isinstance(ref_param, str) or not ref_param.strip():
            raise CatalogValidationError(
                f"Entry '{entry_id}', param '{param_name}': visible_when.param "
                "must be a non-empty string"
            )
        if "equals" not in raw_value:
            raise CatalogValidationError(
                f"Entry '{entry_id}', param '{param_name}': visible_when.equals is required"
            )
        return CatalogVisibleWhen(param=ref_param, equals=raw_value["equals"])

    def _parse_enum_metadata(
        self,
        raw_param: dict[str, Any],
        *,
        entry_id: str,
    ) -> tuple[list[str], list[CatalogEnumOption]]:
        param_name = raw_param["name"]
        raw_enum_values = raw_param.get("enum_values", [])
        raw_enum_options = raw_param.get("enum_options", [])
        if raw_enum_values is None:
            raw_enum_values = []
        if raw_enum_options is None:
            raw_enum_options = []

        if not isinstance(raw_enum_values, list):
            raise CatalogValidationError(
                f"Entry '{entry_id}', param '{param_name}': enum_values must be a list"
            )
        enum_values = list(raw_enum_values)

        if raw_enum_options:
            if not isinstance(raw_enum_options, list):
                raise CatalogValidationError(
                    f"Entry '{entry_id}', param '{param_name}': enum_options must be a list"
                )
            enum_options = [
                self._parse_enum_option(
                    raw_option,
                    entry_id=entry_id,
                    param_name=param_name,
                )
                for raw_option in raw_enum_options
            ]
            option_values = [option.value for option in enum_options]
            if enum_values and enum_values != option_values:
                raise CatalogValidationError(
                    f"Entry '{entry_id}', param '{param_name}': enum_values must match "
                    "enum_options values"
                )
            if not enum_values:
                enum_values = option_values
            return enum_values, enum_options

        return enum_values, [
            CatalogEnumOption(value=value)
            for value in enum_values
        ]

    def _parse_enum_option(
        self,
        raw_option: Any,
        *,
        entry_id: str,
        param_name: str,
    ) -> CatalogEnumOption:
        if not isinstance(raw_option, dict):
            raise CatalogValidationError(
                f"Entry '{entry_id}', param '{param_name}': enum_options items "
                "must be mappings"
            )
        value = raw_option.get("value")
        if not isinstance(value, str) or not value.strip():
            raise CatalogValidationError(
                f"Entry '{entry_id}', param '{param_name}': enum option value "
                "must be a non-empty string"
            )
        required_capabilities = raw_option.get("required_capabilities", [])
        if required_capabilities is None:
            required_capabilities = []
        if not isinstance(required_capabilities, list) or not all(
            isinstance(capability, str) and capability.strip()
            for capability in required_capabilities
        ):
            raise CatalogValidationError(
                f"Entry '{entry_id}', param '{param_name}', enum option '{value}': "
                "required_capabilities must be a list of non-empty strings"
            )
        label = raw_option.get("label")
        if label is not None and not isinstance(label, str):
            raise CatalogValidationError(
                f"Entry '{entry_id}', param '{param_name}', enum option '{value}': "
                "label must be a string"
            )
        description = raw_option.get("description", "")
        if not isinstance(description, str):
            raise CatalogValidationError(
                f"Entry '{entry_id}', param '{param_name}', enum option '{value}': "
                "description must be a string"
            )
        return CatalogEnumOption(
            value=value,
            label=label,
            description=description,
            profiles_min=self._parse_profile_min(
                raw_option.get("profiles"),
                entry_id=entry_id,
                param_name=param_name,
            ),
            required_capabilities=list(required_capabilities),
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
        if entry.kind not in _NODE_KINDS:
            allowed = ", ".join(sorted(_NODE_KINDS))
            raise CatalogValidationError(
                f"Entry '{entry.id}': kind must be one of: {allowed}"
            )
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
        if entry.kind == "authoring_macro":
            if entry.children_min != 0 or entry.children_max != 0:
                raise CatalogValidationError(
                    f"Entry '{entry.id}': authoring macro must have children min=0, max=0"
                )
            if entry.runners:
                raise CatalogValidationError(
                    f"Entry '{entry.id}': authoring macro must not declare runtime runners"
                )

        # Enum parameters must have enum_values
        for p in entry.parameters:
            self._validate_profile_min(
                p.profiles_min,
                f"Entry '{entry.id}', param '{p.name}'",
            )
            if p.schema is not None:
                try:
                    self._schema_registry.resolve(p.schema)
                except CatalogSchemaError as exc:
                    raise CatalogValidationError(
                        f"Entry '{entry.id}', param '{p.name}': {exc}"
                    ) from exc
                if p.default is not None:
                    default_issues = validate_value(
                        p.schema,
                        p.default,
                        registry=self._schema_registry,
                        path=f"Entry '{entry.id}', param '{p.name}'.default",
                    )
                    if default_issues:
                        issue = default_issues[0]
                        raise CatalogValidationError(
                            f"{issue.path}: {issue.message}"
                        )
            if p.type != "enum" and (p.enum_values or p.enum_options):
                raise CatalogValidationError(
                    f"Entry '{entry.id}', param '{p.name}': enum metadata "
                    "requires type=enum"
                )
            if p.type == "enum" and not p.enum_values:
                raise CatalogValidationError(
                    f"Entry '{entry.id}', param '{p.name}': "
                    "enum type requires non-empty enum_values or enum_options"
                )
            if p.type == "enum":
                if not all(isinstance(value, str) and value.strip() for value in p.enum_values):
                    raise CatalogValidationError(
                        f"Entry '{entry.id}', param '{p.name}': "
                        "enum_values must be a list of non-empty strings"
                    )
                if len(p.enum_values) != len(set(p.enum_values)):
                    raise CatalogValidationError(
                        f"Entry '{entry.id}', param '{p.name}': "
                        "enum_values contains duplicate values"
                    )
                if not p.enum_options:
                    raise CatalogValidationError(
                        f"Entry '{entry.id}', param '{p.name}': "
                        "enum_options must not be empty"
                    )
                option_values = [option.value for option in p.enum_options]
                if option_values != p.enum_values:
                    raise CatalogValidationError(
                        f"Entry '{entry.id}', param '{p.name}': "
                        "enum_values must match enum_options values"
                    )
                if len(option_values) != len(set(option_values)):
                    raise CatalogValidationError(
                        f"Entry '{entry.id}', param '{p.name}': "
                        "enum_options contains duplicate values"
                    )
                for option in p.enum_options:
                    self._validate_profile_min(
                        option.profiles_min,
                        (
                            f"Entry '{entry.id}', param '{p.name}', "
                            f"enum option '{option.value}'"
                        ),
                    )
                    unknown_option_capabilities = sorted(
                        capability
                        for capability in option.required_capabilities
                        if capability not in CAPABILITY_REGISTRY
                    )
                    if unknown_option_capabilities:
                        raise CatalogValidationError(
                            f"Entry '{entry.id}', param '{p.name}', "
                            f"enum option '{option.value}': "
                            "unknown required_capabilities "
                            f"{unknown_option_capabilities}"
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
        self._validate_profile_min(entry.profiles_min, f"Entry '{entry.id}'")
        for p in entry.parameters:
            if p.visible_when is None:
                continue
            if p.visible_when.param == p.name or p.visible_when.param not in declared_params:
                raise CatalogValidationError(
                    f"Entry '{entry.id}', param '{p.name}': visible_when.param "
                    f"references unknown parameter '{p.visible_when.param}'"
                )
        if not isinstance(entry.required_capabilities, list) or not all(
            isinstance(capability, str) and capability.strip()
            for capability in entry.required_capabilities
        ):
            raise CatalogValidationError(
                f"Entry '{entry.id}': required_capabilities must be a list of non-empty strings"
            )
        unknown_capabilities = sorted(
            capability
            for capability in entry.required_capabilities
            if capability not in CAPABILITY_REGISTRY
        )
        if unknown_capabilities:
            raise CatalogValidationError(
                f"Entry '{entry.id}': unknown required_capabilities {unknown_capabilities}"
            )

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

        # Runtime entries must be executable. Authoring macros are expanded by
        # Composer and are deliberately absent from the persisted/runtime graph.
        if entry.kind != "authoring_macro" and "py_trees" not in entry.runners:
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

    @staticmethod
    def _validate_profile_min(value: str, context: str) -> None:
        if not isinstance(value, str) or not value.strip() or _parse_profile(value) is None:
            raise CatalogValidationError(
                f"{context}: profiles.min_profile must be a valid profile string"
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

    @property
    def api_version(self) -> str:
        return self._api_version

    @property
    def schema_registry(self) -> CatalogSchemaRegistry:
        return self._schema_registry


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


def profile_satisfied(min_profile: str, target: str) -> bool:
    """Public profile compatibility check used by validators and UIs."""
    return _profile_satisfied(min_profile, target)


def _parse_profile(profile: str) -> Optional[tuple[int, int]]:
    """Parse 'vX.Y' into (X, Y). Returns None if unparseable."""
    m = re.match(r"^v(\d+)\.(\d+)$", profile)
    if m:
        return (int(m.group(1)), int(m.group(2)))
    m = re.match(r"^v(\d+)$", profile)
    if m:
        return (int(m.group(1)), 0)
    return None
