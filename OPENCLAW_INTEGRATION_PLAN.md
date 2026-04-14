# OpenClaw Integration Plan for ManyForge

**Goal**: Integrate NemoClaw/OpenClaw as ManyForge's LLM orchestration layer,
enabling autonomous agent-driven supervision, per-cycle perception-driven
decision-making, delegated failure recovery, and interactive program
composition assistance — while keeping deterministic behavior tree execution
as the authority for robot motion.

**Date**: 2026-04-12 (revised)

**Architecture decision**: Option B — ManyForge and OpenClaw run in separate
containers/processes. ManyForge keeps host hardware access (robot, GPU, ROS2).
OpenClaw runs inside a NemoClaw sandbox. They communicate exclusively through
ManyForge's HTTP API (REST + MCP). The sandbox is a safety feature, not
overhead.

**Deterministic mode**: ManyForge remains fully operational without any LLM
dependency. All LLM integration points are opt-in via CLI flags, deployment
configuration, and node selection. See Section 10 for details.

---

## 1. Context

### 1.1 What ManyForge Provides Today

ManyForge is a self-contained robotics execution framework with:

- **CycleManager** — deterministic state machine
  (IDLE / RUNNING / STOPPED / DIRECT_CONTROL)
- **OrchestratorRunner** — py_trees behavior tree executor (10 Hz tick loop)
- **ExecutionKernel** — C++ motion planner and trajectory executor
  (RoboPlan: RRT + TOPPRA + IK, or MockKernelClient)
- **Composer** — FastAPI REST API + React frontend + Viser 3D HMI
- **MCP server** — 12+ tool interface for LLM agents (stdio + HTTP/SSE
  transport, implemented in OP-8)
- **EventRecorder** — execution history with ring buffer + JSONL file sink,
  model-call audit events (OP-9)
- **Intervention mode** — temporary subtree execution without modifying the
  saved program, with authority provenance, blackboard scoping, lease
  management, and promotion
- **Runtime overrides** — per-node parameter patching with active/staged
  lifecycle and modification_policy enforcement
- **Recovery infrastructure** — `RecoveryTrigger` enum, `recovery_policy`
  on TreeNode, `FallbackOverrideNode` and `RecoverySkillNode` decorators
  for deterministic in-tree recovery (OP-6)
- **Skill catalog** — `pick_object`, `place_object`, `move_to_named_config`
  with pre/postconditions, capability requirements, and parameter contracts
- **QueryModelNode** — non-blocking leaf node for model endpoint queries
  with ThreadPoolExecutor, schema validation, confidence gating, and
  model audit event integration (OP-7)

The LLM agent today is the human operator using Claude Code or the MCP
server. There is no autonomous agent loop.

### 1.2 What NemoClaw/OpenClaw Provides

NemoClaw is a three-layer stack running on Jetson AGX Thor (128 GB unified
memory, SM110a Blackwell GPU):

- **OpenClaw** — the dominant open-source AI agent runtime (355K+ GitHub
  stars, 30K+ commits, daily releases, MIT license). TypeScript-based
  Gateway + Pi agent runtime with tool use, MCP client and server, session
  management (JSONL transcripts), multi-agent routing, and a deterministic
  skills system (AgentSkills spec + ClawHub registry). Supports 25+
  messaging platforms (Telegram, Slack, WhatsApp, Discord, Signal, Teams,
  etc.) as operator channels. NVIDIA directly wraps OpenClaw with NemoClaw.
- **NemoClaw** — sandboxing lifecycle manager (OpenShell blueprint/plan/apply
  pattern, Landlock + seccomp + netns isolation). Provisions and manages
  OpenClaw instances inside hardened sandbox pods.
- **NemoClaw-Thor** — local inference layer (vLLM 0.19 with FlashInfer
  CUTLASS SM110 kernels, 8 model profiles, KV cache budget allocation,
  multi-agent concurrency)

OpenClaw's MCP client supports three transports natively: stdio (local
subprocess), SSE/HTTP (remote endpoint with auth headers), and streamable
HTTP (bidirectional remote). Skills follow the AgentSkills specification —
deterministic, versioned artifacts installed via `openclaw skills install`,
not self-improving procedures. The workspace knowledge system uses
`AGENTS.md` (agent configuration), `SOUL.md` (identity/boundaries), and
`MEMORY.md` (persistent facts) as context-injection files.

Proven capabilities (from CLAW_USAGE_REPORT.md, offline coding context —
not yet validated for runtime robot operation):

- Autonomous C++ implementation from specs — 5 tasks completed in one shot
  each, 52 tests, zero fix cycles (Qwen 3.5-27B-Claude-Distilled)
- Multi-agent concurrency: 6 concurrent sequences yield 8.3x throughput of
  a single request on Thor (bandwidth-bound, not compute-bound)
- Tool calling verified with 378-line, 9.9K character files in a single
  tool call
- Session isolation and reproducibility via sandbox pods

### 1.3 What Exists Between Them

Current integration state (partially ad-hoc, partially implemented):

**Implemented (OP-6/7/8/9)**:
- MCP HTTP/SSE transport on `/api/mcp` + `/api/mcp/sse` (OP-8)
- `QueryModelNode` with non-blocking execution, schema validation,
  confidence gating (OP-7)
- Model audit event infrastructure: `record_model_event()` with redaction
  via `ModelAuditPolicy` (OP-9)
- Recovery infrastructure: `RecoveryTrigger` enum, `recovery_policy` on
  TreeNode, `FallbackOverrideNode`, `RecoverySkillNode` (OP-6)
- X-Principal-ID validation and auth boundary hardening

**Ad-hoc (offline coding, not runtime)**:
- `manyforge-session-start.sh` uploads specs, contract headers, and worker
  workspaces to the NemoClaw sandbox
- Claude Code on the host dispatches OpenClaw agents via kubectl exec to
  implement C++ code against ManyForge specs
- Results are downloaded and committed on the host
- ManyForge's runtime (Composer, HMI, kernel) is not connected to the
  sandbox at all

### 1.4 Design Principles

1. **Deterministic execution is non-negotiable.** The behavior tree tick loop
   and the kernel's motion planner own robot motion. An LLM never drives
   joints directly. It provides targets, parameters, and decisions that the
   deterministic pipeline consumes.

2. **LLM latency is a design constraint, not a bug.** Even on Thor
   (~100-200 ms first-token, 2-10 s per complete decision), the LLM is too
   slow for reactive control (collision response, sensor-triggered stops).
   The architecture must separate timescales: millisecond-level reactive
   safety (kernel), second-level supervisory decisions (LLM).

3. **Traceability is mandatory.** Every LLM decision that affects robot
   behavior must be recorded in ManyForge's event stream with full authority
   provenance (who requested it, why, what authority allowed it, what changed).

4. **The saved program artifact is the source of truth.** Runtime overrides,
   intervention subtrees, and LLM-computed parameters are transient
   adaptations that do not silently modify the canonical artifact. The
   distinction between `saved_program` and `runtime_override` must be
   maintained in all surfaces.

5. **Sandboxing is a safety feature.** In an industrial environment, an LLM
   agent that can reason about and affect robot motion must be constrained
   at the OS level. OpenShell's Landlock/seccomp/netns sandbox provides
   hardware-enforced isolation. The agent's only path to the robot is
   through ManyForge's validated API — two layers of enforcement: sandbox
   prevents bypass, ManyForge prevents misuse.

6. **Operational answers should not rely on free text.** When a workflow
   requires a machine-checked result, the completion contract should be a
   typed tool call or schema-validated structured payload, not a plain-text
   answer that merely "looks correct". This is especially important for
   supervisory and recovery flows in an industrial setting.

7. **LLM is optional.** ManyForge must remain fully functional without any
   LLM dependency. All four integration modes are opt-in. A fully
   deterministic deployment — manual composition, no `query_model` nodes,
   no delegation, no assistant — must be a first-class supported
   configuration. See Section 10.

---

## 2. Integration Modes

The integration defines four operational modes, each serving a distinct
industrial need. They share infrastructure but differ in trigger, authority,
and latency requirements. All modes are opt-in.

### Mode A: Delegated Recovery (Runtime — Tree-Triggered)

**Use case**: A behavior tree node encounters a failure (planning failed,
validation failed, timeout, scene mismatch) and delegates recovery to an
LLM agent rather than failing the entire program.

**Trigger**: A new `delegate_to_llm` catalog node in the behavior tree.

**Flow**:
```
Behavior tree is RUNNING
  → node "pick_object" fails (planning failed: object unreachable)
  → parent fallback ticks next child: "delegate_to_llm" node
  → delegate_to_llm:
      1. Transitions CycleManager to DIRECT_CONTROL
      2. Calls OpenClaw agent via MCP/REST with:
         - failure context (which node failed, why, current scene state)
         - recovery scope (what the agent is allowed to do)
         - lease timeout (max time for recovery attempt)
      3. Agent reasons about the failure:
         - queries scene state, blackboard, execution history
         - decides on recovery action (e.g., adjust object pose, retry
           with different approach, add a collision object, skip this cycle)
      4. Agent executes recovery via ManyForge MCP tools:
         - scene edits, runtime overrides, intervention subtrees
      5. Agent completes → delegate_to_llm node returns SUCCESS or FAILURE
      6. CycleManager returns to RUNNING (or STOPPED on failure)
      7. Behavior tree continues from the fallback's perspective
```

**Authority model**:
- `session_kind: "delegated_recovery"`
- `authority_source: "program_node"`
- `trigger_node_instance: "pick_fallback_delegate"`
- `trigger_reason: "planning_failed"`
- `approval_mode: "policy_granted"` (the tree author pre-approved this
  delegation by including the node)

**Scope constraints**: The delegate_to_llm node must declare what the agent
is allowed to do: scene edits, parameter overrides, intervention subtrees,
blackboard writes. The agent cannot exceed this scope.

**Latency budget**: Seconds to minutes. The tree is paused (kernel PAUSED)
while the agent works. Lease timeout enforces an upper bound.

**Deterministic alternative**: Without this mode, the tree author uses
`FallbackOverrideNode` or `RecoverySkillNode` (OP-6) for deterministic
recovery. `delegate_to_llm` is only needed when deterministic recovery
logic is insufficient.

### Mode B: Perception-Driven Cycle Targets (Runtime — Per-Cycle)

**Use case**: Tasks with variable environments where each cycle requires a
perception step to determine targets. Example: bin picking where a VLM
analyzes the scene to select which object to pick and compute the grasp
pose.

**Trigger**: A `query_model` catalog node in the behavior tree that calls a
model endpoint and writes results to the blackboard.

**Flow**:
```
Cycle start (IDLE → RUNNING)
  → tree ticks "query_model" node:
      1. Calls a model endpoint (VLM or LLM) with:
         - scene snapshot (GET /api/scene/snapshot → JPEG)
         - structured scene state (GET /api/scene/state → JSON)
         - prompt template with cycle context
      2. Model returns structured output:
         - target object ID, grasp pose, approach vector, confidence
      3. Node parses response and writes to blackboard:
         - target_object_id = "part_0042"
         - grasp_pose = {position_m: [...], orientation_xyzw: [...]}
         - approach_height_m = 0.08
      4. Node returns SUCCESS (or FAILURE if model confidence is too low)
  → tree continues with parameterized pick sequence:
      - move_manipulator_action reads grasp_pose from blackboard via
        `pose_goal: "${bb:...}"`
      - pick_object skill uses target_object_id
  → deterministic execution of the planned trajectory
  → cycle completes, repeat from top
```

**Key distinction from Mode A**: This is not recovery — it is the normal
execution path. The model call is a leaf node in the tree, synchronous,
and fast (sub-second for a local VLM). The behavior tree remains the
authority; the model provides data, not control.

**Model options on Thor**:
- **Gemma 4 31B (NVFP4, vision)**: Native vision capability, can process
  scene snapshots directly. ~6 concurrent sequences.
- **Qwen 3.5 27B**: Text-only, but can reason about structured scene state
  JSON without requiring an image.
- **Dedicated small VLM**: A separate vLLM instance serving a 2-4B vision
  model for fast perception, as described in the dual-model vision
  architecture (LLM_OPERABILITY_PLAN Section 5.3).

**Authority model**: No intervention session needed. The model call is a
regular tree node with no special authority. It writes to the blackboard
like any other node. The behavior tree author decides where to place it.

**Latency budget**: Hundreds of milliseconds to low single-digit seconds.
The tree tick loop waits for the model response (the node returns RUNNING
until the response arrives, then SUCCESS/FAILURE).

**Deterministic alternative**: Without this mode, the tree author
hard-codes target parameters in the tree or uses blackboard values set by
other deterministic nodes (sensor readings, fixed configurations).

### Mode C: Emergency / Supervisory Operator (Runtime — External)

**Use case**: An external LLM agent monitors ManyForge's execution and
intervenes when it detects anomalies that the behavior tree's own fallback
logic cannot handle. This is the autonomous operator that the
LLM_OPERABILITY_PLAN envisions.

**Trigger**: External — the OpenClaw agent polls ManyForge's execution
digest and decides when to intervene.

**Flow**:
```
OpenClaw agent session (long-running, in NemoClaw sandbox)
  → Agent starts ManyForge session:
      - manyforge_program_load(path, deploymentPath)
      - manyforge_cycle_control(command="run")
  → Monitoring loop:
      - Poll manyforge_execution_status(include="digest") every 2-5s
      - Check for: errors, warnings, stalled progress, unexpected state
      - Optionally poll manyforge_scene_state() for spatial anomalies
      - Optionally request scene snapshot for VLM analysis
  → On anomaly detection:
      - manyforge_cycle_control(command="stop")
      - manyforge_debug_export() for full diagnostic context
      - Agent reasons about the situation
      - Decision: retry, intervene, adapt, or escalate to human
  → On intervention decision:
      - manyforge_intervention(action="execute", tree=recovery_subtree,
          session_kind="manual_intervention",
          authority_source="llm",
          approval_mode="not_required")
      - Monitor intervention via manyforge_intervention(action="status")
      - On completion: promote blackboard values if needed
      - manyforge_cycle_control(command="reset") or resume
  → On adaptation decision:
      - manyforge_runtime_override(action="apply", node_name=...,
          params=..., authority_source="llm")
      - manyforge_cycle_control(command="reset")
      - manyforge_cycle_control(command="run")
  → On escalation:
      - Agent reports to human operator with diagnosis and recommendations
        via OpenClaw Gateway messaging (Telegram, Slack, WhatsApp, Teams,
        or any configured platform)
      - Waits for human decision before proceeding
```

**Multi-platform escalation**: OpenClaw's Gateway natively supports 25+
messaging platforms. When the supervisor agent escalates, it sends a
structured diagnostic message through the operator's preferred channel
(e.g., Telegram on a phone). The operator can respond with approval,
denial, or instructions — the agent receives the response through the same
Gateway and acts accordingly. This eliminates the need for the operator to
be at a workstation during escalation.

**Authority model**:
- From STOPPED: `authority_source: "llm"`, `approval_mode: "not_required"` —
  already supported.
- From IDLE: Currently blocked by the human-approval gate (OP-4). Options:
  - Keep blocked and require the agent to go through a run → stop → intervene
    cycle (safe, more steps)
  - Add an explicit `llm_operator` policy that authorizes specific agents
    for IDLE direct control (OP-6 deliverable)

**NemoClaw/OpenClaw role**: OpenClaw provides the structured agent runtime
(tool use, session management, multi-step reasoning) and the multi-platform
Gateway for operator escalation (Telegram, Slack, WhatsApp, etc.). NemoClaw
provides sandboxing so the agent cannot affect host systems beyond the
ManyForge API. NemoClaw-Thor provides local inference so the monitoring loop
doesn't depend on cloud connectivity (industrial reliability requirement).

**Latency budget**: Seconds to minutes for intervention decisions. The
monitoring poll is non-blocking. The robot is stopped before the agent
reasons about recovery.

#### Structured Terminal Answer Pattern

For operational workflows where the final result must be consumed by another
machine step, the final answer should be emitted through a narrow typed tool,
not free text.

Example:

- a monitoring workflow may use ManyForge read-only tools freely
- but it must complete with a terminal tool such as:
  - `report_lifecycle_state(lifecycle_state: enum[idle, running, stopped, direct_control])`
  - `report_recovery_outcome(outcome: enum[resolved, unresolved, escalated], summary_code: string)`
  - `report_program_operation(action: enum[load, validate, save, run, stop, intervene], result: enum[success, failure], summary_code: string)`

Rules:

1. plain-text completion does not count as success when a terminal answer tool
   is required
2. the terminal answer tool schema must be narrow and typed
3. retries must be bounded; if the agent does not produce a valid terminal
   tool call, the workflow fails closed
4. the terminal answer tool itself must not grant additional operational
   authority — it is a reporting contract, not a capability expansion

This pattern is recommended for:

- Mode C supervisory monitoring loops
- delegated recovery outcomes in Mode A
- smoke/integration checks that must return deterministic machine-readable
  results

### Mode D: Composer Assistant (Design-Time — Interactive)

**Use case**: A human operator uses a chat interface in the Composer UI to
interactively construct, debug, and refine behavior trees with an LLM that
deeply understands ManyForge's node catalog, skill system, program structure,
and composition rules.

**Trigger**: User types in the Composer's chat overlay panel.

**Flow**:
```
User opens Composer UI (localhost:9000)
  → Chat overlay panel visible (resizable, minimizable)
  → User types: "Add a bin picking sequence that uses vision to find parts"
  → Composer backend proxies to OpenClaw agent in sandbox:
      - Agent has ManyForge MCP tools for live state queries
      - Agent has domain knowledge (AGENTS.md, LLM_GUIDE.md, catalog)
      - Agent can validate proposals via POST /api/program/validate
  → Agent responds with:
      - Explanation of the approach
      - Proposed tree structure (rendered as a preview diff in the editor)
      - Parameter suggestions with rationale
  → User reviews, modifies, accepts or rejects
  → On accept: tree is applied to the editor canvas
  → User can continue: "Make the approach height configurable as a
    runtime override" → agent adjusts modification_policy + params
  → User validates: "Check if this tree is valid" → agent calls
    POST /api/program/validate and explains any issues
```

**Chat overlay panel**: A resizable, minimizable overlay in the Composer UI
that floats over the existing interface. It can be:
- Resized by dragging edges (minimum 300px wide, 200px tall)
- Minimized to a rail button (like existing panels)
- Maximized to take half the viewport
- Repositioned (bottom-right default, draggable)

```
┌──────────────────────────────────────────────────────────┐
│  TreeCanvas (behavior tree editor)  │  Properties │ Viser│
│                                     │  Panel      │ Panel│
│                                     │             │      │
│                                     │             │      │
│                         ┌───────────┤             │      │
│                         │ Chat ≡  ─ │             │      │
│                         │ User: Add │             │      │
│                         │ bin pick..│             │      │
│                         │ Agent: .. │             │      │
│                         │ [Apply]   │             │      │
│                         ├───────────┤             │      │
│                         │ Type... ⏎ │             │      │
│                         └───────────┘             │      │
└──────────────────────────────────────────────────────────┘
```

**Implementation**: The Composer backend proxies chat to the OpenClaw agent
running in the NemoClaw sandbox. This gives the assistant full agent
capabilities — tool use, MCP access to ManyForge, multi-step reasoning,
session memory — rather than a simple LLM chat endpoint. The backend
manages session lifecycle and injects ManyForge context.

Two interaction surfaces:
- **Conversational**: "explain why the last cycle failed" → agent queries
  debug export, reasons, responds
- **Actionable**: "add a recovery fallback to the pick node" → agent
  proposes tree edit, user approves/dismisses in the UI

**LLM context**: The assistant must have deep knowledge of:
- Node catalog (all node types, their parameters, constraints,
  required capabilities)
- Skill catalog (pick_object, place_object, move_to_named_config — their
  pre/postconditions, parameter contracts, expansion rules)
- Composition patterns from LLM_GUIDE.md
- Current program state (loaded tree, deployment, capabilities)
- Scene state (objects, robot configuration)

This context is provided via OpenClaw's workspace knowledge system
(`AGENTS.md`, `SOUL.md`) and live queries through ManyForge MCP tools.

**Authority model**: Design-time only. The assistant proposes changes; the
human applies them via the existing PUT /api/program/tree flow. No special
authority needed.

**Model choice**: This does not require the largest model. A 27-35B model
(Qwen 3.5-27B or 35B-A3B) with the node catalog and LLM_GUIDE.md in
context is sufficient for tree construction assistance. Can share the same
vLLM instance used by NemoClaw-Thor.

---

## 3. Architecture

### 3.1 Architecture Decision: Option B (Separate Containers)

ManyForge and OpenClaw run in separate containers/processes, communicating
exclusively through HTTP APIs. This is the only architecture that maintains
the safety invariant: an LLM agent cannot directly affect robot motion
except through ManyForge's validated API.

**Why not inside the sandbox?** ManyForge needs:
- Host device access (USB/serial for robot hardware)
- GPU access for C++ motion planner (CUDA, not just inference)
- ROS2 DDS multicast for sensor discovery — netns blocks this
- Low-latency kernel tick loop (10 Hz) — sandbox overhead adds jitter

**Why not shared pod / sidecar?** Defeats sandbox isolation. If the LLM
agent escapes the sidecar, it has localhost access to the kernel.

**Why not ROS2 in the sandbox?** ROS2 DDS uses multicast UDP for
discovery, which netns blocks. Workarounds (Fast-DDS Discovery Server,
Zenoh RMW) add fragile complexity for no gain. The agent needs robot
state and sensor data, which ManyForge already exposes through its API
(`scene_state`, `kernel_snapshot`, `execution_status`). If raw ROS2 topics
are needed (point cloud, force/torque), a ManyForge bridge node subscribes
to topics and exposes them via API — keeping the data flow auditable.

### 3.2 Network Topology

```
┌──────────────────────────────────────────────────────────────────────┐
│  HOST (Jetson AGX Thor)                                              │
│                                                                      │
│  ┌─────────────────────┐     ┌────────────────────────────────────┐ │
│  │  vLLM (port 8000)   │     │  ManyForge Container               │ │
│  │  Qwen 3.5 / Gemma 4 │     │  (host network, device access)     │ │
│  │  Local inference     │     │                                    │ │
│  └────────┬────────────┘     │  Composer Backend  :9000            │ │
│           │                   │  ├── REST API (existing)           │ │
│           │                   │  ├── MCP Server (HTTP/SSE, OP-8)   │ │
│           │                   │  ├── /api/assistant/chat (proxy)   │ │
│           │                   │  └── /api/infra/status (health)    │ │
│           │                   │  Viser HMI         :8081           │ │
│           │                   │  Kernel (C++ / Mock / CUDA)        │ │
│           │                   │  ROS2 nodes (sensors, actuators)   │ │
│           │                   │  EventRecorder (audit trail)       │ │
│           │                   └───────┬────────────────────────────┘ │
│           │                           │                              │
│  ┌────────┴───────────────────────────┴──────────────────────────┐  │
│  │  OpenShell (K3s)                                              │  │
│  │  Provider: vllm-local → host:8000/v1 (direct) or             │  │
│  │                       → host:8888/v1 (ManyForge mux mode)    │  │
│  │  Policy:   manyforge  → host:9000 (REST/MCP)                 │  │
│  │            viser-hmi  → host:8081 (WebSocket, snapshots)      │  │
│  │                                                               │  │
│  │  ┌─────────────────────────────────────────────────────────┐ │  │
│  │  │  Sandbox Pod (Landlock + seccomp + netns)               │ │  │
│  │  │                                                         │ │  │
│  │  │  OpenClaw Agent (Pi runtime)                             │ │  │
│  │  │    ├── ManyForge MCP tools (12+ tools, SSE/HTTP)        │ │  │
│  │  │    ├── Bash/Read/Write (workspace ops only)             │ │  │
│  │  │    ├── Gateway (Telegram/Slack/WhatsApp for escalation) │ │  │
│  │  │    └── Domain knowledge (AGENTS.md, SOUL.md)            │ │  │
│  │  │                                                         │ │  │
│  │  │  Agent capabilities:                                    │ │  │
│  │  │    CAN:  reason, plan, propose, query ManyForge API     │ │  │
│  │  │    CAN:  call vLLM for inference                        │ │  │
│  │  │    CAN:  escalate to human via Gateway                  │ │  │
│  │  │    CANNOT: access host filesystem (Landlock)            │ │  │
│  │  │    CANNOT: open arbitrary sockets (netns + policy)      │ │  │
│  │  │    CANNOT: reach ROS2 DDS (multicast blocked)           │ │  │
│  │  │    CANNOT: access USB/serial devices                    │ │  │
│  │  │    CANNOT: escalate privileges (seccomp)                │ │  │
│  │  └─────────────────────────────────────────────────────────┘ │  │
│  └───────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

### 3.3 MCP Connectivity

**Decision: Strategy A (workspace plugin over HTTP MCP transport) for the
current OpenClaw build. Direct native `mcp.servers` SSE remains a longer-term
target when the runtime actually supports it.**

ManyForge's MCP server runs on `/api/mcp` (POST, JSON-RPC) and
`/api/mcp/sse` (GET, Server-Sent Events). The MCP server shares the same
in-process ComposerState — no subprocess needed. Session management,
principal propagation, and auth are implemented.

In the currently installed OpenClaw runtime, the embedded main agent does
not accept a root-level `mcp.servers` block in `openclaw.json`. The verified
main-agent path is:

- workspace plugin under `/sandbox/.openclaw/workspace/.openclaw/extensions`
- `plugins.allow += ["manyforge-mcp"]`
- `plugins.entries.manyforge-mcp.enabled = true`
- `tools.alsoAllow += ["manyforge-mcp"]`
- plugin endpoint `https://inference.local/v1/completions`
- provider behind `inference.local` pointed at `http://host.openshell.internal:8888/v1`

The host mux on `:8888` then forwards:
- normal OpenAI traffic -> vLLM on `:8000`
- ManyForge-marked MCP traffic -> ManyForge `/api/mcp`

The older direct-config idea is kept here only as the eventual simpler target:
```json
{
  "mcp": {
    "servers": {
      "manyforge": {
        "url": "http://host.openshell.internal:9000/api/mcp/sse",
        "headers": {
          "Authorization": "Bearer ${MANYFORGE_API_TOKEN}",
          "X-Principal-ID": "llm:openclaw-agent"
        }
      }
    }
  }
}
```

That direct native MCP registration is not the current production baseline for
this build. The current baseline is workspace-plugin registration exposing
`manyforge_*` tools directly to the embedded agent.

### 3.3.1 Current Baseline vs. Long-Term Industrial Target

The currently validated baseline is:

- sandbox/OpenClaw client base URL: `https://inference.local/v1`
- OpenShell provider target:
  - direct mode -> `http://host.openshell.internal:8000/v1`
  - ManyForge mode -> `http://host.openshell.internal:8888/v1`
- workspace plugin endpoint -> `https://inference.local/v1/completions`
- host mux on `:8888` forwarding:
  - normal inference -> vLLM on `:8000`
  - `x_manyforge` MCP traffic -> ManyForge `/api/mcp`

This baseline is now proven end to end, including a real embedded agent turn
that executes `manyforge_execution_status`. It is acceptable as the current
operational baseline, but it is **not** the final industrial topology.

The long-term industrial target is a split-path design:

- normal model inference remains on the direct vLLM path
- ManyForge tool transport gets its own dedicated route
- the mux is removed from the inference critical path

The preferred direction is:

- keep sandbox/OpenClaw client base URL on `https://inference.local/v1`
- keep OpenShell provider target on `http://host.openshell.internal:8000/v1`
- give ManyForge a dedicated routed endpoint of its own, ideally TLS on a
  proxy-allowed route that does not share the inference path
- repoint the ManyForge workspace plugin to that dedicated route

This is preferred over keeping the mux in place because:

1. inference latency and failure domains stay isolated from ManyForge tool
   transport
2. audit and service identity can be reasoned about per-route
3. the resulting deployment is simpler to operate and easier to certify in an
   industrial environment

Until that dedicated route exists and is validated from the actual sandbox
process, the mux remains the blessed baseline.

### 3.3.2 Dedicated ManyForge Route Design

This section fixes the target topology for Phase I.B. The goal is to remove
the ManyForge mux from the inference critical path while preserving the
currently validated workspace-plugin integration model.

#### Final Route Shape

The preferred industrial route is:

- normal model inference remains on `https://inference.local/v1`
- the OpenShell provider behind `inference.local` remains pointed at direct
  vLLM on `http://host.openshell.internal:8000/v1`
- ManyForge gets a dedicated HTTPS endpoint on a proxy-allowed host/port
  distinct from the inference route
- the plugin talks to ManyForge over `POST /api/mcp`
- `GET /api/health` and `GET /api/infra/status` may also be exposed on the
  same dedicated endpoint for operational checks

The preferred transport for the plugin is direct JSON-RPC over
`POST /api/mcp`, not SSE. SSE remains valuable for future native OpenClaw MCP
registration when the runtime supports it, but it is not required for the
productized plugin path.

#### Host-Side Topology

ManyForge itself should continue to serve its internal HTTP API on `:9000`.
The dedicated industrial route should be a narrow TLS front end that proxies
only the allowed endpoints to the ManyForge backend.

Preferred layout:

- ManyForge backend: `http://127.0.0.1:9000`
- dedicated ManyForge TLS front end: proxy-allowed HTTPS listener
- allowed upstream paths:
  - `/api/mcp`
  - `/api/mcp/sse`
  - `/api/health`
  - `/api/infra/status`
- disallowed from the sandbox on the dedicated route:
  - Composer write APIs unrelated to the plugin
  - arbitrary file access routes
  - any path not explicitly allowlisted

This can be implemented with a small reverse proxy sidecar or host service.
The important property is not the exact proxy product; it is that inference
traffic and ManyForge tool traffic become separate network paths.

#### TLS and Trust Model

For industrial use, the route should not rely on plain HTTP from the sandbox.

Baseline target:

- terminate TLS on the dedicated ManyForge front end
- use a pinned certificate or pinned local CA bundle distributed by the setup
  tooling
- fail closed on certificate mismatch

Preferred operational model:

- generate a dedicated ManyForge route certificate during setup
- install the corresponding trust anchor into the sandbox/plugin environment
- rotate that certificate intentionally as part of setup/update tooling

If certificate management proves too heavy for the first implementation,
pinning a self-signed certificate is acceptable as an intermediate hardening
step. Plain HTTP on the final direct route is not acceptable for the target
industrial topology.

#### Auth and Identity Model

The dedicated route should have a distinct service identity contract from the
human/operator-facing Composer APIs.

Required request properties:

- `Authorization: Bearer <manyforge_plugin_token>`
- `X-Principal-ID: llm:openclaw-agent` or another explicit service principal

Rules:

1. the plugin token is scoped to the dedicated ManyForge route and is not
   reused as a general operator token
2. the route only accepts the headers and methods needed for MCP and health
   checks
3. ManyForge continues to record principal identity and authority provenance in
   its event/audit model
4. the terminal-answer pattern for machine-consumed outcomes remains separate
   from route authorization; reporting tools do not grant authority

Future hardening such as mutual TLS may be added later, but bearer token plus
TLS plus explicit principal identity is sufficient for the first dedicated
route implementation.

#### OpenShell Policy Shape

Once the dedicated route exists, the preferred policy is:

- allow sandbox egress to the dedicated ManyForge TLS route with termination
- keep inference on the existing sanctioned `inference.local` path
- remove the requirement for sandbox access to the mux path in normal
  operation

The old mux-backed policy remains valid only as fallback/debug support until
the dedicated route is proven from the live sandbox process.

#### Plugin Behavior After Repoint

After the dedicated route is implemented:

- the workspace/plugin tool wrappers stop calling
  `https://inference.local/v1/completions` for ManyForge tools
- they call the dedicated ManyForge HTTPS `/api/mcp` route directly
- normal agent inference continues to use the ordinary OpenClaw/OpenShell
  provider path
- smoke tests requiring deterministic machine-readable outcomes should use the
  structured terminal-answer pattern rather than free-text assertions

This preserves the currently validated OpenClaw-side integration surface
(plugin tools) while removing the inference mux dependency.

#### Rollout Sequence

Implementation should proceed in this order:

1. add the dedicated TLS front end on the chosen proxy-allowed route
2. expose only `/api/mcp`, `/api/mcp/sse`, `/api/health`, and
   `/api/infra/status`
3. provision trust material and plugin token through setup tooling
4. update the plugin generator/deployment logic to point at the dedicated route
5. keep the mux baseline available as fallback during migration
6. verify from a real sandbox process that:
   - the plugin can call `manyforge_execution_status`
   - normal inference still uses direct `:8000`
   - the provider target no longer needs `:8888`
7. only after that, demote the mux from primary path

#### Success Condition For I.B.1

The route design is considered complete when:

- the final route shape is fixed to a dedicated proxy-allowed TLS route
- the auth and trust model are explicit
- the plugin transport target is explicit
- the OpenShell policy target is explicit
- I.B.2 can be implemented without further topology ambiguity

#### Current Platform Constraint

Live validation on OpenShell `0.0.26` shows that sandbox egress does not
currently provide a viable second HTTPS route to `host.openshell.internal`:

- `https://host.openshell.internal:9443/...` fails at the proxy layer with
  `HTTP/1.1 403 Forbidden` on the `CONNECT` tunnel even when the `9443`
  `tls: terminate` policy entry is present and loaded
- `https://host.openshell.internal:443/...` also fails with `CONNECT 403` even
  when the `443` `access: full` policy entry is present and loaded
- bypassing the proxy from inside the sandbox and connecting directly to
  `172.17.0.1:443` fails with `connection refused`

That means:

- host-side TLS front end + plugin direct transport support can be implemented
- but `host.openshell.internal` is not currently a viable final second route on
  this platform, even on `:443`
- the mux remains the only validated sandbox-to-ManyForge transport today
- the final industrial route now depends on either:
  - a platform-sanctioned second route from OpenShell, or
  - an alternative routable hostname/interface that the sandbox can actually
    reach outside the inference-local path

Until that route exists and is validated from the live sandbox process, the
mux remains the required primary transport.

### 3.4 OpenShell Policy

The sandbox is tight by default. Only the specific ports ManyForge needs
are opened:

```yaml
# policies/static/manyforge-services.yaml
name: manyforge_services
description: >
  Allow sandbox agents to reach ManyForge Composer API and Viser HMI.
  All robot-affecting actions are gated by ManyForge's authority model
  (velocity limits, collision monitoring, lease timeout, modification
  policy, authority provenance).
endpoints:
  - host: "host.openshell.internal"
    port: 9000
    protocol: rest
    description: "ManyForge Composer REST API + MCP server"
  - host: "host.openshell.internal"
    port: 8081
    protocol: websocket
    description: "ManyForge Viser 3D HMI (scene snapshots)"
```

The sandbox still cannot:
- Access host filesystem (Landlock)
- Open arbitrary network connections (netns + OpenShell policy)
- Escalate privileges (seccomp)
- Reach ROS2 DDS (multicast blocked by netns)
- Touch USB/serial devices (no device access in pod)

Applied during `manyforge-stack up --with-nemoclaw` provisioning.

### 3.5 Seamless Setup

A single entry point provisions both ManyForge and NemoClaw:

```bash
# Full stack — ManyForge + NemoClaw + sandbox + agent
manyforge-stack up --with-nemoclaw

# ManyForge only — fully deterministic, no LLM
manyforge-stack up

# Teardown
manyforge-stack down
```

`manyforge-stack up --with-nemoclaw` performs:

1. Start ManyForge container (Composer + Kernel + ROS2, host networking)
2. Verify vLLM is reachable on host:8000 (or start it)
3. Write OpenShell `manyforge-services.yaml` policy
4. Run `openshell policy apply`
5. Provision sandbox with ManyForge agent workspace:
   - Copy `AGENTS.md` (ManyForge domain knowledge)
   - Copy `SOUL.md` (industrial safety boundaries)
   - Generate/enable the ManyForge workspace plugin
   - Repoint the provider behind `inference.local` to `host:8888`
   - Install ManyForge AgentSkills (see Section 8)
6. Start OpenClaw agent in sandbox
7. Verify connectivity:
   - Sandbox → ManyForge MCP: tool discovery check
   - Sandbox → vLLM: model availability check
   - ManyForge → sandbox: health endpoint check
8. Report status (green/yellow/red for each component)

Configuration lives in a single file:

```yaml
# manyforge-stack.yaml
manyforge:
  port: 9000
  hmi_port: 8081
  catalog: manyforge_behavior/resources/node_catalog.yaml
  api_token: "${MANYFORGE_API_TOKEN}"

nemoclaw:
  enabled: true
  vllm_endpoint: "http://localhost:8000/v1"
  manyforge_mux_endpoint: "http://localhost:8888/v1"
  model_id: "qwen-3.5-27b-a3b"
  vision_model_id: "gemma-4-31b-it-nvfp4"  # optional
  sandbox_name: "manyforge-agent"
  agent_skills:
    - manyforge-monitor-execution
    - manyforge-diagnose-failure
    - manyforge-intervene-recovery

composer:
  assistant_enabled: true  # false for deterministic-only
  assistant_via: "sandbox"  # "sandbox" (OpenClaw) or "direct" (vLLM)
```

### 3.6 Infrastructure Health Monitoring

New Composer endpoints for monitoring the full stack:

```
GET /api/infra/status
```

Returns:
```json
{
  "manyforge": {
    "status": "healthy",
    "version": "0.9.0",
    "kernel": "connected",
    "ros2": "connected"
  },
  "vllm": {
    "status": "healthy",
    "endpoint": "http://localhost:8000/v1",
    "models_loaded": ["qwen-3.5-27b-a3b"],
    "gpu_memory_used_gb": 26.4
  },
  "sandbox": {
    "status": "healthy",
    "name": "manyforge-agent",
    "agent_state": "idle",
    "mcp_connected": true,
    "uptime_s": 3421
  }
}
```

When any component is unhealthy or absent, the status degrades gracefully:
- vLLM down → Modes A/B/C/D unavailable, deterministic execution unaffected
- Sandbox down → Modes A/C/D unavailable, Mode B (direct vLLM) still works
- ManyForge is always the authority — if it's down, everything stops

The Composer frontend shows a status indicator (top bar) with expandable
diagnostics on click.

### 3.7 Model Allocation

For concurrent operation of all four modes on a single Thor:

| Mode | Model | Sequences | Notes |
|------|-------|-----------|-------|
| Mode A (delegated recovery) | Qwen 3.5 27B-Distilled | 1-2 | Infrequent, on-demand |
| Mode B (per-cycle perception) | Gemma 4 31B (vision) | 1 | Per-cycle, sub-second |
| Mode C (supervisory operator) | Qwen 3.5 122B | 1 | Long-running session |
| Mode D (composer assistant) | Qwen 3.5 27B | 1 | On-demand, interactive |

The 122B MoE model (75 GB NVFP4) supports 8 concurrent sequences with 3 GB
KV cache each. A single vLLM instance can serve all four modes if the
models fit. For mixed-model operation, two vLLM instances may be needed
(one for the primary model, one for the VLM).

Alternatively, a single Qwen 3.5-35B-A3B (26 GB NVFP4, 26 concurrent
sequences) can serve all text-based modes with high concurrency, and a
separate Gemma 4 instance handles vision.

---

## 4. Authority and Safety Model

### 4.1 Two-Layer Safety Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Layer 1: Sandbox Isolation (prevents bypass)           │
│                                                         │
│  Agent CANNOT reach the robot except through             │
│  ManyForge's API. Enforced by:                          │
│  - Landlock (filesystem isolation)                      │
│  - seccomp (syscall filtering)                          │
│  - netns + OpenShell policy (network isolation)         │
│  - No device access in pod                              │
│                                                         │
│  Only exits: host:9000 (ManyForge), host:8000 (vLLM),  │
│              host:8081 (Viser, optional)                 │
└────────────────────────┬────────────────────────────────┘
                         │ HTTP only
┌────────────────────────▼────────────────────────────────┐
│  Layer 2: ManyForge Authority Model (prevents misuse)   │
│                                                         │
│  Every agent action is validated:                       │
│  - Authority provenance (who, why, what policy)        │
│  - Modification policy (which params are mutable)      │
│  - Lease timeout (agent can't hold control forever)    │
│  - Scope constraints (delegation contract limits)      │
│  - Velocity limits (kernel-enforced, non-bypassable)   │
│  - Collision monitoring (kernel, always active)        │
│  - Event recording (full audit trail)                  │
│                                                         │
│  Only this layer touches the robot.                     │
└─────────────────────────────────────────────────────────┘
```

### 4.2 Authority Provenance Matrix

| Mode | session_kind | authority_source | approval_mode | trigger_reason |
|------|-------------|-----------------|---------------|----------------|
| A (delegated) | delegated_recovery | program_node | policy_granted | planning_failed, etc. |
| B (perception) | — (no session) | — | — | — (regular node) |
| C (supervisor) from STOPPED | manual_intervention | llm | not_required | monitoring_anomaly |
| C (supervisor) from IDLE | manual_intervention | llm | policy_granted | operational_decision |
| D (assistant) | — (no session) | — | — | — (design-time only) |

### 4.3 Safety Invariants

These invariants must hold regardless of what the LLM decides:

1. **Velocity limits**: The kernel enforces velocity_scale bounds. The LLM
   cannot exceed them even via runtime overrides (the kernel validates).

2. **Collision monitoring**: The kernel's collision check tick runs at the
   configured cadence regardless of LLM decisions. A collision detection
   stops motion immediately — no LLM approval needed.

3. **Lease timeout**: Every intervention session has a lease. If the LLM
   stops responding (inference failure, sandbox crash, network partition),
   the lease expires and the system transitions to STOPPED. It never
   auto-resumes.

4. **Scope constraints**: Mode A (delegated recovery) must declare allowed
   actions. The agent cannot exceed the scope defined by the tree author.
   Mode C (supervisor) has broader scope but still cannot bypass the kernel's
   safety checks.

5. **Saved artifact immutability**: No mode silently modifies the saved
   program YAML. Runtime overrides are tracked separately. The operator can
   always reload the original program.

6. **Graceful degradation**: If any LLM component fails (vLLM down, sandbox
   crash, network partition), ManyForge continues in deterministic mode.
   `query_model` nodes return FAILURE (handled by fallback logic in the
   tree). `delegate_to_llm` nodes time out (lease expiry → STOPPED).
   The Composer assistant becomes unavailable but the tree editor and
   execution controls remain functional.

### 4.4 OP-4 / OP-6 Requirements

Mode A requires implementing OP-6 (delegated recovery contracts):

- A `delegate_to_llm` node type in the catalog
- A delegation contract format specifying: allowed actions, scope, timeout,
  escalation policy
- CycleManager must accept `session_kind: "delegated_recovery"` when the
  request comes from a tree node that has the delegation contract

Mode C from IDLE requires relaxing OP-4 (authority gating):

- Define a `llm_operator` policy that authorizes specific agents
- The policy must be explicit and auditable (not a blanket bypass)
- Alternative: keep the IDLE gate and require the supervisor to always
  go through run → stop → intervene (more steps, but safe by default)

---

## 5. New Components

### 5.1 `delegate_to_llm` Catalog Node (Mode A)

A new behavior tree node type that delegates execution to an LLM agent.

```yaml
- id: delegate_to_llm
  name: Delegate to LLM
  category: integration
  description: >-
    Delegate a recovery or decision task to an LLM agent via the configured
    model endpoint. Transitions the cycle to DIRECT_CONTROL, calls the agent
    with failure context and recovery scope, waits for completion or lease
    expiry, and returns SUCCESS or FAILURE based on the agent's outcome.
  tags: [integration, llm, recovery, delegated]
  profiles:
    min_profile: v0.0
  kind: action
  children:
    min: 0
    max: 0
  parameters:
    - name: endpoint
      type: string
      required: true
      description: >-
        URL of the model endpoint (OpenAI-compatible). Example:
        http://host.openshell.internal:8000/v1
    - name: model
      type: string
      required: true
      description: Model ID to use for the agent call.
    - name: system_prompt
      type: string
      required: false
      description: >-
        System prompt for the agent. If omitted, a default ManyForge-aware
        prompt is used that includes node catalog, current tree, and scene
        state.
    - name: trigger_reason
      type: string
      required: true
      description: >-
        Why this delegation is happening. Passed to the agent as context.
        Examples: planning_failed, validation_failed, timeout,
        scene_mismatch, confidence_low.
    - name: allowed_actions
      type: string_list
      required: false
      default: [scene_edit, runtime_override, intervention_subtree]
      description: >-
        Actions the agent is allowed to take. Restricts which MCP tools the
        agent can call.
    - name: lease_timeout_ms
      type: integer
      required: false
      default: 120000
      description: Maximum time for the agent to complete recovery.
    - name: promote_keys
      type: string_list
      required: false
      description: >-
        Blackboard keys to promote from the intervention scope to the parent
        scope on success.
  required_capabilities: []
  execution:
    blocking: true
    cancellable: true
```

### 5.2 `query_model` Catalog Node (Mode B)

**Implemented in OP-7.** The `QueryModelNode` exists with:
- Non-blocking execution via ThreadPoolExecutor
- Deployment-side prompt templates with `{{variable}}` substitution
- Template variable resolution from blackboard keys
- JSON Schema validation of model responses
- Confidence gating (`min_confidence` threshold)
- Model audit event recording with redaction
- Catalog entry in `node_catalog.yaml`
- Optional live scene snapshot attachment via the HMI render path
- Mirrored structured output fields under `{output_key_prefix}/fields/...`
  so downstream tree nodes can branch on validated model outputs
- Fenced-JSON tolerant parsing for models that wrap valid JSON in harmless
  markdown code fences
- Runtime `${bb:...}` binding for `move_manipulator_action.pose_goal` and
  `move_manipulator_action.relative_move`, so validated VLM pose outputs can
  flow directly into motion execution without a custom adapter layer

### 5.3 Composer Chat Backend (Mode D)

New REST endpoints for the Composer assistant, proxying to the OpenClaw
agent in the sandbox:

```
POST /api/assistant/chat     — send message, receive streamed response
GET  /api/assistant/sessions — list active conversations
DELETE /api/assistant/sessions/{id} — end conversation
POST /api/assistant/actions/{id}/apply — apply a proposed tree change
POST /api/assistant/actions/{id}/dismiss — dismiss a proposal
```

The backend:
1. Maintains a conversation history per session
2. Proxies messages to the OpenClaw agent via its API (the agent has
   ManyForge MCP tools and domain knowledge already)
3. Parses structured actions from the agent response (tree proposals,
   validation requests, parameter suggestions)
4. Returns the response with optional tree proposals that the frontend can
   render as diffs
5. SSE streaming for real-time response delivery

When `assistant_via: "direct"` in config, the backend calls vLLM directly
with injected ManyForge context (catalog, current tree, scene state) as
system prompt. This is simpler but less capable (no tool use, no multi-step
reasoning).

### 5.4 Composer Chat Frontend Panel

A resizable, minimizable chat overlay in the Composer UI:

- **Resize**: drag edges, minimum 300px x 200px
- **Minimize**: collapse to a rail button with unread indicator
- **Maximize**: half-viewport width
- **Reposition**: draggable, bottom-right default
- **Connection indicator**: shows sandbox/LLM health status
- **Tree proposals**: rendered inline with Apply/Dismiss buttons
- **Manual trigger**: "Run monitoring cycle" button for Mode C ad-hoc use

### 5.5 Infrastructure Status Panel

A top-bar status indicator in the Composer UI showing:

```
[● ManyForge] [● vLLM] [● Sandbox]
```

Click expands to show diagnostics: model loaded, GPU usage, sandbox uptime,
MCP connection status, last agent activity. When a component is missing
(e.g., no NemoClaw configured), the indicator shows "—" (not red — absent
is not an error in deterministic mode).

---

## 6. Implementation Phases

### Phase I: Foundation (2-3 weeks)

Establish the infrastructure that all four modes depend on.

| # | Deliverable | Status | Work |
|---|-------------|--------|------|
| I.1 | OpenShell policy for ManyForge | PARTIAL | Policy fragments and merge flow exist. Direct host reachability is still not the blessed baseline, so final policy shape remains tied to the dedicated-route work in Phase I.B. |
| I.2 | ManyForge SSE MCP transport | **DONE** (OP-8) | `/api/mcp/sse` + `/api/mcp` endpoints, session management, auth, principal propagation |
| I.3 | ManyForge tool exposure to OpenClaw main agent | **DONE** (baseline) | Workspace plugin + `tools.alsoAllow` over the muxed `inference.local` path is validated. Native `mcp.servers` remains a future simplification only after the runtime supports it. |
| I.4 | Agent domain knowledge | **DONE** (baseline) | `AGENTS.md`, `SOUL.md`, and ManyForge-specific skills are generated and deployed into the sandbox workspace. |
| I.5 | Connectivity verification | **DONE** (baseline) | Real embedded agent turns can enumerate `manyforge_*` tools and execute `manyforge_execution_status`. Full run/load/stop smoke coverage still belongs in I.B and later phases. |
| I.6 | Seamless setup tooling | **DONE** (baseline) | `configure-manyforge-provider.sh`, `manyforge-stack.sh`, plugin generation, and setup docs exist. Productized packaging and smoke automation move to Phase I.B. |
| I.7 | Infrastructure health endpoint | **DONE** (baseline) | `GET /api/infra/status` exists in ManyForge; follow-on operational checks still need to be wired into the full stack smoke path. |

**Exit criteria (baseline)**: the current stack can provision the workspace
plugin + mux baseline, an embedded OpenClaw agent can discover `manyforge_*`
tools, and at least one real ManyForge tool call succeeds from the embedded
agent in the sandbox.

### Phase I.B: Industrial Transport Hardening (next)

Move from the validated mux baseline to the long-term industrial topology.

| # | Deliverable | Status | Work |
|---|-------------|--------|------|
| I.B.1 | Dedicated ManyForge route design | **DONE** (design) | Final target documented in Section 3.3.2: dedicated proxy-allowed TLS route, direct JSON-RPC plugin transport to `/api/mcp`, direct inference preserved on `:8000`, explicit auth/trust/policy model. |
| I.B.2 | Dedicated ManyForge route implementation | PARTIAL | Host-side TLS front end, plugin direct-route transport, token/CA provisioning, and fallback logic are implemented. Live sandbox validation shows `host.openshell.internal` is not currently a viable second route on this OpenShell stack: `:9443` fails with proxy `CONNECT 403`, `:443` with `access: full` also fails with proxy `CONNECT 403`, and direct no-proxy TCP to `172.17.0.1:443` is refused. |
| I.B.3 | Plugin repoint to dedicated route | TODO | Change the workspace plugin so its endpoint is the dedicated ManyForge route rather than `https://inference.local/v1/completions`. |
| I.B.4 | Restore direct inference in ManyForge mode | TODO | Keep the sandbox/OpenClaw client on `https://inference.local/v1`, but restore the OpenShell provider target to direct vLLM on `http://host.openshell.internal:8000/v1` even when ManyForge integration is enabled. |
| I.B.5 | Plugin productization | **DONE** (baseline) | `build-manyforge-openclaw-plugin.py` now produces a versioned/pinned plugin artifact plus sha/metadata, and `configure-manyforge-provider.sh` installs it into OpenClaw global extensions using `OPENCLAW_STATE_DIR=/sandbox/.openclaw-data` with a writable install-time config path. Runtime origin is now `global`; workspace fallback remains only as a compatibility path if install fails on another stack. |
| I.B.6 | End-to-end smoke tests and operational checks | **DONE** (baseline) | `manyforge-openclaw-smoke.sh` and `manyforge-stack.sh smoke` now validate direct mode and mux mode end to end: provider target, sandbox base URL, pinned plugin version, ManyForge health through `inference.local`, one real embedded `manyforge_*` tool call, and a structured terminal answer instead of free-text success. |
| I.B.7 | Mux retirement | DEFERRED (upstream dependency) | The mux cannot leave the primary path on OpenShell `0.0.26` because no second sandbox-routable route has been validated and the embedded agent still needs the current plugin/MCP workaround surface. Keep mux as the production baseline until the upstream platform exposes the required capabilities. See `PLATFORM_REQUIREMENTS.md`. |

**Acceptance criteria (industrial target)**:

1. normal inference traffic no longer traverses the ManyForge mux
2. ManyForge tool traffic uses a dedicated route distinct from the inference
   route
3. an embedded OpenClaw agent can call `manyforge_*` tools with the provider
   target still set to direct vLLM on `:8000`
4. the plugin is deployed from a versioned/pinned artifact rather than ad hoc
   workspace-local code
5. a single smoke command proves direct mode and ManyForge mode end to end

**Current baseline after I.B.5/I.B.6**:

- plugin build/provenance is versioned and pinned, and runtime loading now uses
  an installed plugin from `~/.openclaw-data/extensions/manyforge-mcp`
- the installer path requires `OPENCLAW_STATE_DIR=/sandbox/.openclaw-data` and a
  writable install-time `OPENCLAW_CONFIG_PATH`; `configure-manyforge-provider.sh`
  handles that and then merges the install provenance back into the runtime
  config managed under `/sandbox/.openclaw/openclaw.json`
- plugin lifecycle is operationalized on the host:
  - versioned archives in `~/.manyforge/plugin-artifacts/`
  - `manyforge-mcp-current.*` tracks the deployed artifact
  - deploy/rollback surface via `manyforge-stack.sh plugin ...`
- smoke automation is in place and should be treated as the operational gate for
  this platform baseline
- mux smoke validates the current deployed plugin artifact by default, so
  rollback candidates can be verified without silently replacing them with the
  newest build
- the structured terminal-answer pattern is implemented in the plugin surface
  and should be used for machine-consumed smoke/supervisory outcomes
- smoke runs now persist machine-readable state under `~/.manyforge/smoke/`
  and `/sandbox/.manyforge/smoke/` so operational status can report the last
  validated baseline
- mux retirement is not an active local engineering item on this platform
  baseline; it is deferred to upstream OpenShell/OpenClaw capability work as
  captured in `PLATFORM_REQUIREMENTS.md`

**Fallback rule**: the muxed baseline remains supported until all I.B
acceptance criteria are met from the live sandbox process, not just from host
probes.

### Phase II: Composer Assistant — Mode D (2-3 weeks)

The lowest-risk mode. Design-time only, no robot motion, highest user value
for program construction.

Current baseline before Phase II work:
- Composer already exposes deployment model endpoint aliases, prompt templates,
  live `/v1/models` discovery, and deployment-side endpoint editing.
- `query_model` node fields now show resolved read-only info when there is only
  one valid deployment alias/template instead of pretending there is a choice.
- The assistant itself is still not implemented; only the supporting model
  configuration surfaces are now in place.

| # | Deliverable | Work |
|---|-------------|------|
| II.1 | Assistant chat backend | `POST /api/assistant/chat` endpoint proxying to OpenClaw agent in sandbox, conversation management, SSE streaming response. Fallback: direct vLLM with context injection. |
| II.2 | Assistant chat frontend | React overlay panel: resizable, minimizable, draggable. Message list, input field, connection indicator, conversation reset. |
| II.3 | Tree proposal integration | When the assistant proposes a tree modification, render it as a visual diff in the TreeCanvas. On "Apply", call `PUT /api/program/tree`. On "Dismiss", discard. |
| II.4 | Assistant model endpoint configuration | Build on the current Composer/deployment model-endpoint surfaces. Add explicit assistant configuration (`assistant_enabled`, `assistant_via`, assistant endpoint alias/model) without conflating assistant model choice with per-node `query_model` endpoint selection. |
| II.5 | System prompt / AGENTS.md engineering | Develop and test the agent workspace knowledge that gives the assistant deep ManyForge understanding. Validate on tree construction tasks: build a pick-and-place, add a fallback, parameterize for runtime override, etc. |
| II.6 | Manual agent trigger | "Run once" button in chat panel that sends a one-shot request to the sandbox agent (e.g., "analyze current tree", "check execution status"). Basis for Mode C manual trigger. |

**Exit criteria**: A user can open the Composer UI, type "build a
pick-and-place program for the graspable object", and receive a valid,
applicable tree proposal that passes validation. The chat overlay is
resizable and minimizable.

### Phase III: Per-Cycle Perception — Mode B (2-3 weeks)

Enable model-in-the-loop execution for variable-environment tasks.

| # | Deliverable | Status | Work |
|---|-------------|--------|------|
| III.1 | `query_model` node implementation | **DONE** (OP-7) | `QueryModelNode` with ThreadPoolExecutor, schema validation, confidence gating, audit events, deployment-bound prompt templates, and response-tool support for model families that behave more reliably through tool calls than through direct JSON formatting |
| III.2 | `query_model` catalog entry | **DONE** (OP-7) | Entry in `node_catalog.yaml` |
| III.3 | Scene snapshot for VLM | **DONE** | In-process HMI render provider wired into `QueryModelNode`, OpenAI-compatible image attachment support, `include_scene_snapshot` + width/height/format parameters |
| III.4 | Example: vision-guided perception-to-motion | **PARTIAL / INFRASTRUCTURE DEMO** | `examples/vision_guided_bin_pick.*` covers the recommended constrained pattern: VLM selects between fixed targets and deterministic motion/skills execute the pick. `examples/vision_guided_pose_pick.*` remains only as a plumbing demonstration that validated model outputs can flow through `${bb:...}` into motion nodes; it is not a recommended grasp-planning direction. |
| III.5 | Tests | **DONE** | Unit coverage expanded for scene snapshots, multimodal request shape, structured field mirroring, blackboard-to-motion pose binding, shipped example execution, and an opt-in live vLLM vision test against the local OpenAI-compatible endpoint |
| III.6 | Deterministic pick-planning redesign | **IN PROGRESS** | The direction is now fixed in code and plan: the model performs semantic selection only, while deterministic backend planning owns approach/pick pose synthesis and preview validation. The old pose-emission path remains scaffold only. |
| III.7 | Scene/object source abstraction | **PARTIAL** | Initial contract is implemented: `plan_pick_sequence` supports `object_source=backend_scene` and `image_source=none|hmi_snapshot`. External detector / ROS2 / blackboard-fed sources remain future work. |
| III.8 | Deterministic pick-plan node/service | **PARTIAL** | `plan_pick_sequence` now exists as a planning-only runtime node. It reads a selected object, bounded XYZ/RPY envelopes, previews the pick-only sequence `current -> approach -> pick -> approach` through backend planning, and writes a structured pick plan (`selected_object_id`, `approach_pose`, `pick_pose`, `attempted_candidates`, `planning_score`, `outcome`) to the blackboard. Place logic is intentionally out of scope for this node. The current implementation uses one fixed top-down planning convention internally. |
| III.9 | Example: object-type selection + deterministic pick plan | **DONE (initial baseline)** | `examples/semantic_shape_pick.*` and `scripts/demo-shape-pick.sh` now demonstrate the intended architecture: the model performs bounded object selection through the `select_scene_object` response tool, the backend computes the actual pick-only sequence deterministically, and any later placement logic remains ordinary downstream tree logic rather than part of the planner contract. The demo currently defaults to text-only selection context; image input remains disabled by default pending better multimodal reliability. |
| III.10 | Transition cleanup | **PLANNED** | Keep the current pose-pick implementation only as temporary scaffold while the deterministic replacement is developed. Once the new path lands, remove or refactor the out-of-spec model-to-tool-pose example/prompt/demo surfaces. |

**Exit criteria**: A bin-picking program runs multiple cycles, each time
calling the configured selection model to choose a target and route into the
corresponding pick sequence. For the industrial path, the model is limited to bounded semantic
selection or bounded approach preferences, while the backend deterministically
computes and validates the actual approach/grasp plan before execution. Direct
VLM-computed grasp-pose injection is treated only as an infrastructure demo,
not as a production direction.

### Phase III.A: Industrial Pick-Planning Hardening

Direct "infer final grasp pose from an RGB snapshot" is useful only as a stress
test for the `query_model` plumbing, but it is not a credible production
contract and should not be pursued as the main grasp-planning direction. For
industrial use, the LLM should not own final motion-authority values. The
preferred direction is:

1. Retrieve scene context from configured sources.
   - Optional image source: HMI/Viser snapshot initially; later camera,
     simulator, ROS2, or blackboard image source.
   - Mandatory object snapshot: initially privileged backend object state;
     later blackboard snapshots from external detectors such as FoundationPose.
   - Robot/tool context: active TCP/tool frame plus allowed offset/orientation
     envelopes.
2. Let the model perform bounded semantic work only.
   - Select object ID or object class.
   - Optionally select bounded approach family / preference ordering.
   - Never emit unconstrained final TCP poses as the preferred production
     path.
3. Use deterministic backend planning for all numeric motion authority.
   - Generate candidate approach/grasp poses from object pose, object size,
     tool frame, and configured approach envelopes.
   - Plan the sequence without moving the robot.
   - Evaluate a two-step sequence where the pick-leg starts from the approach
     leg's terminal state.
   - Return the first feasible or best feasible plan according to a declared
     search strategy.

Current implementation baseline:

- `plan_pick_sequence` is implemented in the behavior layer and backed by a
  native `preview_motion` path in the RoboPlan bridge.
- The initial supported contract is intentionally narrow:
  - object source: `backend_scene`
  - image source: `none` or `hmi_snapshot`
  - search strategies: `first_feasible_grid`, `best_cost_grid`
- The node writes a structured pick plan to the blackboard and does not move
  the robot itself.
- The semantic demo path is `semantic_shape_pick.*`, where the model chooses
  the requested object class through a deterministic response-tool contract and
  the backend owns numeric planning.
- The demo currently defaults to text-only selection context because the
  multimodal path was less reliable on the currently exercised local models.
  Re-enabling image input remains an explicit open investigation item, not the
  default path.

This direction is specified in
`manyforge_specs/docs/spec/475-perception-guided-pick-planning.md`.

Transition rule:

- The existing `vision_guided_pose_pick.*` example and related model-to-tool-pose
  scaffolding may remain in the repo temporarily so the integration plumbing is
  not lost during the redesign.
- That scaffold is not a supported production workflow and must not be treated
  as the target contract.
- When the deterministic pick-planning path is implemented, the out-of-spec
  pieces of the current scaffold should be refactored or removed.

### Future Investigation: Learned Pose/Policy Models

This work is explicitly **future investigation**, not the current production
direction.

- **3D pose-estimation models** such as FoundationPose are the more credible
  learned path for recovering object pose/alignment from sensor data.
- **VLA-style policy motion generation** may be worth exploring later as a
  separate execution mode with its own authority, safety, and validation
  contract.

Neither of these should be conflated with the current `query_model` contract.
The current production direction remains: bounded semantic selection from the
model, deterministic pose synthesis and planning in the backend.

### Phase IV: Supervisory Operator — Mode C (3-4 weeks)

The autonomous monitoring and intervention agent.

| # | Deliverable | Work |
|---|-------------|------|
| IV.1 | Supervisory agent skills | OpenClaw AgentSkills for ManyForge (see Section 8): `manyforge-monitor-execution`, `manyforge-diagnose-failure`, `manyforge-intervene-recovery`, `manyforge-adapt-program`, `manyforge-escalate-to-human`. Published to ClawHub. |
| IV.2 | Monitoring loop pattern | Validated agent prompt implementing load → run → poll → detect → decide → act cycle. Test with induced failures (remove object, inject planning timeout). |
| IV.3 | Authority model for LLM supervisor | Decide on IDLE gating: keep run→stop→intervene or implement `llm_operator` policy. If the latter, implement OP-4 relaxation with explicit policy authorization. |
| IV.4 | Session persistence | Extend `manyforge-stack` to restore agent state and ManyForge traces across restarts. OpenClaw JSONL transcripts + ManyForge event history. |
| IV.5 | Escalation path | Structured diagnostic → OpenClaw Gateway messaging (Telegram/Slack/etc). Composer UI notification panel for in-app escalation visibility. |
| IV.6 | End-to-end integration test | Automated: start stack → agent loads program → runs → induced failure → agent detects → recovers → completes. Full audit trail verification. |

**Exit criteria**: An OpenClaw agent autonomously supervises a pick-and-place
execution, detects an induced failure (object removed from scene), stops
the cycle, diagnoses the issue, applies a scene edit to restore the object,
and resumes execution to completion — with full traceability in ManyForge's
event stream.

### Phase V: Delegated Recovery — Mode A (3-4 weeks)

The deepest integration — behavior tree nodes that trigger LLM agents.

| # | Deliverable | Work |
|---|-------------|------|
| V.1 | Delegation contract format | Spec for the `delegate_to_llm` node's contract: allowed actions, scope, timeout, escalation policy. Integrates with OP-6 `recovery_policy`. |
| V.2 | `delegate_to_llm` node implementation | New py_trees action node. Transition to DIRECT_CONTROL, agent invocation via sandbox, scope enforcement, lease management, blackboard promotion, return to tree. |
| V.3 | `delegate_to_llm` catalog entry | Add to `node_catalog.yaml`. |
| V.4 | OP-6 completion | `delegated_recovery` session kind accepted in CycleManager when the request carries a valid delegation contract from a tree node. |
| V.5 | Scope enforcement | Authorization middleware in ManyForge that checks the active delegation contract before allowing each MCP tool call. Unauthorized tool calls rejected with clear error. |
| V.6 | Recovery skill integration | `manyforge-delegated-recovery` AgentSkill (see Section 8) that the `delegate_to_llm` node invokes. Deterministic workflow with model reasoning. |
| V.7 | Example: pick-and-place with fallback delegation | A program that attempts pick_object, falls back to delegate_to_llm on planning failure, agent resolves obstruction. |
| V.8 | Tests | Unit tests with mocked agent. Integration test with real agent and induced failures. |

**Exit criteria**: A behavior tree with a `delegate_to_llm` fallback node
encounters a planning failure, delegates to an LLM agent, the agent
resolves the issue (e.g., adjusts the approach pose via runtime override),
and the tree continues to successful completion.

---

## 7. Observability and Traceability

Every LLM interaction that affects robot behavior must be recorded.

### 7.1 Event Types

| Event Type | Mode | Status | Payload |
|------------|------|--------|---------|
| `model_call_started` | B | **DONE** (OP-9) | endpoint, model, prompt_hash, template_id |
| `model_call_completed` | B | **DONE** (OP-9) | response_hash, confidence, latency_ms, token_counts |
| `model_call_failed` | B | **DONE** (OP-9) | error, latency_ms |
| `delegation_started` | A | TODO | trigger_reason, allowed_actions, lease_timeout_ms |
| `delegation_completed` | A | TODO | outcome, actions_taken, duration_ms |
| `delegation_failed` | A | TODO | error, actions_attempted |
| `delegation_expired` | A | TODO | actions_taken_before_expiry |
| `supervisor_intervention_started` | C | TODO | anomaly_type, diagnosis_summary |
| `supervisor_intervention_completed` | C | TODO | resolution, actions_taken |
| `supervisor_escalated` | C | TODO | diagnosis, recommendation |
| `assistant_chat_message` | D | TODO | conversation_id, message_hash, has_proposal |
| `assistant_proposal_applied` | D | TODO | conversation_id, tree_diff_hash |

### 7.2 Debug Export Extension

`GET /api/debug/export` already includes `modelEvents` (OP-9). Extend with:

```json
{
  "modelEvents": [...],
  "delegations": [...],
  "supervisorActions": [...]
}
```

### 7.3 Prompt and Response Logging

**Implemented in OP-9**: `ModelAuditPolicy` controls redaction. By default:
- Prompts: only template_id + hash stored (not raw text)
- Responses: only validated/structured fields + hash (not raw text)
- Arguments: only key names + value hashes (not raw values)

Full prompt/response logging can be enabled per-deployment by setting
`store_full_prompts: true` and `store_full_responses: true` in the
deployment YAML's `model_audit_policy`.

---

## 8. OpenClaw AgentSkills for ManyForge

OpenClaw skills are deterministic, versioned artifacts (SKILL.md with YAML
frontmatter) that encode multi-step workflows. They are not self-improving
— each skill is a fixed procedure that the agent follows. This makes them
auditable and reproducible.

ManyForge needs a set of purpose-built skills that the OpenClaw agent
uses to interact with ManyForge reliably. Without these skills, the agent
would need to reason from scratch about ManyForge's API semantics on every
interaction — slower, less reliable, and harder to audit.

### 8.1 Skill: `manyforge-monitor-execution`

**Purpose**: Structured monitoring loop for autonomous supervision (Mode C).

```markdown
---
name: manyforge-monitor-execution
version: 1.0.0
description: >
  Monitor a ManyForge execution session. Poll execution digest at a
  configurable interval, detect anomalies, and report status changes.
  Returns structured monitoring result with any detected issues.
parameters:
  poll_interval_s:
    type: number
    default: 3.0
    description: Seconds between status polls
  max_duration_s:
    type: number
    default: 300.0
    description: Maximum monitoring duration before returning
  anomaly_thresholds:
    type: object
    default:
      stall_timeout_s: 30
      error_count_limit: 3
      cycle_time_deviation_factor: 2.0
    description: Thresholds for anomaly detection
tags: [manyforge, monitoring, supervision]
---

# Monitor ManyForge Execution

## Steps

1. Call `mcp_manyforge_execution_status` with `include: "digest"`.
2. Record baseline: cycle count, state, timestamp.
3. Loop every `poll_interval_s` seconds:
   a. Call `mcp_manyforge_execution_status` with `include: "digest"`.
   b. Compare against baseline and previous poll:
      - State changed? Record transition.
      - Cycle count unchanged for > `stall_timeout_s`? Flag STALL.
      - Error count increased? Flag ERROR.
      - Cycle time > baseline * `cycle_time_deviation_factor`? Flag SLOW.
      - State is STOPPED and was RUNNING? Flag UNEXPECTED_STOP.
   c. If any flag is set, return immediately with the anomaly report.
   d. If `max_duration_s` exceeded, return with "monitoring_complete".
4. Return structured result:
   ```json
   {
     "status": "anomaly_detected" | "monitoring_complete",
     "anomalies": [{"type": "STALL", "details": "..."}],
     "cycles_observed": 42,
     "duration_s": 126.3
   }
   ```

## Error handling

- If ManyForge is unreachable, retry 3 times with 2s backoff.
- If retries exhausted, return `{"status": "connection_lost"}`.
- Never modify ManyForge state — this skill is read-only.
```

### 8.2 Skill: `manyforge-diagnose-failure`

**Purpose**: Structured failure diagnosis (Mode C, Mode A).

```markdown
---
name: manyforge-diagnose-failure
version: 1.0.0
description: >
  Diagnose a ManyForge execution failure. Collects debug export, execution
  history, scene state, and node statuses to produce a structured diagnosis.
parameters:
  run_id:
    type: string
    required: false
    description: Specific run ID to diagnose (default: latest)
tags: [manyforge, diagnosis, recovery]
---

# Diagnose ManyForge Failure

## Steps

1. Call `mcp_manyforge_debug_export` to get the full diagnostic bundle.
2. Extract from the export:
   a. `executionDigest.state` — current cycle state
   b. `executionHistory` — sequence of node executions, find the failed node
   c. `nodeStatuses` — current status of all tree nodes
   d. `sceneState` — current scene objects and robot configuration
   e. `blackboard` — current blackboard state
   f. `modelEvents` — any model calls that preceded the failure
3. Identify the root cause:
   a. Which node failed? What was its status message?
   b. Was it a planning failure (object unreachable, collision)?
   c. Was it a precondition failure (missing object, wrong config)?
   d. Was it a timeout (model call, motion execution)?
   e. Was it a scene mismatch (expected vs actual)?
4. Return structured diagnosis:
   ```json
   {
     "failed_node": "pick_object_1",
     "failure_type": "planning_failed",
     "root_cause": "Object 'part_0042' is behind collision object 'wall_1'",
     "scene_context": {"object_pose": [...], "obstacles": [...]},
     "suggested_actions": [
       {"action": "scene_edit", "description": "Remove obstacle or reposition object"},
       {"action": "runtime_override", "description": "Change approach direction"}
     ],
     "confidence": 0.85
   }
   ```

## Error handling

- If debug export fails, fall back to individual API calls
  (execution_status, scene_state, kernel_snapshot).
- Never modify ManyForge state — this skill is read-only.
```

### 8.3 Skill: `manyforge-intervene-recovery`

**Purpose**: Execute a recovery intervention (Mode C).

```markdown
---
name: manyforge-intervene-recovery
version: 1.0.0
description: >
  Execute a recovery intervention on ManyForge based on a diagnosis.
  Applies scene edits, runtime overrides, or intervention subtrees,
  then resumes execution. Requires ManyForge to be in STOPPED state.
parameters:
  diagnosis:
    type: object
    required: true
    description: Structured diagnosis from manyforge-diagnose-failure
  allowed_actions:
    type: string_list
    default: [scene_edit, runtime_override]
    description: Actions this recovery is allowed to take
  auto_resume:
    type: boolean
    default: false
    description: If true, resume execution after recovery. If false, leave in STOPPED.
tags: [manyforge, recovery, intervention]
---

# Intervene and Recover

## Preconditions

- ManyForge must be in STOPPED state.
- A diagnosis object must be provided.

## Steps

1. Verify ManyForge state is STOPPED:
   - Call `mcp_manyforge_execution_status`.
   - If not STOPPED, abort with error.

2. Based on diagnosis.suggested_actions, execute recovery:

   **If scene_edit is needed and allowed:**
   a. Call `mcp_manyforge_scene_state` to get current scene.
   b. Determine required edits (move object, remove obstacle, add object).
   c. Call `mcp_manyforge_scene_edit` with the edit payload.
   d. Call `mcp_manyforge_scene_state` again to verify the edit took effect.

   **If runtime_override is needed and allowed:**
   a. Determine which node parameters to override.
   b. Call `mcp_manyforge_runtime_override` with action="apply",
      node_name, params, authority_source="llm".
   c. Verify override was accepted (check response).

   **If intervention_subtree is needed and allowed:**
   a. Build a recovery subtree (e.g., move to safe position first).
   b. Call `mcp_manyforge_intervention` with action="execute",
      the subtree, session_kind="manual_intervention",
      authority_source="llm".
   c. Monitor intervention via `mcp_manyforge_intervention(action="status")`
      until complete.

3. If auto_resume:
   a. Call `mcp_manyforge_cycle_control(command="reset")`.
   b. Call `mcp_manyforge_cycle_control(command="run")`.

4. Return recovery result:
   ```json
   {
     "status": "recovered" | "partially_recovered" | "failed",
     "actions_taken": [{"type": "scene_edit", "details": "..."}],
     "resumed": true
   }
   ```

## Error handling

- If any action fails, do NOT continue with remaining actions.
- Return with status "failed" and the error details.
- Never leave ManyForge in an inconsistent state — if a scene edit
  succeeds but a subsequent override fails, log both results.
```

### 8.4 Skill: `manyforge-escalate-to-human`

**Purpose**: Structured escalation when autonomous recovery fails (Mode C).

```markdown
---
name: manyforge-escalate-to-human
version: 1.0.0
description: >
  Escalate a ManyForge issue to a human operator via the configured
  messaging platform. Includes structured diagnosis, recovery attempts,
  and recommended actions.
parameters:
  diagnosis:
    type: object
    required: true
    description: Structured diagnosis from manyforge-diagnose-failure
  recovery_attempts:
    type: array
    default: []
    description: Previous recovery attempts and their outcomes
  urgency:
    type: string
    default: normal
    enum: [low, normal, high, critical]
    description: Urgency level for the escalation
tags: [manyforge, escalation, operator]
---

# Escalate to Human Operator

## Steps

1. Format a structured escalation message:
   ```
   ⚠️ ManyForge Escalation [{urgency}]

   System: {program_name} on {deployment_name}
   State: STOPPED (was RUNNING)
   Failed node: {diagnosis.failed_node}
   Failure: {diagnosis.failure_type}

   Root cause: {diagnosis.root_cause}

   Recovery attempted:
   {for each attempt: - {action}: {outcome}}

   Recommended actions:
   {for each suggestion: - {description}}

   Reply with:
   • "retry" — retry the failed operation
   • "skip" — skip this cycle and continue
   • "stop" — keep stopped, I'll investigate
   • Or describe a custom action
   ```

2. Send the message via the configured platform (the OpenClaw Gateway
   handles platform routing — Telegram, Slack, WhatsApp, etc.).

3. Wait for operator response (with configurable timeout, default 30 min).

4. Parse operator response and return:
   ```json
   {
     "operator_decision": "retry" | "skip" | "stop" | "custom",
     "custom_instruction": "Move the obstacle to the left",
     "response_time_s": 45.2,
     "operator_id": "human:alice"
   }
   ```

## Error handling

- If the messaging platform is unreachable, retry 3 times.
- If operator doesn't respond within timeout, return
  `{"operator_decision": "timeout"}`.
- The agent should NOT take autonomous action after escalation timeout
  unless explicitly authorized.
```

### 8.5 Skill: `manyforge-delegated-recovery`

**Purpose**: Full recovery workflow invoked by `delegate_to_llm` node
(Mode A). Combines diagnosis + intervention in a single skill.

```markdown
---
name: manyforge-delegated-recovery
version: 1.0.0
description: >
  Complete delegated recovery workflow for a failed behavior tree node.
  Diagnoses the failure, attempts recovery within the allowed scope,
  and reports the outcome. Designed to be invoked by the delegate_to_llm
  behavior tree node.
parameters:
  trigger_reason:
    type: string
    required: true
    description: Why this delegation was triggered (from RecoveryTrigger enum)
  failed_node:
    type: string
    required: true
    description: Instance name of the failed node
  allowed_actions:
    type: string_list
    required: true
    description: Actions this recovery is allowed to take
  lease_timeout_ms:
    type: integer
    default: 120000
    description: Maximum time for recovery
  promote_keys:
    type: string_list
    default: []
    description: Blackboard keys to promote on success
tags: [manyforge, recovery, delegated, tree-triggered]
---

# Delegated Recovery

## Preconditions

- ManyForge is in DIRECT_CONTROL state (set by delegate_to_llm node).
- The lease timeout is active — work must complete before it expires.

## Steps

1. **Diagnose** — run `manyforge-diagnose-failure` skill.
2. **Plan recovery** — based on diagnosis and allowed_actions:
   a. Filter suggested_actions by allowed_actions scope.
   b. If no allowed actions match, return FAILURE immediately.
   c. Rank remaining actions by likelihood of success.
3. **Execute recovery** — run `manyforge-intervene-recovery` skill
   with auto_resume=false (the delegate_to_llm node handles resumption).
4. **Verify** — call `mcp_manyforge_scene_state` and
   `mcp_manyforge_execution_status` to confirm recovery worked.
5. **Promote blackboard keys** if specified in promote_keys.
6. Return result:
   ```json
   {
     "status": "recovered" | "failed",
     "diagnosis": {...},
     "actions_taken": [...],
     "promoted_keys": [...],
     "duration_ms": 4200
   }
   ```

## Error handling

- Monitor elapsed time against lease_timeout_ms.
- If approaching timeout (>80% elapsed), abort current action and return.
- Never exceed the allowed_actions scope — unauthorized actions are
  not attempted, not even logged as "considered".
```

### 8.6 Skill Installation

Skills are installed in the sandbox workspace via `openclaw skills install`:

```bash
# During manyforge-stack provisioning
openclaw skills install manyforge-monitor-execution
openclaw skills install manyforge-diagnose-failure
openclaw skills install manyforge-intervene-recovery
openclaw skills install manyforge-escalate-to-human
openclaw skills install manyforge-delegated-recovery
```

Or published to ClawHub for cross-deployment reuse:

```bash
openclaw skills publish --hub manyforge-monitor-execution
```

---

## 9. Open Questions

### Q1: Single vLLM instance or multi-model serving?

Thor can run one large model (122B) or multiple smaller models concurrently.
The choice affects all four modes:
- Single 122B: best reasoning, but no vision, limited concurrency
- 35B + Gemma 4: text + vision, high concurrency, weaker reasoning
- Dynamic switching: swap models between production cycles (downtime during
  switch)

**Decision needed before Phase III.**

### Q2: Scope enforcement mechanism for delegate_to_llm

How does the delegate_to_llm node restrict the agent's tool access?

**Decision: Authorization middleware in ManyForge.** When a `delegate_to_llm`
node activates, it registers a delegation contract with the MCP session.
The MCP transport layer checks each incoming tool call against the contract's
`allowed_actions` before forwarding to the tool handler. Unauthorized calls
are rejected with a JSON-RPC error (`-32600`, "Action not permitted by
delegation contract"). This is enforceable (not prompt-based), auditable
(logged in event stream), and does not require MCP protocol changes.

### Q3: IDLE → DIRECT_CONTROL for LLM supervisor

Keep the human-approval gate (safe, more steps for Mode C) or implement an
explicit `llm_operator` policy (OP-4 relaxation)?

**Decision needed before Phase IV.** Recommendation: keep the gate for now,
implement `llm_operator` policy as a Phase IV.3 deliverable if the
run→stop→intervene workflow proves too cumbersome.

### Q4: Composer assistant — where does the model run?

**Decision: User-configurable.** Default to sandbox (OpenClaw agent with
full tool access). Fallback to direct vLLM when sandbox is not available.
Configurable via `assistant_via` in `manyforge-stack.yaml`.

---

## 10. Fully Deterministic Mode

ManyForge must remain fully operational without any LLM dependency. This
section defines the deterministic-only configuration and guarantees.

### 10.1 What "Deterministic Mode" Means

A deployment where:
- No `query_model` nodes appear in any behavior tree
- No `delegate_to_llm` nodes appear in any behavior tree
- The Composer assistant is disabled (`assistant_enabled: false`)
- No NemoClaw/OpenClaw sandbox is provisioned
- No vLLM instance is required

The operator composes behavior trees manually in the Composer UI (tree
editor, property panel), runs them via the existing cycle controls, and
monitors execution through the Composer UI and Viser HMI.

### 10.2 Startup

```bash
# Deterministic-only — no LLM, no sandbox
manyforge-stack up

# Or directly
python -m manyforge_composer \
  --catalog-path manyforge_behavior/resources/node_catalog.yaml \
  --port 9000
```

No `--mcp-http`, no `--assistant-endpoint`, no NemoClaw configuration.

### 10.3 What Works Without LLM

| Feature | Available? | Notes |
|---------|-----------|-------|
| Composer REST API | Yes | Full API, all endpoints |
| Tree editor (frontend) | Yes | Manual composition |
| Property panel | Yes | Manual parameter editing |
| Viser 3D HMI | Yes | Scene visualization |
| Cycle control (run/stop/reset) | Yes | Manual operation |
| Execution history | Yes | EventRecorder, debug export |
| MCP server (stdio) | Yes | For Claude Code or other MCP clients |
| MCP server (HTTP/SSE) | Optional | Enabled via `--mcp-http` flag |
| Runtime overrides | Yes | Manual parameter patching |
| Intervention mode | Yes | Manual intervention subtrees |
| Recovery nodes | Yes | FallbackOverrideNode, RecoverySkillNode (deterministic) |
| Skill expansion | Yes | pick_object, place_object, etc. |
| Program validation | Yes | Full tree validation |
| Chat assistant | **No** | Requires LLM |
| `query_model` nodes | **No** | Requires vLLM endpoint |
| `delegate_to_llm` nodes | **No** | Requires sandbox + LLM |
| Autonomous supervision | **No** | Requires sandbox + LLM |
| Infrastructure status | Partial | Shows ManyForge only, vLLM/sandbox show "—" |

### 10.4 Validation

When `query_model` or `delegate_to_llm` nodes are present in a tree but
no model endpoint is configured, `POST /api/program/validate` returns a
warning (not an error):

```json
{
  "valid": true,
  "warnings": [
    {
      "node": "perceive_bin",
      "type": "query_model",
      "message": "No model endpoint configured. This node will return FAILURE at runtime."
    }
  ]
}
```

At runtime, `query_model` returns FAILURE immediately when no endpoint is
configured (the node's `_endpoint` is None → FAILURE on first tick). The
tree's fallback logic handles this like any other node failure.

### 10.5 Remaining Gaps To Close

The deterministic / llm-free architecture is correct, but enforcement is not
fully finished yet. The following gaps remain and should be tracked as
implementation work:

1. **Catalog/UI gating**: `query_model` is still exposed as a normal catalog
   node even when no model endpoints are configured. The Composer now exposes
   deployment model endpoints, prompt templates, and resolved single-option
   fields more coherently, but it should still hide or clearly disable LLM-only
   nodes in deterministic deployments.
2. **Validation context**: tree validation must always run with the loaded
   deployment context so missing `model_endpoint_alias` / `prompt_template_id`
   is surfaced during authoring, not only at runtime.
3. **Capability expression**: model-facing nodes still have no dedicated
   capability keys, and deployments still lack explicit model-endpoint
   capability flags such as response-tool support, image-input support, and
   JSON-object support. Today this remains inferred operationally instead of
   declared deterministically.
4. **Explicit mode surface**: the backend/UI still lacks an explicit
   deterministic vs LLM-enabled mode switch that disables assistant/model
   affordances by configuration rather than by convention.
5. **Assistant configuration**: deployment-side model endpoint editing now
   exists in Composer, but assistant-specific endpoint/model configuration is
   still not implemented and should remain distinct from per-node
   `query_model` alias selection.

Runtime fail-closed behavior is already in place, but the authoring-time
experience should be tightened so deterministic deployments cannot drift into
partially configured LLM usage.

### 10.6 Upgrade Path

Moving from deterministic to LLM-assisted is additive:

1. Install vLLM and load a model
2. Add `model_endpoints` and `prompt_templates` to the deployment YAML
3. Add `query_model` nodes to the tree where perception is needed
4. Optionally: provision NemoClaw sandbox for Modes A/C/D

No existing functionality breaks. No existing configuration changes. The
deterministic tree continues to work exactly as before — LLM nodes are
additions, not replacements.

### 10.7 Downgrade Path

Removing LLM integration is also clean:

1. Remove `query_model` and `delegate_to_llm` nodes from trees
2. Stop NemoClaw sandbox and vLLM
3. Run `manyforge-stack up` (without `--with-nemoclaw`)

Or: keep the nodes in the tree but don't configure endpoints. They return
FAILURE, fallback logic handles it, deterministic execution continues.

---

## 11. Success Criteria

The integration is complete when:

1. An OpenClaw agent can autonomously supervise a ManyForge execution
   session: load, run, monitor, detect failures, recover, and resume —
   with full traceability in ManyForge's event stream. (Mode C)

2. A behavior tree can include a `query_model` node that calls a VLM to
   determine per-cycle targets, and the robot executes the VLM-determined
   motion plan reliably across multiple cycles. (Mode B)

3. A behavior tree can include a `delegate_to_llm` fallback node that
   activates an LLM agent for recovery when deterministic fallback logic
   is insufficient, with scope enforcement and lease management. (Mode A)

4. A human operator can use the Composer UI chat overlay to interactively
   construct valid behavior trees with an LLM assistant that has full
   ManyForge domain knowledge and tool access. (Mode D)

5. All LLM interactions that affect robot behavior are recorded with full
   authority provenance and are queryable via the existing execution
   history, debug export, and event stream surfaces.

6. No LLM interaction bypasses the kernel's safety invariants (velocity
   limits, collision monitoring, lease timeout).

7. The saved program artifact is never silently modified by any LLM
   interaction. Runtime overrides and intervention subtrees remain
   distinguishable from persisted edits.

8. ManyForge is fully operational in deterministic mode without any LLM
   dependency. All LLM integration points are opt-in. Startup without
   NemoClaw/vLLM does not degrade existing functionality.

9. `manyforge-stack up --with-nemoclaw` provisions the full stack (ManyForge
   + NemoClaw + sandbox + agent + skills) in a single command. The
   Composer UI shows health status for all components.

10. OpenClaw AgentSkills for ManyForge are published, versioned, and
    deterministic. The agent follows structured workflows (monitor →
    diagnose → intervene → escalate) rather than reasoning from scratch
    on every interaction.
