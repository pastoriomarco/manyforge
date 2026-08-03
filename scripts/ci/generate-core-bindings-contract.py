#!/usr/bin/env python3
"""Generate the deterministic public contract for ``manyforge_core_py``.

The generator imports one explicitly named extension and derives all three
human/machine-readable outputs from that exact runtime module.  It never
searches ``sys.path`` for a substitute module and never overwrites an output.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import stat
import sys
import sysconfig
from types import ModuleType
from typing import Any, get_origin


_MODULE_NAME = "manyforge_core_py"
_API_SCHEMA = "manyforge.composable_cell_i2.binding_api_inventory.v1"
_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_TRUSTED_PROJECTIONS = frozenset(
    {
        "RevisionVerifiedDeploymentArtifact",
        "RevisionVerifiedDeploymentApplicabilityInputs",
        "VerifiedSiteOverlayArtifactProjection",
        "QualifiedSelectedSiteOverlay",
        "GraphRevisionVerifiedCellGraph",
        "ModelSourceRetrievalPlan",
        "ModelSourceIr",
        "SourceVerifiedModelAsset",
        "CompositionQualifiedCellIr",
        "ApplicabilityQualifiedCellIr",
        "CompilerQualifiedCellIr",
    }
)


def _fail(message: str) -> None:
    raise SystemExit(message)


def _plain_directory(raw: str, label: str) -> Path:
    path = Path(raw)
    if not path.is_absolute():
        _fail(f"{label} must be absolute")
    try:
        metadata = path.lstat()
        resolved = path.resolve(strict=True)
    except OSError as error:
        _fail(f"{label} is unavailable: {error}")
    if (
        not stat.S_ISDIR(metadata.st_mode)
        or stat.S_ISLNK(metadata.st_mode)
        or resolved != path
    ):
        _fail(f"{label} must be one canonical plain directory")
    return path


def _plain_file(raw: str, label: str) -> Path:
    path = Path(raw)
    if not path.is_absolute():
        _fail(f"{label} must be absolute")
    try:
        metadata = path.lstat()
        resolved = path.resolve(strict=True)
    except OSError as error:
        _fail(f"{label} is unavailable: {error}")
    if (
        not stat.S_ISREG(metadata.st_mode)
        or stat.S_ISLNK(metadata.st_mode)
        or metadata.st_nlink != 1
        or metadata.st_size == 0
        or resolved != path
    ):
        _fail(f"{label} must be one canonical nonempty regular file")
    return path


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _load_exact_extension(path: Path) -> ModuleType:
    suffix = sysconfig.get_config_var("EXT_SUFFIX")
    if not isinstance(suffix, str) or path.name != f"{_MODULE_NAME}{suffix}":
        _fail("extension filename differs from the active CPython ABI")
    with path.open("rb") as stream:
        magic = stream.read(4)
    if magic != b"\x7fELF":
        _fail("extension is not ELF")
    if _MODULE_NAME in sys.modules:
        _fail("manyforge_core_py was imported before exact-path loading")
    spec = importlib.util.spec_from_file_location(_MODULE_NAME, path)
    if spec is None or spec.loader is None:
        _fail("cannot form an exact-path extension import")
    module = importlib.util.module_from_spec(spec)
    sys.modules[_MODULE_NAME] = module
    try:
        spec.loader.exec_module(module)
    except BaseException:
        sys.modules.pop(_MODULE_NAME, None)
        raise
    origin = getattr(getattr(module, "__spec__", None), "origin", None)
    if (
        not isinstance(module.__file__, str)
        or Path(module.__file__).resolve(strict=True) != path
        or not isinstance(origin, str)
        or Path(origin).resolve(strict=True) != path
    ):
        _fail("loaded module origin differs from the exact extension")
    return module


def _public_exports(module: ModuleType) -> list[tuple[str, Any]]:
    exports = []
    for name in sorted(name for name in dir(module) if not name.startswith("_")):
        if not _IDENTIFIER.fullmatch(name):
            _fail(f"runtime export is not a Python identifier: {name!r}")
        exports.append((name, getattr(module, name)))
    if not exports:
        _fail("runtime module has no public exports")
    return exports


def _enum_members(value: Any) -> dict[str, Any] | None:
    members = getattr(value, "__members__", None)
    if isinstance(value, type) and isinstance(members, dict):
        return members
    return None


def _properties(value: type[Any]) -> dict[str, property]:
    return {
        name: descriptor
        for name, descriptor in vars(value).items()
        if not name.startswith("_") and isinstance(descriptor, property)
    }


def _has_documented_constructor(value: type[Any]) -> bool:
    """Report whether this pybind type publishes a callable constructor.

    pybind installs a generic ``__init__`` descriptor even for classes that
    intentionally have no bound constructor.  Only a real binding publishes
    an ``__init__(...)`` signature in that class-local descriptor's docstring.
    Reading the signature keeps the inventory descriptive: the trusted-type
    list must never be able to mask an accidentally exposed constructor.
    """

    constructor = vars(value).get("__init__")
    signatures = _documented_signatures("__init__", constructor)
    return any(
        parameters not in {"*args: Any, **kwargs: Any", ""}
        for parameters, _ in signatures
    )


def _export_row(name: str, value: Any) -> dict[str, Any]:
    members = _enum_members(value)
    if members is not None:
        kind = "enum"
        constructible = False
        writable: list[str] = []
    elif isinstance(value, type):
        kind = "class"
        constructible = _has_documented_constructor(value)
        writable = sorted(
            field
            for field, descriptor in _properties(value).items()
            if descriptor.fset is not None
        )
    elif get_origin(value) is not None:
        # The extension publishes closed ``typing.Union`` aliases for C++
        # variants.  They are callable implementation objects, but they are
        # type declarations rather than runtime functions.
        kind = "constant"
        constructible = False
        writable = []
    elif callable(value):
        kind = "function"
        constructible = False
        writable = []
    else:
        kind = "constant"
        constructible = False
        writable = []
    return {
        "name": name,
        "kind": kind,
        "constructible": constructible,
        "writable_attributes": writable,
        "trusted_projection": name in _TRUSTED_PROJECTIONS,
    }


def _split_parameters(value: str) -> list[str]:
    result: list[str] = []
    start = 0
    stack: list[str] = []
    quote: str | None = None
    escaped = False
    pairs = {")": "(", "]": "[", "}": "{"}
    for index, character in enumerate(value):
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {"'", '"'}:
            quote = character
        elif character in "([{":
            stack.append(character)
        elif character in pairs:
            if stack and stack[-1] == pairs[character]:
                stack.pop()
        elif character == "," and not stack:
            result.append(value[start:index].strip())
            start = index + 1
    result.append(value[start:].strip())
    return [item for item in result if item]


def _top_level_default(parameter: str) -> bool:
    stack: list[str] = []
    quote: str | None = None
    escaped = False
    pairs = {")": "(", "]": "[", "}": "{"}
    for character in parameter:
        if quote is not None:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = None
            continue
        if character in {"'", '"'}:
            quote = character
        elif character in "([{":
            stack.append(character)
        elif character in pairs:
            if stack and stack[-1] == pairs[character]:
                stack.pop()
        elif character == "=" and not stack:
            return True
    return False


def _stub_parameters(raw: str) -> str | None:
    rendered: list[str] = []
    for parameter in _split_parameters(raw):
        if parameter in {"/", "*"}:
            rendered.append(parameter)
            continue
        prefix = ""
        remainder = parameter
        if remainder.startswith("**"):
            prefix, remainder = "**", remainder[2:]
        elif remainder.startswith("*"):
            prefix, remainder = "*", remainder[1:]
        name = re.split(r"[:=]", remainder, maxsplit=1)[0].strip()
        if not _IDENTIFIER.fullmatch(name):
            return None
        if name in {"self", "cls"} and not prefix:
            rendered.append(name)
            continue
        default = " = ..." if _top_level_default(parameter) and not prefix else ""
        rendered.append(f"{prefix}{name}: Any{default}")
    return ", ".join(rendered)


def _documented_signatures(name: str, value: Any) -> list[tuple[str, bool]]:
    document = getattr(value, "__doc__", "") or ""
    starts = list(re.finditer(rf"(?m)(?:^\d+\.\s*)?{re.escape(name)}\(", document))
    signatures: list[tuple[str, bool]] = []
    for match in starts:
        open_index = document.find("(", match.start())
        depth = 0
        quote: str | None = None
        escaped = False
        close_index: int | None = None
        for index in range(open_index, len(document)):
            character = document[index]
            if quote is not None:
                if escaped:
                    escaped = False
                elif character == "\\":
                    escaped = True
                elif character == quote:
                    quote = None
                continue
            if character in {"'", '"'}:
                quote = character
            elif character == "(":
                depth += 1
            elif character == ")":
                depth -= 1
                if depth == 0:
                    close_index = index
                    break
        if close_index is None:
            continue
        parameters = _stub_parameters(document[open_index + 1 : close_index])
        if parameters is None:
            continue
        remainder = document[close_index + 1 :].lstrip()
        has_return = remainder.startswith("->")
        candidate = (parameters, has_return)
        if candidate not in signatures:
            signatures.append(candidate)
    detailed = [
        signature
        for signature in signatures
        if signature[0] not in {"*args: Any, **kwargs: Any", ""}
    ]
    return detailed or signatures


def _function_stub(name: str, value: Any, indent: str = "") -> list[str]:
    signatures = _documented_signatures(name, value)
    if not signatures:
        signatures = [("*args: Any, **kwargs: Any", False)]
    lines: list[str] = []
    for parameters, has_return in signatures:
        if len(signatures) > 1:
            lines.append(f"{indent}@overload")
        returns = " -> Any" if has_return else ""
        lines.append(f"{indent}def {name}({parameters}){returns}: ...")
    return lines


def _stub(exports: list[tuple[str, Any]], rows: dict[str, dict[str, Any]]) -> str:
    lines = ["from typing import Any, overload", ""]
    for name, value in exports:
        row = rows[name]
        if row["kind"] == "function":
            lines.extend(_function_stub(name, value))
        elif row["kind"] == "constant":
            lines.append(f"{name}: Any")
        elif row["kind"] == "enum":
            lines.append(f"class {name}:")
            members = _enum_members(value)
            assert members is not None
            for member in sorted(members):
                if not _IDENTIFIER.fullmatch(member):
                    _fail(f"enum member is not a Python identifier: {name}.{member}")
                lines.append(f"    {member}: {name}")
        else:
            lines.append(f"class {name}:")
            body: list[str] = []
            if row["constructible"]:
                constructor = vars(value).get("__init__")
                body.extend(_function_stub("__init__", constructor, "    "))
            for field, descriptor in sorted(_properties(value).items()):
                body.extend(
                    [
                        "    @property",
                        f"    def {field}(self) -> Any: ...",
                    ]
                )
                if descriptor.fset is not None:
                    body.extend(
                        [
                            f"    @{field}.setter",
                            f"    def {field}(self, value: Any) -> None: ...",
                        ]
                    )
            lines.extend(body or ["    ..."])
        lines.append("")
    rendered = "\n".join(lines).rstrip() + "\n"
    try:
        ast.parse(rendered, filename=f"{_MODULE_NAME}.pyi")
    except SyntaxError as error:
        _fail(f"derived stub is invalid Python: {error}")
    return rendered


def _documentation(
    exports: list[tuple[str, Any]], rows: dict[str, dict[str, Any]]
) -> str:
    lines = [
        "# `manyforge_core_py` API",
        "",
        "Generated from the exact candidate CPython extension.",
        "",
    ]
    for name, value in exports:
        row = rows[name]
        lines.extend([f"### `{name}`", "", f"Kind: `{row['kind']}`.", ""])
        if row["trusted_projection"]:
            fields = ", ".join(f"`{field}`" for field in sorted(_properties(value)))
            lines.extend(
                [
                    "This is a read-only, non-constructible trusted projection.",
                    f"Fields: {fields}.",
                    "",
                ]
            )
        members = _enum_members(value)
        if members is not None:
            lines.extend(
                [
                    "Members: "
                    + ", ".join(f"`{member}`" for member in sorted(members))
                    + ".",
                    "",
                ]
            )
        document = (getattr(value, "__doc__", "") or "").strip()
        lines.extend(
            [
                "Runtime signature and documentation:",
                "",
                "```text",
                document or "No runtime docstring is published.",
                "```",
                "",
            ]
        )
    return "\n".join(lines).rstrip() + "\n"


def _write_exclusive(root: Path, name: str, payload: bytes) -> None:
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    flags |= getattr(os, "O_NOFOLLOW", 0)
    directory_flags = os.O_RDONLY | os.O_DIRECTORY
    directory_flags |= getattr(os, "O_NOFOLLOW", 0)
    directory_fd = os.open(root, directory_flags)
    try:
        descriptor = os.open(name, flags, 0o644, dir_fd=directory_fd)
    finally:
        os.close(directory_fd)
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
            _fail(f"generated output is not one regular file: {name}")
        view = memoryview(payload)
        while view:
            written = os.write(descriptor, view)
            if written <= 0:
                _fail(f"short write while creating generated output: {name}")
            view = view[written:]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--extension", required=True)
    parser.add_argument("--output-root", required=True)
    arguments = parser.parse_args()

    extension = _plain_file(arguments.extension, "candidate extension")
    output_root = _plain_directory(arguments.output_root, "generated output root")
    expected_extension = output_root / extension.name
    if expected_extension != extension:
        expected_extension = _plain_file(
            str(expected_extension), "generated extension copy"
        )
        if _sha256(expected_extension) != _sha256(extension):
            _fail("generated extension copy differs from the candidate extension")
    if sorted(entry.name for entry in output_root.iterdir()) != [extension.name]:
        _fail("generated output root must initially contain only the extension")

    module = _load_exact_extension(extension)
    exports = _public_exports(module)
    rows = {name: _export_row(name, value) for name, value in exports}
    inventory = {
        "schema_version": _API_SCHEMA,
        "module": _MODULE_NAME,
        "framework": "pybind11",
        "python_abi": {
            "implementation": platform.python_implementation(),
            "version": platform.python_version(),
            "soabi": sysconfig.get_config_var("SOABI"),
            "extension_suffix": sysconfig.get_config_var("EXT_SUFFIX"),
            "gil_disabled": False,
        },
        "exports": [rows[name] for name, _ in exports],
    }
    stub = _stub(exports, rows).encode("utf-8")
    api = (json.dumps(inventory, indent=2, sort_keys=True) + "\n").encode("utf-8")
    documentation = _documentation(exports, rows).encode("utf-8")
    _write_exclusive(output_root, f"{_MODULE_NAME}.pyi", stub)
    _write_exclusive(output_root, f"{_MODULE_NAME}.api.json", api)
    _write_exclusive(output_root, f"{_MODULE_NAME}-api.md", documentation)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
