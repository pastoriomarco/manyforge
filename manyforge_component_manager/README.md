# ManyForge Component Manager C++ foundation

This ROS-free C++ package is the selected semantic-owner foundation for the
Composable Cell Component Manager. It currently owns two bounded mechanisms:

- receiver-local runtime-unit observation and convergence reduction; and
- durable immutable operation identities, append-only journal replay, and an
  internal compare-and-swap persistence fixture.

The observation reducer keeps process-observer evidence, runtime-source
evidence, and Manager-derived freshness/dependency facts distinct. Adapters
must serialize every call into one reducer instance. The reducer intentionally
contains no mutex: mutation has one owner, and readers receive immutable
snapshots instead of sharing mutable reducer state.

The persistence class is internally synchronized because several NRT clients
or replacement processes may race on one SQLite store. It owns its embedded
schema artifact, uses WAL with `synchronous=FULL`, validates immutable rows and
digest chains on reopen, and enforces fixed cardinality, payload, and database
page-image ceilings. Its database file is opened without following a final-path
symlink; SQLite 3.37 or newer and `SQLITE_OPEN_NOFOLLOW` are mandatory. Bootstrap remains
responsible for supplying a trusted existing parent directory; this
foundation's path preflight is not a substitute for directory ownership.

The persistence tests establish transaction and process-crash-window behavior.
They do not claim host-power-loss durability for first-time database or
directory creation; M0 bootstrap owns creation-directory durability and any
required directory `fsync`.

The digest chain is an accidental-corruption/replay consistency check, not
tamper evidence against a writer that can rewrite the database and recompute
digests. Without an external terminal anchor, deleting a valid final journal
suffix is not detectable by this foundation alone.

## Deliberate non-claims

This checkpoint has no ROS types or callbacks and performs no process, scene,
Kernel, controller, robot, command, lifecycle, or motion effect. Opaque
product/configuration/bundle/graph spellings are not I3 qualification. An
operation row named `apply` records only immutable fixture identity; it is not
the I4 Apply state machine. Likewise, `FoundationHead` is not
`IActiveBundlePublicationStore`, an active-bundle pointer, staged readiness, or
a publication receipt.

These raw bounded spellings exist only for pre-I3 cross-language parity. Final
retained-bundle integration must replace them at its boundary with the exact
typed I3 identities; this package must not grow a second product or bundle
identity model.

The Python reducer and persistence adapter remain temporary reference evidence
only while black-box parity and later cutover are completed. They are not a
fallback owner. I4 cutover must delete their semantic implementations after
the C++ owner gains the remaining I3-backed bootstrap, Apply, process
convergence, and publication behavior.

## Focused build

Run inside the canonical ManyForge development image:

```bash
cmake -S manyforge_component_manager \
  -B /tmp/manyforge-component-manager-build \
  -DBUILD_TESTING=ON
cmake --build /tmp/manyforge-component-manager-build --parallel
ctest --test-dir /tmp/manyforge-component-manager-build \
  --output-on-failure
```
