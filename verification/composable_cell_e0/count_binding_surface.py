from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path


def count(pattern: str, text: str) -> int:
    return len(re.findall(pattern, text))


def inspect(files: list[Path], root: Path) -> dict[str, object]:
    text = "\n".join(path.read_text(encoding="utf-8") for path in files)
    special_patterns = {
        "buffer_protocol": r"\bpy::buffer_protocol\b",
        "custom_holder_declaration": r"\bPYBIND11_DECLARE_HOLDER_TYPE\b",
        "dynamic_attr": r"\bpy::dynamic_attr\b",
        "embedding_or_subinterpreter": (
            r"\b(?:py::scoped_interpreter|subinterpreter|"
            r"multiple_interpreters::)"
        ),
        "eval": r"\bpy::eval\b",
        "implicit_conversion": r"\bpy::implicitly_convertible\b",
        "keep_alive": r"\bpy::keep_alive\b",
        "metaclass": r"\bpy::metaclass\b",
        "module_local": r"\bpy::module_local\b",
        "multiple_inheritance": r"\bpy::multiple_inheritance\b",
        "nodelete": r"\bpy::nodelete\b",
        "pickle": r"\bpy::pickle\b",
        "python_trampoline_override": r"\bPYBIND11_OVERRIDE",
    }
    return {
        "files": [str(path.relative_to(root)) for path in files],
        "file_count": len(files),
        "line_count": sum(
            len(path.read_text(encoding="utf-8").splitlines()) for path in files
        ),
        "py_class": count(r"\bpy::class_<", text),
        "py_enum": count(r"\bpy::enum_<", text),
        "def_readwrite": count(r"\.def_readwrite(?:_static)?\s*\(", text),
        "def_readonly_or_property_readonly": count(
            r"\.(?:def_readonly(?:_static)?|def_property_readonly(?:_static)?)\s*\(",
            text,
        ),
        "raw_reference_policy": count(
            r"return_value_policy::reference(?!_internal)", text
        ),
        "reference_internal_policy": count(
            r"return_value_policy::reference_internal", text
        ),
        "shared_ptr_occurrences": count(r"\bstd::shared_ptr\b", text),
        "gil_release_occurrences": count(
            r"\b(?:py::gil_scoped_release|call_guard<\s*py::gil_scoped_release)",
            text,
        ),
        "lambda_constructor_occurrences": count(r"\bpy::init\s*\(\s*\[", text),
        "registered_exception_occurrences": count(
            r"\bpy::register_exception(?:_translator)?\b", text
        ),
        "py_dict_occurrences": count(r"\bpy::dict\b", text),
        "py_object_occurrences": count(r"\bpy::object\b", text),
        "py_none_occurrences": count(r"\bpy::none\b", text),
        "py_none_default_occurrences": count(
            r"py::arg\s*\([^)]*\)\s*=\s*py::none\s*\(\s*\)", text
        ),
        "python_callable_occurrences": count(
            r"\b(?:py::function|std::function\s*<[^>]*py::object)", text
        ),
        "removed_or_special_feature_occurrences": {
            name: count(pattern, text)
            for name, pattern in special_patterns.items()
        },
    }


def declaration_risk_audit(files: list[Path], root: Path) -> dict[str, object]:
    patterns = {
        "custom_allocation_or_deallocation": re.compile(
            r"\boperator\s+(?:new|delete)\b"
        ),
        "enable_shared_from_this": re.compile(r"\benable_shared_from_this\b"),
        "multiple_public_bases_candidate": re.compile(
            r"class\s+[^;{]+:\s*public\s+[^;{,]+,\s*public\s+[^;{]+{",
            re.MULTILINE,
        ),
        "pybind_custom_holder": re.compile(
            r"\bPYBIND11_DECLARE_HOLDER_TYPE\b"
        ),
    }
    matches: list[dict[str, object]] = []
    for path in files:
        text = path.read_text(encoding="utf-8")
        for name, pattern in patterns.items():
            for match in pattern.finditer(text):
                line = text.count("\n", 0, match.start()) + 1
                matches.append(
                    {
                        "kind": name,
                        "path": str(path.relative_to(root)),
                        "line": line,
                        "excerpt": " ".join(match.group(0).split())[:240],
                    }
                )
    return {
        "scope": (
            "candidate declarations under core/deployment/RoboPlan include "
            "trees; matches require manual bound-type review"
        ),
        "file_count": len(files),
        "matches": matches,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument(
        "--allow-dirty",
        action="store_true",
        help="permit exploratory counts that are invalid as retained evidence",
    )
    args = parser.parse_args()
    root = args.source_root.resolve()

    core_files = sorted((root / "manyforge_core" / "python").glob("*.cpp"))
    roboplan_files = [root / "manyforge_roboplan" / "python" / "bindings.cpp"]
    if not core_files or not all(path.is_file() for path in roboplan_files):
        raise SystemExit("source root does not contain both binding surfaces")
    declaration_files = sorted(
        [
            *(
                root / "manyforge_core" / "include"
            ).rglob("*.hpp"),
            *(
                root / "manyforge_deployment" / "include"
            ).rglob("*.hpp"),
            *(
                root / "manyforge_roboplan" / "include"
            ).rglob("*.hpp"),
        ]
    )

    status = subprocess.check_output(
        ["git", "-C", str(root), "status", "--short"], text=True
    ).splitlines()
    if status and not args.allow_dirty:
        raise SystemExit(
            "source tree is dirty; commit/review it first or use "
            "--allow-dirty for non-evidence exploration"
        )

    result = {
        "git_commit": subprocess.check_output(
            ["git", "-C", str(root), "rev-parse", "HEAD"], text=True
        ).strip(),
        "git_status_short": status,
        "manyforge_core_py": inspect(core_files, root),
        "manyforge_roboplan_py": inspect(roboplan_files, root),
        "candidate_bound_declaration_risks": declaration_risk_audit(
            declaration_files, root
        ),
        "checked_in_pyi_files": subprocess.check_output(
            ["git", "-C", str(root), "ls-files", "--", "*.pyi"],
            text=True,
        ).splitlines(),
    }
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
