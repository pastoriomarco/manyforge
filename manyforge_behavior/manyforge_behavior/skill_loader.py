"""Build-owned skill artifacts: bounded authoring expansion, never execution.

Whole-value substitutions preserve typed values. Expanded trees use the
ordinary Program validator and still require Binding compilation/admission.
No runtime imports, callbacks, queues, or skill executor exist.
"""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import re
from typing import Any

from .canonical_artifact import (
    canonical_json_bytes,
    compute_content_identity,
    load_strict_json_yaml,
    raw_sha256_digest,
)
from .catalog import NodeCatalog
from .catalog_schema import parse_schema, validate_value
from .value_path import ValuePathError, parse_value_path


SKILL_API_VERSION = "manyforge.skill.v2"
MAX_SKILLS = 64
MAX_SKILL_BYTES = 65_536
MAX_EXPANDED_NODES = 256
MAX_EXPANDED_BYTES = 262_144
MAX_SKILL_DEPTH = 2
_ID = re.compile(r"^[a-z][a-z0-9_.-]{0,127}$")
_REFERENCE = re.compile(r"^\$\{(param):([a-z][a-z0-9_.-]*)\}$")
_FIELDS = {
    "api_version",
    "skill_id",
    "version",
    "name",
    "description",
    "compatible_profiles",
    "parameters",
    "requires",
    "provides",
    "modifies",
    "body",
    "example",
}
_PROGRAM_MOTION_DEFAULTS = frozenset(
    {
        "velocity_scale",
        "acceleration_scale",
        "translation_tol_m",
        "blend_radius_m",
    }
)


class SkillValidationError(ValueError):
    """A skill or composition violates the portable, bounded contract."""


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise SkillValidationError(message)


def _identifier(value: Any) -> str:
    _require(
        type(value) is str and _ID.fullmatch(value) is not None,
        "skill identities must be bounded portable identifiers",
    )
    return value


def _bounded_document(value: Any) -> None:
    remaining = 8192

    def visit(item: Any, depth: int) -> None:
        nonlocal remaining
        remaining -= 1
        _require(remaining >= 0 and depth <= 32, "skill value exceeds structural bounds")
        if type(item) is dict:
            _require(len(item) <= 256, "skill mapping exceeds bound")
            for key, child in item.items():
                _require(type(key) is str and len(key) <= 256, "invalid skill field")
                visit(child, depth + 1)
        elif type(item) is list:
            _require(len(item) <= 256, "skill array exceeds bound")
            for child in item:
                visit(child, depth + 1)
        elif type(item) is str:
            _require(len(item) <= 8192, "skill text exceeds bound")
        else:
            _require(item is None or type(item) in (bool, int, float), "non-JSON skill value")

    visit(value, 0)
    _require(len(canonical_json_bytes(value)) <= MAX_SKILL_BYTES, "skill exceeds byte bound")


def _identified_program(value: dict[str, Any]) -> dict[str, Any]:
    value["program_revision"] = "sha256:" + "0" * 64
    value["program_revision"] = compute_content_identity(
        value,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v3",
    )
    return value


class SkillCatalog:
    """Immutable compilation input, with no environment/execution authority."""

    def __init__(
        self,
        artifacts: list[dict[str, Any]],
        *,
        catalog: NodeCatalog,
        tool_ids: frozenset[str] | set[str] = frozenset(),
    ) -> None:
        _require(
            type(artifacts) is list and len(artifacts) <= MAX_SKILLS,
            "skill catalog exceeds bound",
        )
        self._catalog = catalog
        self._skills: dict[str, dict[str, Any]] = {}
        self._parameters: dict[str, dict[str, tuple[dict, Any]]] = {}
        for artifact in artifacts:
            _bounded_document(artifact)
            _require(
                type(artifact) is dict
                and _FIELDS <= set(artifact)
                and not set(artifact) - _FIELDS - {"requires_tools", "tags"},
                "skill artifact has missing or unknown fields",
            )
            _require(
                artifact["api_version"] == SKILL_API_VERSION,
                "unsupported skill version",
            )
            skill_id = _identifier(artifact["skill_id"])
            _require(skill_id not in self._skills, "duplicate skill identity")
            for field in ("version", "name", "description"):
                _require(
                    type(artifact[field]) is str and bool(artifact[field]),
                    f"skill {field} must be non-empty text",
                )
            _require(
                artifact["compatible_profiles"] == ["composable-cell-v1"],
                "unsupported skill profile",
            )
            for field in ("requires", "provides", "modifies", "requires_tools", "tags"):
                rows = artifact.get(field, [])
                _require(
                    type(rows) is list
                    and len(rows) <= 64
                    and all(type(row) is str and row for row in rows)
                    and len(rows) == len(set(rows)),
                    f"invalid skill {field}",
                )
            _require(
                set(artifact.get("requires_tools", [])) <= tool_ids,
                "skill requires an unknown tool",
            )
            parameters = artifact["parameters"]
            _require(
                type(parameters) is list and len(parameters) <= 32,
                "invalid skill parameters",
            )
            parsed = {}
            for parameter in parameters:
                _require(
                    type(parameter) is dict
                    and {"id", "schema", "required"} <= set(parameter)
                    and not set(parameter) - {"id", "schema", "required", "default"},
                    "invalid skill parameter declaration",
                )
                key = _identifier(parameter["id"])
                _require(
                    key not in parsed and type(parameter["required"]) is bool,
                    "duplicate or invalid skill parameter",
                )
                schema = parse_schema(parameter["schema"], path=f"{skill_id}.{key}")
                catalog.schema_registry.resolve(schema)
                if "default" in parameter:
                    _require(
                        not validate_value(
                            schema,
                            parameter["default"],
                            registry=catalog.schema_registry,
                        ),
                        "invalid skill parameter default",
                    )
                parsed[key] = (deepcopy(parameter), schema)
            self._skills[skill_id] = deepcopy(artifact)
            self._parameters[skill_id] = parsed
        for skill_id, artifact in self._skills.items():
            self._check_body(artifact["body"], skill_id, (skill_id,), [MAX_EXPANDED_NODES])
        # Examples supply the declarations for typed values/scene references and
        # are compiled here, not merely exposed as decorative prose.
        for skill_id, artifact in self._skills.items():
            example = artifact["example"]
            _require(
                type(example) is dict
                and set(example) == {"arguments", "program_declarations", "effect"}
                and type(example["effect"]) is str
                and bool(example["effect"]),
                "skill requires one executable example",
            )
            declarations = example["program_declarations"]
            _require(
                type(declarations) is dict
                and not set(declarations)
                - {"parameters", "blackboard", "scene", "contact_policies"},
                "invalid skill example declarations",
            )
            host = {
                "api_version": "manyforge.program.v3",
                "program_id": "skill_example",
                "program_revision": "sha256:" + "0" * 64,
                "program_profile": "composable-cell-v1",
                "name": "Skill example",
                "catalog": {
                    "api_version": catalog.api_version,
                    "content_version": catalog.content_version,
                },
                "parameters": [],
                "scene": [],
                **deepcopy(declarations),
            }
            self._expand_checked(
                skill_id,
                example["arguments"],
                "example",
                host,
            )

    def artifacts(self) -> list[dict[str, Any]]:
        return [deepcopy(self._skills[key]) for key in sorted(self._skills)]

    def _check_body(
        self, node: Any, skill_id: str, stack: tuple[str, ...], budget: list[int]
    ) -> None:
        budget[0] -= 1
        _require(budget[0] >= 0 and type(node) is dict, "invalid or oversized skill body")
        _identifier(node.get("instance_id"))
        def check_reference(value: Any) -> None:
            if type(value) is str and "${" in value:
                match = _REFERENCE.fullmatch(value)
                _require(match is not None, "skill substitutions must occupy the whole value")
                kind, key = match.groups()
                declared = self._parameters[skill_id]
                _require(key in declared, "skill template references an undeclared value")
            elif type(value) is dict:
                for item in value.values():
                    check_reference(item)
            elif type(value) is list:
                for item in value:
                    check_reference(item)

        check_reference(node)
        if "skill_id" in node:
            _require(
                set(node)
                == {
                    "skill_id",
                    "instance_id",
                    "arguments",
                },
                "invalid nested skill invocation",
            )
            nested = node["skill_id"]
            _require(
                type(nested) is str
                and nested in self._skills
                and nested not in stack
                and len(stack) < MAX_SKILL_DEPTH,
                "unknown, cyclic or too-deep nested skill",
            )
            _require(
                type(node["arguments"]) is dict
                and not set(node["arguments"]) - set(self._parameters[nested]),
                "unknown nested skill parameter",
            )
            self._check_body(self._skills[nested]["body"], nested, (*stack, nested), budget)
            return
        _require(
            {"node_id", "instance_id", "parameters", "children"} <= set(node)
            and not set(node)
            - {
                "node_id",
                "instance_id",
                "parameters",
                "children",
                "value_sources",
                "metadata",
            },
            "invalid primitive skill template",
        )
        try:
            entry = self._catalog.get(node["node_id"])
        except (KeyError, TypeError) as error:
            raise SkillValidationError("unknown skill primitive") from error
        _require(
            type(node["parameters"]) is dict
            and type(node["children"]) is list
            and entry.children_min <= len(node["children"]) <= entry.children_max
            and type(node.get("value_sources", {})) is dict,
            "invalid skill node parameters/cardinality",
        )
        metadata = node.get("metadata", {})
        _require(
            type(metadata) is dict and "skill_expansions" not in metadata,
            "skill expansion provenance is generated, not supplied by the template",
        )
        try:
            source_roots = {
                str(parse_value_path(path)[0]) for path in node.get("value_sources", {})
            }
        except ValuePathError as error:
            raise SkillValidationError(f"invalid skill value-source path: {error}") from error
        supplied = set(node["parameters"]) | source_roots
        _require(
            supplied <= {parameter.id for parameter in entry.parameters},
            "unknown primitive parameter",
        )
        for child in node["children"]:
            self._check_body(child, skill_id, stack, budget)

    def expand(
        self,
        *,
        skill_id: str,
        arguments: dict[str, Any],
        instance_prefix: str,
        program_context: dict[str, Any],
    ) -> dict[str, Any]:
        # Declaration-only context works for an empty/incomplete Draft as well
        # as an existing Program; no throwaway runnable tree is required.
        _bounded_document(program_context)
        _require(
            type(program_context) is dict
            and not {
                "tree",
                "program_revision",
                "forest",
                "positions",
                "schema_version",
            }
            & set(program_context)
            and program_context.get("program_profile") == "composable-cell-v1",
            "skill expansion requires portable Program declarations, not a tree or binding",
        )
        host = deepcopy(program_context)
        return self._expand_checked(
            skill_id, arguments, instance_prefix, host
        )

    def _expand_checked(
        self,
        skill_id: str,
        arguments: dict,
        prefix: str,
        host: dict,
    ) -> dict[str, Any]:
        from .program import Program

        _bounded_document(
            {
                "arguments": arguments,
                "prefix": prefix,
            }
        )
        trace: list[dict[str, Any]] = []
        budget = [MAX_EXPANDED_NODES]
        rendered_items, rendered_bytes = 8192, MAX_EXPANDED_BYTES
        program_motion_defaults = _PROGRAM_MOTION_DEFAULTS & {
            declaration.get("parameter_id")
            for declaration in host.get("parameters", [])
            if type(declaration) is dict
        }

        def copy_value(value: Any, substitutions: dict | None = None) -> Any:
            nonlocal rendered_items, rendered_bytes
            if (
                substitutions is not None
                and type(value) is str
                and (match := _REFERENCE.fullmatch(value))
            ):
                kind, key = match.groups()
                _require(key in substitutions[kind], "missing skill substitution")
                # An argument is data, never another substitution template.
                return copy_value(substitutions[kind][key])
            rendered_items -= 1
            _require(rendered_items >= 0, "expanded skill exceeds value bound")
            if type(value) is dict:
                rendered_bytes -= 2 + len(value) * 2
                _require(rendered_bytes >= 0, "expanded skill exceeds byte bound")
                return {
                    copy_value(key): copy_value(item, substitutions) for key, item in value.items()
                }
            if type(value) is list:
                rendered_bytes -= 2 + len(value)
                _require(rendered_bytes >= 0, "expanded skill exceeds byte bound")
                return [copy_value(item, substitutions) for item in value]
            rendered_bytes -= len(canonical_json_bytes(value))
            _require(rendered_bytes >= 0, "expanded skill exceeds byte bound")
            return value

        def expand_skill(
            identity: str,
            values: dict,
            scope: str,
            stack: tuple[str, ...],
        ) -> dict:
            _identifier(identity)
            _identifier(scope)
            _require(
                identity in self._skills and identity not in stack and len(stack) < MAX_SKILL_DEPTH,
                "invalid skill selection or nesting",
            )
            artifact = self._skills[identity]
            _require(
                host["program_profile"] in artifact["compatible_profiles"],
                "skill excludes the Program profile",
            )
            params = self._parameters[identity]
            _require(
                type(values) is dict and not set(values) - set(params),
                "unknown skill arguments",
            )
            effective = {}
            for key, (parameter, schema) in params.items():
                if key not in values and "default" not in parameter:
                    _require(not parameter["required"], "missing skill argument")
                    continue
                value = values[key] if key in values else parameter["default"]
                _require(
                    not validate_value(schema, value, registry=self._catalog.schema_registry),
                    f"invalid skill argument {key}",
                )
                effective[key] = value  # copied only when emitted, under the shared output budget
            substitutions = {"param": effective}
            created = []

            def expand_node(template: dict) -> dict:
                budget[0] -= 1
                _require(budget[0] >= 0, "skill expansion exceeds node bound")
                # Each child is rendered once by its own expansion, not once
                # here and again recursively below.
                node = copy_value(
                    {key: value for key, value in template.items() if key != "children"},
                    substitutions,
                )
                instance = _identifier(f"{scope}.{node['instance_id']}")
                if "skill_id" in node:
                    expanded = expand_skill(
                        node["skill_id"],
                        node["arguments"],
                        instance,
                        (*stack, identity),
                    )
                    created.extend(trace[-1]["instance_ids"])
                    return expanded
                entry = self._catalog.get(node["node_id"])
                applicable_defaults = program_motion_defaults & {
                    parameter.id for parameter in entry.parameters
                }
                sources = node.setdefault("value_sources", {})
                for parameter_id in sorted(
                    applicable_defaults - set(node["parameters"]) - set(sources)
                ):
                    sources[parameter_id] = {
                        "kind": "program_parameter",
                        "reference": parameter_id,
                    }
                if sources:
                    node["value_sources"] = {
                        key: sources[key] for key in sorted(sources)
                    }
                else:
                    node.pop("value_sources", None)
                node["instance_id"] = copy_value(instance)
                node["children"] = [expand_node(child) for child in template["children"]]
                created.append(instance)
                return node

            subtree = expand_node(artifact["body"])
            provenance = {
                "skill_id": identity,
                "version": artifact["version"],
                "artifact_digest": raw_sha256_digest(canonical_json_bytes(artifact)),
                "arguments": deepcopy(effective),
                "instance_ids": sorted(created),
            }
            subtree.setdefault("metadata", {}).setdefault("skill_expansions", []).append(
                copy_value(provenance)
            )
            trace.append(copy_value(provenance))
            return subtree

        tree = expand_skill(skill_id, arguments, prefix, ())
        candidate = deepcopy(host)
        candidate["tree"] = tree
        Program.from_dict(_identified_program(candidate), self._catalog)
        return {
            "schema_version": "manyforge.skill_expansion.v2",
            "tree": tree,
            "trace": trace,
        }


def default_skills_dir() -> Path:
    return Path(__file__).resolve().parents[1] / "resources" / "skills"


def load_skill(path: str | Path) -> dict[str, Any]:
    source = Path(path)
    _require(
        not source.is_symlink() and source.is_file() and source.stat().st_size <= MAX_SKILL_BYTES,
        "skill must be one bounded plain file",
    )
    artifact = load_strict_json_yaml(source)
    _bounded_document(artifact)
    return artifact


def load_skill_catalog(
    skills_dir: str | Path,
    *,
    catalog: NodeCatalog,
    tool_ids: frozenset[str] | set[str] = frozenset(),
) -> SkillCatalog:
    directory = Path(skills_dir)
    _require(
        directory.is_dir() and not directory.is_symlink(),
        "invalid build-owned skill directory",
    )
    paths = sorted(directory.glob("*.skill.yaml"))
    _require(len(paths) <= MAX_SKILLS, "skill catalog exceeds artifact bound")
    artifacts = [load_skill(path) for path in paths]
    for path, artifact in zip(paths, artifacts, strict=True):
        _require(
            type(artifact) is dict and path.name == f"{artifact.get('skill_id')}.skill.yaml",
            "skill filename/identity mismatch",
        )
    return SkillCatalog(artifacts, catalog=catalog, tool_ids=tool_ids)
