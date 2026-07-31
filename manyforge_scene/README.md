# ManyForge logical-scene owner

`manyforge_scene` is the ROS-free semantic owner for ManyForge logical-scene
state. This I4 slice implements only the production canonical-empty bootstrap
path. It is the replacement direction for the legacy Core `SceneAuthority`,
not a second owner, compatibility facade, or fallback.

The owner's public constructor accepts the deployment-owned sealed
`CanonicalEmptyRetainedBundleAdmission`; there is no bare query constructor.
It derives and durably binds the admission's cell/bundle/graph identities and
admission digest, and requires Begin's complete retained bundle to equal the
owned verified bundle. Its separate bounded SQLite database contains one immutable
revision-one scene acceptance, one immutable zero-native rebuild record, and
one immutable finalization receipt. Begin atomically rechecks the empty,
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

Not implemented in this slice: nonzero native projection materialization,
named Apply/stage/activation/retirement, public scene edits, projection lease
objects, ROS adapters, or process/Kernel/controller/robot effects. Destructive
legacy Core owner/DTO demolition belongs to the same I4 cutover; no copy,
compatibility path, or fallback is retained in this package.

Build and test in the ManyForge development container:

```bash
cmake -S manyforge_scene -B /tmp/manyforge-scene-build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build /tmp/manyforge-scene-build --parallel
ctest --test-dir /tmp/manyforge-scene-build --output-on-failure
```
