# Reviewed node-package example

`example.check_frame_height` asks the current model owner for a declared frame's
cell-frame pose, then checks its height against two bounds. It issues one read
per node attempt. It owns no robot state, geometry, task, worker or physical
effect. A missing/stale owner or malformed response fails explicitly.

The package is selected statically by `manyforge_behavior`. This is a build
extension example, not a runtime plugin installation API. A caller cannot
select an import path, replace a descriptor or hot-reload executable code.

`descriptor.json` declares the catalog entry and its reviewed owner contract.
The effective catalog, executable registry and NodeEnvironment manifest join
that descriptor to this package's exact source bytes. Unsupported package
layouts fail; additional flat Python modules are covered by the identity.

From the repository root, inside the pinned development container:

```bash
PYTHONPATH=.:manyforge_behavior python3 manyforge_behavior/scripts/generate_effective_node_catalog.py --check
PYTHONPATH=.:manyforge_behavior python3 manyforge_behavior/scripts/generate_node_environment_manifest.py --check
PYTHONPATH=.:manyforge_behavior python3 -m pytest -q manyforge_behavior/tests/test_example_node_package.py
```

A deliberate descriptor change requires catalog review/versioning and normal
artifact regeneration, not a permissive runtime fallback. A source-only change
requires manifest regeneration. The retained project snapshot must match
`pyproject.toml`. Build/install both this wheel and the Behavior wheel when
qualifying an installed environment without the source checkout.
