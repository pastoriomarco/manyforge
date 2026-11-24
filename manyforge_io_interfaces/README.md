# manyforge_io_interfaces

The messages of an I/O bank (`ros_io_bank` adapter family): a device such as
a PLC with up to 64 digital inputs and 64 digital outputs per bank.

- `IoBankState` (device to ROS, periodic): `cycle`, the last decided command
  (`applied_seq`, `applied_result`, `device_code`, `refused[64]`),
  `inputs[64]` and the physical `outputs[64]`.
- `IoBankCommand` (ROS to device): `seq` and `values[64]`, each `OUTPUT_OFF`,
  `OUTPUT_ON` or `OUTPUT_KEEP`.

Topics: `/<component_id>/io_bank/state` and `/<component_id>/io_bank/command`.
The package depends only on `rosidl_default_generators` and builds alone.
User guide: `docs/authoring/IO_BANKS.md`.
