from __future__ import annotations

import argparse
import ast
import hashlib
import json
from pathlib import Path


REQUIRED_CLASSES = {
    "ArtifactCodecError",
    "CellId",
    "KernelEventType",
    "OptionalConfig",
    "ParsedArtifactCandidate",
}
REQUIRED_FUNCTIONS = {
    "compile_closed_schema",
    "compile_schema_revision_repeated",
    "materialize_execution_snapshot",
    "materialize_kernel_event",
    "parse_yaml",
    "pose_round_trip",
    "qualify_empty_cell",
}


def annotation_text(value: ast.expr | None) -> str:
    return "" if value is None else ast.unparse(value)


def class_method(class_node: ast.ClassDef, name: str) -> ast.FunctionDef:
    for node in class_node.body:
        if isinstance(node, ast.FunctionDef) and node.name == name:
            return node
    raise SystemExit(f"stub parity failure: {class_node.name}.{name} is absent")


def class_members(class_node: ast.ClassDef) -> set[str]:
    result: set[str] = set()
    for node in class_node.body:
        if isinstance(node, ast.Assign):
            result.update(
                target.id
                for target in node.targets
                if isinstance(target, ast.Name)
            )
        elif isinstance(node, ast.AnnAssign) and isinstance(
            node.target, ast.Name
        ):
            result.add(node.target.id)
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stub", type=Path, required=True)
    args = parser.parse_args()

    stub = args.stub.resolve()
    payload = stub.read_bytes()
    module = ast.parse(payload.decode("utf-8"), filename=str(stub))
    classes = {
        node.name: node
        for node in module.body
        if isinstance(node, ast.ClassDef)
    }
    functions = {
        node.name: node
        for node in module.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }

    missing_classes = sorted(REQUIRED_CLASSES - classes.keys())
    missing_functions = sorted(REQUIRED_FUNCTIONS - functions.keys())
    if missing_classes or missing_functions:
        raise SystemExit(
            "stub parity failure: "
            f"missing classes={missing_classes}, functions={missing_functions}"
        )

    error_bases = {
        annotation_text(base) for base in classes["ArtifactCodecError"].bases
    }
    if not any(base.endswith("ValueError") for base in error_bases):
        raise SystemExit("ArtifactCodecError is not typed as a ValueError")

    enum_bases = {annotation_text(base) for base in classes["KernelEventType"].bases}
    if not any(base.endswith("Enum") for base in enum_bases):
        raise SystemExit("KernelEventType is not typed as an enum")
    missing_enum_members = {
        "StateTransition",
        "ExecuteStarted",
    } - class_members(classes["KernelEventType"])
    if missing_enum_members:
        raise SystemExit(
            "KernelEventType members missing from stub: "
            f"{sorted(missing_enum_members)}"
        )

    cell_init = class_method(classes["CellId"], "__init__")
    cell_value_argument = next(
        (argument for argument in cell_init.args.args if argument.arg == "value"),
        None,
    )
    if (
        cell_value_argument is None
        or annotation_text(cell_value_argument.annotation)
        not in {"str", "builtins.str"}
    ):
        raise SystemExit("CellId(value: str) signature is absent")
    cell_hash_return = annotation_text(
        class_method(classes["CellId"], "__hash__").returns
    )
    if not cell_hash_return.endswith("int"):
        raise SystemExit("CellId.__hash__ return type is not int")
    if not annotation_text(class_method(classes["CellId"], "value").returns).endswith(
        "str"
    ):
        raise SystemExit("CellId.value return type is not str")

    parsed_candidate_methods = {
        node.name
        for node in classes["ParsedArtifactCandidate"].body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }
    if "__init__" in parsed_candidate_methods:
        raise SystemExit("trusted ParsedArtifactCandidate became constructible")

    optional_init = class_method(classes["OptionalConfig"], "__init__")
    if not optional_init.args.defaults:
        raise SystemExit("OptionalConfig note=None default is absent from the stub")
    if not isinstance(optional_init.args.defaults[-1], ast.Constant):
        raise SystemExit("OptionalConfig final default is not a literal")
    if optional_init.args.defaults[-1].value is not None:
        raise SystemExit("OptionalConfig note default is not None")
    note_argument = next(
        (
            argument
            for argument in optional_init.args.args
            if argument.arg == "note"
        ),
        None,
    )
    note_annotation = (
        "" if note_argument is None else annotation_text(note_argument.annotation)
    )
    if "str" not in note_annotation or not (
        "None" in note_annotation or "Optional" in note_annotation
    ):
        raise SystemExit("OptionalConfig note is not typed as optional str")

    expected_returns = {
        "compile_closed_schema": "dict",
        "compile_schema_revision_repeated": "str",
        "materialize_execution_snapshot": "dict",
        "materialize_kernel_event": "dict",
        "parse_yaml": "ParsedArtifactCandidate",
        "pose_round_trip": "dict",
        "qualify_empty_cell": "dict",
    }
    for name, expected in expected_returns.items():
        actual = annotation_text(functions[name].returns)
        if not actual.endswith(expected):
            raise SystemExit(
                f"{name} return annotation mismatch: expected {expected}, got {actual}"
            )

    print(
        json.dumps(
            {
                "stub": str(stub),
                "bytes": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest(),
                "required_classes": sorted(REQUIRED_CLASSES),
                "required_functions": sorted(REQUIRED_FUNCTIONS),
                "artifact_error_is_value_error": True,
                "enum_members": ["ExecuteStarted", "StateTransition"],
                "strong_id_signature_and_hash": True,
                "key_return_annotations": expected_returns,
                "trusted_stage_constructible": False,
                "optional_none_default": True,
            },
            indent=2,
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()
