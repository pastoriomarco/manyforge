# PLC Boolean-Write Extension Reference

> **Non-operative example:** this folder is not an installable external node.
> It contains no `node.py`, no v1 descriptor, and no executable-package
> manifest. Implementation resolution is unavailable until I5/H0 and addressed
> execution is unavailable until I6/N.

The historical example demonstrated a nonblocking Python leaf that obtained a
deployment-owned PLC client from the blackboard. That mechanism was deleted
with the v1 path loader and `runtime_resources` registry. It must not be copied
back into production code.

For current portable authoring, the built-in v2 `io.set_output` entry already
represents the same intent:

- select a `target_role` from the Program Binding;
- select a semantic `resource_role` on that target;
- provide the boolean `value`;
- require `io.gpio.set_output` at `target_binding` scope; and
- invoke the `kernel.command_target` owner.

Provider selection, endpoint addresses, credentials, PLC tags, and runtime
handles are deployment/compiler concerns. They are neither node parameters nor
blackboard values.

## Files in this reference

- [`node.yaml`](node.yaml) is entirely commented. It shows the intended v2
  catalog-entry shape but deliberately parses as no descriptor, because the
  closed extension-package manifest/compiler is not implemented yet.
- [`reference_semantics.py`](reference_semantics.py) is a dependency-neutral
  pure reducer for the historical start, completion, failure, timeout, and
  cancellation decisions. It performs no I/O and is not a runner.
- [`tests/test_fake_plc.py`](tests/test_fake_plc.py) keeps those pure acceptance
  cases active without importing `py_trees`, loading a source path, or creating
  a provider handle.

Do not add this directory to a runtime path, invoke the removed
`manyforge nodes validate` command, or uncomment `node.yaml` and claim it is a
valid extension package.

## Restoration mapping

The exact historical source is parent commit
`153195a692c50e606f50e40bd755da6081dd24c5`, tree
`c0daa2440275c2635f022c90dd9d71bff5d43b6d`:

- `node.py` and its `ExamplePlcWriteBool` class are evidence for the future
  thin NRT client; they are not retained as dormant executable code.
- `node.yaml` is evidence for user-facing name/description, boolean value,
  bounded cancellation, and I/O capability intent. Its v0 runtime/module/class,
  provider alias, local Stop policy, and unscoped capability fields never
  return.
- the four historical fake-PLC tests map to the pure reducer here and to I6/N
  owner-client integration; Kernel Stop behavior is separately requalified at
  I8/P5.
