from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]


def _load(name: str, path: Path):
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


checker = _load(
    "manyforge_error_catalog_checker_test_module",
    REPOSITORY_ROOT / "scripts/ci/check-error-catalog.py",
)
generator = _load(
    "manyforge_error_catalog_generator_test_module",
    REPOSITORY_ROOT / "manyforge_core/tools/generate_error_catalog.py",
)


def test_catalog_covers_every_used_code() -> None:
    missing = set(checker.used_codes()) - set(checker.catalog_codes())
    assert not missing


def test_generated_constants_are_fresh() -> None:
    for path, text in generator.outputs().items():
        assert path.read_text(encoding="utf-8") == text, path


def test_scanner_finds_codes_at_error_construction_sites(tmp_path: Path) -> None:
    package = tmp_path / "manyforge_example" / "src"
    package.mkdir(parents=True)
    (package / "owner.cpp").write_text(
        'throw core::TypedError("example_typed_refused", "no");\n'
        'return Result<void>::Err(ErrorDTO{"example_dto_refused", "no", {}});\n'
        'RetainTerminalFailure(journal, intent, "example_phase_failed", error);\n'
        '_failure("example_private_failure", "no");\n'
        'Require(ok, "a message, not a code");\n',
        encoding="utf-8",
    )
    (package / "owner.py").write_text(
        'raise HeadlessApiError("example_http_refused", "no")\n'
        'self._failure("example_private_refused", "no")\n'
        'unrelated_failure("example_not_a_helper", "no")\n'
        'detail = {"code": "example_detail_refused"}\n',
        encoding="utf-8",
    )
    (package / "view.ts").write_text(
        "if (error.code === 'example_branch_code') {}\n", encoding="utf-8"
    )
    tests = tmp_path / "manyforge_example" / "test"
    tests.mkdir()
    (tests / "test_owner.cpp").write_text(
        'throw core::TypedError("example_test_only", "no");\n', encoding="utf-8"
    )
    assert set(checker.used_codes(tmp_path)) == {
        "example_typed_refused",
        "example_dto_refused",
        "example_phase_failed",
        "example_private_failure",
        "example_private_refused",
        "example_http_refused",
        "example_detail_refused",
        "example_branch_code",
    }


def test_listed_indirect_codes_count_while_their_producer_has_them(tmp_path: Path) -> None:
    # Every listed code is seen in this repository, so it needs its row.
    used = checker.used_codes()
    for codes in checker._INDIRECT_CODES.values():
        assert set(codes) <= set(used)
    relative, codes = next(iter(checker._INDIRECT_CODES.items()))
    producer = tmp_path / relative
    producer.parent.mkdir(parents=True)
    producer.write_text("reason = 'unrelated'\n", encoding="utf-8")
    assert not set(codes) & set(checker.used_codes(tmp_path))
    producer.write_text(f'reason = "{codes[0]}"\n', encoding="utf-8")
    assert codes[0] in checker.used_codes(tmp_path)


def test_catalog_rows_are_exactly_three_columns(tmp_path: Path) -> None:
    good = tmp_path / "good.tsv"
    good.write_text("# header\nexample_code\tExample\tThe full text.\n", encoding="utf-8")
    assert checker.catalog_codes(good) == {"example_code": ("Example", "The full text.")}
    for bad in (
        "example_code\tExample\n",
        "example_code\t\tno compact text\n",
        "Not_A_Code\tExample\t\n",
        "example_code\tA\t\nexample_code\tB\t\n",
    ):
        path = tmp_path / "bad.tsv"
        path.write_text(bad, encoding="utf-8")
        with pytest.raises(SystemExit):
            checker.catalog_codes(path)


def test_generator_refuses_colliding_cpp_names() -> None:
    with pytest.raises(SystemExit):
        generator.render_cpp([("ab_c1", "A", ""), ("ab_c_1", "B", "")])
