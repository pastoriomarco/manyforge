# ManyForge Deployment

`manyforge_deployment` is the ROS-free, non-real-time artifact boundary for
the composable-cell runtime. It depends on the passive identities and DTOs in
`manyforge_core`; the execution Kernel does not depend on this library.

The implemented foundation owns:

- strict one-document YAML decoding into a closed I-JSON value model;
- Unicode/NFC and number-model validation;
- RFC 8785 canonical JSON plus an internal domain-separated SHA-256 identity
  primitive; and
- intrinsic `CellGraphDTO` decoding, normalization, and validation.

Catalog-referent schema resolution, source/model composition, generated
products, Apply, process ownership, and runtime/lane behavior are later
checkpoints. A successfully parsed artifact is therefore only a candidate;
trust advances explicitly through schema/type, intrinsic-graph, digest, and
later catalog/compiler validation. C0.1 exposes no trusted `GraphRevision`
compute/verify operation because exact catalog-constrained schemas and default
materialization are prerequisites.

Build and run the focused tests through the repository container:

```bash
docker compose run --rm dev ./scripts/test-all.sh cpp
```

The implementation intentionally uses dedicated, qualified libraries for
strict YAML events, Unicode NFC, ECMAScript-compatible binary64 formatting,
and SHA-256. Those dependencies remain here rather than entering
`manyforge_core` or real-time execution paths.
