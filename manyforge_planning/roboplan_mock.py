"""Mock RoboPlan API for testing."""

import numpy as np
from dataclasses import dataclass, field
from typing import List, Optional, Dict


class MockScene:
    """Mock RoboPlan Scene."""
    def __init__(self, joint_names=None, group_name="arm"):
        self._joint_names = joint_names or ["j1","j2","j3","j4","j5","j6"]
        self._group_name = group_name
        self._geometries = {}  # name -> dict
        self._collision_positions = set()  # frozenset of tuples that cause collision
        self._current_q = np.zeros(len(self._joint_names))

    def getActuatedJointNames(self): return list(self._joint_names)
    def getJointGroupInfo(self, group): return {"joint_names": self._joint_names}
    def forwardKinematics(self, q, frame): # returns 4x4 identity offset by q[0]
        if frame not in ["tcp", "base", "tool0"]:
            raise ValueError(f"Unknown frame: {frame}")
        m = np.eye(4); m[0,3]=0.5+q[0]*0.1; m[1,3]=0.0; m[2,3]=0.6; return m
    def hasCollisions(self, q):
        return tuple(np.round(q,3)) in self._collision_positions
    def randomCollisionFreePositions(self): return np.random.randn(len(self._joint_names))
    def getPositionLimitVectors(self, group=None):
        n=len(self._joint_names); return -np.pi*np.ones(n), np.pi*np.ones(n)
    def getVelocityLimitVectors(self, group=None):
        n=len(self._joint_names); return -3.0*np.ones(n), 3.0*np.ones(n)
    def getAccelerationLimitVectors(self, group=None):
        n=len(self._joint_names); return -2.0*np.ones(n), 2.0*np.ones(n)
    def addBoxGeometry(self, name, parent, box, tform, color):
        self._geometries[name] = {"type":"box","box":box,"tform":tform}
    def addSphereGeometry(self, name, parent, box, tform, color):
        self._geometries[name] = {"type":"sphere","tform":tform}
    def updateGeometryPlacement(self, name, parent, tform):
        if name not in self._geometries: raise ValueError(f"Unknown geometry {name}")
        self._geometries[name]["tform"] = tform
    def removeGeometry(self, name):
        if name not in self._geometries: raise ValueError(f"Unknown geometry {name}")
        del self._geometries[name]
    def setRngSeed(self, seed): pass
    def getCurrentJointPositions(self): return self._current_q.copy()
    def setJointPositions(self, q): self._current_q = np.array(q)
    # Expose for tests
    def add_collision_position(self, q): self._collision_positions.add(tuple(np.round(q,3)))


class MockJointConfiguration:
    def __init__(self, names, positions): self.joint_names=list(names); self.positions=np.array(positions)


class MockJointPath:
    def __init__(self, names, positions_list): self.joint_names=list(names); self.positions=[np.array(p) for p in positions_list]


class MockJointTrajectory:
    def __init__(self, names, times, positions, velocities, accelerations):
        self.joint_names=list(names); self.times=list(times)
        self.positions=[np.array(p) for p in positions]; self.velocities=[np.array(v) for v in velocities]
        self.accelerations=[np.array(a) for a in accelerations]


class MockRRTOptions:
    def __init__(self): self.group_name=""; self.max_planning_time=5.0; self.max_nodes=5000


class MockRRT:
    def __init__(self, scene, opts=None): self._scene=scene; self._opts=opts; self._fail=False
    def setRngSeed(self, s): pass
    def set_fail(self, f): self._fail=f
    def plan(self, start, goal):
        if self._fail: raise RuntimeError("Planning failed")
        return MockJointPath(start.joint_names, [start.positions, goal.positions])


class MockPathShortcutter:
    def __init__(self, scene, group): pass
    def shortcut(self, path, max_step_size=0.05, max_iters=100, seed=0): return path


class MockBox:
    def __init__(self, x, y, z): self.x=x; self.y=y; self.z=z


class MockSphere:
    def __init__(self, r): self.r=r


class MockTOPPRA:
    def __init__(self, scene): self._scene=scene; self._fail=False
    def set_fail(self, f): self._fail=f
    def generate(self, path, dt=0.01, velocity_scale=1.0, acceleration_scale=1.0, **kw):
        if self._fail: raise RuntimeError("TOPPRA failed")
        n=len(path.positions); times=[i*dt for i in range(n)]
        zeros=[np.zeros_like(p) for p in path.positions]
        return MockJointTrajectory(path.joint_names, times, path.positions, zeros, zeros)


class MockSimpleIkOptions:
    def __init__(self): self.group_name=""; self.max_restarts=50


class MockSimpleIk:
    def __init__(self, scene, opts=None): self._scene=scene; self._fail=False
    def set_fail(self, f): self._fail=f
    def solveIk(self, cart, start, solution):
        if self._fail: return False
        solution.positions = start.positions.copy(); return True


class MockCartesianConfiguration:
    def __init__(self, base, tip, tform): self.base_frame=base; self.tip_frame=tip; self.tform=tform


def mock_hasCollisionsAlongPath(scene, q0, q1, step): return scene.hasCollisions(q0) or scene.hasCollisions(q1)


class MockRoboPlan:
    """Facade providing all mock roboplan components."""
    Scene = MockScene
    JointConfiguration = MockJointConfiguration
    JointPath = MockJointPath
    JointTrajectory = MockJointTrajectory
    RRTOptions = MockRRTOptions
    RRT = MockRRT
    PathShortcutter = MockPathShortcutter
    Box = MockBox
    Sphere = MockSphere
    TOPPRA = MockTOPPRA
    SimpleIkOptions = MockSimpleIkOptions
    SimpleIk = MockSimpleIk
    CartesianConfiguration = MockCartesianConfiguration
    hasCollisionsAlongPath = staticmethod(mock_hasCollisionsAlongPath)
