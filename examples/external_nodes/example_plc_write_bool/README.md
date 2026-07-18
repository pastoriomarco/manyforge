# Example PLC Write Bool

This folder is the minimum copyable `manyforge.external_node.v0` contract
example. It demonstrates a trusted, non-real-time behavior leaf that consumes a
deployment-owned resource. It is not a Siemens protocol implementation.

Validate the folder before delivery:

```bash
manyforge nodes validate examples/external_nodes/example_plc_write_bool
```

The resource provider must make `write_bool_async(tag=..., value=...)` return
immediately. The returned operation exposes `done()`, `result()`, and
`cancel()`. Provider connection handling, credentials, endpoint selection, and
network timeouts remain outside generated node code.

The descriptor selects a resource through `plc_client_id`. A deployment that
uses this node must define that alias and advertise the required capability:

```yaml
external_nodes:
  enabled: [example_plc_write_bool]
  strict: true
runtime_resources:
  plc_clients:
    default:
      provider: site_plc
      endpoint_alias: cell_plc_1
      timeout_s: 0.5
      capabilities: [plc.siemens.s7.write]
```

Run the fake-provider contract tests from the repository container:

```bash
pytest -q examples/external_nodes/example_plc_write_bool/tests
```
