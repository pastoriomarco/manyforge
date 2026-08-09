# manyforge_assistant_bridge

This package provides the current bounded direct/`nemoclaw` assistant-provider
bridge. It mediates between Composer's strict v1 provider envelope, the active
headless assistant-mode projection, Composer Draft callbacks, owner-neutral
headless reads, and one configured OpenAI-compatible model endpoint.

The bridge:

- independently verifies the active retained mode and request narrowing before
  every model turn;
- exposes only the exact owner-issued tool schemas;
- executes read-only owner routes and revision-fenced Composer Draft callbacks;
- returns proposal/runtime descriptors for review without executing them;
- owns bounded request reconciliation, conversation history, cancellation,
  stuck-loop handling, and redacted rotating audit records; and
- never owns runtime state, catalog meaning, saved artifacts, Apply, or robot
  effects.

Run `python -m manyforge_assistant_bridge.bridge`. The service reads
`MANYFORGE_ASSISTANT_UPSTREAM_URL`, `MANYFORGE_HEADLESS_API_URL`,
`MANYFORGE_COMPOSER_URL`, and optional
`MANYFORGE_ASSISTANT_UPSTREAM_TOKEN`/`MANYFORGE_ASSISTANT_AUDIT_LOG` settings.
Composer must be started with `--headless-api-url` and
`--assistant-bridge-url` to expose the operator conversation surface.

`protocol.py` exports the strict current wire and independent projection
verification helpers. `headless.py` retains the closed owner-neutral H0 read
client. `supervisor.py` retains only a pure availability reducer: its former
fixed launcher/publication loop stays fenced because the Component Manager is
the sole process-convergence owner.

The complete pre-relocation interaction reference remains pinned at ManyForge
commit `08222188293df8e12f90dc72e204c21f22f92ea1` (tree
`bc749b9879d3c8f36fe7bf3036f9cc1f2ef1e60f`). It is acceptance input, not a
runnable compatibility fallback.
