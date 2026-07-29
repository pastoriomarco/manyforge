# ManyForge Deployment

`manyforge_deployment` is the ROS-free, non-real-time artifact boundary for
the composable-cell runtime. It depends on the passive identities and DTOs in
`manyforge_core`; the execution Kernel does not depend on this library.

The implemented foundation owns:

- strict one-document YAML decoding into a closed I-JSON value model;
- Unicode/NFC and number-model validation;
- RFC 8785 canonical JSON plus an internal domain-separated SHA-256 identity
  primitive; and
- immutable `manyforge.closed_schema.v1` descriptor compilation, declared
  `SchemaRevision` verification, bounded default materialization, and
  schema-directed instance normalization;
- one code-owned build registry whose catalog families are explicitly
  unqualified and whose separately qualified value-schema table is empty at
  this checkpoint; and
- intrinsic `CellGraphDTO` decoding, normalization, and validation.

The bootstrap, registry, and dialect examples under `resources/` are hermetic
source-tree projections and cross-language test goldens. They are not installed
or loaded as runtime policy, never override the code-owned grammar, and never
qualify runtime input. Catalog schema qualification, catalog-referent
resolution, source/model composition, generated products, Apply, process
ownership, and runtime/lane behavior are later checkpoints. A successfully
parsed artifact is therefore only a candidate. Arbitrary revision-verified
descriptors can only produce explicitly unqualified normalized candidates.
Production interpretation must first select an exact schema ID, revision, and
purpose from the immutable build-qualified registry, then validate and
normalize through that pinned descriptor before later intrinsic, catalog, and
compiler validation. No catalog family or value schema is production-qualified
at this checkpoint, and no trusted `GraphRevision` compute/verify operation is
exposed yet.

Build and run the focused tests through the repository container:

```bash
docker compose run --rm dev ./scripts/test-all.sh cpp
```

The implementation intentionally uses dedicated, qualified libraries for
strict YAML events, Unicode NFC, ECMAScript-compatible binary64 formatting,
and SHA-256. Those dependencies remain here rather than entering
`manyforge_core` or real-time execution paths.
