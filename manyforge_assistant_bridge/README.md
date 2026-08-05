# manyforge_assistant_bridge

The provider-backed assistant conversation and Program-authoring bridge remains
intentionally unavailable during the composable-cell runtime cutover. Running
`python -m manyforge_assistant_bridge.bridge` fails before starting a server
and returns exit status 78.

What remains current:

- `protocol.py`: deterministic catalog identity only. A hash is not proof of
  authenticity unless it is compared with an independently trusted value.
- `headless.py`: the closed H0 read client over independently owned health,
  runtime, Manager, scene, alarm, and historian projections. It has no
  mutation, callback, provider, conversation, or Program operation.
- `supervisor.py`: a transport-neutral availability reducer over
  already-observed facts. Endpoint probes, publication, polling, and shutdown
  effects were removed; assistant-product publication remains fenced until
  I6/N.
- `bridge.py`: the stable fail-before-effects provider/authoring entrypoint.
  It does not hide or replace the separately available H0 read client.

What must be rebuilt rather than revived:

- A later assistant-product gate: bounded conversation/session history, hard
  memory and turn/time limits, cancellation, argument/schema sanitation,
  stuck-loop handling, proposal-only/nonmutation enforcement, and bounded
  provider audit. H0 restores only the independently useful read client; it
  does not invent a provider contract merely to close I5.
- I6: Program/tree authoring context and tools on the new native addressing
  contract.

Do not copy the former v0 FastAPI envelope, Composer callback URLs, singleton
scene/Program mappings, import-time environment bag, dynamic `sys.path`
injection, or envelope-supplied catalog trust model.

The complete interaction reference is pinned at ManyForge commit
`08222188293df8e12f90dc72e204c21f22f92ea1` (tree
`bc749b9879d3c8f36fe7bf3036f9cc1f2ef1e60f`). It is historical source and
acceptance input, not current product documentation.
