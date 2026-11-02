# ManyForge Isaac Collider Service

This Isaac Sim extension is a disabled stub. ManyForge does not use it: the
Isaac lane needs no ManyForge extension in Isaac Sim.

- `config/extension.toml` ships it disabled
  (`exts."manyforge.isaac.collider_service".enabled = false`).
- If it is enabled anyway, its startup refuses with
  `composable_cell_runtime_unavailable` before it imports anything, starts ROS
  or touches the stage. It registers no ROS service or topic.
- The companion mesh-transfer CLI (`python3 -m manyforge_ros.isaac_mesh_transfer`)
  refuses the same way.

What remains is the owner-neutral helper library
`scripts/isaac_collider_probe_lib.py` (collider recommendations, convex
normalization, primitive OBJ files), covered by
`tests/test_isaac_collider_probe_lib.py`.

The former extension (collider report service, query and mesh-transfer
topics) is in the repository history: commit
`08222188293df8e12f90dc72e204c21f22f92ea1`, extension blob
`fa0b812d0059793ca9f142d5aa25fc9ad4e9bb62`.
