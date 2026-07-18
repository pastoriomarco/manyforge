"""External-node discovery, registry, execution, and resource contracts."""

from __future__ import annotations

import sys
import time
from pathlib import Path
from types import SimpleNamespace

import py_trees
import pytest
import yaml

from manyforge_behavior.cli import main as cli_main
from manyforge_behavior.cycle_manager import CycleManager, CycleState
from manyforge_behavior.kernel_client import MockKernelClient
from manyforge_behavior.node_descriptor import (
    NodeDescriptorError,
    load_node_descriptor,
)
from manyforge_behavior.node_environment import (
    NodeEnvironmentBuildError,
    build_node_environment,
    validate_environment_availability,
)
from manyforge_behavior.node_registry import NodeFactoryContext
from manyforge_behavior.program import Program
from manyforge_behavior.runner import OrchestratorRunner
from manyforge_behavior.runtime_resources import (
    RuntimeResourceError,
    RuntimeResourceFactoryRegistry,
    build_runtime_resource_registry,
)


CATALOG_PATH = Path(__file__).parents[1] / "resources" / "node_catalog.yaml"


@pytest.fixture(autouse=True)
def _clear_blackboard():
    py_trees.blackboard.Blackboard.clear()
    yield
    py_trees.blackboard.Blackboard.clear()


def _write_node(
    root: Path,
    node_id: str,
    source: str,
    *,
    runtime_resources: list[dict] | None = None,
    parameters: list[dict] | None = None,
) -> Path:
    folder = root / node_id
    folder.mkdir(parents=True)
    descriptor = {
        "api_version": "manyforge.external_node.v0",
        "id": node_id,
        "name": "Generated Test Node",
        "version": "0.1.0",
        "category": "IO",
        "description": "A generated node used by the external-node contract tests.",
        "stability": "experimental",
        "kind": "leaf",
        "children": {"min": 0, "max": 0},
        "profiles": {"min_profile": "v0.0"},
        "runtime": {
            "language": "python",
            "file": "node.py",
            "class": "GeneratedNode",
            "py_trees": "==2.4.0",
            "constructor": "name_kwargs",
            "tick_thread_policy": "nonblocking",
        },
        "required_capabilities": [],
        "runtime_resources": runtime_resources or [],
        "parameters": parameters or [],
        "operator_stop_policy": "continue",
    }
    (folder / "node.yaml").write_text(
        yaml.safe_dump(descriptor, sort_keys=False), encoding="utf-8"
    )
    (folder / "node.py").write_text(source, encoding="utf-8")
    return folder


def _mutate_descriptor(folder: Path, mutation) -> None:
    descriptor_path = folder / "node.yaml"
    descriptor = yaml.safe_load(descriptor_path.read_text(encoding="utf-8"))
    mutation(descriptor)
    descriptor_path.write_text(
        yaml.safe_dump(descriptor, sort_keys=False), encoding="utf-8"
    )


SUCCESS_NODE = """
import py_trees

class GeneratedNode(py_trees.behaviour.Behaviour):
    def __init__(self, name, **params):
        super().__init__(name=name)

    def update(self):
        return py_trees.common.Status.SUCCESS
"""


def _program(environment, node_id: str) -> Program:
    return Program.from_dict(
        {
            "api_version": "manyforge.program.v0",
            "program_profile": "v0.0",
            "name": "external_node_program",
            "catalog_version": environment.catalog.version,
            "robot": {
                "robot_id": "test",
                "group_id": "arm",
                "base_frame": "base_link",
                "tcp_frame": "tool0",
            },
            "tree": {"id": node_id, "name": "generated", "params": {}},
        },
        environment.catalog,
    )


def test_descriptor_validation_does_not_import_source(tmp_path: Path) -> None:
    folder = _write_node(tmp_path, "vendor_not_imported", "raise RuntimeError('imported')")
    descriptor = load_node_descriptor(folder)
    assert descriptor.node_id == "vendor_not_imported"


@pytest.mark.parametrize(
    ("node_id", "mutation", "message"),
    [
        (
            "vendor_bad_api",
            lambda descriptor: descriptor.__setitem__("api_version", "bad.v9"),
            "api_version must be manyforge.external_node.v0",
        ),
        (
            "vendor_missing_class",
            lambda descriptor: descriptor["runtime"].pop("class"),
            "runtime.class must be a non-empty string",
        ),
        (
            "vendor_bad_arity",
            lambda descriptor: descriptor.__setitem__(
                "children", {"min": 0, "max": 1}
            ),
            "external leaf children must be",
        ),
        (
            "vendor_bad_stop_policy",
            lambda descriptor: descriptor.__setitem__(
                "operator_stop_policy", "ignore"
            ),
            "operator_stop_policy must be hold, continue, or inherit",
        ),
        (
            "vendor_missing_capabilities",
            lambda descriptor: descriptor.pop("required_capabilities"),
            "required_capabilities is required",
        ),
    ],
)
def test_descriptor_rejects_invalid_required_contract_fields(
    tmp_path: Path,
    node_id: str,
    mutation,
    message: str,
) -> None:
    folder = _write_node(tmp_path, node_id, SUCCESS_NODE)
    _mutate_descriptor(folder, mutation)

    with pytest.raises(NodeDescriptorError, match=message):
        load_node_descriptor(folder)


@pytest.mark.parametrize(
    ("node_id", "parameters", "message"),
    [
        (
            "vendor_duplicate_params",
            [
                {"name": "alias", "schema": {"type": "string"}},
                {"name": "alias", "schema": {"type": "string"}},
            ],
            "parameters contains duplicate names",
        ),
        (
            "vendor_missing_schema",
            [{"name": "alias", "required": True}],
            "v1 parameters require schema or schema_ref",
        ),
        (
            "vendor_bad_default",
            [
                {
                    "name": "count",
                    "required": False,
                    "default": "not-an-int",
                    "schema": {"type": "int"},
                }
            ],
            "must be an integer",
        ),
    ],
)
def test_descriptor_rejects_invalid_parameter_catalog_metadata(
    tmp_path: Path,
    node_id: str,
    parameters: list[dict],
    message: str,
) -> None:
    folder = _write_node(
        tmp_path,
        node_id,
        SUCCESS_NODE,
        parameters=parameters,
    )

    with pytest.raises(NodeDescriptorError, match=message):
        load_node_descriptor(folder)


def test_descriptor_rejects_runtime_resource_with_non_string_alias_parameter(
    tmp_path: Path,
) -> None:
    folder = _write_node(
        tmp_path,
        "vendor_non_string_alias",
        SUCCESS_NODE,
        runtime_resources=[
            {
                "id": "plc_client",
                "type": "plc_client",
                "access": "read_write",
                "required": True,
                "alias_parameter": "plc_client_id",
            }
        ],
        parameters=[
            {
                "name": "plc_client_id",
                "required": False,
                "default": 1,
                "schema": {"type": "int"},
            }
        ],
    )

    with pytest.raises(NodeDescriptorError, match="string-schema parameter"):
        load_node_descriptor(folder)


def test_descriptor_requires_complete_runtime_resource_contract(
    tmp_path: Path,
) -> None:
    folder = _write_node(
        tmp_path,
        "vendor_incomplete_resource",
        SUCCESS_NODE,
        runtime_resources=[
            {
                "id": "plc_client",
                "type": "plc_client",
                "required": True,
                "alias_parameter": "plc_client_id",
            }
        ],
        parameters=[
            {
                "name": "plc_client_id",
                "required": False,
                "default": "default",
                "schema": {"type": "string"},
            }
        ],
    )

    with pytest.raises(NodeDescriptorError, match="missing required fields.*access"):
        load_node_descriptor(folder)


def test_descriptor_rejects_malformed_execution_metadata(tmp_path: Path) -> None:
    folder = _write_node(tmp_path, "vendor_bad_execution", SUCCESS_NODE)
    _mutate_descriptor(
        folder,
        lambda descriptor: descriptor.__setitem__(
            "execution",
            {
                "blocking": False,
                "cancellable": True,
                "timeout_param": "missing_timeout",
            },
        ),
    )

    with pytest.raises(NodeDescriptorError, match="references unknown parameter"):
        load_node_descriptor(folder)


def test_non_strict_discovery_rejects_bad_import_without_losing_good_node(
    tmp_path: Path,
) -> None:
    _write_node(tmp_path, "vendor_good", SUCCESS_NODE)
    _write_node(tmp_path, "vendor_bad", "raise RuntimeError('bad import')")

    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])

    assert environment.registry.contains("vendor_good")
    assert not environment.registry.contains("vendor_bad")
    by_id = {item.node_id: item for item in environment.report.diagnostics}
    assert by_id["vendor_good"].status == "accepted"
    assert by_id["vendor_bad"].error_code == "runtime_import_failed"


def test_external_import_system_exit_is_structured_not_process_control(
    tmp_path: Path,
) -> None:
    _write_node(tmp_path, "vendor_system_exit", "raise SystemExit('not a CLI')")

    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])

    diagnostic = next(
        item
        for item in environment.report.diagnostics
        if item.node_id == "vendor_system_exit"
    )
    assert diagnostic.status == "rejected"
    assert diagnostic.error_code == "runtime_import_failed"
    assert "not a CLI" in diagnostic.message


def test_external_constructor_without_arbitrary_params_is_rejected(
    tmp_path: Path,
) -> None:
    _write_node(
        tmp_path,
        "vendor_incompatible_constructor",
        """
import py_trees

class GeneratedNode(py_trees.behaviour.Behaviour):
    def __init__(self, name):
        super().__init__(name=name)
    def update(self):
        return py_trees.common.Status.SUCCESS
""",
    )

    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])

    assert not environment.registry.contains("vendor_incompatible_constructor")
    diagnostic = next(
        item
        for item in environment.report.diagnostics
        if item.node_id == "vendor_incompatible_constructor"
    )
    assert diagnostic.status == "rejected"
    assert diagnostic.error_code == "runtime_import_failed"
    assert "must be name and **params only" in diagnostic.message


def test_external_constructor_with_extra_explicit_parameter_is_rejected(
    tmp_path: Path,
) -> None:
    _write_node(
        tmp_path,
        "vendor_explicit_constructor_param",
        """
import py_trees

class GeneratedNode(py_trees.behaviour.Behaviour):
    def __init__(self, name, tag, **params):
        super().__init__(name=name)
    def update(self):
        return py_trees.common.Status.SUCCESS
""",
        parameters=[
            {"name": "tag", "required": True, "schema": {"type": "string"}}
        ],
    )

    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    diagnostic = next(
        item
        for item in environment.report.diagnostics
        if item.node_id == "vendor_explicit_constructor_param"
    )
    assert diagnostic.status == "rejected"
    assert diagnostic.error_code == "runtime_import_failed"
    assert "unsupported explicit parameters: ['tag']" in diagnostic.message


def test_external_parameter_schema_is_preserved_in_effective_catalog(
    tmp_path: Path,
) -> None:
    folder = _write_node(
        tmp_path,
        "vendor_schema_projection",
        SUCCESS_NODE,
        parameters=[
            {
                "name": "retry_count",
                "required": False,
                "default": 2,
                "schema": {
                    "type": "int",
                    "constraints": {"min_inclusive": 0, "max_inclusive": 5},
                },
                "description": "Bounded retry count.",
            }
        ],
    )
    _mutate_descriptor(
        folder,
        lambda descriptor: descriptor.update(
            {
                "execution": {
                    "blocking": False,
                    "cancellable": True,
                    "timeout_param": None,
                },
                "observability": {
                    "emits_events": ["vendor_write_completed"],
                    "consumes_events": [],
                },
            }
        ),
    )

    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    entry = environment.catalog.get("vendor_schema_projection")
    parameter = entry.parameters[0]

    assert parameter.name == "retry_count"
    assert parameter.default == 2
    assert parameter.constraints == {"min_inclusive": 0, "max_inclusive": 5}
    assert environment.catalog.schema_registry.to_json_schema(parameter.schema) == {
        "type": "integer",
        "minimum": 0,
        "maximum": 5,
    }
    raw_entry = next(
        item
        for item in environment.catalog.raw_data["entries"]
        if item["id"] == "vendor_schema_projection"
    )
    assert raw_entry["execution"] == {
        "blocking": False,
        "cancellable": True,
        "timeout_param": None,
    }
    assert raw_entry["observability"] == {
        "emits_events": ["vendor_write_completed"],
        "consumes_events": [],
    }


def test_strict_discovery_rejects_bad_import(tmp_path: Path) -> None:
    _write_node(tmp_path, "vendor_bad", "raise RuntimeError('bad import')")
    with pytest.raises(NodeEnvironmentBuildError, match="strict node discovery"):
        build_node_environment(CATALOG_PATH, external_roots=[tmp_path], strict=True)


def test_duplicate_external_id_fails_closed_across_roots(tmp_path: Path) -> None:
    root_a = tmp_path / "a"
    root_b = tmp_path / "b"
    _write_node(root_a, "vendor_duplicate", SUCCESS_NODE)
    _write_node(root_b, "vendor_duplicate", SUCCESS_NODE)
    with pytest.raises(NodeEnvironmentBuildError, match="duplicate external node ids"):
        build_node_environment(CATALOG_PATH, external_roots=[root_a, root_b])


def test_external_module_is_not_retained_in_process_module_cache(tmp_path: Path) -> None:
    _write_node(tmp_path, "vendor_unloaded", SUCCESS_NODE)
    build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    assert not any(
        name.startswith("manyforge_external_vendor_unloaded_") for name in sys.modules
    )


def test_published_environment_executes_captured_source_after_file_replacement(
    tmp_path: Path,
) -> None:
    folder = _write_node(tmp_path, "vendor_immutable", SUCCESS_NODE)
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    descriptor = environment.descriptors["vendor_immutable"]
    captured_hash = descriptor.content_hash
    captured_source = descriptor.source_text

    (folder / "node.py").write_text(
        "raise RuntimeError('replacement must wait for reload')\n",
        encoding="utf-8",
    )
    behavior = environment.registry.instantiate(
        "vendor_immutable",
        NodeFactoryContext(name="generated", params={}),
    )
    behavior.tick_once()

    assert behavior.status is py_trees.common.Status.SUCCESS
    assert descriptor.content_hash == captured_hash
    assert descriptor.source_text == captured_source


def test_external_node_executes_through_runner_registry(tmp_path: Path) -> None:
    _write_node(tmp_path, "vendor_success", SUCCESS_NODE)
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    program = _program(environment, "vendor_success")
    runner = OrchestratorRunner(
        environment,
        "<memory>",
        MockKernelClient(),
        program=program,
    )
    runner.load()
    tree = runner.build_tree()
    tree.tick()
    assert tree.root.status is py_trees.common.Status.SUCCESS


def test_external_node_returning_failure_preserves_normal_tree_semantics(
    tmp_path: Path,
) -> None:
    _write_node(
        tmp_path,
        "vendor_failure",
        SUCCESS_NODE.replace(
            "py_trees.common.Status.SUCCESS",
            "py_trees.common.Status.FAILURE",
        ),
    )
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    runner = OrchestratorRunner(
        environment,
        "<memory>",
        MockKernelClient(),
        program=_program(environment, "vendor_failure"),
    )
    runner.load()
    tree = runner.build_tree()

    tree.tick()

    assert tree.root.status is py_trees.common.Status.FAILURE
    assert tree.root.feedback_message == ""


def test_generated_node_validation_cli_accepts_copyable_folder(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    folder = _write_node(tmp_path, "vendor_cli_ok", SUCCESS_NODE)

    assert cli_main(
        [
            "nodes",
            "validate",
            "--catalog",
            str(CATALOG_PATH),
            str(folder),
        ]
    ) == 0

    payload = yaml.safe_load(capsys.readouterr().out)
    assert payload["status"] == "accepted"
    assert payload["node_id"] == "vendor_cli_ok"
    assert payload["content_hash"].startswith("sha256:")


def test_generated_node_validation_cli_rejects_invalid_runtime_class(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    folder = _write_node(
        tmp_path,
        "vendor_cli_bad",
        "class GeneratedNode:\n    pass\n",
    )

    assert cli_main(
        [
            "nodes",
            "validate",
            "--catalog",
            str(CATALOG_PATH),
            str(folder),
        ]
    ) == 2
    assert "must subclass py_trees.behaviour.Behaviour" in capsys.readouterr().err


def test_external_tick_exception_becomes_failure_with_stable_feedback(
    tmp_path: Path,
) -> None:
    _write_node(
        tmp_path,
        "vendor_raises",
        """
import py_trees
class GeneratedNode(py_trees.behaviour.Behaviour):
    def __init__(self, name, **params): super().__init__(name=name)
    def update(self): raise RuntimeError('PLC disconnected')
""",
    )
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    behavior = environment.registry.instantiate(
        "vendor_raises",
        NodeFactoryContext(name="generated", params={}),
    )
    behavior.tick_once()
    assert behavior.status is py_trees.common.Status.FAILURE
    assert behavior.feedback_message.startswith("external_node_exception:RuntimeError")


def test_external_exception_diagnostics_do_not_leak_into_a_later_retry(
    tmp_path: Path,
) -> None:
    _write_node(
        tmp_path,
        "vendor_retry_after_exception",
        """
import py_trees
class GeneratedNode(py_trees.behaviour.Behaviour):
    def __init__(self, name, **params):
        super().__init__(name=name)
        self.attempt = 0
    def update(self):
        self.attempt += 1
        if self.attempt == 1:
            raise RuntimeError('first attempt failed unexpectedly')
        return py_trees.common.Status.FAILURE
""",
    )
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    behavior = environment.registry.instantiate(
        "vendor_retry_after_exception",
        NodeFactoryContext(name="generated", params={}),
    )

    behavior.tick_once()
    assert behavior._manyforge_failure_code == "external_node_exception"

    behavior.tick_once()
    assert behavior.status is py_trees.common.Status.FAILURE
    assert behavior._manyforge_failure_code is None
    assert behavior._manyforge_failure_details is None


def test_external_tick_system_exit_becomes_normal_tree_failure(
    tmp_path: Path,
) -> None:
    _write_node(
        tmp_path,
        "vendor_tick_exit",
        """
import py_trees
class GeneratedNode(py_trees.behaviour.Behaviour):
    def __init__(self, name, **params): super().__init__(name=name)
    def update(self): raise SystemExit('not process control')
""",
    )
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    behavior = environment.registry.instantiate(
        "vendor_tick_exit",
        NodeFactoryContext(name="generated", params={}),
    )

    behavior.tick_once()

    assert behavior.status is py_trees.common.Status.FAILURE
    assert behavior.feedback_message.startswith(
        "external_node_exception:SystemExit: not process control"
    )


def test_external_terminate_exception_is_contained_on_parent_invalidation(
    tmp_path: Path,
) -> None:
    _write_node(
        tmp_path,
        "vendor_terminate_raises",
        """
import py_trees
class GeneratedNode(py_trees.behaviour.Behaviour):
    def __init__(self, name, **params): super().__init__(name=name)
    def update(self): return py_trees.common.Status.RUNNING
    def terminate(self, new_status): raise RuntimeError('cancel failed')
""",
    )
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    behavior = environment.registry.instantiate(
        "vendor_terminate_raises",
        NodeFactoryContext(name="generated", params={}),
    )
    behavior.tick_once()
    assert behavior.status is py_trees.common.Status.RUNNING

    behavior.stop(py_trees.common.Status.INVALID)

    assert behavior.status is py_trees.common.Status.INVALID
    assert behavior.decorated.status is py_trees.common.Status.INVALID
    assert behavior.feedback_message.startswith(
        "external_node_exception:RuntimeError: cancel failed"
    )


def test_external_tick_exception_surfaces_structured_cycle_diagnostics(
    tmp_path: Path,
) -> None:
    _write_node(
        tmp_path,
        "vendor_cycle_raises",
        """
import py_trees
class GeneratedNode(py_trees.behaviour.Behaviour):
    def __init__(self, name, **params): super().__init__(name=name)
    def update(self): raise RuntimeError('PLC disconnected')
""",
    )
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    kernel = MockKernelClient()
    runner = OrchestratorRunner(
        environment,
        "<memory>",
        kernel,
        program=_program(environment, "vendor_cycle_raises"),
    )
    manager = CycleManager(runner, kernel)

    manager.run()
    deadline = time.monotonic() + 1.0
    while manager.state is CycleState.RUNNING and time.monotonic() < deadline:
        time.sleep(0.005)

    assert manager.state is CycleState.IDLE
    assert manager.error_code == "external_node_exception"
    assert manager.error_details == {
        "node_id": "vendor_cycle_raises",
        "exception_type": "RuntimeError",
        "message": "PLC disconnected",
    }


def test_runtime_resources_are_visible_during_external_setup(tmp_path: Path) -> None:
    _write_node(
        tmp_path,
        "vendor_resource",
        """
import py_trees
class GeneratedNode(py_trees.behaviour.Behaviour):
    def __init__(self, name, **params):
        super().__init__(name=name)
        self._handle = None
    def setup(self, **kwargs):
        bb = self.attach_blackboard_client(name='generated-resource')
        bb.register_key(key='runtime_resources', access=py_trees.common.Access.READ)
        self._handle = bb.runtime_resources.get_plc_client('default')
    def update(self):
        return py_trees.common.Status.SUCCESS if self._handle.ready else py_trees.common.Status.FAILURE
""",
        runtime_resources=[
            {
                "id": "plc_client",
                "type": "plc_client",
                "access": "read_write",
                "required": True,
                "alias_parameter": "plc_client_id",
            }
        ],
        parameters=[
            {
                "name": "plc_client_id",
                "required": False,
                "default": "default",
                "schema": {"type": "string"},
            }
        ],
    )
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    handle = SimpleNamespace(ready=True)
    resources = build_runtime_resource_registry(
        {"plc_clients": {"default": {"provider": "fake"}}},
        RuntimeResourceFactoryRegistry({"fake": lambda config: handle}),
    )
    runner = OrchestratorRunner(
        environment,
        "<memory>",
        MockKernelClient(),
        program=_program(environment, "vendor_resource"),
        runtime_resources=resources,
    )
    runner.load()
    tree = runner.build_tree()
    tree.tick()
    assert tree.root.status is py_trees.common.Status.SUCCESS


def test_runtime_resource_configuration_and_close_are_bounded_and_idempotent() -> None:
    class Handle:
        def __init__(self) -> None:
            self.closed = 0

        def close(self) -> None:
            self.closed += 1

    seen_config = None
    handle = Handle()

    def factory(config):
        nonlocal seen_config
        seen_config = dict(config)
        return handle

    resources = build_runtime_resource_registry(
        {
            "plc_clients": {
                "default": {
                    "provider": "fake",
                    "endpoint_alias": "cell_plc",
                    "timeout_s": 0.5,
                    "capabilities": ["plc.siemens.s7.write"],
                }
            }
        },
        RuntimeResourceFactoryRegistry({"fake": factory}),
    )
    assert resources.get_plc_client() is handle
    assert seen_config == {"endpoint_alias": "cell_plc", "timeout_s": 0.5}
    resources.close()
    resources.close()
    assert handle.closed == 1
    with pytest.raises(RuntimeResourceError, match="closed"):
        resources.get_plc_client()


def test_runtime_resource_close_attempts_every_handle_before_reporting_error() -> None:
    closed: list[str] = []

    class Handle:
        def __init__(self, name: str, *, fails: bool = False) -> None:
            self.name = name
            self.fails = fails

        def close(self) -> None:
            closed.append(self.name)
            if self.fails:
                raise RuntimeError(f"{self.name}_close_failed")

    resources = build_runtime_resource_registry(
        {
            "plc_clients": {
                "first": {"provider": "first"},
                "second": {"provider": "second"},
            }
        },
        RuntimeResourceFactoryRegistry(
            {
                "first": lambda config: Handle("first", fails=True),
                "second": lambda config: Handle("second"),
            }
        ),
    )
    with pytest.raises(RuntimeResourceError, match="first_close_failed"):
        resources.close()
    assert closed == ["first", "second"]


def test_runtime_resource_build_rollback_closes_shared_handles_once() -> None:
    shared = SimpleNamespace(closed=0)

    def close() -> None:
        shared.closed += 1

    shared.close = close

    def fail(config):
        raise RuntimeError("provider failed")

    with pytest.raises(RuntimeError, match="provider failed"):
        build_runtime_resource_registry(
            {
                "plc_clients": {
                    "first": {"provider": "shared"},
                    "second": {"provider": "shared"},
                    "third": {"provider": "fail"},
                }
            },
            RuntimeResourceFactoryRegistry(
                {
                    "shared": lambda config: shared,
                    "fail": fail,
                }
            ),
        )

    assert shared.closed == 1


def test_missing_selected_runtime_resource_alias_fails_before_execution(
    tmp_path: Path,
) -> None:
    _write_node(
        tmp_path,
        "vendor_alias",
        SUCCESS_NODE,
        runtime_resources=[
            {
                "id": "plc_client",
                "type": "plc_client",
                "access": "read_write",
                "required": True,
                "alias_parameter": "plc_client_id",
            }
        ],
        parameters=[
            {
                "name": "plc_client_id",
                "required": False,
                "default": "missing",
                "schema": {"type": "string"},
            }
        ],
    )
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    deployment = SimpleNamespace(
        external_nodes=SimpleNamespace(enabled=None, strict=False),
        runtime_resources={
            "plc_clients": {"default": {"provider": "fake"}}
        },
        assistant_modes={},
    )
    with pytest.raises(
        ValueError,
        match="unavailable runtime resource 'plc_client:missing'",
    ):
        validate_environment_availability(
            environment,
            deployment,
            _program(environment, "vendor_alias"),
        )


def test_omitted_external_enabled_means_all_and_explicit_empty_means_none(
    tmp_path: Path,
) -> None:
    _write_node(tmp_path, "vendor_available", SUCCESS_NODE)
    environment = build_node_environment(CATALOG_PATH, external_roots=[tmp_path])
    program = _program(environment, "vendor_available")
    deployment = SimpleNamespace(
        external_nodes=SimpleNamespace(enabled=None, strict=False),
        runtime_resources={},
        assistant_modes={},
    )
    validate_environment_availability(environment, deployment, program)
    deployment.external_nodes = SimpleNamespace(enabled=(), strict=False)
    with pytest.raises(ValueError, match="not enabled"):
        validate_environment_availability(environment, deployment, program)
