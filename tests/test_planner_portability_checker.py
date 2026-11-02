"""Focused bounds and containment tests for the offline portability checker."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from types import ModuleType

import pytest


_SCRIPT = (
    Path(__file__).resolve().parents[1]
    / "scripts"
    / "ci"
    / "check-planner-portability.py"
)


def _load_checker() -> ModuleType:
    name = "manyforge_planner_portability_checker_test_subject"
    spec = importlib.util.spec_from_file_location(name, _SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


CHECKER = _load_checker()


def test_checker_declares_exact_file_and_corpus_bounds() -> None:
    assert CHECKER.MAX_CORPUS_FILES == 4_096
    assert CHECKER.MAX_CORPUS_BYTES == 64 * 1024 * 1024
    assert CHECKER.MAX_FILE_BYTES == 4 * 1024 * 1024
    assert CHECKER.MAX_DOCUMENT_DEPTH > 0
    assert CHECKER.MAX_DOCUMENT_NODES > 0
    assert CHECKER.MAX_SCALAR_CHARS > 0
    assert CHECKER.MAX_FAILURES > 0


def test_mapping_requires_an_exact_top_level_object_shape() -> None:
    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="top-level object",
    ):
        CHECKER._mapping_document([])  # noqa: SLF001

    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="missing fields",
    ):
        CHECKER._mapping_document({"schema_version": "wrong"})  # noqa: SLF001

    unexpected = {field: None for field in CHECKER._TOP_LEVEL_FIELDS}  # noqa: SLF001
    unexpected["extra"] = None
    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="unexpected fields",
    ):
        CHECKER._mapping_document(unexpected)  # noqa: SLF001


def test_structured_parsers_reject_duplicates_recursion_and_nonfinite() -> None:
    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="duplicate key",
    ):
        CHECKER._parse_structured_text(  # noqa: SLF001
            '{"key": 1, "key": 2}',
            suffix=".json",
            label="fixture",
        )

    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="duplicate key",
    ):
        CHECKER._parse_structured_text(  # noqa: SLF001
            "key: 1\nkey: 2\n",
            suffix=".yaml",
            label="fixture",
        )

    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="recursive",
    ):
        CHECKER._parse_structured_text(  # noqa: SLF001
            "root: &root\n  self: *root\n",
            suffix=".yaml",
            label="fixture",
        )

    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="non-finite",
    ):
        CHECKER._parse_structured_text(  # noqa: SLF001
            '{"value": NaN}',
            suffix=".json",
            label="fixture",
        )


def test_structured_value_depth_node_and_scalar_bounds(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(CHECKER, "MAX_DOCUMENT_DEPTH", 2)
    with pytest.raises(CHECKER.PlannerPortabilityBoundsError, match="level bound"):
        CHECKER._validate_structured_value(  # noqa: SLF001
            {"a": {"b": {"c": 1}}},
            label="fixture",
        )

    monkeypatch.setattr(CHECKER, "MAX_DOCUMENT_DEPTH", 64)
    monkeypatch.setattr(CHECKER, "MAX_DOCUMENT_NODES", 3)
    with pytest.raises(CHECKER.PlannerPortabilityBoundsError, match="node bound"):
        CHECKER._validate_structured_value([1, 2, 3], label="fixture")  # noqa: SLF001

    monkeypatch.setattr(CHECKER, "MAX_DOCUMENT_NODES", 100)
    monkeypatch.setattr(CHECKER, "MAX_SCALAR_CHARS", 3)
    with pytest.raises(CHECKER.PlannerPortabilityBoundsError, match="scalar bound"):
        CHECKER._validate_structured_value("four", label="fixture")  # noqa: SLF001

    monkeypatch.setattr(CHECKER, "MAX_SCALAR_CHARS", 100)
    monkeypatch.setattr(CHECKER, "MAX_TOTAL_SCALAR_CHARS", 3)
    with pytest.raises(
        CHECKER.PlannerPortabilityBoundsError,
        match="aggregate scalar bound",
    ):
        CHECKER._validate_structured_value(  # noqa: SLF001
            ["ab", "cd"],
            label="fixture",
        )

    monkeypatch.setattr(CHECKER, "MAX_TOTAL_SCALAR_CHARS", 100)
    monkeypatch.setattr(CHECKER, "MAX_COLLECTION_ITEMS", 1)
    with pytest.raises(CHECKER.PlannerPortabilityBoundsError, match="item bound"):
        CHECKER._validate_structured_value([1, 2], label="fixture")  # noqa: SLF001


def test_structured_value_and_semantic_helpers_reject_wrong_types() -> None:
    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="JSON-compatible",
    ):
        CHECKER._validate_structured_value(("tuple",), label="fixture")  # noqa: SLF001

    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="mapping keys must be strings",
    ):
        CHECKER._validate_structured_value(  # noqa: SLF001
            {1: "value"},
            label="fixture",
        )

    failures: list[str] = []
    assert (
        CHECKER._string_set(failures, {}, location="strings")  # noqa: SLF001
        == set()
    )
    assert CHECKER._object(failures, [], location="object") == {}  # noqa: SLF001
    assert CHECKER._array(failures, {}, location="array") == []  # noqa: SLF001
    assert CHECKER._string_value_mapping(  # noqa: SLF001
        failures,
        [],
        location="string_map",
    ) == {}
    assert len(failures) == 4


def test_failure_collection_is_bounded(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(CHECKER, "MAX_FAILURES", 1)
    failures: list[str] = []
    CHECKER.fail(failures, "first")
    with pytest.raises(CHECKER.PlannerPortabilityBoundsError, match="failures"):
        CHECKER.fail(failures, "second")


def test_corpus_reads_only_contained_regular_non_symlink_files(tmp_path: Path) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    evidence = root / "evidence.cpp"
    evidence.write_text("EvidenceToken", encoding="utf-8")
    corpus = CHECKER.BoundedFileCorpus(root)

    assert corpus.read_text("evidence.cpp", label="evidence") == "EvidenceToken"
    assert corpus.read_text("evidence.cpp", label="evidence") == "EvidenceToken"
    assert corpus.file_count == 1

    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="contained repository-relative",
    ):
        corpus.read_text("../outside.cpp", label="evidence")
    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="NUL",
    ):
        corpus.read_text("bad\x00name.cpp", label="evidence")

    directory = root / "directory"
    directory.mkdir()
    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="regular file",
    ):
        corpus.read_text("directory", label="evidence")

    outside = tmp_path / "outside.cpp"
    outside.write_text("OutsideToken", encoding="utf-8")
    (root / "linked.cpp").symlink_to(outside)
    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="symlink",
    ):
        corpus.read_text("linked.cpp", label="evidence")


def test_corpus_enforces_per_file_total_and_file_count_bounds(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    (root / "first.txt").write_text("1234", encoding="utf-8")
    (root / "second.txt").write_text("5678", encoding="utf-8")

    monkeypatch.setattr(CHECKER, "MAX_FILE_BYTES", 3)
    with pytest.raises(CHECKER.PlannerPortabilityBoundsError, match="per-file"):
        CHECKER.BoundedFileCorpus(root).read_text("first.txt", label="fixture")

    monkeypatch.setattr(CHECKER, "MAX_FILE_BYTES", 8)
    monkeypatch.setattr(CHECKER, "MAX_CORPUS_BYTES", 7)
    corpus = CHECKER.BoundedFileCorpus(root)
    assert corpus.read_text("first.txt", label="fixture") == "1234"
    with pytest.raises(CHECKER.PlannerPortabilityBoundsError, match="corpus"):
        corpus.read_text("second.txt", label="fixture")

    monkeypatch.setattr(CHECKER, "MAX_CORPUS_BYTES", 64)
    monkeypatch.setattr(CHECKER, "MAX_CORPUS_FILES", 1)
    corpus = CHECKER.BoundedFileCorpus(root)
    corpus.read_text("first.txt", label="fixture")
    with pytest.raises(CHECKER.PlannerPortabilityBoundsError, match="file bound"):
        corpus.read_text("second.txt", label="fixture")


def test_duplicate_evidence_requirements_are_rejected(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    (root / "evidence.cpp").write_text("EvidenceToken", encoding="utf-8")
    monkeypatch.setattr(CHECKER, "REQUIRED_EVIDENCE", {"requirement"})
    rows = [
        {
            "requirement": "requirement",
            "reference": "evidence.cpp::EvidenceToken",
        },
        {
            "requirement": "requirement",
            "reference": "missing.cpp::EvidenceToken",
        },
    ]
    failures: list[str] = []

    assert (
        CHECKER._check_evidence(  # noqa: SLF001
            failures,
            rows,
            CHECKER.BoundedFileCorpus(root),
        )
        == 2
    )
    assert any("duplicate evidence requirement" in item for item in failures)
    assert not any("evidence source" in item for item in failures)


def test_evidence_reference_symlinks_and_traversal_fail_closed(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    root = tmp_path / "repo"
    root.mkdir()
    outside = tmp_path / "outside.cpp"
    outside.write_text("EvidenceToken", encoding="utf-8")
    (root / "linked.cpp").symlink_to(outside)
    monkeypatch.setattr(CHECKER, "REQUIRED_EVIDENCE", {"requirement"})

    for reference, expected in (
        ("linked.cpp::EvidenceToken", "symlink"),
        ("../outside.cpp::EvidenceToken", "contained repository-relative"),
    ):
        failures: list[str] = []
        CHECKER._check_evidence(  # noqa: SLF001
            failures,
            [{"requirement": "requirement", "reference": reference}],
            CHECKER.BoundedFileCorpus(root),
        )
        assert any(expected in item for item in failures)


def test_run_check_rejects_a_symlinked_mapping(tmp_path: Path) -> None:
    root = tmp_path / "repo"
    mapping_parent = root / "verification/planner_portability"
    mapping_parent.mkdir(parents=True)
    outside = tmp_path / "mapping.json"
    outside.write_text("{}", encoding="utf-8")
    mapping = mapping_parent / "mapping.json"
    mapping.symlink_to(outside)

    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="symlink",
    ):
        CHECKER._run_check(root, mapping)  # noqa: SLF001


def test_artifact_traversal_requires_every_declared_root(tmp_path: Path) -> None:
    root = tmp_path / "repo"
    root.mkdir()

    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="required artifact root is missing",
    ):
        CHECKER._collect_program_artifacts(root)  # noqa: SLF001


def test_artifact_traversal_rejects_symlinks_and_file_overflow(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    root = tmp_path / "repo"
    examples = root / "examples"
    examples.mkdir(parents=True)
    outside = tmp_path / "outside.yaml"
    outside.write_text("name: outside\n", encoding="utf-8")
    (examples / "linked.yaml").symlink_to(outside)
    with pytest.raises(
        CHECKER.PlannerPortabilityInputError,
        match="refuses symlink",
    ):
        CHECKER._collect_program_artifacts(root)  # noqa: SLF001

    (examples / "linked.yaml").unlink()
    (examples / "one.yaml").write_text("name: one\n", encoding="utf-8")
    (examples / "two.yaml").write_text("name: two\n", encoding="utf-8")
    monkeypatch.setattr(CHECKER, "MAX_CORPUS_FILES", 1)
    with pytest.raises(CHECKER.PlannerPortabilityBoundsError, match="file bound"):
        CHECKER._collect_program_artifacts(root)  # noqa: SLF001


def test_main_fails_cleanly_on_a_non_object_mapping(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    root = tmp_path / "repo"
    mapping = root / "verification/planner_portability/mapping.json"
    mapping.parent.mkdir(parents=True)
    mapping.write_text("[]", encoding="utf-8")
    monkeypatch.setattr(CHECKER, "ROOT", root)
    monkeypatch.setattr(CHECKER, "MAPPING", mapping)

    assert CHECKER.main() == 1
    assert "top-level object" in capsys.readouterr().err
