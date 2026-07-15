import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api } from '../api/client';
import { usePersistentDraft } from '../hooks/usePersistentDraft';
import { gripperCandidateRequest } from '../utils/gripperAuthoring';
import { robotControlJointsForGroup } from '../utils/robotModelAuthority';
import {
  accumulateJointDrafts,
  jointTargetFromDrafts,
} from '../utils/jointAuthoring';
import { useCycleStore, useEditLocked } from '../store/cycleStore';
import type { RobotCommandAuthority } from '../store/commandAuthority';
import { useRobotCommandAuthority } from '../store/commandAuthority';
import { useProgramStore } from '../store/programStore';
import { useRobotControlCandidateStore } from '../store/robotControlCandidateStore';
import { useSceneStateStore } from '../store/sceneStateStore';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import type {
  CartesianFrame,
  MoveToOrientationMode,
  MoveToSource,
} from '../store/viewerSelectionStore';
import type {
  CandidateCreateRequest,
  SceneFrameState,
  SceneGripperState,
  SceneRobotState,
  SceneUrdfJoint,
  SceneUrdfManifest,
  TreeNode,
} from '../types/api';
import { computeMoveToTarget } from '../utils/moveTo';
import type { MoveToTarget, StoredPoseTarget } from '../utils/moveTo';
import {
  formatGripperSemanticToken,
  gripperCompletionNotice,
  gripperPositionSourceLabel,
} from '../utils/gripperSemantics';

// UI settle thresholds — when to clear the operator's authored value
// from the displayed input (CartesianAxisInput draft + CartesianModeBody
// commandedRefs) once the live pose catches up.
const CARTESIAN_POSITION_ARRIVAL_TOLERANCE_M = 1e-4;
const CARTESIAN_ORIENTATION_ARRIVAL_TOLERANCE_RAD = 2e-3;
const RAD_TO_DEG = 180 / Math.PI;
const DEG_TO_RAD = Math.PI / 180;
// Joint-arrival tolerance for clearing a stale draft once the live joint
// state catches up. Keep at or below the fine jog step so a spinner click
// does not clear early at a visibly different value.
const REVOLUTE_JOINT_ARRIVAL_TOLERANCE_RAD = 1e-3;
const PRISMATIC_JOINT_ARRIVAL_TOLERANCE_M = 1e-4;
type RobotCommandGate = RobotCommandAuthority;

function jointArrivalTolerance(joint: SceneUrdfJoint): number {
  return joint.type === 'prismatic'
    ? PRISMATIC_JOINT_ARRIVAL_TOLERANCE_M
    : REVOLUTE_JOINT_ARRIVAL_TOLERANCE_RAD;
}

// Validated Robot Control authoring (Phase 4-P5.2). Default Robot Control does
// NOT open a streaming session and does NOT move the robot on edit/release:
// slider / spinner / number-input / gizmo edits author a server-owned candidate
// (debounced, latest-wins) that the backend plans + validates with no motion.
// The robot moves ONLY via the explicit Approve control (CandidateApprovalBar).
// Candidate statuses for which the accumulated joint drafts must be reset (no
// live-editable draft remains): discarded (handled via candidate===null) or
// handed off / terminal.
const _JOINT_DRAFTS_DONE_STATUSES = new Set([
  'executing',
  'completed',
  'stopped',
  'interrupted',
  'failed',
  'reverted',
  'superseded',
  'expired',
  'error',
]);

function useJointAuthoring(): {
  push: (jointName: string, valueRad: number) => void;
  pushFinal: (jointName: string, valueRad: number) => void;
  setJointArrivalTolerances: (tolerances: Map<string, number>) => void;
} {
  const author = useRobotControlCandidateStore((s) => s.author);
  const candidate = useRobotControlCandidateStore((s) => s.candidate);
  // Accumulate EVERY edited joint so the authored candidate carries all of them
  // (the backend overlays this partial set onto live; authoring only the last
  // joint would drop earlier edits while the UI still showed them amber).
  const draftsRef = useRef<Record<string, number>>({});

  // Reset the accumulation when there is no live-editable draft candidate
  // (Revert / mode switch -> candidate null; or handed off to execution / a
  // terminal status), so the next jog starts from a clean set.
  const draftsActive =
    candidate !== null && !_JOINT_DRAFTS_DONE_STATUSES.has(candidate.status);
  useEffect(() => {
    if (!draftsActive) {
      draftsRef.current = {};
    }
  }, [draftsActive]);

  const authorJoint = useCallback(
    (jointName: string, valueRad: number): void => {
      draftsRef.current = accumulateJointDrafts(
        draftsRef.current,
        jointName,
        valueRad,
      );
      const target = jointTargetFromDrafts(draftsRef.current);
      author({
        sourceSurface: 'joint_slider',
        inputMode: 'joint',
        jointTarget: target,
      });
    },
    [author],
  );
  const noop = useCallback((): void => {}, []);
  return {
    push: authorJoint,
    pushFinal: authorJoint,
    setJointArrivalTolerances: noop,
  };
}

function useCartesianAuthoring(options: { tcpFrame: string | null }): {
  pushFinal: (
    positionM: [number, number, number],
    orientationXyzw: [number, number, number, number],
  ) => void;
} {
  const author = useRobotControlCandidateStore((s) => s.author);
  const tcpFrame = options.tcpFrame;
  const pushFinal = useCallback(
    (
      positionM: [number, number, number],
      orientationXyzw: [number, number, number, number],
    ): void => {
      author({
        sourceSurface: 'cartesian_gizmo',
        inputMode: 'cartesian',
        cartesianTarget: {
          positionM: [...positionM],
          orientationXyzw: [...orientationXyzw],
          frameId: 'base',
          tcpFrame: tcpFrame ?? undefined,
        },
      });
    },
    [author, tcpFrame],
  );
  return { pushFinal };
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

const ANGULAR_STEP_DEG_OPTIONS: readonly number[] = [0.1, 0.5, 1, 5, 10, 30];
const ANGULAR_STEP_RAD_OPTIONS: readonly number[] =
  ANGULAR_STEP_DEG_OPTIONS.map((deg) => deg * DEG_TO_RAD);
const PRISMATIC_STEPS: readonly number[] = [0.001, 0.01, 0.1];
const DEFAULT_ANGULAR_STEP_RAD = 5 * DEG_TO_RAD;

export function RobotControlPanel({ onClose }: RobotControlPanelProps) {
  const [manifest, setManifest] = useState<SceneUrdfManifest | null>(null);
  const [manifestError, setManifestError] = useState<string | null>(null);
  // Step size = the increment applied by the numeric-input arrow
  // buttons (and as the slider's `step` for keyboard precision). NOT
  // a clamp — sliders move freely limit-to-limit and commits send
  // the requested value verbatim. The dropdown picks coarse vs fine
  // arrow stepping per axis kind.
  const [jointRadStep, setJointRadStep] = useState<number>(DEFAULT_ANGULAR_STEP_RAD);
  const [jointMStep, setJointMStep] = useState<number>(0.01);
  const [cartesianMStep, setCartesianMStep] = useState<number>(0.01);
  const [cartesianRadStep, setCartesianRadStep] = useState<number>(DEFAULT_ANGULAR_STEP_RAD);
  const robot = useSceneStateStore((s) => s.scene?.robot ?? null);
  const robotModelKey = robot?.urdf_model_key ?? robot?.robot_id ?? null;
  const robotMode = useViewerSelectionStore((s) => s.robotMode);
  const setRobotMode = useViewerSelectionStore((s) => s.setRobotMode);
  const cartesianFrame = useViewerSelectionStore((s) => s.cartesianFrame);
  const setCartesianFrame = useViewerSelectionStore((s) => s.setCartesianFrame);
  const commandGate = useRobotCommandAuthority();
  const candidateRevert = useRobotControlCandidateStore((s) => s.revert);
  const candidateClear = useRobotControlCandidateStore((s) => s.clear);

  // Esc reverts the active candidate (no motion). Clearing on unmount discards
  // any draft so a stale server candidate cannot linger after the panel closes.
  useEffect(() => {
    const onKey = (e: KeyboardEvent): void => {
      if (e.key === 'Escape') {
        void candidateRevert();
      }
    };
    window.addEventListener('keydown', onKey);
    return () => {
      window.removeEventListener('keydown', onKey);
      candidateClear();
    };
  }, [candidateRevert, candidateClear]);

  // Switching Robot Control modes discards the prior mode's pending draft
  // exactly like Revert (no motion; an `executing` candidate is preserved so
  // Stop — not Revert — cancels it). This keeps the command source shown in the
  // UI aligned with the active mode and prevents a hidden candidate from a
  // prior tab carrying across. revert() also clears the input drafts and the
  // ghost/gizmo overlay back to live for the newly selected mode.
  const prevModeRef = useRef(robotMode);
  useEffect(() => {
    if (prevModeRef.current !== robotMode) {
      prevModeRef.current = robotMode;
      void candidateRevert();
    }
  }, [robotMode, candidateRevert]);

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
    return robotControlJointsForGroup(manifest, robot?.group_id ?? null);
  }, [manifest, robot?.group_id]);

  return (
    <div className="pointer-events-auto absolute right-2 top-2 z-20 flex max-h-[calc(100%-5rem)] w-72 flex-col overflow-hidden rounded-md border border-gray-700 bg-gray-900/50 text-xs text-gray-200 shadow-xl">
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
          jointMStep={jointMStep}
          onJointMStepChange={setJointMStep}
          commandGate={commandGate}
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
          commandGate={commandGate}
        />
      )}
      {robotMode === 'move-to' && <MoveToBody />}
      <CandidateApprovalBar commandGate={commandGate} />
    </div>
  );
}

/**
 * Validated Robot Control approval bar. Reads the server-owned candidate and
 * is the ONLY surface in this panel that can move the robot (via Approve).
 * Approve is enabled only for a fresh `ready` candidate; Revert discards the
 * draft and clears overlays without motion.
 */
function CandidateApprovalBar({ commandGate }: { commandGate: RobotCommandGate }) {
  const candidate = useRobotControlCandidateStore((s) => s.candidate);
  const approving = useRobotControlCandidateStore((s) => s.approving);
  const busy = useRobotControlCandidateStore((s) => s.busy);
  const rejection = useRobotControlCandidateStore((s) => s.rejection);
  const error = useRobotControlCandidateStore((s) => s.error);
  const approve = useRobotControlCandidateStore((s) => s.approve);
  const revert = useRobotControlCandidateStore((s) => s.revert);

  if (candidate === null && rejection === null && error === null) {
    return null;
  }

  const status = candidate?.status ?? null;
  // Approve only a settled, current candidate: any new edit sets busy:true and
  // swaps in a `planning` placeholder until its create() resolves, so a stale
  // `ready` candidate can never be approved during the debounce/planning
  // window.
  const ready = status === 'ready' && candidate?.commitEligible === true && !busy;
  const canApprove = ready && !approving && commandGate.canCommand;
  // Once dispatched, the candidate is `executing`: Revert must not act on it
  // (execution cancellation belongs to Stop, not Revert).
  const executing = status === 'executing';
  const terminal =
    status === 'completed' ||
    status === 'stopped' ||
    status === 'interrupted' ||
    status === 'failed';
  const diagnostics = candidate?.diagnostics ?? null;
  const execution = candidate?.execution ?? null;

  const statusColor =
    status === 'ready' || status === 'completed'
      ? 'text-emerald-300'
      : status === 'blocked' ||
          status === 'error' ||
          status === 'failed' ||
          status === 'interrupted'
        ? 'text-red-300'
        : status === 'stale' ||
            status === 'expired' ||
            status === 'superseded' ||
            status === 'stopped'
          ? 'text-amber-300'
          : 'text-gray-300';

  return (
    <div className="border-t border-gray-700 bg-gray-800/60 px-2 py-1.5">
      <div className="mb-1 flex items-center justify-between text-[10px]">
        <span className="uppercase tracking-wide text-gray-400">Candidate</span>
        {status !== null && (
          <span className={`font-mono ${statusColor}`}>{status}</span>
        )}
      </div>
      {diagnostics !== null && status !== 'ready' && (
        <div className="mb-1 rounded bg-gray-900/60 px-1.5 py-1 text-[10px] text-gray-300">
          <span className="font-mono text-gray-400">{diagnostics.reasonCode}</span>
          {diagnostics.summary ? ` — ${diagnostics.summary}` : ''}
        </div>
      )}
      {execution !== null && (executing || terminal) && (
        <div className="mb-1 flex items-center justify-between gap-2 text-[10px] text-gray-400">
          <span className="truncate font-mono">
            {execution.moveId ?? 'motion'}
          </span>
          <span className="shrink-0 font-mono">
            {execution.executionId > 0 ? `#${execution.executionId}` : execution.sessionStatus ?? ''}
          </span>
        </div>
      )}
      {rejection !== null && (
        <div className="mb-1 rounded border border-red-700/60 bg-red-950/30 px-1.5 py-1 text-[10px] text-red-300">
          Not executed ({rejection.reasonCode}): {rejection.detail}
        </div>
      )}
      {error !== null && (
        <div className="mb-1 rounded border border-red-700/60 bg-red-950/30 px-1.5 py-1 text-[10px] text-red-300">
          {error}
        </div>
      )}
      {!commandGate.canCommand && ready && (
        <div className="mb-1 text-[10px] text-amber-300">
          Approve disabled: {commandGate.reason}
        </div>
      )}
      <div className="flex gap-1">
        <button
          type="button"
          onClick={() => void approve()}
          disabled={!canApprove}
          className="flex-1 rounded bg-emerald-700 px-2 py-1 text-[11px] text-white hover:bg-emerald-600 disabled:cursor-not-allowed disabled:opacity-50"
          title={
            !ready
              ? 'Approve is enabled only for a fresh, validated candidate'
              : undefined
          }
        >
          {approving
            ? 'Approving…'
            : executing
              ? 'Executing…'
              : status === 'completed'
                ? 'Completed'
                : 'Approve & Execute'}
        </button>
        <button
          type="button"
          onClick={() => void revert()}
          disabled={candidate === null || executing}
          className="rounded bg-gray-700 px-2 py-1 text-[11px] text-gray-200 hover:bg-gray-600 disabled:cursor-not-allowed disabled:opacity-50"
          title={
            executing
              ? 'Executing — use Stop to halt motion; Revert only discards a draft'
              : terminal
                ? 'Clear this execution result'
                : 'Discard the draft and clear the preview (no motion)'
          }
        >
          {terminal ? 'Clear' : 'Revert'}
        </button>
      </div>
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
  jointMStep,
  onJointMStepChange,
  commandGate,
}: {
  manifest: SceneUrdfManifest | null;
  manifestError: string | null;
  drivableJoints: SceneUrdfJoint[];
  robot: SceneRobotState | null;
  jointRadStep: number;
  onJointRadStepChange: (next: number) => void;
  jointMStep: number;
  onJointMStepChange: (next: number) => void;
  commandGate: RobotCommandGate;
}) {
  const hasPrismaticJoints = useMemo(
    () => drivableJoints.some((joint) => joint.type === 'prismatic'),
    [drivableJoints],
  );
  const {
    push: jointDraftPush,
    pushFinal: jointDraftPushFinal,
    setJointArrivalTolerances,
  } = useJointAuthoring();
  const jointArrivalTolerances = useMemo(() => {
    const out = new Map<string, number>();
    for (const joint of drivableJoints) {
      out.set(joint.name, jointArrivalTolerance(joint));
    }
    return out;
  }, [drivableJoints]);
  useEffect(() => {
    setJointArrivalTolerances(jointArrivalTolerances);
  }, [jointArrivalTolerances, setJointArrivalTolerances]);

  return (
    <>
      <div className="flex items-center gap-2 border-b border-gray-800 bg-gray-900/50 px-2 py-1.5">
        <StepCapSelect
          label="step (deg)"
          options={ANGULAR_STEP_RAD_OPTIONS}
          value={jointRadStep}
          onChange={onJointRadStepChange}
          formatOption={formatRadStepAsDeg}
          disabled={!commandGate.canCommand}
        />
        {hasPrismaticJoints && (
          <StepCapSelect
            label="step (m)"
            options={PRISMATIC_STEPS}
            value={jointMStep}
            onChange={onJointMStepChange}
            disabled={!commandGate.canCommand}
          />
        )}
      </div>
      <div className="flex-1 overflow-y-auto p-2">
        {!commandGate.canCommand && (
          <div className="mb-2 rounded border border-amber-700/60 bg-amber-950/30 px-2 py-1 text-[10px] text-amber-300">
            Direct joint control disabled: {commandGate.reason}
          </div>
        )}
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
            step={joint.type === 'prismatic' ? jointMStep : jointRadStep}
            onDraftPush={jointDraftPush}
            onDraftPushFinal={jointDraftPushFinal}
            disabled={!commandGate.canCommand}
            disabledReason={commandGate.reason}
          />
        ))}
        <GrippersSection commandGate={commandGate} />
        <PoseEditSnapshotSection mode="joint" commandGate={commandGate} />
      </div>
      <div className="border-t border-gray-700 bg-gray-800/50 px-2 py-1 text-[10px] text-gray-500">
        Jog · sliders author a draft target only · release does not move the robot · Approve is the only motion path
      </div>
    </>
  );
}

/**
 * Move-To tab body. Operator picks an entity (scene object, named
 * frame, or stored move-node target), an orientation mode, and an
 * optional 6-DoF offset; the panel computes the resulting target and
 * automatically authors a validated Robot Control candidate. NO MOTION:
 * execution still requires Approve on the candidate bar.
 *
 * The actual math lives in `utils/moveTo.ts` so the ghost preview
 * computes the same target as the candidate — no drift between what
 * the operator sees and what Approve can execute.
 */
function MoveToBody() {
  const moveTo = useViewerSelectionStore((s) => s.moveTo);
  const patchMoveTo = useViewerSelectionStore((s) => s.patchMoveTo);
  const objects = useSceneStateStore((s) => s.scene?.objects ?? EMPTY_OBJECTS);
  const tcpPose = useSceneStateStore((s) => s.scene?.robot?.tcp_pose_in_base ?? null);
  const appliedTree = useProgramStore((s) => s.appliedTree);
  const author = useRobotControlCandidateStore((s) => s.author);
  const revertCandidate = useRobotControlCandidateStore((s) => s.revert);
  const activeCandidate = useRobotControlCandidateStore((s) => s.candidate);
  const lastAuthoredMoveToKeyRef = useRef<string | null>(null);

  const [frames, setFrames] = useState<SceneFrameState[]>([]);
  const maxOffsetXyzM = 100;
  const maxOffsetRpyDeg = 720;

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
    for (const m of collectMoveNodes(appliedTree, 'pose')) {
      out.push({ nodeName: m.name, motionType: 'pose', params: m.params });
    }
    for (const m of collectMoveNodes(appliedTree, 'joint')) {
      out.push({ nodeName: m.name, motionType: 'joint', params: m.params });
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

  const targetError = useMemo(() => {
    if (
      moveTo.offsetXyzM.some((v) => Math.abs(v) > maxOffsetXyzM)
      || moveTo.offsetRpyDeg.some((v) => Math.abs(v) > maxOffsetRpyDeg)
    ) {
      return 'offset exceeds workspace guard';
    }
    if (target === null) {
      return null;
    }
    return null;
  }, [moveTo.offsetRpyDeg, moveTo.offsetXyzM, target]);

  const candidateRequest = useMemo<CandidateCreateRequest | null>(() => {
    if (target === null || targetError !== null) {
      return null;
    }
    if (target.kind === 'joint') {
      return {
        sourceSurface: 'move_to',
        inputMode: 'joint',
        jointTarget: {
          jointNames: target.jointNames,
          positionsRad: target.jointPositionsRad,
        },
      };
    }
    return {
      sourceSurface: 'move_to',
      inputMode: 'cartesian',
      cartesianTarget: {
        positionM: target.position,
        orientationXyzw: target.quaternion,
        frameId: 'base',
      },
    };
  }, [target, targetError]);

  const candidateRequestKey = useMemo(
    () => (candidateRequest === null ? null : JSON.stringify(candidateRequest)),
    [candidateRequest],
  );

  useEffect(() => {
    if (candidateRequest !== null && candidateRequestKey !== null) {
      if (candidateRequestKey !== lastAuthoredMoveToKeyRef.current) {
        lastAuthoredMoveToKeyRef.current = candidateRequestKey;
        author(candidateRequest);
      }
      return;
    }

    if (
      lastAuthoredMoveToKeyRef.current !== null &&
      activeCandidate?.sourceSurface === 'move_to' &&
      activeCandidate.status !== 'executing' &&
      activeCandidate.status !== 'completed' &&
      activeCandidate.status !== 'stopped' &&
      activeCandidate.status !== 'interrupted' &&
      activeCandidate.status !== 'failed'
    ) {
      lastAuthoredMoveToKeyRef.current = null;
      void revertCandidate();
      return;
    }

    if (target === null || targetError !== null) {
      lastAuthoredMoveToKeyRef.current = null;
    }
  }, [
    activeCandidate?.sourceSurface,
    activeCandidate?.status,
    author,
    candidateRequest,
    candidateRequestKey,
    revertCandidate,
    target,
    targetError,
  ]);

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
            maxAbs={maxOffsetXyzM}
            onChange={(next) => patchMoveTo({ offsetXyzM: next })}
          />
          <OffsetRow
            label="rpy (deg)"
            axes={['r', 'p', 'y']}
            values={moveTo.offsetRpyDeg}
            step={1}
            maxAbs={maxOffsetRpyDeg}
            onChange={(next) => patchMoveTo({ offsetRpyDeg: next })}
          />
        </div>

        <div className="mt-3 border-t border-gray-800 pt-2">
          <MoveToStatusLine
            target={target}
            errorDetail={targetError}
          />
        </div>
      </div>

      <div className="border-t border-gray-700 bg-gray-800/50 px-2 py-1 text-[10px] text-gray-500">
        Move To · target changes validate automatically · approve below to execute
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
  maxAbs,
  onChange,
}: {
  label: string;
  axes: readonly [string, string, string];
  values: [number, number, number];
  step: number;
  maxAbs: number;
  onChange: (next: [number, number, number]) => void;
}) {
  function setAxis(idx: 0 | 1 | 2, raw: string): void {
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    const next: [number, number, number] = [values[0], values[1], values[2]];
    next[idx] = Math.min(maxAbs, Math.max(-maxAbs, v));
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
  errorDetail,
  target,
}: {
  errorDetail: string | null;
  target: MoveToTarget | null;
}) {
  if (errorDetail !== null) {
    return (
      <div className="mt-1 text-[10px] text-red-300" title={errorDetail}>
        {errorDetail.slice(0, 60)}
      </div>
    );
  }
  if (target !== null) {
    return (
      <div className="mt-1 space-y-0.5 text-[10px] text-gray-500" title="Target is validated automatically">
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
  return (
    <div className="mt-1 text-[10px] text-gray-500">Pick a source + entity to set target.</div>
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
  commandGate,
}: {
  cartesianFrame: CartesianFrame;
  onFrameChange: (frame: CartesianFrame) => void;
  cartesianMStep: number;
  onMStepChange: (next: number) => void;
  cartesianRadStep: number;
  onRadStepChange: (next: number) => void;
  commandGate: RobotCommandGate;
}) {
  const robot = useSceneStateStore((s) => s.scene?.robot ?? null);
  const tcp = robot?.tcp_pose_in_base ?? null;
  const cartesianAuthoring = useCartesianAuthoring({
    tcpFrame: robot?.tcp_frame ?? null,
  });
  const gizmoMode = useViewerSelectionStore((s) => s.cartesianGizmoMode);
  const setGizmoMode = useViewerSelectionStore((s) => s.setCartesianGizmoMode);

  const positionM = useMemo<[number, number, number]>(
    () => (tcp === null ? [0, 0, 0] : readPosition(tcp) as [number, number, number]),
    [tcp],
  );
  const rpyRad = useMemo<[number, number, number]>(
    () => (tcp === null ? [0, 0, 0] : quatToRpyRad(readQuat(tcp))),
    [tcp],
  );
  // The active Cartesian draft target (base frame), or null when there is no
  // live draft. Both the numeric fields AND the gizmo author into
  // candidate.cartesianTarget, so it is the single source of truth: the fields
  // DISPLAY it (a gizmo drag flows into the fields, and diverging axes go amber)
  // and ACCUMULATE onto it (a field edit builds on the gizmo's pose, never a
  // stale local ref). A handed-off / discarded candidate is ignored so the
  // fields fall back to the live TCP.
  const candidate = useRobotControlCandidateStore((s) => s.candidate);
  const cartesianTarget = useMemo(() => {
    if (candidate === null) return null;
    const t = candidate.cartesianTarget;
    if (t == null) return null;
    const st = candidate.status;
    if (
      st === 'executing' ||
      st === 'completed' ||
      st === 'stopped' ||
      st === 'interrupted' ||
      st === 'failed' ||
      st === 'reverted' ||
      st === 'error'
    ) return null;
    if (t.frameId && t.frameId !== 'base') return null;
    return t;
  }, [candidate]);
  const targetPosition = useMemo<[number, number, number] | null>(() => {
    if (cartesianTarget === null) return null;
    const p = cartesianTarget.positionM;
    const out: [number, number, number] = [Number(p[0]), Number(p[1]), Number(p[2])];
    return out.every(Number.isFinite) ? out : null;
  }, [cartesianTarget]);
  const targetRpy = useMemo<[number, number, number] | null>(() => {
    if (cartesianTarget === null) return null;
    return quatToRpyRad(cartesianTarget.orientationXyzw as number[]);
  }, [cartesianTarget]);

  // Displayed pose: the authored target when a draft is active, else live TCP.
  const displayedPosition = targetPosition ?? positionM;
  const displayedRpy = targetRpy ?? rpyRad;

  function commitXyz(next: [number, number, number]): void {
    if (!commandGate.canCommand) return;
    if (tcp === null) return;
    // New position, current authored orientation (no rpy round-trip).
    const quat = (cartesianTarget?.orientationXyzw ?? readQuat(tcp)) as [
      number,
      number,
      number,
      number,
    ];
    cartesianAuthoring.pushFinal(next, quat);
  }

  function commitRpy(nextRpy: [number, number, number]): void {
    if (!commandGate.canCommand) return;
    if (tcp === null) return;
    // New orientation, current authored position.
    cartesianAuthoring.pushFinal(displayedPosition, rpyRadToQuat(nextRpy));
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
          disabled={!commandGate.canCommand}
        />
        <StepCapSelect
          label="step (deg)"
          options={ANGULAR_STEP_RAD_OPTIONS}
          value={cartesianRadStep}
          onChange={onRadStepChange}
          formatOption={formatRadStepAsDeg}
          disabled={!commandGate.canCommand}
        />
      </div>
      <div className="flex-1 overflow-y-auto p-2 text-[11px]">
        {!commandGate.canCommand && (
          <div className="mb-2 rounded border border-amber-700/60 bg-amber-950/30 px-2 py-1 text-[10px] text-amber-300">
            Direct Cartesian control disabled: {commandGate.reason}
          </div>
        )}
        {tcp === null ? (
          <div className="italic text-gray-500">
            TCP pose unavailable. Load a deployment with a configured tcp_frame.
          </div>
        ) : (
          <>
            <CartesianAxisFields
              label="position (m, in base frame)"
              labels={['x', 'y', 'z']}
              values={displayedPosition}
              liveValues={positionM}
              step={cartesianMStep}
              settleTolerance={CARTESIAN_POSITION_ARRIVAL_TOLERANCE_M}
              onCommit={commitXyz}
              disabled={!commandGate.canCommand}
            />
            <CartesianAxisFields
              label="orientation (rpy, deg)"
              labels={['r', 'p', 'y']}
              values={rpyRadToDeg(displayedRpy)}
              liveValues={rpyRadToDeg(rpyRad)}
              step={cartesianRadStep * RAD_TO_DEG}
              settleTolerance={CARTESIAN_ORIENTATION_ARRIVAL_TOLERANCE_RAD * RAD_TO_DEG}
              onCommit={(nextRpyDeg) => commitRpy(rpyDegToRad(nextRpyDeg))}
              wrap
              disabled={!commandGate.canCommand}
            />
          </>
        )}
        <PoseEditSnapshotSection mode="cartesian" commandGate={commandGate} />
      </div>
      <div className="border-t border-gray-700 bg-gray-800/50 px-2 py-1 text-[10px] text-gray-500">
        Cartesian · gizmo + fields author a draft target only · fields show the authored target (amber when ≠ live)
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
  liveValues,
  step,
  settleTolerance,
  onCommit,
  wrap = false,
  disabled = false,
}: {
  label: string;
  labels: readonly [string, string, string];
  // Displayed authored target (or live TCP when there is no draft).
  values: [number, number, number];
  // Actual live TCP, for the authored-vs-live amber signal.
  liveValues: [number, number, number];
  step: number;
  settleTolerance: number;
  onCommit: (next: [number, number, number]) => void;
  wrap?: boolean;
  disabled?: boolean;
}) {
  function commitAxis(idx: 0 | 1 | 2, v: number): void {
    if (disabled) return;
    if (!Number.isFinite(v)) return;
    // Build the new full pose on the CURRENT authored target (`values`), so a
    // field edit accumulates onto the gizmo's pose rather than reverting it.
    const next: [number, number, number] = [values[0], values[1], values[2]];
    next[idx] = v;
    onCommit(next);
  }
  return (
    <div className={wrap ? 'mt-2' : ''}>
      <div className="text-[10px] uppercase tracking-wide text-gray-500">{label}</div>
      <div className="mt-1 grid grid-cols-3 gap-1.5">
        {labels.map((axis, idx) => {
          const i = idx as 0 | 1 | 2;
          return (
            <CartesianAxisInput
              key={axis}
              axis={axis}
              value={values[i] ?? 0}
              liveValue={liveValues[i] ?? 0}
              step={step}
              settleTolerance={settleTolerance}
              disabled={disabled}
              onCommit={(v) => commitAxis(i, v)}
            />
          );
        })}
      </div>
    </div>
  );
}

// Per-axis number input for the Cartesian tab. Mirrors JointRow's
// native-event-listener pattern (see lines ~1819-1880): we bind
// 'input' and 'change' directly on the DOM element rather than
// going through React's synthetic onChange. Reason: React's
// synthetic event does not reliably preserve `InputEvent.inputType`
// across browsers, so the "no inputType => spinner click" detection
// silently misses spinner clicks when wired through onChange. The
// native listener pattern matches what JointRow already does.
function CartesianAxisInput({
  axis,
  value,
  liveValue,
  step,
  settleTolerance,
  disabled,
  onCommit,
}: {
  axis: string;
  // Displayed base: the authored candidate target for this axis (so a gizmo
  // drag updates the field), or live TCP when there is no draft.
  value: number;
  // Actual live TCP axis value — colours the field amber when the shown value
  // diverges from it.
  liveValue: number;
  step: number;
  settleTolerance: number;
  disabled: boolean;
  onCommit: (next: number) => void;
}) {
  // `draft` is only the in-progress typing buffer. A commit authors the
  // candidate (which then drives `value`), so the authored target stays visible
  // without a persistent local draft — and a gizmo drag, which also updates the
  // candidate, flows straight into `value`.
  const [draft, setDraft] = useState<string | null>(null);
  const inputRef = useRef<HTMLInputElement | null>(null);
  const disabledRef = useRef(disabled);
  const onCommitRef = useRef(onCommit);
  const sawBeforeInputRef = useRef(false);
  const skipNextBlurCommitRef = useRef(false);
  useEffect(() => {
    disabledRef.current = disabled;
    onCommitRef.current = onCommit;
  }, [disabled, onCommit]);
  // Clear a leftover typing/arrow draft once the live value catches up (e.g.
  // after an Approve executes), so the field resumes following live state.
  useEffect(() => {
    if (draft === null) return;
    const target = Number(draft);
    if (!Number.isFinite(target)) return;
    if (Math.abs(target - liveValue) <= settleTolerance) setDraft(null);
  }, [draft, liveValue, settleTolerance]);
  useEffect(() => {
    const el = inputRef.current;
    if (el === null) return;
    const commitRaw = (raw: string): void => {
      if (disabledRef.current || raw === '') return;
      const v = Number(raw);
      if (!Number.isFinite(v)) return;
      // Author the value, then clear the typing buffer: the candidate target now
      // drives `value`, so the authored value stays visible (no snap-back) and a
      // later gizmo drag can update this field.
      setDraft(null);
      onCommitRef.current(v);
    };
    const onNativeBeforeInput = (): void => {
      sawBeforeInputRef.current = true;
    };
    const onNativeInput = (event: Event): void => {
      if (disabledRef.current) return;
      const inputEvent = event as InputEvent;
      // Text edits fire beforeinput/input with inputType. Native
      // number-spinner clicks usually fire input without beforeinput
      // (and often with no inputType). Treat that as a jog command.
      const typedEdit = sawBeforeInputRef.current || Boolean(inputEvent.inputType);
      sawBeforeInputRef.current = false;
      if (!typedEdit) {
        commitRaw(el.value);
        return;
      }
      setDraft(el.value);
    };
    const onNativeChange = (): void => {
      commitRaw(el.value);
    };
    el.addEventListener('beforeinput', onNativeBeforeInput);
    el.addEventListener('input', onNativeInput);
    el.addEventListener('change', onNativeChange);
    return () => {
      el.removeEventListener('beforeinput', onNativeBeforeInput);
      el.removeEventListener('input', onNativeInput);
      el.removeEventListener('change', onNativeChange);
    };
  }, []);
  // The currently-shown value (draft while typing, else the authored target /
  // live). Amber when it diverges from the actual live TCP — exactly like a
  // direct edit, whether the divergence came from a field edit or a gizmo drag.
  const shownNumber = draft !== null ? Number(draft) : value;
  const divergesFromLive =
    Number.isFinite(shownNumber) && Math.abs(shownNumber - liveValue) > settleTolerance;
  return (
    <div className="flex min-w-0 flex-col gap-0.5 font-mono text-[10px] text-gray-300">
      <span className="text-[9px] uppercase text-gray-500">{axis}</span>
      <input
        ref={inputRef}
        type="number"
        step={step}
        value={draft ?? value.toFixed(3)}
        disabled={disabled}
        // Noop: real handlers are the native 'input' and 'change'
        // listeners installed via useEffect above. See JointRow.
        onChange={() => {}}
        onBlur={() => {
          if (skipNextBlurCommitRef.current) {
            skipNextBlurCommitRef.current = false;
            return;
          }
          if (draft === null) return;
          const v = Number(draft);
          if (!disabled && Number.isFinite(v)) {
            setDraft(null);
            onCommit(v);
          }
        }}
        onKeyDown={(e) => {
          if (e.key === 'Enter') {
            e.preventDefault();
            if (draft !== null) {
              const v = Number(draft);
              if (!disabled && Number.isFinite(v)) {
                setDraft(null);
                onCommit(v);
              }
            }
            (e.target as HTMLInputElement).blur();
          } else if (e.key === 'Escape') {
            // Explicit cancel of the in-progress typing: drop the draft and blur
            // (the field falls back to the authored target / live).
            e.preventDefault();
            skipNextBlurCommitRef.current = true;
            setDraft(null);
            (e.target as HTMLInputElement).blur();
          } else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
            e.preventDefault();
            if (disabled) return;
            const direction = e.key === 'ArrowUp' ? 1 : -1;
            const base = Number(draft ?? value);
            const delta = (Number.isFinite(step) && step > 0 ? step : 0.001)
              * (e.shiftKey ? 10 : 1)
              * direction;
            const next = (Number.isFinite(base) ? base : value) + delta;
            if (!Number.isFinite(next)) return;
            const text = String(Number(next.toFixed(6)));
            setDraft(text);
            onCommit(next);
          }
        }}
        className={`w-full min-w-0 rounded py-0.5 pl-1 pr-2 text-right ${
          divergesFromLive
            ? 'bg-amber-950/40 text-amber-200 ring-1 ring-amber-600/50'
            : 'bg-gray-800 text-gray-200'
        }`}
      />
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
 * motion_type=joint.
 *
 * For `mode='cartesian'`: snapshots `robot.tcp_pose_in_base` into
 * `pose_goal: {position_m, orientation_xyzw}`. Filters to nodes
 * with motion_type=pose.
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
function GrippersSection({ commandGate }: { commandGate: RobotCommandGate }) {
  const grippers = useSceneStateStore((s) => s.scene?.grippers ?? EMPTY_GRIPPERS);
  if (grippers.length === 0) return null;
  return (
    <div className="mt-3 space-y-2 border-t border-gray-800 pt-3">
      <div className="px-1 text-[10px] font-semibold uppercase tracking-wide text-gray-400">
        Grippers
      </div>
      {grippers.map((g) => (
        <GripperRow key={g.gripper_id} gripper={g} commandGate={commandGate} />
      ))}
    </div>
  );
}

const EMPTY_GRIPPERS: SceneGripperState[] = [];

// Candidate statuses for which a gripper widget shows its staged (authored,
// not-yet-executed) target. Terminal/handed-off statuses fall back to live.
const GRIPPER_STAGED_STATUSES = new Set<string>([
  'planning',
  'ready',
  'blocked',
  'stale',
  'approving',
]);

function GripperRow({
  gripper,
  commandGate,
}: {
  gripper: SceneGripperState;
  commandGate: RobotCommandGate;
}) {
  // Slider mirrors the JointRow pattern: it tracks the live joint
  // value reported by the kernel and only diverges while the operator
  // is actively dragging (`pending` non-null until mouseup). The
  // command target is displayed separately in the cmd label so the
  // operator sees both "where the finger actually is" and "where we
  // asked it to go".
  const author = useRobotControlCandidateStore((s) => s.author);
  const candidate = useRobotControlCandidateStore((s) => s.candidate);
  const [pending, setPending] = useState<number | null>(null);
  // Anything that releases the BT runner (Run or a direct-control session
  // opened from a sibling panel) clears the operator's stale drag value.
  const cycleState = useCycleStore((s) => s.state);
  useEffect(() => {
    if (
      cycleState === 'running'
      || cycleState === 'stopping'
      || cycleState === 'direct_control'
    ) {
      setPending(null);
    }
  }, [cycleState]);
  useEffect(() => {
    if (candidate?.inputMode !== 'gripper') {
      setPending(null);
    }
  }, [candidate?.inputMode]);
  const lower = gripper.limit_lower ?? 0;
  const upper = gripper.limit_upper ?? 1;
  const current = gripper.current_position ?? lower;
  const liveTarget = gripper.target_position ?? current;
  const positionSourceLabel = gripperPositionSourceLabel({
    stateAuthority: gripper.state_authority,
    observedPosition: gripper.observed_position,
    commandedPosition: gripper.commanded_position,
  });
  // The staged candidate target for THIS gripper, when it is the active draft.
  // A slider/preset edit authors a gripper candidate; the robot moves only via
  // the shared Approve action, so the widget shows the staged (not-yet-executed)
  // target until Approve/Revert.
  const stagedTarget =
    candidate !== null &&
    candidate.inputMode === 'gripper' &&
    candidate.gripperTarget != null &&
    candidate.gripperTarget.gripperId === gripper.gripper_id &&
    GRIPPER_STAGED_STATUSES.has(candidate.status)
      ? candidate.gripperTarget.targetPosition
      : null;
  // Slider follows the live drag, then the staged candidate target, then live.
  const displayValue = Math.min(
    upper,
    Math.max(lower, pending ?? stagedTarget ?? current),
  );
  const cmdTarget = stagedTarget ?? liveTarget;
  const stagedDiverges =
    stagedTarget !== null && Math.abs(stagedTarget - current) > 1e-4;
  const presetEntries = useMemo(
    () => Object.entries(gripper.srdf_presets ?? {}),
    [gripper.srdf_presets],
  );
  const derivedJoints = gripper.derived_joints ?? [];
  const evidence = gripper.evidence ?? [];
  const completionNotice = gripperCompletionNotice({
    actuationKind: gripper.actuation_kind,
    completionPolicy: gripper.completion_policy,
    graspConfirmationPolicy: gripper.grasp_confirmation_policy,
  });

  // Author (NOT dispatch) a staged gripper candidate. Slider release and preset
  // clicks no longer call /api/scene/gripper/command directly; the robot moves
  // only via the shared Approve action (CandidateApprovalBar), and Revert
  // discards the staged target. Backend validation marks an out-of-limit target
  // as a blocked candidate.
  function authorGripper(
    value: number,
    surface: 'gripper_slider' | 'gripper_preset',
  ): void {
    setPending(null);
    if (!commandGate.canCommand) {
      console.warn('gripper authoring rejected:', commandGate.reason);
      return;
    }
    if (!Number.isFinite(value)) {
      console.warn('gripper authoring rejected: non-finite target', value);
      return;
    }
    author(gripperCandidateRequest(gripper.gripper_id, value, surface));
  }

  const stateLabel = gripper.state.replace(/^GripperCommandState\./, '');
  const projectionState = gripper.projection_state ?? 'not_required';
  // The kernel reports an in-flight gripper command as `executing` (the
  // GripperCommandState enum value); also accept `in_progress` defensively.
  // While busy the widget disables authoring — the backend additionally
  // fails closed if a command is approved against a busy gripper.
  const busy = ['executing', 'in_progress'].includes(stateLabel.toLowerCase());
  const projectionFailed = projectionState === 'failed';
  const failedRequiresReset =
    stateLabel.toLowerCase() === 'failed' &&
    gripper.reason_code !== 'grasp_confirmation_lost';
  const disabled =
    busy || projectionFailed || failedRequiresReset || !commandGate.canCommand;
  return (
    <div className="space-y-1 rounded border border-gray-700 bg-gray-900/40 p-2">
      <div className="flex items-baseline justify-between gap-2">
        <span className="font-mono text-[11px] text-gray-100">
          {gripper.gripper_id}
        </span>
        <span className="text-[10px] text-gray-500">{gripper.joint_name}</span>
      </div>
      <div className="grid grid-cols-2 gap-1 text-[10px] text-gray-400">
        <span title={gripper.actuation_kind ?? ''}>
          {formatGripperSemanticToken(gripper.actuation_kind)}
        </span>
        <span title={gripper.transport_kind ?? ''}>
          {formatGripperSemanticToken(gripper.transport_kind)}
        </span>
        <span title={gripper.state_authority ?? ''}>
          {formatGripperSemanticToken(gripper.state_authority)}
        </span>
        <span title={gripper.completion_policy ?? ''}>
          {formatGripperSemanticToken(gripper.completion_policy)}
        </span>
      </div>
      {derivedJoints.length > 0 && (
        <div
          className="truncate text-[10px] text-gray-500"
          title={`Mimic followers: ${derivedJoints.join(', ')}`}
        >
          follows: {derivedJoints.join(', ')}
        </div>
      )}
      <div className="flex items-baseline justify-between gap-2 text-[10px] text-gray-400">
        <span>
          {positionSourceLabel}:{' '}
          <span className="font-mono text-gray-300">{current.toFixed(3)}</span>
        </span>
        <span>
          cmd:{' '}
          <span
            className={`font-mono ${stagedDiverges ? 'text-amber-300' : 'text-gray-300'}`}
            title={stagedDiverges ? 'staged target (not yet executed)' : undefined}
          >
            {cmdTarget.toFixed(3)}
          </span>
        </span>
        <span className={busy ? 'text-amber-400' : 'text-gray-500'}>
          {stateLabel}
        </span>
      </div>
      {(gripper.commanded_state || evidence.length > 0 || gripper.evidence_state) && (
        <div className="truncate text-[10px] text-gray-500" title={[
          gripper.commanded_state ? `commanded=${gripper.commanded_state}` : '',
          evidence.length > 0 ? `evidence=${evidence.join(',')}` : '',
          gripper.evidence_state ? `state=${gripper.evidence_state}` : '',
        ].filter(Boolean).join(' · ')}>
          {gripper.commanded_state ? `cmd state: ${gripper.commanded_state}` : 'cmd state: n/a'}
          {evidence.length > 0 ? ` · ${evidence.join(', ')}` : ''}
          {gripper.evidence_state ? ` · ${formatGripperSemanticToken(gripper.evidence_state)}` : ''}
        </div>
      )}
      {completionNotice !== null && (
        <div className="rounded border border-amber-700/40 bg-amber-950/20 px-1.5 py-1 text-[10px] text-amber-200">
          {completionNotice}
        </div>
      )}
      <input
        type="range"
        min={lower}
        max={upper}
        // `step="any"` — same fix as JointRow. Without it Firefox marks
        // the slider invalid whenever the live kernel value lands
        // off-grid (which is almost always for a sim/real gripper
        // mid-motion) and shows a hover tooltip "Please select a valid
        // value, the two nearest valid values are…". The candidate
        // validation path validates the requested target server-side, so
        // the step-grid constraint adds no safety.
        step="any"
        value={displayValue}
        disabled={disabled}
        onChange={(e) => !disabled && setPending(Number(e.target.value))}
        onMouseUp={(e) =>
          authorGripper(Number((e.target as HTMLInputElement).value), 'gripper_slider')
        }
        onTouchEnd={(e) =>
          authorGripper(Number((e.target as HTMLInputElement).value), 'gripper_slider')
        }
        className={`w-full ${stagedDiverges ? 'accent-amber-400' : ''}`}
      />
      <div className="flex flex-wrap gap-1">
        {presetEntries.length > 0 ? (
          presetEntries.map(([name, value]) => (
            <button
              key={name}
              type="button"
              onClick={() => authorGripper(value, 'gripper_preset')}
              disabled={disabled}
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
              onClick={() => authorGripper(lower, 'gripper_preset')}
              disabled={disabled}
              className="rounded border border-gray-600 px-2 py-0.5 text-[10px] text-gray-200 hover:bg-gray-700"
              title={`lower limit (${lower.toFixed(3)})`}
            >
              ↓ {lower.toFixed(2)}
            </button>
            <button
              type="button"
              onClick={() => authorGripper(upper, 'gripper_preset')}
              disabled={disabled}
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
      {projectionFailed && !gripper.error && (
        <div className="text-[10px] text-red-400">
          {gripper.projection_reason || 'Gripper scene projection failed'}
        </div>
      )}
      {!commandGate.canCommand && (
        <div className="text-[10px] text-amber-400">{commandGate.reason}</div>
      )}
    </div>
  );
}

function PoseEditSnapshotSection({
  mode,
  commandGate,
}: {
  mode: 'joint' | 'cartesian';
  commandGate: RobotCommandGate;
}) {
  const appliedTree = useProgramStore((s) => s.appliedTree);
  const editLocked = useEditLocked();
  const wantMotionType = mode === 'joint' ? 'joint' : 'pose';
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
    const baseFrame = robot?.base_frame ?? 'base';
    return {
      pose_goal: {
        frame_id: baseFrame,
        position_m: readPosition(tcp),
        orientation_xyzw: readQuat(tcp),
      },
    };
  }

  async function stageOverride(): Promise<void> {
    if (selectedNodeName === null) return;
    if (!commandGate.canCommand) {
      setStatus({
        kind: 'error',
        detail: commandGate.reason ?? 'robot command unavailable',
      });
      return;
    }
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
    if (editLocked) {
      setStatus({ kind: 'error', detail: 'editor is locked while command is active' });
      return;
    }
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
          disabled={selectedNodeName === null || status.kind === 'sending' || !commandGate.canCommand}
          title={!commandGate.canCommand ? commandGate.reason ?? undefined : undefined}
          className="flex-1 rounded bg-gray-700 px-2 py-1 text-[11px] text-gray-200 hover:bg-gray-600 disabled:cursor-not-allowed disabled:opacity-50"
        >
          Stage as runtime override
        </button>
        <button
          type="button"
          onClick={saveToDraft}
          disabled={selectedNodeName === null || status.kind === 'sending' || editLocked}
          title={editLocked ? 'editor is locked while command is active' : undefined}
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
        {status.kind === 'idle' && !commandGate.canCommand && (
          <span className="ml-1 text-amber-400">· {commandGate.reason}</span>
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
  motionType: 'joint' | 'pose' | 'cartesian',
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

function compactNumber(value: number): string {
  if (!Number.isFinite(value)) return '0';
  return String(Number(value.toFixed(6)));
}

function formatRadStepAsDeg(valueRad: number): string {
  return compactNumber(valueRad * RAD_TO_DEG);
}

function rpyRadToDeg(rpy: [number, number, number]): [number, number, number] {
  return [rpy[0] * RAD_TO_DEG, rpy[1] * RAD_TO_DEG, rpy[2] * RAD_TO_DEG];
}

function rpyDegToRad(rpy: [number, number, number]): [number, number, number] {
  return [rpy[0] * DEG_TO_RAD, rpy[1] * DEG_TO_RAD, rpy[2] * DEG_TO_RAD];
}

function jointValueToDisplay(joint: SceneUrdfJoint, value: number): number {
  return joint.type === 'prismatic' ? value : value * RAD_TO_DEG;
}

function jointDisplayToValue(joint: SceneUrdfJoint, value: number): number {
  return joint.type === 'prismatic' ? value : value * DEG_TO_RAD;
}

function jointDeltaToDisplay(joint: SceneUrdfJoint, value: number): number {
  return joint.type === 'prismatic' ? value : value * RAD_TO_DEG;
}

function StepCapSelect({
  label,
  options,
  value,
  onChange,
  formatOption = (option: number): string => String(option),
  disabled = false,
}: {
  label: string;
  options: readonly number[];
  value: number;
  onChange: (next: number) => void;
  formatOption?: (option: number) => string;
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
            {formatOption(opt)}
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
  onDraftPush,
  onDraftPushFinal,
  disabled = false,
  disabledReason = null,
}: {
  joint: SceneUrdfJoint;
  currentValue: number | null;
  step: number;
  onDraftPush: (jointName: string, valueRad: number) => void;
  onDraftPushFinal: (jointName: string, valueRad: number) => void;
  disabled?: boolean;
  disabledReason?: string | null;
}) {
  const displayValue = jointValueToDisplay(joint, currentValue ?? 0);
  const displayStep = jointDeltaToDisplay(joint, step);
  const displayTolerance = jointDeltaToDisplay(joint, jointArrivalTolerance(joint));
  const displayUnit = joint.type === 'prismatic' ? 'm' : 'deg';
  const hasAuthoritativeRange =
    (joint.limit_lower !== null && joint.limit_upper !== null)
    || joint.type === 'continuous';
  // Joint-limit-based range: explicit limits win; continuous joints
  // default to [-π, π]. If limits are missing, direct control is disabled
  // instead of inventing a sliding live-position window as an authority
  // surrogate.
  const jointRange = useMemo(
    () => (
      hasAuthoritativeRange
        ? sliderRange(joint, currentValue)
        : { min: currentValue ?? 0, max: currentValue ?? 0 }
    ),
    [hasAuthoritativeRange, joint, currentValue],
  );
  const displayJointRange = useMemo(
    () => ({
      min: jointValueToDisplay(joint, jointRange.min),
      max: jointValueToDisplay(joint, jointRange.max),
    }),
    [joint, jointRange.max, jointRange.min],
  );
  // Persistent authored-target draft. A commit (slider release, spinner click,
  // Enter, blur) KEEPS the authored value visible — the control does not snap
  // back to live. The hook clears the draft only when the authored candidate is
  // discarded (Revert / mode-tab switch) or when the live value catches up to
  // the draft within tolerance (after an Approve executes), so the slider
  // resumes following live state. `isDraft` doubles as the authored-vs-live
  // signal: a draft within tolerance of live auto-clears, so a held draft means
  // the authored target diverges from the live joint position.
  const { draft, isDraft, setDraft, clearDraft } = usePersistentDraft(
    currentValue === null ? Number.NaN : displayValue,
    displayTolerance,
    'joint',
  );
  const divergesFromLive = isDraft;
  const draftNumeric = draft === null ? displayValue : Number(draft);
  const sliderValue = Number.isFinite(draftNumeric)
    ? Math.min(displayJointRange.max, Math.max(displayJointRange.min, draftNumeric))
    : displayValue;

  // Clear on any cycle transition that hands the joint over to an authoritative
  // non-operator source (BT runner, interrupt handler, direct-control session),
  // so the slider resumes following live state instead of freezing at the
  // operator's last authored value while live state moves underneath.
  const cycleState = useCycleStore((s) => s.state);
  useEffect(() => {
    if (
      cycleState === 'running'
      || cycleState === 'stopping'
      || cycleState === 'direct_control'
    ) {
      clearDraft();
    }
  }, [cycleState, clearDraft]);

  const clamp = useCallback((value: number): number => {
    return Math.min(displayJointRange.max, Math.max(displayJointRange.min, value));
  }, [displayJointRange.max, displayJointRange.min]);

  const displayToInternal = useCallback(
    (value: number): number => jointDisplayToValue(joint, value),
    [joint],
  );

  function formattedLiveValue(): string {
    return formatJointValue(displayValue, displayStep);
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
  // click, Enter, blur) — which authors and KEEPS that value. No
  // motion; Approve is the only motion path.
  const numberInputRef = useRef<HTMLInputElement | null>(null);
  const clampRef = useRef(clamp);
  const jointNameRef = useRef(joint.name);
  const hasAuthoritativeRangeRef = useRef(hasAuthoritativeRange);
  const disabledRef = useRef(disabled);
  const displayToInternalRef = useRef(displayToInternal);
  const onDraftPushFinalRef = useRef(onDraftPushFinal);
  useEffect(() => {
    clampRef.current = clamp;
    jointNameRef.current = joint.name;
    hasAuthoritativeRangeRef.current = hasAuthoritativeRange;
    disabledRef.current = disabled;
    displayToInternalRef.current = displayToInternal;
    onDraftPushFinalRef.current = onDraftPushFinal;
  }, [clamp, disabled, displayToInternal, hasAuthoritativeRange, joint.name, onDraftPushFinal]);
  useEffect(() => {
    const el = numberInputRef.current;
    if (el === null) return;
    const onNativeInput = (event: Event): void => {
      if (disabledRef.current || !hasAuthoritativeRangeRef.current) return;
      const inputEvent = event as InputEvent;
      // Browser spinner buttons usually emit a native input event
      // without an inputType. Treat that as an operator commit of the
      // authored value. Keyboard text input has inputType values such
      // as insertText, so typing remains local draft state until
      // blur/Enter.
      if (!inputEvent.inputType) {
        const raw = el.value;
        const v = Number(raw);
        if (!Number.isFinite(v)) return;
        const clamped = clampRef.current(v);
        // Keep the authored target visible (no snap-back); it clears only on
        // Revert / mode switch / live catch-up (see usePersistentDraft).
        setDraft(String(Number(clamped.toFixed(6))));
        onDraftPushFinalRef.current(jointNameRef.current, displayToInternalRef.current(clamped));
        return;
      }
      setDraft(el.value);
    };
    const onNativeChange = (): void => {
      if (disabledRef.current || !hasAuthoritativeRangeRef.current) return;
      const raw = el.value;
      if (raw === '') return;
      const v = Number(raw);
      if (!Number.isFinite(v)) return;
      const clamped = clampRef.current(v);
      setDraft(String(Number(clamped.toFixed(6))));
      onDraftPushFinalRef.current(jointNameRef.current, displayToInternalRef.current(clamped));
    };
    el.addEventListener('input', onNativeInput);
    el.addEventListener('change', onNativeChange);
    return () => {
      el.removeEventListener('input', onNativeInput);
      el.removeEventListener('change', onNativeChange);
    };
  }, []);

  function draftFollow(raw: string): void {
    if (disabled) return;
    if (!hasAuthoritativeRange) return;
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    // Author-only: records the in-progress authored value. No motion;
    // Approve is the only motion path.
    onDraftPush(joint.name, displayToInternal(clamp(v)));
  }

  function draftFinal(raw: string): void {
    if (disabled) return;
    if (!hasAuthoritativeRange) return;
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    onDraftPushFinal(joint.name, displayToInternal(clamp(v)));
  }

  function commitValue(raw: string): void {
    const requested = Number(raw);
    if (disabled) {
      return;
    }
    if (!hasAuthoritativeRange || !Number.isFinite(requested)) {
      return;
    }
    const clamped = clamp(requested);
    // Keep the authored target visible after release/Enter (no snap-back).
    setDraft(String(Number(clamped.toFixed(6))));
    onDraftPushFinal(joint.name, displayToInternal(clamped));
  }

  function commitTyped(): void {
    if (draft === null) return;
    commitValue(draft);
  }

  return (
    <div className="border-b border-gray-800 py-1 last:border-b-0">
      <div className="flex items-baseline justify-between gap-1">
        <span className="truncate font-mono text-[11px]">
          {joint.name}
          {divergesFromLive && (
            <span className="ml-1 rounded bg-amber-900/50 px-1 text-[9px] font-medium uppercase tracking-wide text-amber-300">
              target
            </span>
          )}
        </span>
        {divergesFromLive && currentValue !== null ? (
          <span
            className="font-mono text-[10px] text-gray-400"
            title="live joint position (not the authored target)"
          >
            live {formatJointValue(jointValueToDisplay(joint, currentValue), displayStep)} {displayUnit}
          </span>
        ) : (
          <span className="text-[10px] text-gray-500">{joint.type} · {displayUnit}</span>
        )}
      </div>
      <div className="mt-0.5 flex items-center gap-2">
        <input
          type="range"
          min={displayJointRange.min}
          max={displayJointRange.max}
          // `step="any"` makes the slider accept any value the operator
          // drags to AND keeps it `:valid` when the live joint state
          // is pushed in from outside. Without it,
          // Firefox marks the slider invalid and shows the native
          // "Please select a valid value. The two nearest valid values
          // are…" tooltip — operator UX-breaking even though the value
          // is correct. Keyboard arrows fall back to (max-min)/100
          // which is still useful for jog precision. clamp() bounds the
          // authored draft to the joint
          // range; validated planning is the authoritative validator.
          step="any"
          value={sliderValue}
          disabled={disabled || !hasAuthoritativeRange}
          title={disabled ? disabledReason ?? undefined : undefined}
          onChange={(e) => {
            setDraft(e.target.value);
            draftFollow(e.target.value);
          }}
          onMouseUp={(e) => {
            // Release authors the draft target and KEEPS it visible (no
            // snap-back to live); it never moves the robot (Approve is the only
            // motion path).
            const input = e.target as HTMLInputElement;
            commitValue(input.value);
          }}
          onTouchEnd={(e) => {
            const input = e.target as HTMLInputElement;
            commitValue(input.value);
          }}
          className={`flex-1 ${divergesFromLive ? 'accent-amber-400' : ''}`}
        />
        <input
          ref={numberInputRef}
          type="number"
          // Spinner-button increments and keyboard arrows both use this
          // attribute. We deliberately do NOT use `step="any"` here
          // (the slider does, for the Firefox :valid hover tooltip
          // bug — see slider above); for a number input,
          // controlled-value props are accepted verbatim regardless
          // of step, and `step={displayStep}` is what makes the visible
          // spinner buttons honour the operator's selected step
          // instead of the browser default of 1.
          step={displayStep}
          value={draft ?? formattedLiveValue()}
          disabled={disabled || !hasAuthoritativeRange}
          title={disabled ? disabledReason ?? undefined : undefined}
          // Required for React's controlled-input warning, but a noop:
          // the real handlers are the native 'input' and 'change'
          // listeners installed via useEffect above. Going through
          // React onChange would interact badly with the native
          // listener (synthetic-event ordering vs the directly-bound
          // listeners) for spinner clicks.
          onChange={() => {}}
          onBlur={commitTyped}
          onKeyDown={(e) => {
            if (e.key === 'Enter') {
              e.preventDefault();
              commitTyped();
              (e.target as HTMLInputElement).blur();
            } else if (e.key === 'Escape') {
              // Explicit cancel of the in-progress edit: return to the live
              // value and blur. (Commits do NOT clear the draft; only this
              // explicit gesture and Revert/mode-switch/live-catch-up do.)
              e.preventDefault();
              clearDraft();
              (e.target as HTMLInputElement).blur();
            } else if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
              e.preventDefault();
              const direction = e.key === 'ArrowUp' ? 1 : -1;
              const base = Number(draft ?? displayValue);
              const delta = (Number.isFinite(displayStep) && displayStep > 0 ? displayStep : 0.01)
                * (e.shiftKey ? 10 : 1)
                * direction;
              const next = clamp((Number.isFinite(base) ? base : displayValue) + delta);
              const text = String(Number(next.toFixed(6)));
              setDraft(text);
              draftFinal(text);
            }
          }}
          className={`w-20 rounded py-0.5 pl-1 pr-2 text-right font-mono text-[10px] ${
            divergesFromLive
              ? 'bg-amber-950/40 text-amber-200 ring-1 ring-amber-600/50'
              : 'bg-gray-800 text-gray-200'
          }`}
        />
      </div>
      {(joint.limit_lower !== null || joint.limit_upper !== null) && (
        <div className="mt-0.5 flex justify-between font-mono text-[9px] text-gray-600">
          <span>
            {joint.limit_lower !== null
              ? formatJointValue(jointValueToDisplay(joint, joint.limit_lower), displayStep)
              : '-∞'}
          </span>
          <span>
            {joint.limit_upper !== null
              ? formatJointValue(jointValueToDisplay(joint, joint.limit_upper), displayStep)
              : '+∞'}
          </span>
        </div>
      )}
      {!hasAuthoritativeRange && (
        <div className="mt-0.5 text-[9px] text-amber-400">
          Direct joint control disabled: missing position limits.
        </div>
      )}
      {disabled && disabledReason !== null && (
        <div className="mt-0.5 text-[9px] text-amber-400">
          Direct joint control disabled: {disabledReason}.
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
