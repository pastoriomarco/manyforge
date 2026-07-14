# RoboPlan dependency manifest

ManyForge's production container builds use the exact RoboPlan source revision
below. `scripts/setup-docker.sh` materializes that revision into
`.docker/roboplan`; the Dockerfiles apply only the patches listed here after
verifying the machine-readable SHA-256 manifest in `patches.sha256`.

## Base revision

- Upstream: `https://github.com/open-planning/roboplan.git`
- Upstream release name: `0.5.1`
- Commit: `e06d1a2f1b749c38742aee9e401bbe97fa967f7b`
- Tag form: lightweight, unsigned tag pointing directly at the commit
- ManyForge disposition: exact commit reviewed and pinned; no branch, moving
  tag, or Docker-time source substitution is a production dependency

The canonical native and ROS images install `pin==4.1.0` and
`libpinocchio==4.1.0`. On the qualified aarch64 PyPI lane, Pinocchio 4.1.0
resolves against `coal==3.0.3` and `libcoal==3.0.3`; Coal 3.0.4 is not published
there as a wrapper wheel. Both Dockerfiles constrain the complete cmeel graph,
run `pip check`, and build every RoboPlan package from this one source
generation.

`ROBOPLAN_REF` remains a development-only setup override. Evidence or release
qualification produced from an override must identify that source and cannot
be reported as the pinned RoboPlan 0.5.1 build.

## Carried patches

Patches are applied in this exact order:

1. `patches/0001-explicit-isolated-scene-clone.patch`
   - SHA-256: `cfa676dca94d8a8e9d4f61fb76b4988dc0810a5484b4d3a42416b5d294363344`
   - Purpose: make `Scene` cloning explicit and reconstruct all mutable
     pointer-bearing collision scratch against the new generation.
   - Remove only after an upstream release provides an equivalent deep-clone
     and transactional replacement contract and the ManyForge source-lifetime,
     mutation-isolation, replacement, and strict sanitizer tests pass without
     this patch.
2. `patches/0002-cartesian-planner-cooperative-cancellation.patch`
   - SHA-256: `344123b71a6eb2bcf8217ca7aea77413f68e70c5e96a397f6deb856c0cf8c9bf`
   - Purpose: add bounded cooperative cancellation and monotonic progress
     observation to RoboPlan 0.5 Cartesian path resolution. ManyForge exposes
     bounded Cartesian mode only; upstream time-optimal TOPP-RA remains
     unadvertised until its solver can observe cancellation while running.
   - Remove only after upstream exposes equivalent nonblocking cancellation
     and progress hooks, and the pre-start, in-flight, deadline, and structured
     error regressions pass against that release.
3. `patches/0003-own-collision-context-scene-generation.patch`
   - SHA-256: `0e83a7b644bf033e9d00d3ec1f638f8126593602c04416ab9fc70e98d186c81c`
   - Purpose: make planner collision contexts structurally retain the exact
     immutable scene generation whose model storage they reference.
   - Remove only after upstream collision contexts and all planner users carry
     equivalent shared ownership and the source-destruction/context-lifetime
     sanitizer suite passes without the patch.
   - RoboPlan 0.5.1 changes SimpleIK timeout completion in the same source
     region. The rebased hunk preserves upstream's best-candidate-on-timeout
     behavior while changing only collision-context ownership.
4. `patches/0004-configurable-source-build-cmake-arguments.patch`
   - SHA-256: `8867a87ea695b4196e9d05b424c1a4be0219fe9017ab21fa8e89aeb7b89ac086`
   - Purpose: let the source-build helper accept explicit CMake arguments, so
     container builds can disable dependency tests without an untracked `sed`
     mutation of the source tree.
   - Remove after upstream's build helper supports extra CMake arguments, or
     after ManyForge replaces the helper with a checked package build that
     requires no source mutation.
5. `patches/0005-construct-scene-scratch-without-copy-assignment.patch`
   - SHA-256: `e13cdc712cd98c411f8f1a0dbc2ee9944fed536745fc92ddf64e60c41ed2b537`
   - Purpose: construct model and geometry scratch after their owning models
     exist instead of copy-assigning Pinocchio data with uninitialized Eigen
     solver state; keep the public reference API while making replacement and
     broadphase pointer ownership structural.
   - Remove only after upstream constructs scene scratch without copy
     assignment and the strict ASan/UBSan scene-construction plus clone/context
     lifetime suite passes without this patch.
6. `patches/0006-bound-cartesian-duration-arithmetic.patch`
   - SHA-256: `c0b699d5f2eb28edda43290710b328aca6837cee2290ec5b3a339b1d1051a581`
   - Purpose: keep bounded Cartesian duration and step-budget arithmetic in a
     checked floating representation until finite allocation and integer bounds
     are proven, including every whole-trace slowdown retry.
   - Remove only after upstream rejects non-finite or excessive Cartesian
     reference and servo budgets before allocation/conversion, and the extreme
     speed/acceleration regression passes without this patch.
7. `patches/0007-explicit-serialized-collision-query-view.patch`
   - SHA-256: `d14a8eb7b2f99dfc37a59475e0add3514fe1d96d78a818ca4c7ce658cbc2e14d`
   - Purpose: preserve isolated collision scratch as RoboPlan's general,
     concurrency-safe default while allowing a caller that exclusively owns a
     request-private Scene generation to select and serially reuse that Scene's
     existing broadphase workspace across IK, RRT, shortcutting, and retiming.
     Exact Scene-generation identity checks reject mixed geometry/workspace
     inputs before any planning work, randomized IK restarts remain on the
     selected workspace, serialized views retain their Scene generation, and
     borrowed contexts/views reject temporary owners at compile time. The
     patch also makes the source-build helper resolve each freshly installed
     package prefix instead of silently compiling downstream packages against
     an older system install.
   - Remove only after upstream exposes an equivalent typed scratch-ownership
     contract, exact-generation guards, and ordered source-build prefixing, and
     the parity, mismatch, allocation, sanitizer, and Python API-compatibility
     tests pass without this patch.
   - Its SimpleIK query-view overload is rebased around 0.5.1's timeout loop;
     timeout still returns the nearest valid candidate already observed, and
     randomized restarts plus collision checks remain on the selected exact
     scene-generation workspace. Direct regressions prove both candidate
     preservation and no-candidate failure at the deadline.

Changing the base revision, patch content, digest, or order requires rebuilding
both native and ROS/Isaac images and repeating RP5 dependency, parity,
sanitizer, and performance qualification. A patch is deleted rather than left
as a dormant compatibility path once its removal criteria are met.
