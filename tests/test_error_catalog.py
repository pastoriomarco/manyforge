from __future__ import annotations

import ast
import importlib.util
import re
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
        'Require(ok, "a message, not a code");\n'
        'return rejected<Result>("example_template_rejected");\n'
        'return state_view_error("example_cpp_helper_refused", "no");\n',
        encoding="utf-8",
    )
    (package / "owner.py").write_text(
        'raise HeadlessApiError("example_http_refused", "no")\n'
        'self._failure("example_private_refused", "no")\n'
        'unrelated_failure("example_not_a_helper", "no")\n'
        '_error("example_private_error", "no")\n'
        '_LOGGER.error("example_log_tag", extra=report)\n'
        'record(reason_code="example_reason_code")\n'
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
        "example_template_rejected",
        "example_cpp_helper_refused",
        "example_private_error",
        "example_reason_code",
        "example_private_refused",
        "example_http_refused",
        "example_detail_refused",
        "example_branch_code",
    }


def test_listed_indirect_codes_count_while_their_producer_has_them(tmp_path: Path) -> None:
    # Every listed code is seen in this repository, so it needs its row.
    used = checker.used_codes()
    for producer, codes in checker._INDIRECT_CODES.items():
        if (REPOSITORY_ROOT / producer).is_file():
            assert set(codes) <= set(used), producer
    relative, codes = next(iter(checker._INDIRECT_CODES.items()))
    producer = tmp_path / relative
    producer.parent.mkdir(parents=True)
    producer.write_text("reason = 'unrelated'\n", encoding="utf-8")
    assert not set(codes) & set(checker.used_codes(tmp_path))
    producer.write_text(f'reason = "{codes[0]}"\n', encoding="utf-8")
    assert codes[0] in checker.used_codes(tmp_path)


def _write(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def test_message_first_helpers_count_only_their_listed_argument(tmp_path: Path) -> None:
    kernel = "manyforge_core/src/resource_addressed_kernel_operations.cpp"
    assert checker._MESSAGE_FIRST[kernel]["reject"] == 1
    source = (
        'return reject<OperationAcceptedDTO>("the component is busy",\n'
        '                                    "example_kernel_reason");\n'
        'return reject<std::unique_ptr<Owner>>("nested template",\n'
        '    "example_nested_reason", ErrorCode::kInvalidRequest);\n'
        'return error("example_dto_code", "message " + detail,\n'
        '             ik ? "example_ik_reason" : "example_plan_reason");\n'
        'return reject<void>("message", detail, "example_third_argument");\n'
    )
    _write(tmp_path, kernel, source)
    # The same calls in a file that is not listed are invisible.
    _write(tmp_path, "manyforge_core/src/unlisted_owner.cpp", source.replace("example_", "other_"))
    used = set(checker.used_codes(tmp_path))
    assert {
        "example_kernel_reason",
        "example_nested_reason",
        "example_dto_code",
        "example_ik_reason",
        "example_plan_reason",
    } <= used
    assert "example_third_argument" not in used
    assert not any(code.startswith("other_") for code in used - {"other_dto_code"})


def test_message_first_helper_skips_detail_maps_and_messages(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "manyforge_roboplan/src/cartesian_planning_contract.cpp",
        'return fail("Cartesian samples are malformed", "example_cartesian_reason",\n'
        '            {{"sample_index", std::to_string(sample)}});\n'
        'return fail("a message", "Not a code");\n',
    )
    assert set(checker.used_codes(tmp_path)) == {"example_cartesian_reason"}


def test_cpp_reason_members_count_only_as_closed_pairs(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "manyforge_example/src/owner.cpp",
        'return ErrorDTO{code, "message", {{"reason", "example_detail_reason"}}};\n'
        'details.insert({"reason_code", std::string("example_detail_code")});\n'
        'shape(v, {"reason", "example_field_name", "other_field"});\n'
        'state.reason = "example_status_reason";\n',
    )
    assert set(checker.used_codes(tmp_path)) == {
        "example_detail_reason",
        "example_detail_code",
    }


def test_reason_assignments_count_only_for_listed_variables(tmp_path: Path) -> None:
    relative = "manyforge_core/src/ordered_contact_validation.cpp"
    assert "*error_reason" in checker._REASON_VARS[relative]
    source = (
        '*error_reason = "example_error_reason";\n'
        '*error_reason =\n    "example_wrapped_reason";\n'
        'summary.invalid_reason = "example_summary_reason";\n'
        'other.invalid_reason = "example_other_reason";\n'
        'if (*error_reason == "example_compared_reason") {}\n'
    )
    _write(tmp_path, relative, source)
    _write(tmp_path, "manyforge_core/src/resource_addressed_kernel_status.cpp",
           'assessment.reason = "example_status_reason";\n')
    assert set(checker.used_codes(tmp_path)) == {
        "example_error_reason",
        "example_wrapped_reason",
        "example_summary_reason",
    }


def test_python_reasons_count_only_in_listed_files_and_forms(tmp_path: Path) -> None:
    assert checker._PY_REASONS["manyforge_operator/isaac.py"] == ("assignment",)
    source = (
        'reason = "example_assigned_reason"\n'
        'stop(reason="example_keyword_reason")\n'
        'print({"reason": "example_member_reason"})\n'
        'release_reason = "example_other_variable"\n'
    )
    _write(tmp_path, "manyforge_operator/isaac.py", source)
    _write(tmp_path, "manyforge_assistant_bridge/bridge.py",
           source.replace("example_", "bridge_"))
    _write(tmp_path, "manyforge_example/unlisted.py", source.replace("example_", "unlisted_"))
    assert set(checker.used_codes(tmp_path)) == {
        "example_assigned_reason",
        "example_keyword_reason",
        "bridge_member_reason",
    }


def test_python_constants_count_when_listed_or_named_code(tmp_path: Path) -> None:
    relative = "manyforge_program_runner/persistent_keys.py"
    assert "REASON_NOT_DECLARED" in checker._PY_CONSTANTS[relative]
    source = (
        'COMMAND: Final = "example_command_name"\n'
        'REASON_NOT_DECLARED: Final = "example_not_declared"\n'
        'FAULT_CODE: Final = "example_publication_failed"\n'
        '_MALFORMED_CODE = "example_malformed"\n'
        'LOG_CODE: Final = "example_log_tag"\n'
    )
    _write(tmp_path, relative, source)
    _write(tmp_path, "manyforge_example/unlisted.py", source.replace("example_", "unlisted_"))
    assert set(checker.used_codes(tmp_path)) == {
        "example_not_declared",
        "example_publication_failed",
        "example_malformed",
        "unlisted_publication_failed",
        "unlisted_malformed",
    }


def test_cpp_code_parameters_conditionals_and_returned_codes(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "manyforge_example/src/owner.cpp",
        'void Require(bool condition, const std::string &code, std::string message);\n'
        'void Note(bool condition, const std::string &detail);\n'
        'Require(ok, "example_required", "message");\n'
        'Require(ok, busy ? "example_busy" : "example_idle", "message");\n'
        'Require(Text(v, "field_name") == "other_value", "example_compared", "m");\n'
        'Note(ok, "example_detail_not_a_code");\n'
        'return TypedError(a == B::kC ? "example_then" : x ? "example_else_then"\n'
        '                  : other_code, "message");\n'
        'return Result<void>::Err({"example_braced", "message", {}});\n'
        'std::optional<std::string_view> example_field_error(const T& v) noexcept {\n'
        '  if (!v.ok) return "example_returned";\n'
        '  return std::nullopt;\n'
        '}\n'
        'std::string_view describe(const T& v) { return "example_not_returned_code"; }\n',
    )
    _write(tmp_path, "manyforge_example/include/manyforge_example/alarm.hpp",
           'void MarkUnavailable(std::string code, std::string message);\n')
    _write(tmp_path, "manyforge_example/src/alarm.cpp",
           'MarkUnavailable("example_header_helper", "message");\n')
    assert set(checker.used_codes(tmp_path)) == {
        "example_required",
        "example_busy",
        "example_idle",
        "example_compared",
        "example_then",
        "example_else_then",
        "example_braced",
        "example_returned",
        "example_header_helper",
    }


def test_python_conditional_default_and_helper_codes(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "manyforge_example/owner.py",
        'def _refuse(entry, code, message): ...\n'
        'class Refused(Exception):\n'
        '    code: Final = "example_class_code"\n'
        '_refuse(entry, "example_helper_code", "message")\n'
        'raise HeadlessApiError("example_then" if busy else "example_else", "m")\n'
        'record(reason_code=None if ok else "example_keyword")\n'
        'detail = {"code": reason or "example_default"}\n'
        'code = getattr(error, "code", None) or "example_assigned"\n'
        'other("example_not_a_code" if busy else "example_still_not")\n',
    )
    assert set(checker.used_codes(tmp_path)) == {
        "example_class_code",
        "example_helper_code",
        "example_then",
        "example_else",
        "example_keyword",
        "example_default",
        "example_assigned",
    }


def test_runtime_families_count_while_their_template_is_present(tmp_path: Path) -> None:
    used = checker.used_codes()
    for producer, families in checker._RUNTIME_FAMILIES.items():
        if (REPOSITORY_ROOT / producer).is_file():
            for template, codes in families.items():
                assert set(codes) <= set(used), template
    relative = "manyforge_program_runner/runtime.py"
    template = 'f"program_cycle_state_{cycle.state}"'
    _write(tmp_path, relative, f"reason = {template}\n")
    assert "program_cycle_state_running" in checker.used_codes(tmp_path)
    _write(tmp_path, relative, "reason = None\n")
    assert "program_cycle_state_running" not in checker.used_codes(tmp_path)


def _closed_set(relative: str, name: str) -> set[str]:
    """String values of a module-level set/dict constant or an Enum class."""
    tree = ast.parse((REPOSITORY_ROOT / relative).read_text(encoding="utf-8"))
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == name:
            return {
                item.value.value
                for item in node.body
                if isinstance(item, ast.Assign) and isinstance(item.value, ast.Constant)
            }
        target = getattr(node, "target", None) or (getattr(node, "targets", None) or [None])[0]
        if isinstance(node, (ast.Assign, ast.AnnAssign)) and getattr(target, "id", None) == name:
            value = node.value
            if isinstance(value, ast.Dict):
                return {v.value for v in value.values}
            return {
                item.value
                for item in ast.walk(value)
                if isinstance(item, ast.Constant) and isinstance(item.value, str)
            }
    raise AssertionError(f"{name} not found in {relative}")


def test_runtime_family_sets_match_their_sources() -> None:
    assert set(checker._PROGRAM_CYCLE_STATES) == _closed_set(
        "manyforge_program_runner/cycle_journal.py", "STATES")
    assert set(checker._PROGRAM_RUN_STATES) == _closed_set(
        "manyforge_behavior/manyforge_behavior/program_run_owner.py", "ProgramRunState")
    assert set(checker._PREEMPTION_OUTCOMES) == _closed_set(
        "manyforge_behavior/manyforge_behavior/nodes/v2_owner.py", "_PREEMPTION_OUTCOMES")
    ports = (REPOSITORY_ROOT / "manyforge_behavior/manyforge_behavior/orchestration_ports.py"
             ).read_text(encoding="utf-8")
    execution_states = _closed_set(
        "manyforge_behavior/manyforge_behavior/kernel_client.py", "ExecutionState")
    for name, states in (
        ("INTERRUPTED_EXECUTION_STATES", checker._INTERRUPTED_EXECUTION_STATES),
        ("FAILED_EXECUTION_STATES", checker._FAILED_EXECUTION_STATES),
    ):
        body = ports.split(f"{name}: Final = frozenset(", 1)[1].split(")\n", 1)[0]
        members = {m.lower() for m in re.findall(r"ExecutionState\.(\w+)\.value", body)}
        assert set(states) == members and members <= execution_states, name


def test_vocabulary_entries_count_only_when_referenced(tmp_path: Path) -> None:
    cpp, python = checker._CODE_VOCABULARIES
    _write(
        tmp_path,
        cpp,
        'inline constexpr char kExampleReferenced[] = "example_referenced";\n'
        'inline constexpr char kExampleWrapped[] =\n    "example_wrapped";\n'
        'inline constexpr char kExampleQuoted[] = "example_quoted";\n'
        'inline constexpr char kExampleDead[] = "example_dead";\n',
    )
    _write(
        tmp_path,
        python,
        'class ReasonCode(StrEnum):\n'
        '    EXAMPLE_PYTHON = "example_python"\n'
        '    EXAMPLE_PYTHON_DEAD = (\n        "example_python_dead"\n    )\n',
    )
    _write(
        tmp_path,
        "manyforge_example/src/owner.cpp",
        "use(reason_codes::kExampleReferenced, reason_codes::kExampleWrapped);\n"
        'compare(reason, "example_quoted");\n',
    )
    _write(tmp_path, "manyforge_example/owner.py", "use(ReasonCode.EXAMPLE_PYTHON)\n")
    # Generated catalog constants and tests never prove an entry used.
    _write(tmp_path, checker._GENERATED[0], 'constexpr char kExampleDead[] = "example_dead";\n')
    _write(tmp_path, "manyforge_example/test/test_owner.py",
           "use(ReasonCode.EXAMPLE_PYTHON_DEAD)\n")
    assert set(checker.used_codes(tmp_path)) == {
        "example_referenced",
        "example_wrapped",
        "example_quoted",
        "example_python",
    }


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
