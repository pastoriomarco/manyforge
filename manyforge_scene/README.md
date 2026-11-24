# ManyForge logical-scene owner

`manyforge_scene` is the ROS-free semantic owner for ManyForge logical-scene
state. It owns the production canonical-empty bootstrap, named structural
Apply handoff, logical-scene transactions, and the current-binding
overlay used when a finalized Manager-managed native host is replaced. It is
the replacement direction for the legacy Core `SceneAuthority`, not a second
owner, compatibility facade, or fallback.

The owner's public constructor accepts the deployment-owned sealed
`CanonicalEmptyRetainedBundleAdmission`; there is no bare query constructor.
It derives and binds the admission's cell/bundle/graph identities and
admission digest, and requires Begin's complete retained bundle to equal the
owned verified bundle. It holds the Scene in memory only: the immutable
bootstrap facts plus the named-Apply, scene-transaction, and
managed-runtime-recovery rows. Nothing of the Scene is written to disk or
survives the process; every start begins from the canonical empty scene.
Begin atomically rechecks the empty, command-fenced prerequisites and uses
the public Core logical-scene identity
codec, which in turn uses the shared `manyforge_content_identity` RFC 8785 and
SHA-256 implementation. No caller supplies a scene ID, content hash, policy,
revision, or counter.

Scene writes and projection leases are derived closed until Finalize rereads
the requested publication generation through the read-only active-publication
capability, byte-compares the current receipt, and records that exact
receipt. A historical or mismatched receipt leaves both gates closed. Exact
Begin, Rebuild, Finalize, and Status replay is answered from those rows.

The current-head read and the scene commit are intentionally not one
transaction. The required owner protocol closes that interval:
Manager is the sole active-publication writer, and any successor publication
must first enter the scene owner's serialized, quiesced transition. During
canonical-empty bootstrap no successor is admissible until this Finalize
completes. The real-Manager integration proof exercises that sole-writer
ordering and verifies that the receipt stored by the scene owner remains the
generation-one current head when the gates open.

Rows, generations and identifiers are fixed-bounded. A commit whose
resulting scene would encode larger than 384 KiB, the bound of the Scene
section in the Manager's status stream, is refused with `scene_too_large` and
changes nothing.

Not implemented yet: component/passive-frame and
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
