"""The target-query probe prepares activation exactly as production Load does."""

import importlib.util
import json
import os
from pathlib import Path
import re
import stat

import pytest

from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    load_strict_json_yaml,
    raw_sha256_digest,
)
from manyforge_headless_api.activation_preparer import ActivationPreparer
from manyforge_headless_api.native import HeadlessApiError


ROOT = Path(__file__).resolve().parents[3]
SCENARIO = ROOT / "examples/lite6_pick_and_place.scenario"
PROGRAM = ROOT / "examples/lite6_lite_gripper_pick_and_place.program.yaml"
CELL_ID = "c1_build_authority_cell"


@pytest.fixture(scope="module")
def probe():
    path = ROOT / "scripts/qualification/ui6h_target_queries.py"
    spec = importlib.util.spec_from_file_location("ui6h_target_queries", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _native_request_fields() -> set[str]:
    """The exact field set the native preparer requires (its own source)."""
    source = (ROOT / "manyforge_deployment/tools/prepare_activation.cpp").read_text()
    match = re.search(
        r"require_exact_fields\(root,\s*\{([^}]*)\},\s*"
        r"\"activation preparation request\"\)",
        source,
    )
    assert match is not None, "native preparation request field list not found"
    return set(re.findall(r'"([a-z_]+)"', match.group(1)))


def test_probe_request_is_the_complete_native_v3_request(probe, tmp_path: Path) -> None:
    # A recording stand-in for the native executable: production
    # ActivationPreparer still builds, bounds and writes the request.
    recorded = tmp_path / "request.json"
    executable = tmp_path / "record-preparation-request"
    executable.write_text(
        "#!/bin/sh\n"
        'while [ "$#" -gt 0 ]; do\n'
        '  if [ "$1" = --input ]; then cp "$2" "' + str(recorded) + '"; fi\n'
        "  shift\n"
        "done\n"
        "exit 3\n"
    )
    executable.chmod(executable.stat().st_mode | stat.S_IXUSR)
    program = load_strict_json_yaml(PROGRAM)
    inputs = probe.activation_preparation_inputs(SCENARIO, program, allowed_root=ROOT)

    with pytest.raises(HeadlessApiError) as refused:
        ActivationPreparer(
            executable=executable,
            capability_root=ROOT,
            activation_root=tmp_path / "activation",
            runtime_cell_id=CELL_ID,
            timeout_seconds=60.0,
        ).prepare(**inputs)
    assert refused.value.code == "activation_preparation_failed"

    request = json.loads(recorded.read_bytes())
    assert set(request) == _native_request_fields()
    assert request["schema_version"] == "manyforge.activation_preparation_request.v3"
    assert request["runtime_cell_id"] == CELL_ID
    assert request["scenario_id"] == load_strict_json_yaml(
        SCENARIO / "scenario.yaml"
    )["scenario_id"]
    assert request["program"] == program
    assert request["deployment"] == load_strict_json_yaml(SCENARIO / "deployment.yaml")
    assert request["program_role_assignment"] == load_strict_json_yaml(
        SCENARIO / "assignment.yaml"
    )
    assert request["model_source_closure"]["components"]
    assert request["prepared_model_closure"]["components"]
    # The same six-input identity the native preparer recomputes.
    assert request["captured_input_digest"] == raw_sha256_digest(
        canonical_json_bytes(
            {
                name: request[name]
                for name in (
                    "deployment",
                    "model_source_closure",
                    "program",
                    "program_role_assignment",
                    "scenario_id",
                    "source_document_digest",
                )
            }
        )
    )


def test_probe_request_is_accepted_by_the_native_preparer(probe, tmp_path: Path) -> None:
    executable = os.environ.get("MANYFORGE_ACTIVATION_PREPARER")
    if not executable:
        pytest.skip("MANYFORGE_ACTIVATION_PREPARER names no built native preparer")
    program = load_strict_json_yaml(PROGRAM)
    inputs = probe.activation_preparation_inputs(SCENARIO, program, allowed_root=ROOT)
    activation_root = tmp_path / "activation"
    activation_root.mkdir()
    prepared = ActivationPreparer(
        executable=Path(executable),
        capability_root=ROOT,
        activation_root=activation_root,
        runtime_cell_id=CELL_ID,
        timeout_seconds=600.0,
    ).prepare(**inputs)
    assert prepared["schema_version"] == "manyforge.prepared_activation_input.v5"
    assert prepared["scenario_id"] == inputs["scenario_id"]
    assert prepared["captured_input_digest"] == inputs["captured_input_digest"]
