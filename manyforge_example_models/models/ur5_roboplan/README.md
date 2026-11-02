# Universal Robots UR5 (RoboPlan example)

RoboPlan's UR5 example arm, model family `ur5_roboplan`. `ur5.urdf` and
`ur5.srdf` are its structural projection
(`manyforge_deployment/resources/maintained/model_sources/ur5_roboplan/`,
which the model family row reads); the meshes stay in
`roboplan_example_models/models/ur_robot_model`.

This folder gives the model its lane rows (`lanes.mock`) and its Mock
controllers file.

## Joint acceleration limits

UR publishes no per-joint acceleration limit (its arms are power-limited, so
the reachable acceleration depends on pose and payload). The `acceleration`
values in this URDF are about 50% of the worst-case torque-limited joint
acceleration, (τ_max − |g(q)|) / M_ii(q), computed with Pinocchio from this
URDF's torque limits and inertias plus the rated payload 50 mm beyond `tool0`
over ~51,000 configurations. Wrist joints are capped at 10 rad/s² because the
URDF omits motor rotor inertia. They are example values, not a UR
specification: set your robot's real limits for your payload.
