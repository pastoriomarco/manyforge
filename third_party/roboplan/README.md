# RoboPlan dependency manifest

ManyForge selects exact RoboPlan 0.8.0 as the foundation for the current
planner dependency closure. The selection does not give RoboPlan scene,
validation, collision, cancellation, dispatch, or product authority; those
remain behind focused ManyForge adapters. The current pin is RoboPlan 0.8.0,
commit `2a6b447e648b1a3337f10cbab995cda0c0a71d55` (`ROBOPLAN_PINNED_REF` in
`scripts/setup-docker.sh`). The 0.6 dossiers under `upstream/` are kept only
as historical evidence; there is no runtime selector or automatic fallback
between RoboPlan generations.

`scripts/setup-docker.sh` materializes the complete source closure below into
`.docker/`. Both production Dockerfiles verify canonical source contents,
licenses and Git identities, apply exactly the checked patch manifest, and
build without configure-time network fallback.

## Selected source closure

- RoboPlan 0.8.0:
  `2a6b447e648b1a3337f10cbab995cda0c0a71d55`, tree
  `56319fede36b09817b343eefdf43215ee0173f95`
- ProxSuite v0.8.2 (RoboPlan 0.8 requires `>=0.8.2,<0.9`):
  `6ae71b84730bf30453d70764073d186f5fe7ad69`, tree
  `9415ea0be7a30660eb98d3247132304f07de0a5e`, annotated tag object
  `6a45b115194ceb8a0c273ee4f7e4481229a63598`. It has no submodules.
- jrl-cmakemodules v2.3.0 (ProxSuite 0.8's CMake toolbox, the version its
  lock file selects): `984234828e97557b82d30262ffa3a851b6be2383`, tree
  `ec1728de4c2bf3081ed547f29b49e51d32edade9`. Passed to ProxSuite as
  `JRL_CMAKEMODULES_SOURCE_DIR`; build-time only, nothing is installed.
- TOPPRA 0.6.10:
  `8121e9af72f5aa589b515aa15e0dc408344c3220`, tree
  `bb6b2027b5834755c9db2cae0a9a6d924d5f8db7`
- nanoflann 2.0.1 (RoboPlan 0.8 RRT K-D trees, `>=2.0.1` required):
  `18f4edf753e813644a7a12711a86139777a8ef00`, tree
  `aee5f414ac32a44eea1618ee6117680d7dda435a`. Header-only and linked
  privately; given to RoboPlan as `FETCHCONTENT_SOURCE_DIR_NANOFLANN` with
  `find_package(nanoflann)` disabled, so neither a configure-time fetch nor
  Ubuntu's older copy can be selected.

Recorded choices for the remaining native dependencies:

- cereal: not part of the closure. ProxSuite is built with
  `PROXSUITE_BUILD_WITH_SERIALIZATION=OFF` (as RoboPlan's own ProxSuite fetch
  path does); OInK does not serialize. RoboPlan's `libcereal-dev` package
  dependency only serves prebuilt ROS ProxSuite binaries.
- tl::expected: RoboPlan's bundled header (part of the verified RoboPlan
  source); `find_package(tl-expected)` is disabled.
- TinyXML2: the image's private static TinyXML2 11.0.0 build
  (`/opt/manyforge/private-tinyxml2-11`), which keeps ROS's XML ABI separate.
- Pinocchio 4.1, Coal 3.0.3 and Eigen are unchanged.

The canonical native and ROS images also pin `pin==4.1.0`,
`libpinocchio==4.1.0`, `coal==3.0.3`, and `libcoal==3.0.3`. Both images
constrain the complete cmeel graph, run `pip check`, and first build a temporary
disconnected C++ closure from the exact sources above with RoboPlan Python
bindings disabled. Only the exact external ProxSuite and TOPPRA prefixes are
retained from that closure. RoboPlan's split CMake closure is built in
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
ProxSuite, jrl-cmakemodules, TOPPRA and nanoflann stay exact. The setup and
Docker build visibly enter `development_override`, skip only the selected
RoboPlan identity claim, still apply the checked patch series, retain
disconnected dependency resolution, and cannot produce release evidence. An
incompatible override fails rather than silently dropping patches.

## Carried 0.8 patches

`patches.sha256` is authoritative for order and content. These small
dependency-internal changes are carried:

1. `0010-construct-roboplan-080-scene-scratch-without-assignment.patch`
   constructs model-bound Pinocchio Scene scratch only after its owning models
   exist (0.8 reassigns it in `refreshCollisionData`). Remove after a released
   upstream source supplies equivalent defined construction and the focused
   Scene, mutation, lifetime, sanitizer, binding, and performance evidence
   passes without it. The 0.6 reproduction remains in:
   `upstream/roboplan-060-scene-scratch-construction-ubsan.md`.
2. `0011-bound-roboplan-080-toppra-output-samples.patch`
   rejects non-finite solver output and proves the output cardinality bound
   before integer conversion or allocation. It bounds TOPP-RA only: RoboPlan
   0.8's Bounded Cartesian timing samples its own trajectory and is not
   patched: the planning deadline bounds it (the worker process is killed)
   and ManyForge rejects any result above `maximum_trajectory_samples`. Remove after an upstream release supplies the
   same derived-output guard and the finite/extreme-input, allocation, TOPPRA,
   binding, sanitizer, and containment evidence passes without it. Dossier:
   `upstream/roboplan-060-toppra-output-sample-bound.md`.
3. `0013-direct-initialize-roboplan-080-pose-constraint-scratch.patch`
   directly initializes PoseConstraint Pinocchio scratch (moved to
   `roboplan_core` in 0.8) instead of assigning into default-created state.
   Remove after an upstream release supplies equivalent construction and the
   constrained/unconstrained RRT, sanitizer, binding, and conformance evidence
   passes without it. Historical dossier:
   `upstream/roboplan-060-pinocchio-scratch-direct-initialization.md`.
4. `0014-describe-image-owned-native-dependencies.patch`
   removes three wheel-level `cmeel` requirements whose native libraries are
   deliberately supplied by the image's verified closure instead: system
   Eigen/yaml-cpp and exact source-built TOPPRA 0.6.10. This keeps `pip check`
   truthful without installing a second TOPPRA generation or placeholder
   distribution metadata. The build requirements remain unchanged and the
   dependency-stack verifier continues to prove the selected native linkage.
5. `0015-roboplan-080-scene-collision-pair-margins.patch`
   adds `Scene::setCollisionPairMargins`, which sets coal's per-pair
   `security_margin` and rebuilds the broadphase, and makes `SceneContext`
   copy those margins, so RRT and path checks keep the ManyForge collision
   clearance. Remove after an upstream release provides an equivalent per-pair
   margin that reaches `SceneContext`-based planners and the focused Scene,
   RRT, worker-protocol and kernel clearance rows pass without it. Dossier:
   `upstream/roboplan-070-scene-collision-pair-margins.md`.
6. `0016-toppra-velocity-bound-hook.patch` adds
   `TOPPRAOptions::max_path_velocity`, an optional per-grid-point bound on the
   TOPP-RA path speed, so ManyForge holds the TCP at 0.98 of its speed limit
   inside the time-scaling (RP-3). Remove after an upstream release provides
   an equivalent bound or extra-constraint hook and the focused TOPP-RA and
   candidate-computer rows pass without it. Dossier:
   `upstream/roboplan-080-toppra-path-velocity-bound.md`.
7. `0017-runtime-collision-pair-toggle.patch` adds
   `Scene::setCollisionPairsActive`, which flips the active flag of existing
   collision pairs without reordering the pair list, resetting the 0015
   margins or rebuilding anything, and makes `SceneContext` copy the flags.
   The ManyForge worker uses it for each request's contact-policy pairs, so a
   contact-policy change keeps the worker. Remove after an upstream release
   provides equivalent runtime activation that reaches `SceneContext`-based
   planners and the focused Scene, worker-protocol and candidate-client rows
   pass without it. Dossier:
   `upstream/roboplan-080-runtime-collision-pair-toggle.md`.

8. `0018-cartesian-time-optimal-path-velocity-bound.patch` adds
   `CartesianPlannerOptions::toppra_max_path_velocity`, forwarded to 0016's
   `TOPPRAOptions::max_path_velocity` in TimeOptimal mode, so ManyForge
   `time_optimal` Cartesian moves hold the TCP at 0.98 of its speed limit
   (CT-1, RP-3). Remove after an upstream release provides an equivalent
   option and the candidate-computer time_optimal rows pass without it.
   Dossier: `upstream/roboplan-080-cartesian-time-optimal-path-velocity-bound.md`.
9. `0019-bounded-cartesian-curvature-speed-cap.patch` makes Bounded Cartesian
   timing hold the tool's lateral acceleration v²/r on curves within
   `max_linear_acceleration`. Each sample's path speed is capped from the
   curvature of the circle through its neighbouring waypoints, with the same
   half-limit split as the joint q'' term, so only the curved part of a run
   slows down. RP-2 rounded corners then no longer stretch the whole move.
   Remove after an upstream release bounds curvature acceleration in Bounded
   timing and the RoboPlan curve test and the candidate-computer
   constant_speed rounded-corner rows pass without it. Dossier:
   `upstream/roboplan-080-bounded-cartesian-curvature-speed-cap.md`.

Patches 0010, 0011, 0013, 0015, 0016, 0017, 0018 and 0019 are offered upstream. The 0.7 series
(same numbers, `070` names) and the retired 0012 DynoTree initialization patch
(RoboPlan 0.8 replaced DynoTree with nanoflann) are bound to their exact Git
blobs by the external-delta ledger. Patches 0010 through 0013 descend from the
retired 0.6 series 0005, 0006, 0008 and 0009, likewise bound in the ledger;
byte-identical E1 evidence copies remain under
`verification/composable_cell_e1/patches/` for the frozen 0.6 restoration
closure.

Each dossier freezes the clean reproduction, impact, smallest patch,
compatibility risk, focused evidence, intended upstream contract, and
objective removal predicate.

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
