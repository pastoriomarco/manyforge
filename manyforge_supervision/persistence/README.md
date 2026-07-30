# I4 persistence fixture

This directory contains a bounded, ROS-free fixture for qualifying selected
Component Manager persistence mechanics:

- `schema/001_component_manager_fixture_persistence.sql` is the
  language-neutral fixture schema;
- `reference_adapter.py` qualifies initialization, immutable operation rows,
  journal append/replay, a generic fixture compare-and-swap head, and reopen
  consistency; and
- `verification/composable_cell_i4/persistence_fixture_crash_fixture.py`
  exercises four selected crash windows: schema initialization, a committed
  bootstrap write, an uncommitted append, and fixture-CAS commit before a
  later journal record.

The database is fixed to one `CellId`, one exact schema artifact digest, and
fixed resource ceilings. SQLite WAL with `synchronous=FULL`, one serialized
writer per adapter, database-level writer locking, immutable-row triggers,
foreign-key checking, semantic digests, and reopen consistency checks are part
of the fixture.

The 512 MiB value is a ceiling on the main SQLite database page image. The
64 MiB WAL value is a checkpoint/truncation target, not a hard WAL limit or a
bound on total on-disk footprint. A pinned reader can prevent truncation and
allow the WAL to grow past its target; the regression tests exercise that
case and recovery after the reader releases its snapshot.

The journal digest chain detects the tested accidental or out-of-band
inconsistencies when the stored chain is verified. It is not tamper evidence
against a writer able to modify the database and recompute every digest. It
has no external terminal anchor, so deletion of a valid final entry or suffix
is not detectable. The crash fixtures cover only the named windows above, not
every transaction boundary.

The generic fixture CAS is not `IActiveBundlePublicationStore`. Neither this
fixture schema/table layout nor `FixtureCasIntent`/`FixtureCasReceipt` is a
migration contract for `ActiveBundlePublicationRequestDTO` or
`ActiveBundlePublicationReceiptDTO`. The final C++ owner must consume those
normative DTOs directly. Once its equivalent black-box qualification passes,
the Python adapter and its adapter-coupled tests are deleted rather than kept
as a compatibility implementation.

Bundle, graph, request, and owner-epoch values are bounded opaque spellings
until their final I3/E0 wire types are connected. This layer never interprets
or qualifies them. It has no compilation, process, scene, Kernel, robot,
controller, readiness, admission, alarm, or lifecycle effect surface. It also
does not implement Apply phases, compensation, stage/finalize, or convergence;
later I4 slices must layer those semantics over the durable primitives.
