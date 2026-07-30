"""Build-owned immutable v2 node-environment identity.

Runtime health and implementation availability are intentionally absent.  The
environment binds portable catalog meaning to stable executable identities and
exact package/source provenance; a later headless owner projects availability
without changing this identity.
"""

from __future__ import annotations

import re
import tomllib
import unicodedata
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    compute_content_identity,
    load_strict_json_yaml,
    raw_sha256_digest,
    require_sha256_digest,
    verify_content_identity,
)
from manyforge_behavior.catalog import (
    BUILTIN_CATALOG_ID,
    EXPECTED_API_VERSION,
    EXPECTED_BUILTIN_CATALOG_DIGEST,
    EXPECTED_CONTENT_VERSION,
    CatalogValidationError,
    NodeCatalog,
)
from manyforge_behavior.node_registry import (
    BUILTIN_EXECUTABLE_PACKAGE_ID,
    ExecutableRegistryRow,
    NodeRegistry,
    NodeRegistryError,
    build_node_registry,
)


NODE_ENVIRONMENT_MANIFEST_API = "manyforge.node_environment_manifest.v1"
NODE_ENVIRONMENT_IDENTITY_DOMAIN = "node-environment-digest"
EXECUTABLE_PACKAGE_MANIFEST_API = "manyforge.executable_package.v1"
EXECUTABLE_PACKAGE_IDENTITY_DOMAIN = "executable-package-content"
PYTHON_SOURCE_PACKAGE_FORMAT = "manyforge.python-source-package.v1"
MAX_EXECUTABLE_REGISTRY_ROWS = 4096
MAX_EXECUTABLE_PACKAGE_ROWS = 256
MAX_SOURCE_COVERAGE_ROWS = 65_536
MAX_MANIFEST_TEXT_CHARACTERS = 1024
_IDENTIFIER_RE = re.compile(r"^[a-z][a-z0-9_.-]*$")
_SOURCE_ID_RE = re.compile(
    r"^(?:python-module:[a-z][a-z0-9_.]*|"
    r"python-project:manyforge_behavior)$"
)
_ZERO_DIGEST = "sha256:" + ("0" * 64)
_PACKAGE_ROOT = Path(__file__).resolve().parent
_BEHAVIOR_ROOT = _PACKAGE_ROOT.parent
AUTHORITATIVE_PROJECT_PATH = _BEHAVIOR_ROOT / "pyproject.toml"
PACKAGED_PROJECT_SNAPSHOT_PATH = (
    _PACKAGE_ROOT
    / "_retained_source"
    / "manyforge_behavior.pyproject.toml"
)
BUILTIN_CATALOG_PATH = _BEHAVIOR_ROOT / "resources" / "node_catalog.yaml"
BUILTIN_ENVIRONMENT_MANIFEST_PATH = (
    _BEHAVIOR_ROOT / "resources" / "node_environment_manifest.yaml"
)
class NodeEnvironmentBuildError(ValueError):
    """The build-owned node environment is invalid or stale."""


@dataclass(frozen=True, order=True)
class ExecutablePackageRecord:
    executable_package_id: str
    package_version: str
    package_format: str
    content_digest: str

    def to_dict(self) -> dict[str, str]:
        return {
            "executable_package_id": self.executable_package_id,
            "package_version": self.package_version,
            "package_format": self.package_format,
            "content_digest": self.content_digest,
        }


@dataclass(frozen=True, order=True)
class SourceCoverageRecord:
    executable_package_id: str
    source_id: str
    content_digest: str

    def to_dict(self) -> dict[str, str]:
        return {
            "executable_package_id": self.executable_package_id,
            "source_id": self.source_id,
            "content_digest": self.content_digest,
        }


@dataclass(frozen=True)
class NodeEnvironmentManifest:
    environment_digest: str
    catalog_api_version: str
    catalog_id: str
    catalog_content_version: str
    catalog_digest: str
    executable_registry: tuple[ExecutableRegistryRow, ...]
    executable_packages: tuple[ExecutablePackageRecord, ...]
    source_coverage: tuple[SourceCoverageRecord, ...]

    def to_dict(self) -> dict[str, Any]:
        return {
            "api_version": NODE_ENVIRONMENT_MANIFEST_API,
            "environment_digest": self.environment_digest,
            "catalog": {
                "api_version": self.catalog_api_version,
                "catalog_id": self.catalog_id,
                "content_version": self.catalog_content_version,
                "catalog_digest": self.catalog_digest,
            },
            "executable_registry": [
                record.to_dict() for record in self.executable_registry
            ],
            "executable_packages": [
                record.to_dict() for record in self.executable_packages
            ],
            "source_coverage": [
                record.to_dict() for record in self.source_coverage
            ],
        }


@dataclass(frozen=True)
class NodeEnvironment:
    catalog: NodeCatalog
    registry: NodeRegistry
    manifest: NodeEnvironmentManifest

    @property
    def environment_digest(self) -> str:
        return self.manifest.environment_digest


def _require_exact_fields(
    value: dict[str, Any],
    expected: set[str],
    label: str,
) -> None:
    if set(value) != expected:
        missing = sorted(expected - set(value))
        unknown = sorted(set(value) - expected)
        raise NodeEnvironmentBuildError(
            f"{label} has wrong fields; missing={missing}, unknown={unknown}"
        )


def _require_text(
    value: Any,
    label: str,
    *,
    max_characters: int = MAX_MANIFEST_TEXT_CHARACTERS,
) -> str:
    if not isinstance(value, str) or not value:
        raise NodeEnvironmentBuildError(f"{label} must be a non-empty string")
    if len(value) > max_characters:
        raise NodeEnvironmentBuildError(
            f"{label} exceeds {max_characters} characters"
        )
    if any(unicodedata.category(character) == "Cc" for character in value):
        raise NodeEnvironmentBuildError(f"{label} contains control characters")
    return value


def _require_identifier(value: Any, label: str) -> str:
    text = _require_text(value, label)
    if _IDENTIFIER_RE.fullmatch(text) is None:
        raise NodeEnvironmentBuildError(
            f"{label} must be a stable lowercase identifier"
        )
    return text


def _require_digest(value: Any, label: str) -> str:
    try:
        return require_sha256_digest(value, label)
    except StrictArtifactError as exc:
        raise NodeEnvironmentBuildError(str(exc)) from exc


def _require_bounded_list(
    value: Any,
    label: str,
    *,
    maximum: int,
) -> list[Any]:
    if not isinstance(value, list) or not value:
        raise NodeEnvironmentBuildError(f"{label} must be a non-empty list")
    if len(value) > maximum:
        raise NodeEnvironmentBuildError(
            f"{label} contains more than {maximum} rows"
        )
    return value


def _source_id(path: Path) -> str:
    relative = path.relative_to(_PACKAGE_ROOT)
    if relative.suffix not in {".py", ".pyi"}:
        raise NodeEnvironmentBuildError(
            f"unsupported build-owned source suffix: {relative.suffix}"
        )
    module_parts = list(relative.with_suffix("").parts)
    if module_parts[-1] == "__init__":
        module_parts.pop()
    module = ".".join((BUILTIN_EXECUTABLE_PACKAGE_ID, *module_parts))
    return f"python-module:{module}"


def _current_source_coverage() -> tuple[SourceCoverageRecord, ...]:
    source_paths = sorted(
        path
        for suffix in ("*.py", "*.pyi")
        for path in _PACKAGE_ROOT.rglob(suffix)
        if "__pycache__" not in path.parts
    )
    if not source_paths:
        raise NodeEnvironmentBuildError(
            "manyforge_behavior executable package has no covered Python sources"
        )
    try:
        rows = tuple(
            sorted(
                (
                    *(
                        SourceCoverageRecord(
                            executable_package_id=BUILTIN_EXECUTABLE_PACKAGE_ID,
                            source_id=_source_id(path),
                            content_digest=raw_sha256_digest(path.read_bytes()),
                        )
                        for path in source_paths
                    ),
                    SourceCoverageRecord(
                        executable_package_id=BUILTIN_EXECUTABLE_PACKAGE_ID,
                        source_id=(
                            f"python-project:{BUILTIN_EXECUTABLE_PACKAGE_ID}"
                        ),
                        content_digest=raw_sha256_digest(
                            PACKAGED_PROJECT_SNAPSHOT_PATH.read_bytes()
                        ),
                    ),
                )
            )
        )
    except OSError as exc:
        raise NodeEnvironmentBuildError(
            f"cannot read build-owned executable package source: {exc}"
        ) from exc
    if len(rows) > MAX_SOURCE_COVERAGE_ROWS:
        raise NodeEnvironmentBuildError(
            "build-owned package exceeds the source-coverage row limit"
        )
    if len({row.source_id for row in rows}) != len(rows):
        raise NodeEnvironmentBuildError(
            "build-owned package source IDs are not unique"
        )
    return rows


def _package_version() -> str:
    project_path = PACKAGED_PROJECT_SNAPSHOT_PATH
    try:
        project_data = tomllib.loads(project_path.read_text(encoding="utf-8"))
        project = project_data["project"]
        declared_name = project["name"]
        declared_version = project["version"]
    except (OSError, UnicodeError, tomllib.TOMLDecodeError, KeyError, TypeError) as exc:
        raise NodeEnvironmentBuildError(
            f"cannot read build-owned package metadata from {project_path}: {exc}"
        ) from exc
    if declared_name != BUILTIN_EXECUTABLE_PACKAGE_ID:
        raise NodeEnvironmentBuildError(
            "build-owned package metadata has the wrong project name"
        )
    if not isinstance(declared_version, str) or not declared_version:
        raise NodeEnvironmentBuildError(
            "build-owned package metadata has no project version"
        )
    return declared_version


def _package_content_digest(
    *,
    package_version: str,
    source_coverage: tuple[SourceCoverageRecord, ...],
) -> str:
    package_document: dict[str, Any] = {
        "api_version": EXECUTABLE_PACKAGE_MANIFEST_API,
        "content_digest": _ZERO_DIGEST,
        "executable_package_id": BUILTIN_EXECUTABLE_PACKAGE_ID,
        "package_version": package_version,
        "package_format": PYTHON_SOURCE_PACKAGE_FORMAT,
        "sources": [
            {
                "source_id": row.source_id,
                "content_digest": row.content_digest,
            }
            for row in source_coverage
        ],
    }
    return compute_content_identity(
        package_document,
        own_field="content_digest",
        identity_domain=EXECUTABLE_PACKAGE_IDENTITY_DOMAIN,
        api_version=EXECUTABLE_PACKAGE_MANIFEST_API,
    )


def generate_builtin_node_environment_manifest() -> NodeEnvironmentManifest:
    """Generate the sole build-owned candidate from packaged source inputs."""

    try:
        catalog = NodeCatalog(BUILTIN_CATALOG_PATH)
        registry = build_node_registry(catalog)
    except (CatalogValidationError, NodeRegistryError) as exc:
        raise NodeEnvironmentBuildError(str(exc)) from exc
    source_coverage = _current_source_coverage()
    package_version = _package_version()
    package = ExecutablePackageRecord(
        executable_package_id=BUILTIN_EXECUTABLE_PACKAGE_ID,
        package_version=package_version,
        package_format=PYTHON_SOURCE_PACKAGE_FORMAT,
        content_digest=_package_content_digest(
            package_version=package_version,
            source_coverage=source_coverage,
        ),
    )
    candidate = NodeEnvironmentManifest(
        environment_digest=_ZERO_DIGEST,
        catalog_api_version=catalog.api_version,
        catalog_id=catalog.catalog_id,
        catalog_content_version=catalog.content_version,
        catalog_digest=catalog.catalog_digest,
        executable_registry=registry.rows,
        executable_packages=(package,),
        source_coverage=source_coverage,
    )
    identity = compute_content_identity(
        candidate.to_dict(),
        own_field="environment_digest",
        identity_domain=NODE_ENVIRONMENT_IDENTITY_DOMAIN,
        api_version=NODE_ENVIRONMENT_MANIFEST_API,
    )
    generated = NodeEnvironmentManifest(
        environment_digest=identity,
        catalog_api_version=candidate.catalog_api_version,
        catalog_id=candidate.catalog_id,
        catalog_content_version=candidate.catalog_content_version,
        catalog_digest=candidate.catalog_digest,
        executable_registry=candidate.executable_registry,
        executable_packages=candidate.executable_packages,
        source_coverage=candidate.source_coverage,
    )
    # Generation and ingestion share one closed-schema validator. This prevents
    # a newly introduced source filename or project value from producing an
    # invalid retained artifact that only fails on the next process start.
    return _parse_manifest(generated.to_dict())


def _parse_manifest(document: dict[str, Any]) -> NodeEnvironmentManifest:
    _require_exact_fields(
        document,
        {
            "api_version",
            "environment_digest",
            "catalog",
            "executable_registry",
            "executable_packages",
            "source_coverage",
        },
        "node environment manifest",
    )
    if document["api_version"] != NODE_ENVIRONMENT_MANIFEST_API:
        raise NodeEnvironmentBuildError(
            f"node environment api_version must be {NODE_ENVIRONMENT_MANIFEST_API!r}"
        )
    try:
        verify_content_identity(
            document,
            own_field="environment_digest",
            identity_domain=NODE_ENVIRONMENT_IDENTITY_DOMAIN,
            api_version=NODE_ENVIRONMENT_MANIFEST_API,
            label="node environment manifest",
        )
    except StrictArtifactError as exc:
        raise NodeEnvironmentBuildError(str(exc)) from exc

    catalog = document["catalog"]
    if not isinstance(catalog, dict):
        raise NodeEnvironmentBuildError("node environment catalog must be a mapping")
    _require_exact_fields(
        catalog,
        {"api_version", "catalog_id", "content_version", "catalog_digest"},
        "node environment catalog",
    )
    if catalog["api_version"] != EXPECTED_API_VERSION:
        raise NodeEnvironmentBuildError("node environment catalog API mismatch")
    if catalog["catalog_id"] != BUILTIN_CATALOG_ID:
        raise NodeEnvironmentBuildError("node environment catalog ID mismatch")
    if catalog["content_version"] != EXPECTED_CONTENT_VERSION:
        raise NodeEnvironmentBuildError(
            "node environment catalog content version mismatch"
        )
    catalog_digest = _require_digest(
        catalog["catalog_digest"],
        "node environment catalog.catalog_digest",
    )
    if catalog_digest != EXPECTED_BUILTIN_CATALOG_DIGEST:
        raise NodeEnvironmentBuildError(
            "node environment catalog digest is not the frozen v2 packet"
        )

    registry_rows: list[ExecutableRegistryRow] = []
    raw_registry = _require_bounded_list(
        document["executable_registry"],
        "node environment executable_registry",
        maximum=MAX_EXECUTABLE_REGISTRY_ROWS,
    )
    for index, raw in enumerate(raw_registry):
        if not isinstance(raw, dict):
            raise NodeEnvironmentBuildError(
                f"executable_registry[{index}] must be a mapping"
            )
        _require_exact_fields(
            raw,
            {
                "node_id",
                "engine_id",
                "implementation_id",
                "executable_package_id",
            },
            f"executable_registry[{index}]",
        )
        registry_rows.append(
            ExecutableRegistryRow(
                node_id=_require_identifier(
                    raw["node_id"],
                    f"registry[{index}].node_id",
                ),
                engine_id=_require_identifier(
                    raw["engine_id"],
                    f"registry[{index}].engine_id",
                ),
                implementation_id=_require_identifier(
                    raw["implementation_id"],
                    f"registry[{index}].implementation_id",
                ),
                executable_package_id=_require_identifier(
                    raw["executable_package_id"],
                    f"registry[{index}].executable_package_id",
                ),
            )
        )
    registry = tuple(registry_rows)
    if registry != tuple(sorted(registry)):
        raise NodeEnvironmentBuildError(
            "node environment executable_registry is not canonical"
        )
    if len({(row.node_id, row.engine_id) for row in registry}) != len(registry):
        raise NodeEnvironmentBuildError(
            "node environment executable_registry contains duplicate node/engine rows"
        )
    if any(
        row.engine_id != "py_trees"
        or row.executable_package_id != BUILTIN_EXECUTABLE_PACKAGE_ID
        for row in registry
    ):
        raise NodeEnvironmentBuildError(
            "node environment executable_registry contains an unqualified "
            "engine or executable package"
        )

    packages: list[ExecutablePackageRecord] = []
    raw_packages = _require_bounded_list(
        document["executable_packages"],
        "node environment executable_packages",
        maximum=MAX_EXECUTABLE_PACKAGE_ROWS,
    )
    for index, raw in enumerate(raw_packages):
        if not isinstance(raw, dict):
            raise NodeEnvironmentBuildError(
                f"executable_packages[{index}] must be a mapping"
            )
        _require_exact_fields(
            raw,
            {
                "executable_package_id",
                "package_version",
                "package_format",
                "content_digest",
            },
            f"executable_packages[{index}]",
        )
        packages.append(
            ExecutablePackageRecord(
                executable_package_id=_require_identifier(
                    raw["executable_package_id"],
                    f"packages[{index}].executable_package_id",
                ),
                package_version=_require_text(
                    raw["package_version"],
                    f"packages[{index}].package_version",
                ),
                package_format=_require_text(
                    raw["package_format"],
                    f"packages[{index}].package_format",
                ),
                content_digest=_require_digest(
                    raw["content_digest"],
                    f"packages[{index}].content_digest",
                ),
            )
        )
    package_rows = tuple(packages)
    if package_rows != tuple(sorted(package_rows)):
        raise NodeEnvironmentBuildError(
            "node environment executable_packages is not canonical"
        )
    package_ids = {row.executable_package_id for row in package_rows}
    if len(package_ids) != len(package_rows):
        raise NodeEnvironmentBuildError(
            "node environment executable_packages contains duplicate IDs"
        )
    if (
        len(package_rows) != 1
        or package_rows[0].executable_package_id
        != BUILTIN_EXECUTABLE_PACKAGE_ID
        or package_rows[0].package_format != PYTHON_SOURCE_PACKAGE_FORMAT
    ):
        raise NodeEnvironmentBuildError(
            "node environment executable package is not the qualified "
            "built-in source-package format"
        )

    coverage: list[SourceCoverageRecord] = []
    raw_coverage = _require_bounded_list(
        document["source_coverage"],
        "node environment source_coverage",
        maximum=MAX_SOURCE_COVERAGE_ROWS,
    )
    for index, raw in enumerate(raw_coverage):
        if not isinstance(raw, dict):
            raise NodeEnvironmentBuildError(
                f"source_coverage[{index}] must be a mapping"
            )
        _require_exact_fields(
            raw,
            {"executable_package_id", "source_id", "content_digest"},
            f"source_coverage[{index}]",
        )
        coverage.append(
            SourceCoverageRecord(
                executable_package_id=_require_identifier(
                    raw["executable_package_id"],
                    f"source_coverage[{index}].executable_package_id",
                ),
                source_id=_require_text(
                    raw["source_id"],
                    f"source_coverage[{index}].source_id",
                ),
                content_digest=_require_digest(
                    raw["content_digest"],
                    f"source_coverage[{index}].content_digest",
                ),
            )
        )
    coverage_rows = tuple(coverage)
    if coverage_rows != tuple(sorted(coverage_rows)):
        raise NodeEnvironmentBuildError(
            "node environment source_coverage is not canonical"
        )
    if len(
        {
            (row.executable_package_id, row.source_id)
            for row in coverage_rows
        }
    ) != len(coverage_rows):
        raise NodeEnvironmentBuildError(
            "node environment source_coverage contains duplicate rows"
        )
    if any(
        row.executable_package_id != BUILTIN_EXECUTABLE_PACKAGE_ID
        or _SOURCE_ID_RE.fullmatch(row.source_id) is None
        for row in coverage_rows
    ):
        raise NodeEnvironmentBuildError(
            "node environment source_coverage contains an unqualified "
            "package or path-like source identity"
        )
    referenced_packages = {
        row.executable_package_id for row in registry
    } | {
        row.executable_package_id for row in coverage_rows
    }
    if referenced_packages != package_ids:
        raise NodeEnvironmentBuildError(
            "node environment package joins are not exact and total"
        )
    expected_package_digest = _package_content_digest(
        package_version=package_rows[0].package_version,
        source_coverage=coverage_rows,
    )
    if package_rows[0].content_digest != expected_package_digest:
        raise NodeEnvironmentBuildError(
            "node environment executable-package content digest does not "
            "match its canonical source coverage"
        )

    return NodeEnvironmentManifest(
        environment_digest=_require_digest(
            document["environment_digest"],
            "node environment manifest.environment_digest",
        ),
        catalog_api_version=str(catalog["api_version"]),
        catalog_id=str(catalog["catalog_id"]),
        catalog_content_version=str(catalog["content_version"]),
        catalog_digest=catalog_digest,
        executable_registry=registry,
        executable_packages=package_rows,
        source_coverage=coverage_rows,
    )


def load_builtin_node_environment_manifest() -> NodeEnvironmentManifest:
    """Verify the retained manifest and every build-owned freshness input."""

    try:
        serialized = load_strict_json_yaml(BUILTIN_ENVIRONMENT_MANIFEST_PATH)
    except StrictArtifactError as exc:
        raise NodeEnvironmentBuildError(str(exc)) from exc
    retained = _parse_manifest(serialized)
    current = generate_builtin_node_environment_manifest()
    if retained != current:
        raise NodeEnvironmentBuildError(
            "build-owned node environment manifest is stale; regenerate it "
            "after catalog, registry, package metadata, or covered source changes"
        )
    return retained


def build_node_environment() -> NodeEnvironment:
    """Build the sole immutable built-in environment.

    There is deliberately no catalog-path or caller-supplied digest argument.
    The descriptor, registry mapping rule, package identity, and manifest are
    build-owned.
    """

    try:
        catalog = NodeCatalog(BUILTIN_CATALOG_PATH)
        registry = build_node_registry(catalog)
        manifest = load_builtin_node_environment_manifest()
    except (CatalogValidationError, NodeRegistryError) as exc:
        raise NodeEnvironmentBuildError(str(exc)) from exc
    if manifest.catalog_digest != catalog.catalog_digest:
        raise NodeEnvironmentBuildError(
            "retained node environment catalog digest does not match descriptor"
        )
    if manifest.executable_registry != registry.rows:
        raise NodeEnvironmentBuildError(
            "retained node environment executable registry does not match catalog"
        )
    return NodeEnvironment(
        catalog=catalog,
        registry=registry,
        manifest=manifest,
    )
