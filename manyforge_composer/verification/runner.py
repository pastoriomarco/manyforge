"""Artifact support and typed boundary for qualification scenarios.

The complete HTTP/runtime runner is pinned at
manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
d7a21f8250d2c7fb3f6cb31e1261525c8d787de0. It is design input, not a
runnable fallback.

Retained here:

* strict JSON-model YAML loading into scenario/matrix value objects;
* bounded, workspace-contained matrix expansion;
* payload-path resolution and owner-neutral expectation matching;
* deterministic hashes and Git revision evidence; and
* bounded JSON artifact and archive packaging.

Removed ScenarioRunner methods are assigned as follows:

* HTTP step execution, Program load, signal publication, cycle control, and
  condition endpoint fetching return as thin clients;
* execution/Stop assertions return against C++ owner facts;
* scene/viewer assertions return later; and
* Isaac and real-robot scenario selection returns later.

Historical test blob 947c44fb50d55d0f9ab018acb600966faf417b18 contains
six acceptance cases. Matrix repeat/tag expansion, Git metadata, JSON output,
and archive packaging are retained in focused tests. Pick/place, visual-scene,
cycle, snapshot, and runtime endpoint assertions remain deferred.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace
import gzip
import hashlib
import json
import math
import os
from pathlib import Path
import re
import stat
import subprocess
import tarfile
import tempfile
from typing import Any, Final, NoReturn

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    StrictJsonYamlLoader,
    load_strict_json_yaml_bytes,
)


UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
# Scenario/evidence values use the repository's bounded JSON-model pattern,
# with a tighter 1 MiB authored-YAML envelope and a 64 MiB run-directory cap.
MAX_YAML_DOCUMENT_BYTES: Final = 1024 * 1024
MAX_DOCUMENT_DEPTH: Final = 32
MAX_DOCUMENT_NODES: Final = 16_384
MAX_SCALAR_BYTES: Final = 64 * 1024
MAX_PATH_BYTES: Final = 4096
MAX_SCENARIO_STEPS: Final = 256
MAX_SCENARIO_CONDITIONS: Final = 256
MAX_SCENARIO_TAGS: Final = 64
MAX_MATRIX_SCENARIOS: Final = 256
MAX_SCENARIO_REPEAT: Final = 256
MAX_EXPANDED_SCENARIOS: Final = 1024
MAX_FINAL_ARTIFACTS: Final = 64
MAX_EVIDENCE_DOCUMENT_BYTES: Final = 16 * 1024 * 1024
MAX_EVIDENCE_FILE_BYTES: Final = 16 * 1024 * 1024
MAX_OUTPUT_FILES: Final = 128
MAX_OUTPUT_BYTES: Final = 64 * 1024 * 1024
MAX_ARCHIVE_OUTPUT_BYTES: Final = 72 * 1024 * 1024
MAX_OUTPUT_NAME_BYTES: Final = 240
MAX_PAYLOAD_PATH_TOKENS: Final = 64
MAX_SAFE_INTEGER: Final = 9_007_199_254_740_991
_ARTIFACT_ID_RE: Final = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}")
_PATH_TOKEN_RE: Final = re.compile(r"([^\[\]]+)|\[(\d+)\]")
_OUTPUT_JSON_NAME_RE: Final = re.compile(
    r"[A-Za-z0-9][A-Za-z0-9_.-]*\.json"
)
_ARCHIVE_NAME_RE: Final = re.compile(
    r"[A-Za-z0-9][A-Za-z0-9_.-]{0,127}\.tar\.gz"
)
_EXPECTATION_OPERATORS: Final = (
    "equals",
    "not_equals",
    "greater_equal",
    "less_equal",
    "contains",
    "truthy",
)
_OUTPUT_MARKER_NAME: Final = ".manyforge-scenario-artifacts.v1"
_OUTPUT_MARKER_TEXT: Final = "manyforge.scenario-artifacts.v1\n"
_MISSING: Final = object()


class ScenarioExecutionError(RuntimeError):
    """Raised when a retained scenario artifact is invalid."""


class ScenarioExecutionUnavailableError(ScenarioExecutionError):
    """Raised before any historical runtime client or endpoint is used."""


class ScenarioAssertionError(AssertionError):
    """Historical assertion type retained for future thin clients."""


class _BoundedScenarioLoader(StrictJsonYamlLoader):
    """Strict JSON-model YAML loader with the tighter scenario envelope.

    Scalars resolve by JSON rules, so a JSON number such as ``-4e-05`` is a
    number, never the string YAML 1.1 implicit typing would make of it.
    """

    max_document_depth = MAX_DOCUMENT_DEPTH
    max_document_nodes = MAX_DOCUMENT_NODES
    max_scalar_bytes = MAX_SCALAR_BYTES


@dataclass(slots=True)
class ScenarioDefinition:
    """Structured, non-executable scenario artifact."""

    scenario_id: str
    description: str = ""
    program: str | None = None
    deployment: str | None = None
    timeouts: dict[str, Any] = field(default_factory=dict)
    steps: list[dict[str, Any]] = field(default_factory=list)
    assertions: list[dict[str, Any]] = field(default_factory=list)
    forbidden: list[dict[str, Any]] = field(default_factory=list)
    tags: list[str] = field(default_factory=list)
    source_path: Path | None = None


@dataclass(slots=True)
class ScenarioMatrixDefinition:
    """Collection of non-executable qualification scenario references."""

    matrix_id: str
    description: str = ""
    scenarios: list[dict[str, Any]] = field(default_factory=list)
    source_path: Path | None = None


def _scalar_bytes(value: str, label: str) -> bytes:
    if len(value) > MAX_SCALAR_BYTES:
        raise ValueError(
            f"{label} exceeds the {MAX_SCALAR_BYTES}-byte scalar bound"
        )
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError(f"{label} is not valid Unicode") from exc
    if len(encoded) > MAX_SCALAR_BYTES:
        raise ValueError(
            f"{label} exceeds the {MAX_SCALAR_BYTES}-byte scalar bound"
        )
    return encoded


def _bounded_string(
    value: Any,
    *,
    label: str,
    allow_empty: bool = True,
    max_bytes: int = MAX_SCALAR_BYTES,
) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    if len(value) > max_bytes:
        raise ValueError(f"{label} exceeds the {max_bytes}-byte bound")
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise ValueError(f"{label} is not valid Unicode") from exc
    if len(encoded) > max_bytes:
        raise ValueError(f"{label} exceeds the {max_bytes}-byte bound")
    if not allow_empty and not value:
        raise ValueError(f"{label} must not be empty")
    return value


def _validate_json_model(
    value: Any,
    label: str,
    *,
    collection_depth: int = 0,
    budget: list[int] | None = None,
) -> None:
    """Validate one bounded JSON-model value before comparison or encoding."""

    if budget is None:
        budget = [MAX_DOCUMENT_NODES]
    budget[0] -= 1
    if budget[0] < 0:
        raise ValueError(
            f"{label} contains more than {MAX_DOCUMENT_NODES} values"
        )
    if value is None or isinstance(value, (bool, str, int)):
        if isinstance(value, str):
            _scalar_bytes(value, label)
        elif isinstance(value, int) and not isinstance(value, bool):
            if abs(value) > MAX_SAFE_INTEGER:
                raise ValueError(
                    f"{label} integer exceeds the I-JSON safe range"
                )
        return
    if isinstance(value, float):
        if not math.isfinite(value):
            raise ValueError(f"{label} contains a non-finite number")
        if value.is_integer() and abs(value) > MAX_SAFE_INTEGER:
            raise ValueError(
                f"{label} integral number exceeds the I-JSON safe range"
            )
        return
    if isinstance(value, list):
        if collection_depth >= MAX_DOCUMENT_DEPTH:
            raise ValueError(
                f"{label} nesting exceeds {MAX_DOCUMENT_DEPTH} collections"
            )
        for index, item in enumerate(value):
            _validate_json_model(
                item,
                f"{label}[{index}]",
                collection_depth=collection_depth + 1,
                budget=budget,
            )
        return
    if isinstance(value, dict):
        if collection_depth >= MAX_DOCUMENT_DEPTH:
            raise ValueError(
                f"{label} nesting exceeds {MAX_DOCUMENT_DEPTH} collections"
            )
        for key, item in value.items():
            if not isinstance(key, str):
                raise ValueError(f"{label} mapping keys must be strings")
            budget[0] -= 1
            if budget[0] < 0:
                raise ValueError(
                    f"{label} contains more than {MAX_DOCUMENT_NODES} values"
                )
            _scalar_bytes(key, f"{label} key")
            _validate_json_model(
                item,
                f"{label}.{key[:80]}",
                collection_depth=collection_depth + 1,
                budget=budget,
            )
        return
    raise ValueError(
        f"{label} contains non-JSON value type {type(value).__name__}"
    )


def _bounded_yaml_mapping(path: str | Path) -> tuple[Path, dict[str, Any]]:
    raw_path = _bounded_path(path, label="scenario artifact path")
    try:
        artifact_path = raw_path.resolve(strict=True)
        if not artifact_path.is_file():
            raise ValueError(f"{artifact_path} is not a regular file")
        with artifact_path.open("rb") as stream:
            encoded = stream.read(MAX_YAML_DOCUMENT_BYTES + 1)
    except (OSError, RuntimeError) as exc:
        raise ValueError(f"cannot read scenario artifact {raw_path}: {exc}") from exc
    if len(encoded) > MAX_YAML_DOCUMENT_BYTES:
        raise ValueError(
            f"{artifact_path} exceeds the "
            f"{MAX_YAML_DOCUMENT_BYTES}-byte YAML bound"
        )
    try:
        raw = load_strict_json_yaml_bytes(
            encoded,
            source=str(artifact_path),
            loader=_BoundedScenarioLoader,
        )
    except StrictArtifactError as exc:
        raise ValueError(f"{artifact_path} has invalid bounded YAML: {exc}") from exc
    mapping = _mapping_document(raw, artifact_path)
    _validate_json_model(mapping, str(artifact_path))
    return artifact_path, mapping


def _mapping_document(raw: Any, source_path: Path) -> dict[str, Any]:
    if raw is None:
        return {}
    if not isinstance(raw, dict):
        raise ValueError(f"{source_path} must contain one YAML mapping")
    return raw


def _artifact_id(value: Any, *, field_name: str, source_path: Path) -> str:
    if not isinstance(value, str):
        raise ValueError(
            f"{source_path} has invalid {field_name!r}; expected a string"
        )
    bounded_value = _bounded_string(
        value,
        label=f"{source_path} {field_name} source",
        max_bytes=256,
    )
    artifact_id = _bounded_string(
        bounded_value.strip(),
        label=f"{source_path} {field_name}",
        allow_empty=False,
    )
    if not _ARTIFACT_ID_RE.fullmatch(artifact_id):
        raise ValueError(
            f"{source_path} has invalid {field_name!r}; expected 1-128 "
            "letters, digits, '.', '_', or '-'"
        )
    return artifact_id


def _optional_string(value: Any, *, label: str) -> str | None:
    if value is None:
        return None
    result = _bounded_string(value, label=label)
    return result if result else None


def _mapping_field(
    raw: dict[str, Any],
    field_name: str,
    *,
    source_path: Path,
) -> dict[str, Any]:
    value = raw.get(field_name)
    if value is None:
        return {}
    if not isinstance(value, dict):
        raise ValueError(f"{source_path} field {field_name!r} must be a mapping")
    return dict(value)


def _mapping_list_field(
    raw: dict[str, Any],
    field_name: str,
    *,
    source_path: Path,
    max_items: int,
) -> list[dict[str, Any]]:
    value = raw.get(field_name)
    if value is None:
        return []
    if not isinstance(value, list):
        raise ValueError(f"{source_path} field {field_name!r} must be a list")
    if len(value) > max_items:
        raise ValueError(
            f"{source_path} field {field_name!r} exceeds {max_items} items"
        )
    result: list[dict[str, Any]] = []
    for index, item in enumerate(value):
        if not isinstance(item, dict):
            raise ValueError(
                f"{source_path} field {field_name!r}[{index}] must be a mapping"
            )
        result.append(dict(item))
    return result


def _string_list(
    value: Any,
    *,
    label: str,
    max_items: int,
) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list):
        raise ValueError(f"{label} must be a list")
    if len(value) > max_items:
        raise ValueError(f"{label} exceeds {max_items} items")
    return [
        _bounded_string(item, label=f"{label}[{index}]", allow_empty=False)
        for index, item in enumerate(value)
    ]


def _bounded_repeat(value: Any, *, label: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{label} must be an integer")
    if value <= 0:
        raise ValueError(f"{label} must be positive")
    if value > MAX_SCENARIO_REPEAT:
        raise ValueError(
            f"{label} exceeds the repeat bound {MAX_SCENARIO_REPEAT}"
        )
    return value


def _normalize_matrix_item(
    item: Any,
    *,
    source_path: Path,
    index: int,
) -> dict[str, Any]:
    label = f"{source_path} scenarios[{index}]"
    if isinstance(item, str):
        path = _bounded_string(
            item,
            label=f"{label}.path",
            allow_empty=False,
            max_bytes=MAX_PATH_BYTES,
        )
        return {"path": path, "repeat": 1, "tags": []}
    if not isinstance(item, dict):
        raise ValueError(f"{label} must be a path string or mapping")
    path = _bounded_string(
        item.get("path"),
        label=f"{label}.path",
        allow_empty=False,
        max_bytes=MAX_PATH_BYTES,
    )
    repeat = _bounded_repeat(item.get("repeat", 1), label=f"{label}.repeat")
    tags = _string_list(
        item.get("tags"),
        label=f"{label}.tags",
        max_items=MAX_SCENARIO_TAGS,
    )
    return {"path": path, "repeat": repeat, "tags": tags}


def load_scenario_definition(path: str | Path) -> ScenarioDefinition:
    """Load one scenario YAML artifact without executing its steps."""

    scenario_path, raw = _bounded_yaml_mapping(path)
    scenario_id = _artifact_id(
        raw.get("scenario_id"),
        field_name="scenario_id",
        source_path=scenario_path,
    )
    steps = _mapping_list_field(
        raw,
        "steps",
        source_path=scenario_path,
        max_items=MAX_SCENARIO_STEPS,
    )
    assertions = _mapping_list_field(
        raw,
        "assertions",
        source_path=scenario_path,
        max_items=MAX_SCENARIO_CONDITIONS,
    )
    forbidden = _mapping_list_field(
        raw,
        "forbidden",
        source_path=scenario_path,
        max_items=MAX_SCENARIO_CONDITIONS,
    )
    if len(assertions) + len(forbidden) > MAX_SCENARIO_CONDITIONS:
        raise ValueError(
            f"{scenario_path} exceeds the combined condition bound "
            f"{MAX_SCENARIO_CONDITIONS}"
        )
    description = raw.get("description")
    if description is None:
        description = ""
    description = _bounded_string(
        description,
        label=f"{scenario_path} description",
    )
    return ScenarioDefinition(
        scenario_id=scenario_id,
        description=description,
        program=_optional_string(
            raw.get("program"),
            label=f"{scenario_path} program",
        ),
        deployment=_optional_string(
            raw.get("deployment"),
            label=f"{scenario_path} deployment",
        ),
        timeouts=_mapping_field(raw, "timeouts", source_path=scenario_path),
        steps=steps,
        assertions=assertions,
        forbidden=forbidden,
        tags=_string_list(
            raw.get("tags"),
            label=f"{scenario_path} tags",
            max_items=MAX_SCENARIO_TAGS,
        ),
        source_path=scenario_path,
    )


def load_matrix_definition(path: str | Path) -> ScenarioMatrixDefinition:
    """Load one matrix YAML artifact without loading referenced scenarios."""

    matrix_path, raw = _bounded_yaml_mapping(path)
    matrix_id = _artifact_id(
        raw.get("matrix_id"),
        field_name="matrix_id",
        source_path=matrix_path,
    )
    raw_scenarios = raw.get("scenarios")
    if not isinstance(raw_scenarios, list):
        raise ValueError(f"{matrix_path} field 'scenarios' must be a list")
    if len(raw_scenarios) > MAX_MATRIX_SCENARIOS:
        raise ValueError(
            f"{matrix_path} exceeds the matrix scenario bound "
            f"{MAX_MATRIX_SCENARIOS}"
        )
    scenarios: list[dict[str, Any]] = []
    expanded_count = 0
    for index, item in enumerate(raw_scenarios):
        normalized = _normalize_matrix_item(
            item,
            source_path=matrix_path,
            index=index,
        )
        repeat = normalized["repeat"]
        if repeat > MAX_EXPANDED_SCENARIOS - expanded_count:
            raise ValueError(
                f"{matrix_path} exceeds the expanded scenario bound "
                f"{MAX_EXPANDED_SCENARIOS}"
            )
        expanded_count += repeat
        scenarios.append(normalized)
    if not scenarios:
        raise ValueError(f"{matrix_path} does not declare any scenarios")
    description = raw.get("description")
    if description is None:
        description = ""
    return ScenarioMatrixDefinition(
        matrix_id=matrix_id,
        description=_bounded_string(
            description,
            label=f"{matrix_path} description",
        ),
        scenarios=scenarios,
        source_path=matrix_path,
    )


def _contained_path(root: Path, raw_path: str) -> Path:
    raw_path = _bounded_string(
        raw_path,
        label="scenario path",
        allow_empty=False,
        max_bytes=MAX_PATH_BYTES,
    )
    candidate = Path(raw_path)
    try:
        resolved = (
            candidate.resolve(strict=True)
            if candidate.is_absolute()
            else (root / candidate).resolve(strict=True)
        )
    except (OSError, RuntimeError) as exc:
        raise ValueError(f"cannot resolve scenario path {raw_path!r}: {exc}") from exc
    if not resolved.is_relative_to(root):
        raise ValueError(f"scenario path escapes workspace root: {raw_path}")
    return resolved


def expand_matrix_scenarios(
    matrix_definition: ScenarioMatrixDefinition,
    *,
    workspace_root: str | Path | None = None,
) -> list[ScenarioDefinition]:
    """Expand a matrix into values while containing reads to one workspace."""

    raw_root = (
        Path(workspace_root)
        if workspace_root is not None
        else Path(__file__).resolve().parents[2]
    )
    try:
        root = raw_root.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise ValueError(f"cannot resolve workspace root {raw_root}: {exc}") from exc
    if not root.is_dir():
        raise ValueError(f"workspace root is not a directory: {root}")
    source_path = matrix_definition.source_path or root / "<in-memory-matrix>"
    _artifact_id(
        matrix_definition.matrix_id,
        field_name="matrix_id",
        source_path=source_path,
    )
    _bounded_string(
        matrix_definition.description,
        label=f"{source_path} description",
    )
    if not isinstance(matrix_definition.scenarios, list):
        raise ValueError("matrix scenarios must be a list")
    if len(matrix_definition.scenarios) > MAX_MATRIX_SCENARIOS:
        raise ValueError(
            f"matrix exceeds the scenario bound {MAX_MATRIX_SCENARIOS}"
        )

    normalized_items: list[dict[str, Any]] = []
    expanded_count = 0
    for index, item in enumerate(matrix_definition.scenarios):
        normalized = _normalize_matrix_item(
            item,
            source_path=source_path,
            index=index,
        )
        repeat = normalized["repeat"]
        if repeat > MAX_EXPANDED_SCENARIOS - expanded_count:
            raise ValueError(
                "matrix exceeds the expanded scenario bound "
                f"{MAX_EXPANDED_SCENARIOS}"
            )
        expanded_count += repeat
        normalized_items.append(normalized)
    if not normalized_items:
        raise ValueError("matrix does not declare any scenarios")

    definitions: list[ScenarioDefinition] = []
    for item in normalized_items:
        definition = load_scenario_definition(
            _contained_path(root, item["path"])
        )
        tags = list(item["tags"])
        repeat = item["repeat"]
        if repeat == 1:
            definitions.append(replace(definition, tags=tags))
            continue
        for repeat_index in range(repeat):
            repeated_id = _artifact_id(
                f"{definition.scenario_id}__repeat_{repeat_index + 1:02d}",
                field_name="expanded scenario_id",
                source_path=definition.source_path or source_path,
            )
            repeated_description = (
                f"{definition.description} "
                f"[repeat {repeat_index + 1}/{repeat}]"
            ).strip()
            definitions.append(
                replace(
                    definition,
                    scenario_id=repeated_id,
                    description=_bounded_string(
                        repeated_description,
                        label="expanded scenario description",
                    ),
                    tags=tags,
                )
            )
    return definitions


def resolve_scenario_payload_path(payload: Any, path: str) -> Any:
    """Resolve dotted dictionary keys and bracketed list indexes."""

    path = _bounded_string(
        path,
        label="payload path",
        max_bytes=MAX_PATH_BYTES,
    )
    tokens: list[tuple[str, str]] = []
    for segment in path.split("."):
        if not segment:
            continue
        position = 0
        for match in _PATH_TOKEN_RE.finditer(segment):
            if match.start() != position:
                raise ScenarioExecutionError(
                    f"payload path has invalid syntax: {path!r}"
                )
            key_token, index_token = match.groups()
            tokens.append((key_token or "", index_token or ""))
            if len(tokens) > MAX_PAYLOAD_PATH_TOKENS:
                raise ScenarioExecutionError(
                    "payload path exceeds the token bound "
                    f"{MAX_PAYLOAD_PATH_TOKENS}"
                )
            if index_token:
                if len(index_token) > 16:
                    raise ScenarioExecutionError(
                        "payload path index exceeds its 16-digit bound"
                    )
                if int(index_token) > MAX_SAFE_INTEGER:
                    raise ScenarioExecutionError(
                        "payload path index exceeds the I-JSON safe range"
                    )
            position = match.end()
        if position != len(segment):
            raise ScenarioExecutionError(
                f"payload path has invalid syntax: {path!r}"
            )

    current = payload
    for key_token, index_token in tokens:
        token = key_token or index_token
        if key_token:
            if not isinstance(current, dict) or token not in current:
                return _MISSING
            current = current[token]
            continue
        if not isinstance(current, list):
            return _MISSING
        index = int(token)
        if index >= len(current):
            return _MISSING
        current = current[index]
    return current


def _expectation_operator(condition: dict[str, Any]) -> str:
    if not isinstance(condition, dict):
        raise ScenarioExecutionError("condition must be a mapping")
    try:
        _bounded_json_bytes(condition, label="condition")
    except ValueError as exc:
        raise ScenarioExecutionError(str(exc)) from exc
    operators = [
        operator
        for operator in _EXPECTATION_OPERATORS
        if operator in condition
    ]
    if len(operators) != 1:
        raise ScenarioExecutionError(
            "condition must define exactly one supported operator: "
            + ", ".join(_EXPECTATION_OPERATORS)
        )
    operator = operators[0]
    if operator == "truthy" and condition[operator] is not True:
        raise ScenarioExecutionError("condition truthy operator must be true")
    return operator


def _json_values_equal(left: Any, right: Any) -> bool:
    """Compare validated JSON values without Python bool/number coercion."""

    if isinstance(left, bool) or isinstance(right, bool):
        return isinstance(left, bool) and isinstance(right, bool) and left is right
    if left is None or right is None:
        return left is None and right is None
    if isinstance(left, (int, float)) or isinstance(right, (int, float)):
        return (
            isinstance(left, (int, float))
            and isinstance(right, (int, float))
            and left == right
        )
    if isinstance(left, str) or isinstance(right, str):
        return isinstance(left, str) and isinstance(right, str) and left == right
    if isinstance(left, list) or isinstance(right, list):
        return (
            isinstance(left, list)
            and isinstance(right, list)
            and len(left) == len(right)
            and all(
                _json_values_equal(left_item, right_item)
                for left_item, right_item in zip(left, right, strict=True)
            )
        )
    if isinstance(left, dict) or isinstance(right, dict):
        return (
            isinstance(left, dict)
            and isinstance(right, dict)
            and left.keys() == right.keys()
            and all(
                _json_values_equal(left[key], right[key])
                for key in left
            )
        )
    return False


def match_scenario_expectation(
    actual: Any,
    condition: dict[str, Any],
) -> bool:
    """Match one value without fetching or owning its source fact."""

    operator = _expectation_operator(condition)
    if actual is _MISSING:
        return False
    try:
        _bounded_json_bytes(actual, label="actual fact")
    except ValueError as exc:
        raise ScenarioExecutionError(str(exc)) from exc
    if operator == "equals":
        return _json_values_equal(actual, condition[operator])
    if operator == "not_equals":
        return not _json_values_equal(actual, condition[operator])
    if operator == "greater_equal":
        expected = condition[operator]
        if (
            isinstance(actual, bool)
            or isinstance(expected, bool)
            or not isinstance(actual, (int, float))
            or not isinstance(expected, (int, float))
        ):
            return False
        return actual >= expected
    if operator == "less_equal":
        expected = condition[operator]
        if (
            isinstance(actual, bool)
            or isinstance(expected, bool)
            or not isinstance(actual, (int, float))
            or not isinstance(expected, (int, float))
        ):
            return False
        return actual <= expected
    if operator == "contains":
        expected = condition["contains"]
        if isinstance(actual, str):
            return isinstance(expected, str) and expected in actual
        if isinstance(actual, list):
            return any(_json_values_equal(item, expected) for item in actual)
        if isinstance(actual, dict):
            return any(
                _json_values_equal(item, expected)
                for item in actual.keys()
            ) or any(
                _json_values_equal(item, expected)
                for item in actual.values()
            )
        return False
    if operator == "truthy":
        return actual is True
    raise AssertionError(f"unhandled expectation operator: {operator}")


def scenario_condition_summary(condition: dict[str, Any]) -> str:
    """Render one owner-neutral expectation for evidence or diagnostics."""

    operator = _expectation_operator(condition)
    endpoint = condition.get("endpoint")
    path = condition.get("path")
    if operator == "equals":
        return f"{endpoint}.{path} == {condition[operator]!r}"
    if operator == "not_equals":
        return f"{endpoint}.{path} != {condition[operator]!r}"
    if operator == "greater_equal":
        return f"{endpoint}.{path} >= {condition[operator]!r}"
    if operator == "less_equal":
        return f"{endpoint}.{path} <= {condition[operator]!r}"
    if operator == "contains":
        return f"{endpoint}.{path} contains {condition[operator]!r}"
    if operator == "truthy":
        return f"{endpoint}.{path} is truthy"
    raise AssertionError(f"unhandled expectation operator: {operator}")


def format_scenario_condition_failure(
    condition: dict[str, Any],
    actual: Any,
) -> str:
    """Render a stable mismatch without implying a runtime owner."""

    if actual is _MISSING:
        rendered_actual = "<missing>"
    else:
        try:
            _bounded_json_bytes(actual, label="actual fact")
        except ValueError as exc:
            raise ScenarioExecutionError(str(exc)) from exc
        rendered_actual = repr(actual)
    return (
        "condition not met: "
        f"{scenario_condition_summary(condition)} (actual={rendered_actual})"
    )


def _bounded_json_bytes(value: Any, *, label: str) -> bytes:
    try:
        _validate_json_model(value, label)
    except ValueError:
        raise
    encoder = json.JSONEncoder(
        allow_nan=False,
        indent=2,
        sort_keys=True,
    )
    chunks: list[bytes] = []
    total_bytes = 0
    try:
        for chunk in encoder.iterencode(value):
            encoded = chunk.encode("utf-8")
            if len(encoded) > MAX_EVIDENCE_DOCUMENT_BYTES - total_bytes:
                raise ValueError(
                    f"{label} exceeds the "
                    f"{MAX_EVIDENCE_DOCUMENT_BYTES}-byte evidence bound"
                )
            total_bytes += len(encoded)
            chunks.append(encoded)
    except (TypeError, ValueError, UnicodeError) as exc:
        if isinstance(exc, ValueError) and str(exc).startswith(label):
            raise
        raise ValueError(f"{label} is not bounded JSON: {exc}") from exc
    if total_bytes == MAX_EVIDENCE_DOCUMENT_BYTES:
        raise ValueError(
            f"{label} exceeds the "
            f"{MAX_EVIDENCE_DOCUMENT_BYTES}-byte evidence bound"
        )
    chunks.append(b"\n")
    return b"".join(chunks)


def _bounded_path(value: str | Path, *, label: str) -> Path:
    path_text = _bounded_string(
        os.fspath(value),
        label=label,
        allow_empty=False,
        max_bytes=MAX_PATH_BYTES,
    )
    if "\x00" in path_text:
        raise ValueError(f"{label} contains a NUL byte")
    return Path(path_text)


def _resolve_output_directory(output_dir: str | Path) -> Path:
    raw_target = _bounded_path(output_dir, label="artifact output directory")
    if raw_target.is_symlink():
        raise ScenarioExecutionError(
            f"artifact output directory must not be a symlink: {raw_target}"
        )
    try:
        target = raw_target.resolve(strict=False)
        parent = target.parent.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise ScenarioExecutionError(
            f"cannot resolve artifact output directory {raw_target}: {exc}"
        ) from exc
    if not parent.is_dir():
        raise ScenarioExecutionError(
            f"artifact output parent is not a directory: {parent}"
        )
    if target == parent or target == Path(target.anchor):
        raise ScenarioExecutionError(
            f"artifact output must be a dedicated child directory: {target}"
        )
    return target


def _output_json_name(*parts: str) -> str:
    name = ".".join((*parts, "json"))
    if (
        len(name.encode("utf-8")) > MAX_OUTPUT_NAME_BYTES
        or _OUTPUT_JSON_NAME_RE.fullmatch(name) is None
    ):
        raise ValueError(
            f"artifact output name exceeds its safe bound: {name!r}"
        )
    return name


def _require_output_marker(target: Path) -> Path:
    marker = target / _OUTPUT_MARKER_NAME
    expected = _OUTPUT_MARKER_TEXT.encode("utf-8")
    try:
        metadata = marker.lstat()
    except OSError as exc:
        raise ScenarioExecutionError(
            f"artifact directory is not owned by this writer: {target}"
        ) from exc
    if not stat.S_ISREG(metadata.st_mode) or marker.is_symlink():
        raise ScenarioExecutionError(
            f"artifact ownership marker is not a regular file: {marker}"
        )
    if metadata.st_size != len(expected):
        raise ScenarioExecutionError(
            f"artifact ownership marker is invalid: {marker}"
        )
    try:
        descriptor = os.open(
            marker,
            os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
        )
        with os.fdopen(descriptor, "rb") as stream:
            opened_metadata = os.fstat(stream.fileno())
            marker_bytes = stream.read(len(expected) + 1)
    except OSError as exc:
        raise ScenarioExecutionError(
            f"cannot read artifact ownership marker {marker}: {exc}"
        ) from exc
    if (
        not stat.S_ISREG(opened_metadata.st_mode)
        or opened_metadata.st_dev != metadata.st_dev
        or opened_metadata.st_ino != metadata.st_ino
        or marker_bytes != expected
    ):
        raise ScenarioExecutionError(
            f"artifact ownership marker is invalid: {marker}"
        )
    return marker


def _ensure_owned_output_directory(target: Path) -> None:
    if os.path.lexists(target):
        try:
            metadata = target.lstat()
        except OSError as exc:
            raise ScenarioExecutionError(
                f"cannot inspect artifact output directory {target}: {exc}"
            ) from exc
        if not stat.S_ISDIR(metadata.st_mode) or target.is_symlink():
            raise ScenarioExecutionError(
                f"artifact output is not a dedicated directory: {target}"
            )
        _require_output_marker(target)
        return

    try:
        target.mkdir(mode=0o700, exist_ok=False)
        marker = target / _OUTPUT_MARKER_NAME
        with marker.open("x", encoding="utf-8") as stream:
            stream.write(_OUTPUT_MARKER_TEXT)
            stream.flush()
            os.fsync(stream.fileno())
    except FileExistsError as exc:
        raise ScenarioExecutionError(
            f"artifact output was created concurrently: {target}"
        ) from exc
    except OSError as exc:
        raise ScenarioExecutionError(
            f"cannot create artifact output directory {target}: {exc}"
        ) from exc


def _output_inventory(
    target: Path,
) -> tuple[list[tuple[Path, int, int, int]], int]:
    _require_output_marker(target)
    entries: list[tuple[Path, int, int, int]] = []
    total_bytes = 0
    try:
        scanner = os.scandir(target)
    except OSError as exc:
        raise ScenarioExecutionError(
            f"cannot inventory artifact directory {target}: {exc}"
        ) from exc
    with scanner:
        for entry in scanner:
            if entry.name == _OUTPUT_MARKER_NAME:
                continue
            child = target / entry.name
            if len(entries) >= MAX_OUTPUT_FILES:
                raise ScenarioExecutionError(
                    f"artifact directory exceeds {MAX_OUTPUT_FILES} files"
                )
            if (
                len(entry.name.encode("utf-8")) > MAX_OUTPUT_NAME_BYTES
                or _OUTPUT_JSON_NAME_RE.fullmatch(entry.name) is None
            ):
                raise ScenarioExecutionError(
                    f"artifact directory contains an unowned entry: {child}"
                )
            try:
                metadata = entry.stat(follow_symlinks=False)
            except OSError as exc:
                raise ScenarioExecutionError(
                    f"cannot inspect artifact entry {child}: {exc}"
                ) from exc
            if not stat.S_ISREG(metadata.st_mode) or entry.is_symlink():
                raise ScenarioExecutionError(
                    "artifact entry must be a regular non-symlink file: "
                    f"{child}"
                )
            if metadata.st_size > MAX_EVIDENCE_DOCUMENT_BYTES:
                raise ScenarioExecutionError(
                    f"artifact entry exceeds the evidence bound: {child}"
                )
            if metadata.st_size > MAX_OUTPUT_BYTES - total_bytes:
                raise ScenarioExecutionError(
                    f"artifact directory exceeds {MAX_OUTPUT_BYTES} bytes"
                )
            entries.append(
                (
                    child,
                    metadata.st_size,
                    metadata.st_dev,
                    metadata.st_ino,
                )
            )
            total_bytes += metadata.st_size
    entries.sort(key=lambda row: row[0].name)
    return entries, total_bytes


def _fsync_directory(path: Path) -> None:
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _rollback_output_files(
    target: Path,
    created: list[tuple[Path, int, int]],
) -> list[str]:
    failures: list[str] = []
    for path, expected_device, expected_inode in reversed(created):
        try:
            metadata = path.lstat()
        except FileNotFoundError:
            continue
        except OSError as exc:
            failures.append(f"cannot inspect {path}: {exc}")
            continue
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_dev != expected_device
            or metadata.st_ino != expected_inode
        ):
            failures.append(f"refusing to remove changed output {path}")
            continue
        try:
            path.unlink()
        except OSError as exc:
            failures.append(f"cannot remove {path}: {exc}")
    try:
        _fsync_directory(target)
    except OSError as exc:
        failures.append(f"cannot fsync rollback directory {target}: {exc}")
    return failures


def _publish_output_files(
    target: Path,
    files: list[tuple[str, bytes]],
) -> list[tuple[Path, int, int]]:
    names = [name for name, _ in files]
    if len(names) != len(set(names)):
        raise ValueError("artifact output contains duplicate file names")
    if len(files) > MAX_OUTPUT_FILES:
        raise ValueError(f"artifact output exceeds {MAX_OUTPUT_FILES} files")
    new_total = sum(len(payload) for _, payload in files)
    if new_total > MAX_OUTPUT_BYTES:
        raise ValueError(f"artifact output exceeds {MAX_OUTPUT_BYTES} bytes")

    _ensure_owned_output_directory(target)
    existing, existing_total = _output_inventory(target)
    if len(existing) + len(files) > MAX_OUTPUT_FILES:
        raise ScenarioExecutionError(
            f"artifact output exceeds {MAX_OUTPUT_FILES} files"
        )
    if new_total > MAX_OUTPUT_BYTES - existing_total:
        raise ScenarioExecutionError(
            f"artifact output exceeds {MAX_OUTPUT_BYTES} bytes"
        )
    destinations = [(target / name, payload) for name, payload in files]
    for destination, _ in destinations:
        if not destination.is_relative_to(target):
            raise ScenarioExecutionError(
                f"artifact output escapes its owned directory: {destination}"
            )
        if os.path.lexists(destination):
            raise ScenarioExecutionError(
                f"refusing to overwrite artifact output: {destination}"
            )

    created: list[tuple[Path, int, int]] = []
    try:
        for destination, payload in destinations:
            try:
                descriptor = os.open(
                    destination,
                    os.O_WRONLY
                    | os.O_CREAT
                    | os.O_EXCL
                    | getattr(os, "O_NOFOLLOW", 0),
                    0o600,
                )
            except FileExistsError as exc:
                raise ScenarioExecutionError(
                    f"refusing to overwrite artifact output: {destination}"
                ) from exc
            try:
                metadata = os.fstat(descriptor)
                stream = os.fdopen(descriptor, "wb")
            except Exception:
                os.close(descriptor)
                raise
            created.append(
                (destination, metadata.st_dev, metadata.st_ino)
            )
            with stream:
                stream.write(payload)
                stream.flush()
                os.fsync(stream.fileno())
        _fsync_directory(target)
    except Exception as exc:
        rollback_failures = _rollback_output_files(target, created)
        if rollback_failures:
            raise ScenarioExecutionError(
                "artifact publication failed and rollback was incomplete: "
                + "; ".join(rollback_failures)
            ) from exc
        if isinstance(exc, ScenarioExecutionError):
            raise
        raise ScenarioExecutionError(
            f"cannot publish artifact output: {exc}"
        ) from exc
    return created


def _resolve_archive_destination(source: Path, archive_path: str | Path) -> Path:
    raw_archive = _bounded_path(archive_path, label="artifact archive path")
    if raw_archive.is_symlink() or os.path.lexists(raw_archive):
        raise ScenarioExecutionError(
            f"refusing to overwrite artifact archive: {raw_archive}"
        )
    try:
        archive = raw_archive.resolve(strict=False)
        parent = archive.parent.resolve(strict=True)
    except OSError as exc:
        raise ScenarioExecutionError(
            f"cannot resolve artifact archive path {raw_archive}: {exc}"
        ) from exc
    if archive.is_relative_to(source):
        raise ScenarioExecutionError(
            "artifact archive must not be inside its artifact directory: "
            f"{archive}"
        )
    if parent != source.parent:
        raise ScenarioExecutionError(
            "artifact archive must be a direct sibling of its owned "
            f"artifact directory: {archive}"
        )
    if (
        len(archive.name.encode("utf-8")) > MAX_OUTPUT_NAME_BYTES
        or _ARCHIVE_NAME_RE.fullmatch(archive.name) is None
    ):
        raise ScenarioExecutionError(
            f"artifact archive has an unsafe name: {archive.name!r}"
        )
    return archive


def write_scenario_artifacts(
    result: dict[str, Any],
    *,
    output_dir: str | Path,
) -> Path:
    """Publish one bounded result into a dedicated owned artifact directory."""

    if not isinstance(result, dict):
        raise ValueError("scenario result must be a mapping")
    target_dir = _resolve_output_directory(output_dir)
    scenario_id = _artifact_id(
        result.get("scenarioId"),
        field_name="scenarioId",
        source_path=target_dir,
    )
    raw_artifacts = result.get("finalArtifacts")
    if raw_artifacts is None:
        raw_artifacts = {}
    if not isinstance(raw_artifacts, dict):
        raise ValueError("scenario finalArtifacts must be a mapping")
    if len(raw_artifacts) > MAX_FINAL_ARTIFACTS:
        raise ValueError(
            "scenario finalArtifacts exceeds the artifact bound "
            f"{MAX_FINAL_ARTIFACTS}"
        )
    result_name = _output_json_name(scenario_id, "result")
    result_payload = _bounded_json_bytes(
        result,
        label="scenario result",
    )
    serialized_total = len(result_payload)
    artifacts: list[tuple[str, bytes]] = []
    for raw_name, payload in raw_artifacts.items():
        if payload is None:
            continue
        artifact_name = _artifact_id(
            raw_name,
            field_name="artifact name",
            source_path=target_dir,
        )
        artifact_payload = _bounded_json_bytes(
            payload,
            label=f"scenario artifact {artifact_name!r}",
        )
        if len(artifact_payload) > MAX_OUTPUT_BYTES - serialized_total:
            raise ValueError(
                f"scenario output exceeds {MAX_OUTPUT_BYTES} bytes"
            )
        serialized_total += len(artifact_payload)
        artifacts.append(
            (
                _output_json_name(scenario_id, artifact_name),
                artifact_payload,
            )
        )
    _publish_output_files(
        target_dir,
        [*artifacts, (result_name, result_payload)],
    )
    return target_dir / result_name


def archive_artifacts(
    source_dir: str | Path,
    archive_path: str | Path,
) -> Path:
    """Publish a deterministic archive of one owned, flat artifact directory."""

    raw_source = _bounded_path(source_dir, label="artifact source directory")
    if raw_source.is_symlink():
        raise ScenarioExecutionError(
            f"artifact source must not be a symlink: {raw_source}"
        )
    try:
        resolved_source = raw_source.resolve(strict=True)
    except OSError as exc:
        raise ScenarioExecutionError(
            f"cannot resolve artifact source {raw_source}: {exc}"
        ) from exc
    try:
        source_metadata = resolved_source.lstat()
    except OSError as exc:
        raise ScenarioExecutionError(
            f"cannot inspect artifact source {resolved_source}: {exc}"
        ) from exc
    if not stat.S_ISDIR(source_metadata.st_mode):
        raise ScenarioExecutionError(
            f"artifact source is not a directory: {resolved_source}"
        )
    marker = _require_output_marker(resolved_source)
    entries, _ = _output_inventory(resolved_source)
    marker_metadata = marker.lstat()
    resolved_archive = _resolve_archive_destination(
        resolved_source,
        archive_path,
    )

    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{resolved_archive.name}.pending-",
        suffix=".tmp",
        dir=resolved_archive.parent,
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "w+b") as stream:
            with gzip.GzipFile(
                filename="",
                mode="wb",
                fileobj=stream,
                mtime=0,
            ) as compressed:
                with tarfile.open(fileobj=compressed, mode="w") as archive:
                    root_info = tarfile.TarInfo("artifacts/")
                    root_info.type = tarfile.DIRTYPE
                    root_info.mode = 0o755
                    root_info.uid = 0
                    root_info.gid = 0
                    root_info.uname = ""
                    root_info.gname = ""
                    root_info.mtime = 0
                    archive.addfile(root_info)
                    for source, expected_size, expected_device, expected_inode in [
                        (
                            marker,
                            len(_OUTPUT_MARKER_TEXT.encode("utf-8")),
                            marker_metadata.st_dev,
                            marker_metadata.st_ino,
                        ),
                        *entries,
                    ]:
                        input_descriptor = os.open(
                            source,
                            os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
                        )
                        with os.fdopen(input_descriptor, "rb") as input_stream:
                            metadata = os.fstat(input_stream.fileno())
                            if (
                                not stat.S_ISREG(metadata.st_mode)
                                or metadata.st_size != expected_size
                                or metadata.st_dev != expected_device
                                or metadata.st_ino != expected_inode
                            ):
                                raise ScenarioExecutionError(
                                    "artifact changed during archive creation: "
                                    f"{source}"
                                )
                            info = tarfile.TarInfo(
                                f"artifacts/{source.name}"
                            )
                            info.size = metadata.st_size
                            info.mode = 0o644
                            info.uid = 0
                            info.gid = 0
                            info.uname = ""
                            info.gname = ""
                            info.mtime = 0
                            archive.addfile(info, input_stream)
            stream.flush()
            os.fsync(stream.fileno())
        staged_metadata = temporary.stat()
        if staged_metadata.st_size > MAX_ARCHIVE_OUTPUT_BYTES:
            raise ScenarioExecutionError(
                "artifact archive exceeds the output bound "
                f"{MAX_ARCHIVE_OUTPUT_BYTES}"
            )
        try:
            os.link(temporary, resolved_archive)
        except FileExistsError as exc:
            raise ScenarioExecutionError(
                f"refusing to overwrite artifact archive: {resolved_archive}"
            ) from exc
        try:
            _fsync_directory(resolved_archive.parent)
        except OSError as exc:
            rollback_failures = _rollback_output_files(
                resolved_archive.parent,
                [
                    (
                        resolved_archive,
                        staged_metadata.st_dev,
                        staged_metadata.st_ino,
                    )
                ],
            )
            if rollback_failures:
                raise ScenarioExecutionError(
                    "archive publication failed and rollback was incomplete: "
                    + "; ".join(rollback_failures)
                ) from exc
            raise ScenarioExecutionError(
                f"cannot durably publish artifact archive: {exc}"
            ) from exc
    finally:
        temporary.unlink(missing_ok=True)
    return resolved_archive


def write_matrix_artifacts(
    summary: dict[str, Any],
    *,
    output_dir: str | Path,
    archive_path: str | Path | None = None,
) -> Path:
    """Publish one bounded matrix summary and optional deterministic archive."""

    if not isinstance(summary, dict):
        raise ValueError("matrix summary must be a mapping")
    target_dir = _resolve_output_directory(output_dir)
    matrix_id = _artifact_id(
        summary.get("matrixId"),
        field_name="matrixId",
        source_path=target_dir,
    )
    summary_name = _output_json_name(matrix_id, "summary")
    summary_payload = _bounded_json_bytes(
        summary,
        label="matrix summary",
    )
    resolved_archive: Path | None = None
    if archive_path is not None:
        resolved_archive = _resolve_archive_destination(
            target_dir,
            archive_path,
        )
    published_summary = _publish_output_files(
        target_dir,
        [(summary_name, summary_payload)],
    )
    if resolved_archive is not None:
        try:
            archive_artifacts(target_dir, resolved_archive)
        except Exception as exc:
            rollback_failures = _rollback_output_files(
                target_dir,
                published_summary,
            )
            if rollback_failures:
                raise ScenarioExecutionError(
                    "matrix archive failed and summary rollback was incomplete: "
                    + "; ".join(rollback_failures)
                ) from exc
            raise
    return target_dir / summary_name


def _raise_scenario_execution_unavailable() -> NoReturn:
    raise ScenarioExecutionUnavailableError(
        f"{UNAVAILABLE_REASON_CODE}: HTTP scenario execution returns with the "
        "qualified headless Program/Kernel client"
    )


class ScenarioRunner:
    """Historical name retained solely as a fail-before-effects boundary."""

    def __init__(
        self,
        client: Any,
        *,
        workspace_root: str | Path | None = None,
    ) -> None:
        del client, workspace_root
        _raise_scenario_execution_unavailable()


def _sha256_path(path: Path | None) -> str | None:
    """Hash one regular evidence file within the explicit file-size bound."""

    if path is None:
        return None
    try:
        expected_metadata = path.lstat()
    except FileNotFoundError:
        return None
    except OSError as exc:
        raise ScenarioExecutionError(
            f"cannot inspect evidence file {path}: {exc}"
        ) from exc
    if not stat.S_ISREG(expected_metadata.st_mode) or path.is_symlink():
        raise ScenarioExecutionError(
            f"evidence source must be a regular non-symlink file: {path}"
        )
    digest = hashlib.sha256()
    try:
        descriptor = os.open(
            path,
            os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0),
        )
        source = os.fdopen(descriptor, "rb")
    except OSError as exc:
        raise ScenarioExecutionError(
            f"cannot read evidence file {path}: {exc}"
        ) from exc
    with source:
        metadata = os.fstat(source.fileno())
        if (
            not stat.S_ISREG(metadata.st_mode)
            or metadata.st_dev != expected_metadata.st_dev
            or metadata.st_ino != expected_metadata.st_ino
        ):
            raise ScenarioExecutionError(
                f"evidence source changed before hashing: {path}"
            )
        if metadata.st_size > MAX_EVIDENCE_FILE_BYTES:
            raise ScenarioExecutionError(
                f"evidence file exceeds {MAX_EVIDENCE_FILE_BYTES} bytes: {path}"
            )
        total_bytes = 0
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            if len(chunk) > MAX_EVIDENCE_FILE_BYTES - total_bytes:
                raise ScenarioExecutionError(
                    f"evidence file grew beyond {MAX_EVIDENCE_FILE_BYTES} "
                    f"bytes while hashing: {path}"
                )
            total_bytes += len(chunk)
            digest.update(chunk)
    return digest.hexdigest()


def _git_rev_parse(workspace_root: Path) -> str | None:
    """Resolve revision evidence from a validated environment value or Git."""

    for env_name in ("MANYFORGE_GIT_COMMIT", "GIT_COMMIT"):
        value = os.environ.get(env_name, "").strip()
        if re.fullmatch(r"[0-9a-fA-F]{7,40}", value):
            return value

    commands = (
        ("git", "-C", str(workspace_root), "rev-parse", "HEAD"),
        (
            "git",
            "-c",
            f"safe.directory={workspace_root}",
            "-C",
            str(workspace_root),
            "rev-parse",
            "HEAD",
        ),
    )
    for command in commands:
        try:
            completed = subprocess.run(
                command,
                capture_output=True,
                check=True,
                text=True,
                timeout=2.0,
            )
        except (OSError, subprocess.SubprocessError):
            continue
        value = completed.stdout.strip()
        if re.fullmatch(r"[0-9a-fA-F]{7,40}", value):
            return value
    return None
