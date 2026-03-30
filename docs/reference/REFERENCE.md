# ManyMove Architecture Reference for ManyForge Rewrite

Curated patterns from the old manymove codebase. Use as inspiration; do not copy ROS-dependent code.

---

## 1. State Machine (action_server.hpp)

```cpp
enum class MoveExecutionState { IDLE, PLANNING, EXECUTING, COMPLETED, CANCELLED };
```

Transitions (original):
- IDLE → PLANNING (goal accepted)
- PLANNING → EXECUTING (plan success)
- EXECUTING → COMPLETED (trajectory finished)
- EXECUTING → CANCELLED (cancel requested)
- Any → IDLE (reset)

Guards: `move_state_mutex_` protects all transitions.

**For ManyForge:** Expand to include explicit failure states (`kFailed`, `kStopping`).
State machine is in `manyforge_core/include/manyforge_core/types/execution_status.hpp`
and will be implemented in `state_machine.hpp`.

---

## 2. Controlled Stop (planner_interface.hpp)

```cpp
enum class ControlledStopResult { STOP_SENT, NOT_REQUIRED, FAILED };

// Abstract interface
ControlledStopResult sendControlledStop(
    const MovementConfig& move_cfg,
    const RobotTrajectory& running_traj = {},
    double elapsed_s = 0.0) = 0;
```

Two stop modes:
- **Spring-back stop**: send single-point hold at current position
- **Trajectory-aware deceleration**: compute remaining trajectory, apply deceleration profile

`deceleration_time` and `min_stop_time` in MovementConfig control the deceleration duration.

**For ManyForge:** `CollisionSeverity` enum (kWarning/kSoftStop/kHardStop) maps to these two modes.
Stop semantics owned by ExecutionKernel (ADR-0005), not the planner.

---

## 3. Abstract PlannerInterface (planner_interface.hpp)

Key methods to replicate in `IPlannerBackend`:

```cpp
// Core
std::pair<bool, Trajectory> plan(const PlanGoal& goal);
std::pair<bool, Trajectory> applyTimeParameterization(
    const Trajectory& traj, const MovementConfig& config);

// Validation
bool isJointStateValid(const std::vector<double>& positions) const;
bool isTrajectoryStartValid(const Trajectory& traj, double tol) const;
bool isTrajectoryEndValid(const Trajectory& traj, double tol) const;
bool isTrajectoryValid(const Trajectory& traj, bool check_collisions) const;

// Utility
void alignTrajectoryToCurrentState(Trajectory& traj, double tol) const;
```

Time parameterization enforces `max_cartesian_speed` — TCP speed is hard-limited for safety.
Velocity is reduced if Cartesian speed exceeds limit; acceleration scaling is NOT reduced.

**For ManyForge:** `IPlannerBackend::plan()` + `IPlannerBackend::validate()` cover this.
Time parameterization becomes part of the planner adapter, not the core interface.

---

## 4. MovementConfig Fields (MovementConfig.msg)

```
# Core
float64 max_cartesian_speed         # TCP speed limit (m/s), 0 = unlimited
int32   plan_number_target          # Valid plans before accepting (4-8 typical)
int32   plan_number_limit           # Max attempts (e.g. 16)
string  smoothing_type              # "time_optimal", "ruckig"
string  tcp_frame                   # End-effector frame for Cartesian speed
float64 deceleration_time           # Controlled stop duration (s)
float64 min_stop_time               # Min time for controlled stop (s)

# MoveIt
float64 velocity_scaling_factor     # 0.0–1.0
float64 acceleration_scaling_factor # 0.0–1.0
string  planning_pipeline           # "ompl", "pilz", "cumotion", "chomp"
string  planner_id                  # "RRTConnect", "PTP", "LIN", etc.
float64 planning_time               # Budget (s)
int32   planning_attempts

# Cartesian interpolation
float64 step_size
float64 jump_threshold
```

Presets defined in codebase: `max_move`, `mid_move`, `slow_move`, `cartesian_*`,
`PTP_*`, `LIN_*`, `CHOMP_*`, `cumotion_*`.

**For ManyForge:** `MovementConfig` struct is in `types/movement_config.hpp` (already frozen).
Presets can be added as free functions: `MovementConfig slow_config()`, etc.

---

## 5. Move Struct (move.hpp)

```cpp
struct Move {
    std::string type;                    // "pose", "joint", "named", "cartesian"
    std::string pose_key;                // Blackboard key (for BT layer)
    std::vector<double> joint_values;   // Goal for "joint" type
    std::string named_target;           // Goal for "named" type
    MovementConfig config;
    std::vector<double> start_joint_values;
    std::string robot_prefix;           // Multi-robot support
    std::string tcp_frame;
};
```

**For ManyForge:** Move type is a Phase 1 DTO. Use enum instead of string:
`enum class MoveType { kPose, kJoint, kNamed, kCartesian }`.

---

## Patterns to Keep

- Mutex-guarded state machine with explicit states
- Three-value stop result (SENT/NOT_REQUIRED/FAILED)
- Abstract planner interface with swap-friendly backends
- Configuration presets for common use cases
- Cartesian speed enforcement as a hard safety limit
- Layered trajectory validation (joint, start, end, path)

## Patterns to Improve

- Add explicit `kFailed` state (manymove has no failure state — it just goes CANCELLED)
- Use enum discriminated union for move type instead of string
- Unify trajectory validation overloads (msg vs. internal type)
- Make stop strategy configurable (policy object, not hardcoded logic)
- Nested MovementConfig (planning, execution, safety sub-structs) to reduce field count
