# ManyForge documentation

Start with the [README](../README.md) for an overview and the
[workbench guide](operations/DEVELOPMENT_WORKBENCH.md) for setup, operation and
diagnostics. No private development documentation is required to use the
workbench.

| Task | Guide |
| --- | --- |
| Install ManyForge (run the prebuilt image, or pull or build the ROS image), check it, update, fall back | [Development workbench: Install](operations/DEVELOPMENT_WORKBENCH.md#install) |
| Start, inspect and stop the workbench | [Development workbench](operations/DEVELOPMENT_WORKBENCH.md) |
| Connect a real robot (xArm, Lite 6) | [Real robot setup](operations/REAL_ROBOT_SETUP.md) |
| Check a fresh-checkout setup | [Fresh-checkout checklist](operations/FRESH_CHECKOUT_CHECKLIST.md) |
| Overview of lanes, dependencies and the assistant | [Stack setup](operations/STACK_SETUP.md) |
| Diagnose a failure (site file, Isaac, real robot, e-stop, AI assistant) | [Troubleshooting](operations/TROUBLESHOOTING.md) |
| Find logs, status endpoints and capture a bug report | [Observability](operations/OBSERVABILITY.md) |
| Mirror an Isaac Sim stage in a Scene | [Isaac scene authoring](operations/ISAAC_SCENE_AUTHORING.md) |
| Run the Isaac Lite 6 trocar FoundationPose workflow | [Trocar FoundationPose runbook](operations/ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md) |
| Run from a digest-checked package or a remote host (advanced) | [Operator launcher](operations/MANYFORGE_OPERATOR_RUNBOOK.md) |
| Build a Scenario from scratch (New, components, Program, Apply, Save as) | [New Scenario](authoring/NEW_SCENARIO.md) |
| Use Program data | [Blackboard keys](authoring/BLACKBOARD_KEYS.md) and [Expressions](authoring/EXPRESSIONS.md) |
| Set Program speeds and tolerances, override them per node, copy nodes | [Motion defaults](authoring/MOTION_DEFAULTS.md) |
| Author signal and interrupt rules | [Interrupts](authoring/INTERRUPTS.md) |
| Understand gripper position without feedback | [Grippers](authoring/GRIPPERS.md) |
| Choose TCPs in Programs and Robot Control | [TCPs](authoring/TCPS.md) |
| Set clearances and allow contact between parts (collision rules) | [Collision rules](authoring/COLLISION_RULES.md) |
| Add a robot (Mock), tool or I/O device | [Adding a component](authoring/ADDING_A_COMPONENT.md) |
| Connect a PLC's I/O (I/O banks, ROS topics, ROXSIE example, simulated PLC) | [I/O banks](authoring/IO_BANKS.md) |
| Choose a controlled Stop profile | [Controlled Stop profiles](operations/STOP_PROFILES.md) |
| Understand the system and its component model | [Architecture](reference/ARCHITECTURE.md) |
| Set up and use the AI assistant (model server, AI model and assistant components, AI tab, failure codes) | [Assistant cookbook](operations/ASSISTANT_E2E_COOKBOOK.md) |
| Review the model-server interface | [Assistant provider contract](reference/ASSISTANT_PROVIDER_CONTRACT.md) |
| Integrate with Composer | [Composer REST API](reference/REST_API.md) |
| Integrate with Headless | [Headless REST API](reference/HEADLESS_REST_API.md) |
| Regenerate generated files and digests | [Identities and pins](reference/IDENTITY_AND_PINNING.md) |
| Check the ros2_medkit status | [ros2_medkit](operations/ROS2_MEDKIT.md) |
| Contribute | [CONTRIBUTING.md](../CONTRIBUTING.md) |

## Use ManyForge over the API (scripts and AI agents)

The GUI and a script use the same API. Read these in order:

1. **What the parts are.** [Architecture](reference/ARCHITECTURE.md): Scenario,
   Program, Deployment, components and lanes.
2. **How to send a change.** [Composer REST API: Change requests](reference/REST_API.md#change-requests):
   which values each change carries, where to read them, and what to do when a
   change is refused. Every edit, Save, Apply, Load and New follows these rules.
3. **A complete worked example.** [scripts/smoke-new-scenario-api.py](../scripts/smoke-new-scenario-api.py)
   builds a Scenario from scratch (New, add components, add a Program root,
   Apply, Save as, Load back), one function per step. The same steps in the GUI:
   [New Scenario](authoring/NEW_SCENARIO.md).
4. **Exact request and response shapes.** [Composer REST API](reference/REST_API.md)
   (route list; the full schemas are at `/openapi.json` while Composer runs),
   [Headless REST API](reference/HEADLESS_REST_API.md),
   [Control WebSocket](reference/CONTROL_WEBSOCKET.md) and the optional
   [MCP contract](reference/MCP_RUNTIME_CONTRACT.md). These pages are generated
   from the code, so they match the running version.
5. **Adding a component.** [Adding a component](authoring/ADDING_A_COMPONENT.md).

Long commands (Apply, Load, New) answer at once; read their progress instead of
waiting on the request. Do not start motion, Apply, Load or New on a cell
without the operator's authorization.

ManyForge's software Stops and motion controls are not safety-rated. A robot
installation needs its own suitable safety system and risk assessment.
