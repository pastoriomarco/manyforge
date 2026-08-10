#!/usr/bin/env bash
# Typed cutover gate for the removed unified launcher.
#
# The complete launcher is pinned at
# manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
# f11f1344ae2fc8b8d3875318f8d60aef7a1b6766. It is design and acceptance
# input, never a runnable fallback.
#
# Historical responsibility mapping:
#   parse_args, usage, wizard selection, and review rendering
#       -> thin operator client over Manager/headless facts at I4/I5;
#   native program/deployment launch and smoke orchestration
#       -> addressed Component Manager, Headless, Program-runner, and Composer
#          entrypoints at N/I6 (now active for the maintained ManyForge-only
#          product rows, but not through this aggregate launcher);
#   assistant status observation
#       -> qualified read-only headless client at H0/I5;
#   assistant selection, setup, bridge, and provider operation
#       -> addressed assistant client at N/I6;
#   Isaac and real-robot lane orchestration
#       -> I/I9 and R/I10 respectively; and
#   stop/cleanup of PID files, named containers, Compose services, and the
#   real-robot supervisor
#       -> owner-issued process/container identities at I4/I5/I9/I10.
#
# The historical cleanup body used mutable PID files, process command strings,
# and container names that the relocated source can no longer authenticate.
# Keeping it active would risk signaling a reused PID or deleting an unrelated
# same-name container. The file also combines still-future Isaac and real-robot
# responsibilities, so there is no one exact current successor to activate.
# Every verb therefore fails before argument parsing,
# imports, filesystem access, subprocesses, Docker, ROS, network, or hardware
# activity. Future cleanup must consume identities minted by the new owners;
# it must not revive the name/PID compatibility contract.

set -euo pipefail

readonly UNAVAILABLE_EXIT_CODE=78
readonly UNAVAILABLE_MESSAGE="composable_cell_runtime_unavailable: this legacy aggregate launcher is not a current product entrypoint; maintained ManyForge-only runtime functions use the Component Manager, Headless, Program runner, and Composer entrypoints, while unified Isaac, real-robot, and owner-issued cleanup orchestration remain at their later gates."

printf '%s\n' "${UNAVAILABLE_MESSAGE}" >&2
exit "${UNAVAILABLE_EXIT_CODE}"
