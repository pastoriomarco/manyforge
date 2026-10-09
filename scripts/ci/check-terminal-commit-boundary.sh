#!/usr/bin/env bash
# check-terminal-commit-boundary.sh — K2 guard: terminal execution states
# (Completed/Stopped/Failed/Interrupted/InterventionRequired) may only be
# committed through TerminalCommitter. A direct try_transition to a terminal
# state re-opens the authority-laundering window the committer closes
# (decomposition plan K2/K3; spec/230 terminal authority).
#
# Allowed exceptions: the committer's own implementation and the state-machine
# unit tests that pin the transition table. The second grep catches the variable-target form that previously
# escaped the literal-enum check (for example `try_transition(terminal_state)`).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."

literal_violations=$(grep -rn \
  "try_transition(ExecutionState::k\(Completed\|Stopped\|Failed\|Interrupted\|InterventionRequired\))" \
  manyforge_core/src manyforge_core/include manyforge_core/test \
  --include="*.cpp" --include="*.hpp" \
  | grep -v "manyforge_core/src/terminal_committer.cpp" \
  | grep -v "manyforge_core/test/test_state_machine.cpp" \
  || true)

variable_violations=$(grep -rnE \
  "try_transition\\([^)]*(terminal|Terminal|decided|proposed)[^)]*\\)" \
  manyforge_core/src manyforge_core/include manyforge_core/test \
  --include="*.cpp" --include="*.hpp" \
  | grep -v "manyforge_core/src/terminal_committer.cpp" \
  | grep -v "manyforge_core/test/test_state_machine.cpp" \
  || true)

violations="${literal_violations}"
if [[ -n "${literal_violations}" && -n "${variable_violations}" ]]; then
  violations+=$'\n'
fi
violations+="${variable_violations}"

if [[ -n "${violations}" ]]; then
  echo "ERROR: direct terminal try_transition outside TerminalCommitter:" >&2
  echo "${violations}" >&2
  echo "Route terminal commits through commit_terminal()/TerminalCommitter." >&2
  exit 1
fi
echo "terminal-commit boundary OK"
