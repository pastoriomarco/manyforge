# RoboPlan 0.6.0 TOPPRA output sample arithmetic and allocation bound

Status: reproduction confirmed; smallest candidate passes the focused and
cumulative Release, ASan/UBSan, binding, disconnected-source, containment, and
comparable-performance rows; not yet submitted upstream

ManyForge tracking ID: `RP-UP-060-002`

## Exact affected source

- Repository: `https://github.com/open-planning/roboplan.git`
- Release: `0.6.0`
- Commit: `d2e5162eda2f51f5599ecd60375b65346d0b2944`
- Tree: `60b5791cd34288fc19eea2777a2fc6867c3af0b2`
- Affected file and lines:
  `roboplan_toppra/src/toppra.cpp:326-330` in that exact source
- Affected operation:

  ```cpp
  const auto num_traj_pts =
      static_cast<size_t>(std::ceil(t_final / options.dt)) + 1;
  traj.times.reserve(num_traj_pts);
  traj.positions.reserve(num_traj_pts);
  traj.velocities.reserve(num_traj_pts);
  traj.accelerations.reserve(num_traj_pts);
  ```

The first exact reproduction used GCC 12.2.0, CMake 3.25.1, `pin==4.1.0`,
`libpinocchio==4.1.0`, `coal==3.0.3`, `libcoal==3.0.3`, and TOPPRA 0.6.8 from
the pinned ManyForge AMD64 image. The final submission packet will include the
complete selected dependency manifest and the disconnected exact TOPPRA source
identity rather than rely on those prose version labels.

## Minimal reproduction

Add this focused case to RoboPlan's existing `roboplan_toppra/test/test_toppra.cpp`
fixture at the exact source above:

```cpp
TEST_F(RoboPlanToppraTest, NonFiniteDtDoesNotReachAllocation) {
  const auto path = createTestPathShort();
  auto toppra = PathParameterizerTOPPRA(scene, "arm");

  const auto result =
      toppra.generate(path, {std::numeric_limits<double>::quiet_NaN()});
  ASSERT_FALSE(result.has_value());
}
```

Build `roboplan_example_models`, `roboplan`, and `roboplan_toppra` from the
clean source and run the just-built test with the just-built libraries first
in the loader path:

```text
LD_LIBRARY_PATH=<build/roboplan_toppra>:<install/roboplan/lib>:\
<install/roboplan_example_models/lib>:<dependency-libs> \
  test_toppra \
  --gtest_filter=RoboPlanToppraTest.NonFiniteDtDoesNotReachAllocation
```

Putting the candidate prefixes first is essential. The ManyForge development
image contains an older same-soname RoboPlan generation under `/usr/local`;
allowing that ambient path to win would test a mixed dependency generation,
not this exact source.

## Observed failure

The clean 0.6.0 implementation does not reject NaN because `NaN <= 0.0` is
false. TOPPRA completes, the code attempts to derive a `size_t` from the
non-finite sample count, and the test terminates with an uncaught
`std::vector::reserve` exception instead of receiving `tl::unexpected`:

```text
C++ exception with description "vector::reserve" thrown in the test body.
```

The same site also accepts any finite positive `dt` whose solver-derived
duration implies an excessive output cardinality. A count can therefore become
non-representable at the floating-to-integer conversion or request multiple
large vector reservations without a caller-defined resource bound.

## Expected behavior

Non-finite sampling inputs must be rejected before solver dispatch. Once the
solver derives the final duration, RoboPlan must prove that duration finite and
nonnegative and prove the resulting output cardinality representable and
within the caller's declared limit before integer conversion or allocation.
Failure must be returned through the existing structured error channel.

## Root cause

The existing validation uses ordered comparisons that do not reject NaN and
has no output cardinality option. After solving, it converts
`ceil(t_final / dt)` directly to `size_t` and reserves four output vectors.
The duration is dependency-internal and cannot be fully checked by an outer
adapter before the planner call. Process memory containment limits the effect
of a failure, but it does not make an out-of-range C++ conversion defined or
provide a deterministic structured error.

## Correctness, safety, reliability, and performance impact

- A non-finite or out-of-range floating-to-integer conversion has no portable
  C++ result.
- A merely representable but excessive count can drive large allocations and
  process termination rather than a bounded planner error.
- Cartesian planning calls this parameterizer after nontrivial IK and TOPPRA
  work, so an uncontrolled allocation can waste substantial CPU before failing.
- A warmed worker and cgroup remain defense in depth, but predictable rejection
  avoids worker loss and replacement latency on an invalid request.
- The healthy path adds only finite comparisons and one cardinality comparison
  after TOPPRA has already solved; it adds no allocation or per-sample work.

No physical motion or live simulator was used for the reproduction.

## Smallest intended correction

1. Reject non-finite `dt`, velocity scale, and acceleration scale through the
   existing validation errors before constructing constraints or dispatching
   TOPPRA.
2. Append `max_output_samples` to `TOPPRAOptions`, defaulting to 1,000,000 and
   requiring a value greater than zero. Expose the same backwards-compatible
   keyword/property in the Python binding.
3. Validate the solver-derived final duration.
4. Compute the interval count in `long double`, compare it with
   `max_output_samples - 1`, and only then convert to `size_t` and reserve.

The default admits roughly 2.8 hours at a 10 ms sample period. A product such
as ManyForge applies its stricter request-specific limit in its adapter; the
dependency option is the minimal hook that enforces that policy at the only
site that knows the solver-derived duration and performs the allocations.

The exact candidate is:

- path:
  `verification/composable_cell_e1/patches/0006-bound-roboplan-060-toppra-output-samples.patch`;
- SHA-256:
  `ba886434bdca4eafb6f59ce630b1b4a4957a2c7ffb0c7e3c140a1bab5c5fac6a`;
- scope: five RoboPlan files, 82 insertions and 9 deletions; and
- base: clean RoboPlan 0.6.0 commit
  `d2e5162eda2f51f5599ecd60375b65346d0b2944`, to which `git apply --check`
  succeeds independently of the Scene construction correction.

The source count includes the committed generated Python type stub and the two
focused regressions. The production logic itself is confined to the TOPPRA
options header, implementation, and binding declaration.

## Candidate evidence obtained

The candidate was built in AMD64 Docker image
`sha256:9dcf29cd84e7df0c9539a0edb872afce6b3bcf2b4da62ea57731f7bfba4f7c1c`
against the exact RoboPlan 0.6.0 source plus the separately tracked Scene
construction correction.

Current results are:

- the exact raw 0.6.0 reproduction loads the just-built raw libraries and
  fails with `vector::reserve`;
- the corrected focused Release corpus passes all 17 TOPPRA cases, including
  NaN/infinite inputs, a zero sample budget, and a solved trajectory exceeding
  a one-sample budget;
- the same 17 cases pass under focused ASan/UBSan fail-fast behavior;
  LeakSanitizer is separately recorded unavailable for the exact
  Pinocchio/Coal closure and is not claimed;
- the Python extension builds, accepts `max_output_samples` as a constructor
  keyword and mutable property, and reports the 1,000,000 default; and
- the frozen patch digest exactly matches the candidate diff and applies to
  the clean 0.6.0 source;
- the corrected final exact ProxSuite/TOPPRA/RoboPlan source closure builds
  with network disabled and its source-installed Python corpus passes 47/47;
  an earlier 239/239 C++ replay is retained as pre-correction input and the
  selected broad replay remains pending;
- the 22-case finite-admission/containment suite rejects invalid input before
  native dispatch, binds the caller's exact projection content, bounds
  output/process resources, classifies cancel and deadline, replaces a
  terminated worker, and succeeds afterwards; and
- five fresh interleaved offline control/candidate cohorts show no material
  TOPPRA regression and a material Cartesian improvement; candidate medians
  are 23.678 ms TOPPRA p50 and 28.230 ms Cartesian p50.

The corrected four-patch source and exact disconnected dependency closure have
therefore replayed every affected focused row in the same generation intended
for the maintainer decision. The one broad selected-outcome replay remains
after that decision rather than being repeated for each correction.

## Focused regression and sanitizer evidence required

1. **Passed:** exact raw failure and corrected non-finite-input regression;
2. **Passed:** zero and solver-derived excessive sample-budget regressions;
3. **Passed:** complete 17-case TOPPRA Release and ASan/UBSan corpora;
4. **Passed:** focused Python constructor/property/default compatibility;
5. **Passed:** complete corrected eight-package Release and binding-stub build;
6. **Passed:** disconnected exact TOPPRA 0.6.8 source build with DSO
   attestation and no ambient same-soname substitution;
7. **Passed:** ManyForge pre-dispatch subnormal/extreme input bounds and
   structured error normalization prototype;
8. **Passed:** bounded-worker peak-memory, termination, replacement, and
   post-replacement recovery evidence; and
9. **Passed:** comparable healthy-path TOPPRA/Cartesian evidence shows no
   material regression.

## Compatibility risk

- Existing C++ aggregate initializers remain source-compatible because the new
  field is appended with a default.
- Existing Python calls remain source-compatible because the new keyword is
  appended and optional.
- The C++ struct layout and extension ABI change, so all RoboPlan packages and
  consumers must be rebuilt as one exact generation. ManyForge already
  requires that source-build rule and must prove loader isolation explicitly.
- Requests that previously tried to produce more than the default one million
  samples now return an error. Callers with a justified larger bounded policy
  can set a larger value; unbounded allocation is intentionally no longer the
  default.
- The normal path performs constant-time checks only. The E1 comparison still
  verifies no material timing regression.

## Intended upstream contract

TOPPRA output sampling never converts a non-finite, negative, out-of-range, or
caller-excessive interval count to an integer and never reserves output storage
before proving the complete output cardinality within the declared bound.
Invalid input or solver output is returned through `tl::unexpected`.

This is dependency-level correctness and resource control. It does not transfer
ManyForge planning authority, cancellation semantics, or product policy into
RoboPlan.

## Removal predicate for the local patch

The local patch can be removed only when a later exact RoboPlan release exposes
an equivalent pre-conversion output cardinality bound, the raw reproduction and
complete focused regression/sanitizer corpus pass without the local patch, and
the ManyForge adapter can express its selected resource limit without relying
on worker failure.

## Upstream submission plan

After E1 selects and freezes its RoboPlan baseline:

1. reduce the final qualified change to the smallest upstream-form commit;
2. attach the exact raw reproduction, loader-path proof, dependency closure,
   and before/after result;
3. attach the complete Release, sanitizer, Python, and performance evidence;
4. submit the correction without ManyForge-specific policy or naming; and
5. retain the exact local patch until an upstream release satisfies the
   removal predicate.

Upstream review time is not an E1 dependency. ManyForge remains pinned to one
exact locally qualified source/patch generation in the interim.
