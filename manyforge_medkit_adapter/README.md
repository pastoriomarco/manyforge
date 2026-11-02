# manyforge_medkit_adapter

This package currently retains the transport-neutral fault latch, healing,
archive-reference, and medkit-report mapping in `forwarder.py`. The effectful
adapter entrypoint is intentionally unavailable until I6/N provides an
addressed runtime diagnostics source; it exits 78 before parsing arguments,
accessing a network, importing ROS, or publishing a fault.

ManyForge core stays medkit-free — this package owns the coupling, one-way.

The previous Composer-polling adapter is a behavior reference, not a runnable
fallback. Its complete source is pinned at
`manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`, blob
`ef1b592fac2c99ebb10c4683b86c654704e7d3aa`. At N, the restored adapter must
consume addressed owner-issued diagnostics while preserving FAILED-on-latch,
PASSED healing, severity, archive-reference, discovery, and fail-closed
behavior.

**Status and medkit notes:**
[`docs/operations/ROS2_MEDKIT.md`](../docs/operations/ROS2_MEDKIT.md)

Hermetic reducer and mapping tests remain part of the ROS lane. No current
real-lifecycle or operator command is claimed until N qualification.
