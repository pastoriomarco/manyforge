#!/usr/bin/env python3
"""Focused invariants for the target-scoped native Stop closure generator."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("generate_native_stop_build_closure.py")
sys.dont_write_bytecode = True
SPEC = importlib.util.spec_from_file_location("native_stop_closure", MODULE_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot load native Stop closure generator")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class NativeStopClosureGeneratorTest(unittest.TestCase):
    def roots(self, parent: Path, name: str) -> tuple[Path, Path]:
        source = parent / name / "source"
        build = parent / name / "build"
        source.mkdir(parents=True)
        build.mkdir(parents=True)
        return source, build

    def test_source_and_build_relocation_preserve_logical_ids(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source_a, build_a = self.roots(root, "a")
            source_b, build_b = self.roots(root, "b")
            for source in (source_a, source_b):
                path = source / "pkg" / "backend.cpp"
                path.parent.mkdir()
                path.write_text("same-source\n", encoding="utf-8")
            for build in (build_a, build_b):
                path = build / "generated" / "contract.hpp"
                path.parent.mkdir()
                path.write_text("same-generated\n", encoding="utf-8")

            rows_a = MODULE.input_rows(
                [source_a / "pkg/backend.cpp", build_a / "generated/contract.hpp"],
                "source",
                source_a,
                build_a,
            )
            rows_b = MODULE.input_rows(
                [source_b / "pkg/backend.cpp", build_b / "generated/contract.hpp"],
                "source",
                source_b,
                build_b,
            )
            self.assertEqual(rows_a, rows_b)

    def test_relevant_input_change_invalidates_digest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source, build = self.roots(Path(temporary), "tree")
            path = source / "backend.cpp"
            path.write_text("before\n", encoding="utf-8")
            before = MODULE.input_rows([path], "source", source, build)
            path.write_text("after\n", encoding="utf-8")
            after = MODULE.input_rows([path], "source", source, build)
            self.assertEqual(before[0]["input_id"], after[0]["input_id"])
            self.assertNotEqual(
                before[0]["content_digest"], after[0]["content_digest"]
            )

    def test_input_outside_declared_roots_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source, build = self.roots(root, "tree")
            outside = root / "outside.cpp"
            outside.write_text("outside\n", encoding="utf-8")
            with self.assertRaises(MODULE.ClosureError):
                MODULE.input_rows([outside], "source", source, build)

    def test_symlinked_input_is_rejected_before_resolution(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source, build = self.roots(Path(temporary), "tree")
            real = source / "backend.cpp"
            real.write_text("source\n", encoding="utf-8")
            alias = source / "backend-alias.cpp"
            alias.symlink_to(real)
            with self.assertRaises(MODULE.ClosureError):
                MODULE.input_rows([alias], "source", source, build)

    def test_generated_metadata_captures_required_compiler_and_abi_fields(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            metadata = build / "CMakeFiles" / "3.30.0"
            metadata.mkdir(parents=True)
            (metadata / "CMakeCXXCompiler.cmake").write_text(
                'set(CMAKE_CXX_COMPILER_ID "GNU")\n'
                'set(CMAKE_CXX_COMPILER_VERSION "14.2.0")\n'
                'set(CMAKE_CXX_SIZEOF_DATA_PTR "8")\n'
                'set(CMAKE_CXX_STANDARD_COMPUTED_DEFAULT "17")\n',
                encoding="utf-8",
            )
            (metadata / "CMakeSystem.cmake").write_text(
                'set(CMAKE_SYSTEM_NAME "Linux")\n'
                'set(CMAKE_SYSTEM_PROCESSOR "x86_64")\n',
                encoding="utf-8",
            )
            values = MODULE.cmake_generated_metadata(build)
            self.assertEqual(values["CMAKE_CXX_COMPILER_ID"], "GNU")
            self.assertEqual(values["CMAKE_CXX_COMPILER_VERSION"], "14.2.0")
            self.assertEqual(values["CMAKE_CXX_SIZEOF_DATA_PTR"], "8")
            self.assertEqual(values["CMAKE_SYSTEM_NAME"], "Linux")
            self.assertEqual(values["CMAKE_SYSTEM_PROCESSOR"], "x86_64")


if __name__ == "__main__":
    unittest.main()
