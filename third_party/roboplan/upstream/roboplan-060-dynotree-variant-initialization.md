# RoboPlan 0.6.0 DynoTree variant-state initialization

Status: exact compiler reproduction confirmed; corrected aggregate-compatible
candidate passes the focused warning-as-error, Release, and ASan/UBSan rows;
package and fresh fair-performance rows pass their invariants; selected broad
consumer replay remains pending the baseline decision; not yet submitted
upstream

ManyForge tracking ID: `RP-UP-060-003`

## Exact affected source

- Repository: `https://github.com/open-planning/roboplan.git`
- Release: `0.6.0`
- Commit: `d2e5162eda2f51f5599ecd60375b65346d0b2944`
- Tree: `60b5791cd34288fc19eea2777a2fc6867c3af0b2`
- Affected file:
  `roboplan_rrt/include/dynotree/StateSpace.h:346-418`
- Affected constructors: default construction of `SO2Squared<double>` and
  fixed-size `RnSquared<double, N>` values before insertion into
  `Combined<double>::spaces`.

The exact reproduction used GCC 12.2.0, CMake 3.25.1, Eigen 3.4.0, and the
RoboPlan 0.6 dependency closure frozen by E1 in the pinned AMD64 image.

## Minimal reproduction and observed failure

Configure a clean 0.6 `roboplan_rrt` Release build with GCC's existing
`-Wmaybe-uninitialized` diagnostics enabled. Compiling `src/rrt.cpp` reports
eight paths through `std::variant` construction. Representative diagnostics
are:

```text
Eigen/src/Core/PlainObjectBase.h:496:9: warning:
  SO2Squared<double>::lb ... may be used uninitialized
Eigen/src/Core/PlainObjectBase.h:496:9: warning:
  SO3Squared<double>::rn_squared...weights ... may be used uninitialized
```

The complete diagnostic traces end at
`Combined<double>::Combined(const std::vector<std::string>&)` lines 1246 and
1254, where temporary state-space objects are moved into the variant.

For a mechanically decisive regression, build the same source with
`-Werror=maybe-uninitialized`. Raw 0.6 fails compilation at those paths. The
minimal correction below builds cleanly with that warning promoted to an
error.

## Root cause and impact

Eigen deliberately leaves default-constructed fixed-size coefficient storage
uninitialized. `SO2Squared` and `RnSquared` contain such matrices and have no
constructor. `Combined` default-constructs those objects and immediately moves
them into a `std::variant`; the generated Eigen move/copy path may evaluate the
indeterminate coefficients even though the selected algorithms do not intend
to use the bounds or weights until later configuration.

This is dependency-internal object construction, not a request or adapter
policy. An outer worker can contain a crash but cannot make movement of
indeterminate numerical state a reliable library operation. The correction
also prevents accidental future use of arbitrary coefficients when a
state-space branch is configured incompletely.

## Smallest correction

Default-initialize `SO2Squared` bounds to zero and `RnSquared` bounds to zero
with neutral unit weights. Dynamic-size vectors remain valid empty vectors;
fixed-size vectors receive deterministic coefficients. No public option,
planner algorithm, type name, allocation, or ManyForge policy changes.

The exact candidate is:

- path:
  `verification/composable_cell_e1/patches/0008-initialize-roboplan-060-dynotree-variant-state.patch`;
- base: clean RoboPlan 0.6.0 commit
  `d2e5162eda2f51f5599ecd60375b65346d0b2944`;
- scope: one embedded DynoTree header, 11 insertions; and
- SHA-256:
  `e705472bfa05feecc8cf0b669018968966687a5a911047307be01f421ac2cbdd`;
  and
- independent application: the patch applies after, but does not overlap,
  E1 patches 0005 and 0006.

## Evidence and compatibility

- Raw exact 0.6 emitted eight `-Wmaybe-uninitialized` paths while building the
  complete disconnected stack.
- The corrected header builds `roboplan_rrt` Release with
  `-Werror=maybe-uninitialized`.
- All 15 RRT and constrained-RRT C++ cases pass with the just-built corrected
  library first in the loader path; the focused aggregate/default-state
  contract adds two cases, for 17/17 under ASan/UBSan.
- The same exact header builds the complete disconnected 0.6 source stack and
  preserves aggregate initialization for fixed- and dynamic-size state.
- The complete corrected package-installed Python corpus passes 47/47, the
  common selected-adapter conformance case passes all 11 logical paths, and
  five fresh performance cohorts bind this exact source/build identity.
- Fresh evidence discloses the broader 0.6 joint-RRT regression rather than
  attributing it to this initialization patch; results from the superseded
  non-aggregate-compatible candidate are not reused as final evidence.
- The selected broad retained-consumer replay remains after the Adopt/No-go
  decision in accordance with the proportional qualification rule.
- The correction does not change the selected RRT algorithms, public
  signatures, struct fields, or result semantics.
- The unit-weight initialization is neutral while `use_weights == false` and
  is overwritten by `set_weights` before weighted operation.

The focused TSan binary compiled but this Docker host's GCC runtime exited
before `main` with `ThreadSanitizer: unexpected memory mapping`; no TSan-clean
claim is inferred. This one-header correction initializes instance state and
does not add shared synchronization. Concurrent isolated-context and warmed-
worker stress evidence remains green.

## Intended upstream contract and removal predicate

Constructing any DynoTree state-space alternative and moving it into the
`Combined` variant does not propagate indeterminate numerical state. The local
patch can be removed when an exact later RoboPlan release initializes the same
state equivalently, the warning-as-error reproducer and RRT corpus pass without
this patch, and the selected sanitizer/performance evidence remains green.

After E1 selects the baseline, maintainers can submit this one-header fix with
the exact before/after compiler output and focused corpus. Upstream response
time is not an E1 dependency.
