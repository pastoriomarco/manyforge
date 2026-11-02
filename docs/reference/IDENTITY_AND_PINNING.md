# Identities and pins

ManyForge identifies content by digest (`sha256:…`). Some digests are computed
while the cell runs; others are committed to the repository ("pins"). This page
lists both, says why each exists, and how to regenerate the committed ones.

## Rule

- **Identity only where the running cell is protected.** A runtime identity
  exists only where it protects a running command or what the robot runs
  (for example an operation id that a result must match, or the Applied
  composition a run executes). Component lifecycle carries none: no
  generations, epochs or receipts; a component's state is what its adapter
  reports now.
- **Computed, never hand-kept.** Runtime identities are computed from the
  bytes the system actually uses. A committed digest or generated document is
  produced by a generator. Never edit one by hand: change the source, run
  `scripts/regen.py --update`, review the diff, commit the result.
- **No new pins without a running-cell reason.** If a value can be computed at
  load, build or Apply and recorded, do that instead of committing it.

Digests are SHA-256 over RFC 8785 canonical JSON, from one codec in
`manyforge_content_identity` (domain-framed where a contract names a domain).

## Computed at runtime (not in the repository)

| Identity | Checked when | Protects |
| --- | --- | --- |
| Component fingerprint: the digest of the component's build spec (`component_spec_fingerprint` in `manyforge_deployment`); a machine member's spec holds its owner's fingerprint | Apply preview and Apply (the component delta) | A component restarts exactly when what it is launched or constructed with changes; the replacement reasons are the spec sections that differ (`adapter`, `address`, `bindings`, `controllers`, `description`, `launch`, `owner`, `providers`) |
| Applied composition: retained bundle, graph revision, Program Binding, Program revision, node-environment digest, applied record digest | Execute (`program_node_availability_stale`) and Runner binding | The robot runs exactly what was Applied |
| Retained bundle against this build's closed-schema registry | Bundle verification | Bundle and build agree on every schema |
| Stop contract roles of each provider | Activation | Every arm and gripper provider can be stopped as configured |
| Cell admission request ids and owner epochs, against the current admission facts | Run, Apply, Reset, manual operations (Stop bypasses them) | No two effects race on the robot |
| Manager Apply journal and Program cycle journal records (digests, schema versions) | Manager and runner restart | An interrupted Apply or run is reported from intact records |

Nothing is journaled for component lifecycle: after a restart every component
starts `not_prepared` or `offline` and is prepared again.

## Committed pins and generated files

| What | Where | Checked by | Why it is committed |
| --- | --- | --- | --- |
| Node catalog content version | `EXPECTED_CONTENT_VERSION` in `manyforge_behavior/manyforge_behavior/catalog.py`; the extension descriptor's `catalog_content_version`; `kBuiltinCatalogContentVersion` in `manyforge_deployment/src/build_manifest.cpp` | Catalog load, extension selection, compiler build manifest | A Program names the exact active catalog version |
| Node catalog digest | `catalog_digest` in `node_catalog.yaml`; `EXPECTED_BUILTIN_CATALOG_DIGEST` in `catalog.py` | Catalog load; node environment | The loaded catalog equals the reviewed one |
| Node environment manifest | `manyforge_behavior/resources/node_environment_manifest.yaml` | CMake configure; workbench start (regenerated and compared) | Provenance of the node code an Apply uses |
| Assistant instrument registry | `manyforge_deployment/resources/assistant/assistant_instrument_registry_source.v1.json` | CMake configure | Assistant tools match the node schemas the backend validates |
| Generated closed schemas | `closed_schema/value/…io_provider_execution.v1.yaml`, `…pose_source.v2.yaml`; their revisions in `registry.v1.yaml` and tests | Schema registry load | Bundles and providers agree on a schema |
| Alarm catalog digest | `descriptor_digest` in `manyforge_headless/resources/operator-alarm-catalog.v1.yaml`; its test | Headless start | Alarm definitions are intact; the digest is recorded in alarm snapshots |
| Maintained Deployment sources | `manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json`; assistant profile and template digests in `manyforge_deployment/tools/check_maintained_deployment_sources.py` | Source checker | Maintained examples come from the reviewed description |
| Maintained ModelAssets | `manyforge_deployment/resources/maintained/model_assets/*.model_asset.json` (URDF/SRDF/mesh digests, `model_revision`) | ctest `MaintainedModelCatalogSources` | Model families come from the reviewed example model sources |
| Maintained occurrences | `manyforge_deployment/resources/maintained/catalog/`, site overlays, generator-owned Scenarios | ctest `MaintainedDeploymentOccurrenceSources` | Offline qualification inputs |
| Program revision | `program_revision` in every Program | Program load | File integrity; the revision is recorded in Binding and journal |
| Scenario pins | `members`, `content`, `saved_status.checked_content_digest`, `source_snapshot` in `scenario.yaml` | Composer (flags "changed") | Change detection; "the last check applies to exactly this content" |
| REST and MCP contracts | `docs/reference/REST_API.md`, `HEADLESS_REST_API.md`, `MCP_RUNTIME_CONTRACT.md` | `scripts/ci/check-public-contracts-fresh.sh` | Published interface |
| Vendor and lane drivers | `source.commit` in `manyforge_xarm_control/vendor/driver.yaml` and `manyforge_isaac_adapter/lane/driver.yaml`; `XARM_SOURCE_COMMIT` in `Dockerfile.ros-lyrical-full` | Image build | Every image runs the reviewed driver source |
| RoboPlan | `ROBOPLAN_PINNED_REF` in `scripts/setup-docker.sh`, `.docker/roboplan.manyforge-sha`; patches in `third_party/roboplan/patches/` with `patches.sha256` | Image build | The planner is the reviewed RoboPlan 0.8.0 with the reviewed patches |

## Regenerate

Run in the workbench or dev image, from the repository root:

```bash
python3 scripts/regen.py --update --build-dir <build>/ros-integration
python3 scripts/regen.py --check  --build-dir <build>/ros-integration --require-native
```

- Steps, in order: `project-snapshots`, `catalog-pin`, `node-catalog`,
  `closed-schemas`, `alarm-catalog`, `maintained-sources`,
  `program-binding-transport`, `assistant-registry`, `node-environment`,
  `program-revisions`*, `model-assets`*, `maintained-occurrences`, `scenario-pins`,
  `rest-contracts`. `--list` prints them; `--only a,b` runs a subset.
- \* The native steps need built tools: `manyforge_program_revision`, and
  `manyforge_maintained_model_catalog` from a `manyforge_deployment` build
  configured with tests (its configure prepares `generated/<family>_structural`
  beside the tool). Pass `--build-dir` (the directory that contains
  `deployment/manyforge_program_revision` and
  `deployment/manyforge_maintained_model_catalog`), or `--program-revision-tool`
  / `--model-catalog-tool`, or set `MANYFORGE_PROGRAM_REVISION_TOOL` /
  `MANYFORGE_MODEL_CATALOG_TOOL`. Without a tool its step is reported as
  skipped; `--require-native` fails instead.
- The C++ tools embed the catalog, closed schemas and node environment at
  build time. After changing any of those, or when regen reports that C++ pins
  moved: run `--update`, rebuild, run `--update` again, then `--check`.
- `--update` repeats the steps until nothing changes. When a digest moves, regen
  replaces the old value in every source and test file that holds a copy. It
  never edits `docs/`, `examples/` (other than its own outputs), `*.md`, sealed
  `*.deployment.yaml` files or another generator's outputs; it lists any old
  value left in those places.
- `--check` writes nothing and exits 1 on any stale output or pin.
- `scenario-pins` re-pins `checked_content_digest` to the current content. It
  does not re-run the check; open and check the Scenario if its content changed.

## Not covered by regen (edit deliberately)

- A catalog content version change: edit the three places in the first table
  row and the `catalog.content_version` of every Program and Scenario Program,
  then regenerate.
- Hand-authored closed-schema revisions, the schema bootstrap digest, and the
  capability definition digests in `capability_definition.hpp` and the
  occurrence generator.
- The content tables in `check_maintained_deployment_sources.py` (assistant
  profile contracts, model families).
- The vendor and lane driver commits and the RoboPlan pin: change them
  together with the image build (`driver.yaml`, `Dockerfile.ros-lyrical-full`,
  `scripts/setup-docker.sh`), then rebuild the image.
- The release golden (`scripts/qualification/review_canonical_empty_release_golden.py`),
  which is updated only when publishing a new frozen candidate.
