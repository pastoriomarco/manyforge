from __future__ import annotations

import ast
import importlib.util
from pathlib import Path

import pytest


_LAUNCH_FILE = (
    Path(__file__).resolve().parents[1]
    / "launch"
    / "isaac_topic_based_control.launch.py"
)


def _load_launch_module():
    spec = importlib.util.spec_from_file_location(
        "manyforge_isaac_topic_based_control_launch_test",
        _LAUNCH_FILE,
    )
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _function(tree: ast.Module, name: str) -> ast.FunctionDef:
    matches = [
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == name
    ]
    assert len(matches) == 1
    return matches[0]


def _body_after_docstring(function: ast.FunctionDef) -> list[ast.stmt]:
    assert function.body
    docstring = function.body[0]
    assert isinstance(docstring, ast.Expr)
    assert isinstance(docstring.value, ast.Constant)
    assert isinstance(docstring.value.value, str)
    return function.body[1:]


def _assert_definition_is_import_safe(function: ast.FunctionDef) -> None:
    """Reject decorators, defaults, or annotations that execute on import."""

    assert function.decorator_list == []
    assert getattr(function, "type_params", []) == []
    assert not any(
        isinstance(node, ast.Call) for node in ast.walk(function.args)
    )
    if function.returns is not None:
        assert not any(
            isinstance(node, ast.Call) for node in ast.walk(function.returns)
        )


def test_isaac_runtime_fails_before_launch_actions_are_constructed() -> None:
    source = _LAUNCH_FILE.read_text(encoding="utf-8")
    tree = ast.parse(source)

    assert not any(isinstance(node, ast.Import) for node in tree.body)
    assert [
        node.module for node in tree.body if isinstance(node, ast.ImportFrom)
    ] == ["__future__", "typing"]
    assert {
        target.id
        for node in tree.body
        if isinstance(node, ast.Assign)
        for target in node.targets
        if isinstance(target, ast.Name)
    } == {"ISAAC_RUNTIME_UNAVAILABLE_MESSAGE"}
    assert {
        node.name for node in tree.body if isinstance(node, ast.FunctionDef)
    } == {
        "generate_launch_description",
        "_launch_setup",
        "_parse_bool_launch_value",
        "_parse_bounded_int_launch_value",
        "_parse_cpu_affinity_launch_value",
    }
    for statement in tree.body:
        assert isinstance(
            statement,
            (ast.Expr, ast.ImportFrom, ast.Assign, ast.FunctionDef),
        )
        if isinstance(statement, ast.FunctionDef):
            _assert_definition_is_import_safe(statement)
        else:
            assert not any(
                isinstance(node, ast.Call) for node in ast.walk(statement)
            )

    generate = _function(tree, "generate_launch_description")
    generate_body = _body_after_docstring(generate)
    assert len(generate_body) == 1
    generate_statement = generate_body[0]
    assert isinstance(generate_statement, ast.Expr)
    generate_call = generate_statement.value
    assert isinstance(generate_call, ast.Call)
    assert isinstance(generate_call.func, ast.Name)
    assert generate_call.func.id == "_launch_setup"
    assert len(generate_call.args) == 1
    assert isinstance(generate_call.args[0], ast.Constant)
    assert generate_call.args[0].value is None
    assert generate_call.keywords == []

    setup = _function(tree, "_launch_setup")
    setup_body = _body_after_docstring(setup)
    assert len(setup_body) == 1
    setup_statement = setup_body[0]
    assert isinstance(setup_statement, ast.Raise)
    assert setup_statement.cause is None
    setup_error = setup_statement.exc
    assert isinstance(setup_error, ast.Call)
    assert isinstance(setup_error.func, ast.Name)
    assert setup_error.func.id == "RuntimeError"
    assert len(setup_error.args) == 1
    assert isinstance(setup_error.args[0], ast.Name)
    assert setup_error.args[0].id == "ISAAC_RUNTIME_UNAVAILABLE_MESSAGE"
    assert setup_error.keywords == []

    assert "DeclareLaunchArgument" not in source
    assert "LaunchConfiguration" not in source
    assert "LaunchDescription" not in source
    assert "OpaqueFunction" not in source
    assert "from launch" not in source
    assert "launch_ros" not in source
    assert "Node(" not in source
    assert "TimerAction" not in source
    assert "composer_url" not in source
    assert "_resolve_start_gripper_action_bridge" not in source

    module = _load_launch_module()

    with pytest.raises(RuntimeError) as generation_error:
        module.generate_launch_description()

    assert str(generation_error.value) == module.ISAAC_RUNTIME_UNAVAILABLE_MESSAGE

    with pytest.raises(RuntimeError) as error:
        module._launch_setup(object())

    assert str(error.value) == module.ISAAC_RUNTIME_UNAVAILABLE_MESSAGE


def test_pose_source_console_entrypoint_has_no_dormant_runtime_body(
    capsys: pytest.CaptureFixture[str],
) -> None:
    from manyforge_ros_bringup import pose_source_snapshot_publisher

    source = Path(pose_source_snapshot_publisher.__file__).read_text(
        encoding="utf-8"
    )
    assert "def _port_target_main" not in source
    assert "rclpy" not in source
    assert "urllib" not in source
    assert "manyforge_behavior.deployment" not in source

    assert pose_source_snapshot_publisher.main([]) == 78
    assert capsys.readouterr().err.strip() == (
        pose_source_snapshot_publisher.UNAVAILABLE_MESSAGE
    )


def test_demo_supervisor_entrypoint_has_no_backup_owner(
    capsys: pytest.CaptureFixture[str],
) -> None:
    from manyforge_ros_bringup import isaac_demo_supervisor

    source = Path(isaac_demo_supervisor.__file__).read_text(encoding="utf-8")
    assert "class IsaacDemoSupervisor" not in source
    assert "rclpy" not in source
    assert "urllib" not in source
    assert isaac_demo_supervisor.main() == 78
    assert capsys.readouterr().err.strip() == isaac_demo_supervisor.UNAVAILABLE_MESSAGE


def test_controller_manager_cpu_affinity_parser_accepts_ranges_and_deduplicates():
    module = _load_launch_module()

    assert module._parse_cpu_affinity_launch_value(
        "7, 9-11,10",
        name="controller_manager_cpu_affinity",
    ) == [7, 9, 10, 11]
    assert module._parse_cpu_affinity_launch_value(
        "",
        name="controller_manager_cpu_affinity",
    ) == []


@pytest.mark.parametrize(
    "value",
    ["3-1", "1,,2", "x", "1-2-3", "-1", "0-2147483647"],
)
def test_controller_manager_cpu_affinity_parser_rejects_malformed_values(value):
    module = _load_launch_module()

    with pytest.raises(ValueError):
        module._parse_cpu_affinity_launch_value(
            value,
            name="controller_manager_cpu_affinity",
        )


def test_controller_manager_priority_and_lock_parsers_are_bounded():
    module = _load_launch_module()

    assert module._parse_bool_launch_value("true", name="lock_memory") is True
    assert module._parse_bool_launch_value("OFF", name="lock_memory") is False
    assert (
        module._parse_bounded_int_launch_value(
            "50",
            name="thread_priority",
            minimum=1,
            maximum=99,
        )
        == 50
    )
    with pytest.raises(ValueError):
        module._parse_bool_launch_value("sometimes", name="lock_memory")
    with pytest.raises(ValueError):
        module._parse_bounded_int_launch_value(
            "100",
            name="thread_priority",
            minimum=1,
            maximum=99,
        )
