# ManyForge documentation

Start with the [README](../README.md) for an overview and the
[workbench guide](operations/DEVELOPMENT_WORKBENCH.md) for setup, operation and
diagnostics. No private development documentation is required to use the
workbench.

| Task | Guide |
| --- | --- |
| Prepare, start, inspect and stop the workbench | [Development workbench](operations/DEVELOPMENT_WORKBENCH.md) |
| Connect a real robot (xArm, Lite 6) | [Real robot setup](operations/REAL_ROBOT_SETUP.md) |
| Check a fresh-checkout setup | [Fresh-checkout checklist](operations/FRESH_CHECKOUT_CHECKLIST.md) |
| Overview of lanes, dependencies and the assistant | [Stack setup](operations/STACK_SETUP.md) |
| Diagnose a failure (site file, Isaac, real robot, e-stop, AI assistant) | [Troubleshooting](operations/TROUBLESHOOTING.md) |
| Find logs, status endpoints and capture a bug report | [Observability](operations/OBSERVABILITY.md) |
| Mirror an Isaac Sim stage in a Scene | [Isaac scene authoring](operations/ISAAC_SCENE_AUTHORING.md) |
| Run the Isaac Lite 6 trocar FoundationPose workflow | [Trocar FoundationPose runbook](operations/ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md) |
| Run from a digest-checked package or a remote host | [Operator launcher](operations/MANYFORGE_OPERATOR_RUNBOOK.md) |
| Use Program data | [Blackboard keys](authoring/BLACKBOARD_KEYS.md) and [Expressions](authoring/EXPRESSIONS.md) |
| Set Program speeds and tolerances, override them per node, copy nodes | [Motion defaults](authoring/MOTION_DEFAULTS.md) |
| Author signal and interrupt rules | [Interrupts](authoring/INTERRUPTS.md) |
| Understand gripper position without feedback | [Grippers](authoring/GRIPPERS.md) |
| Choose TCPs in Programs and Robot Control | [TCPs](authoring/TCPS.md) |
| Add a robot (Mock), tool or I/O device | [Adding a component](authoring/ADDING_A_COMPONENT.md) |
| Choose a controlled Stop profile | [Controlled Stop profiles](operations/STOP_PROFILES.md) |
| Understand the system and its component model | [Architecture](reference/ARCHITECTURE.md) |
| Set up and use the AI assistant (model server, AI model and assistant components, AI tab, failure codes) | [Assistant cookbook](operations/ASSISTANT_E2E_COOKBOOK.md) |
| Review the model-server interface | [Assistant provider contract](reference/ASSISTANT_PROVIDER_CONTRACT.md) |
| Integrate with Composer | [Composer REST API](reference/REST_API.md) |
| Integrate with Headless | [Headless REST API](reference/HEADLESS_REST_API.md) |
| Regenerate generated files and digests | [Identities and pins](reference/IDENTITY_AND_PINNING.md) |
| Check the ros2_medkit status | [ros2_medkit](operations/ROS2_MEDKIT.md) |
| Contribute | [CONTRIBUTING.md](../CONTRIBUTING.md) |

ManyForge's software Stops and motion controls are not safety-rated. A robot
installation needs its own suitable safety system and risk assessment.
