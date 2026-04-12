# OpenClaw Integration Plan for ManyForge

**Goal**: Integrate NemoClaw/OpenClaw as ManyForge's LLM orchestration layer,
enabling autonomous agent-driven supervision, per-cycle perception-driven
decision-making, delegated failure recovery, and interactive program
composition assistance — while keeping deterministic behavior tree execution
as the authority for robot motion.

**Date**: 2026-04-11

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
- **MCP server** — 12 tool interface for LLM agents (stdio transport)
- **EventRecorder** — execution history with ring buffer + JSONL file sink
- **Intervention mode** — temporary subtree execution without modifying the
  saved program, with authority provenance, blackboard scoping, lease
  management, and promotion
- **Runtime overrides** — per-node parameter patching with active/staged
  lifecycle and modification_policy enforcement
- **Skill catalog** — `pick_object`, `place_object`, `move_to_named_config`
  with pre/postconditions, capability requirements, and parameter contracts

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

The current integration is ad-hoc and offline:

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

5. **Sandboxing is a feature, not overhead.** In an industrial environment,
   an LLM agent that can execute arbitrary code must be isolated. OpenShell's
   Landlock/seccomp/netns sandbox provides this. The agent communicates with
   ManyForge only through explicitly allowed API surfaces.

---

## 2. Integration Modes

The integration defines four operational modes, each serving a distinct
industrial need. They share infrastructure but differ in trigger, authority,
and latency requirements.

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
      - move_manipulator_action reads grasp_pose from blackboard
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

### Mode D: Composer Assistant (Design-Time — Interactive)

**Use case**: A human operator uses a chat interface in the Composer UI to
interactively construct, debug, and refine behavior trees with an LLM that
deeply understands ManyForge's node catalog, skill system, program structure,
and composition rules.

**Trigger**: User types in the Composer's prompt panel.

**Flow**:
```
User opens Composer UI (localhost:9000)
  → Prompt panel visible alongside the tree editor and property panel
  → User types: "Add a bin picking sequence that uses vision to find parts"
  → LLM responds with:
      - Explanation of the approach
      - Proposed tree structure (rendered as a preview diff in the editor)
      - Parameter suggestions with rationale
  → User reviews, modifies, accepts or rejects
  → On accept: tree is applied to the editor canvas
  → User can continue: "Make the approach height configurable as a
    runtime override" → LLM adjusts modification_policy + params
  → User validates: "Check if this tree is valid" → LLM calls
    POST /api/program/validate and explains any issues
```

**LLM context**: The assistant must have deep knowledge of:
- Node catalog (all 19 node types, their parameters, constraints,
  required capabilities)
- Skill catalog (pick_object, place_object, move_to_named_config — their
  pre/postconditions, parameter contracts, expansion rules)
- Composition patterns from LLM_GUIDE.md
- Current program state (loaded tree, deployment, capabilities)
- Scene state (objects, robot configuration)

**Implementation options**:

- **Option 1 (Backend-served, recommended)**: New REST endpoint
  `POST /api/assistant/chat` that wraps an LLM call with ManyForge context
  injected as system prompt. The frontend sends user messages; the backend
  appends catalog, current tree, scene state, and composition rules as
  context, calls the LLM, and returns the response with optional structured
  actions (tree diffs, validation requests).

- **Option 2 (OpenClaw as MCP server)**: OpenClaw can run as an MCP server
  (`openclaw mcp serve`). The Composer backend connects to this MCP server,
  giving the assistant access to OpenClaw's full tool suite (ManyForge MCP
  tools, file read/write, workspace knowledge) while the Composer backend
  handles context injection and UI integration. More powerful than Option 1
  (agent can validate, load examples, read docs) but introduces an
  OpenClaw dependency for the Composer assistant surface.

- **Option 3 (Frontend-only)**: The React frontend calls an OpenAI-
  compatible endpoint directly with a static system prompt. Simplest, but
  the LLM has no access to live ManyForge state.

**Authority model**: Design-time only. The assistant proposes changes; the
human applies them via the existing PUT /api/program/tree flow. No special
authority needed.

**Model choice**: This does not require the largest model. A 27-35B model
(Qwen 3.5-27B or 35B-A3B) with the node catalog and LLM_GUIDE.md in
context is sufficient for tree construction assistance. Can share the same
vLLM instance used by NemoClaw-Thor.

---

## 3. Infrastructure

### 3.1 Network Topology

```
┌────────────────────────────────────────────────────────────────────┐
│  HOST (Jetson AGX Thor)                                            │
│                                                                    │
│  ┌─────────────────────┐     ┌──────────────────────────────────┐ │
│  │  vLLM (port 8000)   │     │  ManyForge Composer              │ │
│  │  Qwen 3.5 / Gemma 4 │     │  REST API      :9000             │ │
│  │  Local inference     │     │  Viser HMI     :8081             │ │
│  └────────┬────────────┘     │  MCP (stdio)                     │ │
│           │                   │  Kernel (C++ / Mock)             │ │
│           │                   └───────┬──────────────────────────┘ │
│           │                           │                            │
│  ┌────────┴───────────────────────────┴────────────────────────┐  │
│  │  OpenShell (K3s)                                            │  │
│  │  Provider: vllm-local → host:8000/v1                       │  │
│  │  Policy:   manyforge → host:9000, host:8081 (REST/WS)     │  │
│  │                                                             │  │
│  │  ┌───────────────────────────────────────────────────────┐ │  │
│  │  │  Sandbox Pod                                          │ │  │
│  │  │                                                       │ │  │
│  │  │  OpenClaw Agent (Pi runtime)                           │ │  │
│  │  │    ├── ManyForge MCP tools (12 tools, SSE/HTTP)       │ │  │
│  │  │    ├── Bash/Read/Write (workspace ops)                │ │  │
│  │  │    └── Domain knowledge (AGENTS.md, LLM_GUIDE.md)     │ │  │
│  │  └───────────────────────────────────────────────────────┘ │  │
│  └─────────────────────────────────────────────────────────────┘  │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │  ManyForge Docker Container (optional, or bare-metal)       │  │
│  │  Kernel, Runner, CycleManager, EventRecorder               │  │
│  │  Binds 0.0.0.0:9000 and 0.0.0.0:8081                      │  │
│  └─────────────────────────────────────────────────────────────┘  │
└────────────────────────────────────────────────────────────────────┘
```

### 3.2 MCP Connectivity

ManyForge's MCP server currently runs over stdio only. OpenClaw's MCP
client natively supports stdio, SSE/HTTP, and streamable-HTTP transports —
so the work is on ManyForge's side to expose an HTTP-based MCP endpoint.

**Strategy A (SSE/HTTP MCP transport — recommended for production)**:

Add an SSE-based MCP transport to ManyForge's Composer backend:
```
GET /api/mcp/sse  →  Server-Sent Events MCP transport
POST /api/mcp     →  MCP message endpoint
```

The MCP server shares the same in-process ComposerState — no subprocess
needed. OpenShell policy allows the sandbox to reach host:9000. OpenClaw
registers ManyForge as an MCP server via its native config:
```json
{
  "mcp": {
    "servers": {
      "manyforge": {
        "url": "http://host.openshell.internal:9000/api/mcp/sse",
        "headers": {
          "Authorization": "Bearer ${MANYFORGE_API_TOKEN}"
        }
      }
    }
  }
}
```

OpenClaw's MCP client auto-discovers all ManyForge tools at startup and
namespaces them as `mcp_manyforge_<tool_name>` (e.g.,
`mcp_manyforge_scene_state`, `mcp_manyforge_cycle_control`).

**Strategy B (stdio via SSH — works today, fragile)**:

Register ManyForge as a stdio MCP server using SSH through the OpenShell
proxy. This requires ManyForge to be installed (or its Python path set)
in the sandbox, which is heavy:
```json
{
  "mcp": {
    "servers": {
      "manyforge": {
        "command": "ssh",
        "args": [
          "-o", "StrictHostKeyChecking=no",
          "-o", "UserKnownHostsFile=/dev/null",
          "-o", "LogLevel=ERROR",
          "-o", "ProxyCommand=openshell ssh-proxy --gateway-name nemoclaw --name <sandbox>",
          "sandbox@openshell-<sandbox>",
          "python -m manyforge_composer --mcp --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml"
        ]
      }
    }
  }
}
```

**Strategy C (REST via curl — fallback for prototyping)**:

The agent uses ManyForge's REST API directly via `curl` in Bash tool calls.
No MCP machinery needed. Lower ergonomics but zero infrastructure changes
on ManyForge's side. Viable for early prototyping before Strategy A is
implemented.

### 3.3 OpenShell Policy Extension

Add ManyForge service endpoints to the NemoClaw-Thor policy:

```yaml
# policies/static/manyforge-services.yaml
name: manyforge_services
description: Allow sandbox to reach ManyForge Composer and Viser HMI
endpoints:
  - host: "host.openshell.internal"
    port: 9000
    protocol: rest
    description: "ManyForge Composer REST API"
  - host: "host.openshell.internal"
    port: 8081
    protocol: rest
    description: "ManyForge Viser 3D HMI"
```

Applied during configure-local-provider.sh or via a separate setup script.

### 3.4 Model Allocation

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

### 4.1 Authority Provenance Matrix

| Mode | session_kind | authority_source | approval_mode | trigger_reason |
|------|-------------|-----------------|---------------|----------------|
| A (delegated) | delegated_recovery | program_node | policy_granted | planning_failed, etc. |
| B (perception) | — (no session) | — | — | — (regular node) |
| C (supervisor) from STOPPED | manual_intervention | llm | not_required | monitoring_anomaly |
| C (supervisor) from IDLE | manual_intervention | llm | policy_granted | operational_decision |
| D (assistant) | — (no session) | — | — | — (design-time only) |

### 4.2 Safety Invariants

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

### 4.3 OP-4 / OP-6 Requirements

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

A new behavior tree node that calls a model endpoint and writes structured
results to the blackboard.

```yaml
- id: query_model
  name: Query Model
  category: integration
  description: >-
    Call an LLM or VLM endpoint with a prompt and optional image, parse the
    structured response, and write results to blackboard keys. Designed for
    per-cycle perception and decision-making in variable environments.
  tags: [integration, llm, vlm, perception]
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
        URL of the model endpoint (OpenAI-compatible chat/completions).
    - name: model
      type: string
      required: true
      description: Model ID.
    - name: prompt_template
      type: string
      required: true
      description: >-
        Prompt template. Supports ${bb:key} for blackboard value
        substitution and ${scene:state} / ${scene:snapshot} for automatic
        scene context injection.
    - name: include_scene_snapshot
      type: boolean
      required: false
      default: false
      description: >-
        If true, capture a scene snapshot (JPEG) and include it as an image
        in the model request. Requires a VLM endpoint.
    - name: response_format
      type: string
      required: false
      default: json
      description: >-
        Expected response format. "json" parses the response as JSON and
        writes each top-level key to the blackboard. "text" writes the raw
        response to a single blackboard key.
    - name: output_key_prefix
      type: string
      required: false
      default: ""
      description: >-
        Prefix for blackboard keys written from the response. Example:
        "perception." writes keys as "perception.target_object_id".
    - name: min_confidence
      type: float
      required: false
      default: 0.0
      description: >-
        If the response includes a "confidence" field and its value is below
        this threshold, the node returns FAILURE.
    - name: timeout_ms
      type: integer
      required: false
      default: 5000
      description: Maximum time to wait for the model response.
  required_capabilities: []
  execution:
    blocking: true
    cancellable: true
```

### 5.3 Composer Chat Backend (Mode D)

New REST endpoint for the Composer assistant:

```
POST /api/assistant/chat
```

Request:
```json
{
  "message": "Add a bin picking loop with vision",
  "conversationId": "conv-001"
}
```

Response (streamed via SSE):
```json
{
  "role": "assistant",
  "content": "Here's a bin picking sequence that uses a VLM...",
  "actions": [
    {
      "type": "tree_proposal",
      "tree": { ... },
      "description": "Proposed tree modification"
    }
  ],
  "conversationId": "conv-001"
}
```

The backend:
1. Maintains a conversation history per `conversationId`
2. Injects as system context: node catalog, skill catalog, current tree,
   deployment capabilities, composition rules from LLM_GUIDE.md
3. Calls the configured model endpoint (vLLM on Thor or cloud)
4. Parses structured actions from the model response
5. Returns the response with optional tree proposals that the frontend can
   render as diffs

### 5.4 Composer Chat Frontend Panel

A new collapsible panel in the Composer UI, alongside the existing
PropertyPanel and ViserPanel:

```
┌──────────────────────────────────────────────────────────────┐
│  TreeCanvas (behavior tree editor)  │  Properties │  Viser  │
│                                     │  Panel      │  Panel  │
│                                     │             │         │
│                                     │             │         │
│─────────────────────────────────────┤             │         │
│  Assistant Chat Panel               │             │         │
│  ┌────────────────────────────────┐ │             │         │
│  │ User: Add bin picking with... │ │             │         │
│  │ Assistant: Here's a sequence  │ │             │         │
│  │ that uses query_model for...  │ │             │         │
│  │ [Apply to Tree] [Dismiss]     │ │             │         │
│  ├────────────────────────────────┤ │             │         │
│  │ Type a message...         [⏎] │ │             │         │
│  └────────────────────────────────┘ │             │         │
└──────────────────────────────────────────────────────────────┘
```

The panel is a bottom drawer that shares horizontal space with the
TreeCanvas, collapsible via a rail button like the existing panels.

---

## 6. Implementation Phases

### Phase I: Foundation (2-3 weeks)

Establish the infrastructure that all four modes depend on.

| # | Deliverable | Work |
|---|-------------|------|
| I.1 | OpenShell policy for ManyForge | Write `manyforge-services.yaml` policy, integrate into `configure-local-provider.sh` |
| I.2 | ManyForge SSE MCP transport | Add `/api/mcp/sse` + `/api/mcp` endpoints to the Composer backend, sharing the existing ComposerState |
| I.3 | ManyForge as OpenClaw MCP provider | Register ManyForge's SSE endpoint in OpenClaw's `mcp.servers` config (JSON format, `url` + `headers`), verify tool auto-discovery and namespaced invocation (`mcp_manyforge_*`) from sandbox |
| I.4 | Agent domain knowledge | Create `AGENTS.md` for the sandbox workspace with ManyForge domain knowledge (LLM_GUIDE.md content, MCP_SCHEMA.md content, composition rules, recovery patterns) and `SOUL.md` with industrial safety boundaries |
| I.5 | Connectivity verification | End-to-end test: OpenClaw agent in sandbox → ManyForge MCP tools → load program → read tree → run cycle → poll digest → stop |

**Exit criteria**: An OpenClaw agent in the NemoClaw sandbox can discover
all 12+ ManyForge MCP tools, load a program, and execute a complete
run/monitor/stop cycle.

### Phase II: Composer Assistant — Mode D (2-3 weeks)

The lowest-risk mode. Design-time only, no robot motion, highest user value
for program construction.

| # | Deliverable | Work |
|---|-------------|------|
| II.1 | Assistant chat backend | `POST /api/assistant/chat` endpoint with conversation management, context injection (catalog, skills, current tree, capabilities), model endpoint call, SSE streaming response |
| II.2 | Assistant chat frontend | React panel component in Composer UI: message list, input field, tree proposal rendering with Apply/Dismiss buttons, conversation reset |
| II.3 | Tree proposal integration | When the assistant proposes a tree modification, render it as a visual diff in the TreeCanvas. On "Apply", call `PUT /api/program/tree`. On "Dismiss", discard. |
| II.4 | Model endpoint configuration | Add `--assistant-endpoint` and `--assistant-model` CLI args to `manyforge_composer`. Default to the Thor vLLM endpoint. |
| II.5 | System prompt engineering | Develop and test the system prompt that gives the assistant deep ManyForge knowledge. Validate on tree construction tasks: build a pick-and-place, add a fallback, parameterize for runtime override, etc. |

**Exit criteria**: A user can open the Composer UI, type "build a
pick-and-place program for the graspable object", and receive a valid,
applicable tree proposal that passes validation.

### Phase III: Per-Cycle Perception — Mode B (2-3 weeks)

Enable model-in-the-loop execution for variable-environment tasks.

| # | Deliverable | Work |
|---|-------------|------|
| III.1 | `query_model` node implementation | New py_trees action node in `manyforge_behavior/nodes/integration.py`. Implements: HTTP call to model endpoint, prompt template rendering with blackboard/scene substitution, response parsing, blackboard write, confidence gating. |
| III.2 | `query_model` catalog entry | Add to `node_catalog.yaml` with full parameter schema. |
| III.3 | Scene snapshot for VLM | Ensure `GET /api/scene/snapshot` is callable from within the behavior tree tick context (in-process, no HTTP needed — direct call to ViserHmiServer.get_render()). |
| III.4 | Example: vision-guided bin picking | A program YAML that uses `query_model` with Gemma 4 to select a target object and compute a grasp pose, then executes a parameterized pick sequence. |
| III.5 | Tests | Unit tests with mocked model endpoint. Integration test with the real vLLM endpoint (Gemma 4 or Qwen) in the Docker container. |

**Exit criteria**: A bin-picking program runs multiple cycles, each time
calling Gemma 4 to select a target, and the robot picks the selected
object at the VLM-determined pose.

### Phase IV: Supervisory Operator — Mode C (3-4 weeks)

The autonomous monitoring and intervention agent.

| # | Deliverable | Work |
|---|-------------|------|
| IV.1 | Supervisory agent AGENTS.md and skills | OpenClaw `AGENTS.md` with ManyForge domain knowledge and AgentSkills-spec skills for: `monitor-execution`, `diagnose-failure`, `intervene-recovery`, `adapt-program`, `escalate-to-human`. Each skill is a deterministic, version-pinned artifact (SKILL.md with YAML frontmatter) encoding a multi-step workflow using ManyForge MCP tools. Skills should be publishable to ClawHub for reuse across deployments. |
| IV.2 | Monitoring loop pattern | Validated agent prompt that implements: load → run → poll → detect → decide → act cycle. Test with induced failures (remove an object mid-run, inject a planning timeout). |
| IV.3 | Authority model for LLM supervisor | Decide on IDLE gating: either keep the run→stop→intervene pattern, or implement a `llm_operator` policy. If the latter, implement OP-4 relaxation with explicit policy authorization. |
| IV.4 | Session persistence | Extend `manyforge-session-start.sh` to restore agent state and ManyForge traces. OpenClaw stores session transcripts as JSONL under `~/.openclaw/agents/<agentId>/sessions/`; checkpoint agent conversation at key decision points. |
| IV.5 | Escalation path | When the agent cannot resolve an issue, it produces a structured diagnostic report and sends it to the human operator via OpenClaw Gateway messaging (Telegram, Slack, WhatsApp, or configured platform). Operator responds through the same channel. Optionally also show agent diagnosis in a Composer UI notification panel. |
| IV.6 | End-to-end integration test | Automated test: start ManyForge → start OpenClaw agent → load pick-and-place → run → induce failure → agent detects → agent recovers → program completes successfully. |

**Exit criteria**: An OpenClaw agent autonomously supervises a pick-and-place
execution, detects an induced failure (object removed from scene), stops
the cycle, diagnoses the issue, applies a scene edit to restore the object,
and resumes execution to completion — with full traceability in ManyForge's
event stream.

### Phase V: Delegated Recovery — Mode A (3-4 weeks)

The deepest integration — behavior tree nodes that trigger LLM agents.

| # | Deliverable | Work |
|---|-------------|------|
| V.1 | Delegation contract format | Spec for the `delegate_to_llm` node's contract: allowed actions, scope, timeout, escalation policy. Integrate with OP-6. |
| V.2 | `delegate_to_llm` node implementation | New py_trees action node. Implements: transition to DIRECT_CONTROL, agent invocation (via configured endpoint), scope enforcement, lease management, blackboard promotion, return to tree. |
| V.3 | `delegate_to_llm` catalog entry | Add to `node_catalog.yaml`. |
| V.4 | OP-6 implementation | `delegated_recovery` session kind accepted in CycleManager when the request carries a valid delegation contract from a tree node. |
| V.5 | Scope enforcement | The delegate_to_llm node restricts which MCP tools the agent can call based on `allowed_actions`. Unauthorized tool calls are rejected with a clear error. |
| V.6 | Example: pick-and-place with fallback delegation | A program that attempts pick_object, falls back to delegate_to_llm on planning failure, and the agent reasons about and resolves the obstruction. |
| V.7 | Tests | Unit tests with mocked agent endpoint. Integration test with real agent and induced failures. |

**Exit criteria**: A behavior tree with a `delegate_to_llm` fallback node
encounters a planning failure, delegates to an LLM agent, the agent
resolves the issue (e.g., adjusts the approach pose via runtime override),
and the tree continues to successful completion.

---

## 7. Observability and Traceability

Every LLM interaction that affects robot behavior must be recorded.

### 7.1 Event Types

New event types in the EventRecorder:

| Event Type | Mode | Payload |
|------------|------|---------|
| `model_query_started` | B | endpoint, model, prompt_hash, include_snapshot |
| `model_query_completed` | B | response_keys, confidence, latency_ms |
| `model_query_failed` | B | error, latency_ms |
| `delegation_started` | A | trigger_reason, allowed_actions, lease_timeout_ms |
| `delegation_completed` | A | outcome, actions_taken, duration_ms |
| `delegation_failed` | A | error, actions_attempted |
| `delegation_expired` | A | actions_taken_before_expiry |
| `supervisor_intervention_started` | C | anomaly_type, diagnosis_summary |
| `supervisor_intervention_completed` | C | resolution, actions_taken |
| `supervisor_escalated` | C | diagnosis, recommendation |

### 7.2 Debug Export Extension

`GET /api/debug/export` extended with:

```json
{
  "model_queries": [
    {
      "timestamp_ns": ...,
      "node_instance": "perceive_bin",
      "endpoint": "http://host:8000/v1",
      "model": "gemma-4-31b-it-nvfp4",
      "prompt_hash": "sha256:abc...",
      "response_keys": ["target_object_id", "grasp_pose"],
      "confidence": 0.92,
      "latency_ms": 340
    }
  ],
  "delegations": [...],
  "supervisor_actions": [...]
}
```

### 7.3 Prompt and Response Logging

For auditability in industrial settings, model prompts and responses must be
logged to durable storage (JSONL trace sink). The event recorder writes:

- Full prompt text (or hash + reference to a prompt template)
- Full response text
- Parsed structured output
- Model ID, endpoint, latency, token counts
- Correlation: run_id, session_id, node_instance

Sensitive prompts (containing proprietary process parameters) should be
flagged and stored in a separate access-controlled log.

---

## 8. Open Questions

### Q1: Single vLLM instance or multi-model serving?

Thor can run one large model (122B) or multiple smaller models concurrently.
The choice affects all four modes:
- Single 122B: best reasoning, but no vision, limited concurrency
- 35B + Gemma 4: text + vision, high concurrency, weaker reasoning
- Dynamic switching: swap models between production cycles (downtime during
  switch)

**Decision needed before Phase III.**

### Q2: MCP transport for OpenClaw → ManyForge

OpenClaw's MCP client already supports SSE/HTTP and streamable-HTTP
transports natively. The remaining work is on ManyForge's side: add
`/api/mcp/sse` + `/api/mcp` endpoints to the Composer backend. This is
the recommended path (Strategy A). SSH-bridged stdio (Strategy B) is a
working fallback. REST via curl (Strategy C) is viable for early
prototyping.

**Decision: Strategy A (SSE/HTTP). Implementation in Phase I.2.**

### Q3: Scope enforcement mechanism for delegate_to_llm

How does the delegate_to_llm node restrict the agent's tool access?
Options:
- MCP tool filtering at the transport level (server only exposes allowed
  tools)
- Authorization middleware in ManyForge that checks the active delegation
  contract before allowing each tool call
- Trust-based: the agent's system prompt says "only use these tools" (not
  enforceable, not suitable for industrial use)

**Decision needed before Phase V.**

### Q4: Composer assistant — where does the model run?

Options:
- Same vLLM instance on Thor (shared with runtime modes)
- Separate cloud endpoint (e.g., Claude API) for better design-time
  reasoning
- User-configurable (support both)

**Decision needed in Phase II.**

### Q5: IDLE → DIRECT_CONTROL for LLM supervisor

Keep the human-approval gate (safe, more steps for Mode C) or implement an
explicit `llm_operator` policy (OP-4 relaxation)?

**Decision needed before Phase IV.**

---

## 9. Success Criteria

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

4. A human operator can use the Composer UI chat panel to interactively
   construct valid behavior trees with an LLM assistant that understands
   ManyForge's full node and skill catalog. (Mode D)

5. All LLM interactions that affect robot behavior are recorded with full
   authority provenance and are queryable via the existing execution
   history, debug export, and event stream surfaces.

6. No LLM interaction bypasses the kernel's safety invariants (velocity
   limits, collision monitoring, lease timeout).

7. The saved program artifact is never silently modified by any LLM
   interaction. Runtime overrides and intervention subtrees remain
   distinguishable from persisted edits.
