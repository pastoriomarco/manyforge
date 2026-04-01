#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>

namespace py = pybind11;

namespace manyforge::core {

// ============================================================================
// IPlannerBackend Trampoline
// ============================================================================

class PyPlannerBackend : public IPlannerBackend {
 public:
    PyPlannerBackend() = default;
    ~PyPlannerBackend() override = default;

    PlanResultDTO plan(const PlanRequestDTO& request) override {
        PYBIND11_OVERRIDE_PURE(
            PlanResultDTO,
            IPlannerBackend,
            plan,
            request
        );
    }
};

// ============================================================================
// IPlanningContext Trampoline
// ============================================================================

class PyPlanningContext : public IPlanningContext {
 public:
    PyPlanningContext() = default;
    ~PyPlanningContext() override = default;

    RobotStateDTO get_current_state() override {
        PYBIND11_OVERRIDE_PURE(
            RobotStateDTO,
            IPlanningContext,
            get_current_state
        );
    }

    ValidationResultDTO validate_trajectory(const TrajectoryDTO& trajectory,
                                            const ValidationOptionsDTO& options) override {
        PYBIND11_OVERRIDE_PURE(
            ValidationResultDTO,
            IPlanningContext,
            validate_trajectory,
            trajectory, options
        );
    }

    CollisionCheckResultDTO collision_check(const CollisionCheckRequestDTO& request) override {
        PYBIND11_OVERRIDE_PURE(
            CollisionCheckResultDTO,
            IPlanningContext,
            collision_check,
            request
        );
    }

    Pose3DTO forward_kinematics(const FkRequestDTO& request) override {
        PYBIND11_OVERRIDE_PURE(
            Pose3DTO,
            IPlanningContext,
            forward_kinematics,
            request
        );
    }

    JointLimitsDTO get_joint_limits(const std::string& robot_id,
                                    const std::string& group_id) override {
        PYBIND11_OVERRIDE_PURE(
            JointLimitsDTO,
            IPlanningContext,
            get_joint_limits,
            robot_id, group_id
        );
    }

    SceneEditResultDTO add_object(const AddObjectRequestDTO& request) override {
        PYBIND11_OVERRIDE_PURE(
            SceneEditResultDTO,
            IPlanningContext,
            add_object,
            request
        );
    }

    SceneEditResultDTO remove_object(const RemoveObjectRequestDTO& request) override {
        PYBIND11_OVERRIDE_PURE(
            SceneEditResultDTO,
            IPlanningContext,
            remove_object,
            request
        );
    }

    SceneEditResultDTO update_object_pose(const UpdateObjectPoseRequestDTO& request) override {
        PYBIND11_OVERRIDE_PURE(
            SceneEditResultDTO,
            IPlanningContext,
            update_object_pose,
            request
        );
    }
};

// ============================================================================
// IExecutionBackend Trampoline
// ============================================================================

class PyExecutionBackend : public IExecutionBackend {
 public:
    PyExecutionBackend() = default;
    ~PyExecutionBackend() override = default;

    ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) override {
        PYBIND11_OVERRIDE_PURE(
            ExecutionHandle,
            IExecutionBackend,
            execute_trajectory,
            trajectory
        );
    }

    StopResultDTO stop() override {
        PYBIND11_OVERRIDE_PURE(
            StopResultDTO,
            IExecutionBackend,
            stop
        );
    }

    PauseResultDTO pause() override {
        PYBIND11_OVERRIDE_PURE(
            PauseResultDTO,
            IExecutionBackend,
            pause
        );
    }

    ExecutionHandle resume(const TrajectoryDTO& trajectory) override {
        PYBIND11_OVERRIDE_PURE(
            ExecutionHandle,
            IExecutionBackend,
            resume,
            trajectory
        );
    }

    ResetResultDTO reset() override {
        PYBIND11_OVERRIDE_PURE(
            ResetResultDTO,
            IExecutionBackend,
            reset
        );
    }
};

// ============================================================================
// IClock Trampoline
// ============================================================================

class PyClock : public IClock {
 public:
    PyClock() = default;
    ~PyClock() override = default;

    TimePoint now() const override {
        PYBIND11_OVERRIDE_PURE(
            TimePoint,
            IClock,
            now
        );
    }
};

// ============================================================================
// IScheduler Trampoline
// ============================================================================

class PyScheduler : public IScheduler {
 public:
    PyScheduler() = default;
    ~PyScheduler() override = default;

    ScheduleHandle schedule(TimePoint deadline, std::function<void()> callback) override {
        PYBIND11_OVERRIDE_PURE(
            ScheduleHandle,
            IScheduler,
            schedule,
            deadline, callback
        );
    }

    void cancel(ScheduleHandle handle) override {
        PYBIND11_OVERRIDE_PURE(
            void,
            IScheduler,
            cancel,
            handle
        );
    }
};

}  // namespace manyforge::core
