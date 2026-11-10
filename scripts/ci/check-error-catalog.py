#!/usr/bin/env python3
"""Refuse any error code the program uses that the error catalog lacks.

The catalog is `manyforge_core/resources/error_catalog.tsv`: one line per error,
three tab-separated columns `code`, `compact text`, `full text`. The area is the
code prefix; there is no severity column.

A code is "used" when a snake_case string literal (two or more segments) appears
at one of these error-construction sites in non-test sources:

- C++: the first literal argument of a call (template arguments allowed)
  whose name ends in `Error`, `Failure`, `Refusal`, `Rejection` or
  `Exception`, or is one of the helpers in `_CALLS` or `_CPP_CALLS`; an
  `ErrorDTO{...}` initializer; a `kError*`/`k*ReasonCode` constant; a
  `{"code", ...}` canonical member; a closed `{"reason", "..."}` or
  `{"reason_code", "..."}` pair. Per file: the listed argument of a
  `_MESSAGE_FIRST` helper (`reject<T>("message", "code")`) and a literal
  assigned to a `_REASON_VARS` variable.
- Python: the first literal argument of a call whose name ends in `Error`,
  `Failure`, `Refusal`, `Rejection` or `Exception`, or a `_CALLS` helper with
  an optional leading underscore, or a `_PY_CALLS` name; a `"code": "..."`,
  `"reason_code": "..."` or `"error_code": "..."` member; a `code=`,
  `reason_code=` or `error_code=` keyword or assignment. Per file: the
  `_PY_REASONS` forms and the `_PY_CONSTANTS` constants.
- A `_CODE_VOCABULARIES` entry whose constant or quoted code a non-test,
  non-generated source references.
- TypeScript: a code compared in a branch (`code === '...'`, `code !== '...'`).

Codes built at runtime (for example `section + "_source_absent"`) are invisible
to this check by construction. A few operator-facing codes are produced where no
pattern can see them (a reason assignment, a projection reason, a reason-code
constant); `_INDIRECT_CODES` names each with its producer file, and the code
counts as used while that file still contains it. `--list` prints every used code; `--seed` prints
the missing ones as catalog rows with a humanized compact text and an empty
full text, for a human to complete.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "manyforge_core/resources/error_catalog.tsv"

_CODE = r"[a-z][a-z0-9]*(?:_[a-z0-9]+)+"
_ERROR_NAME = r"[A-Za-z_]\w*(?:Error|Failure|Refusal|Rejection|Exception)"
# Helpers whose literal argument is an error code (C++ and Python).
_CALLS = (
    "fail",
    "invalid",
    "reject",
    "refuse",
    "refused",
    "failure",
    "coordinator_error",
    "stage_error",
    "ErrorResponse",
    "DetailedErrorResponse",
    "ApplyErrorResponse",
    "ApplyKnownNoEffectErrorResponse",
    "ApplyErrorResponseFromJournalEvidence",
    "RetainTerminalFailure",
    "retain_pre_effect_failure",
    "TypedError",
    "RequestInvalid",
)
# Python-only helpers and error classes, matched by exact name (a bare `error`
# would match logger calls).
_PY_CALLS = (
    "_error",
    "_fault_class",
    "_diagnostic",
    "_reason",
    "_trace_unavailable",
    "_record_node_failure",
    "_record_owner_failure",
    "_record_owner_error",
    "mark_malformed",
    "ApplyRefused",
    "ApplyStepFailed",
    "_NodeRefused",
)
# C++-only helpers whose literal argument is an error code, with an optional
# leading underscore.
_CPP_CALLS = (
    "error",
    "Error",
    "err",
    "err_structured",
    "rejected",
    "Reject",
    "Rejected",
    "reject_owned",
    "reject_state",
    "RejectSupportContact",
    "failed",
    "Fail",
    "CoreFail",
    "AlarmFail",
    "StoreFail",
    "HistorianFail",
    "FailNative",
    "fail_physical",
    "failure_profile",
    "unknown",
    "stale",
    "not_sent",
    "Invalid",
    "InvalidConfiguration",
    "invalid_profile",
    "invalid_publication",
    "mark_invalid",
    "ErrorValue",
    "PublishUnavailable",
    "source_unavailable",
    "RefusedBeforeEffect",
    "LatchAndThrow",
    "emit",
    "bb_error",
    "channel_error",
    "digest_input_error",
    "global_rule_error",
    "gripper_runtime_error",
    "projection_error",
    "rule_error",
    "snapshot_error",
    "source_error",
    "state_error",
    "state_view_error",
    "stop_error",
    "stop_acceptance_input_error",
    "switch_error",
    "terminal_input_error",
    "ambiguous_resolution_error",
    "candidate_error",
    "planning_error",
    "readiness_error",
    "resolution_error",
    "route_publication_error",
    "AuthoringNotReady",
    "carrier_frame_unresolved",
    "invalid_value",
    "measured_source_unavailable",
)


# C++ template arguments, one nesting level (`reject<std::unique_ptr<T>>(`).
_TEMPLATE = r"(?:<(?:[^;(){}<>]|<[^;(){}<>]*>)*>\s*)?"


def _call(names: str) -> re.Pattern[str]:
    # The literal may follow C++ template arguments and up to three non-literal
    # arguments (for example `RetainTerminalFailure(journal, intent, "code", ...)`).
    return re.compile(
        r"\b(?:" + names + r")\s*" + _TEMPLATE + r"\(\s*"
        r"(?:[A-Za-z_][\w:.\->*&]*\s*,\s*){0,3}"
        r"(?:std::string\s*\(\s*)?[\"'](" + _CODE + r")[\"']"
    )


_CALL = _call(_ERROR_NAME + r"|_?(?:" + "|".join(_CALLS + ("unavailable",)) + r")")
_PATTERNS = {
    ".cpp": (
        _CALL,
        _call(r"_?(?:" + "|".join(_CPP_CALLS) + r")"),
        re.compile(r"ErrorDTO\s*\{\s*(?:std::string\s*\(\s*)?\"(" + _CODE + r")\""),
        re.compile(r"\bk\w*(?:Error|ReasonCode|Code)\w*\s*=\s*\"(" + _CODE + r")\""),
        re.compile(r"\{\s*\"code\"\s*,\s*(?:CanonicalValue\s*\(\s*)?\"(" + _CODE + r")\""),
    ),
    ".py": (
        _CALL,
        _call("|".join(_PY_CALLS)),
        re.compile(r"[\"'](?:reason_|error_)?code[\"']\s*:\s*[\"'](" + _CODE + r")[\"']"),
        re.compile(r"\b(?:reason_|error_)?code\s*=\s*[\"'](" + _CODE + r")[\"']"),
    ),
    ".ts": (
        re.compile(r"\b[cC]ode\s*[!=]==\s*'(" + _CODE + r")'"),
        re.compile(r"'(" + _CODE + r")'\s*[!=]==\s*[\w.?]*[cC]ode\b"),
    ),
}
_PATTERNS[".hpp"] = _PATTERNS[".cpp"]
# C++ `{"reason", "..."}` / `{"reason_code", "..."}` pair in an ErrorDTO or
# detail initializer; the closing brace keeps out field lists such as
# `shape(v, {"reason", "last_accepted_source_sequence", ...})`.
_CPP_REASON_MEMBER = re.compile(
    r"\{\s*\"reason(?:_code)?\"\s*,\s*(?:std::string\s*\(\s*)?\"(" + _CODE + r")\"\s*\)?\s*\}"
)
# File -> {helper: zero-based argument index} for local C++ helpers whose code
# is not the first literal argument (for example `reject<T>("message", "code")`
# or `error(code, "message", "reason")`). The argument counts when it is a
# literal or a conditional between two literals.
_RESOURCE_KERNEL = {"reject": 1, "refuse": 1, "error": 2}
_MESSAGE_FIRST = {
    **{
        f"manyforge_core/src/{name}": _RESOURCE_KERNEL
        for name in (
            "resource_addressed_execution_kernel.cpp",
            "resource_addressed_execution_kernel_internal.hpp",
            "resource_addressed_kernel_admission.cpp",
            "resource_addressed_kernel_cell_latch.cpp",
            "resource_addressed_kernel_events.cpp",
            "resource_addressed_kernel_global_io_rules.cpp",
            "resource_addressed_kernel_immediate_step.cpp",
            "resource_addressed_kernel_io_rules.cpp",
            "resource_addressed_kernel_operations.cpp",
            "resource_addressed_kernel_program_blackboard.cpp",
            "resource_addressed_kernel_runtime.cpp",
            "resource_addressed_kernel_status.cpp",
            "resource_addressed_kernel_stop.cpp",
            "resource_addressed_kernel_takeover.cpp",
            "resource_addressed_kernel_takeover_fallback.cpp",
        )
    },
    "manyforge_core/src/resource_addressed_kernel_io_rules.cpp": {
        **_RESOURCE_KERNEL, "rule_error": 1},
    "manyforge_core/src/resource_addressed_kernel_operations.cpp": {
        **_RESOURCE_KERNEL, "mark_failed": 2},
    "manyforge_core/src/collision_validation_coordinator.cpp": {"record_violation": 3},
    "manyforge_core/src/execution_kernel.cpp": {
        "reject": 2,
        "settle_local_failure": 5,
        "finalize_prepared_motion_cancellation_if_requested": 4,
    },
    "manyforge_core/src/kernel_cell_admission_owner.cpp": {"Reject": 1},
    "manyforge_core/src/motion_target_resolution.cpp": {"invalid_target_shape": 1},
    "manyforge_core/src/operation_intent.cpp": {"invalid": 1},
    "manyforge_core/src/ordered_contact_validation.cpp": {"make_release_violation": 3},
    "manyforge_core/src/parallel_collision_validation_worker_pool.cpp": {"exception_bundle": 1},
    "manyforge_core/src/resource_slot_table.cpp": {
        "reject": 1, "reject_value": 1, "refuse": 1},
    "manyforge_core/src/runtime_contract_manifest_codec.cpp": {
        "reject": 2, "canonical_failure": 1},
    "manyforge_core/src/runtime_participant_binding.cpp": {"invalid": 1},
    "manyforge_core/src/runtime_participant_policy.cpp": {"invalid": 1},
    "manyforge_core/src/runtime_participant_registry.cpp": {"reject": 1},
    "manyforge_collision/src/manyforge_collision_engine.cpp": {"refusal": 1},
    "manyforge_component_manager/src/manager_runtime_convergence_coordinator.cpp": {
        "reject": 2},
    "manyforge_perception_provider/src/provider_model.cpp": {"Finish": 1},
    "manyforge_roboplan/src/cartesian_planning_contract.cpp": {
        "fail": 1, "cartesian_incomplete_error": 1, "set_path_parameterization_error": 1},
    "manyforge_roboplan/src/managed_projection_registry.cpp": {"WorkerFailure": 2},
    "manyforge_roboplan/src/owner_settlement_readers.cpp": {"Rejected": 1},
    "manyforge_roboplan/src/process_contained_candidate_client.cpp": {"error": 2},
    "manyforge_roboplan/src/product_backed_runtime_participant_resolver.cpp": {"Reject": 1},
    "manyforge_roboplan/src/retained_focused_adapters.cpp": {"adapter_error": 2},
    "manyforge_roboplan/src/retained_planner_scene_projection.cpp": {
        "projection_error": 1, "invalid": 1, "error": 2},
    "manyforge_roboplan/src/roboplan_candidate_computer.cpp": {"fail": 2},
    "manyforge_roboplan/src/roboplan_planner_backend.cpp": {"error": 2},
    "manyforge_roboplan/src/sealed_program_execution_capture.cpp": {"CaptureError": 1},
    "manyforge_xarm_control/src/xarm_arm_adapter.cpp": {"unconfirmed": 1},
}
# File -> C++ reason variables whose literal assignments are codes
# (`result.reason = "..."`). Status reasons (resource_addressed_kernel_status.cpp,
# scene_runtime_state_owner.cpp) are deliberately absent.
_REASON_VARS = {
    "manyforge_core/src/ordered_contact_validation.cpp": (
        "*error_reason", "summary.invalid_reason"),
    "manyforge_core/src/runtime_collision_lookahead_session.cpp": (
        "result.reason", "error_reason", "out.error_reason"),
    "manyforge_roboplan/src/cartesian_planning_contract.cpp": ("reason",),
    "manyforge_xarm_control/src/xarm_driver_services.cpp": ("result.reason",),
}
# File -> Python reason forms that reach an operator, API client or the
# assistant: "assignment" (`reason = "..."`, `reason="..."`) and/or "member"
# (`"reason": "..."`).
_PY_REASONS = {
    "manyforge_assistant_bridge/bridge.py": ("member",),
    "manyforge_behavior/manyforge_behavior/production_ports.py": ("member",),
    "manyforge_behavior/manyforge_behavior/program_run_owner.py": ("assignment",),
    "manyforge_headless_api/control_state.py": ("assignment",),
    "manyforge_operator/isaac.py": ("assignment",),
    "manyforge_program_runner/runtime.py": ("assignment", "member"),
}
_PY_REASON_FORMS = {
    "assignment": re.compile(r"(?<![\w.])reason\s*=\s*[\"'](" + _CODE + r")[\"']"),
    "member": re.compile(r"[\"']reason[\"']\s*:\s*[\"'](" + _CODE + r")[\"']"),
}
# File -> Python upper-case constants that hold codes.
_PY_CONSTANTS = {
    "manyforge_assistant_bridge/recovery.py": ("RECOVERY_CODE",),
    "manyforge_behavior/manyforge_behavior/nodes/interruption.py": (
        "NODE_INTERRUPTED_REASON",),
    "manyforge_composer/backend/state.py": ("RUNTIME_UNAVAILABLE_REASON_CODE",),
    "manyforge_program_runner/bbkey_publication.py": ("FAULT_CODE",),
    "manyforge_program_runner/liveness.py": ("PUBLISHER_FAULT_CODE",),
    "manyforge_program_runner/persistent_keys.py": (
        "REASON_NOT_DECLARED",
        "REASON_DECLARATION_CHANGED",
        "REASON_VALUE_INVALID",
        "FAULT_APPLY_NOT_OBSERVED",
    ),
}
# Producer file -> codes it produces at a site no pattern above can see.
_INDIRECT_CODES = {
    "manyforge_behavior/manyforge_behavior/program_run_owner.py": (
        "selected_motion_stop_settling",
    ),
    "manyforge_behavior/manyforge_behavior/orchestration_values.py": (
        "node_input_unavailable",
        "motion_value_above_program_limit",
    ),
    "manyforge_core/include/manyforge_core/types/carried_object_lost.hpp": (
        "carried_object_lost",
    ),
    # The other arm of a conditional whose first arm a pattern sees.
    "manyforge_behavior/manyforge_behavior/assistant_program_port.py": (
        "model_request_failed",
    ),
    "manyforge_core/src/execution_collision_support.cpp": (
        "grasp_contact_before_terminal_window",
    ),
    "manyforge_core/src/resource_addressed_kernel_events.cpp": (
        "motion_admission_closed",
    ),
    "manyforge_core/src/resource_addressed_kernel_io_rules.cpp": (
        "motion_interrupt_signal_trip",
    ),
    "manyforge_core/src/resource_addressed_kernel_status.cpp": (
        "operation_recovery_invalid",
    ),
    "manyforge_core/src/sqlite_operation_journal.cpp": ("operation_journal_busy",),
    "manyforge_deployment/src/io_signal_catalog.cpp": (
        "ai_model_configuration_invalid",
    ),
    "manyforge_deployment/src/program_reference_validator.cpp": (
        "collision_body_missing",
    ),
    "manyforge_headless/src/sqlite_event_history_store.cpp": (
        "historian_storage_busy",
    ),
    "manyforge_headless_api/control_state.py": ("source_recovered",),
    "manyforge_roboplan/src/geometric_contact_input_provider.cpp": (
        "contact_bodies_static",
    ),
    "manyforge_roboplan/src/retained_gripper_route_publication_owner.cpp": (
        "route_predecessor_gripper_rollback_failed",
    ),
    "manyforge_roboplan/src/retained_kinematic_state_projector.cpp": (
        "retained_state_projection_failed",
    ),
    "manyforge_roboplan/src/retained_ros2_control_runtime_handle.cpp": (
        "ros2_control_hardware_state_timeout",
    ),
    "manyforge_roboplan/src/ros_boolean_output_provider.cpp": (
        "io_output_acceptance_unknown",
    ),
    "manyforge_scene/src/logical_scene_owner_store.cpp": (
        "active_bundle_publication_not_current",
    ),
}
# Reason-code vocabularies: a listed code counts as used only while a non-test
# source references its constant (`reason_codes::kName`, `ReasonCode.NAME`) or
# quotes the code itself outside the vocabularies.
_CODE_VOCABULARIES = {
    "manyforge_core/include/manyforge_core/types/reason_codes.hpp": re.compile(
        r"\b(k\w+)\s*\[\]\s*=\s*\"(" + _CODE + r")\""),
    "manyforge_behavior/manyforge_behavior/reason_codes.py": re.compile(
        r"^\s*([A-Z][A-Z0-9_]*)\s*=\s*\(?\s*\"(" + _CODE + r")\"", re.M),
}
# Files generated from the catalog: they quote every code, so they never prove
# a vocabulary entry used.
_GENERATED = (
    "manyforge_core/include/manyforge_core/types/error_catalog.generated.hpp",
    "manyforge_headless_api/error_codes.py",
    "manyforge_composer/frontend/src/api/errorCatalog.generated.ts",
)
_VOCABULARY_REFERENCE = re.compile(r"\b(?:reason_codes::|ReasonCode\.)(\w+)")
_QUOTED = re.compile(r"[\"'](" + _CODE + r")[\"']")
_PATTERNS[".tsx"] = _PATTERNS[".ts"]

_SKIP_PARTS = {
    "node_modules",
    "build",
    "install",
    "log",
    "third_party",
    "test",
    "tests",
    "process_tests",
    "__pycache__",
    ".manyforge-dev",
    "qualification",
    "verification",
    "examples",
}


def _is_test(path: Path) -> bool:
    name = path.name
    return (
        name.startswith("test_")
        or name.endswith(("_test.py", ".test.ts", ".test.tsx", "_test.cpp"))
        or name == "conftest.py"
    )


def source_files(root: Path = ROOT):
    for suffix in _PATTERNS:
        for path in root.rglob(f"*{suffix}"):
            relative = path.relative_to(root)
            if _SKIP_PARTS.intersection(relative.parts) or _is_test(path):
                continue
            if relative.parts[0] in {"scripts", "docs", "tools"}:
                continue
            yield path


def _arguments(text: str, start: int) -> list[str]:
    """The top-level arguments of the call whose `(` ends just before `start`."""
    arguments: list[str] = []
    depth = 0
    begin = index = start
    while index < len(text):
        character = text[index]
        if character in "\"'":
            index += 1
            while index < len(text) and text[index] != character:
                index += 2 if text[index] == "\\" else 1
        elif character in "([{":
            depth += 1
        elif character in ")]}":
            if depth == 0:
                arguments.append(text[begin:index])
                return arguments
            depth -= 1
        elif character == "," and depth == 0:
            arguments.append(text[begin:index])
            begin = index + 1
        elif character == ";":
            break
        index += 1
    return arguments


_LITERAL = r"(?:std::string\s*\(\s*)?\"(" + _CODE + r")\"\s*\)?"
_ARGUMENT_FORMS = (
    re.compile(r"\s*" + _LITERAL + r"\s*"),
    re.compile(r"[^\"]*\?\s*" + _LITERAL + r"\s*:\s*" + _LITERAL + r"\s*"),
)


def _message_first_codes(text: str, helpers: dict[str, int]):
    for helper, position in helpers.items():
        call = re.compile(r"\b" + helper + r"\s*" + _TEMPLATE + r"\(")
        for match in call.finditer(text):
            arguments = _arguments(text, match.end())
            if position >= len(arguments):
                continue
            for form in _ARGUMENT_FORMS:
                literal = form.fullmatch(arguments[position])
                if literal:
                    yield from (code for code in literal.groups() if code)
                    break


def _file_codes(relative: str, suffix: str, text: str):
    """Codes at the error-construction sites of one non-test source file."""
    for pattern in _PATTERNS[suffix]:
        for match in pattern.finditer(text):
            yield match.group(1)
    if suffix in {".cpp", ".hpp"}:
        yield from (match.group(1) for match in _CPP_REASON_MEMBER.finditer(text))
        yield from _message_first_codes(text, _MESSAGE_FIRST.get(relative, {}))
        for name in _REASON_VARS.get(relative, ()):
            assignment = re.compile(
                r"(?<![\w.>])" + re.escape(name) + r"\s*=\s*\"(" + _CODE + r")\"")
            yield from (match.group(1) for match in assignment.finditer(text))
    if suffix == ".py":
        for form in _PY_REASONS.get(relative, ()):
            yield from (match.group(1) for match in _PY_REASON_FORMS[form].finditer(text))
        for name in _PY_CONSTANTS.get(relative, ()):
            constant = re.compile(
                r"^\s*" + name + r"\s*(?::[^=\n]+)?=\s*[\"'](" + _CODE + r")[\"']", re.M)
            yield from (match.group(1) for match in constant.finditer(text))


def used_codes(root: Path = ROOT) -> dict[str, str]:
    """Every used code with the first file that uses it."""
    found: dict[str, str] = {}
    quoted: set[str] = set()
    references: set[str] = set()
    for path in sorted(source_files(root)):
        relative = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8", errors="replace")
        for code in _file_codes(relative, path.suffix, text):
            found.setdefault(code, relative)
        if relative not in _CODE_VOCABULARIES and relative not in _GENERATED:
            quoted.update(_QUOTED.findall(text))
            references.update(_VOCABULARY_REFERENCE.findall(text))
    for relative, codes in _INDIRECT_CODES.items():
        path = root / relative
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for code in codes:
            if f'"{code}"' in text:
                found.setdefault(code, relative)
    for relative, entry in _CODE_VOCABULARIES.items():
        path = root / relative
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for name, code in entry.findall(text):
            if name in references or code in quoted:
                found.setdefault(code, relative)
    return found


def catalog_codes(path: Path = CATALOG) -> dict[str, tuple[str, str]]:
    rows: dict[str, tuple[str, str]] = {}
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        columns = line.split("\t")
        if len(columns) != 3 or not re.fullmatch(_CODE, columns[0]) or not columns[1]:
            raise SystemExit(f"{path}:{number}: expected code<TAB>compact<TAB>full")
        if columns[0] in rows:
            raise SystemExit(f"{path}:{number}: duplicate code {columns[0]}")
        rows[columns[0]] = (columns[1], columns[2])
    return rows


def humanized(code: str) -> str:
    text = code.replace("_", " ")
    return text[:1].upper() + text[1:]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--list", action="store_true", help="print every used code")
    mode.add_argument("--seed", action="store_true", help="print missing codes as rows")
    args = parser.parse_args()
    used = used_codes()
    if args.list:
        for code in sorted(used):
            print(f"{code}\t{used[code]}")
        return 0
    catalog = catalog_codes()
    missing = sorted(set(used) - set(catalog))
    if args.seed:
        for code in missing:
            print(f"{code}\t{humanized(code)}\t")
        return 0
    if missing:
        for code in missing:
            print(f"error code {code!r} (used in {used[code]}) is missing from "
                  f"{CATALOG.relative_to(ROOT)}", file=sys.stderr)
        return 1
    print(f"error catalog covers all {len(used)} used codes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
