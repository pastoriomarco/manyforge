import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api } from '../api/client';
import { useCycleStore } from '../store/cycleStore';
import { useProgramStore } from '../store/programStore';
import { useSceneStateStore } from '../store/sceneStateStore';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import type {
  CartesianFrame,
  MoveToOrientationMode,
  MoveToSource,
} from '../store/viewerSelectionStore';
import { followTargetController } from '../streaming/followTargetController';
import type { FollowTargetDragToken } from '../streaming/followTargetController';
import type {
  SceneFrameState,
  SceneGripperState,
  SceneRobotState,
  SceneUrdfJoint,
  SceneUrdfManifest,
  TreeNode,
} from '../types/api';
import { computeMoveToTarget } from '../utils/moveTo';
import type { StoredPoseTarget } from '../utils/moveTo';

/**
 * Joint streaming-session hook (ADR-0016, input_mode=joint).
 *
 * Routes joint slider/numeric updates through the same WebSocket
 * streaming session the TCP gizmo uses. Replaces the previous
 * one-shot /api/direct/joint-jog path which produced 409 Conflict
 * badges whenever slider events arrived faster than the cycle could
 * return to idle.
 *
 * Per-joint throttle (4 Hz) bounds the wire rate; the ack-gated
 * latest-wins WS client coalesces faster-arriving frames to the
 * latest pose. On release (or numeric blur/Enter) `pushFinal`
 * bypasses the throttle so the released value is guaranteed to land
 * before the debounced End fires. A 500 ms idle window without any
 * new push closes the session via spring_back soft-stop; the next
 * interaction reopens automatically.
 *
 * One session is shared across all JointRows in the panel — the
 * hook lives at the body level. Switching the robot panel out of
 * Joint mode unmounts the body, the hook's cleanup aborts the
 * session, and the kernel returns to Idle.
 */
const JOINT_PUSH_INTERVAL_MS = 250; // 4 Hz; matches the operator's "2-4 Hz is enough" note
const JOINT_END_DEBOUNCE_MS = 500; // probe interval — re-check arrival every 500 ms idle
// Joint-arrival tolerance for closing the streaming session. Keep this
// at or below the fine jog step so a spinner click does not complete
// early at a visibly different value. The backend's streaming worker
// clamps actual target crossings to the requested value, so this
// threshold is reachable without timing-specific exact equality.
const JOINT_ARRIVAL_TOLERANCE_RAD = 1e-3;

function useJointStreaming(sessionJointNames: string[]): {
  push: (jointName: string, valueRad: number) => void;
  pushFinal: (jointName: string, valueRad: number) => void;
} {
  const tokenRef = useRef<FollowTargetDragToken | null>(null);
  const endTimerRef = useRef<number | null>(null);
  const armEndRef = useRef<() => void>(() => {});
  const lastPushTsRef = useRef<Map<string, number>>(new Map());
  // Per-joint last-pushed target. The end-timer callback checks the
  // live joint state against these targets and only closes the session
  // once every commanded joint has converged within
  // JOINT_ARRIVAL_TOLERANCE_RAD. Without this, a click on the slider
  // track produces one push + 500 ms of silence + spring_back, which
  // latches the joint partway to its target and leaves the operator
  // confused about why the robot didn't finish the move.
  const lastPushTargetsRef = useRef<Map<string, number>>(new Map());
  // Keep the latest known session joint list in a ref so begin() at any
  // future drag picks up the freshest value, even if the manifest loaded
  // after the hook mounted. The kernel uses this list to validate
  // incoming partial joint names — an empty list rejects everything as
  // `unknown_joint_name`, so we MUST pass the robot's drivable joints.
  const jointNamesRef = useRef<string[]>(sessionJointNames);
  useEffect(() => {
    jointNamesRef.current = sessionJointNames;
  }, [sessionJointNames]);

  const ensureSession = useCallback((): FollowTargetDragToken | null => {
    if (tokenRef.current !== null) return tokenRef.current;
    if (jointNamesRef.current.length === 0) {
      // No joints known yet (manifest still loading). The kernel would
      // reject every partial frame; better to skip silently than open
      // a doomed session that produces rejection log spam.
      return null;
    }
    const token = followTargetController.begin({
      inputMode: 'joint',
      jointNames: jointNamesRef.current,
      softStopStrategy: 'spring_back',
      principalId: 'composer_operator',
      authoritySource: 'human',
      triggerReason: 'composer_joint_slider',
    });
    if (token === null) return null;
    tokenRef.current = token;
    return token;
  }, []);

  // Check whether every commanded joint has converged within tolerance
  // and, if so, close the streaming session. Returns true when the
  // session was closed (so the timer can stop re-arming). Called from
  // both the timer probe AND from a scene-store subscription below
  // — the latter is what makes Stop deactivate the instant live state
  // crosses the arrival threshold rather than up to JOINT_END_DEBOUNCE_MS
  // later.
  const tryCloseOnArrival = useCallback((): boolean => {
    const token = tokenRef.current;
    if (token === null) return true;
    const live = useSceneStateStore.getState().scene?.robot?.joint_positions;
    if (lastPushTargetsRef.current.size === 0) {
      // No target ever pushed (shouldn't happen, but don't hang the
      // session waiting for a phantom arrival).
    } else {
      if (live === undefined) return false;
      for (const [name, target] of lastPushTargetsRef.current) {
        const observed = live[name];
        if (observed === undefined
            || Math.abs(observed - target) > JOINT_ARRIVAL_TOLERANCE_RAD) {
          return false;
        }
      }
    }
    tokenRef.current = null;
    lastPushTsRef.current.clear();
    lastPushTargetsRef.current.clear();
    if (endTimerRef.current !== null) {
      window.clearTimeout(endTimerRef.current);
      endTimerRef.current = null;
    }
    followTargetController.end(token, 'spring_back');
    return true;
  }, []);

  const armEnd = useCallback((): void => {
    if (endTimerRef.current !== null) window.clearTimeout(endTimerRef.current);
    endTimerRef.current = window.setTimeout(() => {
      endTimerRef.current = null;
      if (!tryCloseOnArrival()) armEndRef.current();
    }, JOINT_END_DEBOUNCE_MS);
  }, [tryCloseOnArrival]);
  useEffect(() => {
    armEndRef.current = armEnd;
  }, [armEnd]);

  // NOTE: we used to ALSO drive `tryCloseOnArrival` from a scene-store
  // subscription so the session would close the instant the joint
  // reached its target. That introduced a race window: during the
  // ~200 ms `ending` round-trip (WS goodbye + kernel
  // kStreaming→kStopped→kIdle), `tokenRef` was already cleared by
  // tryCloseOnArrival but the controller's `activeDragToken` was also
  // null (it gets cleared synchronously inside `controller.end(...)`),
  // so a follow-up spinner click on the same input was processed as:
  //   - ensureSession() → begin() queues (phase === 'ending') and
  //     returns a fresh token,
  //   - the push frame is dropped because `token !== activeDragToken`,
  //   - the new session eventually opens with no target,
  //   - the arrival probe sees live ≠ target forever, session stays
  //     open, Stop stays active, kernel sometimes rejects the new
  //     begin with "valid only from Idle" because cleanup wasn't done.
  // Timer-based close (below) avoids this entirely — a fresh push
  // re-arms the same session and the close only fires after the
  // operator pauses for one full debounce interval.

  const push = useCallback(
    (jointName: string, valueRad: number): void => {
      const now = performance.now();
      const last = lastPushTsRef.current.get(jointName) ?? 0;
      if (now - last < JOINT_PUSH_INTERVAL_MS) return;
      lastPushTsRef.current.set(jointName, now);
      lastPushTargetsRef.current.set(jointName, valueRad);
      const token = ensureSession();
      if (token === null) return;
      followTargetController.push(token, {
        kind: 'joint',
        jointNames: [jointName],
        jointPositionsRad: [valueRad],
      });
      armEnd();
    },
    [armEnd, ensureSession],
  );

  const pushFinal = useCallback(
    (jointName: string, valueRad: number): void => {
      // Bypass the throttle — the released value is the goal, the
      // operator's last hand position must reach the backend before
      // End closes the session.
      lastPushTsRef.current.set(jointName, performance.now());
      lastPushTargetsRef.current.set(jointName, valueRad);
      const token = ensureSession();
      if (token === null) return;
      followTargetController.push(token, {
        kind: 'joint',
        jointNames: [jointName],
        jointPositionsRad: [valueRad],
      });
      armEnd();
    },
    [armEnd, ensureSession],
  );

  useEffect(() => {
    // Recovery channel: if the controller drops to idle while we
    // still hold a token, something tore the session down without
    // going through us (ControlBar Stop/Reset → followTargetController.abort,
    // a fatal rejection from the server, a network drop, etc.).
    // Clear our refs so the next slider event opens a fresh session
    // instead of pushing into a dead one and looking frozen.
    const unsubscribe = followTargetController.subscribe((phase) => {
      if ((phase === 'idle' || phase === 'aborted') && tokenRef.current !== null) {
        if (endTimerRef.current !== null) {
          window.clearTimeout(endTimerRef.current);
          endTimerRef.current = null;
        }
        tokenRef.current = null;
        lastPushTsRef.current.clear();
        lastPushTargetsRef.current.clear();
      }
    });
    return () => {
      unsubscribe();
      if (endTimerRef.current !== null) {
        window.clearTimeout(endTimerRef.current);
        endTimerRef.current = null;
      }
      if (tokenRef.current !== null) {
        followTargetController.abort('joint streaming hook unmount');
        tokenRef.current = null;
      }
    };
  }, []);

  return { push, pushFinal };
}

/**
 * Day-5 robot-control overlay panel.
 *
 * Day-5c.1 adds three things to the V1 surface:
 * - Step cap: per-commit clamp `|new - current| <= cap`, with a
 *   dropdown at the panel head. Prevents the "I dragged from 0 to
 *   1 rad in one click" surprise.
 * - Status indicator per joint row: idle / sending / error /
 *   success, so the operator sees when the cycle has accepted the
 *   commit and when the next one can fire.
 * - The slider mirrors the live joint state via the backend's
 *   `_last_known_joint_positions_rad` fallback (set on first jog
 *   or from the deployment's `initial_joint_positions_rad`).
 *
 * Day-5c.2 (Cartesian gizmo) and Day-5c.3 (Pose Edit) will add
 * sibling tabs to this panel; the current Joint tab is the
 * jog-mode view.
 */
interface RobotControlPanelProps {
  onClose: () => void;
}

const DRIVABLE_TYPES = new Set(['revolute', 'prismatic', 'continuous']);


const REVOLUTE_STEPS: readonly number[] = [0.001, 0.01, 0.1, 0.5];
const PRISMATIC_STEPS: readonly number[] = [0.001, 0.01, 0.1];

export function RobotControlPanel({ onClose }: RobotControlPanelProps) {
  const [manifest, setManifest] = useState<SceneUrdfManifest | null>(null);
  const [manifestError, setManifestError] = useState<string | null>(null);
  // Step size = the increment applied by the numeric-input arrow
  // buttons (and as the slider's `step` for keyboard precision). NOT
  // a clamp — sliders move freely limit-to-limit and commits send
  // the requested value verbatim. The dropdown picks coarse vs fine
  // arrow stepping per axis kind.
  const [jointRadStep, setJointRadStep] = useState<number>(0.1);
  const [cartesianMStep, setCartesianMStep] = useState<number>(0.01);
  const [cartesianRadStep, setCartesianRadStep] = useState<number>(0.1);
  const robot = useSceneStateStore((s) => s.scene?.robot ?? null);
  const robotModelKey = robot?.urdf_model_key ?? robot?.robot_id ?? null;
  const robotMode = useViewerSelectionStore((s) => s.robotMode);
  const setRobotMode = useViewerSelectionStore((s) => s.setRobotMode);
  const cartesianFrame = useViewerSelectionStore((s) => s.cartesianFrame);
  const setCartesianFrame = useViewerSelectionStore((s) => s.setCartesianFrame);

  useEffect(() => {
    let cancelled = false;
    setManifest(null);
    setManifestError(null);
    api.urdf
      .manifest()
      .then((data) => {
        if (!cancelled) setManifest(data);
      })
      .catch((err: unknown) => {
        if (!cancelled) {
          setManifestError(err instanceof Error ? err.message : 'URDF unavailable');
        }
      });
    return () => {
      cancelled = true;
    };
  }, [robotModelKey]);

  const drivableJoints = useMemo(() => {
    if (manifest?.joints === undefined) return [];
    // Hide URDF `<mimic>` joints — they're pinned to a master joint
    // (multiplier·q_master + offset) and cannot be commanded
    // independently. Also hide the masters, because the current
    // MoveIt-planning-group streaming session has no command path to
    // the gripper chain; moving the master slider would do nothing
    // until a dedicated gripper widget owns it. Net effect: the
    // Robotiq gripper chain disappears from the jog panel; the
    // visualisation still follows when the gripper is commanded via
    // its own controller through the deployment's gripper node.
    return manifest.joints.filter(
      (j) =>
        DRIVABLE_TYPES.has(j.type) &&
        !j.mimic &&
        !j.is_mimic_master,
    );
  }, [manifest]);

  return (
    <div className="pointer-events-auto absolute right-2 top-2 z-20 flex max-h-[calc(100%-1rem)] w-72 flex-col overflow-hidden rounded-md border border-gray-700 bg-gray-900/50 text-xs text-gray-200 shadow-xl">
      <div className="flex items-center justify-between border-b border-gray-700 bg-gray-800/50 px-2 py-1">
        <div className="text-[11px] font-semibold uppercase tracking-wide text-gray-300">
          Robot Control
        </div>
        <button
          type="button"
          onClick={onClose}
          className="rounded px-1 text-gray-400 hover:bg-gray-700 hover:text-white"
          title="Close panel"
        >
          ✕
        </button>
      </div>
      <div className="flex gap-1 border-b border-gray-800 bg-gray-900/50 px-2 py-1.5">
        <ModeTab id="joint" active={robotMode} onClick={setRobotMode}>
          Joint
        </ModeTab>
        <ModeTab id="cartesian" active={robotMode} onClick={setRobotMode}>
          Cartesian
        </ModeTab>
        <ModeTab id="move-to" active={robotMode} onClick={setRobotMode}>
          Move To
        </ModeTab>
      </div>
      {robotMode === 'joint' && (
        <JointModeBody
          manifest={manifest}
          manifestError={manifestError}
          drivableJoints={drivableJoints}
          robot={robot}
          jointRadStep={jointRadStep}
          onJointRadStepChange={setJointRadStep}
        />
      )}
      {robotMode === 'cartesian' && (
        <CartesianModeBody
          cartesianFrame={cartesianFrame}
          onFrameChange={setCartesianFrame}
          cartesianMStep={cartesianMStep}
          onMStepChange={setCartesianMStep}
          cartesianRadStep={cartesianRadStep}
          onRadStepChange={setCartesianRadStep}
        />
      )}
      {robotMode === 'move-to' && <MoveToBody />}
    </div>
  );
}

function JointModeBody({
  manifest,
  manifestError,
  drivableJoints,
  robot,
  jointRadStep,
  onJointRadStepChange,
}: {
  manifest: SceneUrdfManifest | null;
  manifestError: string | null;
  drivableJoints: SceneUrdfJoint[];
  robot: SceneRobotState | null;
  jointRadStep: number;
  onJointRadStepChange: (next: number) => void;
}) {
  // The streaming hook lives at the body level so its lifecycle is
  // tied to the joint-mode body — switching the panel to Cartesian
  // unmounts this component and aborts the active session via the
  // hook's cleanup effect. The session joint-name list MUST come from
  // the backend's view (scene.robot.joint_names) rather than the URDF
  // manifest, since the kernel validates incoming names against the
  // session list and the URDF order/set may diverge from what the
  // backend actually uses. Falls back to URDF drivable names when the
  // scene state hasn't loaded yet; the kernel's begin_streaming_session
  // fallback (populates from get_current_state when empty) covers any
  // remaining gap.
  const sessionJointNames = useMemo(
    () => robot?.joint_names ?? drivableJoints.map((j) => j.name),
    [robot, drivableJoints],
  );
  const streaming = useJointStreaming(sessionJointNames);

  return (
    <>
      <div className="flex items-center gap-2 border-b border-gray-800 bg-gray-900/50 px-2 py-1.5">
        <StepCapSelect
          label="step (rad)"
          options={REVOLUTE_STEPS}
          value={jointRadStep}
          onChange={onJointRadStepChange}
        />
      </div>
      <div className="flex-1 overflow-y-auto p-2">
        {manifest === null && manifestError === null && (
          <div className="italic text-gray-500">Loading robot manifest…</div>
        )}
        {manifestError !== null && (
          <div className="italic text-gray-500">
            No robot manifest available ({manifestError}). Load a program/deployment first.
          </div>
        )}
        {manifest !== null && drivableJoints.length === 0 && (
          <div className="italic text-gray-500">
            No drivable joints in this robot.
          </div>
        )}
        {drivableJoints.map((joint) => (
          <JointRow
            key={joint.name}
            joint={joint}
            currentValue={robot?.joint_positions[joint.name] ?? null}
            step={jointRadStep}
            onStreamingPush={streaming.push}
            onStreamingPushFinal={streaming.pushFinal}
          />
        ))}
        <GrippersSection />
        <PoseEditSnapshotSection mode="joint" />
      </div>
      <div className="border-t border-gray-700 bg-gray-800/50 px-2 py-1 text-[10px] text-gray-500">
        Jog · slider streams at 4 Hz · release commits final pose · session auto-closes after 500 ms idle
      </div>
    </>
  );
}

/**
 * Move-To tab body. Operator picks an entity (scene object, named
 * frame, or stored move-node target), an orientation mode, and an
 * optional 6-DoF offset; the panel computes the resulting target
 * pose, the 3D ghost in SceneViewer renders it, and Go dispatches a
 * one-shot planned move via /api/direct/{cartesian-jog | joint-jog}.
 *
 * The actual math lives in `utils/moveTo.ts` so the ghost preview
 * computes the same target as the dispatched Go — no drift between
 * what the operator sees and what runs.
 */
function MoveToBody() {
  const moveTo = useViewerSelectionStore((s) => s.moveTo);
  const patchMoveTo = useViewerSelectionStore((s) => s.patchMoveTo);
  const objects = useSceneStateStore((s) => s.scene?.objects ?? EMPTY_OBJECTS);
  const tcpPose = useSceneStateStore((s) => s.scene?.robot?.tcp_pose_in_base ?? null);
  const appliedTree = useProgramStore((s) => s.appliedTree);

  const [frames, setFrames] = useState<SceneFrameState[]>([]);
  const [status, setStatus] = useState<'idle' | 'sending' | 'error' | 'success'>('idle');
  const [errorDetail, setErrorDetail] = useState<string | null>(null);

  // Refresh named frames every time the Move-To tab mounts so the
  // operator sees what is currently in the scene (frames can be
  // added/removed via deployment edits, tool changes, etc.).
  useEffect(() => {
    let cancelled = false;
    void api.scene
      .frames()
      .then((data) => {
        if (!cancelled) setFrames(data.frames.filter((f) => f.poseAvailable));
      })
      .catch(() => {
        if (!cancelled) setFrames([]);
      });
    return () => {
      cancelled = true;
    };
  }, []);

  const eligibleObjects = useMemo(
    () => objects.filter((o) => !o.attached),
    [objects],
  );

  const storedPoses = useMemo<StoredPoseTarget[]>(() => {
    const out: StoredPoseTarget[] = [];
    for (const m of collectMoveNodes(appliedTree, 'pose_goal')) {
      out.push({ nodeName: m.name, motionType: 'pose_goal', params: m.params });
    }
    for (const m of collectMoveNodes(appliedTree, 'joint_goal')) {
      out.push({ nodeName: m.name, motionType: 'joint_goal', params: m.params });
    }
    return out;
  }, [appliedTree]);

  const target = useMemo(
    () =>
      computeMoveToTarget({
        moveTo,
        objects: eligibleObjects,
        frames,
        storedPoses,
        tcpPose,
      }),
    [moveTo, eligibleObjects, frames, storedPoses, tcpPose],
  );

  function doGo(): void {
    if (target === null) {
      setStatus('error');
      setErrorDetail('no valid target selected');
      return;
    }
    setStatus('sending');
    setErrorDetail(null);
    const dispatched =
      target.kind === 'joint'
        ? api.direct.jointJog(target.jointNames, target.jointPositionsRad)
        : api.direct.cartesianJog(target.position, target.quaternion);
    void dispatched
      .then(() => {
        setStatus('success');
        setErrorDetail(null);
        window.setTimeout(() => {
          setStatus((current) => (current === 'success' ? 'idle' : current));
        }, 1500);
      })
      .catch((err: unknown) => {
        setStatus('error');
        setErrorDetail(err instanceof Error ? err.message : 'move failed');
      });
  }

  return (
    <>
      <div className="flex items-center gap-1 border-b border-gray-800 bg-gray-900/50 px-2 py-1.5 text-[10px]">
        <span className="text-gray-400">source</span>
        <SourceChip current={moveTo.source} value="object" onClick={(v) => patchMoveTo({ source: v })}>
          object
        </SourceChip>
        <SourceChip current={moveTo.source} value="frame" onClick={(v) => patchMoveTo({ source: v })}>
          frame
        </SourceChip>
        <SourceChip current={moveTo.source} value="pose" onClick={(v) => patchMoveTo({ source: v })}>
          pose
        </SourceChip>
      </div>

      <div className="flex-1 overflow-y-auto p-2">
        {moveTo.source === 'object' && (
          <EntityList
            label="Objects"
            items={eligibleObjects.map((o) => ({
              key: o.object_id,
              label: o.object_id,
              detail: `${o.source}${o.shape ? ` · ${String((o.shape as { type?: unknown }).type ?? '')}` : ''}`,
            }))}
            selectedKey={moveTo.objectId}
            emptyLabel="No movable objects in scene."
            onSelect={(key) => patchMoveTo({ objectId: key })}
          />
        )}

        {moveTo.source === 'frame' && (
          <EntityList
            label="Named frames"
            items={frames.map((f) => ({
              key: f.name,
              label: f.name,
              detail: f.roles.length > 0 ? f.roles.join(', ') : '',
            }))}
            selectedKey={moveTo.frameName}
            emptyLabel="No named frames with poses available."
            onSelect={(key) => patchMoveTo({ frameName: key })}
          />
        )}

        {moveTo.source === 'pose' && (
          <EntityList
            label="Stored poses"
            items={storedPoses.map((p) => ({
              key: p.nodeName,
              label: p.nodeName,
              detail: p.motionType,
            }))}
            selectedKey={moveTo.poseNodeName}
            emptyLabel="No move nodes in current program."
            onSelect={(key) => patchMoveTo({ poseNodeName: key })}
          />
        )}

        <div className="mt-3 border-t border-gray-800 pt-2">
          <div className="mb-1 text-[10px] uppercase tracking-wide text-gray-500">
            Orientation
          </div>
          <div className="flex gap-1">
            <OrientationChip
              current={moveTo.orientationMode}
              value="keep-current"
              onClick={(v) => patchMoveTo({ orientationMode: v })}
            >
              keep TCP
            </OrientationChip>
            <OrientationChip
              current={moveTo.orientationMode}
              value="face-down"
              onClick={(v) => patchMoveTo({ orientationMode: v })}
            >
              face down
            </OrientationChip>
            <OrientationChip
              current={moveTo.orientationMode}
              value="use-entity"
              onClick={(v) => patchMoveTo({ orientationMode: v })}
            >
              use entity
            </OrientationChip>
          </div>
        </div>

        <div className="mt-3 border-t border-gray-800 pt-2">
          <div className="mb-1 text-[10px] uppercase tracking-wide text-gray-500">
            Offset (in target local frame)
          </div>
          <OffsetRow
            label="xyz (m)"
            axes={['x', 'y', 'z']}
            values={moveTo.offsetXyzM}
            step={0.01}
            onChange={(next) => patchMoveTo({ offsetXyzM: next })}
          />
          <OffsetRow
            label="rpy (deg)"
            axes={['r', 'p', 'y']}
            values={moveTo.offsetRpyDeg}
            step={1}
            onChange={(next) => patchMoveTo({ offsetRpyDeg: next })}
          />
        </div>

        <div className="mt-3 border-t border-gray-800 pt-2">
          <button
            type="button"
            onClick={doGo}
            disabled={target === null || status === 'sending'}
            className="w-full rounded bg-blue-700 px-2 py-1 text-[11px] text-white hover:bg-blue-600 disabled:cursor-not-allowed disabled:opacity-50"
          >
            {status === 'sending' ? 'Sending…' : 'Go'}
          </button>
          <MoveToStatusLine
            status={status}
            errorDetail={errorDetail}
            target={target}
          />
        </div>
      </div>

      <div className="border-t border-gray-700 bg-gray-800/50 px-2 py-1 text-[10px] text-gray-500">
        Move To · planned move via /api/direct/{'{cartesian|joint}'}-jog · ghost in 3D updates as you edit
      </div>
    </>
  );
}

function SourceChip({
  current,
  value,
  onClick,
  children,
}: {
  current: MoveToSource;
  value: MoveToSource;
  onClick: (value: MoveToSource) => void;
  children: React.ReactNode;
}) {
  const active = current === value;
  return (
    <button
      type="button"
      onClick={() => onClick(value)}
      className={`rounded px-2 py-0.5 text-[10px] uppercase tracking-wide ${
        active ? 'bg-gray-700 text-white' : 'bg-gray-800 text-gray-400 hover:text-gray-200'
      }`}
    >
      {children}
    </button>
  );
}

function OrientationChip({
  current,
  value,
  onClick,
  children,
}: {
  current: MoveToOrientationMode;
  value: MoveToOrientationMode;
  onClick: (value: MoveToOrientationMode) => void;
  children: React.ReactNode;
}) {
  const active = current === value;
  return (
    <button
      type="button"
      onClick={() => onClick(value)}
      className={`flex-1 rounded px-2 py-1 text-[10px] ${
        active ? 'bg-gray-700 text-white' : 'bg-gray-800 text-gray-400 hover:text-gray-200'
      }`}
    >
      {children}
    </button>
  );
}

function EntityList({
  label,
  items,
  selectedKey,
  emptyLabel,
  onSelect,
}: {
  label: string;
  items: { key: string; label: string; detail: string }[];
  selectedKey: string | null;
  emptyLabel: string;
  onSelect: (key: string) => void;
}) {
  return (
    <div>
      <div className="mb-1 text-[10px] uppercase tracking-wide text-gray-500">{label}</div>
      {items.length === 0 ? (
        <div className="italic text-[11px] text-gray-500">{emptyLabel}</div>
      ) : (
        <div className="space-y-0.5">
          {items.map((item) => (
            <button
              key={item.key}
              type="button"
              onClick={() => onSelect(item.key)}
              className={`flex w-full items-center justify-between gap-2 rounded px-2 py-1 text-left text-[11px] ${
                item.key === selectedKey
                  ? 'bg-blue-900/40 text-white'
                  : 'bg-gray-800/40 text-gray-300 hover:bg-gray-800'
              }`}
            >
              <span className="truncate font-mono">{item.label}</span>
              <span className="shrink-0 text-[9px] text-gray-500">{item.detail}</span>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}

function OffsetRow({
  label,
  axes,
  values,
  step,
  onChange,
}: {
  label: string;
  axes: readonly [string, string, string];
  values: [number, number, number];
  step: number;
  onChange: (next: [number, number, number]) => void;
}) {
  function setAxis(idx: 0 | 1 | 2, raw: string): void {
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    const next: [number, number, number] = [values[0], values[1], values[2]];
    next[idx] = v;
    onChange(next);
  }
  // 4-column grid: a narrow label cell + three equal axis cells.
  // Native number-spinner arrows are hidden via the `no-spin` utility
  // class (defined in globals) so the entire visible width is for the
  // digits — otherwise the spinners ate ~16px per input and forced an
  // overflow in the w-72 panel.
  return (
    <div className="mb-1 grid grid-cols-[3rem_1fr_1fr_1fr] items-center gap-1">
      <span className="text-[10px] text-gray-500">{label}</span>
      {axes.map((axis, idx) => (
        <label
          key={axis}
          className="flex items-center gap-0.5 font-mono text-[10px]"
          title={`${label} ${axis}`}
        >
          <span className="text-gray-500">{axis}</span>
          <input
            type="number"
            step={step}
            value={values[idx as 0 | 1 | 2]}
            onChange={(e) => setAxis(idx as 0 | 1 | 2, e.target.value)}
            className="no-spin w-full min-w-0 rounded bg-gray-800 px-1 py-0.5 text-right text-gray-200
              [appearance:textfield]
              [&::-webkit-inner-spin-button]:appearance-none
              [&::-webkit-outer-spin-button]:appearance-none"
          />
        </label>
      ))}
    </div>
  );
}

function MoveToStatusLine({
  status,
  errorDetail,
  target,
}: {
  status: 'idle' | 'sending' | 'success' | 'error';
  errorDetail: string | null;
  target: import('../utils/moveTo').MoveToTarget | null;
}) {
  if (status === 'idle' && target !== null) {
    return (
      <div className="mt-1 space-y-0.5 text-[10px] text-gray-500" title="Target previewed in 3D">
        <div className="truncate">target: {target.label}</div>
        {target.kind === 'cartesian' ? (
          <div className="font-mono text-[9px] text-gray-400">
            xyz: {target.position.map((v) => v.toFixed(3)).join(', ')}
            <br />
            quat: {target.quaternion.map((v) => v.toFixed(3)).join(', ')}
          </div>
        ) : (
          <div className="font-mono text-[9px] text-gray-400">
            joint_goal · {target.jointNames.length} joints
          </div>
        )}
      </div>
    );
  }
  if (status === 'idle') {
    return (
      <div className="mt-1 text-[10px] text-gray-500">Pick a source + entity to set target.</div>
    );
  }
  if (status === 'sending') {
    return <div className="mt-1 text-[10px] text-blue-300">sending move…</div>;
  }
  if (status === 'success') {
    return <div className="mt-1 text-[10px] text-green-300">✓ accepted</div>;
  }
  return (
    <div className="mt-1 text-[10px] text-red-300" title={errorDetail ?? undefined}>
      ✗ {(errorDetail ?? 'move failed').slice(0, 60)}
    </div>
  );
}

const EMPTY_OBJECTS: never[] = [];

function ModeTab({
  id,
  active,
  onClick,
  disabled = false,
  children,
}: {
  id: 'joint' | 'cartesian' | 'move-to' | 'pose-edit';
  active: 'joint' | 'cartesian' | 'move-to' | 'pose-edit';
  onClick: (id: 'joint' | 'cartesian' | 'move-to' | 'pose-edit') => void;
  disabled?: boolean;
  children: React.ReactNode;
}) {
  const isActive = id === active;
  return (
    <button
      type="button"
      onClick={() => !disabled && onClick(id)}
      disabled={disabled}
      className={`flex-1 rounded px-2 py-1 text-[11px] font-medium uppercase tracking-wide disabled:cursor-not-allowed disabled:opacity-40 ${
        isActive
          ? 'bg-gray-700 text-white'
          : 'bg-gray-800 text-gray-400 hover:text-gray-200'
      }`}
    >
      {children}
    </button>
  );
}

function CartesianModeBody({
  cartesianFrame,
  onFrameChange,
  cartesianMStep,
  onMStepChange,
  cartesianRadStep,
  onRadStepChange,
}: {
  cartesianFrame: CartesianFrame;
  onFrameChange: (frame: CartesianFrame) => void;
  cartesianMStep: number;
  onMStepChange: (next: number) => void;
  cartesianRadStep: number;
  onRadStepChange: (next: number) => void;
}) {
  const robot = useSceneStateStore((s) => s.scene?.robot ?? null);
  const tcp = robot?.tcp_pose_in_base ?? null;
  const gizmoMode = useViewerSelectionStore((s) => s.cartesianGizmoMode);
  const setGizmoMode = useViewerSelectionStore((s) => s.setCartesianGizmoMode);

  const positionM = useMemo(() => (tcp === null ? [0, 0, 0] : readPosition(tcp)), [tcp]);
  const rpyRad = useMemo(
    () => (tcp === null ? [0, 0, 0] : quatToRpyRad(readQuat(tcp))),
    [tcp],
  );

  function commitXyz(next: [number, number, number]): void {
    if (tcp === null) return;
    const quat = readQuat(tcp) as [number, number, number, number];
    void api.direct.cartesianJog(next, quat).catch((err) => {
      console.warn('cartesian jog (xyz) failed', err);
    });
  }

  function commitRpy(nextRpy: [number, number, number]): void {
    if (tcp === null) return;
    const quat = rpyRadToQuat(nextRpy);
    const pos = readPosition(tcp) as [number, number, number];
    void api.direct.cartesianJog(pos, quat).catch((err) => {
      console.warn('cartesian jog (rpy) failed', err);
    });
  }

  return (
    <>
      <div className="flex flex-wrap items-center gap-2 border-b border-gray-800 bg-gray-900/50 px-2 py-1.5">
        <label className="flex items-center gap-1 text-[10px] text-gray-400">
          <span>frame</span>
          <select
            value={cartesianFrame}
            onChange={(e) => onFrameChange(e.target.value as CartesianFrame)}
            className="rounded bg-gray-800 px-1 py-0.5 font-mono text-[10px] text-gray-200"
          >
            <option value="world">world</option>
            <option value="base">base</option>
            <option value="tcp">tcp</option>
          </select>
        </label>
        <div className="flex items-center gap-1">
          <GizmoModeChip
            mode="translate"
            current={gizmoMode}
            onClick={() => setGizmoMode('translate')}
          />
          <GizmoModeChip
            mode="rotate"
            current={gizmoMode}
            onClick={() => setGizmoMode('rotate')}
          />
        </div>
      </div>
      <div className="flex items-center gap-2 border-b border-gray-800 bg-gray-900/50 px-2 py-1.5">
        <StepCapSelect
          label="step (m)"
          options={PRISMATIC_STEPS}
          value={cartesianMStep}
          onChange={onMStepChange}
        />
        <StepCapSelect
          label="step (rad)"
          options={REVOLUTE_STEPS}
          value={cartesianRadStep}
          onChange={onRadStepChange}
        />
      </div>
      <div className="flex-1 overflow-y-auto p-2 text-[11px]">
        {tcp === null ? (
          <div className="italic text-gray-500">
            TCP pose unavailable. Load a deployment with a configured tcp_frame.
          </div>
        ) : (
          <>
            <CartesianAxisFields
              label="position (m, in base frame)"
              labels={['x', 'y', 'z']}
              values={positionM}
              step={cartesianMStep}
              onCommit={commitXyz}
            />
            <CartesianAxisFields
              label="orientation (rpy, rad)"
              labels={['r', 'p', 'y']}
              values={rpyRad}
              step={cartesianRadStep}
              onCommit={commitRpy}
              wrap
            />
          </>
        )}
        <PoseEditSnapshotSection mode="cartesian" />
      </div>
      <div className="border-t border-gray-700 bg-gray-800/50 px-2 py-1 text-[10px] text-gray-500">
        Cartesian · gizmo + fields · commits return to IDLE between releases
      </div>
    </>
  );
}

function GizmoModeChip({
  mode,
  current,
  onClick,
}: {
  mode: 'translate' | 'rotate';
  current: 'translate' | 'rotate';
  onClick: () => void;
}) {
  const active = mode === current;
  return (
    <button
      type="button"
      onClick={onClick}
      className={`rounded px-2 py-0.5 text-[10px] font-medium ${
        active
          ? 'bg-gray-700 text-white'
          : 'bg-gray-800 text-gray-400 hover:text-gray-200'
      }`}
      title={mode === 'translate' ? 'Linear gizmo (drag arrows)' : 'Rotational gizmo (drag rings)'}
    >
      {mode}
    </button>
  );
}

function CartesianAxisFields({
  label,
  labels,
  values,
  step,
  onCommit,
  wrap = false,
}: {
  label: string;
  labels: readonly [string, string, string];
  values: number[];
  step: number;
  onCommit: (next: [number, number, number]) => void;
  wrap?: boolean;
}) {
  const [drafts, setDrafts] = useState<Partial<Record<0 | 1 | 2, string>>>({});
  function fire(idx: 0 | 1 | 2, raw: string): void {
    const v = Number(raw);
    if (!Number.isFinite(v)) {
      setDrafts((d) => ({ ...d, [idx]: undefined }));
      return;
    }
    const next: [number, number, number] = [values[0], values[1], values[2]];
    next[idx] = v;
    setDrafts((d) => ({ ...d, [idx]: undefined }));
    onCommit(next);
  }
  return (
    <div className={wrap ? 'mt-2' : ''}>
      <div className="text-[10px] uppercase tracking-wide text-gray-500">{label}</div>
      <div className="mt-1 grid grid-cols-3 gap-1.5">
        {labels.map((axis, idx) => {
          const i = idx as 0 | 1 | 2;
          const live = values[i] ?? 0;
          const draft = drafts[i];
          return (
            <div key={axis} className="flex min-w-0 flex-col gap-0.5 font-mono text-[10px] text-gray-300">
              <span className="text-[9px] uppercase text-gray-500">{axis}</span>
              <input
                type="number"
                step={step}
                value={draft ?? live.toFixed(3)}
                onChange={(e) => {
                  setDrafts((d) => ({ ...d, [i]: e.target.value }));
                  // Spinner-button click → immediate commit
                  // (no inputType on native InputEvent).
                  const native = e.nativeEvent as InputEvent;
                  if (!native.inputType) fire(i, e.target.value);
                }}
                onBlur={() => {
                  if (drafts[i] === undefined) return;
                  fire(i, drafts[i] as string);
                }}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') {
                    e.preventDefault();
                    (e.target as HTMLInputElement).blur();
                  } else if (e.key === 'Escape') {
                    e.preventDefault();
                    setDrafts((d) => ({ ...d, [i]: undefined }));
                    (e.target as HTMLInputElement).blur();
                  }
                }}
                className="w-full min-w-0 rounded bg-gray-800 py-0.5 pl-1 pr-2 text-right text-gray-200"
              />
            </div>
          );
        })}
      </div>
    </div>
  );
}

function quatToRpyRad(quat: number[]): [number, number, number] {
  const [x, y, z, w] = quat;
  // Convert xyzw quaternion to XYZ-Euler (roll about X, pitch about Y, yaw about Z),
  // matching Three.js's "XYZ" order which is the convention used elsewhere in the
  // viewer (visual_origin_rpy_rad on URDF visuals also use this order).
  const sinr_cosp = 2 * (w * x + y * z);
  const cosr_cosp = 1 - 2 * (x * x + y * y);
  const roll = Math.atan2(sinr_cosp, cosr_cosp);
  const sinp = 2 * (w * y - z * x);
  let pitch: number;
  if (Math.abs(sinp) >= 1) {
    pitch = Math.sign(sinp) * Math.PI / 2;
  } else {
    pitch = Math.asin(sinp);
  }
  const siny_cosp = 2 * (w * z + x * y);
  const cosy_cosp = 1 - 2 * (y * y + z * z);
  const yaw = Math.atan2(siny_cosp, cosy_cosp);
  return [roll, pitch, yaw];
}

function rpyRadToQuat(rpy: [number, number, number]): [number, number, number, number] {
  const [r, p, y] = rpy;
  const cr = Math.cos(r / 2);
  const sr = Math.sin(r / 2);
  const cp = Math.cos(p / 2);
  const sp = Math.sin(p / 2);
  const cy = Math.cos(y / 2);
  const sy = Math.sin(y / 2);
  const w = cr * cp * cy + sr * sp * sy;
  const qx = sr * cp * cy - cr * sp * sy;
  const qy = cr * sp * cy + sr * cp * sy;
  const qz = cr * cp * sy - sr * sp * cy;
  return [qx, qy, qz, w];
}

/**
 * Pose-edit snapshot section. Appears at the bottom of both the
 * Joint and Cartesian mode bodies (no longer its own tab).
 *
 * Semantics changed in this revision: instead of letting the
 * operator type values into sliders that don't move the live arm,
 * this section now *snapshots* the live arm state and writes it
 * into a selected move-node's params. The operator jogs the arm
 * (via Joint sliders or the Cartesian gizmo / fields) to the
 * desired pose, then picks a target move and clicks either
 * "Stage as runtime override" (session-only, gated by the node's
 * runtime_override policy) or "Save to program (draft)" (mutates
 * the local editor draft, lights up Apply/Save in the toolbar).
 *
 * For `mode='joint'`: snapshots `robot.joint_positions` ordered by
 * `robot.joint_names` into `joint_goal_positions_rad`. Filters
 * target dropdown to move_manipulator_action nodes with
 * motion_type=joint_goal.
 *
 * For `mode='cartesian'`: snapshots `robot.tcp_pose_in_base` into
 * `pose_goal: {position_m, orientation_xyzw}`. Filters to nodes
 * with motion_type=pose_goal.
 */
/**
 * Per-deployment gripper widget. Mirrors what the BT `command_gripper`
 * node does — looks up the gripper config (group_id, joint_name,
 * motion_time_s) from the active deployment, builds a
 * `GripperCommandRequest`, and hands it to the kernel. The operator
 * picks a target position via slider or SRDF-named preset (e.g. UR10E
 * Robotiq has `open=0.0` and `closed=0.66`); the kernel reports
 * live state back through `/api/scene/state.grippers`.
 *
 * Joint-mimic chains (Robotiq's knuckle/finger joints chained to
 * `finger_joint`) follow automatically through FK once the master is
 * commanded — the jog panel hides the entire chain and routes here.
 */
function GrippersSection() {
  const grippers = useSceneStateStore((s) => s.scene?.grippers ?? EMPTY_GRIPPERS);
  if (grippers.length === 0) return null;
  return (
    <div className="mt-3 space-y-2 border-t border-gray-800 pt-3">
      <div className="px-1 text-[10px] font-semibold uppercase tracking-wide text-gray-400">
        Grippers
      </div>
      {grippers.map((g) => (
        <GripperRow key={g.gripper_id} gripper={g} />
      ))}
    </div>
  );
}

const EMPTY_GRIPPERS: SceneGripperState[] = [];

function GripperRow({ gripper }: { gripper: SceneGripperState }) {
  // Slider mirrors the JointRow pattern: it tracks the live joint
  // value reported by the kernel and only diverges while the operator
  // is actively dragging (`pending` non-null until mouseup). The
  // command target is displayed separately in the cmd label so the
  // operator sees both "where the finger actually is" and "where we
  // asked it to go".
  const [pending, setPending] = useState<number | null>(null);
  // Anything that releases the BT runner (Run, an interrupt handler,
  // a direct-control session opened from a sibling panel) clears the
  // operator's stale drag value. Without this, a drag that didn't
  // close cleanly (mouseup outside the slider, a torn touch event)
  // leaves pending set forever and the slider stops following live
  // state on the next cycle transition.
  const cycleState = useCycleStore((s) => s.state);
  useEffect(() => {
    if (
      cycleState === 'running'
      || cycleState === 'interrupt_handler'
      || cycleState === 'direct_control'
    ) {
      setPending(null);
    }
  }, [cycleState]);
  const lower = gripper.limit_lower ?? 0;
  const upper = gripper.limit_upper ?? 1;
  const current = gripper.current_position ?? lower;
  const target = gripper.target_position ?? current;
  // Clamp the displayed slider value to the URDF limits so a kernel
  // overshoot or a stale-state read can't push the input outside the
  // declared range (HTML5 range silently snaps; we just want the
  // displayed number to be honest).
  const displayValue = Math.min(
    upper,
    Math.max(lower, pending ?? current),
  );
  const presetEntries = useMemo(
    () => Object.entries(gripper.srdf_presets ?? {}),
    [gripper.srdf_presets],
  );

  function sendCommand(value: number): void {
    setPending(null);
    void api.scene
      .commandGripper({ gripperId: gripper.gripper_id, targetPosition: value })
      .catch((err) => console.warn('gripper command failed', err));
  }

  const stateLabel = gripper.state.replace(/^GripperCommandState\./, '');
  const busy = stateLabel.toLowerCase() === 'in_progress';
  return (
    <div className="space-y-1 rounded border border-gray-700 bg-gray-900/40 p-2">
      <div className="flex items-baseline justify-between gap-2">
        <span className="font-mono text-[11px] text-gray-100">
          {gripper.gripper_id}
        </span>
        <span className="text-[10px] text-gray-500">{gripper.joint_name}</span>
      </div>
      <div className="flex items-baseline justify-between gap-2 text-[10px] text-gray-400">
        <span>
          live:{' '}
          <span className="font-mono text-gray-300">{current.toFixed(3)}</span>
        </span>
        <span>
          cmd:{' '}
          <span className="font-mono text-gray-300">{target.toFixed(3)}</span>
        </span>
        <span className={busy ? 'text-amber-400' : 'text-gray-500'}>
          {stateLabel}
        </span>
      </div>
      <input
        type="range"
        min={lower}
        max={upper}
        // `step="any"` — same fix as JointRow. Without it Firefox marks
        // the slider invalid whenever the live kernel value lands
        // off-grid (which is almost always for a sim/real gripper
        // mid-motion) and shows a hover tooltip "Please select a valid
        // value, the two nearest valid values are…". The streaming
        // path validates the requested target server-side, so the
        // step-grid constraint adds no safety.
        step="any"
        value={displayValue}
        onChange={(e) => setPending(Number(e.target.value))}
        onMouseUp={(e) => sendCommand(Number((e.target as HTMLInputElement).value))}
        onTouchEnd={(e) => sendCommand(Number((e.target as HTMLInputElement).value))}
        className="w-full"
      />
      <div className="flex flex-wrap gap-1">
        {presetEntries.length > 0 ? (
          presetEntries.map(([name, value]) => (
            <button
              key={name}
              type="button"
              onClick={() => sendCommand(value)}
              className="rounded border border-gray-600 px-2 py-0.5 text-[10px] text-gray-200 hover:bg-gray-700"
              title={`${name} = ${value.toFixed(3)}`}
            >
              {name}
            </button>
          ))
        ) : (
          <>
            <button
              type="button"
              onClick={() => sendCommand(lower)}
              className="rounded border border-gray-600 px-2 py-0.5 text-[10px] text-gray-200 hover:bg-gray-700"
              title={`lower limit (${lower.toFixed(3)})`}
            >
              ↓ {lower.toFixed(2)}
            </button>
            <button
              type="button"
              onClick={() => sendCommand(upper)}
              className="rounded border border-gray-600 px-2 py-0.5 text-[10px] text-gray-200 hover:bg-gray-700"
              title={`upper limit (${upper.toFixed(3)})`}
            >
              ↑ {upper.toFixed(2)}
            </button>
          </>
        )}
      </div>
      {gripper.error && (
        <div className="text-[10px] text-red-400">{gripper.error}</div>
      )}
    </div>
  );
}

function PoseEditSnapshotSection({ mode }: { mode: 'joint' | 'cartesian' }) {
  const appliedTree = useProgramStore((s) => s.appliedTree);
  const wantMotionType = mode === 'joint' ? 'joint_goal' : 'pose_goal';
  const editableNodes = useMemo(
    () => collectMoveNodes(appliedTree, wantMotionType),
    [appliedTree, wantMotionType],
  );
  const robot = useSceneStateStore((s) => s.scene?.robot ?? null);
  const [selectedNodeName, setSelectedNodeName] = useState<string | null>(null);
  const [status, setStatus] = useState<{
    kind: 'idle' | 'sending' | 'success' | 'error';
    detail?: string;
  }>({ kind: 'idle' });

  function snapshotParams(): Record<string, unknown> | null {
    if (mode === 'joint') {
      const names = robot?.joint_names ?? [];
      if (names.length === 0) return null;
      const positions = names.map((n) => Number(robot?.joint_positions?.[n] ?? 0));
      return {
        joint_goal_names: names,
        joint_goal_positions_rad: positions,
      };
    }
    const tcp = robot?.tcp_pose_in_base;
    if (!tcp) return null;
    return {
      pose_goal: {
        position_m: readPosition(tcp),
        orientation_xyzw: readQuat(tcp),
      },
    };
  }

  async function stageOverride(): Promise<void> {
    if (selectedNodeName === null) return;
    const params = snapshotParams();
    if (params === null) {
      setStatus({ kind: 'error', detail: 'no live state to snapshot' });
      return;
    }
    setStatus({ kind: 'sending' });
    try {
      await api.program.applyRuntimeOverride(
        selectedNodeName,
        params,
        `Pose snapshot (${mode}) from native viewer`,
      );
      setStatus({ kind: 'success', detail: 'staged' });
    } catch (err: unknown) {
      setStatus({
        kind: 'error',
        detail: err instanceof Error ? err.message : String(err),
      });
    }
  }

  function saveToDraft(): void {
    if (selectedNodeName === null) return;
    const params = snapshotParams();
    if (params === null) {
      setStatus({ kind: 'error', detail: 'no live state to snapshot' });
      return;
    }
    const flowNodes = useProgramStore.getState().nodes;
    const flowNode = flowNodes.find((n) => n.data.label === selectedNodeName);
    if (flowNode === undefined) {
      setStatus({
        kind: 'error',
        detail: `Node '${selectedNodeName}' not found in editor tree`,
      });
      return;
    }
    const mergedParams = {
      ...(flowNode.data.params as Record<string, unknown>),
      ...params,
    };
    useProgramStore.getState().updateNodeParams(flowNode.id, mergedParams);
    setStatus({
      kind: 'success',
      detail: 'editor draft updated — Apply / Save in toolbar',
    });
  }

  const motionLabel = mode === 'joint' ? 'joint-goal' : 'pose-goal';
  const captureSource = mode === 'joint' ? 'live joint state' : 'live TCP pose';

  return (
    <div className="mt-4 border-t border-gray-800 pt-3">
      <h4 className="text-[11px] uppercase tracking-wide text-gray-400">
        Apply to stored move
      </h4>
      <p className="mt-1 text-[10px] text-gray-500">
        Captures the {captureSource} and writes it to the selected{' '}
        {motionLabel} move. Doesn't move the robot.
      </p>
      <label className="mt-2 flex items-center gap-1 text-[10px] text-gray-400">
        <span className="shrink-0">target</span>
        <select
          value={selectedNodeName ?? ''}
          onChange={(e) => setSelectedNodeName(e.target.value || null)}
          className="min-w-0 flex-1 rounded bg-gray-800 px-1 py-0.5 font-mono text-[10px] text-gray-200"
        >
          <option value="">— select a {motionLabel} move —</option>
          {editableNodes.map((n) => (
            <option key={n.name} value={n.name}>
              {n.name}
            </option>
          ))}
        </select>
      </label>
      {editableNodes.length === 0 && (
        <div className="mt-1 text-[10px] italic text-gray-500">
          No {motionLabel} moves in the loaded program.
        </div>
      )}
      <div className="mt-2 flex gap-1">
        <button
          type="button"
          onClick={stageOverride}
          disabled={selectedNodeName === null || status.kind === 'sending'}
          className="flex-1 rounded bg-gray-700 px-2 py-1 text-[11px] text-gray-200 hover:bg-gray-600 disabled:cursor-not-allowed disabled:opacity-50"
        >
          Stage as runtime override
        </button>
        <button
          type="button"
          onClick={saveToDraft}
          disabled={selectedNodeName === null || status.kind === 'sending'}
          className="flex-1 rounded bg-blue-700 px-2 py-1 text-[11px] text-white hover:bg-blue-600 disabled:cursor-not-allowed disabled:opacity-50"
        >
          Save to program (draft)
        </button>
      </div>
      <div className="mt-1 min-h-[14px] text-[10px]">
        {status.kind === 'idle' && (
          <span className="text-gray-500">
            Stage = runtime override (session) · Save = editor draft (Apply / Save in toolbar)
          </span>
        )}
        {status.kind === 'sending' && <span className="text-blue-300">sending…</span>}
        {status.kind === 'success' && (
          <span className="text-green-300">✓ {status.detail ?? 'ok'}</span>
        )}
        {status.kind === 'error' && (
          <span className="text-red-300" title={status.detail}>
            ✗ {(status.detail ?? 'failed').slice(0, 60)}
          </span>
        )}
      </div>
    </div>
  );
}



function collectMoveNodes(
  tree: TreeNode | null,
  motionType: 'joint_goal' | 'pose_goal' | 'relative_move',
): Array<{ name: string; params: Record<string, unknown> }> {
  const out: Array<{ name: string; params: Record<string, unknown> }> = [];
  function visit(node: TreeNode): void {
    if (node.id === 'move_manipulator_action') {
      if (node.params['motion_type'] === motionType) {
        out.push({ name: node.name, params: node.params });
      }
    }
    for (const child of node.children ?? []) {
      visit(child);
    }
  }
  if (tree !== null) visit(tree);
  return out;
}



function readPosition(pose: Record<string, unknown>): number[] {
  const p = pose.position_m;
  if (Array.isArray(p) && p.length === 3) {
    return p.map((v) => Number(v) || 0);
  }
  return [0, 0, 0];
}

function readQuat(pose: Record<string, unknown>): number[] {
  const q = pose.orientation_xyzw;
  if (Array.isArray(q) && q.length === 4) {
    return q.map((v) => Number(v) || 0);
  }
  return [0, 0, 0, 1];
}

function StepCapSelect({
  label,
  options,
  value,
  onChange,
  disabled = false,
}: {
  label: string;
  options: readonly number[];
  value: number;
  onChange: (next: number) => void;
  disabled?: boolean;
}) {
  return (
    <label
      className={`flex items-center gap-1 text-[10px] ${
        disabled ? 'text-gray-600' : 'text-gray-400'
      }`}
    >
      <span>{label}</span>
      <select
        value={value}
        onChange={(e) => onChange(Number(e.target.value))}
        disabled={disabled}
        className="rounded bg-gray-800 px-1 py-0.5 font-mono text-[10px] text-gray-200 disabled:cursor-not-allowed disabled:opacity-50"
      >
        {options.map((opt) => (
          <option key={opt} value={opt}>
            {opt}
          </option>
        ))}
      </select>
    </label>
  );
}

function JointRow({
  joint,
  currentValue,
  step,
  onStreamingPush,
  onStreamingPushFinal,
}: {
  joint: SceneUrdfJoint;
  currentValue: number | null;
  step: number;
  onStreamingPush: (jointName: string, valueRad: number) => void;
  onStreamingPushFinal: (jointName: string, valueRad: number) => void;
}) {
  // Joint-limit-based range: explicit limits win; continuous joints
  // default to [-π, π]; otherwise center on live value ± a window.
  // The slider is free to move limit-to-limit; `step` only drives
  // the numeric-input arrow buttons.
  const jointRange = useMemo(
    () => sliderRange(joint, currentValue),
    [joint, currentValue],
  );
  const [draft, setDraft] = useState<string | null>(null);
  const displayValue = currentValue ?? 0;
  const draftNumeric = draft === null ? displayValue : Number(draft);
  const sliderValue = Number.isFinite(draftNumeric)
    ? Math.min(jointRange.max, Math.max(jointRange.min, draftNumeric))
    : displayValue;

  // Clear the draft when the live value catches up — happens once
  // the streamed pose lands and scene-state polls in the new
  // position. Use the same tolerance as the session close gate:
  // real controllers may not report exact floating-point equality,
  // and a draft that never clears makes the slider/input look stuck
  // while live state is actually moving underneath.
  useEffect(() => {
    if (draft !== null && Number.isFinite(Number(draft)) && currentValue !== null) {
      if (Math.abs(Number(draft) - currentValue) < JOINT_ARRIVAL_TOLERANCE_RAD) {
        setDraft(null);
      }
    }
  }, [currentValue, draft]);

  // Also clear on any cycle transition that hands the joint over to
  // an authoritative non-operator source (BT runner, interrupt
  // handler, direct-control session). Without this, a draft set by
  // the numeric input's arrow keys or by a slider drag that didn't
  // close cleanly leaves the slider frozen at the operator's last
  // commanded value while live state moves underneath.
  const cycleState = useCycleStore((s) => s.state);
  useEffect(() => {
    if (
      cycleState === 'running'
      || cycleState === 'interrupt_handler'
      || cycleState === 'direct_control'
    ) {
      setDraft(null);
    }
  }, [cycleState]);

  // And clear when the follow-target streaming session aborts /
  // returns to idle (operator hit Stop mid-motion). The post-stop
  // refresh in ControlBar updates `currentValue` to the actual
  // stopped pose immediately; this just ensures we're not still
  // sitting on a torn-drag draft that would render before the new
  // currentValue lands.
  useEffect(() => {
    const unsubscribe = followTargetController.subscribe((phase) => {
      if (phase === 'aborted' || phase === 'idle') {
        setDraft(null);
      }
    });
    return unsubscribe;
  }, []);

  const clamp = useCallback((value: number): number => {
    return Math.min(jointRange.max, Math.max(jointRange.min, value));
  }, [jointRange.max, jointRange.min]);

  function formattedLiveValue(): string {
    return formatJointValue(displayValue, step);
  }

  // Number-input event wiring. We bind 'input' and 'change' directly
  // on the DOM element rather than going through React's synthetic
  // `onChange`. The reason: React 18's automatic batching can re-order
  // a delegated `setDraft(value)` (from React onChange on the 'input'
  // event) and a directly-bound `setDraft(null)` (from a native
  // 'change' listener) when both fire from a single spinner click.
  // The wrong-order outcome leaves `draft = "1.5"` after the commit
  // and the slider freezes at the commanded value while the robot
  // moves underneath. Binding both events natively gives a
  // deterministic source of truth: 'input' tracks the operator's
  // draft text; 'change' fires once on a committed value (spinner
  // click, Enter, blur) — which clears the draft and pushes the
  // target through the streaming session.
  const numberInputRef = useRef<HTMLInputElement | null>(null);
  useEffect(() => {
    const el = numberInputRef.current;
    if (el === null) return;
    const onNativeInput = (event: Event): void => {
      const inputEvent = event as InputEvent;
      // Browser spinner buttons usually emit a native input event
      // without an inputType. Treat that as an operator jog command.
      // Keyboard text input has inputType values such as insertText,
      // so typing remains local draft state until blur/Enter.
      if (!inputEvent.inputType) {
        const raw = el.value;
        const v = Number(raw);
        if (!Number.isFinite(v)) return;
        setDraft(null);
        onStreamingPushFinal(joint.name, clamp(v));
        return;
      }
      setDraft(el.value);
    };
    const onNativeChange = (): void => {
      const raw = el.value;
      if (raw === '') return;
      const v = Number(raw);
      if (!Number.isFinite(v)) return;
      setDraft(null);
      onStreamingPushFinal(joint.name, clamp(v));
    };
    el.addEventListener('input', onNativeInput);
    el.addEventListener('change', onNativeChange);
    return () => {
      el.removeEventListener('input', onNativeInput);
      el.removeEventListener('change', onNativeChange);
    };
  }, [clamp, joint.name, onStreamingPushFinal]);

  function streamFollow(raw: string): void {
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    // Streaming hook owns the rate throttle (4 Hz) and session
    // lifecycle. No badge churn — robot motion is the feedback.
    onStreamingPush(joint.name, clamp(v));
  }

  function streamFinal(raw: string): void {
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    onStreamingPushFinal(joint.name, clamp(v));
  }

  function commitValue(raw: string): void {
    const requested = Number(raw);
    setDraft(null);
    if (!Number.isFinite(requested)) {
      return;
    }
    onStreamingPushFinal(joint.name, clamp(requested));
  }

  function commitTyped(): void {
    if (draft === null) return;
    commitValue(draft);
  }

  return (
    <div className="border-b border-gray-800 py-1 last:border-b-0">
      <div className="flex items-baseline justify-between gap-1">
        <span className="truncate font-mono text-[11px]">{joint.name}</span>
        <span className="text-[10px] text-gray-500">{joint.type}</span>
      </div>
      <div className="mt-0.5 flex items-center gap-2">
        <input
          type="range"
          min={jointRange.min}
          max={jointRange.max}
          // `step="any"` makes the slider accept any value the operator
          // drags to AND keeps it `:valid` when the live joint state
          // (e.g. -0.722563 rad) is pushed in from outside. Without it,
          // Firefox marks the slider invalid and shows the native
          // "Please select a valid value. The two nearest valid values
          // are…" tooltip — operator UX-breaking even though the value
          // is correct. Keyboard arrows fall back to (max-min)/100
          // which is still useful for jog precision (~0.06 rad on a
          // ±π joint). The streaming-hook + clamp() in the commit
          // path are the authoritative validators.
          step="any"
          value={sliderValue}
          onChange={(e) => {
            setDraft(e.target.value);
            streamFollow(e.target.value);
          }}
          onMouseUp={(e) => {
            // Final push: the released slider value is the goal,
            // bypass the throttle so it always lands before the
            // streaming-session End fires.
            const input = e.target as HTMLInputElement;
            commitValue(input.value);
          }}
          onTouchEnd={(e) => {
            const input = e.target as HTMLInputElement;
            commitValue(input.value);
          }}
          className="flex-1"
        />
        <input
          ref={numberInputRef}
          type="number"
          // Spinner-button increments and keyboard arrows both use this
          // attribute. We deliberately do NOT use `step="any"` here
          // (the slider does, for the Firefox :valid hover tooltip
          // bug — see slider above); for a number input,
          // controlled-value props are accepted verbatim regardless
          // of step, and `step={step}` is what makes the visible
          // spinner buttons honour the operator's selected step
          // instead of the browser default of 1.
          step={step}
          value={draft ?? formattedLiveValue()}
          // Required for React's controlled-input warning, but a noop:
          // the real handlers are the native 'input' and 'change'
          // listeners installed via useEffect above. Going through
          // React onChange would interact badly with the native
          // listener (race on setDraft(null) vs setDraft(value)) and
          // freeze the slider after a spinner click.
          onChange={() => {}}
          onBlur={commitTyped}
          onKeyDown={(e) => {
            if (e.key === 'Enter') {
              e.preventDefault();
              commitTyped();
              (e.target as HTMLInputElement).blur();
            } else if (e.key === 'Escape') {
              e.preventDefault();
              setDraft(null);
              (e.target as HTMLInputElement).blur();
            } else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
              e.preventDefault();
              const direction = e.key === 'ArrowUp' ? 1 : -1;
              const base = Number(draft ?? displayValue);
              const delta = (Number.isFinite(step) && step > 0 ? step : 0.01)
                * (e.shiftKey ? 10 : 1)
                * direction;
              const next = clamp((Number.isFinite(base) ? base : displayValue) + delta);
              const text = String(Number(next.toFixed(6)));
              setDraft(text);
              streamFinal(text);
            }
          }}
          className="w-20 rounded bg-gray-800 py-0.5 pl-1 pr-2 text-right font-mono text-[10px] text-gray-200"
        />
      </div>
      {(joint.limit_lower !== null || joint.limit_upper !== null) && (
        <div className="mt-0.5 flex justify-between font-mono text-[9px] text-gray-600">
          <span>{joint.limit_lower !== null ? joint.limit_lower.toFixed(2) : '-∞'}</span>
          <span>{joint.limit_upper !== null ? joint.limit_upper.toFixed(2) : '+∞'}</span>
        </div>
      )}
    </div>
  );
}

function sliderRange(
  joint: SceneUrdfJoint | null,
  currentValue: number | null,
): { min: number; max: number } {
  if (joint !== null && joint.limit_lower !== null && joint.limit_upper !== null) {
    return { min: joint.limit_lower, max: joint.limit_upper };
  }
  if (joint?.type === 'continuous') {
    return { min: -Math.PI, max: Math.PI };
  }
  const center = currentValue ?? 0;
  return { min: center - 1, max: center + 1 };
}

function formatJointValue(value: number, step: number): string {
  if (!Number.isFinite(value)) return '0';
  if (!Number.isFinite(step) || step <= 0) return value.toFixed(3);
  const decimals = Math.max(
    2,
    Math.min(6, Math.ceil(Math.log10(1 / step))),
  );
  return value.toFixed(decimals);
}
