# External Nodes: Install, Validate, and Operate

This runbook covers trusted external behavior-tree leaves loaded without
editing ManyForge source. External nodes are non-real-time orchestration code.
They cannot own controller streaming, stop authority, collision checking, or
the RuntimeSafety lane.

## Trust boundary

`node.py` is imported Python code. Installing a folder grants it the same
process privileges as Composer. Only install reviewed artifacts from trusted
suppliers. Programs, browser clients, and assistant tools cannot select an
import path or write an installed node folder.

ManyForge records a SHA-256 hash over each descriptor and executable source,
then binds the catalog and runtime registry into one immutable effective node
environment. A reload either swaps the whole environment while idle or keeps
the previous environment unchanged.

## Install location

The standard containers mount this repository at `/workspace` and scan these
roots by default:

```text
/workspace/manyforge_nodes
~/.manyforge/nodes
```

The repository-owned root is therefore the portable default. Each immediate
subdirectory is one node:

```text
manyforge_nodes/
  vendor_domain_action/
    node.yaml
    node.py
    README.md                 # recommended
    tests/                    # recommended
```

Override roots with a colon-separated container-path list:

```bash
export MANYFORGE_NODE_ROOTS=/workspace/manyforge_nodes:/opt/site/nodes
```

Every custom path must be mounted into the selected container. The unified
launcher forwards `MANYFORGE_NODE_ROOTS` and `MANYFORGE_NODE_STRICT` to both
native and ROS/Isaac Composer lanes. `MANYFORGE_NODE_STRICT=1` turns missing
roots and rejected nodes into startup/reload failures; use it for qualified
deployments.

## Validate before installation

Run validation in the same ManyForge container image that will execute the
node. Container names vary by selected lane; identify the active Composer
container with `docker ps`, then run:

```bash
docker exec <composer-container> bash -lc \
  'cd /workspace && manyforge nodes validate /path/to/vendor_domain_action'
```

The command parses `node.yaml`, verifies containment and the runtime contract,
imports `node.py`, checks the class and constructor, merges it with the built-in
catalog, and builds a registry candidate. Success prints the node ID, version,
path, and content hash. Exit status `2` means rejection.

The repository images expose `manyforge` through `/workspace/scripts`, so the
same command works in direct `docker exec` sessions even though the source tree
is mounted rather than installed. A non-container package installation uses
the equivalent Python package console entry point.

Useful inventory commands:

```bash
manyforge nodes list --json
manyforge nodes validate --strict
manyforge nodes catalog --format yaml
```

A copyable provider-neutral example is at
[`examples/external_nodes/example_plc_write_bool`](../../examples/external_nodes/example_plc_write_bool).

## Phase-1 descriptor contract

`manyforge.external_node.v0` intentionally has a small surface:

- folder name exactly equals `id`, which matches `^[a-z][a-z0-9_]*$`;
- semantic `version` uses `X.Y.Z`;
- `kind` is `leaf` and children are exactly `{min: 0, max: 0}`;
- runtime language/file are exactly `python` and `node.py`;
- runtime `py_trees` is exactly `==2.4.0`;
- class subclasses `py_trees.behaviour.Behaviour`;
- constructor signature is exactly keyword-capable `name` plus `**params`;
- tick policy is `nonblocking`;
- source and descriptor paths remain inside the node folder;
- unknown descriptor/runtime/resource fields are rejected.

Every parameter uses the same schema model as built-in catalog nodes. That
catalog schema remains the sole authority for program validation and Composer
editing.

External I/O must be initiated through a provider-owned asynchronous handle.
`update()` polls bounded state and returns promptly. `terminate()` cancels any
in-flight operation. Import and construction must have no external side
effects.

Provider acquisition in `setup()` runs before the behavior cycle starts. A
setup exception rejects that Run/build attempt; it is not converted into a
mid-cycle node failure. Once ticking begins, the ManyForge external boundary
contains unexpected tick and termination exceptions as structured
`external_node_exception` failures.

## Deployment enablement

Installation and authorization are separate:

```yaml
external_nodes:
  enabled: [vendor_domain_action]
  strict: true
```

- omitted `enabled`: every installed external node is enabled;
- `enabled: []`: no external nodes are enabled;
- a list: only those installed IDs can execute.

Assistant authoring is independently default-deny. An external node must also
appear in `assistant_modes.<mode>.catalog.nodes` before that mode can insert it.
Installing or enabling a node does not grant model authority.

## Runtime resources

Generated code consumes logical aliases, never endpoints or credentials:

```yaml
runtime_resources:
  plc_clients:
    default:
      provider: site_siemens_s7
      endpoint_alias: cell_plc_1
      timeout_s: 0.5
      capabilities: [plc.siemens.s7.write]
```

The node descriptor declares the required type and the parameter that selects
its alias:

```yaml
runtime_resources:
  - id: plc_client
    type: plc_client
    access: read_write
    required: true
    alias_parameter: plc_client_id
```

Before execution, ManyForge verifies that the node is enabled, the resource
type exists, the selected alias exists, and required capability keys are
present. At runtime the node reads the immutable `runtime_resources`
blackboard key and calls `get_plc_client(alias)`.

Resource providers are process-owned adapter integrations. A deployment that
names an unregistered `provider` fails while runtime resources are built. All
created handles are closed on replacement and shutdown; cleanup attempts every
handle even if one provider reports an error.

## Reload in Composer

Open **Deployment > External Nodes** and press **Reload Node Catalog**.

Reload is accepted only while execution is idle. Candidate scan, import, and
validation happen before the active-state lock. Composer then rechecks the
loaded program and deployment against the candidate and atomically swaps the
catalog/registry pair as one environment. The deployment-owned resource
registry is unchanged by node-only reload; changing resource declarations uses
the normal deployment apply path. A running tree keeps its captured environment
for its entire lifetime.

The panel reports accepted, warning, and rejected sources plus old/new catalog
hashes and added/changed/removed counts. Typical outcomes:

- `409 execution_not_idle`: stop or finish the current run, then retry;
- duplicate ID: remove or rename both conflicting external folders;
- invalid descriptor/import: fix the reported folder; active catalog remains;
- active artifact validation failed: the candidate removed or changed a node
  required by the loaded program/deployment; update the artifacts first;
- missing root warning: create/mount the root, or enable strict mode to make it
  a release-blocking error.

## Delivery checklist

Before accepting a generated folder:

1. Review `node.py` as trusted adapter code and verify import has no side
   effects.
1. Run `manyforge nodes validate <folder>` in the deployment image.
1. Run supplier tests with fake resources covering success, provider failure,
   timeout, and cancellation.
1. Confirm every I/O call is asynchronous and every wait/operation is bounded.
1. Confirm `terminate()` cancels in-flight work and provider `close()` is
   bounded/idempotent.
1. Confirm IDs, parameter defaults, status semantics, and capability/resource
   requirements are documented.
1. Install in a non-production root, reload while idle, and inspect provenance
   hashes and diagnostics.
1. Explicitly enable the node and, only if needed, add it to an assistant-mode
   allowlist.

The descriptor rules, runtime-resource boundary, validation command, example,
and delivery checklist above form the self-contained deployment contract.
