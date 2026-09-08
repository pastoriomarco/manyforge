"""V2 retained node-environment identity and removed runtime boundaries."""

from __future__ import annotations

import copy
from pathlib import Path

import pytest

import manyforge_behavior.node_registry as node_registry_module
from manyforge_behavior import node_environment
from manyforge_behavior.node_environment import (
    AUTHORITATIVE_PROJECT_PATH,
    BUILTIN_ENVIRONMENT_MANIFEST_PATH,
    NODE_ENVIRONMENT_IDENTITY_DOMAIN,
    NODE_ENVIRONMENT_MANIFEST_API,
    PACKAGED_PROJECT_SNAPSHOT_PATH,
    NodeEnvironmentBuildError,
    SourceCoverageRecord,
    build_node_environment,
    generate_builtin_node_environment_manifest,
    load_builtin_node_environment_manifest,
)
from manyforge_behavior.node_registry import NodeRegistry, NodeRegistryError


def test_retained_environment_manifest_is_fresh_and_build_owned() -> None:
    retained = load_builtin_node_environment_manifest()
    generated = generate_builtin_node_environment_manifest()
    environment = build_node_environment()

    assert retained == generated == environment.manifest
    assert retained.environment_digest.startswith("sha256:")
    assert retained.catalog_digest == environment.catalog.catalog_digest
    assert retained.executable_registry == environment.registry.rows
    assert len(retained.executable_registry) == 52
    assert len(retained.executable_packages) == 2
    assert retained.source_coverage
    assert any(
        row.source_id == "python-project:manyforge_behavior" for row in retained.source_coverage
    )
    assert any(
        row.source_id == "skill-artifact:move_to_named_config" for row in retained.source_coverage
    )


def test_manifest_projection_contains_no_path_command_or_availability() -> None:
    projection = generate_builtin_node_environment_manifest().to_dict()
    serialized_names: set[str] = set()

    def collect(value) -> None:
        if isinstance(value, dict):
            serialized_names.update(value)
            for nested in value.values():
                collect(nested)
        elif isinstance(value, list):
            for nested in value:
                collect(nested)

    collect(projection)
    assert projection["api_version"] == NODE_ENVIRONMENT_MANIFEST_API
    assert {
        "path",
        "module",
        "class",
        "factory",
        "command",
        "availability",
        "runtime_available",
    }.isdisjoint(serialized_names)


def test_manifest_rejects_unknown_root_and_row_members() -> None:
    for mutate in (
        lambda document: document.__setitem__("availability", {}),
        lambda document: document["executable_registry"][0].__setitem__(
            "module",
            "forbidden.runtime.import",
        ),
    ):
        raw = node_environment.load_strict_json_yaml(BUILTIN_ENVIRONMENT_MANIFEST_PATH)
        forged = copy.deepcopy(raw)
        mutate(forged)
        forged["environment_digest"] = "sha256:" + ("0" * 64)
        forged["environment_digest"] = node_environment.compute_content_identity(
            forged,
            own_field="environment_digest",
            identity_domain=NODE_ENVIRONMENT_IDENTITY_DOMAIN,
            api_version=NODE_ENVIRONMENT_MANIFEST_API,
        )

        with pytest.raises(NodeEnvironmentBuildError, match="wrong fields"):
            node_environment._parse_manifest(forged)  # noqa: SLF001


def test_manifest_accepts_shared_implementation_id_for_distinct_nodes() -> None:
    raw = node_environment.load_strict_json_yaml(BUILTIN_ENVIRONMENT_MANIFEST_PATH)
    forged = copy.deepcopy(raw)
    forged["executable_registry"][1]["implementation_id"] = forged["executable_registry"][0][
        "implementation_id"
    ]
    forged["environment_digest"] = "sha256:" + ("0" * 64)
    forged["environment_digest"] = node_environment.compute_content_identity(
        forged,
        own_field="environment_digest",
        identity_domain=NODE_ENVIRONMENT_IDENTITY_DOMAIN,
        api_version=NODE_ENVIRONMENT_MANIFEST_API,
    )

    parsed = node_environment._parse_manifest(forged)  # noqa: SLF001
    assert (
        parsed.executable_registry[0].implementation_id
        == parsed.executable_registry[1].implementation_id
    )


def test_manifest_rejects_noncanonical_and_duplicate_registry_rows() -> None:
    raw = node_environment.load_strict_json_yaml(BUILTIN_ENVIRONMENT_MANIFEST_PATH)
    noncanonical = copy.deepcopy(raw)
    noncanonical["executable_registry"][0:2] = reversed(noncanonical["executable_registry"][0:2])
    noncanonical["environment_digest"] = "sha256:" + ("0" * 64)
    noncanonical["environment_digest"] = node_environment.compute_content_identity(
        noncanonical,
        own_field="environment_digest",
        identity_domain=NODE_ENVIRONMENT_IDENTITY_DOMAIN,
        api_version=NODE_ENVIRONMENT_MANIFEST_API,
    )
    with pytest.raises(NodeEnvironmentBuildError, match="not canonical"):
        node_environment._parse_manifest(noncanonical)  # noqa: SLF001

    duplicate = copy.deepcopy(raw)
    duplicate["executable_registry"].insert(
        0,
        copy.deepcopy(duplicate["executable_registry"][0]),
    )
    duplicate["environment_digest"] = "sha256:" + ("0" * 64)
    duplicate["environment_digest"] = node_environment.compute_content_identity(
        duplicate,
        own_field="environment_digest",
        identity_domain=NODE_ENVIRONMENT_IDENTITY_DOMAIN,
        api_version=NODE_ENVIRONMENT_MANIFEST_API,
    )
    with pytest.raises(NodeEnvironmentBuildError, match="duplicate node/engine"):
        node_environment._parse_manifest(duplicate)  # noqa: SLF001


def test_registry_and_package_joins_are_exact_and_total() -> None:
    manifest = generate_builtin_node_environment_manifest()
    package_ids = {package.executable_package_id for package in manifest.executable_packages}

    assert {row.executable_package_id for row in manifest.executable_registry} == package_ids
    assert {row.executable_package_id for row in manifest.source_coverage} == package_ids
    assert {
        (row.node_id, row.engine_id, row.implementation_id) for row in manifest.executable_registry
    } == {
        (entry.id, engine_id, implementation_id)
        for entry in build_node_environment().catalog.entries
        for engine_id, implementation_id in entry.runner_implementation_ids.items()
    }


def test_environment_digest_changes_when_covered_source_changes(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    original = node_environment._current_source_coverage()  # noqa: SLF001
    changed = list(original)
    row = changed[0]
    changed[0] = SourceCoverageRecord(
        executable_package_id=row.executable_package_id,
        source_id=row.source_id,
        content_digest="sha256:" + ("f" * 64),
    )
    monkeypatch.setattr(
        node_environment,
        "_current_source_coverage",
        lambda: tuple(changed),
    )

    generated = generate_builtin_node_environment_manifest()
    with pytest.raises(NodeEnvironmentBuildError, match="manifest is stale"):
        load_builtin_node_environment_manifest()
    assert generated.environment_digest != (
        node_environment._parse_manifest(  # noqa: SLF001
            node_environment.load_strict_json_yaml(BUILTIN_ENVIRONMENT_MANIFEST_PATH)
        ).environment_digest
    )


def test_source_package_version_comes_from_project_metadata() -> None:
    assert (
        generate_builtin_node_environment_manifest().executable_packages[0].package_version
        == "0.1.0"
    )


def test_skill_artifact_tamper_invalidates_the_selected_source_package(
    tmp_path, monkeypatch
) -> None:
    import shutil

    skills = tmp_path / "skills"
    shutil.copytree(node_environment.BUILTIN_SKILLS_PATH, skills)
    artifact = skills / "move_to_named_config.skill.yaml"
    artifact.write_bytes(artifact.read_bytes() + b"\n# Changed build input\n")
    monkeypatch.setattr(node_environment, "BUILTIN_SKILLS_PATH", skills)
    with pytest.raises(NodeEnvironmentBuildError, match="manifest is stale"):
        build_node_environment()


def test_packaged_project_snapshot_is_exact_build_evidence() -> None:
    assert PACKAGED_PROJECT_SNAPSHOT_PATH.read_bytes() == AUTHORITATIVE_PROJECT_PATH.read_bytes()


def test_manifest_rejects_c1_control_characters() -> None:
    raw = node_environment.load_strict_json_yaml(BUILTIN_ENVIRONMENT_MANIFEST_PATH)
    forged = copy.deepcopy(raw)
    forged["executable_packages"][0]["package_version"] = "0.1.0\u0085"
    forged["environment_digest"] = "sha256:" + ("0" * 64)
    forged["environment_digest"] = node_environment.compute_content_identity(
        forged,
        own_field="environment_digest",
        identity_domain=NODE_ENVIRONMENT_IDENTITY_DOMAIN,
        api_version=NODE_ENVIRONMENT_MANIFEST_API,
    )

    with pytest.raises(NodeEnvironmentBuildError, match="control characters"):
        node_environment._parse_manifest(forged)  # noqa: SLF001


def test_registry_is_not_caller_constructible() -> None:
    with pytest.raises(NodeRegistryError, match="build-owned"):
        NodeRegistry((), _token=object())


def test_registry_rejects_duplicate_node_engine_rows() -> None:
    row = generate_builtin_node_environment_manifest().executable_registry[0]

    with pytest.raises(NodeRegistryError, match="duplicate node/engine"):
        NodeRegistry(
            (row, row),
            _token=node_registry_module._BUILD_TOKEN,  # noqa: SLF001
        )


def test_registry_identity_remains_separate_from_closed_runtime_resolution() -> None:
    environment = build_node_environment()

    assert not hasattr(environment.registry, "instantiate")
    assert not hasattr(node_registry_module, "NodeFactoryContext")
    assert not hasattr(node_registry_module, "NodeRuntimeUnavailableError")
    from manyforge_behavior.executable_node_resolver import (
        build_executable_node_resolver,
    )

    resolver = build_executable_node_resolver(environment)
    assert resolver.environment_digest == environment.environment_digest
    assert len(resolver.dispositions) == len(environment.registry.rows)


def test_caller_cannot_choose_a_catalog_path() -> None:
    with pytest.raises(TypeError):
        build_node_environment("/tmp/forged-catalog.yaml")  # type: ignore[call-arg]


def test_removed_strict_compatibility_keyword_is_not_accepted() -> None:
    with pytest.raises(TypeError):
        build_node_environment(strict=True)  # type: ignore[call-arg]


def test_path_based_extension_loading_surface_is_removed(tmp_path: Path) -> None:
    assert not hasattr(node_environment, "configured_node_roots")
    with pytest.raises(TypeError):
        build_node_environment(external_roots=[tmp_path])  # type: ignore[call-arg]


def test_manifest_field_splice_fails_identity_verification() -> None:
    raw = node_environment.load_strict_json_yaml(BUILTIN_ENVIRONMENT_MANIFEST_PATH)
    spliced = copy.deepcopy(raw)
    spliced["catalog"]["catalog_digest"] = "sha256:" + ("f" * 64)

    with pytest.raises(NodeEnvironmentBuildError, match="content identity mismatch"):
        node_environment._parse_manifest(spliced)  # noqa: SLF001


def test_manifest_package_digest_must_cover_exact_source_rows() -> None:
    raw = node_environment.load_strict_json_yaml(BUILTIN_ENVIRONMENT_MANIFEST_PATH)
    forged = copy.deepcopy(raw)
    forged["executable_packages"][0]["content_digest"] = "sha256:" + ("f" * 64)
    forged["environment_digest"] = "sha256:" + ("0" * 64)
    forged["environment_digest"] = node_environment.compute_content_identity(
        forged,
        own_field="environment_digest",
        identity_domain=NODE_ENVIRONMENT_IDENTITY_DOMAIN,
        api_version=NODE_ENVIRONMENT_MANIFEST_API,
    )

    with pytest.raises(
        NodeEnvironmentBuildError,
        match="content digest does not match",
    ):
        node_environment._parse_manifest(forged)  # noqa: SLF001


def test_manifest_rejects_path_like_source_identity() -> None:
    raw = node_environment.load_strict_json_yaml(BUILTIN_ENVIRONMENT_MANIFEST_PATH)
    forged = copy.deepcopy(raw)
    forged["source_coverage"][0]["source_id"] = "/tmp/forged.py"
    forged["environment_digest"] = "sha256:" + ("0" * 64)
    forged["environment_digest"] = node_environment.compute_content_identity(
        forged,
        own_field="environment_digest",
        identity_domain=NODE_ENVIRONMENT_IDENTITY_DOMAIN,
        api_version=NODE_ENVIRONMENT_MANIFEST_API,
    )

    with pytest.raises(
        NodeEnvironmentBuildError,
        match="path-like source identity",
    ):
        node_environment._parse_manifest(forged)  # noqa: SLF001


def test_old_external_factory_loader_surface_is_removed() -> None:
    assert not hasattr(node_registry_module, "load_external_node_factory")
    assert not hasattr(node_registry_module, "_load_symbol")
