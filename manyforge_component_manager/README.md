# ManyForge Component Manager C++ foundation

This ROS-free C++ package is the selected semantic-owner foundation for the
Composable Cell Component Manager. It currently owns three bounded mechanisms:

- receiver-local runtime-unit observation and convergence reduction;
- durable immutable operation identities, append-only journal replay, and an
  internal compare-and-swap persistence fixture; and
- the separate durable `IActiveBundlePublicationStore` public-pointer owner.

The adjacent ROS-free contracts in `manyforge_core` additionally define the
bounded owner-to-owner substrate that this package will consume:

- strongly separated Apply, bootstrap, stage, publication, runtime-epoch, and
  configuration-fence identities/generations;
- focused Manager-to-scene-owner Apply and canonical-empty bootstrap ports;
- separate active-bundle publish and read-only receipt-verification
  capabilities; and
- an Apply-only Kernel fence acquire/rehydrate capability that exposes neither
  generic Stop nor an unfrozen release operation.

Those DTOs reject contradictory phase evidence, non-canonical transition
rows, mismatched predecessor/candidate expectations, inconsistent native
binding sets, invalid publication provenance, and fence reconciliation that
would reopen admission without complete local evidence. Persisted required-set
completeness and the final logical-scene snapshot contract remain owning-layer
checks; the current contract substrate deliberately does not pretend to prove
them.

The observation reducer keeps process-observer evidence, runtime-source
evidence, and Manager-derived freshness/dependency facts distinct. Adapters
must serialize every call into one reducer instance. The reducer intentionally
contains no mutex: mutation has one owner, and readers receive immutable
snapshots instead of sharing mutable reducer state.

Both SQLite owners are internally synchronized because several NRT clients or
replacement processes may race on one store. Each owns a distinct embedded
schema artifact, uses WAL with `synchronous=FULL`, and enforces fixed
cardinality and database page-image ceilings. The foundation validates
immutable rows and its journal digest chains on reopen; the publication store
separately validates generation continuity, each receipt digest, and the
singleton head. Database files are opened without following a final-path
symlink; SQLite 3.37 or newer and `SQLITE_OPEN_NOFOLLOW` are mandatory.
Bootstrap remains responsible for supplying a trusted existing parent
directory; path preflight is not a substitute for directory ownership.

The active-bundle publication owner is deliberately not the foundation CAS
head. It stores one immutable, generation-contiguous receipt chain and one
singleton mutable public-pointer head. Publish performs an exact durable
compare-and-swap, exact transaction replay returns the original receipt
without another effect, and conflicting replay fails closed. The read-only
scene-owner capability independently rereads and verifies the requested
receipt but succeeds only while that generation is still the active singleton
head. Historical receipts remain internal replay/integrity evidence and cannot
authorize stale finalization after a later publication.

The initial store is deliberately exhaustion-bounded to 4,096 immutable
receipts and a 64 MiB main database page image. It performs no pruning,
compaction, or generation reuse: exact replay remains available until the
store rejects a new publication at that hard boundary. This is a bounded I4
owner, not an indefinite configuration-history service. The filesystem threat
model is likewise explicit: bootstrap supplies a stable trusted parent
directory, while the store rejects a final database-path symlink and opens the
file with SQLite `NOFOLLOW`; it does not defend against a privileged writer
that can replace or rewrite entries in the trusted parent.

The persistence tests establish transaction and process-crash-window behavior.
They do not claim host-power-loss durability for first-time database or
directory creation; M0 bootstrap owns creation-directory durability and any
required directory `fsync`.

The digest chain is an accidental-corruption/replay consistency check, not
tamper evidence against a writer that can rewrite the database and recompute
digests. Without an external terminal anchor, deleting a valid final journal
suffix is not detectable by this foundation alone.

## Deliberate non-claims

These checkpoints have no ROS types or callbacks and perform no process,
scene, Kernel, controller, robot, command, lifecycle, or motion effect. Opaque
product/configuration/bundle/graph spellings in the foundation fixture are not
I3 qualification. An operation row named `apply` records only immutable
fixture identity; it is not the I4 Apply state machine. `FoundationHead` is
not the active-bundle publication store, active pointer, staged readiness, or
publication receipt. The focused store implements only publication
persistence and reread; it does not implement a scene owner, Apply
coordinator, private materialization, obsolete-command-surface retirement,
fence release, process convergence, bootstrap coordinator, or M0 behavior.

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
