# Orin AGX — full stack install & run (master index)

**This is the entry point for bringing up the entire ManyForge + NemoClaw +
OpenClaw stack on a Jetson Orin AGX Dev Kit (64 GB).** The instructions are
split across the repos that own each layer; this file is the ordered checklist
with links. (All repos here are LLM-operated — follow the linked docs in order.)

> **Why a split + index:** the stack spans three repos — host storage
> (`isaac_ros_custom_bringup`), model serving + the NemoClaw/OpenShell/OpenClaw
> toolchain (`NemoClaw-Thor`), and the composer + launcher (`manyforge`). Each
> repo documents its own layer; this index sequences them. **`manyforge_specs`
> is private and intentionally holds no install/run instructions** (developer
> specs only).

**Platform:** Orin AGX = aarch64, CC 8.7 (Ampere), 64 GB unified. Everything is
staged on the NVMe (`/mnt/nova_ssd`); the ~30 GB eMMC must stay free.

## Repository availability

This index assumes the repos are checked out as siblings under
`${HOME}/workspaces/dev_ws/src/`. If a local link does not resolve, use the
matching GitHub repository before assuming the doc is stale:

| Repo | Preferred local path | Accepted alternate local path | Remote |
|---|---|---|---|
| `manyforge` | `${HOME}/workspaces/dev_ws/src/manyforge` | - | `https://github.com/pastoriomarco/manyforge` |
| `NemoClaw-Thor` | `${HOME}/workspaces/dev_ws/src/NemoClaw-Thor` | `${HOME}/workspaces/nemoclaw/src/NemoClaw-Thor` | `https://github.com/pastoriomarco/NemoClaw-Thor` |
| `isaac_ros_custom_bringup` | `${HOME}/workspaces/dev_ws/src/isaac_ros_custom_bringup` | `${HOME}/workspaces/isaac_ros-dev/src/isaac_ros_custom_bringup` | `https://github.com/pastoriomarco/isaac_ros_custom_bringup` |
| `manyforge_specs` | `${HOME}/workspaces/dev_ws/src/manyforge_specs` | - | `https://github.com/pastoriomarco/manyforge_specs` |

Minimal clone layout:

```bash
mkdir -p "${HOME}/workspaces/dev_ws/src"
cd "${HOME}/workspaces/dev_ws/src"
git clone https://github.com/pastoriomarco/manyforge.git
git clone https://github.com/pastoriomarco/NemoClaw-Thor.git
git clone https://github.com/pastoriomarco/isaac_ros_custom_bringup.git
# Optional for maintainer/spec work, not required for runtime:
git clone https://github.com/pastoriomarco/manyforge_specs.git

# Use one tested stack ref across the repos that participate in runtime.
# Deployment ref is `main` in every repo (the former three-lane-migration
# branch, merged 2026-06-12).
git -C manyforge checkout main
git -C NemoClaw-Thor checkout main
[ ! -d manyforge_specs ] || git -C manyforge_specs checkout main
```

---

## Phase order

| # | Phase | Repo / doc | Key command(s) |
|---|---|---|---|
| 0 | **Host storage** — NVMe mount, Docker `data-root`, bind mounts | Canonical recipe: [`isaac_ros_custom_bringup/jetson_orin_storage/README.md`](../../../isaac_ros_custom_bringup/jetson_orin_storage/README.md); NemoClaw contract: [`NemoClaw-Thor/ORIN-SETUP.md`](../../../NemoClaw-Thor/ORIN-SETUP.md#0-host-storage-and-runtime-prerequisites) §0 | (one-time host setup) |
| 1 | **Serve the model** — `gemma4-12b-it-gguf-orin` (Orin image, NVMe caches, 128k/q8_0/FA, cache-first `--offline`) | [`NemoClaw-Thor/ORIN-SETUP.md`](../../../NemoClaw-Thor/ORIN-SETUP.md) §1 | `./serving/start-model.sh gemma4-12b-it-gguf-orin` |
| 2 | **Install toolchain** — node + nemoclaw + openshell on NVMe; bridge venvs; `~/.nemoclaw` bind mount | [`NemoClaw-Thor/ORIN-SETUP.md`](../../../NemoClaw-Thor/ORIN-SETUP.md) §2 | (see §2a–2e) |
| 3 | **Create the sandbox** — start the model, run first-time `nemoclaw onboard` (`provider=custom`), then apply the local-provider config | [`NemoClaw-Thor/ORIN-SETUP.md`](../../../NemoClaw-Thor/ORIN-SETUP.md) §3 | `nemoclaw onboard --non-interactive …` |
| 4 | **Build the composer image** | this repo (below) | `./scripts/setup-docker.sh` |
| 5 | **Run the stack** — launcher brings up Composer, model/proxy, provider config, OpenClaw provisioning (`setup-manyforge-assistant`), gateway/bridge, and supervisor | [`LAUNCHER_QUICKSTART.md`](LAUNCHER_QUICKSTART.md) (Jetson Orin section) | `./scripts/launch-orin-gemma.sh` |
| 6 | **Smoke** | this repo + NemoClaw-Thor (below) | `smoke_corpus_runner.py` |

Phases 0–3 are one-time host setup. Phases 4–6 are the build/run/verify loop.
The full NVMe procedure stays in `isaac_ros_custom_bringup`; the minimum
runtime contract is repeated in `NemoClaw-Thor` so agents starting from that repo
can verify the load-bearing storage assumptions without rediscovering this
index.

---

## Phase 4 — Build the composer image (`manyforge-dev:latest`)
```bash
cd manyforge
./scripts/setup-docker.sh          # pre-clones RoboPlan, then `docker compose build`
# or just the dev image:  docker compose build dev
```
**Orin gotcha (fixed in `Dockerfile`):** `pin==3.9.0` (pinocchio) links against
**urdfdom 4.0**, but the transitive `cmeel-urdfdom` was unpinned and pip
resolved **v6** → the RoboPlan bindings wheel died with
`ImportError: liburdfdom_sensor.so.4.0`. The `Dockerfile` now pins the cmeel set
via a constraints file (`cmeel-urdfdom==4.0.1`, …) taken from the working Thor
image. A fresh build now succeeds; verify with
`docker run --rm --entrypoint bash manyforge-dev:latest -lc 'python3 -c "import roboplan"'`.

## Phase 5 — Run the stack
The model profile is selected by `MODEL_PROFILE`; on Orin use the turnkey wrapper
(pins `gemma4-12b-it-gguf-orin` + assistant lane):
```bash
./scripts/launch-orin-gemma.sh                      # direct/openclaw per ASSISTANT_PROVIDER
ASSISTANT_PROVIDER=openclaw OPENCLAW_ASSISTANT_TOOL_SURFACE=tools ./scripts/launch-orin-gemma.sh
```
`launch.sh` starts Composer and the selected deployment first, then brings up
the assistant runtime pieces: model/proxy → local-provider config →
OpenClaw provisioning (`setup-manyforge-assistant`) → gateway/bridge →
supervisor status. See the **Jetson Orin AGX** section of
[`LAUNCHER_QUICKSTART.md`](LAUNCHER_QUICKSTART.md). It requires Phases 0–4 done
(image present, toolchain installed, sandbox onboarded) — it does **not** install
the toolchain or onboard.

## Phase 6 — Smoke (corpus)
The smoke harness is `smoke_corpus_runner.py` (the "60-case corpus") in
**NemoClaw-Thor**; it drives the composer and is lane-agnostic (the lane = the
provider the composer is configured with). Short subset:
```bash
# from ${HOME}/workspaces/dev_ws/src/manyforge
python3 ../NemoClaw-Thor/manyforge/scripts/debug/smoke_corpus_runner.py \
  --filter '^(P1|P2|P3|WRAP_root|SCENE_add|TREE_insert)' \
  --report /tmp/gemma-orin-smoke/<lane>-report.json
```
Reference cell runners (composer + bridge + corpus + teardown, per lane) are
archived in
[`NemoClaw-Thor/manyforge/docs/smoke-evidence/2026-06-05-gemma4-12b-it-gguf/`](../../../NemoClaw-Thor/manyforge/docs/smoke-evidence/2026-06-05-gemma4-12b-it-gguf/).

---

## Cross-cutting Orin gotchas (all hit + fixed)
Full table in [`NemoClaw-Thor/ORIN-SETUP.md`](../../../NemoClaw-Thor/ORIN-SETUP.md#orin-gotchas-all-hit--fixed-this-session). Summary:
- **composer build**: pin `cmeel-urdfdom==4.0.1` (done in `Dockerfile`).
- **model re-download**: cache-first `--offline` (profile default).
- **`~/.nemoclaw`**: bind-mount to NVMe, never symlink (NemoClaw rejects symlinked config).
- **onboard provider**: `custom` (v0.0.59 renamed from `compatible-endpoint`).
- **in-sandbox gateway**: `--bind loopback --auth token`.
- **bridge venvs**: `sudo apt-get install -y python3.12-venv` first.

## Status (2026-06-05, this Orin)
- Phases 0–4 ✅ verified. Model serves (offline cache, clean `tool_calls`).
- **Direct lane smoke**: 3/9 on the hard subset (`P1`, `WRAP_root_medium`,
  `SCENE_add_generic` pass; fails = gemma arg-shape quirk + bounded-autonomy +
  3× 504 on slow multi-turn). Pipeline healthy; matches Thor's direct profile
  (Orin's slower prefill tips a couple more multi-turn cases over the 170 s wall).
- **OpenClaw-tools lane**: ✅ verified end-to-end — **6/9** on the same hard subset
  (beats direct's 3/9; the tool surface/schema guidance helps Gemma avoid the
  direct lane's arg-shape and bounded-autonomy failures). Egress works through the
  OpenShell subject-whitelist (the `setup-manyforge-assistant` reachability probe
  uses `curl` and 403s as a *false negative* — the real node/openclaw subject is
  allowed). Same model path as direct: agent → `:8000` vllm-proxy (mutations
  applied) → llama.cpp `:8050`. Failures are the known gemma behaviors + the
  multi-turn 504 (P3, 170 s wall) — same as direct/Thor.
- Live message monitoring: `NemoClaw-Thor/manyforge/scripts/debug/pipeline_message_monitor.py`
  (tails every hop; records the verified proxy/composer field paths).
