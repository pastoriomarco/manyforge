# manyforge_supervision (no code)

This directory holds no code; it marks where the old Python cell supervisor
used to live.

Process supervision belongs to the Manager (`manyforge_component_manager`):
it starts and stops the execution host, observes its runtime units
(`runtime_observation`, `manager_active_runtime_coordinator`,
`retained_posix_runtime_unit_adapter`) and converges them to the applied
Deployment. The execution host supervises the processes of its components
(drivers, bridges, `ros2_control.launch.py`). Component health is each
component's `ComponentState`, served by the Headless API (`GET /api/components`).

`verification/composable_cell_i4/` and `tests/composable_cell_i4/` keep
test-only reference fixtures of the runtime-observation rules; production code
never imports them.

The deleted Python sources are in the repository history (commit
`08222188293df8e12f90dc72e204c21f22f92ea1`).
