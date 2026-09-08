"""The selected example is real package code behind ordinary owner adapters."""

from pathlib import Path
from types import SimpleNamespace

import py_trees
import pytest

from manyforge_behavior import qualified_node_extensions as extension
from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.executable_node_resolver import (
    ExecutableNodeResolverError,
    NodeAdapterBuildContext,
    build_executable_node_resolver,
)
from manyforge_behavior.node_environment import (
    NodeEnvironmentBuildError,
    build_node_environment,
)
from manyforge_behavior.orchestration_ports import (
    NodeAdapterPorts,
    OwnerInvocationResult,
)
from manyforge_behavior.orchestration_values import (
    NodeAttemptInputResolver,
    ProgramValueStore,
)
from manyforge_behavior.program import Program
from manyforge_behavior.program_runtime import ProgramRunAttemptSource


def _adapter(port, *, minimum=0.0, maximum=1.0):
    environment = build_node_environment()
    document = Program(
        Path(__file__).resolve().parents[2]
        / "examples/lite6_lite_gripper_pick_and_place.program.yaml",
        environment.catalog,
    ).to_dict()
    document["target_roles"]["arm"]["required_capabilities"].extend(
        [
            {
                "capability_id": "model.kinematics.fk",
                "parameters": {},
                "disposition": "required",
            },
        ]
    )
    document["target_roles"]["arm"]["required_capabilities"].sort(
        key=lambda row: row["capability_id"]
    )
    parameters = {
        "target_role": "arm",
        "frame_role": "tcp",
        "minimum_height_m": minimum,
        "maximum_height_m": maximum,
    }
    document["tree"] = {
        "node_id": extension.NODE_ID,
        "instance_id": "height",
        "parameters": parameters,
        "children": [],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    program = Program.from_dict(document, environment.catalog)
    context = NodeAdapterBuildContext(
        name="height",
        node_instance_id="height",
        parameters=parameters,
        input_resolver=NodeAttemptInputResolver(
            program=program,
            catalog=environment.catalog,
            value_store=ProgramValueStore.from_program(program),
        ),
        attempt_source=ProgramRunAttemptSource("run.extension"),
        ports=NodeAdapterPorts(model_queries=port),
    )
    row = environment.registry.record(extension.NODE_ID)
    assert row.executable_package_id == extension.PACKAGE_ID
    return build_executable_node_resolver(environment).instantiate(row, context)


@pytest.mark.parametrize("height,expected", [(0.5, True), (1.5, False), (float("nan"), False)])
def test_actual_package_uses_one_current_owner_query(height, expected):
    calls = []

    def invoke(node_id, identity, inputs):
        calls.append((node_id, identity, inputs))
        return OwnerInvocationResult(
            True,
            output={
                "frame_ref": {"kind": "cell_frame"},
                "position_m": [0.0, 0.0, height],
            },
            metadata={"source_sequence": 17},
        )

    adapter = _adapter(SimpleNamespace(invoke=invoke))
    adapter.initialise()
    status = py_trees.common.Status.SUCCESS if expected else py_trees.common.Status.FAILURE
    assert adapter.update() is status and adapter.update() is status
    assert len(calls) == 1 and calls[0][0] == "model.get_frame_pose"
    assert dict(calls[0][2].parameters) == {"target_role": "arm", "frame_role": "tcp"}


def test_owner_unavailable_and_invalid_input_do_not_acquire_another_authority():
    with pytest.raises(ExecutableNodeResolverError, match="model_queries port"):
        _adapter(None)

    def unavailable(*args):
        return OwnerInvocationResult(
            False, reason_code="state_unavailable", message="No current observation"
        )

    adapter = _adapter(SimpleNamespace(invoke=unavailable))
    adapter.initialise()
    assert adapter.update() is py_trees.common.Status.FAILURE
    assert adapter.failure_reason_code == "state_unavailable"

    def unexpected(*args):
        raise AssertionError("invalid inputs must not invoke the model owner")

    invalid = _adapter(SimpleNamespace(invoke=unexpected), minimum=2.0, maximum=1.0)
    invalid.initialise()
    assert invalid.update() is py_trees.common.Status.FAILURE
    assert "finite and ordered" in invalid.failure_message


def test_extension_bytes_are_part_of_startup_integrity(tmp_path, monkeypatch):
    source = tmp_path / "extension.py"
    source.write_text("# altered installed source\n")
    original = extension.source_files()
    monkeypatch.setattr(
        extension, "source_files", lambda: ((original[0][0], source), *original[1:])
    )
    with pytest.raises(NodeEnvironmentBuildError, match="manifest is stale"):
        build_node_environment()


def test_unknown_extension_descriptor_cannot_widen_the_reviewed_owner_binding(
    tmp_path, monkeypatch
):
    import json

    value = extension.descriptor()
    value["entries"][0]["execution"]["owner_api"] = "kernel.operation"
    path = tmp_path / "descriptor.json"
    path.write_text(json.dumps(value))
    monkeypatch.setattr(extension, "DESCRIPTOR_PATH", path)
    with pytest.raises(NodeEnvironmentBuildError, match="reviewed NRT owner"):
        build_node_environment()


@pytest.mark.parametrize("relative", ["extra.pyi", "nested/extra.py"])
def test_unsupported_extension_sources_fail_instead_of_escaping_coverage(
    tmp_path, monkeypatch, relative
):
    path = tmp_path / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("# unsupported package source\n")
    monkeypatch.setattr(extension, "PACKAGE_ROOT", tmp_path)
    with pytest.raises(NodeEnvironmentBuildError, match="unsupported source layout"):
        build_node_environment()


def test_added_flat_module_is_covered_and_invalidates_installed_identity(tmp_path, monkeypatch):
    (tmp_path / "extra.py").write_text("# new executable module\n")
    monkeypatch.setattr(extension, "PACKAGE_ROOT", tmp_path)
    assert extension.source_files()[0][0] == "python-module:manyforge_example_nodes.extra"
    with pytest.raises(NodeEnvironmentBuildError, match="manifest is stale"):
        build_node_environment()
