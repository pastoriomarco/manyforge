# Trusted External Behavior Nodes During The V2 Cutover

> **Current status:** trusted extension nodes are intentionally unavailable.
> ManyForge does not scan node folders, import a caller-selected Python file,
> merge a runtime catalog, or instantiate an extension implementation. The
> **I5/H0** headless projection now independently verifies the immutable
> built-in NodeEnvironment selected by Manager and reports its implementation
> identity and runtime availability. It explicitly reports execution as
> unavailable; component/target-addressed execution returns at **I6/N**, and
> no installable extension-package contract is claimed yet.

This runbook records the supported authoring boundary and the work required to
restore trusted extensions without restoring the deleted v1 architecture. It
is not an installation procedure for the current tree.

## What an external node may be

A trusted external node is reviewed deployment code for non-real-time (NRT)
orchestration. It may be a thin client of an approved semantic owner. It may
not own or emulate:

- controller streaming or a real-time loop;
- planning, collision consequence, or execution terminal truth;
- Stop, Reset, or recovery authority;
- component lifecycle or controller-route authority;
- alarm, scene, measured-state, or runtime-health truth; or
- endpoint, credential, process, or provider selection.

Those constraints remain valid from the historical external-node design. The
loading and runtime mechanisms described by that design do not.

## The current v2 identity chain

The frozen `manyforge.node_catalog.v2` contract separates portable meaning
from executable identity:

1. A catalog entry declares portable parameters, scoped capability
   requirements, owner API, outcomes, and a stable runner
   `implementation_id`.
2. An executable-registry row binds `(node_id, engine_id)` to that
   implementation ID and a stable `executable_package_id`.
3. Executable-package and source-coverage rows bind the exact reviewed package
   bytes by digest. Host paths are not identities.
4. One deterministic compiler produces the immutable effective
   `NodeEnvironment`; its digest covers the catalog, executable registry,
   package identities, and source identities.
5. Component Manager retains that environment with the selected deployment
   bundle.
6. At I5/H0, the headless projection independently verifies this retained
   environment and adds runtime availability without changing catalog meaning
   or constructing an implementation.
7. At I6/N, the program runner will resolve the retained implementation and
   invoke resource-addressed semantic owners.

The checked-in public catalog and exact environment identity are
[`node_catalog.yaml`](../../manyforge_behavior/resources/node_catalog.yaml)
and
[`node_environment_manifest.yaml`](../../manyforge_behavior/resources/node_environment_manifest.yaml).
The current implementation in `manyforge_behavior` qualifies the built-in
package only. Its registry deliberately has no `instantiate` method.

## What is unavailable now

Do not use or document any of these as working interfaces:

- `/workspace/manyforge_nodes` or `~/.manyforge/nodes` folder scanning;
- `MANYFORGE_NODE_ROOTS` or `MANYFORGE_NODE_STRICT`;
- `manyforge.external_node.v0`;
- descriptor fields containing Python module, file, class, or constructor
  names;
- `NodeFactoryContext`, a Python factory table, or path-based symbol loading;
- `manyforge nodes validate`, `manyforge nodes list`, or runtime catalog reload;
- deployment `external_nodes` allowlists;
- deployment `runtime_resources` aliases or blackboard-injected handles; or
- Composer/assistant selection of source paths or executable symbols.

The absence of those surfaces is intentional. Re-adding a CLI around the old
loader would create a second catalog and runtime authority.

## Future extension-package requirements

Before installation can be documented as supported, the responsible
implementation must provide one closed extension-package contract with:

- extension ID, semantic version, and content digest;
- compatible catalog API/content range;
- catalog entries using the exact v2 entry schema;
- exact executable-package digest and source provenance;
- dependencies and supported orchestration engines;
- bounded NRT execution and cancellation limits; and
- rejection of duplicate IDs, incompatible versions, unsupported imports,
  missing implementations, and forbidden ownership claims.

The compiler must derive one canonical effective environment before retention.
Discovery or validation failure must leave the selected environment unchanged.
Dynamic health cannot be folded into its digest, and an environment change
cannot alter a running Program silently.

No machine-readable extension-manifest schema or production compiler path is
implemented in the current repository. Do not invent manifest members or
digests in site code while that contract is absent.

## Portable I/O authoring

The historical PLC example used a `plc_client_id`, logical tag, provider alias,
and a blackboard `runtime_resources` object. Those are not v2 program data.

Portable boolean output intent uses:

- `target_role`: a Program-Binding role, never a component/runtime ID;
- `resource_role`: a portable semantic resource role, never a topic, address,
  tag, endpoint, or credential; and
- `value`: the boolean command value.

Its capability requirement is `io.gpio.set_output`, scoped to the
`target_role`. Its semantic owner is `kernel.command_target`. Provider,
endpoint, resource, and component selection happens in the retained deployment
and Program Binding, not inside the node.

The built-in v2 catalog already contains `io.set_output` for this exact
portable operation. Prefer it over a custom extension. The retained
[`example_plc_write_bool`](../../examples/external_nodes/example_plc_write_bool/README.md)
folder is therefore a migration/reference example, not something to install.

## Review guidance while extensions are unavailable

Supplier code may still be reviewed and unit-tested independently, but that
does not qualify a ManyForge extension. Keep it outside runtime scan paths and
do not claim that a passing unit test makes it executable through Composer or
the Program runner.

Preserve these acceptance properties for the future package:

1. Import and construction cause no external effects.
2. Every operation is bounded and cancellation is idempotent.
3. Unexpected exceptions become bounded diagnostics, not process control.
4. Portable parameters contain no runtime identity or provider data.
5. The implementation cannot widen its catalog schema or capabilities.
6. Package/source identity is immutable and digest-bound.
7. Missing, stale, ambiguous, or unhealthy bindings fail before mutation.
8. Stop, lifecycle, collision, and runtime truth remain with their owners.

## Historical acceptance disposition

The last complete v1 source is parent commit
`153195a692c50e606f50e40bd755da6081dd24c5`, tree
`c0daa2440275c2635f022c90dd9d71bff5d43b6d`. Deletion commit
`c42087c559941550eb291cb357d813dd33e7fe0d`, tree
`460745ab7115dfef8727f79d52d905f300bb0088`, removed the path loader and
factory authority. Git is the implementation reference, not a runtime
fallback.

Acceptance moves in dependency order:

| Historical cases | Disposition |
| --- | --- |
| Descriptor shape, schema preservation, duplicate IDs, compatible versions, source containment, immutable captured source, and CLI validation | Recreate against a future closed v2 extension-package compiler and retained environment before extension enablement. Never restore `manyforge.external_node.v0` or path import. |
| `test_descriptor_*`, `test_non_strict_discovery_*`, `test_strict_discovery_*`, `test_external_import_*`, constructor checks, module-cache checks, and generated-node CLI checks | Future extension-package acceptance using content/package identities rather than folders and Python symbols; H0 qualifies only the retained built-in environment projection. |
| Registry execution, normal tree outcomes, contained tick/terminate exceptions, structured diagnostics, and captured-environment behavior | I5/H0 now verifies and projects the selected built-in implementation identity and runtime availability; I6/N must supply addressed execution. |
| Runtime-resource setup, alias admission, provider construction, rollback, close, and enablement tests | Replace with retained deployment/Program-Binding resolution at I6/N. The Python `runtime_resources` registry and deployment `external_nodes` fields never return. |
| PLC example `test_success`, `test_provider_failure`, `test_timeout_cancels`, and `test_stop_cancels_in_flight_operation` | Dependency-neutral attempt decisions remain tested in the example. Actual command/result/cancellation behavior returns through `kernel.command_target` at I6/N; Stop remains Kernel-owned and is requalified at I8/P5. |

Until those gates pass, the truthful operator instruction is simple: external
node installation, validation, reload, and execution are unavailable.
