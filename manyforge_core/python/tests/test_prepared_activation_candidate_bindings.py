"""The live candidate wrapper is read-only and refuses incomplete closure inputs."""

from pathlib import Path

import pytest

core = pytest.importorskip("manyforge_core_py")


def test_candidate_cannot_be_constructed_or_populated_by_python() -> None:
    with pytest.raises(TypeError):
        core.PreparedActivationCandidate()
    assert not hasattr(core.PreparedActivationCandidate, "publish")
    assert not hasattr(core.PreparedActivationCandidate, "set_catalog")


@pytest.mark.parametrize("cell_id", ["", "Upper", "cell/other", "x" * 129])
def test_invalid_runtime_cell_is_refused_before_materialization(
    tmp_path: Path, cell_id: str
) -> None:
    before = sorted(tmp_path.iterdir())
    with pytest.raises(core.PreparedActivationCandidateError, match="runtime_cell_id"):
        core.prepare_activation_candidate(str(tmp_path), "{}", "{}", "{}", cell_id)
    assert sorted(tmp_path.iterdir()) == before


def test_invalid_captured_closure_does_not_create_candidate_or_write(tmp_path: Path) -> None:
    # Strict JSON parsing fails before any catalog/materialization work. There
    # is no way to supply replacement candidate facts in the Python API.
    with pytest.raises(core.PreparedActivationCandidateError):
        core.prepare_activation_candidate(str(tmp_path), "{}", "{", "{}", "cell_a")
    assert list(tmp_path.iterdir()) == []


def _model_free_candidate():
    # Same real I/O-only retained materialization as Deployment's
    # ActualAuthoredModelFreePipelineProducesQualifiedCatalogArtifactsAndDomains.
    import json

    component = "station"
    signals = [
        {
            "controllable_component_id": component,
            "command_target_id": "io",
            "resource_role_id": "ready",
            "signal_name": "ready",
            "source_id": "station.ready",
            "value_type": "bool",
            "direction": "input",
            "update_mode": "change_only",
            "transition_capability": "sampled_only",
            "freshness": {"mode": "provider_validity"},
            "provider": {"profile": "mock"},
        },
        {
            "controllable_component_id": component,
            "command_target_id": "io",
            "resource_role_id": "lamp",
            "signal_name": "lamp",
            "source_id": "station.lamp",
            "value_type": "bool",
            "direction": "output",
            "provider": {"profile": "mock_output", "channel": "station.lamp"},
        },
    ]
    deployment = {
        "api_version": "manyforge.deployment_intent.v2",
        "deployment_artifact_id": "authored_io",
        "name": "authored_io",
        "compatibility": {"lane": "manyforge-only", "provides": [], "tags": []},
        "attachments": [],
        "components": [{
            "component_id": component,
            "component_profile_id": "io_device_v0",
            "target_settings": {},
            "configuration": {
                "runtime_kind": "io_provider",
                "runtime_configuration": {
                    "io_provider_devices": [{"controllable_component_id": component}],
                    "io_signals": signals,
                },
            },
        }],
        "policies": {},
    }
    source = {"schema_version": "manyforge.activation_model_source_closure.v1", "components": []}
    prepared = {"schema_version": "manyforge.activation_prepared_model_closure.v1", "components": []}
    return core.prepare_activation_candidate(
        str(Path(__file__).resolve().parents[3]), json.dumps(deployment),
        json.dumps(source), json.dumps(prepared), "authored_io_cell",
    )


def test_real_candidate_is_immutable_and_preserves_program_format_error() -> None:
    candidate = _model_free_candidate()
    with pytest.raises(AttributeError):
        candidate.bundle = object()
    with pytest.raises(core.ProgramContractError) as raised:
        candidate.validate_program_references('{"api_version":"manyforge.program.v2"}', False)
    assert raised.value.code == "program_format_unsupported"
    assert raised.value.details == {
        "actual_api_version": "manyforge.program.v2",
        "supported_api_version": "manyforge.program.v3",
    }


def test_real_candidate_selector_facts_are_stable_and_candidate_owned() -> None:
    import json

    candidate = _model_free_candidate()
    first = candidate.selector_facts_json()
    assert candidate.selector_facts_json() == first
    facts = json.loads(first)
    assert set(facts) == {"components", "targets", "signals", "frames", "bodies", "rules"}
    assert all(isinstance(rows, list) for rows in facts.values())
    assert {row["component_id"] for row in facts["components"]} >= {"station"}


def test_real_candidate_reports_node_parameter_for_unavailable_target_capability() -> None:
    """A direct target is checked against the retained candidate before Apply."""
    import json
    from manyforge_behavior.canonical_artifact import compute_content_identity
    from manyforge_behavior.catalog import NodeCatalog

    root = Path(__file__).resolve().parents[3]
    catalog = NodeCatalog(root / "manyforge_behavior/resources/node_catalog.yaml")
    program = {
        "api_version": "manyforge.program.v3", "program_id": "io_capability_test",
        "program_revision": "sha256:" + "a" * 64,
        "program_profile": "composable-cell-v1", "name": "I/O capability test",
        "catalog": {"api_version": catalog.api_version, "content_version": catalog.content_version},
        "tree": {"node_id": "motion.set_tcp", "instance_id": "set_tcp", "children": [],
                 "parameters": {"target": {"component_id": "station", "command_target_id": "io"},
                                "tcp": {"kind": "current"}}},
    }
    program["program_revision"] = compute_content_identity(
        program, own_field="program_revision", identity_domain="program-revision",
        api_version="manyforge.program.v3")
    refusal = json.loads(_model_free_candidate().validate_program_references(json.dumps(program), False))
    rows = [row for row in refusal["diagnostics"]
            if row["location"]["node_instance_id"] == "set_tcp"
            and row["location"]["parameter"] == "target"]
    assert len(rows) == 1
    assert rows[0]["code"] == "capability_missing"
    assert rows[0]["location"]["path"] == "/tree/parameters/target"
    assert rows[0]["payload"]["capability_id"] == "execution.trajectory.execute"
