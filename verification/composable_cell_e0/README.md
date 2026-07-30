# E0 Python-Boundary Feasibility Proof

This directory is non-production qualification evidence for the bounded E0
checkpoint. It builds two disposable modules over the same representative
ManyForge types and operations. It does not create a production binding
choice, compatibility layer, feature flag, or runtime path.

The proof covers the actual `CellId`, strict parser, empty-cell identity and
structural-qualification calls, closed-schema compilation, actual
`ExecutionSnapshot` and `KernelEvent` materialization, and a representative
RoboPlan `Pose3DTO` dictionary round-trip. It also covers enum equality/hash,
trusted-stage
nonconstructibility, optional `None`, structured exceptions, borrowed and
shared lifetimes, a representative concurrent released-GIL schema compile,
synthetic released-GIL wait overlap, asyncio observation-only cancellation,
imports, and clean interpreter shutdown.
`verify_production_coexistence.py` separately imports the two actual production
pybind11 modules from their clean build/install directories. The separate
`e0_nanobind_roboplan_peer` module provides a bounded two-module nanobind
coexistence check in both import orders. Neither subset is a substitute for
building and testing both complete production modules during an approved
atomic cutover.

Run it only in the documented ManyForge development image. Pin
`MANYFORGE_SOURCE_DIR` to the exact clean commit being evaluated; a dirty
source tree invalidates the retained result.

```bash
export E0_SOURCE_COMMIT=<full-clean-manyforge-commit>
python3 verification/composable_cell_e0/verify_source_state.py \
  --source-root /workspace \
  --expected-commit "${E0_SOURCE_COMMIT}"
python3 verification/composable_cell_e0/capture_environment.py \
  --image-reference manyforge-dev:latest \
  --image-id <immutable-image-id>
cmake -S verification/composable_cell_e0 \
  -B /tmp/manyforge-e0-build \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DE0_BUILD_NANOBIND_PEER=ON \
  -DMANYFORGE_SOURCE_DIR=/workspace \
  "-DCMAKE_CXX_COMPILER_LAUNCHER=python3;/workspace/verification/composable_cell_e0/compiler_launcher.py"
python3 verification/composable_cell_e0/measure_command.py \
  cmake --build /tmp/manyforge-e0-build --parallel 1
python3 verification/composable_cell_e0/count_binding_surface.py \
  --source-root /workspace
PYTHONPATH=/tmp/manyforge-e0-build \
  python3 verification/composable_cell_e0/verify.py \
    --source-root /workspace
PYTHONPATH=/tmp/manyforge-e0-build \
  taskset -c 0 python3 verification/composable_cell_e0/summarize_trials.py \
    --source-root /workspace --trials 8
python3 -m nanobind.stubgen \
  -m e0_nanobind \
  -i /tmp/manyforge-e0-build \
  -O /tmp/manyforge-e0-build/stubs
python3 verification/composable_cell_e0/verify_stub.py \
  --stub /tmp/manyforge-e0-build/stubs/e0_nanobind.pyi
python3 verification/composable_cell_e0/verify_nanobind_coexistence.py \
  --module-dir /tmp/manyforge-e0-build
python3 verification/composable_cell_e0/verify_nanobind_coexistence.py \
  --reverse \
  --module-dir /tmp/manyforge-e0-build
CMAKE_ARGS="-DMANYFORGE_SOURCE_DIR=/workspace -DE0_BINDING=pybind" \
SKBUILD_BUILD_DIR=/tmp/manyforge-e0-wheel-build-py \
  python3 -m pip wheel --no-deps --no-build-isolation \
  --wheel-dir /tmp/manyforge-e0-py-wheel \
  verification/composable_cell_e0
CMAKE_ARGS="-DMANYFORGE_SOURCE_DIR=/workspace -DE0_BINDING=nanobind" \
SKBUILD_BUILD_DIR=/tmp/manyforge-e0-wheel-build-nb \
  python3 -m pip wheel --no-deps --no-build-isolation \
  --wheel-dir /tmp/manyforge-e0-nb-wheel \
  verification/composable_cell_e0
```

For incremental binding-TU evidence, first build once, remove only the
corresponding generated object with Ninja's clean tool, then rebuild that
target through the retained command-measurement wrapper:

```bash
ninja -C /tmp/manyforge-e0-build -t clean \
  CMakeFiles/e0_pybind.dir/pybind_module.cpp.o
python3 verification/composable_cell_e0/measure_command.py \
  cmake --build /tmp/manyforge-e0-build \
    --target e0_pybind --parallel 1
ninja -C /tmp/manyforge-e0-build -t clean \
  CMakeFiles/e0_nanobind.dir/nanobind_module.cpp.o
python3 verification/composable_cell_e0/measure_command.py \
  cmake --build /tmp/manyforge-e0-build \
    --target e0_nanobind --parallel 1
```

The E0 decision record pins the source commit, toolchain, exact commands,
raw-summary hashes, and measurements. Generated build trees, modules, wheels,
and stubs are not production artifacts and are not committed.

Build both repository-native production modules from the same clean source
commit and pinned image. The paths below are disposable:

```bash
cmake -S /workspace/manyforge_core \
  -B /tmp/manyforge-e0-production/core \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF
cmake --build /tmp/manyforge-e0-production/core \
  --parallel 1
cmake -S /workspace/manyforge_deployment \
  -B /tmp/manyforge-e0-production/deployment \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF \
  -DMANYFORGE_CORE_INCLUDE_DIR=/workspace/manyforge_core/include
cmake --build /tmp/manyforge-e0-production/deployment \
  --parallel 1
E0_PYBIND_CMAKE_DIR="$(python3 -m pybind11 --cmakedir)"
cmake -S /workspace/manyforge_core/python \
  -B /tmp/manyforge-e0-production/core-python \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -Dpybind11_DIR="${E0_PYBIND_CMAKE_DIR}" \
  -DMANYFORGE_CORE_DIR=/tmp/manyforge-e0-production/core \
  -DMANYFORGE_CORE_INCLUDE_DIR=/workspace/manyforge_core/include \
  -DMANYFORGE_DEPLOYMENT_DIR=/tmp/manyforge-e0-production/deployment \
  -DMANYFORGE_DEPLOYMENT_INCLUDE_DIR=/workspace/manyforge_deployment/include
cmake --build /tmp/manyforge-e0-production/core-python \
  --target manyforge_core_py --parallel 1
E0_CMEEL_PREFIX="$(
  python3 -c \
    'import pathlib,site; print(pathlib.Path(site.getsitepackages()[0]) / "cmeel.prefix")'
)"
E0_FMT_SOURCE=<read-only-fmt-11.0.2-source-at-commit-0c9fce2ffefecfdce794e1859584e25877b7b592>
test "$(git -C "${E0_FMT_SOURCE}" rev-parse HEAD)" = \
  0c9fce2ffefecfdce794e1859584e25877b7b592
test -z "$(git -C "${E0_FMT_SOURCE}" status --porcelain=v1 \
  --untracked-files=all)"
CMAKE_PREFIX_PATH="/usr/local:${E0_CMEEL_PREFIX}:${CMAKE_PREFIX_PATH:-}" \
  cmake -S /workspace/manyforge_roboplan \
    -B /tmp/manyforge-e0-production/roboplan \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DMANYFORGE_ENABLE_ROS2_CONTROL=OFF \
    -Dpybind11_DIR="${E0_PYBIND_CMAKE_DIR}" \
    -DFETCHCONTENT_SOURCE_DIR_FMT="${E0_FMT_SOURCE}"
cmake --build /tmp/manyforge-e0-production/roboplan \
  --target manyforge_roboplan_py --parallel 1
```

The production baseline must remain network-isolated. The pinned development
image installs RoboPlan but intentionally removes its build source, while
`manyforge_roboplan` still consumes fmt directly. Therefore the retained run
supplies the exact clean fmt 11.0.2 source commit above as an explicit,
read-only build input; it records the commit and a deterministic source-tree
hash alongside the image identity. An unpinned download invalidates the
baseline.

Then verify the current production pybind11 coexistence baseline in both
initialization orders:

```bash
LD_LIBRARY_PATH="${E0_CMEEL_PREFIX}/lib:${LD_LIBRARY_PATH:-}" \
  python3 verification/composable_cell_e0/verify_production_coexistence.py \
    --module-dir /tmp/manyforge-e0-production/core-python \
    --module-dir /tmp/manyforge-e0-production/roboplan
LD_LIBRARY_PATH="${E0_CMEEL_PREFIX}/lib:${LD_LIBRARY_PATH:-}" \
  python3 verification/composable_cell_e0/verify_production_coexistence.py \
  --reverse \
  --module-dir /tmp/manyforge-e0-production/core-python \
  --module-dir /tmp/manyforge-e0-production/roboplan
```

The production baseline is invalid if either module came from a
different source commit or dependency image. Record both module hashes and
`ldd` output. In particular, do not reuse a host build whose dynamic-library
SONAMEs differ from the pinned image. This baseline does not prove coexistence
after a nanobind migration: verdict B still requires both complete production
modules to be migrated, built, and tested together before the cutover.

Run the same nanobind proof once in a separate Debug build and select only
`e0_nanobind`; interpreter exit must be clean and emit no leak diagnostic:

```bash
cmake -S verification/composable_cell_e0 \
  -B /tmp/manyforge-e0-debug-nb \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DE0_BINDING=nanobind \
  -DMANYFORGE_SOURCE_DIR=/workspace
cmake --build /tmp/manyforge-e0-debug-nb --parallel 1
PYTHONPATH=/tmp/manyforge-e0-debug-nb \
  python3 verification/composable_cell_e0/verify.py \
    --source-root /workspace --module e0_nanobind \
    >/tmp/manyforge-e0-debug-nb/verify.stdout \
    2>/tmp/manyforge-e0-debug-nb/verify.stderr
test ! -s /tmp/manyforge-e0-debug-nb/verify.stderr
```

For comparable clean-target build evidence, configure separate build trees
with one binding target apiece:

```bash
cmake -S verification/composable_cell_e0 \
  -B /tmp/manyforge-e0-build-py \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DE0_BINDING=pybind \
  -DMANYFORGE_SOURCE_DIR=/workspace \
  "-DCMAKE_CXX_COMPILER_LAUNCHER=python3;/workspace/verification/composable_cell_e0/compiler_launcher.py"
python3 verification/composable_cell_e0/measure_command.py \
  cmake --build /tmp/manyforge-e0-build-py --parallel 1
cmake -S verification/composable_cell_e0 \
  -B /tmp/manyforge-e0-build-nb \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DE0_BINDING=nanobind \
  -DMANYFORGE_SOURCE_DIR=/workspace \
  "-DCMAKE_CXX_COMPILER_LAUNCHER=python3;/workspace/verification/composable_cell_e0/compiler_launcher.py"
python3 verification/composable_cell_e0/measure_command.py \
  cmake --build /tmp/manyforge-e0-build-nb --parallel 1
```

## Retained final evidence

The final E0 run uses an exact clean proof-source commit mounted read-only into
the pinned image. Raw output is first written outside the source tree, then a
reviewed decision record retains the source/image identities, bounded
summaries and one ledger hash. Raw logs may remain in the ignored external
qualification archive; generated modules, build trees, and wheels are not
repository artifacts. This keeps the measured tree clean while making any
later raw-record substitution visible through the ledger.

Retain these text or JSON records:

- exact source commit and empty `git status --short`;
- image reference plus immutable image ID/digest, architecture, Python,
  compiler, CMake, Ninja, pybind11, nanobind, and relevant native-library
  versions;
- `binding-surface.json` from the clean recount;
- separate clean-target configure/build logs, `measure_command.py` summaries,
  and per-TU `E0_COMPILER_METRIC` rows;
- combined-module functional verification and the eight counterbalanced raw
  trials plus their summary;
- both incremental binding-object rebuild logs and measurements;
- stub-generation log, generated `.pyi` text, and the focused stub-parity
  result;
- wheel-build logs plus wheel/module byte sizes and SHA-256 digests;
- Debug nanobind build/verification output and clean interpreter-exit status;
- exact fmt source commit, clean status, and deterministic source-tree hash;
- repository-native core and RoboPlan module build logs, hashes, `ldd` output,
  and coexistence results in both import orders; and
- bounded two-module nanobind coexistence results in both import orders; and
- one SHA-256 ledger over every retained raw record.

The decision record reports the ledger hash, exact command transcript, source
commit, image ID, and bounded summary. A rerun is accepted only when every
functional row passes; timing and RSS values are observations, not golden
thresholds. Generated binaries and wheels are deleted after their hashes,
sizes, dependency reports, and build commands are retained.

Generate the ledger only after the raw directory is complete, excluding the
ledger output itself:

```bash
python3 verification/composable_cell_e0/hash_evidence.py \
  --evidence-root /tmp/manyforge-e0-evidence \
  --exclude raw-evidence-ledger.json \
  >/tmp/manyforge-e0-evidence/raw-evidence-ledger.json
```
