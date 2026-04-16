> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Engineering Reference

Internal engineering reference for ManyForge contributors and sandboxed coding
agents. This file replaces the old split between:

- the former `docs/reference/REFERENCE.md` (ManyMove rewrite notes)
- the former `WORKSPACE_GUIDE.md` (core C++ conventions)

Use this document for implementation patterns and core workspace rules. Keep
user-facing onboarding in [`../../README.md`](../../README.md).

## Scope

This reference is intentionally narrow:

- core C++ conventions for `manyforge_core` and `manyforge_roboplan`
- architecture patterns worth carrying forward from the old `manymove` codebase
- constraints that sandboxed agents should treat as stable

It is not:

- a product roadmap
- a stack bring-up guide
- a user tutorial

See also:

- [`../plans/LLM_OPERABILITY_PLAN.md`](../plans/LLM_OPERABILITY_PLAN.md)
- [`../plans/OPENCLAW_INTEGRATION_PLAN.md`](../plans/OPENCLAW_INTEGRATION_PLAN.md)
- [`../operations/STACK_SETUP.md`](../operations/STACK_SETUP.md)
- [`../integration/PLATFORM_REQUIREMENTS.md`](../integration/PLATFORM_REQUIREMENTS.md)

## Core C++ Conventions

These conventions apply to all `manyforge_core` and `manyforge_roboplan` code.

### Namespace

```cpp
namespace manyforge::core { ... }
```

### Directory Layout

```text
manyforge_core/
├── include/manyforge_core/
│   ├── types/          # DTOs and enums
│   ├── ports/          # Abstract interfaces (IPlannerBackend, etc.)
│   ├── time/           # IClock, IScheduler
│   ├── cache/          # Trajectory cache
│   └── collision/      # Collision policy
├── src/                # Implementation files (.cpp)
└── test/               # Google Test files
```

### Include Paths

```cpp
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/time/iclock.hpp>
```

### Error Handling

- Use `Result<T>` for fallible operations.
- Use `ErrorCode` enums for stable failure categories.
- Do not throw exceptions from `manyforge_core`.

### C++ Standard and Runtime Constraints

- C++20
- `#pragma once` for include guards
- No ROS headers in core (`rclcpp`, `rosidl`, `rmw`)
- No `std::this_thread::sleep_for` in core logic

### Naming

- `snake_case` for files, functions, variables, namespaces
- `PascalCase` for types, classes, enums, structs
- `k` prefix for enum values: `kSuccess`, `kIdle`
- `I` prefix for interfaces: `IClock`, `IPlannerBackend`

### Build

- CMake changes are allowed when required; keep them intentional.
- Requalify affected suites after build-graph changes.

```bash
mkdir -p build && cd build
cmake ..
make -j"$(nproc)"
ctest --output-on-failure
```

Expected flags:

- `-Wall`
- `-Wextra`
- `-Werror`
- `-pedantic`

### Frozen Contract Headers

Do not edit these headers casually. Treat them as stability boundaries.

- `include/manyforge_core/types/`
- `include/manyforge_core/ports/`
- `include/manyforge_core/time/iclock.hpp`
- `include/manyforge_core/time/ischeduler.hpp`

## Reference Patterns From ManyMove

Curated patterns from the old `manymove` codebase. Use them as design input,
not as copy-paste source. ManyMove was ROS-centric; ManyForge is not.

### 1. Execution State Machine

Original pattern:

```cpp
enum class MoveExecutionState { IDLE, PLANNING, EXECUTING, COMPLETED, CANCELLED };
```

Original transitions:

- `IDLE -> PLANNING`
- `PLANNING -> EXECUTING`
- `EXECUTING -> COMPLETED`
- `EXECUTING -> CANCELLED`
- any state -> `IDLE` on reset

Guarding principle:

- state transitions are mutex-protected
- no implicit state mutation from external code

For ManyForge:

- keep explicit state ownership and guarded transitions
- include explicit failure and stopping states
- keep state changes centralized in the execution state machine

See current target types in:

- `manyforge_core/include/manyforge_core/types/execution_status.hpp`

### 2. Controlled Stop Semantics

Original pattern:

```cpp
enum class ControlledStopResult { STOP_SENT, NOT_REQUIRED, FAILED };
```

Key idea:

- stop is a first-class operation with an explicit result
- stop policy distinguishes between hold-position and trajectory-aware slowdown

For ManyForge:

- keep explicit stop results instead of burying stop behavior inside planner code
- map collision or fault severity to stop policy
- keep stop ownership in the execution kernel, not in the planner adapter

### 3. Abstract Planner Interface

Original responsibilities worth preserving:

```cpp
std::pair<bool, Trajectory> plan(const PlanGoal& goal);
std::pair<bool, Trajectory> applyTimeParameterization(
    const Trajectory& traj, const MovementConfig& config);

bool isJointStateValid(const std::vector<double>& positions) const;
bool isTrajectoryStartValid(const Trajectory& traj, double tol) const;
bool isTrajectoryEndValid(const Trajectory& traj, double tol) const;
bool isTrajectoryValid(const Trajectory& traj, bool check_collisions) const;

void alignTrajectoryToCurrentState(Trajectory& traj, double tol) const;
```

For ManyForge:

- preserve swap-friendly planner backends behind `IPlannerBackend`
- keep planning separate from validation
- keep time-parameterization in the planner adapter layer
- preserve hard Cartesian speed limiting as a safety boundary

### 4. Movement Configuration

Fields that still matter conceptually:

```text
max_cartesian_speed
plan_number_target
plan_number_limit
smoothing_type
tcp_frame
deceleration_time
min_stop_time

velocity_scaling_factor
acceleration_scaling_factor
planning_pipeline
planner_id
planning_time
planning_attempts

step_size
jump_threshold
```

For ManyForge:

- keep movement config as a typed DTO, not an unstructured parameter bag
- prefer presets built as code helpers or deployment-level policies
- separate planning, execution, and safety concerns cleanly even if the DTO is
  still flat today

### 5. Move DTO

Original pattern:

```cpp
struct Move {
    std::string type;
    std::string pose_key;
    std::vector<double> joint_values;
    std::string named_target;
    MovementConfig config;
    std::vector<double> start_joint_values;
    std::string robot_prefix;
    std::string tcp_frame;
};
```

For ManyForge:

- use enums instead of stringly-typed move kinds
- keep the BT-facing DTO small and explicit
- avoid leaking ROS message conventions into core types

## Patterns To Keep

- mutex-guarded state transitions
- explicit stop-result enums
- planner backends behind stable interfaces
- layered trajectory validation
- hard Cartesian speed limits
- typed motion/config DTOs

## Patterns To Improve

- add explicit failure/stopping states instead of overloading cancel semantics
- use enum-discriminated move types instead of raw strings
- reduce duplicated trajectory-validation entry points
- keep stop strategy configurable instead of hardcoded
- split large config DTOs when field count becomes a maintenance problem

## Sandbox Upload Note

[`scripts/manyforge-session-start.sh`](../../scripts/manyforge-session-start.sh)
uploads this file into the sandbox as the single engineering reference
document. If this file changes materially, keep the session-start script and
any agent instructions in sync.
