# ManyForge runtime-agent reference

This is a downstream operator template, not repository development guidance.
For repository changes follow the root AGENTS.md and its development references.

The optional MCP HTTP adapter uses the current tool schemas published by
`tools/list` and generated in `docs/reference/MCP_RUNTIME_CONTRACT.md`.
It is disabled unless Composer starts with `--mcp-http`. Historical v0 tool
signatures and the old CycleManager are not compatibility interfaces.
OpenClaw/Hermes packaging and provider qualification are separate gates;
this template does not start or qualify either provider.

## Authority and commands

- Kernel owns motion, admission, collision checks and Stop. Program Runner
  owns cycle state, Resetting and recovery offers. Scene, Manager and alarms
  retain their own owners. Composer owns the editable Draft, not execution.
- Read `manyforge_cycle_control` with `{"action":"state"}`. Send an explicitly
  authorized command with `{"action":"command","body":<current REST DTO>}`.
  Use the exact current owner identities, freshness envelope and caller
  attribution required by that DTO. Never fabricate or bypass them.
- Submit once. Observe backend state to learn the outcome. A timeout or lost
  tool result is not proof of rejection; do not repeat a motion command.
  A receipt is not proof of physical rest. Stop/Full Stop still use the
  backend's reserved path, not a tree flag or browser state machine.
- Reset owns its stop-and-restore transaction. Do not invent Idle, Running,
  Stopped or successful recovery from a disconnected or incomplete response.
- Do not Apply, Run, Reset, clear alarms, alter scene collision geometry or
  save/overwrite artifacts unless the user has authorized that action.

## Authoring and diagnosis examples

- Read Draft: `manyforge_program_tree` with `{"action":"read"}`.
- Validate without changing Draft: `manyforge_program_validate` with
  `{"action":"source","body":{"authoring":<portable authoring forest>}}`.
- Browse allowed artifact roots: `manyforge_files` with `{"action":"browse"}`.
- Discover build-owned skills: `manyforge_skills` with `{"action":"catalog"}`.
  `action: "expand"` takes the current environment digest, typed arguments,
  explicit role maps and declaration-only `program_context`. It returns an
  ordinary subtree and provenance without editing Draft or executing it.
  Insert through the normal authorized Draft workflow, then validate and
  compile the Program Binding. A logical-gripper template is not a physical
  gripper command or confirmation.
- Read an exact event: `manyforge_debug_export` with
  `{"action":"event","path":{"sequence":"123"}}`.
- Export the recorded incident trace: `manyforge_debug_export` with
  `{"action":"export","query":{"program_run_id":"<exact run>"}}`.
  Missing/truncated evidence is not evidence that nothing happened.
- Read Program recorder health: `manyforge_debug_export` with
  `{"action":"program_recorder"}`. Captures occur at recorded transitions,
  not every tick; values carry their producer clock and omission/loss counts.
- Recover only through a currently offered backend action and its exact
  confirmation/fencing contract. Never invent a recovery skill or assume
  gripper command completion proves a grasp.

Actions route through the existing REST handlers. Read their live schemas;
do not translate older examples by guessing fields or silently selecting a
robot. A tool name does not prove a provider or historical workflow is ready.
The historical skill recipes remain non-operative until their owning
functional/provider gate is qualified; do not invoke their v0 signatures.
