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
- one code-owned build registry with eight exact qualified catalog families
  (`capability_definition`, `component_type`, `control_interface_contract`,
  `controller_topology`, `model_asset`, `provider_manifest`,
  `runtime_profile`, and `stop_evidence_policy`), two exact qualified value
  schemas, and explicit rejection of every remaining unqualified family; and
- intrinsic `CellGraphDTO` decoding and validation, exact runtime-profile
  closure, schema-normalized runtime configuration, whole-graph output
  budgets, and staged nested/outer graph content identity;
- exact caller-resolved model-source closure verification, including
  one compiler-owned immutable retrieval plan, digest-derived store keys, size
  and SHA-256 checks, resource-bounded non-resolving URDF/SRDF parsing,
  structural/source-role proof, and a canonical typed structural index that
  retains no raw model blobs; and
- exact model-asset/component-type dependency closure plus immutable graph
  structural qualification: owned and passive model occurrences, single
  placement derivation, model-role/root/mount/target resolution, and
  bidirectional attachment compatibility/capacity with child endpoints fixed
  to the child component root.

The bootstrap, registry, and dialect examples under `resources/` are hermetic
source-tree projections and cross-language test goldens. They are not installed
or loaded as runtime policy, never override the code-owned grammar, and never
qualify runtime input. The admitted catalog resolvers enforce exact typed
reference, retrieval key, stored-byte digest, schema, semantic identity, and
referent boundaries. Source retrieval/storage, complete re-emittable model IR,
SRDF expansion, generated naming, control compatibility, retained compiler
products, Apply, process ownership, and runtime/lane behavior remain later
checkpoints.
A successfully parsed artifact is therefore only a candidate. Arbitrary
revision-verified descriptors can only produce explicitly unqualified
normalized candidates. Production interpretation must select an exact schema
ID, revision, and purpose from the immutable build-qualified registry, then
validate and normalize through that pinned descriptor before later intrinsic,
catalog, and compiler validation. The eight catalog families and two value
schemas above are the only production-qualified schema families at this
checkpoint. `GraphRevision` compute/verify accepts only an intrinsically
validated graph whose runtime configuration has been normalized through the
exact qualified runtime-profile closure. It computes or verifies
control-domain, control-binding, and component-attachment revisions before the
outer graph revision; verification never repairs stale input. The resulting
wrapper proves normalized graph content identity, not model/source/control
compatibility, compiler products, runtime readiness, admission, or a lane.
Topology qualification proves only its immutable logical controller,
contract, chain, activation-set, and route declaration. It does not select
plugins or configuration, generate names or endpoints, observe controller
state, switch a route, or qualify a runtime lane.
Component-type declaration qualification similarly proves only its immutable
model-role tree, target-local aliases, capability selectors, tags, and attachment
interfaces. It does not select model assets, control contracts/topologies,
providers, generated names, graph instances, runtime state, or a product lane.
The later structural qualifier described above selects exact graph occurrences
and source/type closures, but still makes none of the deferred naming,
control-product, runtime, or lane claims.

Build and run the focused tests through the repository container:

```bash
docker compose run --rm dev ./scripts/test-all.sh cpp
```

The implementation intentionally uses dedicated, qualified libraries for
strict YAML events, Unicode NFC, ECMAScript-compatible binary64 formatting,
and SHA-256. Those dependencies remain here rather than entering
`manyforge_core` or real-time execution paths.
