# RoboPlan dependency manifest

ManyForge selects RoboPlan 0.6.0 as its sole planner-dependency baseline. The
selection does not give RoboPlan scene, validation, collision, cancellation,
dispatch, or product authority; those remain behind focused ManyForge adapters
at their restoration gates. E1 is a dependency checkpoint and does not make a
planner product lane available.

`scripts/setup-docker.sh` materializes the complete source closure below into
`.docker/`. Both production Dockerfiles verify canonical source contents,
licenses, Git identities and declared submodule state, apply exactly the
checked patch manifest, and build without configure-time network fallback.

## Selected source closure

- RoboPlan 0.6.0:
  `d2e5162eda2f51f5599ecd60375b65346d0b2944`, tree
  `60b5791cd34288fc19eea2777a2fc6867c3af0b2`
- ProxSuite v0.7.3:
  `b93d7778ffc3299d84b5cb0851022a29bf24a596`, tree
  `66f7dcd274efe11d1e6816b73e49cf9663671988`, annotated tag object
  `d032c98f65d9c90575bf06dffa564693e502f875`
- ProxSuite `cmake-module` gitlink:
  `30795190916d0297092e37bc1f7b50f5d76fc09c`; this is the only populated
  ProxSuite submodule. Its nanobind and cereal gitlinks remain uninitialized.
- TOPPRA 0.6.8:
  `cbbc89d46208fddfaa3e1aab52ab44553751b510`, tree
  `4fee36b7f1bdd39cb5f10bc103254634757c417e`

The canonical native and ROS images also pin `pin==4.1.0`,
`libpinocchio==4.1.0`, `coal==3.0.3`, and `libcoal==3.0.3`. Both images
constrain the complete cmeel graph, run `pip check`, and first build a temporary
disconnected C++ closure from the exact sources above with RoboPlan Python
bindings disabled. Only the exact external ProxSuite and TOPPRA prefixes are
retained from that closure. Every RoboPlan distribution is then built and
installed once from the same patched source generation; those recorded Python
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

## Carried 0.6 patches

`patches.sha256` is authoritative for order and content. Four small
dependency-internal fixes are carried:

1. `0005-construct-roboplan-060-scene-scratch-without-assignment.patch`
   (`00ca95fea84dcf1180d80f8abd2782897a3911fef80ae639131d75f834250184`)
   constructs model-bound Pinocchio Scene scratch only after its owning models
   exist. Remove after a released upstream source supplies equivalent defined
   construction and the focused Scene, mutation, lifetime, sanitizer, binding,
   and performance evidence passes without it. Upstream dossier:
   `upstream/roboplan-060-scene-scratch-construction-ubsan.md`.
2. `0006-bound-roboplan-060-toppra-output-samples.patch`
   (`ba886434bdca4eafb6f59ce630b1b4a4957a2c7ffb0c7e3c140a1bab5c5fac6a`)
   rejects non-finite solver output and proves the output cardinality bound
   before integer conversion or allocation. ManyForge still owns request and
   worker resource policy. Remove after an upstream release supplies the same
   derived-output guard and the finite/extreme-input, allocation, TOPPRA,
   binding, sanitizer, and containment evidence passes without it. Dossier:
   `upstream/roboplan-060-toppra-output-sample-bound.md`.
3. `0008-initialize-roboplan-060-dynotree-variant-state.patch`
   (`e705472bfa05feecc8cf0b669018968966687a5a911047307be01f421ac2cbdd`)
   gives fixed and dynamic DynoTree state deterministic aggregate-compatible
   initialization. Remove after an upstream release provides equivalent
   initialization and the warning-as-error, RRT, sanitizer, binding, and
   conformance rows pass without it. Dossier:
   `upstream/roboplan-060-dynotree-variant-initialization.md`.
4. `0009-direct-initialize-roboplan-060-pinocchio-scratch.patch`
   (`af8526dad047c046c6cd5a33a0513a3c94fca321da22c658d54e0efa33a5f858`)
   directly initializes PoseConstraint and SimpleIK Pinocchio scratch instead
   of assigning into default-created state. Remove after an upstream release
   supplies equivalent construction and constrained/unconstrained RRT, IK,
   sanitizer, binding, and conformance evidence passes without it. Dossier:
   `upstream/roboplan-060-pinocchio-scratch-direct-initialization.md`.

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
  only -0.45% and throughput by +0.74%; exact 0.6 exposes no safe reuse hook
  and stays within E1 memory and containment bounds. I6 retains ordinary
  allocation, latency, throughput, and memory regression gates without
  recreating that lifetime-bearing dependency API.

Historical tests and behavior mappings remain acceptance input for I6; the old
patch bodies are not kept as dormant compatibility code. Changing selected
source, dependency content, patch content, digest, or order requires rotating
the exact closure identity and rerunning every affected native and ROS/Isaac
qualification row.
