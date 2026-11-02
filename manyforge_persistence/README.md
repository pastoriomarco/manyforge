# ManyForge persistence primitives

This ROS-free library contains mechanical durable-storage primitives shared by
semantic owners. It owns no deployment, scene, Component Manager, Kernel,
controller, robot, or client state.

Its current public operation performs bounded read-only preflight of an
existing SQLite database before a semantic owner opens it read-write. Clean
at-rest WAL databases are inspected through an immutable connection; live WAL
databases with both sidecars are read in place; bounded WAL crash residue is
validated through a private copy. Unsafe paths, orphan sidecars, changing
files, and over-bound images fail closed.

Each consuming semantic owner remains responsible for its exact schema,
metadata, row invariants, resource policy, and recovery state machine. This
library neither interprets nor repairs their records.
