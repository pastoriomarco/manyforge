#!/usr/bin/env bash
# launch-orin-gemma.sh — bring up the full ManyForge assistant stack on a
# Jetson Orin AGX (64 GB), served by Gemma 4 12B (GGUF UD-Q4_K_XL) via
# llama.cpp with the Gemma 4 E2B GGUF as the speculative draft.
#
# This is a thin convenience wrapper over scripts/launch.sh. It pins the
# Orin model profile and the manyforge-only + assistant=on selection so the
# operator gets a turnkey "launcher for this platform". Everything else is
# launch.sh: same verbs (start/stop/status/restart/smoke/preflight), same
# review gate, same env overrides.
#
# The Orin model profile (gemma4-12b-it-gguf-orin, defined in
# NemoClaw-Thor/serving/{config,launch}.sh) differs from the Thor profile
# only in the runtime image (ghcr.io/nvidia-ai-iot/llama_cpp:latest-jetson-orin)
# and its cache mounts, which point at the NVMe:
#   /mnt/nova_ssd/hf-cache-orin   -> /data/models/huggingface + /root/.cache/huggingface
#   /mnt/nova_ssd/llama-cpp-cache -> /root/.cache/llama.cpp
# These mounts are load-bearing — they keep the ~7 GB GGUF + draft off the
# small eMMC and persist them across `docker run --rm`. The /mnt/nova_ssd
# storage layout is documented in
# isaac_ros_custom_bringup/jetson_orin_storage/README.md.
#
# Usage:
#   ./scripts/launch-orin-gemma.sh                 # interactive review gate, then start
#   ./scripts/launch-orin-gemma.sh -y              # skip the review gate
#   ./scripts/launch-orin-gemma.sh smoke           # start + strict assistant e2e smoke
#   ./scripts/launch-orin-gemma.sh status
#   ./scripts/launch-orin-gemma.sh stop
#
# Override the defaults by passing the same flag again (launch.sh's parser
# lets a later occurrence win), e.g. a different assistant scenario:
#   ./scripts/launch-orin-gemma.sh --scenario ur10e-scene-authoring smoke
#
# Env overrides (all optional; defaults shown):
#   MODEL_PROFILE=gemma4-12b-it-gguf-orin
#   THOR_HF_CACHE_DIR=/mnt/nova_ssd/hf-cache-orin           (set by the profile)
#   THOR_LLAMACPP_CACHE_DIR_HOST=/mnt/nova_ssd/llama-cpp-cache  (set by the profile)
#   ASSISTANT_PROVIDER=openclaw                             (launch.sh default)

set -euo pipefail

# Pin the Orin GGUF model profile unless the operator overrode it. start_model
# (scripts/lib/assistant.sh) forwards MODEL_PROFILE to NemoClaw-Thor's
# start-model.sh, which resolves the image + NVMe mounts for this profile.
export MODEL_PROFILE="${MODEL_PROFILE:-gemma4-12b-it-gguf-orin}"

# The launcher's openclaw sub-steps (configure-local-provider,
# setup-manyforge-assistant, the in-sandbox gateway + bridge start) shell out to
# `nemoclaw` / `openshell` / `node`, which live on the NVMe-staged PATH
# (~/.local/bin + /mnt/nova_ssd/opt/node/bin). Running this wrapper as
# `bash launch-orin-gemma.sh` (or via its shebang) is a non-login, non-interactive
# shell that sources neither ~/.bashrc nor ~/.profile, so those dirs can be
# missing from PATH and `setup-manyforge-assistant.sh` aborts with
# "nemoclaw not found". Ensure them here (idempotent; operator PATH still wins).
for _d in "${HOME}/.local/bin" /mnt/nova_ssd/opt/node/bin; do
    case ":${PATH}:" in
        *":${_d}:"*) ;;
        *) [ -d "${_d}" ] && PATH="${_d}:${PATH}" ;;
    esac
done
export PATH
unset _d

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Default selection: full assistant stack on the native (no-ROS) lane with the
# tool-driven scene-authoring scenario (Gemma 4 GGUF is text-only --no-mmproj,
# so the non-vision assistant scenario is the natural pairing). Any of these
# flags can be overridden by re-passing it in "$@".
exec "${SCRIPT_DIR}/launch.sh" \
    --lane manyforge-only \
    --assistant on \
    --scenario ur10e-scene-authoring \
    "$@"
