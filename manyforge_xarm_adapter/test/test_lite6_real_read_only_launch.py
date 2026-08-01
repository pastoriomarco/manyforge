"""Focused source contract for the pre-I10 real-Lite6 launch fence.

The six historical launch definitions remain acceptance input at blob
``e3cff1010dd18992066fafb9329fe9ce8c018fa4``. Driver parameters, model and
observer construction, address/report validation, and shell coordination must
be adapted to retained products at I10/R; they are not current lane evidence.
"""

from __future__ import annotations

import ast
import importlib.util
from pathlib import Path

import pytest


_LAUNCH_FILE = (
    Path(__file__).resolve().parents[1]
    / "launch"
    / "lite6_real_read_only.launch.py"
)


def _load_launch_module():
    spec = importlib.util.spec_from_file_location(
        "manyforge_lite6_real_read_only_launch_test",
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


def test_launch_fails_before_model_or_ros_node_construction() -> None:
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
    } == {"UNAVAILABLE_REASON_CODE", "UNAVAILABLE_MESSAGE"}
    assert {
        node.name for node in tree.body if isinstance(node, ast.FunctionDef)
    } == {"_unavailable", "generate_launch_description"}
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

    unavailable = _function(tree, "_unavailable")
    unavailable_body = _body_after_docstring(unavailable)
    assert len(unavailable_body) == 2
    delete_statement, raise_statement = unavailable_body
    assert isinstance(delete_statement, ast.Delete)
    assert [
        target.id
        for target in delete_statement.targets
        if isinstance(target, ast.Name)
    ] == ["context", "args", "kwargs"]
    assert isinstance(raise_statement, ast.Raise)
    assert raise_statement.cause is None
    unavailable_error = raise_statement.exc
    assert isinstance(unavailable_error, ast.Call)
    assert isinstance(unavailable_error.func, ast.Name)
    assert unavailable_error.func.id == "RuntimeError"
    assert len(unavailable_error.args) == 1
    assert isinstance(unavailable_error.args[0], ast.Name)
    assert unavailable_error.args[0].id == "UNAVAILABLE_MESSAGE"
    assert unavailable_error.keywords == []

    generate = _function(tree, "generate_launch_description")
    generate_body = _body_after_docstring(generate)
    assert len(generate_body) == 1
    generate_statement = generate_body[0]
    assert isinstance(generate_statement, ast.Expr)
    generate_call = generate_statement.value
    assert isinstance(generate_call, ast.Call)
    assert isinstance(generate_call.func, ast.Name)
    assert generate_call.func.id == "_unavailable"
    assert len(generate_call.args) == 1
    assert isinstance(generate_call.args[0], ast.Constant)
    assert generate_call.args[0].value is None
    assert generate_call.keywords == []

    for forbidden in (
        "from launch",
        "LaunchDescription",
        "OpaqueFunction",
        "launch_ros.actions",
        "xarm_driver_node",
        "read_text(",
        "compile_lite6_binding(",
    ):
        assert forbidden not in source

    module = _load_launch_module()

    with pytest.raises(
        RuntimeError,
        match=module.UNAVAILABLE_REASON_CODE,
    ) as generation_error:
        module.generate_launch_description()

    assert str(generation_error.value) == module.UNAVAILABLE_MESSAGE

    with pytest.raises(RuntimeError, match=module.UNAVAILABLE_REASON_CODE) as error:
        module._unavailable(None)

    assert str(error.value) == module.UNAVAILABLE_MESSAGE
