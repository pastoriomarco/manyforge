# RoboPlan 0.6.0 Pinocchio scratch direct initialization

Status: exact UBSan reproduction confirmed; smallest correction passes the
focused sanitizer, Release, binding, retained-consumer, and performance rows;
not yet submitted upstream

ManyForge tracking ID: `RP-UP-060-004`

## Exact upstream source and dependency closure

- Repository: `https://github.com/open-planning/roboplan.git`
- Release: `0.6.0`
- Commit: `d2e5162eda2f51f5599ecd60375b65346d0b2944`
- Tree: `60b5791cd34288fc19eea2777a2fc6867c3af0b2`
- Affected files:
  - `roboplan_rrt/src/constraints.cpp:12-60`
  - `roboplan_simple_ik/src/simple_ik.cpp:11-15`
- Dependency closure: the E1-pinned Pinocchio/Coal installation in AMD64
  image
  `sha256:9dcf29cd84e7df0c9539a0edb872afce6b3bcf2b4da62ea57731f7bfba4f7c1c`,
  exact ProxSuite `v0.7.3`, and exact TOPPRA `0.6.8`.

The failure and correction were tested on top of independent E1 corrections
0005, 0006, and 0008. This patch does not overlap them.

## Minimal reproduction and observed failure

Build RoboPlan core, RRT, and the upstream RRT test with
`-fsanitize=address,undefined -fno-omit-frame-pointer`. Preserve the Eigen
allocator ABI used by the prebuilt Pinocchio library, then run
`RoboPlanConstrainedRRTTest.DisplacementMatchesForwardKinematics` with UBSan
halt-on-error.

Clean 0.6 fails while constructing `PoseConstraint`, before the test performs
constraint math:

```text
Eigen/src/Cholesky/LLT.h:66:49: runtime error:
  load of value 65535, which is not a valid value for type 'ComputationInfo'
pinocchio::Data::operator=(...)
roboplan::PoseConstraint::PoseConstraint(...) at constraints.cpp:56
RoboPlanConstrainedRRTTest::SetUp() at test_rrt.cpp:345
```

The exact source shape is:

```cpp
pinocchio::Data data_;
// ... later in the constructor body
data_ = pinocchio::Data(model);
```

`SimpleIk` contains the same default-construction-then-assignment sequence.
The repository-wide Pinocchio scratch audit found no other remaining member
with this shape: `Scene` is corrected by E1 patch 0005, `CollisionContext` and
OInK construct their scratch directly, and local temporaries already receive a
model at construction.

## Root cause and impact

Default `pinocchio::Data` does not represent model-sized initialized scratch.
Assigning a fully initialized `Data(model)` over it enters Eigen-generated
assignment for internal solver objects and reads an indeterminate
`ComputationInfo` value. This is undefined behavior inside the dependency.

An adapter cannot validate it away: every normal PoseConstraint or Simple IK
construction executes the dependency's constructor. A worker could classify a
crash, but deliberately retaining preventable undefined behavior would weaken
reliability and add replacement latency on an ordinary request.

## Smallest patch

Initialize each `data_` member directly from the constructor's Scene model and
delete the later assignment. There is no new type, allocation policy, public
option, algorithm, exception, or ManyForge-specific contract.

- Patch:
  `verification/composable_cell_e1/patches/0009-direct-initialize-roboplan-060-pinocchio-scratch.patch`
- SHA-256:
  `af8526dad047c046c6cd5a33a0513a3c94fca321da22c658d54e0efa33a5f858`
- Scope: two implementation files, three insertions and three deletions.
- Application: cleanly applies to the exact 0.6 source and after E1 patches
  0005, 0006, and 0008.

## Focused regression and sanitizer evidence

With patch 0009 applied to the exact candidate:

- focused ASan/UBSan: native lifetime 5/5, RRT 17/17 including two
  aggregate/default-state contracts, and Simple IK 4/4;
- focused Release: RRT 15/15 and Simple IK 4/4;
- complete source-installed Python corpus: 47/47;
- common selected-adapter conformance and five historical Scene-generation
  behaviors pass;
- the earlier cumulative C++ 239/239 and retained-consumer 52/52 results remain
  pre-correction input; the selected broad replay is pending; and
- five fresh performance cohorts include this exact source and disclose a
  release-level joint-RRT regression of about 12-13%, while Simple IK remains
  comparable. That delta is not attributed to this direct-initialization patch.

The focused TSan binary compiled but the GCC runtime exited before `main` with
`ThreadSanitizer: unexpected memory mapping` under this Docker host, both with
ordinary PIE and the one bounded non-PIE retry. No TSan-clean claim is made.
The patch changes construction of instance-private scratch rather than shared
synchronization; concurrent isolated contexts remain covered by the five-case
native lifetime suite and the warmed-worker stress evidence.

## Compatibility and performance risk

The public signatures, object layout, result semantics, exceptions, and
bindings do not change. Direct initialization is the normal Pinocchio
construction path and removes one whole-object assignment. No attributable
performance cost is introduced by the three-line construction correction; the
fresh record separately discloses the broader release-level RRT regression.
The intended observable change is removal of undefined behavior during
construction.

## Intended upstream contract and removal predicate

Every model-dependent `pinocchio::Data` member is constructed directly from
its final owning model; no constructor assigns model-sized scratch over a
default object with indeterminate Eigen solver state.

The local patch can be removed when a later exact RoboPlan release directly
constructs the PoseConstraint and Simple IK scratch (or provides an equivalent
sanitizer-clean implementation), the raw UBSan reproducer no longer reports,
and the focused RRT, Simple IK, binding, and performance rows remain green.
Upstream response time is not an E1 dependency.
