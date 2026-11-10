"""Argument checks and catalog reads for the assistant tools.

The tree tools take one generic node shape. Each node is validated here, at
call time, against its own JSON Schema from the mode projection (generated
from the node catalog where the projection is built). A failure reports every
error of the closest alternative (bounded): the field path, the reason and
the expected shape of that field, written in the catalog's compact notation
for node parameters.
"""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
from typing import Any, Callable, Final, Mapping, Sequence

from jsonschema import Draft202012Validator
from jsonschema.exceptions import ValidationError

MAXIMUM_ERROR_TEXT: Final = 200
MAXIMUM_ERRORS: Final = 8
_SHAPE_DEPTH: Final = 2
VALUE_SOURCE_SHAPE: Final = (
    "{kind: program_parameter|blackboard_key|blackboard_field, reference, field?}"
)
PLACEMENT_FIELDS: Final = ("parent_instance_id", "after_instance_id", "before_instance_id")


@dataclass(frozen=True, slots=True)
class FieldError:
    """One typed argument failure: field path, expected shape and reason."""

    path: str
    expected: str | None
    reason: str
    message: str

    def text(self) -> str:
        if self.expected:
            return f"{self.path}: expected {self.expected} ({self.message})"
        return f"{self.path}: {self.message}"

    def row(self) -> dict[str, Any]:
        return {"path": self.path, "expected": self.expected, "reason": self.reason,
                "message": self.message}


def failure_detail(errors: Sequence[FieldError]) -> dict[str, Any]:
    """Typed detail of one rejected call: every error, the first one on top."""

    first = errors[0]
    return {
        "code": "assistant_tool_arguments_invalid",
        "path": first.path,
        "expected": first.expected,
        "reason": first.reason,
        "errors": [error.row() for error in errors],
    }


def failure_text(errors: Sequence[FieldError]) -> str:
    return "; ".join(error.text() for error in errors)


def field_path(prefix: str, tokens: Sequence[Any]) -> str:
    path = prefix
    for token in tokens:
        if type(token) is int:
            path += f"[{token}]"
        else:
            path = f"{path}.{token}" if path else str(token)
    return path or "$"


def _short(text: str) -> str:
    return text if len(text) <= MAXIMUM_ERROR_TEXT else text[: MAXIMUM_ERROR_TEXT - 1] + "…"


# --- JSON Schema walk ------------------------------------------------------------


def _deref(schema: Any, root: Mapping[str, Any]) -> Any:
    seen = 0
    while isinstance(schema, dict) and "$ref" in schema and seen < 16:
        reference = schema["$ref"]
        if not reference.startswith("#/$defs/"):
            break
        target = root.get("$defs", {}).get(reference.removeprefix("#/$defs/"))
        if target is None:
            break
        merged = {key: value for key, value in schema.items() if key != "$ref"}
        schema = {**target, **merged}
        seen += 1
    return schema


def _const_variants(alternatives: Sequence[Any], root: Mapping[str, Any]) -> str | None:
    """`kind a (no other field) | kind b + frame` for const-discriminated forms."""

    parts = []
    for alternative in alternatives:
        option = _deref(alternative, root)
        properties = option.get("properties") if isinstance(option, dict) else None
        if not isinstance(properties, dict):
            return None
        consts = [(n, _deref(p, root)) for n, p in properties.items()]
        discriminator = next(((n, p["const"]) for n, p in consts
                              if isinstance(p, dict) and "const" in p), None)
        if discriminator is None:
            return None
        others = [n for n in properties if n != discriminator[0]]
        parts.append(f"{discriminator[0]} {discriminator[1]} "
                     + ("+ " + ", ".join(others) if others else "(no other field)"))
    return " | ".join(parts) if parts else None


def json_shape(schema: Any, root: Mapping[str, Any], *, depth: int = 1) -> str | None:
    """Compact summary of a JSON Schema location (tool arguments)."""

    schema = _deref(schema, root)
    if not isinstance(schema, dict):
        return None
    if "const" in schema:
        return repr(schema["const"])
    if "enum" in schema:
        return "|".join(str(value) for value in schema["enum"])
    alternatives = schema.get("oneOf") or schema.get("anyOf")
    if alternatives and "properties" not in schema:
        variants = _const_variants(alternatives, root)
        if variants:
            return variants
    if alternatives and depth > 0 and "properties" not in schema and "type" not in schema:
        shapes = [json_shape(item, root, depth=depth - 1) for item in alternatives]
        return " | ".join(shape for shape in shapes if shape)
    kind = schema.get("type")
    if kind == "object" and isinstance(schema.get("properties"), dict):
        if depth <= 0:
            return "object"
        required = set(schema.get("required", []))
        return "{" + ", ".join(
            f"{name}{'' if name in required else '?'}: {json_shape(child, root, depth=depth - 1) or 'value'}"
            for name, child in schema["properties"].items()
        ) + "}"
    if kind == "array":
        item = json_shape(schema.get("items", {}), root, depth=depth - 1)
        return f"list of {item}" if item else "list"
    if kind == "string" and "pattern" in schema:
        return "identifier"
    return str(kind) if kind else None


def _closest(error: ValidationError) -> list[ValidationError]:
    """Descend a failed oneOf/anyOf into its closest alternative."""

    if error.validator not in ("oneOf", "anyOf") or not error.context:
        return [error]
    branches: dict[Any, list[ValidationError]] = {}
    for child in error.context:
        branches.setdefault(child.relative_schema_path[0], []).append(child)
    best = min(
        branches.values(),
        key=lambda rows: (len(rows), -max(len(row.absolute_path) for row in rows)),
    )
    result: list[ValidationError] = []
    for child in best:
        result.extend(_closest(child))
    return result


_PATH_PART = __import__("re").compile(r"([^.\[\]]+)|\[(\d+)\]")


def enclosing_shape(schema: Mapping[str, Any], type_name: str, path: str) -> tuple[str, str]:
    """The nearest enclosing object of an error path inside one typed value.

    Returns (field name or the type name, compact shape) of the deepest object
    with known properties that contains the failing field.
    """

    root = schema
    current: Any = _deref(schema, root)
    name, best = type_name, current
    for field_name, index in _PATH_PART.findall(path.lstrip(".")):
        if not isinstance(current, dict):
            break
        if index:
            current = _deref(current.get("items", {}), root)
            continue
        child = _deref((current.get("properties") or {}).get(field_name), root)
        if not isinstance(child, dict) or not child.get("properties"):
            break
        name, best, current = field_name, child, child
    return name, json_shape(best, root, depth=2) or "object"


Expected = Callable[[Sequence[Any], ValidationError], str | None]


# Text a model sends for an optional argument it means to leave out.
_NO_VALUE_TEXT: Final = frozenset({"", "None", "none", "null", "Null"})


def placeholder_errors(schema: Mapping[str, Any], arguments: Mapping[str, Any]) -> list[FieldError]:
    """An optional string argument given a "no value" placeholder: omit it.

    The placeholder may even match the field's pattern ("null"), so this
    runs before the schema check.
    """

    errors = []
    required = set(schema.get("required", []))
    for name, value in arguments.items():
        declared = schema.get("properties", {}).get(name)
        if name in required or not isinstance(value, str) or value not in _NO_VALUE_TEXT:
            continue
        if not isinstance(declared, dict) or not any(
            "pattern" in form for form in (declared, *declared.get("oneOf", []))
        ):
            continue  # only identifier fields; an enum may accept "none"
        description = declared.get("description")
        errors.append(FieldError(name, None, "placeholder", f"{value!r} is not a value: " + (
            description if isinstance(description, str) else f"omit {name}")))
    return errors


def _required_names(schema: Any) -> list[str]:
    return list(schema.get("required", [])) if isinstance(schema, Mapping) else []


def _describe_condition(condition: Any) -> str:
    """`kind is a|b`, from a {required, properties: {k: const|enum}} condition."""

    parts = []
    for name, value in (condition.get("properties") or {}).items() if isinstance(condition, Mapping) else []:
        if isinstance(value, Mapping) and "const" in value:
            parts.append(f"{name} is {value['const']}")
        elif isinstance(value, Mapping) and "enum" in value:
            parts.append(f"{name} is " + "|".join(str(item) for item in value["enum"]))
    return " and ".join(parts)


def _forbidden_form(error: ValidationError, root: Mapping[str, Any]) -> tuple[str, list[str]]:
    """A generated message for a `not` failure, and the field it names.

    The forms the instruments and the catalog schemas produce: a value set
    in both parameters and value_sources, mutually exclusive fields, fields
    allowed only under a condition, and forbidden alternatives.
    """

    forbidden = _deref(error.validator_value, root)
    if not isinstance(forbidden, Mapping):
        return "matches a forbidden form", []
    parts = [_deref(part, root) for part in forbidden.get("allOf", [])] or [forbidden]
    # A value both literal and bound: {parameters: {required: [x]}} + {value_sources: ...}.
    nested = {}
    for part in parts:
        for container in _required_names(part):
            inner = (part.get("properties") or {}).get(container)
            for name in _required_names(inner):
                nested.setdefault(name, []).append(container)
    for name, containers in nested.items():
        if len(containers) > 1:
            return (f"{name} is set in both {' and '.join(containers)}; set it in only one",
                    [containers[-1], name])
    names = [name for part in parts for name in _required_names(part)]
    alternatives = [name for part in forbidden.get("anyOf", []) for name in _required_names(_deref(part, root))]
    if alternatives:
        present = [name for name in alternatives
                   if isinstance(error.instance, Mapping) and name in error.instance]
        return (f"none of {', '.join(alternatives)} may be set here",
                [(present or alternatives)[0]])
    if len(names) > 1:
        return f"{' and '.join(names)} cannot both be set", [names[-1]]
    if len(names) == 1:
        # A field allowed only under a condition: allOf[i] = {if: {not: cond}, then: {not: ...}}.
        path = list(error.absolute_schema_path)
        if len(path) >= 2 and path[-2:] == ["then", "not"]:
            node: Any = root
            for token in path[:-2]:
                if token == "$ref":
                    node = _deref(node, root)
                    continue
                if isinstance(node, dict) and token not in node:
                    # The validator's path continues inside a $ref target
                    # without naming the $ref keyword.
                    node = _deref(node, root)
                node = node[token]
            condition = (_deref(node, root).get("if") or {}).get("not")
            described = _describe_condition(_deref(condition, root)) if condition else ""
            if described:
                return f"{names[0]} is allowed only when {described}", [names[0]]
        return f"{names[0]} is not allowed here", [names[0]]
    return "matches a forbidden form", []


def collect_errors(
    validator: Draft202012Validator,
    instance: Any,
    *,
    prefix: str = "",
    expected: Expected | None = None,
) -> list[FieldError]:
    """Every error of the closest alternative, as typed field errors."""

    root = validator.schema
    rows: list[FieldError] = []
    seen: set[tuple[str, str]] = set()
    for top in validator.iter_errors(instance):
        for error in _closest(top):
            location = _deref(error.schema, root) if isinstance(error.schema, dict) else {}
            if error.validator == "required" and isinstance(error.instance, dict):
                # One row per missing field, all in one result.
                for name in [n for n in error.validator_value if n not in error.instance]:
                    tokens = [*error.absolute_path, name]
                    shown = expected(tokens, error) if expected is not None else None
                    if shown is None:
                        shown = json_shape(location.get("properties", {}).get(name, {}), root)
                    row = FieldError(field_path(prefix, tokens), shown, "required",
                                     f"'{name}' is missing")
                    if (row.path, row.reason) not in seen:
                        seen.add((row.path, row.reason))
                        rows.append(row)
                continue
            tokens = list(error.absolute_path)
            message = error.message
            shape_schema: Any = location
            if error.validator == "additionalProperties" and isinstance(error.instance, dict):
                allowed = set(location.get("properties", {}))
                extra = sorted(set(error.instance) - allowed)
                if extra:
                    tokens.append(extra[0])
                    message = f"'{extra[0]}' is not accepted here"
                    shape_schema = None
            elif error.validator == "not":
                message, fields = _forbidden_form(error, root)
                tokens.extend(fields)
                shape_schema = None
            elif error.validator in ("enum", "const"):
                message = f"{_short(repr(error.instance))} is not an accepted value"
            elif error.validator in ("oneOf", "anyOf"):
                message = "matches none of the accepted forms" if not error.context else (
                    "matches more than one accepted form")
            else:
                message = _short(message)
            shown = expected(tokens, error) if expected is not None else None
            if shown is None and shape_schema is not None:
                shown = json_shape(shape_schema, root)
            if shown is None and error.validator == "additionalProperties":
                shown = "one of: " + ", ".join(sorted(location.get("properties", {}))) if location.get(
                    "properties") else "no fields"
            row = FieldError(field_path(prefix, tokens), shown, str(error.validator), message)
            key = (row.path, row.reason)
            if key not in seen:
                seen.add(key)
                rows.append(row)
    rows.sort(key=lambda row: row.path)
    return rows[:MAXIMUM_ERRORS]


# --- expected shapes in the catalog's compact notation -----------------------


def _type_name(schema: Mapping[str, Any]) -> str | None:
    reference = schema.get("schema_ref")
    return reference.rsplit("/", 1)[-1] if type(reference) is str else None


def _resolve(schema: Mapping[str, Any], types: Mapping[str, Any]) -> Mapping[str, Any]:
    seen: set[str] = set()
    while (name := _type_name(schema)) is not None and name not in seen:
        seen.add(name)
        target = types.get(name)
        if type(target) is not dict:
            break
        schema = target
    return schema


def _when_values(condition: Any, field: str) -> set[str] | None:
    if not isinstance(condition, dict) or field not in condition:
        return None
    value = condition[field]
    return {str(item) for item in value} if isinstance(value, list) else {str(value)}


def variant_rule(concrete: Mapping[str, Any], types: Mapping[str, Any]) -> str | None:
    """`kind a (no other field) | kind b + frame {...}` for a kind-dependent type.

    Generated from the catalog's required_when / allowed_when on the type's
    enum discriminator; None when the type has no such discriminator.
    """

    properties = concrete.get("properties")
    if not isinstance(properties, dict):
        return None
    for field, child in properties.items():
        resolved = _resolve(child, types) if isinstance(child, dict) else {}
        if resolved.get("type") != "enum":
            continue
        values = [str(value) for value in resolved.get("values", [])]
        dependent = {
            name: (
                _when_values(other.get("required_when"), field),
                _when_values(other.get("allowed_when"), field),
            )
            for name, other in properties.items()
            if name != field and isinstance(other, dict)
        }
        if not any(required or allowed for required, allowed in dependent.values()):
            continue
        variants = []
        for value in values:
            parts = []
            for name, (required, allowed) in dependent.items():
                permitted = allowed if allowed is not None else required
                if permitted is not None and value not in permitted:
                    continue
                optional = required is None or value not in required
                parts.append(
                    f"{name}{'?' if optional else ''} "
                    f"{compact_shape(properties[name], types, depth=1)}"
                )
            variants.append(
                f"{field} {value} " + ("+ " + ", ".join(parts) if parts else "(no other field)")
            )
        return " | ".join(variants)
    return None


def compact_shape(
    schema: Mapping[str, Any], types: Mapping[str, Any], *, depth: int = _SHAPE_DEPTH
) -> str:
    """One-line summary of a catalog schema, e.g. ``coordinate_position {kind: a|b, ...}``.

    A kind-dependent type shows its variant rule instead of a field list.
    """

    name = _type_name(schema)
    concrete = _resolve(schema, types)
    kind = concrete.get("type")
    rule = variant_rule(concrete, types) if depth >= _SHAPE_DEPTH and kind == "object" else None
    if rule is not None:
        return f"{name}: {rule}" if name else rule
    if kind == "enum":
        body = "|".join(str(value) for value in concrete.get("values", []))
    elif kind == "object" and isinstance(concrete.get("properties"), dict):
        if depth <= 0:
            body = "{…}"
        else:
            required = set(concrete.get("required", []))
            fields = []
            for field, child in concrete["properties"].items():
                child_shape = (
                    compact_shape(child, types, depth=depth - 1)
                    if isinstance(child, dict)
                    else "?"
                )
                conditional = isinstance(child, dict) and (
                    "required_when" in child or "allowed_when" in child
                )
                marker = "" if field in required and not conditional else "?"
                fields.append(f"{field}{marker}: {child_shape}")
            body = "{" + ", ".join(fields) + "}"
    elif kind == "array" and isinstance(concrete.get("items"), dict):
        body = "list of " + compact_shape(concrete["items"], types, depth=depth - 1)
    else:
        body = str(kind) if kind is not None else "value"
    if name is None:
        return body
    return name if depth < _SHAPE_DEPTH and kind in {"object", "array"} else f"{name} {body}"


def _parameter_schema(entry: Mapping[str, Any], parameter_id: str) -> Mapping[str, Any] | None:
    for parameter in entry.get("parameters", []):
        if isinstance(parameter, dict) and parameter.get("id") == parameter_id:
            if "schema_ref" in parameter:
                return {"schema_ref": parameter["schema_ref"]}
            schema = parameter.get("schema")
            return schema if isinstance(schema, dict) else None
    return None


def expected_shape(
    catalog: Mapping[str, Any], node_id: str, tokens: Sequence[Any]
) -> str | None:
    """Expected shape of the field at ``tokens`` inside one node, if known."""

    types = catalog.get("schema_types") or {}
    entries = {
        row.get("id"): row for row in catalog.get("entries", []) if isinstance(row, dict)
    }
    entry = entries.get(node_id)
    if entry is None or len(tokens) < 2 or tokens[0] != "parameters":
        return None
    schema = _parameter_schema(entry, str(tokens[1]))
    for token in tokens[2:]:
        if schema is None:
            return None
        concrete = _resolve(schema, types)
        if type(token) is int:
            child = concrete.get("items") if isinstance(concrete.get("items"), dict) else None
        else:
            properties = concrete.get("properties")
            child = properties.get(token) if isinstance(properties, dict) else None
        if child is None:
            # An unknown field: the enclosing type's shape is what is expected.
            return compact_shape(schema, types)
        schema = child
    return compact_shape(schema, types) if schema is not None else None


# --- per-node validation -------------------------------------------------------


ValidatorFor = Callable[[Mapping[str, Any]], Draft202012Validator]


def value_source_ids(node_schema: Mapping[str, Any]) -> list[str]:
    sources = _deref(node_schema.get("properties", {}).get("value_sources", {}), node_schema)
    return sorted(sources.get("properties", {})) if isinstance(sources, dict) else []


def normalize_tree_arguments(tool_id: str, arguments: dict[str, Any]) -> None:
    """In place: omitted `parameters` and `children` are empty, and an empty
    `value_sources` is absent. Required parameters are still enforced by the
    node's own schema and the Draft diagnostics."""

    def node(value: Any, *, tree: bool) -> None:
        if not isinstance(value, dict):
            return
        value.setdefault("parameters", {})
        if value.get("value_sources") == {}:
            del value["value_sources"]
        if tree:
            value.setdefault("children", [])
            for child in value["children"] if isinstance(value["children"], list) else []:
                node(child, tree=True)

    if tool_id == "tree_draft_insert_node":
        node(arguments.get("node"), tree=True)
    elif tool_id == "tree_draft_replace_subtree":
        node(arguments.get("subtree"), tree=True)
    elif tool_id == "tree_draft_wrap_node":
        wrapper = arguments.get("wrapper")
        node(wrapper, tree=False)
        # The wrapped node becomes the only child; an empty list says the same.
        if isinstance(wrapper, dict) and wrapper.get("children") == []:
            del wrapper["children"]
    elif tool_id == "tree_draft_change_node_kind":
        node(arguments.get("replacement"), tree=False)
    elif tool_id == "tree_draft_update_node_params" and arguments.get("value_sources") == {}:
        del arguments["value_sources"]


def placement_errors(tool_id: str, arguments: Mapping[str, Any]) -> list[FieldError]:
    """The insert placement rule, stated instead of an opaque oneOf failure."""

    if tool_id != "tree_draft_insert_node":
        return []
    given = [name for name in PLACEMENT_FIELDS if name in arguments]
    rule = ("exactly one of parent_instance_id (with optional index), "
            "after_instance_id, before_instance_id")
    if len(given) != 1:
        return [FieldError("placement", rule, "placement",
                           "got " + (" and ".join(given) if given else "none of them"))]
    if "index" in arguments and given[0] != "parent_instance_id":
        return [FieldError("placement", rule, "placement",
                           f"index is only valid with parent_instance_id, got {given[0]}")]
    return []


class NodeCallChecker:
    """Validate the node payloads of one tree-tool call against their schemas."""

    def __init__(
        self,
        nodes: Sequence[Mapping[str, Any]],
        catalog: Mapping[str, Any],
        validator_for: ValidatorFor,
    ) -> None:
        self._nodes = {row["node_id"]: row for row in nodes}
        self._catalog = catalog
        self._validator_for = validator_for

    def check(self, tool_id: str, arguments: Mapping[str, Any]) -> list[FieldError]:
        if tool_id == "tree_draft_insert_node":
            return self._tree(arguments["node"], "node")
        if tool_id == "tree_draft_replace_subtree":
            return self._tree(arguments["subtree"], "subtree")
        if tool_id == "tree_draft_wrap_node":
            wrapper = arguments["wrapper"]
            return self._one(wrapper["node_id"], "node_schema", wrapper, "wrapper")
        if tool_id == "tree_draft_change_node_kind":
            replacement = arguments["replacement"]
            return self._one(
                replacement["node_id"], "replacement_schema", replacement, "replacement"
            )
        if tool_id == "tree_draft_update_node_params":
            patch = {
                field: arguments[field]
                for field in ("parameters", "value_sources")
                if field in arguments
            }
            return self._one(arguments["expected_node_id"], "update_schema", patch, "")
        return []

    def _expected(self, node_id: str, schema: Mapping[str, Any]) -> Expected:
        def shown(tokens: Sequence[Any], error: ValidationError) -> str | None:
            if tokens[:1] == ["value_sources"] or tokens == ["value_sources"]:
                sources = value_source_ids(schema)
                if not sources:
                    return "no value_sources: this node takes literal parameters only"
                return (f"value_sources binds a parameter to a Program parameter or blackboard "
                        f"key: {{<parameter>: {VALUE_SOURCE_SHAPE}}} for: {', '.join(sources)}")
            if tokens[:1] == ["parameters"] and error.validator == "additionalProperties" \
                    and len(tokens) == 2:
                entry = next((row for row in self._catalog.get("entries", [])
                              if isinstance(row, dict) and row.get("id") == node_id), {})
                names = [p.get("id") for p in entry.get("parameters", [])]
                return "one of the node's parameters: " + ", ".join(names)
            return expected_shape(self._catalog, node_id, tokens)
        return shown

    def _one(
        self, node_id: str, schema_field: str, instance: Mapping[str, Any], prefix: str
    ) -> list[FieldError]:
        schema = self._nodes[node_id][schema_field]
        return collect_errors(
            self._validator_for(schema), dict(instance), prefix=prefix,
            expected=self._expected(node_id, schema),
        )

    def _tree(self, node: Mapping[str, Any], prefix: str) -> list[FieldError]:
        node_id = node["node_id"]
        configuration = {key: value for key, value in node.items() if key != "children"}
        errors = self._one(node_id, "node_schema", configuration, prefix)
        bounds = self._nodes[node_id]["children"]
        children = node.get("children", [])
        # Too few children is a Draft diagnostic (authoring_node_children_
        # incomplete); more than the maximum the Draft refuses as well.
        if len(children) > bounds["max"]:
            expected = (
                "no children"
                if bounds["max"] == 0
                else f"at most {bounds['max']} children"
            )
            message = f"{node_id} has {len(children)} children"
            if bounds["max"] == 1:
                message += (f"; to run several steps under {node.get('instance_id')}, put them "
                            "in a sequence and make that its child")
            errors.append(FieldError(f"{prefix}.children", expected, "children_count", message))
        for index, child in enumerate(children):
            if len(errors) >= MAXIMUM_ERRORS:
                break
            errors.extend(self._tree(child, f"{prefix}.children[{index}]"))
        return errors[:MAXIMUM_ERRORS]


# --- catalog_read --------------------------------------------------------------


def _referenced_types(value: Any, types: Mapping[str, Any], found: dict[str, Any]) -> None:
    if isinstance(value, dict):
        name = _type_name(value)
        if name is not None and name not in found and isinstance(types.get(name), dict):
            found[name] = deepcopy(types[name])
            _referenced_types(types[name], types, found)
        for item in value.values():
            _referenced_types(item, types, found)
    elif isinstance(value, list):
        for item in value:
            _referenced_types(item, types, found)


def kind_shapes(
    catalog: Mapping[str, Any], nodes: Sequence[Mapping[str, Any]]
) -> dict[str, list[str]]:
    """Shape of every catalog kind that no ``schema_types`` entry names.

    Scalars and generated grammars (``component_interrupt_condition``,
    ``key_value``) appear by kind name in parameter rows and expected shapes;
    their shape is read from the node schemas the validator uses, at every
    place the catalog uses the kind. One shape per distinct use.
    """

    types = catalog.get("schema_types") or {}
    generated = {row.get("node_id"): row.get("node_schema") for row in nodes}
    found: dict[str, list[str]] = {}

    def walk(source: Any, schema: Any, root: Mapping[str, Any], named: frozenset[str]) -> None:
        if not isinstance(source, Mapping):
            return
        name = _type_name(source)
        if name is not None:
            if name in named:
                return
            named, source = named | {name}, _resolve(source, types)
        schema = _deref(schema, root)
        if not isinstance(schema, Mapping):
            return
        kind = source.get("type")
        if kind == "object":
            properties = schema.get("properties") or {}
            for field, child in (source.get("properties") or {}).items():
                walk(child, properties.get(field), root, named)
            walk(source.get("additional_properties"), schema.get("additionalProperties"),
                 root, named)
        elif kind == "array":
            walk(source.get("items"), schema.get("items"), root, named)
        elif isinstance(kind, str) and kind != "enum":
            shape = json_shape(schema, root, depth=6)
            if shape and shape not in found.setdefault(kind, []):
                found[kind].append(shape)

    for entry in catalog.get("entries", []):
        root = generated.get(entry.get("id")) if isinstance(entry, dict) else None
        if not isinstance(root, Mapping):
            continue
        parameters = _deref((root.get("properties") or {}).get("parameters"), root)
        properties = parameters.get("properties") or {} if isinstance(parameters, Mapping) else {}
        for parameter in entry.get("parameters", []):
            if isinstance(parameter, dict):
                walk(_parameter_schema(entry, parameter.get("id")),
                     properties.get(parameter.get("id")), root, frozenset())
    return found


_ENTRY_FIELDS: Final = ("id", "name", "category", "kind", "stability", "description", "children")


def catalog_rows(
    catalog: Mapping[str, Any],
    *,
    entry_ids: set[str],
    category: str | None,
    kind: str | None,
    include_parameters: bool,
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    """Selected entries and, with parameters, every schema type they use."""

    types = catalog.get("schema_types") or {}
    rows: list[dict[str, Any]] = []
    for source in catalog.get("entries", []):
        if not isinstance(source, dict):
            continue
        if entry_ids and source.get("id") not in entry_ids:
            continue
        if category is not None and source.get("category") != category:
            continue
        if kind is not None and source.get("kind") != kind:
            continue
        row = {field: deepcopy(source[field]) for field in _ENTRY_FIELDS if field in source}
        if include_parameters:
            row["parameters"] = deepcopy(source.get("parameters", []))
        rows.append(row)
    referenced: dict[str, Any] = {}
    if include_parameters:
        _referenced_types([row.get("parameters") for row in rows], types, referenced)
    return rows, dict(sorted(referenced.items()))


__all__ = [
    "FieldError",
    "NodeCallChecker",
    "catalog_rows",
    "collect_errors",
    "compact_shape",
    "expected_shape",
    "failure_detail",
    "failure_text",
    "field_path",
    "kind_shapes",
    "normalize_tree_arguments",
    "placeholder_errors",
    "placement_errors",
    "value_source_ids",
    "VALUE_SOURCE_SHAPE",
]
