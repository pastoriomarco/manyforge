#!/usr/bin/env bash
# Jetson Orin assistant-launch operator intent and typed cutover boundary.
#
# The historical wrapper pinned MODEL_PROFILE=gemma4-12b-it-gguf-orin,
# ensured the NVMe-staged OpenShell/Node paths were visible, and delegated to
# launch.sh with the manyforge-only, assistant-on, scene-authoring selection.
# Its exact source is
# manyforge@08222188293df8e12f90dc72e204c21f22f92ea1, blob
# dec8cf2baed4cd2dd681e71f4b49f21b77807580.
#
# Operator intent to preserve at restoration:
#   * keep model and llama.cpp caches on /mnt/nova_ssd rather than eMMC;
#   * retain the Orin-specific Gemma profile as an overridable default;
#   * preserve launch.sh verbs/arguments and PATH discovery; and
#   * do not claim the assistant or scene-authoring lanes before their gates.
#
# Historical examples (currently unavailable):
#   ./scripts/launch-orin-gemma.sh
#   ./scripts/launch-orin-gemma.sh -y
#   ./scripts/launch-orin-gemma.sh smoke
#   ./scripts/launch-orin-gemma.sh status
#   ./scripts/launch-orin-gemma.sh stop
#
# Historical environment defaults:
#   MODEL_PROFILE=gemma4-12b-it-gguf-orin
#   THOR_HF_CACHE_DIR=/mnt/nova_ssd/hf-cache-orin
#   THOR_LLAMACPP_CACHE_DIR_HOST=/mnt/nova_ssd/llama-cpp-cache

set -euo pipefail

_manyforge_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/composable-cell-runtime-availability.sh
source "${_manyforge_script_dir}/lib/composable-cell-runtime-availability.sh"
MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_MESSAGE="${MANYFORGE_COMPOSABLE_CELL_RUNTIME_UNAVAILABLE_REASON_CODE}: the Orin assistant launcher requires N7 live-product and platform-lifecycle qualification."
manyforge_fail_composable_cell_runtime_unavailable
