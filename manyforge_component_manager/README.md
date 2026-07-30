# ManyForge Component Manager C++ foundation

This ROS-free C++ package is the selected semantic-owner foundation for the
Composable Cell Component Manager. It currently owns three bounded mechanisms:

- receiver-local runtime-unit observation and convergence reduction;
- a typed durable journal for one canonical-empty bootstrap intent and its
  scene-owner-accepted target; and
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
cardinality and database page-image ceilings. The bootstrap journal validates
its exact schema, immutable rows, typed identities, canonical binding order,
and semantic digests on every reopen. The publication store separately
validates generation continuity, each receipt digest, and the singleton head.
Database files are opened without following a final-path symlink; SQLite 3.37
or newer and `SQLITE_OPEN_NOFOLLOW` are mandatory.

`ExternallyProvisionedManagerStores` is the composition-root startup helper.
It requires a trusted, existing, non-symlink stable-state directory; opens the
bootstrap journal and publication store at fixed filenames with one exact
`CellId`; verifies both stores; and `fsync`s the directory before exposing
them. It neither makes the two databases one transaction nor infers
cross-store recovery. Deployment bootstrap remains responsible for directory
ownership and permissions. When exactly one store already exists, startup
uses a cell-neutral directory lock and read-only SQLite classification to
validate an initialized peer before creating or initializing the missing or
schema-empty store. A foreign-cell startup attempt therefore cannot poison the
directory with a mismatched database.

The bootstrap journal retains one bounded append-only generation sequence
under the first durable `BootstrapId`. Its latest attempt has exactly three
derived states: empty, immutable intent persisted, and immutable accepted
target persisted. A new attempt must advance the durable generation
high-water mark by exactly one; generations and bootstrap identities cannot be
reused or silently replaced. An exact duplicate write replays the original
record, while a different payload or stale first-time acceptance fails closed.
It stores no arbitrary event bytes, mutable generic phase, Apply transaction,
active alias, or effect command. Its initial resource envelope is at most 64
attempts, 64 canonical native-runtime bindings per attempt, 256
printable-ASCII bytes per identity, and a 64 MiB main database page image.

The active-bundle publication owner is deliberately separate from the
bootstrap journal. It stores one immutable, generation-contiguous receipt
chain and one singleton mutable public-pointer head. Publish performs an exact
durable compare-and-swap, exact transaction replay returns the original
receipt without another effect, and conflicting replay fails closed. The
read-only scene-owner capability independently rereads and verifies the
requested receipt but succeeds only while that generation is still the active
singleton head. Historical receipts remain internal replay/integrity evidence
and cannot authorize stale finalization after a later publication.

The initial store is deliberately exhaustion-bounded to 4,096 immutable
receipts and a 64 MiB main database page image. It performs no pruning,
compaction, or generation reuse: exact replay remains available until the
store rejects a new publication at that hard boundary. This is a bounded I4
owner, not an indefinite configuration-history service. The filesystem threat
model is likewise explicit: bootstrap supplies a stable trusted parent
directory, while the store rejects a final database-path symlink and opens the
file with SQLite `NOFOLLOW`; it does not defend against a privileged writer
that can replace or rewrite entries in the trusted parent.

The focused tests establish transaction, reopen, concurrency, process-exit,
uncommitted-transaction rollback, schema-integrity, path, and resource-bound
behavior. They do not emulate host power loss or claim durability beyond the
SQLite/POSIX filesystem contract. The digests are accidental-corruption and
replay-consistency checks, not tamper evidence against a writer that can
rewrite the databases and recompute them.

## Deliberate non-claims

These checkpoints have no ROS types or callbacks and perform no process,
scene, Kernel, controller, robot, command, lifecycle, or motion effect. Opaque
bundle and graph identities in the journal do not prove that an I3 artifact
exists, was loaded, or matches the canonical-empty golden. The journal does
not choose or mint a bootstrap identity/generation for its caller, call
`BeginProjectionBootstrap`, retain the scene owner's logical-scene value,
invoke native rebuild/status, publish a receipt, or open scene writes and
leases. It only enforces and exposes the durable high-water mark so the later
coordinator can recover it before minting the next generation. Those ordered
coordinator steps remain later I4 work.

The focused publication store implements only publication persistence and
current-head reread. It does not implement a scene owner, Apply coordinator,
private materialization, obsolete-command-surface retirement, fence release,
process convergence, bootstrap coordinator, or M0 behavior. Opening the
external store set creates no bootstrap intent and publishes nothing.

The old generic raw persistence fixture and its `FoundationHead` compare-and-
swap surface have been removed rather than retained as compatibility
architecture. The Python runtime-observation reducer remains temporary
reference evidence until C++ observation cutover; it is not a fallback owner.

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
