# manyforge_assistant_bridge

The live assistant bridge is intentionally unavailable during the composable-
cell runtime cutover. Running `python -m manyforge_assistant_bridge.bridge`
fails before starting a server and returns exit status 78.

What remains current:

- `protocol.py`: deterministic catalog identity only. A hash is not proof of
  authenticity unless it is compared with an independently trusted value.
- `supervisor.py`: a transport-neutral availability reducer over
  already-observed facts. Endpoint probes, publication, polling, and shutdown
  effects were removed; publishing status is fenced until H0.
- `bridge.py`: the stable H0/I5/I6 unavailable entrypoint.

What must be rebuilt rather than revived:

- H0/I5: bounded conversation/session history, hard memory and turn/time
  limits, cancellation, argument/schema sanitation, stuck-loop handling,
  proposal-only/nonmutation enforcement, bounded audit, and the headless
  callback client.
- I6: Program/tree authoring context and tools on the new native addressing
  contract.

Do not copy the former v0 FastAPI envelope, Composer callback URLs, singleton
scene/Program mappings, import-time environment bag, dynamic `sys.path`
injection, or envelope-supplied catalog trust model.

The complete interaction reference is pinned at ManyForge commit
`08222188293df8e12f90dc72e204c21f22f92ea1` (tree
`bc749b9879d3c8f36fe7bf3036f9cc1f2ef1e60f`). It is historical source and
acceptance input, not current product documentation.
