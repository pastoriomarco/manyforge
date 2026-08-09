"""Artifact-owned scenario discovery contracts."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest
import yaml

from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    compute_content_identity,
)
from manyforge_behavior.node_environment import NodeEnvironment, build_node_environment
from manyforge_behavior.scenarios import (
    PROGRAM_BINDING_API,
    RETAINED_BUNDLE_API,
    SCENARIO_CATALOG_API,
    NativeScenarioArtifactClosureVerifier,
    Scenario,
    ScenarioArtifactClosure,
    ScenarioCatalog,
    ScenarioDiscoveryError,
    VerifiedScenarioMetadata,
    configured_scenario_root_values,
    discover_scenarios,
)

_ZERO_DIGEST = "sha256:" + "0" * 64


class _RecordingQualifiedVerifier:
    def __init__(self, failure: str | None = None) -> None:
        self.closures: list[ScenarioArtifactClosure] = []
        self._failure = failure

    def verify(self, closure: ScenarioArtifactClosure, /) -> VerifiedScenarioMetadata:
        self.closures.append(closure)
        if self._failure is not None:
            raise ValueError(self._failure)
        return VerifiedScenarioMetadata(
            lane="manyforge-only",
            assistant_enabled=False,
        )


def _identified(
    value: dict[str, object],
    *,
    own_field: str,
    identity_domain: str,
    api_version: str,
) -> dict[str, object]:
    identified = {**value, own_field: _ZERO_DIGEST}
    identified[own_field] = compute_content_identity(
        identified,
        own_field=own_field,
        identity_domain=identity_domain,
        api_version=api_version,
    )
    return identified


def _write_yaml(path: Path, value: dict[str, object]) -> None:
    path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")


def _write_repository_artifact(
    root: Path,
    collection: str,
    value: dict[str, object],
    identity_field: str,
) -> Path:
    digest = value[identity_field]
    assert isinstance(digest, str)
    path = root / collection / "sha256" / f"{digest.removeprefix('sha256:')}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(canonical_json_bytes(value))
    return path


def _write_exact_closure(
    root: Path,
    environment: NodeEnvironment,
    *,
    scenario_id: str = "cell-pick",
    alias: str = "pick",
) -> dict[str, str]:
    deployment_revision = "sha256:" + "a" * 64
    program_revision = "sha256:" + "b" * 64
    graph_revision = "sha256:" + "c" * 64
    deployment = {
        "api_version": "manyforge.deployment.v1",
        "deployment_artifact_id": "cell_a",
        "deployment_revision": deployment_revision,
        "assistant": {},
        "compatibility": {
            "lane": "manyforge-only",
            "provides": ["scene.mutable"],
            "tags": ["pick-and-place"],
        },
    }
    program = {
        "api_version": "manyforge.program.v1",
        "program_id": "pick_program",
        "program_revision": program_revision,
        "catalog": {
            "api_version": environment.manifest.catalog_api_version,
            "content_version": environment.manifest.catalog_content_version,
        },
        "compatibility": {
            "lanes": ["manyforge-only"],
            "assistant": "forbidden",
            "requires": ["scene.mutable"],
            "tags": ["pick-and-place"],
        },
    }
    _write_yaml(root / "cell_a.deployment.yaml", deployment)
    _write_yaml(root / "pick_program.program.yaml", program)

    bundle = _identified(
        {
            "api_version": RETAINED_BUNDLE_API,
            "deployment_artifact_id": "cell_a",
            "deployment_revision": deployment_revision,
            "cell_graph": {"graph_revision": graph_revision},
        },
        own_field="retained_bundle_id",
        identity_domain="retained-bundle-id",
        api_version=RETAINED_BUNDLE_API,
    )
    retained_bundle_id = bundle["retained_bundle_id"]
    assert isinstance(retained_bundle_id, str)
    bundle_path = _write_repository_artifact(
        root, "retained-bundles", bundle, "retained_bundle_id"
    )

    binding = _identified(
        {
            "api_version": PROGRAM_BINDING_API,
            "program_id": "pick_program",
            "program_revision": program_revision,
            "retained_bundle_id": retained_bundle_id,
            "graph_revision": graph_revision,
            "node_environment": {
                "catalog_id": environment.manifest.catalog_id,
                "content_version": environment.manifest.catalog_content_version,
                "catalog_digest": environment.manifest.catalog_digest,
                "environment_digest": environment.manifest.environment_digest,
            },
        },
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version=PROGRAM_BINDING_API,
    )
    program_binding_id = binding["program_binding_id"]
    assert isinstance(program_binding_id, str)
    binding_path = _write_repository_artifact(
        root, "program-bindings", binding, "program_binding_id"
    )

    catalog = _identified(
        {
            "api_version": SCENARIO_CATALOG_API,
            "scenarios": [
                {
                    "scenario_id": scenario_id,
                    "deployment_artifact_id": "cell_a",
                    "deployment_revision": deployment_revision,
                    "retained_bundle_id": retained_bundle_id,
                    "program_binding_id": program_binding_id,
                    "assistant_mode": "forbidden",
                    "typed_parameter_overrides": {"attempts": 2},
                    "description": "Exact v1 pick scenario",
                    "aliases": [alias],
                }
            ],
        },
        own_field="scenario_catalog_revision",
        identity_domain="scenario-catalog-revision",
        api_version=SCENARIO_CATALOG_API,
    )
    _write_yaml(root / f"{scenario_id}.scenario-catalog.yaml", catalog)
    scenario_catalog_revision = catalog["scenario_catalog_revision"]
    assert isinstance(scenario_catalog_revision, str)
    return {
        "scenario_catalog_revision": scenario_catalog_revision,
        "deployment_revision": deployment_revision,
        "program_revision": program_revision,
        "retained_bundle_id": retained_bundle_id,
        "program_binding_id": program_binding_id,
        "bundle_path": str(bundle_path.resolve()),
        "binding_path": str(binding_path.resolve()),
    }


def test_exact_v1_scenario_closure_is_discovered_and_verified(
    tmp_path: Path,
) -> None:
    environment = build_node_environment()
    expected = _write_exact_closure(tmp_path, environment)
    verifier = _RecordingQualifiedVerifier()

    catalog = discover_scenarios(
        [tmp_path],
        node_environment=environment,
        closure_verifier=verifier,
    )

    assert len(catalog.scenarios) == 1
    scenario = catalog.scenarios[0]
    assert scenario.name == "cell-pick"
    assert scenario.lane == "manyforge-only"
    assert scenario.assistant == "off"
    assert scenario.aliases == ("pick",)
    assert scenario.deployment_revision == expected["deployment_revision"]
    assert scenario.program_revision == expected["program_revision"]
    assert scenario.retained_bundle_id == expected["retained_bundle_id"]
    assert scenario.program_binding_id == expected["program_binding_id"]
    assert json.loads(scenario.typed_parameter_overrides_json) == {"attempts": 2}
    resolved, deprecated = catalog.resolve("pick")
    assert resolved is scenario
    assert deprecated is True

    assert len(verifier.closures) == 1
    closure = verifier.closures[0]
    assert closure.repository_root == tmp_path.resolve()
    assert closure.scenario_catalog_revision == expected["scenario_catalog_revision"]
    assert str(closure.retained_bundle_path) == expected["bundle_path"]
    assert str(closure.program_binding_path) == expected["binding_path"]
    assert closure.node_environment_digest == environment.environment_digest


def test_native_verifier_adapter_reuses_program_values_and_forwards_exact_closure(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    environment = build_node_environment()
    program_path = (
        Path(__file__).resolve().parents[2] / "examples" / "pick_and_place.program.yaml"
    )
    captured: list[tuple[object, ...]] = []
    returned: list[tuple[object, object]] = [("manyforge-only", False)]

    def verify(*args: object) -> tuple[object, object]:
        captured.append(args)
        return returned[0]

    monkeypatch.setitem(
        sys.modules,
        "manyforge_core_py",
        SimpleNamespace(verify_scenario_artifact_closure=verify),
    )
    closure = ScenarioArtifactClosure(
        scenario_id="ur5_pick_and_place",
        scenario_catalog_revision="sha256:" + "1" * 64,
        repository_root=program_path.parent,
        scenario_catalog_path=program_path,
        deployment_path=program_path,
        retained_bundle_path=program_path,
        program_binding_path=program_path,
        program_path=program_path,
        deployment_artifact_id="ur5_roboplan_local_pick_place",
        deployment_revision="sha256:" + "2" * 64,
        retained_bundle_id="sha256:" + "3" * 64,
        program_binding_id="sha256:" + "4" * 64,
        graph_revision="sha256:" + "5" * 64,
        program_id="pick_and_place_demo",
        program_revision="sha256:" + "6" * 64,
        node_environment_digest=environment.environment_digest,
        assistant_mode="forbidden",
        typed_parameter_overrides_json='{"velocity_scale":0.6}',
    )

    result = NativeScenarioArtifactClosureVerifier(environment).verify(closure)

    assert result == VerifiedScenarioMetadata(
        lane="manyforge-only", assistant_enabled=False
    )
    assert len(captured) == 1
    assert captured[0][0] == "ur5_pick_and_place"
    assert captured[0][-1] == '{"velocity_scale":0.6}'

    returned[0] = ("manyforge-only", 0)
    with pytest.raises(ValueError, match="invalid assistant state"):
        NativeScenarioArtifactClosureVerifier(environment).verify(closure)


@pytest.mark.parametrize(
    ("overrides_json", "message"),
    (
        ('{"unknown":1}', "undeclared parameter"),
        ('{"velocity_scale":"fast"}', "does not match float64"),
        ('{"velocity_scale":0.0}', "violates min_exclusive"),
    ),
)
def test_native_verifier_adapter_rejects_invalid_override_before_native_call(
    monkeypatch: pytest.MonkeyPatch,
    overrides_json: str,
    message: str,
) -> None:
    environment = build_node_environment()
    program_path = (
        Path(__file__).resolve().parents[2] / "examples" / "pick_and_place.program.yaml"
    )
    called = False

    def verify(*args: object) -> tuple[str, bool]:
        del args
        nonlocal called
        called = True
        return "manyforge-only", False

    monkeypatch.setitem(
        sys.modules,
        "manyforge_core_py",
        SimpleNamespace(verify_scenario_artifact_closure=verify),
    )
    closure = ScenarioArtifactClosure(
        scenario_id="ur5_pick_and_place",
        scenario_catalog_revision="sha256:" + "1" * 64,
        repository_root=program_path.parent,
        scenario_catalog_path=program_path,
        deployment_path=program_path,
        retained_bundle_path=program_path,
        program_binding_path=program_path,
        program_path=program_path,
        deployment_artifact_id="ur5_roboplan_local_pick_place",
        deployment_revision="sha256:" + "2" * 64,
        retained_bundle_id="sha256:" + "3" * 64,
        program_binding_id="sha256:" + "4" * 64,
        graph_revision="sha256:" + "5" * 64,
        program_id="pick_and_place_demo",
        program_revision="sha256:" + "6" * 64,
        node_environment_digest=environment.environment_digest,
        assistant_mode="forbidden",
        typed_parameter_overrides_json=overrides_json,
    )

    with pytest.raises(ValueError, match=message):
        NativeScenarioArtifactClosureVerifier(environment).verify(closure)
    assert not called


def test_scenarios_are_globally_deterministic_across_root_order(
    tmp_path: Path,
) -> None:
    environment = build_node_environment()
    first = tmp_path / "first"
    second = tmp_path / "second"
    first.mkdir()
    second.mkdir()
    _write_exact_closure(first, environment, scenario_id="z-cell", alias="z")
    _write_exact_closure(second, environment, scenario_id="a-cell", alias="a")

    catalog = discover_scenarios(
        [first, second],
        node_environment=environment,
        closure_verifier=_RecordingQualifiedVerifier(),
    )

    assert tuple(scenario.name for scenario in catalog.scenarios) == (
        "a-cell",
        "z-cell",
    )


def test_missing_scenario_root_is_a_structured_discovery_error(
    tmp_path: Path,
) -> None:
    environment = build_node_environment()

    with pytest.raises(ScenarioDiscoveryError, match="scenario root does not exist"):
        discover_scenarios(
            [tmp_path / "missing"],
            node_environment=environment,
            closure_verifier=_RecordingQualifiedVerifier(),
        )


def test_repository_canonicality_is_delegated_to_qualified_closure_verifier(
    tmp_path: Path,
) -> None:
    environment = build_node_environment()
    expected = _write_exact_closure(tmp_path, environment)
    binding_path = Path(expected["binding_path"])
    value = json.loads(binding_path.read_text(encoding="utf-8"))
    binding_path.write_text(json.dumps(value, indent=2), encoding="utf-8")

    verifier = _RecordingQualifiedVerifier(
        "Program Binding repository bytes are not canonical JSON"
    )
    with pytest.raises(ScenarioDiscoveryError, match="not canonical JSON"):
        discover_scenarios(
            [tmp_path],
            node_environment=environment,
            closure_verifier=verifier,
        )
    assert len(verifier.closures) == 1


def test_qualified_closure_rejection_is_a_structured_discovery_error(
    tmp_path: Path,
) -> None:
    environment = build_node_environment()
    _write_exact_closure(tmp_path, environment)

    with pytest.raises(
        ScenarioDiscoveryError,
        match="exact artifact closure is invalid: binding role is stale",
    ):
        discover_scenarios(
            [tmp_path],
            node_environment=environment,
            closure_verifier=_RecordingQualifiedVerifier("binding role is stale"),
        )


def test_pure_scenario_catalog_alias_resolution_remains_available(
    tmp_path: Path,
) -> None:
    scenario = Scenario(
        name="canonical-cell",
        lane="real-robot",
        assistant="off",
        program_path=tmp_path / "cell.program.yaml",
        deployment_path=tmp_path / "cell.deployment.yaml",
        description="metadata only",
        aliases=("cell",),
    )
    catalog = ScenarioCatalog([scenario])

    resolved, deprecated = catalog.resolve("cell")

    assert resolved is scenario
    assert deprecated is True


def test_scenario_root_setting_has_one_shared_process_contract(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv(
        "MANYFORGE_SCENARIO_ROOTS",
        "/workspace/site-a:/opt/site-b",
    )
    # The removed pre-cutover spelling must not affect either launcher path.
    monkeypatch.setenv("MANYFORGE_SCENARIO_PATHS", "/wrong/legacy/path")

    assert configured_scenario_root_values() == (
        "/workspace/site-a",
        "/opt/site-b",
    )
