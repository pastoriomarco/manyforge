# RoboPlan dependency manifest

ManyForge selects exact RoboPlan 0.7.0 as the foundation for the current
planner dependency closure. The selection does not give RoboPlan scene,
validation, collision, cancellation, dispatch, or product authority; those
remain behind focused ManyForge adapters. The current pin is RoboPlan 0.7.0,
commit `099dfa5ccd80b191eb3d6da9f4795f808a63913f` (`ROBOPLAN_PINNED_REF` in
`scripts/setup-docker.sh`). The 0.6 dossiers under `upstream/` are kept only
as historical evidence; no 0.6 build can be selected, and there is no runtime
selector or automatic fallback between RoboPlan generations.

`scripts/setup-docker.sh` materializes the complete source closure below into
`.docker/`. Both production Dockerfiles verify canonical source contents,
licenses, Git identities and declared submodule state, apply exactly the
checked patch manifest, and build without configure-time network fallback.

## Selected source closure

- RoboPlan 0.7.0:
  `099dfa5ccd80b191eb3d6da9f4795f808a63913f`, tree
  `3ce46cce8b51b21a0eb7b803858cb0750524e5be`
- ProxSuite v0.7.3:
  `b93d7778ffc3299d84b5cb0851022a29bf24a596`, tree
  `66f7dcd274efe11d1e6816b73e49cf9663671988`, annotated tag object
  `d032c98f65d9c90575bf06dffa564693e502f875`
- ProxSuite `cmake-module` gitlink:
  `30795190916d0297092e37bc1f7b50f5d76fc09c`; this is the only populated
  ProxSuite submodule. Its nanobind and cereal gitlinks remain uninitialized.
- TOPPRA 0.6.10:
  `8121e9af72f5aa589b515aa15e0dc408344c3220`, tree
  `bb6b2027b5834755c9db2cae0a9a6d924d5f8db7`

The canonical native and ROS images also pin `pin==4.1.0`,
`libpinocchio==4.1.0`, `coal==3.0.3`, and `libcoal==3.0.3`. Both images
constrain the complete cmeel graph, run `pip check`, and first build a temporary
disconnected C++ closure from the exact sources above with RoboPlan Python
bindings disabled. Only the exact external ProxSuite and TOPPRA prefixes are
retained from that closure. RoboPlan 0.7's split CMake closure is built in
dependency order (`roboplan_common`, `roboplan_example_models`,
`roboplan_core`, then the consumers); the empty aggregate metapackage is not a
C++ build input. Every RoboPlan Python distribution, including the aggregate
metadata package, is then built and installed once from the same patched
source generation; those recorded Python
distributions supply the sole retained RoboPlan extensions, DSOs, headers, and
CMake packages. The image check rejects unrecorded extensions, a competing
`/usr/local/lib` RoboPlan DSO set, mixed distribution identities, or unexpected
linkage to anything outside the selected planner, ProxSuite, TOPPRA, Pinocchio,
and Coal roots.

`ROBOPLAN_REF` is a development-only setup override. It changes RoboPlan only;
ProxSuite and TOPPRA stay exact. The setup and Docker build visibly enter
`development_override`, skip only the selected RoboPlan identity claim, still
apply the checked patch series, retain disconnected dependency resolution, and
cannot produce release evidence. An incompatible override fails rather than
silently dropping patches.

## Carried 0.7 patches

`patches.sha256` is authoritative for order and content. Six small
dependency-internal changes are carried:

1. `0010-construct-roboplan-070-scene-scratch-without-assignment.patch`
   (`57b699cb2dec5604f091ad5170c6e187caa7132cc14dd2b0b58b726c04d4930b`)
   constructs model-bound Pinocchio Scene scratch only after its owning models
   exist. Remove after a released upstream source supplies equivalent defined
   construction and the focused Scene, mutation, lifetime, sanitizer, binding,
   and performance evidence passes without it. The 0.6 reproduction remains in:
   `upstream/roboplan-060-scene-scratch-construction-ubsan.md`.
2. `0011-bound-roboplan-070-toppra-output-samples.patch`
   (`7ddf2e2f899678876d36016a334743d03daff9ed8c9c7c51b39f5dc1936ecb05`)
   rejects non-finite solver output and proves the output cardinality bound
   before integer conversion or allocation. ManyForge still owns request and
   worker resource policy. Remove after an upstream release supplies the same
   derived-output guard and the finite/extreme-input, allocation, TOPPRA,
   binding, sanitizer, and containment evidence passes without it. Dossier:
   `upstream/roboplan-060-toppra-output-sample-bound.md`.
3. `0012-initialize-roboplan-070-dynotree-variant-state.patch`
   (`aab3c280517c7759ab28f094a87cb3aa6bd261f8ec7e727ff12d9adfc7db980a`)
   gives fixed and dynamic DynoTree state deterministic aggregate-compatible
   initialization. Remove after an upstream release provides equivalent
   initialization and the warning-as-error, RRT, sanitizer, binding, and
   conformance rows pass without it. Dossier:
   `upstream/roboplan-060-dynotree-variant-initialization.md`.
4. `0013-direct-initialize-roboplan-070-pose-constraint-scratch.patch`
   (`db7acbac01e3868495966bc79fa0fbc0f0d37f0a3af88596a5a76b66a29998ce`)
   directly initializes PoseConstraint Pinocchio scratch instead of assigning
   into default-created state. RoboPlan 0.7's SimpleIK now uses its own
   per-solve `SceneContext`, so the old SimpleIK half is deliberately absent.
   Remove this patch after an upstream release supplies equivalent construction
   and the constrained/unconstrained RRT, sanitizer, binding, and conformance
   evidence passes without it. Historical dossier:
   `upstream/roboplan-060-pinocchio-scratch-direct-initialization.md`.
5. `0014-describe-image-owned-native-dependencies.patch`
   removes three wheel-level `cmeel` requirements whose native libraries are
   deliberately supplied by the image's verified closure instead: system
   Eigen/yaml-cpp and exact source-built TOPPRA 0.6.10. This keeps `pip check`
   truthful without installing a second TOPPRA generation or placeholder
   distribution metadata. The build requirements remain unchanged and the
   dependency-stack verifier continues to prove the selected native linkage.
6. `0015-roboplan-070-scene-collision-pair-margins.patch`
   (`fdcb0fb10d84f2df9f3f33bb7c37c8c9a5e4ddd9cd9767595894e06a63f93fb1`)
   adds `Scene::setCollisionPairMargins`, which sets coal's per-pair
   `security_margin` and rebuilds the broadphase, and makes `SceneContext`
   copy those margins, so RRT and path checks keep the ManyForge collision
   clearance. Remove after an upstream release provides an equivalent per-pair
   margin that reaches `SceneContext`-based planners and the focused Scene,
   RRT, worker-protocol and kernel clearance rows pass without it. Dossier:
   `upstream/roboplan-070-scene-collision-pair-margins.md`.

Patches 0010 through 0013 are the rebased 0.7 successors of the retired 0.6
series 0005, 0006, 0008 and 0009. Those 0.6 bodies are no longer kept in this
directory: the external-delta ledger binds each to its exact Git blob at
ManyForge commit `6ff42c4cf70be415159624adba1428100a2ee116`, the last commit
that applied them, and byte-identical E1 evidence copies remain under
`verification/composable_cell_e1/patches/` for the frozen 0.6 restoration
closure.

Each dossier freezes the clean reproduction, impact, smallest patch,
compatibility risk, focused evidence, intended upstream contract, and
objective removal predicate. Upstream submission follows E1; the reproducible
local baseline does not depend on upstream response time.

## Retired 0.5 integration and authority patches

The former 0.5.1 production series—patches 0001 through 0007, including the
already-retired build-helper patch 0004—remains exactly traceable in Git at
ManyForge commit `ec2d17baa0d5f5b6d8126b608b82814371d758ee` and in
`verification/composable_cell_e1/external_delta_ledger.v1.json`. It is not a
second selectable dependency generation.

- Scene clone/replacement becomes fresh native-scene reconstruction per exact
  retained projection generation at the I6 adapter boundary.
- Cancellation/progress uses a qualified cooperative hook when available and
  otherwise a warmed, bounded, killable generation-private worker process.
- Collision contexts and every borrowed native object stay private to a worker
  generation whose Scene lifetime encloses them.
- Build arguments, exact sources, prefixes, and same-soname isolation are owned
  by the checked ManyForge source-closure builder.
- The serialized collision-view optimization is retired. Its isolated 0.5.1
  A/B reduced allocation calls by 6.6% and bytes by 17.6%, but changed p50 by
  only -0.45% and throughput by +0.74%; exact 0.6 exposed no safe reuse hook
  and stayed within E1 memory and containment bounds. I6 retains ordinary
  allocation, latency, throughput, and memory regression gates without
  recreating that lifetime-bearing dependency API.

Historical tests and behavior mappings remain acceptance input for I6; the old
patch bodies are not kept as dormant compatibility code. Changing selected
source, dependency content, patch content, digest, or order requires rotating
the exact closure identity and rerunning every affected native and ROS/Isaac
qualification row.
