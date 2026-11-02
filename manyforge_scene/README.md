# ManyForge logical-scene owner

`manyforge_scene` is the ROS-free semantic owner for ManyForge logical-scene
state. It owns the production canonical-empty bootstrap, named structural
Apply handoff, logical-scene transactions, and the durable current-binding
overlay used when a finalized Manager-managed native host is replaced. It is
the replacement direction for the legacy Core `SceneAuthority`, not a second
owner, compatibility facade, or fallback.

The owner's public constructor accepts the deployment-owned sealed
`CanonicalEmptyRetainedBundleAdmission`; there is no bare query constructor.
It derives and durably binds the admission's cell/bundle/graph identities and
admission digest, and requires Begin's complete retained bundle to equal the
owned verified bundle. Its separate bounded SQLite database contains the
immutable bootstrap facts plus append-only named-Apply, scene-transaction, and
managed-runtime-recovery evidence. Begin atomically rechecks the empty,
command-fenced prerequisites and uses the public Core logical-scene identity
codec, which in turn uses the shared `manyforge_content_identity` RFC 8785 and
SHA-256 implementation. No caller supplies a scene ID, content hash, policy,
revision, or counter.

Scene writes and projection leases are derived closed until Finalize rereads
the requested publication generation through the read-only active-publication
capability, byte-compares the current receipt, and durably commits that exact
receipt. A historical or mismatched receipt leaves both gates closed. Exact
Begin, Rebuild, Finalize, and Status replay is reconstructed from the durable
rows after restart.

The current-head read and the scene database commit are intentionally not one
cross-database transaction. The required owner protocol closes that interval:
Manager is the sole active-publication writer, and any successor publication
must first enter the scene owner's serialized, quiesced transition. During
canonical-empty bootstrap no successor is admissible until this Finalize
completes. The real-Manager integration proof exercises that sole-writer
ordering and verifies that the receipt stored by the scene owner remains the
generation-one current head when the gates open.

Storage startup reuses `manyforge_persistence` only for bounded mechanical
read-only SQLite/WAL preflight. This package alone owns its schema and recovery
decisions. Its database is not joined transactionally or semantically with a
Component Manager store. Database page image, WAL, row cardinality,
generations, and identifiers are all fixed-bounded and corruption fails
closed. An exact schema-free SQLite file left by a crash during first creation
is safely initialized on restart; any foreign application/version marker or
partial schema is rejected. Initial schema publication also fsyncs the owning
directory.

Construction and WAL-topology-changing Close/destruction share one
process-local scene-store lifecycle gate. Construction takes the lifecycle gate
before SQLite. Close/destruction takes the per-instance mutex, then the
lifecycle gate, then SQLite; ordinary scene transactions take only the
per-instance mutex and SQLite. No path holds the lifecycle gate while waiting
for an instance mutex, so an injected publication reader cannot reverse this
order through another scene-store lifecycle operation. This prevents a
same-process constructor from preflighting a WAL while last-close removes it.
Constructor acquisition is bounded to ten seconds; Close/destruction waits for
the gate so teardown cannot abandon a live SQLite handle merely because gate
acquisition timed out. Concurrent teardown in one process and startup in
another process is not qualified by this I4 foundation and requires the future
operational process-lifecycle owner plus forked replacement coverage.

Not implemented in the current N5c checkpoint: component/passive-frame and
carried-object activation, the complete execution-facing projection-lease
path, sustained-history lifetime management, remote/ROS adapters, or robot
effects. Native participants are reached only through the injected exact-
identity resolver; the scene owner contains no process discovery, placement,
authorization, or transport selection. No legacy owner copy, compatibility
path, or fallback is retained in this package.

Build and test in the ManyForge development container:

```bash
cmake -S manyforge_scene -B /tmp/manyforge-scene-build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build /tmp/manyforge-scene-build --parallel
ctest --test-dir /tmp/manyforge-scene-build --output-on-failure
```
