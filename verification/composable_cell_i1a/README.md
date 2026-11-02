# Composable Cell I1a Prototypes

This package is bounded selection evidence for I1a of the private composable
cell implementation plan. It is not a production schema, runtime feature,
compatibility layer, or supported deployment path.

The prototypes exercise the following architecture risks with deterministic,
standard-library-only fakes and disposable subprocess probes:

- P0: durable Apply and Component Manager restart reconciliation;
- P1: caller-neutral request ordering, replay, and backpressure;
- P2: one absolute Stop envelope and one terminal certificate;
- P4: deterministic model-instance and SRDF-group compilation; and
- P5: independent and coordinated execution across two control domains;
- process placement: manager, bounded compiler worker, execution host, program
  runner, headless state/API, and control-domain failure boundaries.

Production code must never import this package. The prototype may read existing
model-authority helpers and checked-in model assets in order to test candidate
contracts against real source shapes. Promotion requires the separate I1b
normative approval and a clean production implementation. These state machines
and wire models must not be copied wholesale or retained as a compatibility
API. Production slices extract only approved invariants, and delete this
prototype when replacement evidence closes its selection purpose.

The probes do not qualify real-time behavior, DDS, `controller_manager`, JTC,
ROS interfaces, physical attachment transforms, TCP/collision semantics, a
product lane, or a container count. Their timing/RSS values are informational
for this Python process and source tree only.

Run the focused evidence:

```bash
docker compose run --rm dev pytest tests/composable_cell_i1a -q
```

The serialized schema identifier is
`manyforge.experimental.composable_cell.i1a.v0`. No stability or migration
promise applies to it.
