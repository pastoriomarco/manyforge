#!/usr/bin/env python3
"""Validate planner-neutral contracts and truthful adapter availability.

This is an offline source/evidence checker.  It does not construct a planner,
load a runtime adapter, or make the unavailable RoboPlan product operational.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
from typing import Final, Iterator

import yaml


ROOT = Path(__file__).resolve().parents[2]
MAPPING = ROOT / "verification/planner_portability/rp5-6-backend-mapping.json"

MAX_CORPUS_FILES: Final = 4_096
MAX_CORPUS_BYTES: Final = 64 * 1024 * 1024
MAX_FILE_BYTES: Final = 4 * 1024 * 1024
MAX_DOCUMENT_DEPTH: Final = 64
MAX_DOCUMENT_NODES: Final = 65_536
MAX_COLLECTION_ITEMS: Final = 4_096
MAX_SCALAR_CHARS: Final = 65_536
MAX_TOTAL_SCALAR_CHARS: Final = 4 * 1024 * 1024
MAX_INTEGER_BITS: Final = 256
MAX_IDENTIFIER_CHARS: Final = 4_096
MAX_FAILURES: Final = 4_096
MAX_FAILURE_CHARS: Final = 8_192
MAX_TRAVERSAL_ENTRIES: Final = 16_384
MAX_TRAVERSAL_DEPTH: Final = 64

REQUIRED_CAPABILITIES = {
    "planning.motion.joint_goal",
    "planning.motion.pose_goal",
    "planning.motion.cartesian_path",
    "planning.ik.single_shot",
    "planning.cancellation.cooperative",
    "planning.trajectory.time_parameterized",
    "scene.projection.revision_bound",
    "planning.failure.structured",
}
REQUIRED_PROFILES = {
    "planning.profile.first_feasible",
    "planning.profile.balanced",
    "planning.profile.quality_bounded",
}
REQUIRED_EVIDENCE = {
    "neutral_harness_reference_backend",
    "neutral_harness_detects_invalid_outputs",
    "neutral_harness_time_parameterized_output",
    "neutral_harness_detects_private_names",
    "neutral_harness_detects_private_ik_names",
    "neutral_harness_contains_noncooperative_cancel",
    "explicit_optional_capability_rejection",
    "relative_resolution_precedes_planner",
    "relative_execution_reaches_planner_as_resolved_pose",
    "exact_revision_cache_hit_revalidates",
    "older_revision_candidate_revalidates_current_snapshot",
    "failed_revalidation_forces_planning",
    "intent_change_forces_planning",
    "profile_participates_in_cache_identity",
    "start_incompatibility_forces_planning",
    "goal_incompatibility_forces_planning",
    "model_binding_participates_in_cache_identity",
    "contact_policy_participates_in_cache_identity",
    "cache_identity_mismatch_forces_planning",
    "planner_candidate_cannot_bypass_non_collision_validation",
    "planner_candidate_cannot_bypass_collision_authority",
    "authoritative_collision_replans_candidate",
    "cached_candidate_cannot_bypass_validation",
}

FORBIDDEN_BACKEND_OPTION_KEYS = {
    "fast_return",
    "goal_bias",
    "goal_biasing_probability",
    "max_nodes",
    "oin_k",
    "rrt_star",
}

_TOP_LEVEL_FIELDS = {
    "schema_version",
    "contract_revision",
    "runtime_availability",
    "portable_capabilities",
    "portable_profiles",
    "authority",
    "adapters",
    "test_references",
}
_PROGRAM_ARTIFACT_ROOTS = (
    "examples",
    "verification/programs",
    "manyforge_behavior/resources",
    "manyforge_behavior/tests/fixtures",
    "tests/fixtures",
)
_PROGRAM_ARTIFACT_SUFFIXES = {".json", ".yaml", ".yml"}


class PlannerPortabilityInputError(ValueError):
    """Raised when checked evidence is malformed, unsafe, or unreadable."""


class PlannerPortabilityBoundsError(PlannerPortabilityInputError):
    """Raised before an input can exceed a checker resource bound."""


class _UniqueKeySafeLoader(yaml.SafeLoader):
    """SafeLoader variant that rejects duplicate mapping keys."""


def _safe_excerpt(value: str, *, limit: int = 128) -> str:
    if len(value) <= limit:
        return value
    return value[: limit - 3] + "..."


def _child_diagnostic_path(parent: str, suffix: str) -> str:
    return _safe_excerpt(parent + suffix, limit=512)


def _construct_unique_mapping(
    loader: _UniqueKeySafeLoader,
    node: yaml.nodes.MappingNode,
    deep: bool = False,
) -> dict[str, object]:
    loader.flatten_mapping(node)
    pairs = loader.construct_pairs(node, deep=deep)
    result: dict[str, object] = {}
    for key, value in pairs:
        if type(key) is not str:
            raise PlannerPortabilityInputError(
                "YAML mappings must use string keys"
            )
        if key in result:
            raise PlannerPortabilityInputError(
                "YAML mapping contains duplicate key "
                f"{_safe_excerpt(key)!r}"
            )
        result[key] = value
    return result


_UniqueKeySafeLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG,
    _construct_unique_mapping,
)


def fail(failures: list[str], message: str) -> None:
    if type(message) is not str or not message:
        raise PlannerPortabilityInputError(
            "failure diagnostics must be non-empty strings"
        )
    if len(message) > MAX_FAILURE_CHARS:
        raise PlannerPortabilityBoundsError(
            f"failure diagnostic exceeds {MAX_FAILURE_CHARS} characters"
        )
    if len(failures) >= MAX_FAILURES:
        raise PlannerPortabilityBoundsError(
            f"checker failures exceeds the {MAX_FAILURES}-row bound"
        )
    failures.append(message)


def _bounded_text(
    value: object,
    *,
    label: str,
    limit: int = MAX_IDENTIFIER_CHARS,
    allow_empty: bool = False,
) -> str:
    if type(value) is not str or (not allow_empty and not value):
        qualifier = "a string" if allow_empty else "a non-empty string"
        raise PlannerPortabilityInputError(f"{label} must be {qualifier}")
    if len(value) > limit:
        raise PlannerPortabilityBoundsError(
            f"{label} exceeds the {limit}-character bound"
        )
    return value


def _normalized_relative_path(value: object, *, label: str) -> str:
    text = _bounded_text(value, label=label)
    if "\x00" in text:
        raise PlannerPortabilityInputError(f"{label} must not contain NUL bytes")
    if "\\" in text:
        raise PlannerPortabilityInputError(
            f"{label} must use repository-relative POSIX separators"
        )
    path = PurePosixPath(text)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise PlannerPortabilityInputError(
            f"{label} must be a contained repository-relative path"
        )
    if path.as_posix() != text:
        raise PlannerPortabilityInputError(
            f"{label} must be a normalized repository-relative path"
        )
    return text


@dataclass(slots=True)
class BoundedFileCorpus:
    """Contained, no-symlink, regular-file reader with one shared budget."""

    root: Path
    _resolved_root: Path = field(init=False)
    _cache: dict[Path, str] = field(default_factory=dict, init=False)
    _total_bytes: int = field(default=0, init=False)

    def __post_init__(self) -> None:
        try:
            resolved = self.root.resolve(strict=True)
            metadata = resolved.stat(follow_symlinks=False)
        except OSError as exc:
            raise PlannerPortabilityInputError(
                f"cannot resolve checker root {self.root}: {exc}"
            ) from exc
        if not stat.S_ISDIR(metadata.st_mode):
            raise PlannerPortabilityInputError(
                f"checker root is not a directory: {resolved}"
            )
        self._resolved_root = resolved

    @property
    def resolved_root(self) -> Path:
        return self._resolved_root

    @property
    def file_count(self) -> int:
        return len(self._cache)

    @property
    def total_bytes(self) -> int:
        return self._total_bytes

    def _checked_path(self, relative_path: object, *, label: str) -> Path:
        normalized = _normalized_relative_path(relative_path, label=label)
        candidate = self._resolved_root
        try:
            for part in PurePosixPath(normalized).parts:
                candidate = candidate / part
                metadata = candidate.lstat()
                if stat.S_ISLNK(metadata.st_mode):
                    raise PlannerPortabilityInputError(
                        f"{label} must not traverse a symlink: {normalized}"
                    )
        except FileNotFoundError as exc:
            raise PlannerPortabilityInputError(
                f"{label} is missing: {normalized}"
            ) from exc
        except OSError as exc:
            raise PlannerPortabilityInputError(
                f"cannot inspect {label} {normalized}: {exc}"
            ) from exc

        if not stat.S_ISREG(metadata.st_mode):
            raise PlannerPortabilityInputError(
                f"{label} must be a regular file: {normalized}"
            )
        try:
            resolved = candidate.resolve(strict=True)
            resolved.relative_to(self._resolved_root)
        except (OSError, ValueError) as exc:
            raise PlannerPortabilityInputError(
                f"{label} escapes the checker root: {normalized}"
            ) from exc
        return resolved

    def read_text(self, relative_path: object, *, label: str) -> str:
        resolved = self._checked_path(relative_path, label=label)
        cached = self._cache.get(resolved)
        if cached is not None:
            return cached
        if len(self._cache) >= MAX_CORPUS_FILES:
            raise PlannerPortabilityBoundsError(
                f"checked file corpus exceeds the {MAX_CORPUS_FILES}-file bound"
            )

        flags = os.O_RDONLY
        flags |= getattr(os, "O_CLOEXEC", 0)
        flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            descriptor = os.open(resolved, flags)
        except OSError as exc:
            raise PlannerPortabilityInputError(
                f"cannot open {label} {relative_path}: {exc}"
            ) from exc
        try:
            metadata = os.fstat(descriptor)
            if not stat.S_ISREG(metadata.st_mode):
                raise PlannerPortabilityInputError(
                    f"{label} must remain a regular file: {relative_path}"
                )
            if metadata.st_size > MAX_FILE_BYTES:
                raise PlannerPortabilityBoundsError(
                    f"{label} exceeds the {MAX_FILE_BYTES}-byte per-file bound: "
                    f"{relative_path}"
                )
            payload = bytearray()
            while len(payload) <= MAX_FILE_BYTES:
                remaining = MAX_FILE_BYTES + 1 - len(payload)
                chunk = os.read(descriptor, min(1024 * 1024, remaining))
                if not chunk:
                    break
                payload.extend(chunk)
        except OSError as exc:
            raise PlannerPortabilityInputError(
                f"cannot read {label} {relative_path}: {exc}"
            ) from exc
        finally:
            os.close(descriptor)

        if len(payload) > MAX_FILE_BYTES:
            raise PlannerPortabilityBoundsError(
                f"{label} exceeds the {MAX_FILE_BYTES}-byte per-file bound: "
                f"{relative_path}"
            )
        if self._total_bytes + len(payload) > MAX_CORPUS_BYTES:
            raise PlannerPortabilityBoundsError(
                f"checked file corpus exceeds the {MAX_CORPUS_BYTES}-byte bound"
            )
        try:
            text = bytes(payload).decode("utf-8", errors="strict")
        except UnicodeDecodeError as exc:
            raise PlannerPortabilityInputError(
                f"{label} is not valid UTF-8: {relative_path}"
            ) from exc
        self._cache[resolved] = text
        self._total_bytes += len(payload)
        return text


def _validate_structured_value(value: object, *, label: str) -> None:
    remaining_nodes = [MAX_DOCUMENT_NODES]
    remaining_scalar_chars = [MAX_TOTAL_SCALAR_CHARS]
    active_containers: set[int] = set()

    def visit(item: object, *, path: str, depth: int) -> None:
        if depth > MAX_DOCUMENT_DEPTH:
            raise PlannerPortabilityBoundsError(
                f"{label}:{path} exceeds the {MAX_DOCUMENT_DEPTH}-level bound"
            )
        remaining_nodes[0] -= 1
        if remaining_nodes[0] < 0:
            raise PlannerPortabilityBoundsError(
                f"{label} exceeds the {MAX_DOCUMENT_NODES}-node bound"
            )

        if item is None or type(item) is bool:
            return
        if type(item) is str:
            if len(item) > MAX_SCALAR_CHARS:
                raise PlannerPortabilityBoundsError(
                    f"{label}:{path} exceeds the {MAX_SCALAR_CHARS}-character "
                    "scalar bound"
                )
            remaining_scalar_chars[0] -= len(item)
            if remaining_scalar_chars[0] < 0:
                raise PlannerPortabilityBoundsError(
                    f"{label} exceeds the {MAX_TOTAL_SCALAR_CHARS}-character "
                    "aggregate scalar bound"
                )
            return
        if type(item) is int:
            if item.bit_length() > MAX_INTEGER_BITS:
                raise PlannerPortabilityBoundsError(
                    f"{label}:{path} integer exceeds the "
                    f"{MAX_INTEGER_BITS}-bit bound"
                )
            return
        if type(item) is float:
            if not math.isfinite(item):
                raise PlannerPortabilityInputError(
                    f"{label}:{path} must contain only finite numbers"
                )
            return
        if type(item) not in (dict, list):
            raise PlannerPortabilityInputError(
                f"{label}:{path} must contain only JSON-compatible values"
            )

        identity = id(item)
        if identity in active_containers:
            raise PlannerPortabilityInputError(
                f"{label}:{path} contains a recursive container"
            )
        if len(item) > MAX_COLLECTION_ITEMS:
            raise PlannerPortabilityBoundsError(
                f"{label}:{path} exceeds the {MAX_COLLECTION_ITEMS}-item bound"
            )
        active_containers.add(identity)
        try:
            if type(item) is list:
                for index, child in enumerate(item):
                    visit(
                        child,
                        path=_child_diagnostic_path(path, f"[{index}]"),
                        depth=depth + 1,
                    )
            else:
                for key, child in item.items():
                    if type(key) is not str:
                        raise PlannerPortabilityInputError(
                            f"{label}:{path} mapping keys must be strings"
                        )
                    if len(key) > MAX_IDENTIFIER_CHARS:
                        raise PlannerPortabilityBoundsError(
                            f"{label}:{path} key exceeds the "
                            f"{MAX_IDENTIFIER_CHARS}-character bound"
                        )
                    visit(
                        key,
                        path=_child_diagnostic_path(path, ".<key>"),
                        depth=depth + 1,
                    )
                    visit(
                        child,
                        path=_child_diagnostic_path(path, f".{key}"),
                        depth=depth + 1,
                    )
        finally:
            active_containers.remove(identity)

    visit(value, path="root", depth=0)


def _reject_json_constant(value: str) -> None:
    raise PlannerPortabilityInputError(
        f"JSON document contains non-finite constant {value}"
    )


def _unique_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise PlannerPortabilityInputError(
                "JSON object contains duplicate key "
                f"{_safe_excerpt(key)!r}"
            )
        result[key] = value
    return result


def _parse_structured_text(
    text: str,
    *,
    suffix: str,
    label: str,
) -> object:
    _bounded_text(
        text,
        label=f"{label} text",
        limit=MAX_FILE_BYTES,
        allow_empty=True,
    )
    try:
        if suffix == ".json":
            parsed = json.loads(
                text,
                object_pairs_hook=_unique_json_object,
                parse_constant=_reject_json_constant,
            )
        elif suffix in (".yaml", ".yml"):
            parsed = yaml.load(text, Loader=_UniqueKeySafeLoader)
        else:
            raise PlannerPortabilityInputError(
                f"{label} has unsupported structured-data suffix {suffix!r}"
            )
    except PlannerPortabilityInputError:
        raise
    except (
        json.JSONDecodeError,
        yaml.YAMLError,
        RecursionError,
        OverflowError,
        ValueError,
    ) as exc:
        raise PlannerPortabilityInputError(
            f"cannot parse {label}: {exc}"
        ) from exc
    _validate_structured_value(parsed, label=label)
    return parsed


def _mapping_document(parsed: object) -> dict[str, object]:
    if type(parsed) is not dict:
        raise PlannerPortabilityInputError(
            "planner portability mapping must be a top-level object"
        )
    missing = _TOP_LEVEL_FIELDS - set(parsed)
    unexpected = set(parsed) - _TOP_LEVEL_FIELDS
    if missing:
        raise PlannerPortabilityInputError(
            f"planner portability mapping is missing fields {sorted(missing)}"
        )
    if unexpected:
        raise PlannerPortabilityInputError(
            f"planner portability mapping has unexpected fields "
            f"{[_safe_excerpt(value) for value in sorted(unexpected)[:8]]}"
        )
    return parsed


def require_mapping_fields(
    failures: list[str], value: object, fields: set[str], location: str
) -> dict[str, object]:
    if type(value) is not dict:
        fail(failures, f"{location}: expected object")
        return {}
    missing = fields - set(value)
    if missing:
        fail(failures, f"{location}: missing fields {sorted(missing)}")
    return value


def _string_set(
    failures: list[str], value: object, *, location: str
) -> set[str]:
    if type(value) is not list:
        fail(failures, f"{location}: expected array")
        return set()
    result: set[str] = set()
    for index, item in enumerate(value):
        if type(item) is not str or not item:
            fail(failures, f"{location}[{index}]: expected non-empty string")
            continue
        if len(item) > MAX_IDENTIFIER_CHARS:
            fail(
                failures,
                f"{location}[{index}]: identifier exceeds "
                f"{MAX_IDENTIFIER_CHARS} characters",
            )
            continue
        if item in result:
            fail(failures, f"{location}: duplicate value {item!r}")
        result.add(item)
    return result


def _object(
    failures: list[str], value: object, *, location: str
) -> dict[str, object]:
    if type(value) is not dict:
        fail(failures, f"{location}: expected object")
        return {}
    return value


def _string_value_mapping(
    failures: list[str], value: object, *, location: str
) -> dict[str, str]:
    mapping = _object(failures, value, location=location)
    result: dict[str, str] = {}
    for key, item in mapping.items():
        if type(item) is not str or not item:
            fail(failures, f"{location}.{key}: expected non-empty string")
            continue
        if len(item) > MAX_IDENTIFIER_CHARS:
            fail(
                failures,
                f"{location}.{key}: value exceeds "
                f"{MAX_IDENTIFIER_CHARS} characters",
            )
            continue
        result[key] = item
    return result


def _array(failures: list[str], value: object, *, location: str) -> list[object]:
    if type(value) is not list:
        fail(failures, f"{location}: expected array")
        return []
    return value


def iter_mapping_keys(value: object, path: str = "root") -> Iterator[tuple[str, str]]:
    if type(value) is dict:
        for key, child in value.items():
            yield path, key
            yield from iter_mapping_keys(
                child,
                _child_diagnostic_path(path, f".{key}"),
            )
    elif type(value) is list:
        for index, child in enumerate(value):
            yield from iter_mapping_keys(
                child,
                _child_diagnostic_path(path, f"[{index}]"),
            )


def _relative_to_root(root: Path, path: Path, *, label: str) -> str:
    try:
        relative = path.relative_to(root)
    except ValueError as exc:
        raise PlannerPortabilityInputError(
            f"{label} is outside the checker root: {path}"
        ) from exc
    return _normalized_relative_path(relative.as_posix(), label=label)


def _read_or_fail(
    failures: list[str],
    corpus: BoundedFileCorpus,
    relative_path: str,
    *,
    label: str,
) -> str | None:
    try:
        return corpus.read_text(relative_path, label=label)
    except PlannerPortabilityBoundsError:
        raise
    except PlannerPortabilityInputError as exc:
        fail(failures, str(exc))
        return None


def _check_evidence(
    failures: list[str],
    evidence_value: object,
    corpus: BoundedFileCorpus,
) -> int:
    evidence = _array(failures, evidence_value, location="test_references")
    found_requirements: set[str] = set()
    for index, raw_entry in enumerate(evidence):
        location = f"test_references[{index}]"
        entry = require_mapping_fields(
            failures,
            raw_entry,
            {"requirement", "reference"},
            location,
        )
        if not entry:
            continue
        if set(entry) != {"requirement", "reference"}:
            fail(failures, f"{location}: expected only requirement/reference")
            continue
        try:
            requirement = _bounded_text(
                entry.get("requirement"), label=f"{location}.requirement"
            )
            reference = _bounded_text(
                entry.get("reference"),
                label=f"{location}.reference",
                limit=MAX_SCALAR_CHARS,
            )
        except PlannerPortabilityInputError as exc:
            fail(failures, str(exc))
            continue

        if requirement in found_requirements:
            fail(
                failures,
                f"{location}: duplicate evidence requirement {requirement!r}",
            )
            continue
        found_requirements.add(requirement)
        if requirement not in REQUIRED_EVIDENCE:
            continue
        if reference.count("::") != 1:
            fail(failures, f"{location}: reference must contain one '::'")
            continue
        relative_path, identifier = reference.split("::", 1)
        try:
            relative_path = _normalized_relative_path(
                relative_path, label=f"{location}.reference path"
            )
            identifier = _bounded_text(
                identifier,
                label=f"{location}.reference identifier",
                limit=MAX_IDENTIFIER_CHARS,
            )
        except PlannerPortabilityInputError as exc:
            fail(failures, str(exc))
            continue
        source = _read_or_fail(
            failures,
            corpus,
            relative_path,
            label=f"evidence source for {requirement}",
        )
        if source is not None and identifier not in source:
            fail(
                failures,
                f"test reference identifier is missing: "
                f"{relative_path}::{identifier}",
            )

    missing = REQUIRED_EVIDENCE - found_requirements
    unexpected = found_requirements - REQUIRED_EVIDENCE
    if missing:
        fail(failures, f"missing required evidence rows: {sorted(missing)}")
    if unexpected:
        fail(failures, f"unexpected evidence rows: {sorted(unexpected)}")
    return len(evidence)


def _collect_program_artifacts(root: Path) -> list[str]:
    artifacts: list[str] = []
    traversed_entries = 0
    for relative_root in _PROGRAM_ARTIFACT_ROOTS:
        start = root
        try:
            for part in PurePosixPath(relative_root).parts:
                start = start / part
                metadata = start.lstat()
                if stat.S_ISLNK(metadata.st_mode):
                    raise PlannerPortabilityInputError(
                        "artifact root must not traverse a symlink: "
                        f"{relative_root}"
                    )
        except FileNotFoundError as exc:
            raise PlannerPortabilityInputError(
                f"required artifact root is missing: {relative_root}"
            ) from exc
        except OSError as exc:
            raise PlannerPortabilityInputError(
                f"cannot inspect artifact root {relative_root}: {exc}"
            ) from exc
        if stat.S_ISLNK(metadata.st_mode):
            raise PlannerPortabilityInputError(
                f"artifact root must not be a symlink: {relative_root}"
            )
        if not stat.S_ISDIR(metadata.st_mode):
            raise PlannerPortabilityInputError(
                f"artifact root must be a directory: {relative_root}"
            )

        stack: list[tuple[Path, int]] = [(start, 0)]
        while stack:
            directory, depth = stack.pop()
            if depth > MAX_TRAVERSAL_DEPTH:
                raise PlannerPortabilityBoundsError(
                    f"artifact traversal exceeds {MAX_TRAVERSAL_DEPTH} levels"
                )
            try:
                rows: list[tuple[str, Path, int]] = []
                with os.scandir(directory) as entries:
                    for entry in entries:
                        traversed_entries += 1
                        if traversed_entries > MAX_TRAVERSAL_ENTRIES:
                            raise PlannerPortabilityBoundsError(
                                "artifact traversal exceeds the "
                                f"{MAX_TRAVERSAL_ENTRIES}-entry bound"
                            )
                        entry_metadata = entry.stat(follow_symlinks=False)
                        rows.append(
                            (entry.name, Path(entry.path), entry_metadata.st_mode)
                        )
            except PlannerPortabilityInputError:
                raise
            except OSError as exc:
                relative = directory.relative_to(root).as_posix()
                raise PlannerPortabilityInputError(
                    f"cannot traverse artifact directory {relative}: {exc}"
                ) from exc

            for _name, path, mode in sorted(
                rows,
                key=lambda row: row[0],
                reverse=True,
            ):
                relative = path.relative_to(root).as_posix()
                if stat.S_ISLNK(mode):
                    raise PlannerPortabilityInputError(
                        f"artifact traversal refuses symlink: {relative}"
                    )
                if stat.S_ISDIR(mode):
                    stack.append((path, depth + 1))
                    continue
                if not stat.S_ISREG(mode):
                    raise PlannerPortabilityInputError(
                        f"artifact traversal found non-regular entry: {relative}"
                    )
                if path.suffix.lower() in _PROGRAM_ARTIFACT_SUFFIXES:
                    artifacts.append(relative)
                    if len(artifacts) > MAX_CORPUS_FILES:
                        raise PlannerPortabilityBoundsError(
                            "program artifact corpus exceeds the "
                            f"{MAX_CORPUS_FILES}-file bound"
                        )
    return sorted(artifacts)


def _run_check(root: Path, mapping: Path) -> tuple[list[str], int]:
    corpus = BoundedFileCorpus(root)
    lexical_root = Path(os.path.abspath(root))
    lexical_mapping = Path(os.path.abspath(mapping))
    mapping_relative = _relative_to_root(
        lexical_root,
        lexical_mapping,
        label="planner portability mapping",
    )
    mapping_text = corpus.read_text(
        mapping_relative,
        label="planner portability mapping",
    )
    parsed = _parse_structured_text(
        mapping_text,
        suffix=".json",
        label="planner portability mapping",
    )
    document = _mapping_document(parsed)
    failures: list[str] = []

    if document.get("schema_version") != "manyforge.planner_portability.v1":
        fail(failures, "mapping has wrong schema_version")
    try:
        _bounded_text(document.get("contract_revision"), label="contract_revision")
    except PlannerPortabilityInputError as exc:
        fail(failures, str(exc))

    if (
        _string_set(
            failures,
            document.get("portable_capabilities"),
            location="portable_capabilities",
        )
        != REQUIRED_CAPABILITIES
    ):
        fail(failures, "portable_capabilities does not match the required contract")
    if (
        _string_set(
            failures,
            document.get("portable_profiles"),
            location="portable_profiles",
        )
        != REQUIRED_PROFILES
    ):
        fail(failures, "portable_profiles does not match the required contract")

    availability = _object(
        failures,
        document.get("runtime_availability"),
        location="runtime_availability",
    )
    if availability.get("status") != "temporarily_unavailable_composable_cell_rebuild":
        fail(
            failures,
            "planner runtime availability must record the composable-cell outage",
        )
    if (
        availability.get("available_surface")
        != "retained_planning_product_consumer_qualification_only"
    ):
        fail(
            failures,
            "runtime availability overclaims the retained-consumer-only surface",
        )

    authority = _object(
        failures,
        document.get("authority"),
        location="authority",
    )
    expected_authority = {
        "logical_scene": "manyforge_scene",
        "non_collision_validation": "manyforge_core.ITrajectoryValidator",
        "collision_validation": "manyforge_core.ICollisionQueryEngine",
        "dispatch": "manyforge_core.ExecutionKernel",
        "runtime_collision": "manyforge_core.RuntimeCollisionMonitor",
        "stop_interrupt_recovery": "manyforge_core.ExecutionKernel",
    }
    for key, expected in expected_authority.items():
        if authority.get(key) != expected:
            fail(failures, f"authority.{key}: expected {expected!r}")

    adapters = require_mapping_fields(
        failures,
        document.get("adapters"),
        {"deterministic_fake", "roboplan_0_5_1", "moveit2", "curobo"},
        "adapters",
    )
    fake = _object(
        failures,
        adapters.get("deterministic_fake"),
        location="adapters.deterministic_fake",
    )
    if fake.get("status") != "test_only_implemented":
        fail(failures, "deterministic_fake is not marked test_only_implemented")

    roboplan = _object(
        failures,
        adapters.get("roboplan_0_5_1"),
        location="adapters.roboplan_0_5_1",
    )
    if roboplan.get("status") != "temporarily_unavailable_composable_cell_rebuild":
        fail(failures, "RoboPlan runtime is not explicitly marked unavailable")
    if (
        roboplan.get("available_surface")
        != "retained_planning_product_consumer_qualification_only"
    ):
        fail(
            failures,
            "RoboPlan overclaims more than retained-product consumer qualification",
        )
    capability_disposition = _string_value_mapping(
        failures,
        roboplan.get("capability_disposition"),
        location="adapters.roboplan_0_5_1.capability_disposition",
    )
    if set(capability_disposition) != REQUIRED_CAPABILITIES:
        fail(failures, "RoboPlan mapping does not cover every portable capability")
    elif set(capability_disposition.values()) != {"temporarily_unavailable"}:
        fail(failures, "RoboPlan capability mapping contains a runtime support claim")
    profile_disposition = _string_value_mapping(
        failures,
        roboplan.get("profile_disposition"),
        location="adapters.roboplan_0_5_1.profile_disposition",
    )
    if set(profile_disposition) != REQUIRED_PROFILES:
        fail(failures, "RoboPlan mapping does not cover every portable profile")
    elif set(profile_disposition.values()) != {"temporarily_unavailable"}:
        fail(failures, "RoboPlan profile mapping contains a runtime support claim")

    moveit = _object(
        failures,
        adapters.get("moveit2"),
        location="adapters.moveit2",
    )
    require_mapping_fields(
        failures,
        moveit,
        {
            "status",
            "proposed_package",
            "dependency_boundary",
            "composition",
            "retained_product_input",
            "ports",
            "capability_disposition",
            "profile_disposition",
            "adapter_local_gaps",
            "forbidden_shortcuts",
        },
        "adapters.moveit2",
    )
    if moveit.get("status") != "fully_mapped_not_implemented":
        fail(failures, "MoveIt2 status must remain fully_mapped_not_implemented")
    moveit_capabilities = _object(
        failures,
        moveit.get("capability_disposition"),
        location="adapters.moveit2.capability_disposition",
    )
    if set(moveit_capabilities) != REQUIRED_CAPABILITIES:
        fail(failures, "MoveIt2 mapping does not cover every portable capability")
    moveit_profiles = _object(
        failures,
        moveit.get("profile_disposition"),
        location="adapters.moveit2.profile_disposition",
    )
    if set(moveit_profiles) != REQUIRED_PROFILES:
        fail(failures, "MoveIt2 mapping does not cover every portable profile")
    required_ports = {
        "IPlannerBackend",
        "IRobotKinematics",
        "IReferencePoseResolver",
        "IExecutionStateSink",
    }
    moveit_ports = _object(
        failures,
        moveit.get("ports"),
        location="adapters.moveit2.ports",
    )
    if set(moveit_ports) != required_ports:
        fail(failures, "MoveIt2 mapping does not cover every focused adapter port")
    adapter_local_gaps = _array(
        failures,
        moveit.get("adapter_local_gaps"),
        location="adapters.moveit2.adapter_local_gaps",
    )
    if len(adapter_local_gaps) < 5:
        fail(failures, "MoveIt2 adapter-local gaps are incomplete")

    serialized_moveit = json.dumps(moveit, sort_keys=True).lower()
    for backend_option in FORBIDDEN_BACKEND_OPTION_KEYS:
        if backend_option in serialized_moveit:
            fail(failures, f"MoveIt2 mapping leaks backend option {backend_option}")

    serialized_document = json.dumps(document, sort_keys=True)
    for deleted_surface in ("IPlannerSceneProjection", "ISceneAuthority"):
        if deleted_surface in serialized_document:
            fail(failures, f"mapping references deleted surface {deleted_surface}")

    curobo = _object(
        failures,
        adapters.get("curobo"),
        location="adapters.curobo",
    )
    if curobo.get("status") != "explicitly_unsupported_deferred":
        fail(failures, "cuRobo must remain explicitly unsupported and deferred")
    if curobo.get("implementation_changes_in_rp5_6") != "none":
        fail(failures, "RP5-6 must not implement or modify a cuRobo adapter")

    evidence_count = _check_evidence(
        failures,
        document.get("test_references"),
        corpus,
    )

    harness_files = (
        "manyforge_core/test_support/include/manyforge_core/testing/"
        "planner_backend_conformance.hpp",
        "manyforge_core/test_support/src/planner_backend_conformance.cpp",
    )
    forbidden_harness_dependency = re.compile(
        r"(?:#\s*include\s*[<\"](?:manyforge_roboplan|moveit|curobo)|"
        r"\b(?:roboplan|moveit|curobo)::|#\s*include\s*[<\"]gtest)",
        re.IGNORECASE,
    )
    for relative in harness_files:
        source = _read_or_fail(
            failures,
            corpus,
            relative,
            label="reusable planner harness source",
        )
        if source is not None and forbidden_harness_dependency.search(source):
            fail(
                failures,
                f"{relative} imports a concrete planner or GTest",
            )

    plan_request_relative = (
        "manyforge_core/include/manyforge_core/types/plan_request.hpp"
    )
    plan_request = _read_or_fail(
        failures,
        corpus,
        plan_request_relative,
        label="neutral PlanRequest DTO source",
    )
    if plan_request is not None:
        for field_name in ("target_mode", "target_reference", "offset_reference"):
            if re.search(rf"\b{field_name}\b", plan_request):
                fail(
                    failures,
                    f"PlanRequestDTO contains unresolved program field {field_name}",
                )

    portable_source_paths = (
        plan_request_relative,
        "manyforge_core/include/manyforge_core/types/motion_spec.hpp",
    )
    portable_source_values: list[str] = []
    for relative in portable_source_paths:
        source = _read_or_fail(
            failures,
            corpus,
            relative,
            label="neutral planning DTO source",
        )
        if source is not None:
            portable_source_values.append(source)
    portable_sources = "\n".join(portable_source_values).lower()
    for backend_option in FORBIDDEN_BACKEND_OPTION_KEYS:
        if backend_option in portable_sources:
            fail(failures, f"neutral DTO surface leaks backend option {backend_option}")

    program_sources = (
        "manyforge_behavior/manyforge_behavior/program.py",
        "manyforge_behavior/manyforge_behavior/catalog_schema.py",
        "manyforge_behavior/manyforge_behavior/nodes/motion_scoped.py",
    )
    for relative in program_sources:
        source = _read_or_fail(
            failures,
            corpus,
            relative,
            label="neutral program source",
        )
        if source is None:
            continue
        source_text = source.lower()
        for backend_option in FORBIDDEN_BACKEND_OPTION_KEYS:
            if re.search(rf"[\"']{re.escape(backend_option)}[\"']", source_text):
                fail(
                    failures,
                    "neutral program surface declares backend option "
                    f"{backend_option}: {relative}",
                )

    for relative in _collect_program_artifacts(corpus.resolved_root):
        artifact_text = corpus.read_text(
            relative,
            label="program artifact",
        )
        try:
            parsed_artifact = _parse_structured_text(
                artifact_text,
                suffix=Path(relative).suffix,
                label=f"program artifact {relative}",
            )
        except PlannerPortabilityInputError as exc:
            fail(failures, str(exc))
            continue
        for object_path, key in iter_mapping_keys(parsed_artifact):
            if key.lower() in FORBIDDEN_BACKEND_OPTION_KEYS:
                fail(
                    failures,
                    "program artifact declares backend-native option "
                    f"{key}: {relative}:{object_path}",
                )

    return failures, evidence_count


def main() -> int:
    try:
        failures, evidence_count = _run_check(ROOT, MAPPING)
    except (PlannerPortabilityInputError, OSError) as exc:
        print(f"invalid planner portability input: {exc}", file=sys.stderr)
        return 1

    if failures:
        print("Planner portability check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        "Planner portability check passed "
        f"({evidence_count} evidence rows; RoboPlan runtime explicitly unavailable)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
