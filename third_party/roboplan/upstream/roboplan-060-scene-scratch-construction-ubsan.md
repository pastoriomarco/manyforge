# RoboPlan 0.6.0 Scene scratch construction undefined behavior

Status: reproduction confirmed; smallest 0.6 patch passes the focused core and
lifetime sanitizers, complete Release/source-installed binding suites,
retained-consumer compatibility, and comparable performance; not yet submitted
upstream

ManyForge tracking ID: `RP-UP-060-001`

## Exact affected source

- Repository: `https://github.com/open-planning/roboplan.git`
- Release: `0.6.0`
- Commit: `d2e5162eda2f51f5599ecd60375b65346d0b2944`
- Tree: `60b5791cd34288fc19eea2777a2fc6867c3af0b2`
- Affected file and line: `roboplan/src/core/scene.cpp:185` in that exact
  source
- Affected construction statements:

  ```cpp
  model_data_ = pinocchio::Data(model_);
  collision_model_data_ = pinocchio::GeometryData(collision_model_);
  ```

The first reproduction used GCC 12.2.0, CMake 3.25.1, `pin==4.1.0`,
`libpinocchio==4.1.0`, `coal==3.0.3`, and `libcoal==3.0.3` from the pinned
ManyForge cmeel closure, and the exact RoboPlan
example-model source from the same 0.6.0 tree. The final submission packet
will include the complete selected image and package manifest rather than rely
on these prose version labels.

## Minimal reproduction

No custom planning request is needed. RoboPlan's existing first Scene test is
already a minimal repository-level reproducer:

```text
roboplan/test/test_scene.cpp
RoboPlanSceneTest.SceneProperties
```

Build `roboplan_example_models` and `roboplan` from the clean source above
with Python bindings disabled and these flags:

```text
-DCMAKE_BUILD_TYPE=RelWithDebInfo
-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g -DEIGEN_MALLOC_ALREADY_ALIGNED=1
-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g -DEIGEN_MALLOC_ALREADY_ALIGNED=1
-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g -DEIGEN_MALLOC_ALREADY_ALIGNED=1
```

Run the just-built test directly with its just-built library first:

```text
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=66 \
  roboplan/test/test_scene \
  --gtest_filter=RoboPlanSceneTest.SceneProperties
```

The failure occurs during fixture setup before the test body performs a
planning or collision operation.

## Observed failure

UBSan reports that Eigen's `ComputationInfo` enum is loaded with a value that
is not valid for that enum. The attributable stack is:

```text
Eigen::LLT::operator=(...)
pinocchio::Data::operator=(...)
roboplan::Scene::Scene(...) at roboplan/src/core/scene.cpp:185
RoboPlanSceneTest::SetUp() at roboplan/test/test_scene.cpp:48
```

The exact diagnostic value is not a contract and may vary with stack contents.
The stable failure is the invalid enum read from the destination object's
default-created Eigen solver state during `pinocchio::Data` copy assignment.

## Expected behavior

Constructing a valid `Scene` from valid URDF, SRDF, package paths, and joint
configuration must not read uninitialized state. Its Pinocchio scratch must be
constructed against the final owning `Model` and `GeometryModel`, and every
broadphase pointer must refer only to live scratch from the same Scene
generation.

## Root cause

`Scene` stores `pinocchio::Data` and `pinocchio::GeometryData` as direct
members. C++ therefore default-constructs them before the constructor body has
parsed the robot model. The constructor later creates model-bound temporary
scratch and copy-assigns it into those default objects. Pinocchio's generated
assignment traverses Eigen solver members in the destination, including status
that the default constructor did not initialize for this use. Reading that
status is undefined behavior.

This is not caused by a ManyForge request or adapter lifetime. Every normal
RoboPlan Scene construction executes the sequence. Process containment can
limit the consequence of a crash, but it cannot make the dependency operation
defined or reliable.

## Correctness, safety, and reliability impact

- The C++ abstract machine provides no behavior guarantee after the invalid
  read. A Release build passing does not make it safe.
- Optimizer, compiler, Eigen, Pinocchio, architecture, and incidental stack
  changes may alter the outcome.
- Every planner mode that constructs a Scene is downstream of this path.
- Treating a sanitizer-detected dependency bug as a worker crash would reduce
  availability and obscure a preventable root cause.
- Reconstructing `GeometryData` after scene geometry changes has the same
  construction concern and additionally requires destroying the old
  broadphase manager before destroying the scratch it references.

No physical motion occurred while reproducing the defect.

## Smallest intended correction

Keep the public accessors and Scene semantics unchanged, but leave the two
scratch owners absent in disengaged `std::optional` storage until the final
models exist and construct each scratch directly with `emplace`. All internal
users dereference that storage. Before reconstructing collision scratch after
add/remove/collision-pair mutations, destroy the broadphase manager that points
into the old scratch; then emplace new `GeometryData` and bind a new manager.

The candidate uses deferred in-place owned storage rather than assigning one
`pinocchio::Data` or `GeometryData` object over another. `std::optional` was
selected over the first `std::unique_ptr` prototype because it preserves raw
0.6's copy/move traits, avoids an extra allocation and pointer indirection, and
still has safe exception unwinding before scratch construction. It does not add
a ManyForge type, planner policy, cancellation hook, clone API, or
serialization optimization.

The exact candidate is:

- path:
  `verification/composable_cell_e1/patches/0005-construct-roboplan-060-scene-scratch-without-assignment.patch`;
- SHA-256:
  `00ca95fea84dcf1180d80f8abd2782897a3911fef80ae639131d75f834250184`;
- scope: two RoboPlan files, 36 insertions and 31 deletions; and
- base: clean RoboPlan 0.6.0 commit
  `d2e5162eda2f51f5599ecd60375b65346d0b2944`, to which `git apply --check`
  succeeds.

## Candidate evidence obtained

The candidate was built in AMD64 Docker image
`sha256:9dcf29cd84e7df0c9539a0edb872afce6b3bcf2b4da62ea57731f7bfba4f7c1c`
with GCC 12.2.0 and the dependency closure named above. The focused
RelWithDebInfo sanitizer build used `-O1 -g1` because compiling the Pinocchio-
heavy Scene translation unit with higher debug/optimization settings exceeded
the bounded compiler cgroup; the normal Release build retained its normal
optimization profile.

Current results are:

- the raw one-test reproducer fails immediately with the UBSan stack above;
- the same reproducer passes with the exact candidate;
- all 47 upstream `roboplan` cases pass under ASan/UBSan: 22 Scene, 7 path,
  6 joint, 5 URDF-limit, 5 filter, and 2 type cases;
- four construction/geometry/collision-pair mutation cases pass 32 consecutive
  repetitions each, for 128 mutation cases under ASan/UBSan;
- raw and corrected 0.6 both remain copy-constructible, copy-assignable,
  move-constructible, and move-assignable at the public C++ type-trait level;
  the adapter will not rely on raw Scene copying because its pointer-bearing
  generation semantics remain a separate lifetime concern; and
- the corrected final four-patch exact-source Release stack builds, including
  generated Python binding stubs; an earlier broad candidate passed all 239
  discovered upstream C++ cases but predates the aggregate-compatible final
  patch 0008 and is retained as pre-correction input rather than a final seal;
- the adapter-shaped native lifetime suite passes 5/5 under ASan/UBSan,
  covering destroyed serialized inputs, fresh reconstruction, wrong
  generation before native dispatch, concurrent isolated contexts, and
  destroyed/reconstructed generations;
- the complete corrected source-installed Python corpus passes 47/47 with
  package metadata available and every module/DSO resolved from the exact
  candidate prefix;
- the earlier 52/52 retained-consumer replay is pre-correction evidence; the
  exact selected-outcome replay remains after the baseline decision; and
- five fresh interleaved comparable offline control/candidate cohorts show no
  material Scene-construction or collision-query regression. Candidate medians
  are 15.285 ms Scene-construction p50 and 0.00637 ms collision-query p50.

LeakSanitizer teardown recursively emitted `AddressSanitizer:DEADLYSIGNAL`
inside the uninstrumented Pinocchio/Coal closure. Focused evidence therefore
uses ASan with `detect_leaks=0:halt_on_error=1` plus UBSan fail-fast behavior.
Compiler-inserted address/undefined checks and nonzero raw-signal exits remain
active, but no LeakSanitizer-clean claim is made. Worker process-lifetime and
RSS bounds provide separate leak/containment evidence. This prevents another
large reporter-output loop without waiving a sanitizer finding.

This is ready for the post-decision upstream-form submission packet. The
remaining E1 work is baseline selection, the selected-image cumulative replay,
and publication of the already-recorded dossier; none changes this defect's
technical disposition.

## Focused regression and sanitizer evidence required

The correction is not ready for upstream submission until all of these pass
against the clean source plus only the candidate patch:

1. **Passed:** the exact one-test reproducer under ASan/UBSan;
2. **Passed:** the complete upstream `roboplan` Scene, path, joint, type,
   filter, and URDF test binaries under ASan/UBSan;
3. **Passed:** repeated construct/destroy and
   add/remove/re-add/reparent/collision-pair mutation under ASan/UBSan;
4. **Passed:** explicit CollisionContext lifetime and generation-isolation
   proof, 5/5 under ASan/UBSan;
5. **Passed:** the corrected final exact-source upstream Release build; the
   earlier 239-case replay is explicitly pre-correction input;
6. **Pending selected outcome:** the exact retained-consumer replay; the prior
   52/52 result remains historical input;
7. **Passed:** 47/47 corrected source-installed Python binding/API cases; and
8. **Passed:** five comparable Scene construction and collision-query cohorts
   show no material regression.

The focused TSan target compiled, but this Docker host's GCC TSan runtime
failed before `main` with `unexpected memory mapping` under both ordinary PIE
and one bounded non-PIE retry. No TSan-clean claim is inferred. The independent
concurrent-context lifetime case and repeated warmed-worker probes are green.
This patch removes undefined construction; it does not make one mutable Scene
safe for concurrent queries or transfer synchronization ownership out of the
adapter.

## Compatibility risk

- Source API: intended unchanged; `getData()` and `getCollisionData()` still
  return the same const references.
- ABI/layout: internal Scene layout changes, so consumers must rebuild against
  the corrected library. ManyForge already builds the exact dependency from
  source and never mixes generations.
- Copy/move traits: confirmed preserved relative to raw 0.6. The separate raw
  pointer-bearing copy semantics are not accepted as a ManyForge adapter
  lifetime mechanism and remain covered by the generation-isolation gate.
- Performance: two scratch-owner constructions occur once per Scene or
  geometry-generation rebuild. The E1 benchmark must show that the ownership
  representation has no material Scene-build or query-path cost.

## Intended upstream contract

RoboPlan Scene construction and collision-scratch reconstruction do not read
or copy-assign from uninitialized model-bound scratch. Scratch is constructed
against its final owning model, and any object holding a pointer into replaced
scratch is destroyed before that scratch.

This contract is planner-library correctness and contains no ManyForge
authority or product behavior.

## Removal predicate for the local patch

The local patch can be removed only when a later exact RoboPlan release
provides an equivalent direct-construction and replacement-lifetime contract,
the standalone reproducer passes without the local patch, and the complete
focused regression, sanitizer, compatibility, and performance rows above pass
against that release.

## Upstream submission plan

After E1 selects and freezes its RoboPlan baseline:

1. reduce the final qualified change to the smallest upstream-form commit;
2. attach this exact reproducer, stack, dependency closure, and before/after
   sanitizer result;
3. attach the focused Release and performance results;
4. submit the fix to RoboPlan without ManyForge-specific naming or policy; and
5. retain the exact local patch until an upstream release satisfies the
   removal predicate.

Upstream review time is not an E1 dependency. ManyForge remains pinned to one
exact locally qualified source/patch generation in the interim.
