# ManyForge Deployment

`manyforge_deployment` is the ROS-free, non-real-time artifact boundary for
the composable-cell runtime. It depends on the passive identities and DTOs in
`manyforge_core`; the execution Kernel does not depend on this library.

The implemented offline boundary owns:

- strict one-document YAML decoding into a closed I-JSON value model;
- Unicode/NFC and number-model validation;
- RFC 8785 canonical JSON plus an internal domain-separated SHA-256 identity
  primitive;
- immutable `manyforge.closed_schema.v1` descriptor compilation, declared
  `SchemaRevision` verification, bounded default materialization, and
  schema-directed instance normalization;
- one code-owned build registry with eight exact qualified catalog families
  (`capability_definition`, `component_type`, `control_interface_contract`,
  `controller_topology`, `model_asset`, `provider_manifest`,
  `runtime_profile`, and `stop_evidence_policy`), two exact qualified value
  schemas, and explicit rejection of every remaining unqualified family;
- intrinsic `CellGraphDTO` decoding and validation, exact runtime-profile
  closure, schema-normalized runtime configuration, whole-graph output
  budgets, and staged nested/outer graph content identity;
- exact caller-resolved source closure, complete typed/re-emittable URDF/SRDF
  IR, bounded self-contained COLLADA support, structural/source-role proof,
  and immutable source qualification;
- deterministic `mf1` occurrence composition, flattened URDF/SRDF, complete
  rewrite proof, portable frame/path projection, resource-coordinate maps,
  target projections, and checked cell-wide bounds;
- exact occurrence-specific control/provider/capability/route/Stop
  applicability and deterministic logical templates;
- a private-constructor `CompilerQualifiedCellIr`, complete current-subset
  deployment-envelope producer/verifier, and exact envelope/IR retention proof;
  and
- code-owned product-contract/raw-CAS foundations plus closed retained-bundle
  projection/verifier and bounded semantic repository mechanics;
- immutable filesystem repositories for build manifests, raw artifacts, and
  semantic retained bundles beneath one trusted package root; and
- one build-owned canonical-empty package generator plus a sealed, zero-input
  Manager-facing admission proof that independently verifies the marker,
  current build authority, semantic bundle, and complete raw-artifact closure
  without repairing or enumerating the package.

The bootstrap, registry, and dialect examples under `resources/` are hermetic
source-tree projections and cross-language test goldens. They are not installed
or loaded as runtime policy, never override the code-owned grammar, and never
qualify runtime input. Admitted resolvers enforce exact typed references,
retrieval keys, stored-byte digests, schemas, semantic identities, and complete
referent closure.

The trust stages are deliberately non-forgeable and non-repairing. A parsed
artifact is only a candidate; each later wrapper is privately constructed by
the exact qualifying operation. The final C++ compiler value is the only C++
semantic input accepted by retention qualification. Unsupported optional
deployment blocks and unqualified catalog families fail before raw storage.

Current limits are explicit:

- the production Python compiler binding still awaits the separately approved
  atomic binding cutover, so I2 is not complete;
- C1.0 build-qualified retention authority is implemented: the closed v2
  node-environment, compiler-contract, and non-circular compiler-build
  manifests are produced by one non-caller-constructible authority, and the
  production canonical-empty path persists and independently reloads that exact
  closure before any raw-artifact write; compiler-build evidence covers the
  exact compiler sources, configured core headers, build recipes/templates,
  and node-environment freshness checker under stable logical IDs;
- qualified compiler-build evidence currently requires a single-configuration
  CMake generator; multi-config generators reject at configure time rather
  than sharing one ambiguous manifest across several binary configurations;
- the bounded I3 durability checkpoint now provides an immutable filesystem
  build-manifest repository, raw-artifact CAS, and distinct semantic
  retained-bundle repository beneath one existing trusted package root; each
  process opens its own bounded, crash-adopting immutable repository instance,
  while production materialization independently reloads the semantic envelope
  and directly rechecks its raw-artifact closure;
- the canonical-empty generator accepts only `--output-root`, traverses the
  ordinary I2/I3 compiler and retention path, and publishes its admission marker
  last with immutable no-replace semantics; the public filesystem source takes
  only the trusted root and returns a sealed value containing the exact fixed
  `CellId`, deployment/graph/bundle identities, build-authority digests, and
  privately owned verified bundle;
- all canonical-empty reads are fixed-name, bounded, read-only point lookups.
  A visible invalid marker is never repaired, unrelated repository entries are
  ignored, and missing, substituted, malformed, stale, or identity-incoherent
  owned entries fail closed;
- retained product consumers can point-read only one exact
  `(RetainedBundleId, ProductId, RetainedProductKind, ArtifactRoleId)` tuple.
  A successful sealed result echoes the immutable product-contract and
  artifact-reference metadata and carries independently verified raw bytes;
  unknown tuples are absent and present corruption fails closed. The
  canonical-empty URDF/SRDF pair is qualified through a real RoboPlan
  planning/collision `Scene`, including the semantic cell frame, zero
  coordinates, zero geometry/pairs, collision-query behavior, and
  deterministic reopen;
- this does not complete I3 or the complete C1 exit: no build-owned public
  runtime selection or active alias exists, and remaining products, consumer
  equivalence outside canonical-empty planning/collision, and runtime-shadow
  behavior remain later I3 work; and
- Apply, active publication, process ownership, runtime convergence,
  simulation, controllers, execution, and lane restoration belong to later
  packages and plan checkpoints.

Build and run the focused tests through the repository container:

```bash
docker compose run --rm dev ./scripts/test-all.sh cpp
```

The repository qualification command enables the exact checked canonical-empty
Release golden. Ad-hoc builds leave that golden gate disabled because a
different compiler, toolchain, or flag set legitimately produces a different
build-qualified identity; deterministic and closure verification still run.
Maintainers reproducing the repository golden directly must configure Release
with `-DMANYFORGE_VERIFY_CANONICAL_EMPTY_RELEASE_GOLDEN=ON`.

The implementation intentionally uses dedicated, qualified libraries for
strict YAML events, Unicode NFC, ECMAScript-compatible binary64 formatting,
and SHA-256. Those dependencies remain here rather than entering
`manyforge_core` or real-time execution paths.
