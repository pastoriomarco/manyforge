"""Focused tests for retained qualification artifact support."""

from __future__ import annotations

from pathlib import Path
import tarfile

import pytest

import manyforge_composer.verification.runner as scenario_runner
from manyforge_composer.verification import (
    archive_artifacts,
    expand_matrix_scenarios,
    format_scenario_condition_failure,
    load_matrix_definition,
    match_scenario_expectation,
    resolve_scenario_payload_path,
    ScenarioExecutionError,
    ScenarioExecutionUnavailableError,
    ScenarioRunner,
    write_matrix_artifacts,
    write_scenario_artifacts,
)
from manyforge_composer.verification.runner import _git_rev_parse, _sha256_path


def _write_yaml(path: Path, text: str) -> Path:
    path.write_text(text.strip(), encoding="utf-8")
    return path


def test_matrix_loading_expands_repeats_and_preserves_matrix_tags(
    tmp_path: Path,
) -> None:
    scenario_path = tmp_path / "sample.scenario.yaml"
    scenario_path.write_text(
        """
scenario_id: sample
description: Sample scenario.
assertions:
  - endpoint: future_owner_status
    path: state
    equals: idle
""".strip(),
        encoding="utf-8",
    )
    matrix_path = tmp_path / "matrix.yaml"
    matrix_path.write_text(
        """
matrix_id: repeat_matrix
scenarios:
  - path: sample.scenario.yaml
    repeat: 3
    tags: [soak, deterministic]
""".strip(),
        encoding="utf-8",
    )

    definitions = expand_matrix_scenarios(
        load_matrix_definition(matrix_path),
        workspace_root=tmp_path,
    )

    assert [definition.scenario_id for definition in definitions] == [
        "sample__repeat_01",
        "sample__repeat_02",
        "sample__repeat_03",
    ]
    assert all(
        definition.tags == ["soak", "deterministic"]
        for definition in definitions
    )


def test_result_summary_and_archive_packaging_remain_available(
    tmp_path: Path,
) -> None:
    output_dir = tmp_path / "artifacts"
    archive_path = tmp_path / "matrix.tar.gz"

    result_path = write_scenario_artifacts(
        {
            "scenarioId": "offline_case",
            "success": True,
            "finalArtifacts": {"digest": {"sha256": "abc"}},
        },
        output_dir=output_dir,
    )
    summary_path = write_matrix_artifacts(
        {"matrixId": "offline_matrix", "success": True},
        output_dir=output_dir,
        archive_path=archive_path,
    )

    assert result_path.name == "offline_case.result.json"
    assert summary_path.name == "offline_matrix.summary.json"
    assert (output_dir / "offline_case.digest.json").is_file()
    with tarfile.open(archive_path, "r:gz") as archive:
        names = archive.getnames()
    assert any(name.endswith("offline_case.result.json") for name in names)
    assert any(name.endswith("offline_matrix.summary.json") for name in names)


def test_payload_resolution_and_expectation_matching_remain_owner_neutral() -> None:
    payload = {"rows": [{"state": "ready", "count": 3}]}
    actual = resolve_scenario_payload_path(payload, "rows[0].count")

    assert match_scenario_expectation(actual, {"greater_equal": 2})
    assert match_scenario_expectation("headless-ready", {"contains": "ready"})
    assert not match_scenario_expectation(actual, {"equals": 4})
    assert format_scenario_condition_failure(
        {"endpoint": "status", "path": "rows[0].count", "equals": 4},
        actual,
    ) == "condition not met: status.rows[0].count == 4 (actual=3)"


def test_missing_fact_cannot_satisfy_any_expectation_operator() -> None:
    missing = resolve_scenario_payload_path({"row": {}}, "row.state")

    for condition in (
        {"equals": None},
        {"not_equals": "failed"},
        {"greater_equal": 0},
        {"less_equal": 0},
        {"contains": "ready"},
        {"truthy": True},
    ):
        assert match_scenario_expectation(missing, condition) is False

    assert format_scenario_condition_failure(
        {"endpoint": "status", "path": "state", "not_equals": "failed"},
        missing,
    ).endswith("(actual=<missing>)")


@pytest.mark.parametrize(
    "condition",
    (
        {},
        {"equals": "ready", "not_equals": "failed"},
        {"truthy": False},
        {"truthy": 1},
    ),
)
def test_expectations_require_exactly_one_operator_and_exact_truthy_bool(
    condition: dict[str, object],
) -> None:
    with pytest.raises(
        ScenarioExecutionError,
        match="exactly one supported operator|truthy operator must be true",
    ):
        match_scenario_expectation("ready", condition)


def test_expectations_do_not_use_python_truth_or_type_coercion() -> None:
    assert not match_scenario_expectation(True, {"equals": 1})
    assert match_scenario_expectation(True, {"not_equals": 1})
    assert not match_scenario_expectation(True, {"greater_equal": 1})
    assert not match_scenario_expectation("item-123", {"contains": 123})
    assert not match_scenario_expectation("false", {"truthy": True})
    assert not match_scenario_expectation(1, {"truthy": True})
    assert match_scenario_expectation(True, {"truthy": True})
    assert not match_scenario_expectation([True], {"contains": 1})
    assert match_scenario_expectation([1.0], {"contains": 1})


def test_payload_path_limits_are_checked_before_fact_traversal() -> None:
    too_many_tokens = ".".join(
        ["missing"] * (scenario_runner.MAX_PAYLOAD_PATH_TOKENS + 1)
    )

    with pytest.raises(ScenarioExecutionError, match="token bound"):
        resolve_scenario_payload_path({}, too_many_tokens)
    with pytest.raises(ScenarioExecutionError, match="16-digit bound"):
        resolve_scenario_payload_path({}, "missing[12345678901234567]")
    with pytest.raises(ScenarioExecutionError, match="invalid syntax"):
        resolve_scenario_payload_path({}, "missing[")


def test_json_exponent_numbers_load_as_numbers_not_yaml11_strings(
    tmp_path: Path,
) -> None:
    # PyYAML's YAML 1.1 SafeLoader reads `-4e-05` (no dot) as a string; the
    # Isaac UR10e example Scenes carry exactly this JSON spelling.
    exponent = _write_yaml(
        tmp_path / "exponent.scenario.yaml",
        "scenario_id: exponent\ntimeouts: {rpy_deg: [-2.1e-05, -4e-05]}",
    )
    yaml11_only = _write_yaml(
        tmp_path / "yaml11.scenario.yaml",
        "scenario_id: yaml11\ntimeouts: {enabled: on}",
    )

    definition = scenario_runner.load_scenario_definition(exponent)
    assert definition.timeouts == {"rpy_deg": [-2.1e-05, -4e-05]}
    assert all(type(value) is float for value in definition.timeouts["rpy_deg"])
    with pytest.raises(ValueError, match="YAML-only implicit scalar"):
        scenario_runner.load_scenario_definition(yaml11_only)


def test_yaml_byte_and_scalar_bounds_apply_before_scenario_allocation(
    tmp_path: Path,
) -> None:
    oversized_document = tmp_path / "oversized.scenario.yaml"
    oversized_document.write_bytes(
        b"x" * (scenario_runner.MAX_YAML_DOCUMENT_BYTES + 1)
    )
    oversized_scalar = tmp_path / "scalar.scenario.yaml"
    _write_yaml(
        oversized_scalar,
        "scenario_id: scalar\n"
        f"description: {'x' * (scenario_runner.MAX_SCALAR_BYTES + 1)}",
    )

    with pytest.raises(ValueError, match="YAML bound"):
        scenario_runner.load_scenario_definition(oversized_document)
    with pytest.raises(ValueError, match="scalar exceeds"):
        scenario_runner.load_scenario_definition(oversized_scalar)


def test_yaml_depth_and_node_bounds_apply_during_composition(
    tmp_path: Path,
) -> None:
    nested = "value"
    for _ in range(scenario_runner.MAX_DOCUMENT_DEPTH + 1):
        nested = f"{{level: {nested}}}"
    too_deep = _write_yaml(
        tmp_path / "too-deep.scenario.yaml",
        f"scenario_id: too_deep\nextra: {nested}",
    )
    too_many_nodes = _write_yaml(
        tmp_path / "too-many-nodes.scenario.yaml",
        "scenario_id: too_many_nodes\nextra:\n"
        + "\n".join(
            "  - 0" for _ in range(scenario_runner.MAX_DOCUMENT_NODES + 1)
        ),
    )

    with pytest.raises(ValueError, match="nesting exceeds"):
        scenario_runner.load_scenario_definition(too_deep)
    with pytest.raises(ValueError, match="more than.*nodes"):
        scenario_runner.load_scenario_definition(too_many_nodes)


def test_yaml_rejects_aliases_duplicate_keys_and_invalid_field_shapes(
    tmp_path: Path,
) -> None:
    cases = {
        "alias": (
            "scenario_id: alias\n"
            "steps: &steps []\n"
            "assertions: *steps",
            "aliases are forbidden|anchors are forbidden",
        ),
        "duplicate": (
            "scenario_id: first\nscenario_id: second",
            "duplicate mapping key",
        ),
        "timeouts": (
            "scenario_id: bad_timeouts\ntimeouts: []",
            "timeouts.*must be a mapping",
        ),
        "steps": (
            "scenario_id: bad_steps\nsteps: {}",
            "steps.*must be a list",
        ),
        "program": (
            "scenario_id: bad_program\nprogram: 7",
            "program must be a string",
        ),
    }

    for name, (document, match) in cases.items():
        path = _write_yaml(tmp_path / f"{name}.scenario.yaml", document)
        with pytest.raises(ValueError, match=match):
            scenario_runner.load_scenario_definition(path)


def test_scenario_and_matrix_cardinality_bounds_are_fail_closed(
    tmp_path: Path,
) -> None:
    scenario = tmp_path / "too-many-steps.scenario.yaml"
    steps = "\n".join(
        "  - action: retained"
        for _ in range(scenario_runner.MAX_SCENARIO_STEPS + 1)
    )
    _write_yaml(scenario, f"scenario_id: bounded\nsteps:\n{steps}")
    bool_repeat = _write_yaml(
        tmp_path / "bool-repeat.yaml",
        """
matrix_id: bool_repeat
scenarios:
  - path: unavailable.scenario.yaml
    repeat: true
""",
    )
    oversized_repeat = _write_yaml(
        tmp_path / "oversized-repeat.yaml",
        "matrix_id: oversized_repeat\n"
        "scenarios:\n"
        "  - path: unavailable.scenario.yaml\n"
        f"    repeat: {scenario_runner.MAX_SCENARIO_REPEAT + 1}",
    )
    cumulative = _write_yaml(
        tmp_path / "cumulative.yaml",
        "matrix_id: cumulative\nscenarios:\n"
        + "\n".join(
            "  - path: unavailable.scenario.yaml\n"
            f"    repeat: {scenario_runner.MAX_SCENARIO_REPEAT}"
            for _ in range(
                scenario_runner.MAX_EXPANDED_SCENARIOS
                // scenario_runner.MAX_SCENARIO_REPEAT
                + 1
            )
        ),
    )

    with pytest.raises(ValueError, match="steps.*exceeds"):
        scenario_runner.load_scenario_definition(scenario)
    with pytest.raises(ValueError, match="repeat.*must be an integer"):
        load_matrix_definition(bool_repeat)
    with pytest.raises(ValueError, match="repeat bound"):
        load_matrix_definition(oversized_repeat)
    with pytest.raises(ValueError, match="expanded scenario bound"):
        load_matrix_definition(cumulative)


def test_archive_rejects_output_inside_source_directory(tmp_path: Path) -> None:
    source_dir = tmp_path / "artifacts"
    write_scenario_artifacts(
        {"scenarioId": "bounded", "finalArtifacts": {}},
        output_dir=source_dir,
    )

    with pytest.raises(ScenarioExecutionError, match="must not be inside"):
        archive_artifacts(source_dir, source_dir / "recursive.tar.gz")


def test_artifact_writer_rejects_path_like_ids_before_creating_output(
    tmp_path: Path,
) -> None:
    output_dir = tmp_path / "must-not-exist"

    with pytest.raises(ValueError, match="invalid 'scenarioId'"):
        write_scenario_artifacts(
            {"scenarioId": "../escape", "finalArtifacts": {}},
            output_dir=output_dir,
        )

    assert not output_dir.exists()


def test_artifact_writer_rejects_unbounded_or_duplicate_outputs_before_effects(
    tmp_path: Path,
) -> None:
    oversized_dir = tmp_path / "oversized"
    duplicate_dir = tmp_path / "duplicate"
    too_many_dir = tmp_path / "too-many"
    nonfinite_dir = tmp_path / "nonfinite"
    huge_id_dir = tmp_path / "huge-id"

    with pytest.raises(ValueError, match="scalar bound"):
        write_scenario_artifacts(
            {
                "scenarioId": "oversized",
                "detail": "x" * (scenario_runner.MAX_SCALAR_BYTES * 32),
            },
            output_dir=oversized_dir,
        )
    with pytest.raises(ValueError, match="duplicate file names"):
        write_scenario_artifacts(
            {
                "scenarioId": "collision",
                "finalArtifacts": {"result": {"value": 1}},
            },
            output_dir=duplicate_dir,
        )
    with pytest.raises(ValueError, match="artifact bound"):
        write_scenario_artifacts(
            {
                "scenarioId": "too_many",
                "finalArtifacts": {
                    f"artifact_{index}": None
                    for index in range(scenario_runner.MAX_FINAL_ARTIFACTS + 1)
                },
            },
            output_dir=too_many_dir,
        )
    with pytest.raises(ValueError, match="non-finite"):
        write_scenario_artifacts(
            {"scenarioId": "nonfinite", "metric": float("nan")},
            output_dir=nonfinite_dir,
        )
    with pytest.raises(ValueError, match="256-byte bound"):
        write_scenario_artifacts(
            {"scenarioId": " " * (scenario_runner.MAX_SCALAR_BYTES * 32)},
            output_dir=huge_id_dir,
        )

    assert not oversized_dir.exists()
    assert not duplicate_dir.exists()
    assert not too_many_dir.exists()
    assert not nonfinite_dir.exists()
    assert not huge_id_dir.exists()


def test_artifact_writer_uses_owned_directory_and_never_overwrites(
    tmp_path: Path,
) -> None:
    output_dir = tmp_path / "owned"
    result = {"scenarioId": "immutable", "success": True}
    result_path = write_scenario_artifacts(result, output_dir=output_dir)
    original = result_path.read_bytes()

    with pytest.raises(ScenarioExecutionError, match="refusing to overwrite"):
        write_scenario_artifacts(result, output_dir=output_dir)

    assert result_path.read_bytes() == original


def test_multifile_publication_rolls_back_exact_outputs_on_write_failure(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    output_dir = tmp_path / "rollback"
    original_fsync = scenario_runner.os.fsync
    calls = 0

    def fail_result_fsync(descriptor: int) -> None:
        nonlocal calls
        calls += 1
        if calls == 3:
            raise OSError("injected result fsync failure")
        original_fsync(descriptor)

    monkeypatch.setattr(scenario_runner.os, "fsync", fail_result_fsync)
    with pytest.raises(ScenarioExecutionError, match="cannot publish"):
        write_scenario_artifacts(
            {
                "scenarioId": "retryable",
                "finalArtifacts": {"digest": {"value": 1}},
            },
            output_dir=output_dir,
        )
    monkeypatch.setattr(scenario_runner.os, "fsync", original_fsync)

    assert [path.name for path in output_dir.iterdir()] == [
        ".manyforge-scenario-artifacts.v1"
    ]
    result_path = write_scenario_artifacts(
        {
            "scenarioId": "retryable",
            "finalArtifacts": {"digest": {"value": 1}},
        },
        output_dir=output_dir,
    )
    assert result_path.is_file()


def test_matrix_summary_rolls_back_when_requested_archive_fails(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    output_dir = tmp_path / "matrix-rollback"
    write_scenario_artifacts(
        {"scenarioId": "seed"},
        output_dir=output_dir,
    )
    archive_path = tmp_path / "matrix-rollback.tar.gz"
    original_archive = scenario_runner.archive_artifacts

    def fail_archive(source_dir: Path, destination: Path) -> Path:
        del source_dir, destination
        raise ScenarioExecutionError("injected archive failure")

    monkeypatch.setattr(scenario_runner, "archive_artifacts", fail_archive)
    with pytest.raises(ScenarioExecutionError, match="injected archive failure"):
        write_matrix_artifacts(
            {"matrixId": "retryable_matrix", "success": True},
            output_dir=output_dir,
            archive_path=archive_path,
        )
    monkeypatch.setattr(scenario_runner, "archive_artifacts", original_archive)

    assert not (output_dir / "retryable_matrix.summary.json").exists()
    assert not archive_path.exists()
    summary_path = write_matrix_artifacts(
        {"matrixId": "retryable_matrix", "success": True},
        output_dir=output_dir,
        archive_path=archive_path,
    )
    assert summary_path.is_file()
    assert archive_path.is_file()


def test_artifact_writer_refuses_an_arbitrary_existing_caller_tree(
    tmp_path: Path,
) -> None:
    caller_tree = tmp_path / "caller-tree"
    caller_tree.mkdir()
    victim = caller_tree / "victim.json"
    victim.write_text('{"keep": true}\n', encoding="utf-8")

    with pytest.raises(ScenarioExecutionError, match="not owned"):
        write_scenario_artifacts(
            {"scenarioId": "must_not_write"},
            output_dir=caller_tree,
        )

    assert victim.read_text(encoding="utf-8") == '{"keep": true}\n'


def test_archive_rejects_arbitrary_nested_and_symlink_entries(
    tmp_path: Path,
) -> None:
    arbitrary = tmp_path / "arbitrary"
    arbitrary.mkdir()
    (arbitrary / "victim.txt").write_text("keep", encoding="utf-8")
    with pytest.raises(ScenarioExecutionError, match="not owned"):
        archive_artifacts(arbitrary, tmp_path / "arbitrary.tar.gz")

    nested_source = tmp_path / "nested-source"
    write_scenario_artifacts(
        {"scenarioId": "nested"},
        output_dir=nested_source,
    )
    (nested_source / "unexpected").mkdir()
    with pytest.raises(ScenarioExecutionError, match="unowned entry"):
        archive_artifacts(nested_source, tmp_path / "nested.tar.gz")

    symlink_source = tmp_path / "symlink-source"
    write_scenario_artifacts(
        {"scenarioId": "linked"},
        output_dir=symlink_source,
    )
    (symlink_source / "linked.extra.json").symlink_to(
        symlink_source / "linked.result.json"
    )
    with pytest.raises(ScenarioExecutionError, match="non-symlink"):
        archive_artifacts(symlink_source, tmp_path / "symlink.tar.gz")


def test_archive_is_deterministic_contained_and_exclusive(tmp_path: Path) -> None:
    first_source = tmp_path / "first"
    second_source = tmp_path / "second"
    payload = {
        "scenarioId": "stable",
        "success": True,
        "finalArtifacts": {"digest": {"sha256": "abc"}},
    }
    write_scenario_artifacts(payload, output_dir=first_source)
    write_scenario_artifacts(payload, output_dir=second_source)
    first_archive = archive_artifacts(first_source, tmp_path / "first.tar.gz")
    second_archive = archive_artifacts(second_source, tmp_path / "second.tar.gz")

    assert first_archive.read_bytes() == second_archive.read_bytes()
    with tarfile.open(first_archive, "r:gz") as archive:
        members = archive.getmembers()
    assert [member.name for member in members] == [
        "artifacts",
        "artifacts/.manyforge-scenario-artifacts.v1",
        "artifacts/stable.digest.json",
        "artifacts/stable.result.json",
    ]
    assert all(member.uid == 0 and member.gid == 0 for member in members)
    assert all(member.mtime == 0 for member in members)

    original = first_archive.read_bytes()
    with pytest.raises(ScenarioExecutionError, match="refusing to overwrite"):
        archive_artifacts(first_source, first_archive)
    assert first_archive.read_bytes() == original

    outside = tmp_path.parent / f"{tmp_path.name}-outside.tar.gz"
    with pytest.raises(ScenarioExecutionError, match="direct sibling"):
        archive_artifacts(first_source, outside)
    assert not outside.exists()


def test_evidence_hashing_rejects_oversized_files(tmp_path: Path) -> None:
    oversized = tmp_path / "oversized-evidence.json"
    with oversized.open("wb") as stream:
        stream.truncate(scenario_runner.MAX_EVIDENCE_FILE_BYTES + 1)

    with pytest.raises(ScenarioExecutionError, match="evidence file exceeds"):
        _sha256_path(oversized)


def test_git_commit_metadata_can_come_from_environment(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    revision = "0123456789abcdef0123456789abcdef01234567"
    monkeypatch.setenv("MANYFORGE_GIT_COMMIT", revision)

    assert _git_rev_parse(tmp_path) == revision


def test_runtime_runner_name_is_only_a_typed_gate() -> None:
    with pytest.raises(
        ScenarioExecutionUnavailableError,
        match="composable_cell_runtime_unavailable",
    ):
        ScenarioRunner(object())
