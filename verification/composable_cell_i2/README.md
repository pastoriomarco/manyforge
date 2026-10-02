# Composable Cell I2 Binding Contract Tests

I2 of the Composable Cell Runtime Implementation Plan is closed. Its final
qualification evidence (exact source identities, lane results, independent
review, and closure digests) is recorded permanently in ManyForge Specs under
`docs/implementation/composable-cell-i2-final-qualification-evidence-2026-08-04.md`.
The qualification-manifest apparatus that produced that evidence was frozen
against a RoboPlan revision that is no longer selected and has been removed;
the Specs record, not this directory, is the authority for the historical I2
claim.

This directory now holds only the live direct contract tests for the sole
ROS-free `manyforge_core_py` pybind11 module:

- `binding_contract_tests.py` exercises the exact staged extension supplied
  through the `MANYFORGE_I2_BINDING_*` environment variables documented in the
  module docstring. Collection performs no product import; execution fails
  closed when those inputs are absent or aliased.

The tests are executed by the binding lanes, not collected by the ordinary
Python suite:

- `scripts/ci/run-core-bindings.sh` (reached from `./scripts/test-all.sh
  bindings`) builds and stages the Release extension and runs the full module
  together with the `manyforge_core/python/tests` binding tests.
- `scripts/ci/run-core-bindings-contract.sh` regenerates the binding API
  contract outputs and runs the generated-stub parity and forbidden-surface
  cases.
- `scripts/ci/run-core-bindings-sanitizer.sh` builds one ASan/UBSan or TSan
  extension and runs the lifetime, released-GIL exception, fresh-interpreter,
  or concurrent-determinism cases against it.

All three lanes run inside the repository development image.
