"""Bounded discovery of exact v1 scenario artifact closures.

Discovery is an authoring/client operation.  It validates immutable artifact
identities and their cross-references, but it does not activate a deployment,
select runtime truth, compile a Program Binding, or dispatch robot work.
"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Final, Iterable, Protocol

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    canonical_json_bytes,
    load_strict_json_yaml,
    require_sha256_digest,
    verify_content_identity,
)
from manyforge_behavior.node_environment import NodeEnvironment

SCENARIO_CATALOG_API: Final = "manyforge.scenario_catalog.v1"
SCENARIO_CATALOG_IDENTITY_DOMAIN: Final = "scenario-catalog-revision"
PROGRAM_API: Final = "manyforge.program.v1"
DEPLOYMENT_API: Final = "manyforge.deployment.v1"
PROGRAM_BINDING_API: Final = "manyforge.program_binding.v1"
RETAINED_BUNDLE_API: Final = "manyforge.retained_bundle.v1"

_MAX_ROOTS: Final = 128
_MAX_SOURCE_ARTIFACTS: Final = 4096
_MAX_SCENARIOS: Final = 4096
_MAX_ALIASES_PER_SCENARIO: Final = 64
_MAX_TEXT_CHARACTERS: Final = 4096
_ID_RE = re.compile(r"^[a-z][a-z0-9_.-]{0,127}$")


class ScenarioDiscoveryError(ValueError):
    """Scenario artifacts are malformed, stale, missing, or ambiguous."""


def configured_scenario_root_values(value: str | None = None) -> tuple[str, ...]:
    """Return the one process-level scenario-root setting without defaults."""

    raw = os.environ.get("MANYFORGE_SCENARIO_ROOTS", "") if value is None else value
    return tuple(item for item in raw.split(os.pathsep) if item)


@dataclass(frozen=True, slots=True)
class Scenario:
    """One exact named artifact closure plus source-local authoring paths."""

    name: str
    lane: str
    assistant: str
    program_path: Path
    deployment_path: Path
    description: str
    aliases: tuple[str, ...] = ()
    runtime_resolver: str = ""
    cell_path: Path | None = None
    deployment_artifact_id: str = ""
    deployment_revision: str = ""
    retained_bundle_id: str = ""
    program_binding_id: str = ""
    program_id: str = ""
    program_revision: str = ""
    typed_parameter_overrides_json: str = "{}"


@dataclass(frozen=True, slots=True)
class ScenarioArtifactClosure:
    """Exact files and identities presented to the native closure verifier.

    Python discovery owns bounded enumeration and presentation metadata only.
    The verifier must reuse the qualified deployment, retained-bundle,
    Program, Program-Binding, and node-environment owners before returning.
    """

    scenario_id: str
    scenario_catalog_revision: str
    repository_root: Path
    scenario_catalog_path: Path
    deployment_path: Path
    retained_bundle_path: Path
    program_binding_path: Path
    program_path: Path
    deployment_artifact_id: str
    deployment_revision: str
    retained_bundle_id: str
    program_binding_id: str
    graph_revision: str
    program_id: str
    program_revision: str
    node_environment_digest: str
    assistant_mode: str
    typed_parameter_overrides_json: str


@dataclass(frozen=True, slots=True)
class VerifiedScenarioMetadata:
    """Presentation facts returned by the qualified semantic verifier."""

    lane: str
    assistant_enabled: bool


class ScenarioArtifactClosureVerifier(Protocol):
    """Qualified semantic verifier required by scenario discovery."""

    def verify(self, closure: ScenarioArtifactClosure, /) -> VerifiedScenarioMetadata:
        """Reject or return facts from the complete verified closure."""


class NativeScenarioArtifactClosureVerifier:
    """Production adapter over the qualified C++ cross-artifact verifier.

    The existing Program owner first validates catalog parameter/dataflow
    meaning and typed scenario overrides. C++ then independently joins the
    exact deployment, retained bundle, symbol map, Program Binding, Program,
    node environment, lane, and assistant closure. Neither side activates or
    dispatches anything.
    """

    def __init__(self, node_environment: NodeEnvironment) -> None:
        if not isinstance(node_environment, NodeEnvironment):
            raise TypeError("native scenario verifier requires one NodeEnvironment")
        self._node_environment = node_environment

    def verify(self, closure: ScenarioArtifactClosure, /) -> VerifiedScenarioMetadata:
        if not isinstance(closure, ScenarioArtifactClosure):
            raise TypeError("native scenario verifier requires one artifact closure")
        if closure.node_environment_digest != self._node_environment.environment_digest:
            raise ValueError("scenario closure selects another node environment")

        try:
            overrides = json.loads(closure.typed_parameter_overrides_json)
        except (TypeError, ValueError) as exc:
            raise ValueError("scenario parameter overrides are not JSON") from exc
        if type(overrides) is not dict:
            raise ValueError("scenario parameter overrides must be a mapping")

        # Reuse the qualified Program/catalog value owner instead of creating a
        # second Python schema implementation for scenario discovery.
        from manyforge_behavior.orchestration_values import (
            NodeAttemptInputResolver,
            ProgramValueStore,
        )
        from manyforge_behavior.program import Program

        program = Program(
            closure.program_path,
            catalog=self._node_environment.catalog,
        )
        for parameter_id in overrides:
            declaration = program.parameters.get(parameter_id)
            if declaration is None:
                raise ValueError(
                    f"scenario override selects undeclared parameter {parameter_id!r}"
                )
            if declaration.runtime_mutability == "immutable":
                raise ValueError(
                    f"scenario override selects immutable parameter {parameter_id!r}"
                )
        NodeAttemptInputResolver(
            program=program,
            catalog=self._node_environment.catalog,
            value_store=ProgramValueStore.from_program(program),
            program_parameter_values=overrides,
        )

        try:
            from manyforge_core_py import verify_scenario_artifact_closure
        except (ImportError, AttributeError) as exc:
            raise ValueError(
                "native scenario artifact verifier is unavailable in this build"
            ) from exc

        lane, assistant_enabled = verify_scenario_artifact_closure(
            closure.scenario_id,
            closure.scenario_catalog_revision,
            str(closure.repository_root),
            str(closure.scenario_catalog_path),
            str(closure.deployment_path),
            str(closure.retained_bundle_path),
            str(closure.program_binding_path),
            str(closure.program_path),
            closure.deployment_artifact_id,
            closure.deployment_revision,
            closure.retained_bundle_id,
            closure.program_binding_id,
            closure.graph_revision,
            closure.program_id,
            closure.program_revision,
            closure.node_environment_digest,
            closure.assistant_mode,
            closure.typed_parameter_overrides_json,
        )
        if type(lane) is not str or lane not in {
            "manyforge-only",
            "isaac-sim",
            "real-robot",
        }:
            raise ValueError("native scenario verifier returned an invalid lane")
        if type(assistant_enabled) is not bool:
            raise ValueError(
                "native scenario verifier returned invalid assistant state"
            )
        return VerifiedScenarioMetadata(
            lane=lane,
            assistant_enabled=assistant_enabled,
        )


class ScenarioCatalog:
    """Small deterministic name/alias index with no runtime ownership."""

    def __init__(self, scenarios: Iterable[Scenario]):
        by_name: dict[str, Scenario] = {}
        aliases: dict[str, str] = {}
        for scenario in sorted(scenarios, key=lambda item: item.name):
            if scenario.name in by_name or scenario.name in aliases:
                raise ScenarioDiscoveryError(
                    f"duplicate scenario identifier '{scenario.name}'"
                )
            by_name[scenario.name] = scenario
            for alias in scenario.aliases:
                if alias in by_name or alias in aliases:
                    raise ScenarioDiscoveryError(
                        f"duplicate scenario identifier '{alias}'"
                    )
                aliases[alias] = scenario.name
        self._by_name = by_name
        self._aliases = aliases

    @property
    def scenarios(self) -> tuple[Scenario, ...]:
        return tuple(self._by_name.values())

    def resolve(self, name: str) -> tuple[Scenario, bool]:
        canonical = self._aliases.get(name, name)
        try:
            return self._by_name[canonical], canonical != name
        except KeyError as exc:
            raise KeyError(name) from exc


@dataclass(frozen=True, slots=True)
class _SourceArtifact:
    path: Path
    value: dict[str, Any]


def _require_exact_fields(
    value: object,
    *,
    required: frozenset[str],
    optional: frozenset[str] = frozenset(),
    path: str,
) -> dict[str, Any]:
    if type(value) is not dict:
        raise ScenarioDiscoveryError(f"{path} must be a mapping")
    missing = sorted(required - set(value))
    unknown = sorted(set(value) - required - optional)
    if missing or unknown:
        raise ScenarioDiscoveryError(
            f"{path} has wrong fields; missing={missing}, unknown={unknown}"
        )
    return value


def _require_id(value: object, *, path: str) -> str:
    if type(value) is not str or _ID_RE.fullmatch(value) is None:
        raise ScenarioDiscoveryError(f"{path} must be a bounded lowercase identifier")
    return value


def _require_text(value: object, *, path: str) -> str:
    if type(value) is not str or not value.strip():
        raise ScenarioDiscoveryError(f"{path} must be non-empty text")
    if len(value) > _MAX_TEXT_CHARACTERS:
        raise ScenarioDiscoveryError(
            f"{path} exceeds {_MAX_TEXT_CHARACTERS} characters"
        )
    return value.strip()


def _require_digest(value: object, *, path: str) -> str:
    try:
        return require_sha256_digest(value, path)
    except StrictArtifactError as exc:
        raise ScenarioDiscoveryError(str(exc)) from exc


def _load(path: Path, *, label: str) -> dict[str, Any]:
    if path.is_symlink():
        raise ScenarioDiscoveryError(f"{label} must not be a symbolic link: {path}")
    try:
        return load_strict_json_yaml(path)
    except StrictArtifactError as exc:
        raise ScenarioDiscoveryError(f"{label} is invalid: {exc}") from exc


def _verify_identity(
    value: dict[str, Any],
    *,
    own_field: str,
    identity_domain: str,
    api_version: str,
    label: str,
) -> str:
    if value.get("api_version") != api_version:
        raise ScenarioDiscoveryError(f"{label}.api_version must be {api_version}")
    try:
        return verify_content_identity(
            value,
            own_field=own_field,
            identity_domain=identity_domain,
            api_version=api_version,
            label=label,
        )
    except StrictArtifactError as exc:
        raise ScenarioDiscoveryError(str(exc)) from exc


def _resolve_roots(roots: Iterable[str | Path]) -> tuple[Path, ...]:
    resolved: list[Path] = []
    for root_value in roots:
        if len(resolved) == _MAX_ROOTS:
            raise ScenarioDiscoveryError(f"scenario root count exceeds {_MAX_ROOTS}")
        try:
            root = Path(root_value).resolve(strict=True)
        except OSError as exc:
            raise ScenarioDiscoveryError(
                f"scenario root does not exist: {root_value}"
            ) from exc
        if not root.is_dir():
            raise ScenarioDiscoveryError(f"scenario root is not a directory: {root}")
        if root in resolved:
            raise ScenarioDiscoveryError(f"duplicate scenario root: {root}")
        resolved.append(root)
    return tuple(resolved)


def _index_sources(
    root: Path,
    *,
    suffix: str,
    api_version: str,
    own_id: str,
    own_revision: str,
    source_count: list[int],
) -> dict[tuple[str, str], _SourceArtifact]:
    result: dict[tuple[str, str], _SourceArtifact] = {}
    for path in sorted(root.glob(f"*{suffix}")):
        source_count[0] += 1
        if source_count[0] > _MAX_SOURCE_ARTIFACTS:
            raise ScenarioDiscoveryError(
                f"{suffix} source count exceeds {_MAX_SOURCE_ARTIFACTS}"
            )
        value = _load(path, label=f"source artifact {path}")
        # Other schema generations may share an authoring root. They are not
        # accepted or interpreted by this v1 index.
        if value.get("api_version") != api_version:
            continue
        # Full source identity is schema-normalized by the qualified native
        # verifier. Recomputing it here without that schema would create a
        # second, subtly different Program/deployment validator.
        revision = _require_digest(
            value.get(own_revision), path=f"{path}.{own_revision}"
        )
        key = (
            _require_id(value.get(own_id), path=f"{path}.{own_id}"),
            revision,
        )
        if key in result:
            raise ScenarioDiscoveryError(
                f"duplicate exact {api_version} source {key}: "
                f"{result[key].path} and {path}"
            )
        result[key] = _SourceArtifact(path=path.resolve(), value=value)
    return result


def _repository_path(root: Path, collection: str, digest: str) -> Path:
    return root / collection / "sha256" / f"{digest.removeprefix('sha256:')}.json"


def _load_repository_artifact(
    root: Path,
    *,
    collection: str,
    digest: str,
    api_version: str,
    own_field: str,
    cache: dict[tuple[str, str], _SourceArtifact],
) -> _SourceArtifact:
    key = (collection, digest)
    if key in cache:
        return cache[key]
    namespace = root / collection
    data_directory = namespace / "sha256"
    if namespace.is_symlink() or data_directory.is_symlink():
        raise ScenarioDiscoveryError(
            f"{collection} repository namespaces must not be symbolic links"
        )
    path = _repository_path(root, collection, digest)
    if not path.is_file():
        raise ScenarioDiscoveryError(
            f"{collection} artifact is unavailable for {digest} below {root}"
        )
    value = _load(path, label=f"{collection} artifact {digest}")
    if value.get("api_version") != api_version:
        raise ScenarioDiscoveryError(f"{path}.api_version must be {api_version}")
    selected_digest = _require_digest(
        value.get(own_field), path=f"{path}.{own_field}"
    )
    if selected_digest != digest:
        raise ScenarioDiscoveryError(f"{path}: repository key and {own_field} differ")
    # The qualified native closure verifier below independently validates the
    # complete content identity and canonical repository bytes. Repeating that
    # cryptographic serialization here made discovery slower without adding a
    # distinct acceptance boundary.
    artifact = _SourceArtifact(path=path.resolve(), value=value)
    cache[key] = artifact
    return artifact


def _scenario_from_row(
    row: dict[str, Any],
    *,
    row_path: str,
    repository_root: Path,
    scenario_catalog_path: Path,
    scenario_catalog_revision: str,
    deployments: dict[tuple[str, str], _SourceArtifact],
    programs: dict[tuple[str, str], _SourceArtifact],
    node_environment: NodeEnvironment,
    closure_verifier: ScenarioArtifactClosureVerifier,
    repository_cache: dict[tuple[str, str], _SourceArtifact],
) -> Scenario:
    scenario_id = _require_id(row.get("scenario_id"), path=f"{row_path}.scenario_id")
    deployment_id = _require_id(
        row.get("deployment_artifact_id"),
        path=f"{row_path}.deployment_artifact_id",
    )
    deployment_revision = _require_digest(
        row.get("deployment_revision"), path=f"{row_path}.deployment_revision"
    )
    retained_bundle_id = _require_digest(
        row.get("retained_bundle_id"), path=f"{row_path}.retained_bundle_id"
    )
    program_binding_id = _require_digest(
        row.get("program_binding_id"), path=f"{row_path}.program_binding_id"
    )
    deployment_compatibility = _require_exact_fields(
        row.get("deployment_compatibility"),
        required=frozenset({"lane", "provides"}),
        path=f"{row_path}.deployment_compatibility",
    )
    deployment_lane = deployment_compatibility.get("lane")
    if deployment_lane not in {"manyforge-only", "isaac-sim", "real-robot"}:
        raise ScenarioDiscoveryError(
            f"{row_path}.deployment_compatibility.lane is invalid"
        )
    deployment_provides = deployment_compatibility.get("provides")
    if type(deployment_provides) is not list or len(deployment_provides) > 4096:
        raise ScenarioDiscoveryError(
            f"{row_path}.deployment_compatibility.provides violates its bound"
        )
    qualified_provides = tuple(
        _require_id(item, path=f"{row_path}.deployment_compatibility.provides[]")
        for item in deployment_provides
    )
    if qualified_provides != tuple(sorted(set(qualified_provides))):
        raise ScenarioDiscoveryError(
            f"{row_path}.deployment_compatibility.provides must be "
            "duplicate-free and canonical-sorted"
        )
    assistant_mode = row.get("assistant_mode")
    if assistant_mode not in {"required", "optional", "forbidden"}:
        raise ScenarioDiscoveryError(f"{row_path}.assistant_mode is invalid")
    overrides = row.get("typed_parameter_overrides")
    if type(overrides) is not dict:
        raise ScenarioDiscoveryError(
            f"{row_path}.typed_parameter_overrides must be a mapping"
        )

    try:
        deployment = deployments[(deployment_id, deployment_revision)]
    except KeyError as exc:
        raise ScenarioDiscoveryError(
            f"{row_path}: exact deployment source is unavailable"
        ) from exc
    bundle = _load_repository_artifact(
        repository_root,
        collection="retained-bundles",
        digest=retained_bundle_id,
        api_version=RETAINED_BUNDLE_API,
        own_field="retained_bundle_id",
        cache=repository_cache,
    )
    binding = _load_repository_artifact(
        repository_root,
        collection="program-bindings",
        digest=program_binding_id,
        api_version=PROGRAM_BINDING_API,
        own_field="program_binding_id",
        cache=repository_cache,
    )
    if (
        bundle.value.get("deployment_artifact_id") != deployment_id
        or bundle.value.get("deployment_revision") != deployment_revision
    ):
        raise ScenarioDiscoveryError(
            f"{row_path}: retained bundle selects another deployment"
        )
    if binding.value.get("retained_bundle_id") != retained_bundle_id:
        raise ScenarioDiscoveryError(
            f"{row_path}: Program Binding selects another retained bundle"
        )
    bundle_graph = bundle.value.get("cell_graph")
    if type(bundle_graph) is not dict:
        raise ScenarioDiscoveryError(f"{row_path}: retained bundle has no cell graph")
    graph_revision = _require_digest(
        bundle_graph.get("graph_revision"),
        path=f"{bundle.path}.cell_graph.graph_revision",
    )
    if binding.value.get("graph_revision") != graph_revision:
        raise ScenarioDiscoveryError(
            f"{row_path}: Program Binding graph revision is stale"
        )

    program_id = _require_id(
        binding.value.get("program_id"), path=f"{binding.path}.program_id"
    )
    program_revision = _require_digest(
        binding.value.get("program_revision"),
        path=f"{binding.path}.program_revision",
    )
    try:
        program = programs[(program_id, program_revision)]
    except KeyError as exc:
        raise ScenarioDiscoveryError(
            f"{row_path}: exact Program source is unavailable"
        ) from exc

    aliases_raw = row.get("aliases", [])
    if type(aliases_raw) is not list or len(aliases_raw) > _MAX_ALIASES_PER_SCENARIO:
        raise ScenarioDiscoveryError(f"{row_path}.aliases violates its bound")
    aliases = tuple(
        _require_id(alias, path=f"{row_path}.aliases[]") for alias in aliases_raw
    )
    if tuple(sorted(aliases)) != aliases or len(set(aliases)) != len(aliases):
        raise ScenarioDiscoveryError(
            f"{row_path}.aliases must be duplicate-free and canonical-sorted"
        )
    description = _require_text(
        row.get("description", scenario_id), path=f"{row_path}.description"
    )
    overrides_json = canonical_json_bytes(overrides).decode("utf-8")
    closure = ScenarioArtifactClosure(
        scenario_id=scenario_id,
        scenario_catalog_revision=scenario_catalog_revision,
        repository_root=repository_root,
        scenario_catalog_path=scenario_catalog_path,
        deployment_path=deployment.path,
        retained_bundle_path=bundle.path,
        program_binding_path=binding.path,
        program_path=program.path,
        deployment_artifact_id=deployment_id,
        deployment_revision=deployment_revision,
        retained_bundle_id=retained_bundle_id,
        program_binding_id=program_binding_id,
        graph_revision=graph_revision,
        program_id=program_id,
        program_revision=program_revision,
        node_environment_digest=node_environment.environment_digest,
        assistant_mode=assistant_mode,
        typed_parameter_overrides_json=overrides_json,
    )
    try:
        verified = closure_verifier.verify(closure)
    except ScenarioDiscoveryError:
        raise
    except ValueError as exc:
        raise ScenarioDiscoveryError(
            f"{row_path}: exact artifact closure is invalid: {exc}"
        ) from exc
    if type(verified) is not VerifiedScenarioMetadata:
        raise ScenarioDiscoveryError(
            f"{row_path}: closure verifier returned an invalid result"
        )
    if verified.lane not in {"manyforge-only", "isaac-sim", "real-robot"}:
        raise ScenarioDiscoveryError(
            f"{row_path}: closure verifier returned an invalid lane"
        )
    if type(verified.assistant_enabled) is not bool:
        raise ScenarioDiscoveryError(
            f"{row_path}: closure verifier returned invalid assistant state"
        )
    return Scenario(
        name=scenario_id,
        lane=verified.lane,
        assistant="on" if verified.assistant_enabled else "off",
        program_path=program.path,
        deployment_path=deployment.path,
        description=description,
        aliases=aliases,
        deployment_artifact_id=deployment_id,
        deployment_revision=deployment_revision,
        retained_bundle_id=retained_bundle_id,
        program_binding_id=program_binding_id,
        program_id=program_id,
        program_revision=program_revision,
        typed_parameter_overrides_json=overrides_json,
    )


def discover_scenarios(
    roots: Iterable[str | Path],
    *,
    node_environment: NodeEnvironment,
    closure_verifier: ScenarioArtifactClosureVerifier,
) -> ScenarioCatalog:
    """Discover exact v1 closures after qualified semantic verification."""

    resolved_roots = _resolve_roots(roots)
    scenarios: list[Scenario] = []
    deployment_source_count = [0]
    program_source_count = [0]
    for root in resolved_roots:
        deployments = _index_sources(
            root,
            suffix=".deployment.yaml",
            api_version=DEPLOYMENT_API,
            own_id="deployment_artifact_id",
            own_revision="deployment_revision",
            source_count=deployment_source_count,
        )
        programs = _index_sources(
            root,
            suffix=".program.yaml",
            api_version=PROGRAM_API,
            own_id="program_id",
            own_revision="program_revision",
            source_count=program_source_count,
        )
        repository_cache: dict[tuple[str, str], _SourceArtifact] = {}
        for raw_catalog_path in sorted(root.glob("*.scenario-catalog.yaml")):
            catalog = _load(raw_catalog_path, label="scenario catalog")
            catalog_path = raw_catalog_path.resolve()
            _require_exact_fields(
                catalog,
                required=frozenset(
                    {"api_version", "scenario_catalog_revision", "scenarios"}
                ),
                path=str(catalog_path),
            )
            scenario_catalog_revision = _verify_identity(
                catalog,
                own_field="scenario_catalog_revision",
                identity_domain=SCENARIO_CATALOG_IDENTITY_DOMAIN,
                api_version=SCENARIO_CATALOG_API,
                label=str(catalog_path),
            )
            rows = catalog.get("scenarios")
            if type(rows) is not list:
                raise ScenarioDiscoveryError(f"{catalog_path}.scenarios must be a list")
            if len(scenarios) + len(rows) > _MAX_SCENARIOS:
                raise ScenarioDiscoveryError(
                    f"aggregate scenario count exceeds {_MAX_SCENARIOS}"
                )
            identifiers: list[str] = []
            for index, raw_row in enumerate(rows):
                row_path = f"{catalog_path}.scenarios[{index}]"
                row = _require_exact_fields(
                    raw_row,
                    required=frozenset(
                        {
                            "scenario_id",
                            "deployment_artifact_id",
                            "deployment_revision",
                            "retained_bundle_id",
                            "program_binding_id",
                            "deployment_compatibility",
                            "assistant_mode",
                            "typed_parameter_overrides",
                        }
                    ),
                    optional=frozenset({"description", "aliases"}),
                    path=row_path,
                )
                scenario = _scenario_from_row(
                    row,
                    row_path=row_path,
                    repository_root=root,
                    scenario_catalog_path=catalog_path,
                    scenario_catalog_revision=scenario_catalog_revision,
                    deployments=deployments,
                    programs=programs,
                    node_environment=node_environment,
                    closure_verifier=closure_verifier,
                    repository_cache=repository_cache,
                )
                identifiers.append(scenario.name)
                scenarios.append(scenario)
            if identifiers != sorted(identifiers) or len(set(identifiers)) != len(
                identifiers
            ):
                raise ScenarioDiscoveryError(
                    f"{catalog_path}.scenarios must be unique and canonical-sorted"
                )
    return ScenarioCatalog(scenarios)


__all__ = [
    "DEPLOYMENT_API",
    "PROGRAM_API",
    "RETAINED_BUNDLE_API",
    "SCENARIO_CATALOG_API",
    "Scenario",
    "ScenarioArtifactClosure",
    "ScenarioArtifactClosureVerifier",
    "NativeScenarioArtifactClosureVerifier",
    "ScenarioCatalog",
    "ScenarioDiscoveryError",
    "VerifiedScenarioMetadata",
    "configured_scenario_root_values",
    "discover_scenarios",
]
