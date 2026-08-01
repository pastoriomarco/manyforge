"""Source-level acceptance for pre-I10 xArm product boundaries."""

from __future__ import annotations

import ast
import importlib
from pathlib import Path

import pytest


_PACKAGE_DIRECTORY = Path(__file__).resolve().parents[1] / "manyforge_xarm_adapter"
_ENTRYPOINT_MODULE_NAMES = (
    "manyforge_xarm_adapter.controller_gate_evidence",
    "manyforge_xarm_adapter.execution_admission_provider_launch",
    "manyforge_xarm_adapter.live_controller_parameter_validation",
    "manyforge_xarm_adapter.live_evidence",
    "manyforge_xarm_adapter.read_only_observer",
    "manyforge_xarm_adapter.supervised_command_evidence",
    "manyforge_xarm_adapter.supervised_runtime_binding",
)
_ALLOWED_IMPORT_ROOTS = {
    "__future__",
    "dataclasses",
    "datetime",
    "hashlib",
    "ipaddress",
    "math",
    "sys",
    "typing",
}


def _assert_definition_is_import_safe(function: ast.FunctionDef) -> None:
    assert function.decorator_list == []
    assert getattr(function, "type_params", []) == []
    assert not any(isinstance(node, ast.Call) for node in ast.walk(function.args))
    if function.returns is not None:
        assert not any(
            isinstance(node, ast.Call) for node in ast.walk(function.returns)
        )


def _assert_exact_unavailable_main(module_name: str) -> None:
    path = _PACKAGE_DIRECTORY / (module_name.rsplit(".", 1)[1] + ".py")
    tree = ast.parse(path.read_text(encoding="utf-8"))
    imported_roots: set[str] = set()
    for node in tree.body:
        if isinstance(node, ast.Import):
            imported_roots.update(
                alias.name.partition(".")[0] for alias in node.names
            )
        elif isinstance(node, ast.ImportFrom):
            assert node.module is not None
            imported_roots.add(node.module.partition(".")[0])
        elif isinstance(node, ast.FunctionDef):
            _assert_definition_is_import_safe(node)
    assert imported_roots <= _ALLOWED_IMPORT_ROOTS

    matches = [
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == "main"
    ]
    assert len(matches) == 1
    main = matches[0]
    _assert_definition_is_import_safe(main)
    assert main.body
    docstring = main.body[0]
    assert isinstance(docstring, ast.Expr)
    assert isinstance(docstring.value, ast.Constant)
    assert isinstance(docstring.value.value, str)
    assert len(main.body) == 3

    print_statement = main.body[1]
    assert isinstance(print_statement, ast.Expr)
    print_call = print_statement.value
    assert isinstance(print_call, ast.Call)
    assert isinstance(print_call.func, ast.Name)
    assert print_call.func.id == "print"
    assert len(print_call.args) == 1
    assert isinstance(print_call.args[0], ast.Name)
    assert print_call.args[0].id == "UNAVAILABLE_MESSAGE"
    assert len(print_call.keywords) == 1
    assert print_call.keywords[0].arg == "file"
    output = print_call.keywords[0].value
    assert isinstance(output, ast.Attribute)
    assert output.attr == "stderr"
    assert isinstance(output.value, ast.Name)
    assert output.value.id == "sys"

    return_statement = main.body[2]
    assert isinstance(return_statement, ast.Return)
    assert isinstance(return_statement.value, ast.Constant)
    assert return_statement.value.value == 78


def _load_entrypoint_module(module_name: str):
    # Inspect the effect boundary before importing or invoking the module.
    _assert_exact_unavailable_main(module_name)
    return importlib.import_module(module_name)


@pytest.mark.parametrize(
    "module_name",
    _ENTRYPOINT_MODULE_NAMES,
    ids=lambda value: value.rsplit(".", 1)[1],
)
def test_runtime_entrypoints_fail_before_effects(
    module_name: str,
    capsys: pytest.CaptureFixture[str],
) -> None:
    module = _load_entrypoint_module(module_name)
    assert module.main() == 78
    assert capsys.readouterr().err.strip() == module.UNAVAILABLE_MESSAGE


def test_only_effect_free_xarm_helpers_survive_the_split() -> None:
    controller_gate_evidence = _load_entrypoint_module(
        "manyforge_xarm_adapter.controller_gate_evidence"
    )
    execution_admission_provider_launch = _load_entrypoint_module(
        "manyforge_xarm_adapter.execution_admission_provider_launch"
    )
    live_controller_parameter_validation = _load_entrypoint_module(
        "manyforge_xarm_adapter.live_controller_parameter_validation"
    )
    live_evidence = _load_entrypoint_module("manyforge_xarm_adapter.live_evidence")
    read_only_observer = _load_entrypoint_module(
        "manyforge_xarm_adapter.read_only_observer"
    )
    supervised_command_evidence = _load_entrypoint_module(
        "manyforge_xarm_adapter.supervised_command_evidence"
    )
    supervised_runtime_binding = _load_entrypoint_module(
        "manyforge_xarm_adapter.supervised_runtime_binding"
    )

    assert callable(controller_gate_evidence.max_pose_deviation)
    assert callable(
        live_controller_parameter_validation.expected_live_controller_parameters
    )
    assert callable(
        live_controller_parameter_validation.validate_live_controller_parameters
    )
    assert callable(live_evidence.assess_probe)
    assert callable(live_evidence.evaluate_postcondition)
    assert callable(supervised_runtime_binding.validate_robot_ipv4)

    assert not hasattr(
        execution_admission_provider_launch, "admission_provider_command"
    )
    assert not hasattr(
        supervised_runtime_binding, "resolve_supervised_runtime_binding"
    )
    assert not hasattr(
        supervised_command_evidence, "capture_supervised_command_evidence"
    )
    assert not hasattr(read_only_observer, "ReadOnlyObserver")
