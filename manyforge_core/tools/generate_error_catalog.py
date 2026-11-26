#!/usr/bin/env python3
"""Render the error catalog into C++, Python and TypeScript constants.

The one source is `manyforge_core/resources/error_catalog.tsv` (`code`,
`compact text`, `full text`). The outputs are generated, checked in and kept
fresh by `--check` in CI; nothing reads the TSV at runtime.

- C++: `manyforge::core::error_codes::k<CamelCode>` code constants, plus the
  sorted code array `kSortedCodes` and `error_catalog_code_index`,
  which maps a code to 1 + its position (0 when not catalogued) without
  allocating; Stop event records store that index (stop state model §2.4).
- Python: `manyforge_headless_api.error_codes.<UPPER_CODE>` code constants.
- TypeScript: `ERROR_CATALOG` (code -> [compact, full]) for the GUI.
- Python: `manyforge_headless_api.error_http_status.HTTP_STATUS`, the HTTP
  status of every catalogued code (Headless's error answers), from the one
  rule spec `http_status` below; `DEFAULT_HTTP_STATUS` for any other code.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "manyforge_core/resources/error_catalog.tsv"
CPP = ROOT / "manyforge_core/include/manyforge_core/types/error_catalog.generated.hpp"
PYTHON = ROOT / "manyforge_headless_api/error_codes.py"
TYPESCRIPT = ROOT / "manyforge_composer/frontend/src/api/errorCatalog.generated.ts"
HTTP_STATUS = ROOT / "manyforge_headless_api/error_http_status.py"

_CODE = re.compile(r"[a-z][a-z0-9]*(?:_[a-z0-9]+)+")
_HEADER = "Generated from manyforge_core/resources/error_catalog.tsv by manyforge_core/tools/generate_error_catalog.py; do not edit."


def read_catalog(path: Path = CATALOG) -> list[tuple[str, str, str]]:
    rows: list[tuple[str, str, str]] = []
    seen: set[str] = set()
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        columns = line.split("\t")
        if len(columns) != 3 or not _CODE.fullmatch(columns[0]) or not columns[1]:
            raise SystemExit(f"{path}:{number}: expected code<TAB>compact<TAB>full")
        if columns[0] in seen:
            raise SystemExit(f"{path}:{number}: duplicate code {columns[0]}")
        seen.add(columns[0])
        rows.append((columns[0], columns[1], columns[2]))
    return sorted(rows)


# ---------------------------------------------------------------------------
# The HTTP status rule spec. Order matters: the first matching rule decides.
# ---------------------------------------------------------------------------

DEFAULT_HTTP_STATUS = 500

# Typed refusals of Execute, Resume and a group's Run/Resume before any
# effect: every RunReadiness reason and execution block, and the runner's own
# run refusals; 409, never 5xx.
RUN_REFUSAL_CODES = frozenset(
    {
        "cell_latched",
        "component_not_enabled",
        "component_unhealthy",
        "rule_holds",
        "rule_unknown",
        "input_not_fresh",
        "run_in_progress",
        "program_motion_running",
        "manual_operation_in_progress",
        "program_not_executable",
        "execution_host_internal_fault",
        "manager_unavailable",
        "program_scene_preparation_failed",
        "program_cycle_reset_required",
        "program_run_history_full",
        "program_end_effector_tcp_unavailable",
        "program_motion_resume_incomplete",
        "io_manual_write_motion_executing",
    }
)


def http_status(code: str) -> int:
    """The HTTP status of an error answer carrying `code`."""

    if code == "draft_candidate_response_too_large":
        return 413
    if code == "component_frame_lookup_invalid":
        return 400
    if code in {"component_frame_lookup_stale", "component_reference_missing", "frame_missing"}:
        return 409
    if code == "program_run_started_then_failed":
        return 422
    if code == "full_stack_restart_required":
        return 409
    if code in {
        "headless_projection_response_invalid",
        "headless_projection_response_oversized",
        "manager_command_response_invalid",
        "execution_command_response_invalid",
        "node_environment_projection_invalid",
        "assistant_mode_context_invalid",
        "source_model_context_invalid",
        "source_model_projection_invalid",
        "program_applied_selection_invalid",
        "program_runner_response_invalid",
        "viewer_model_asset_invalid",
        "assistant_mode_projection_invalid",
    }:
        return 502
    if code == "headless_api_internal_failure":
        return 500
    if code.endswith("_not_found") or code in {
        "missing_reference",
        "operator_alarm_not_found",
        "planning_job_unknown",
    }:
        return 404
    if (
        "conflict" in code
        or "stale" in code
        or "fence" in code
        or code.startswith(("program_cycle_state_", "program_run_state_"))
        or code
        in {
            "invalid_state_transition",
            "command_not_allowed",
            "manual_handover_required",
            "object_already_exists",
            "program_run_admission_failed",
            "program_run_already_active",
            "program_applied_non_executable",
            "program_not_selected",
            # Preparation runs before any claim or effect; it refuses the input.
            "activation_preparation_failed",
            "program_reference_validation_failed",
            "prepared_activation_unverified",
            "authoring_not_ready",
            "program_run_draft_applied_mismatch",
            "activation_preparer_busy",
            "model_sources_unknown",
            "program_node_preview_busy",
            "deployment_candidate_cell_mismatch",
            "persistent_key_request_replayed",
            # An operator-alarm condition interlock refuses motion before any effect.
            "operator_alarm_interlock_active",
            "request_result_expired",
            # Component model v2: refusals before dispatch.
            "component_action_in_progress",
            "component_action_unsupported",
            "owner_not_prepared",
            "io_output_controller_owned",
            # A position reset while the simulation isn't playing.
            "simulation_not_playing",
            # An assistant mode read while the Deployment has no assistant.
            "assistant_component_absent",
            # Pick/place planning jobs: a held name, a release before the end.
            "planning_job_exists",
            "planning_job_unfinished",
        }
        # RunReadiness reasons and run refusals.
        or code in RUN_REFUSAL_CODES
    ):
        return 409
    # A busy refusal is "try again later" (OD-413), unless a rule above makes
    # it a conflict.
    if "unavailable" in code or code.endswith("_busy") or code in {
        "assistant_ai_model_absent",
        "headless_api_closed",
        "kernel_owner_unavailable",
        "execution_owner_not_ready",
        "operator_alarm_state_unavailable",
        "program_applied_selection_not_active",
        "program_runner_unavailable",
        "program_run_scheduling_failed",
        "program_cycle_reconciliation_required",
        "request_ledger_full",
        "scene_projection_commit_uncertain",
        # A runtime unit launching or preparing (contract 1.3).
        "runtime_unit_launch_in_progress",
    }:
        return 503
    if (
        code == "invalid_request"
        or code.endswith("_invalid")
        or code.endswith("_rejected")
    ):
        return 400
    if code in {
        # A Scene change whose result would exceed the Scene size limit.
        "scene_too_large",
        "confirmation_required",
        "request_key_required",
        "start_identity_required",
        "operator_alarm_acknowledgement_confirmation_required",
        "operator_alarm_acknowledgement_reason_required",
        "operator_alarm_acknowledge_all_confirmation_required",
        "operator_alarm_acknowledge_all_duplicate_member",
        "operator_alarm_limit_invalid",
        "operator_alarm_state_filter_invalid",
    }:
        return 400
    if code == "operator_alarm_membership_mismatch":
        return 409
    return DEFAULT_HTTP_STATUS


def _camel(code: str) -> str:
    return "k" + "".join(part[:1].upper() + part[1:] for part in code.split("_"))


def render_cpp(rows: list[tuple[str, str, str]]) -> str:
    names: dict[str, str] = {}
    lines = [
        f"// {_HEADER}",
        "#pragma once",
        "",
        "#include <algorithm>",
        "#include <array>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace manyforge::core::error_codes {",
        "",
    ]
    for code, _compact, _full in rows:
        name = _camel(code)
        if name in names:
            raise SystemExit(f"codes {names[name]} and {code} share the C++ name {name}")
        names[name] = code
        lines.append(f'inline constexpr std::string_view {name}{{"{code}"}};')
    if len(rows) >= 0xFFFF:
        raise SystemExit("error catalog index no longer fits uint16 below 0xFFFF")
    lines += [
        "",
        "/// Every catalogued code, sorted by code text (byte order).",
        f"inline constexpr std::array<std::string_view, {len(rows)}> kSortedCodes{{",
    ]
    lines += [f"    {_camel(code)}," for code, _compact, _full in rows]
    lines += [
        "};",
        "",
        "/// 1 + the position of `code` in `kSortedCodes`; 0 when the",
        "/// code is not catalogued. Binary search; no allocation.",
        "[[nodiscard]] constexpr std::uint16_t error_catalog_code_index(std::string_view code) noexcept {",
        "  const auto found = std::lower_bound(kSortedCodes.begin(),",
        "                                      kSortedCodes.end(), code);",
        "  if (found == kSortedCodes.end() || *found != code) return 0U;",
        "  return static_cast<std::uint16_t>(1 + (found - kSortedCodes.begin()));",
        "}",
        "",
        "}  // namespace manyforge::core::error_codes",
        "",
    ]
    return "\n".join(lines)


def render_python(rows: list[tuple[str, str, str]]) -> str:
    lines = [f'"""{_HEADER}"""', "", "from typing import Final", ""]
    lines += [f'{code.upper()}: Final = "{code}"' for code, _compact, _full in rows]
    return "\n".join(lines) + "\n"


def render_typescript(rows: list[tuple[str, str, str]]) -> str:
    lines = [
        f"// {_HEADER}",
        "",
        "/** code -> [compact text, full text]; an empty full text is not written yet. */",
        "export const ERROR_CATALOG: Readonly<Record<string, readonly [string, string]>> = {",
    ]
    lines += [
        f"  {code}: [{json.dumps(compact, ensure_ascii=False)}, {json.dumps(full, ensure_ascii=False)}],"
        for code, compact, full in rows
    ]
    lines += ["};", ""]
    return "\n".join(lines)


def render_http_status(rows: list[tuple[str, str, str]]) -> str:
    lines = [
        f'"""{_HEADER}',
        "",
        "The HTTP status of Headless's error answer for each catalogued code;",
        "any other code answers DEFAULT_HTTP_STATUS.",
        '"""',
        "",
        "from types import MappingProxyType",
        "from typing import Final, Mapping",
        "",
        f"DEFAULT_HTTP_STATUS: Final = {DEFAULT_HTTP_STATUS}",
        "",
        "HTTP_STATUS: Final[Mapping[str, int]] = MappingProxyType({",
    ]
    lines += [f'    "{code}": {http_status(code)},' for code, _compact, _full in rows]
    lines += ["})", ""]
    return "\n".join(lines)


def outputs() -> dict[Path, str]:
    rows = read_catalog()
    return {
        CPP: render_cpp(rows),
        PYTHON: render_python(rows),
        TYPESCRIPT: render_typescript(rows),
        HTTP_STATUS: render_http_status(rows),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail when an output is stale")
    args = parser.parse_args()
    stale = []
    for path, text in outputs().items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current == text:
            continue
        if args.check:
            stale.append(path.relative_to(ROOT))
        else:
            path.write_text(text, encoding="utf-8")
    if stale:
        for path in stale:
            print(f"{path} is stale; run manyforge_core/tools/generate_error_catalog.py", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
