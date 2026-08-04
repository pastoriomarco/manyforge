# ManyForge Component Manager C++ foundation

This ROS-free C++ package is the selected semantic-owner foundation for the
Composable Cell Component Manager. It currently owns these bounded mechanisms:

- receiver-local runtime-unit observation and convergence reduction;
- a typed durable journal for one canonical-empty bootstrap intent and its
  scene-owner-accepted target;
- the separate durable `IActiveBundlePublicationStore` public-pointer owner;
- a typed durable journal for immutable named-Apply intent, affected scope,
  Kernel-fence recovery, runtime-convergence intent, and complete staged-ready
  evidence;
- the canonical-empty prerequisite/Begin coordinator that consumes the sealed
  I3 admission, durably records the scene-owner-accepted target, and advances
  that exact zero-native target through Rebuild, publication, and Finalize;
- the whole-Apply affected-scope/fence coordinator and observe-first runtime
  convergence coordinator; and
- one closed concrete Linux/POSIX adapter for the exact I3-qualified retained
  reference runtime product.

The adjacent ROS-free contracts in `manyforge_core` additionally define the
bounded owner-to-owner substrate that this package will consume:

- strongly separated Apply, bootstrap, stage, publication, runtime-epoch, and
  configuration-fence identities/generations;
- focused Manager-to-scene-owner Apply and canonical-empty bootstrap ports,
  including the read-only pre-Begin prerequisite observation;
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

All three SQLite owners are internally synchronized because several NRT
clients may race on one store. Their direct constructors, Close/destruction,
and `ExternallyProvisionedManagerStores` share one recursive process-local
lifecycle gate. Direct construction holds it from read-only preflight through
WAL configuration, schema initialization, and first integrity verification;
Close/destruction holds it through checkpoint and close, including sidecar
removal when it owns the last handle; and the composition root holds it across preliminary classification,
all three ordered nested opens, and partial-construction cleanup. This prevents
one same-process opener from mistaking a peer's transient pre-WAL rollback
journal for crash residue, inspecting a WAL that last-close is removing, or
invalidating the root's validate-existing-peers-before-creating-missing-peers
order. Cooperating multi-process root startup/classification is additionally
serialized through the stable-state-directory lock. Already-open processes
coordinate ordinary database operations through SQLite/WAL. Overlap between
one process's root teardown and another process's root startup is not qualified
by this foundation; it requires the future operational process-lifecycle owner
and forked shutdown/replacement coverage. Each owner has a distinct embedded
schema artifact, uses WAL with `synchronous=FULL`, and enforces fixed
cardinality and database page-image ceilings. Before any read-write open or
persistent PRAGMA, each owner performs an exact read-only identity, schema,
metadata, resource-limit, and cell preflight. Clean at-rest WAL databases use
an immutable connection so
inspection cannot create sidecars. A WAL crash residue without SHM is
validated through a bounded private copy; source database and WAL bytes remain
untouched. A live database whose WAL and SHM already exist is read coherently
in place; SQLite may update transient coordination bytes in that existing SHM,
but the preflight does not create a sidecar or change database/WAL/header
content. Schema-empty files are adoptable only when both SQLite identity
headers remain zero. Orphan WAL/SHM/journal files, SHM without WAL,
rollback-journal residue, and over-ceiling main/WAL images fail closed before a
missing peer can be created.

The lifecycle lock order is stable-state-directory `flock` (when the
composition root is used), then the process-local lifecycle mutex, then each
store's per-instance mutex and SQLite transaction or close. No operation
acquires those locks in reverse. Direct concurrent first creation from separate
processes outside `ExternallyProvisionedManagerStores` is not a supported
startup path and has no per-database lock-file fallback. The
constructor's process-local acquisition and SQLite busy waits are bounded to
ten seconds; Close/destruction intentionally waits for the process-local gate
so teardown cannot abandon a live SQLite handle merely because lifecycle-gate
acquisition timed out. This does not claim that either Close/destruction or the
composition root's cross-process directory `flock` is time-bounded, nor does it
turn an underlying SQLite close failure into a successful close.

The mechanical filesystem/WAL inspection is supplied by the adjacent
`manyforge_persistence` library so other semantic owners can reuse the same
qualified startup primitive. Manager still owns all three schemas, records,
limits, and recovery decisions; the shared library interprets none of them.

The bootstrap journal validates its exact schema, immutable rows, typed
identities, canonical binding order, and semantic digests on every reopen. The
Apply journal validates immutable public-request/Apply identity, contiguous
per-Apply generations, exact predecessor projection, and candidate retained
identity. The publication store separately validates generation continuity,
each receipt digest, and the singleton head. Database files are opened without
following a final-path symlink; SQLite 3.37 or newer and
`SQLITE_OPEN_NOFOLLOW` are mandatory.

`ExternallyProvisionedManagerStores` is the composition-root startup helper.
It requires a trusted, existing, non-symlink stable-state directory; opens the
bootstrap journal, Apply journal, and publication store at fixed filenames
with the exact `CellId` derived from a sealed
`CanonicalEmptyRetainedBundleAdmission`; verifies all three stores; and `fsync`s
the directory before exposing them. There is no caller-supplied parallel cell
identity. The running store set also retains the sealed admission digest,
bundle ID, and graph revision, so another otherwise-valid package for the same
cell cannot be substituted through the coordinator. It neither makes the three
databases one transaction nor infers cross-store recovery. Deployment
bootstrap remains responsible for loading the admission first and for
directory ownership and permissions. When any subset of stores already
exists, startup uses a cell-neutral directory lock and read-only SQLite
classification to validate every initialized peer before creating or
initializing any missing or schema-empty store. A foreign-cell startup attempt
therefore cannot poison the directory with a mismatched database.

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

The Apply journal stores the exact immutable named transaction and predecessor
projection, the canonical affected scope, the complete exact Kernel-fence
attempt, the accepted fence bound to that attempt, the immutable aggregate
runtime-convergence intent, and bounded complete staged-ready generations. The
focused `ManagerApplyFenceCoordinator`
commits the intent and attempt before each Acquire it invokes and never returns
the fence until the acceptance is durable. A durable acceptance replays without
another Kernel call. An attempt-only restart first uses the read-only Kernel
reconciliation query: `accepted_exact` is appended, only authoritative
`not_seen` permits an exact persisted-request retry, and conflicting,
uncertain, malformed, or mismatched evidence fails closed. Acquire uncertainty
is a distinct typed error retaining the exact recovery query and every observed
Kernel result. This ordering applies only to Acquire calls made through this
coordinator; it does not claim global first-effect ownership. One
`(request_owner_epoch, request_id)` maps to exactly one intent; an exact
duplicate replays the original digest, and another payload fails without
another journal write. Reusing one `ApplyId` requires its next contiguous
generation. The runtime records are parent-bound to the accepted fence and
predecessor public pointer; exact replay cannot change their plan or reuse
foreign evidence. The journal intentionally stops before scene-stage input and
evidence, activation selection, named-Apply publication/finalize, fence release,
or terminal outcome. Its resource envelope is at most 256 intents/fence
attempts, 64 domains, 256 bindings, and 1,024 resources per attempt, with at
most 4,096 staged-ready sets globally and 16 per Apply, bounded text fields,
and a 64 MiB main database page image.

`ManagerApplyRuntimeConvergenceCoordinator` independently reloads the sealed
predecessor/candidate products, derives the complete candidate runtime plan,
rehydrates the accepted Kernel fence, persists that aggregate before a process
effect, observes every process before any managed start, and joins fresh direct
status with Manager-local presence, dependency, and predecessor-publication
evidence. It journals a complete staged-ready set only when every candidate is
serving, healthy, role-complete, command-fenced, dependency-current, and within
the fixed receiver-local age bounds. A missing report remains pending; foreign,
contradictory, or publication-drift evidence fails closed. The current closed
coordinator admits only the build-qualified retained POSIX reference product;
unsupported external or replacement products fail before fence or process
effects.

`RetainedPosixRuntimeUnitAdapter` is the first concrete process/progress port,
not a generic subprocess API. It independently reloads and verifies the exact
retained bundle, manifest, artifact, query, and host contract before mutating
one trusted Manager-private runtime root. It uses one product-subject/product/
configuration slot, a bounded cross-process lock, an atomically materialized
0500 script, and a durable boot/PID/Linux-start-time/adapter-epoch record. A
forked child cannot exec before that record is durable. Exact surviving state
is reconciled before a start, fixed crash-temporary names are recoverable, and
PID reuse or a previous boot cannot match. Private fixed stdin/stdout channels
accept only the qualified `status`/`shutdown` protocol; a progress observation
is returned only from a newly received exact status record. Manager
disappearance closes stdin, so a replacement first observes the old
incarnation disappear and then starts a fresh epoch rather than fabricating a
replacement status channel.

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

`CanonicalEmptyBootstrapCoordinator::BeginOrRecover` is a bounded Manager-side
I4 ordering proof. It verifies that the sealed admission, its bootstrap
journal and publication store, and the publication head identify the same
cell; observes the scene owner’s exact
command-fenced prerequisites; mints at least 128 bits of random bootstrap
identity; and persists the exact empty-binding intent before any effectful
external call or Begin. It then reconciles scene-owner status, repeats the
prerequisite observation immediately before no-native-effect Begin, and
records the exact accepted target durably. Crash replay and concurrent
coordinators reuse the single durable winner. Before acceptance the publication
head must be empty; after the exact accepted target is durable, an exact
generation-1 bootstrap receipt is allowed solely as crash-recovery evidence
and must agree with the scene-owner phase. Absence, mismatch, invalid
phase/publication ordering, conflicting later state before durable acceptance,
or projection uncertainty fails closed.

`AdvanceOrRecover` handles the bounded zero-native remainder. Each invocation
performs at most one mutating scene-owner operation: it either invokes Rebuild,
waits on an observed rebuild, or publishes the exact generation-1 receipt and
invokes Finalize. For this canonical-empty zero case only,
private-materialization and retired-command-surface evidence digests are
deterministic, domain-separated proofs over the admitted package, bootstrap
identity, exact scene target, and explicit zero counts; this is not a general
materializer or command-surface retirement implementation. The scene-owner
Finalize contract independently rereads the exact current durable receipt; the
focused test owner exercises that obligation even when the coordinator has
just published the receipt. Rebuild/Finalize lost responses reconcile only
through exact later status, publication replay is exact and idempotent, and
uncertainty, mismatched status, a foreign publication, corrupt durable state,
or premature publication fails closed.

## Deliberate non-claims

These checkpoints have no ROS types or callbacks. The only concrete process
effect is the closed retained-POSIX reference product above; it exposes no
caller-selected command, path, arguments, environment, working directory, or
active command surface. There is still no operational Component Manager
process, production named-Apply scene transition, external-runtime adapter,
obsolete-runtime retirement, controller/robot lifecycle, motion effect, or
restored lane wiring. The scene owner used for the focused bootstrap proof is
test-only.
The lower-level journal still treats bundle and graph identities as opaque;
only the external store composition root and coordinator require the sealed,
verified I3 admission. The coordinator does not retain the scene owner's
logical-scene value, implement native projection adapters, or directly open
scene writes and leases. Those openings remain the scene owner's responsibility
inside a successful Finalize after its independent receipt and health checks.

The focused Apply journal itself has no effect surface. Its fence/runtime
coordinators acquire or recover the configuration fence and converge only the
closed reference runtime; they do not make a named Apply operational. The
focused publication store implements only publication persistence and
current-head reread. These slices do not implement a production scene owner,
complete Apply coordinator, private native materialization,
obsolete-command-surface retirement, fence release, terminal outcome, or M0
behavior. Opening the external store set creates no bootstrap intent and
publishes nothing.

The old generic raw persistence fixture and its `FoundationHead` compare-and-
swap surface have been removed rather than retained as compatibility
architecture. The former production Python runtime-observation reducer is also
removed now that this C++ package is selected as the semantic owner. The
independent sources under `verification/composable_cell_i4/` remain test-only
reference fixtures and are never a fallback runtime owner.

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
