import { useEffect, useMemo, useRef, useState } from 'react';

import { api } from '../api/client';
import { useProgramStore } from '../store/programStore';
import { useSceneStateStore } from '../store/sceneStateStore';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import type { CartesianFrame } from '../store/viewerSelectionStore';
import type {
  SceneUrdfJoint,
  SceneUrdfManifest,
  TreeNode,
} from '../types/api';

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

type JogStatus = 'idle' | 'sending' | 'success' | 'error';

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
  const robotMode = useViewerSelectionStore((s) => s.robotMode);
  const setRobotMode = useViewerSelectionStore((s) => s.setRobotMode);
  const cartesianFrame = useViewerSelectionStore((s) => s.cartesianFrame);
  const setCartesianFrame = useViewerSelectionStore((s) => s.setCartesianFrame);

  useEffect(() => {
    let cancelled = false;
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
  }, []);

  const drivableJoints = useMemo(() => {
    if (manifest?.joints === undefined) return [];
    return manifest.joints.filter((j) => DRIVABLE_TYPES.has(j.type));
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
      </div>
      {robotMode === 'joint' && (
        <>
          <div className="flex items-center gap-2 border-b border-gray-800 bg-gray-900/50 px-2 py-1.5">
            <StepCapSelect
              label="step (rad)"
              options={REVOLUTE_STEPS}
              value={jointRadStep}
              onChange={setJointRadStep}
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
              />
            ))}
            <PoseEditSnapshotSection mode="joint" />
          </div>
          <div className="border-t border-gray-700 bg-gray-800/50 px-2 py-1 text-[10px] text-gray-500">
            Jog · slider drags limit-to-limit · numeric arrow = step · returns to IDLE between commits
          </div>
        </>
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
    </div>
  );
}

function ModeTab({
  id,
  active,
  onClick,
  disabled = false,
  children,
}: {
  id: 'joint' | 'cartesian' | 'pose-edit';
  active: 'joint' | 'cartesian' | 'pose-edit';
  onClick: (id: 'joint' | 'cartesian' | 'pose-edit') => void;
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
}: {
  joint: SceneUrdfJoint;
  currentValue: number | null;
  step: number;
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
  const [status, setStatus] = useState<JogStatus>('idle');
  const [statusDetail, setStatusDetail] = useState<string | null>(null);
  const displayValue = currentValue ?? 0;
  const draftNumeric = draft === null ? displayValue : Number(draft);
  const sliderValue = Number.isFinite(draftNumeric)
    ? Math.min(jointRange.max, Math.max(jointRange.min, draftNumeric))
    : displayValue;
  // Real-time follow throttle: keep slider drags + numeric-arrow
  // clicks responsive without flooding the backend.
  const lastJogTsRef = useRef(0);
  const FOLLOW_MIN_INTERVAL_MS = 33;

  // Clear the draft when the live value catches up — happens after
  // a successful jog when scene-state polls in the new position.
  useEffect(() => {
    if (draft !== null && Number.isFinite(Number(draft)) && currentValue !== null) {
      if (Math.abs(Number(draft) - currentValue) < 1e-6) {
        setDraft(null);
      }
    }
  }, [currentValue, draft]);

  // Auto-clear success status after a moment so the row settles
  // back to idle visually once the operator moves on.
  useEffect(() => {
    if (status !== 'success') return;
    const t = window.setTimeout(() => {
      setStatus('idle');
      setStatusDetail(null);
    }, 1500);
    return () => window.clearTimeout(t);
  }, [status]);

  function fireJog(value: number): void {
    const clamped = Math.min(jointRange.max, Math.max(jointRange.min, value));
    setStatus('sending');
    setStatusDetail(null);
    void api.direct
      .jointJog([joint.name], [clamped])
      .then(() => {
        setStatus('success');
        setStatusDetail(
          Math.abs(clamped - value) > 1e-6
            ? `clamped to ${clamped.toFixed(3)}`
            : null,
        );
      })
      .catch((err: unknown) => {
        setStatus('error');
        const msg = err instanceof Error ? err.message : 'jog failed';
        setStatusDetail(msg);
        console.warn('joint jog failed for', joint.name, err);
      });
  }

  function commitTyped(): void {
    if (draft === null) return;
    const requested = Number(draft);
    if (!Number.isFinite(requested)) {
      setDraft(null);
      return;
    }
    setDraft(null);
    fireJog(requested);
  }

  function followJog(raw: string): void {
    const v = Number(raw);
    if (!Number.isFinite(v)) return;
    const now = performance.now();
    if (now - lastJogTsRef.current < FOLLOW_MIN_INTERVAL_MS) return;
    lastJogTsRef.current = now;
    fireJog(v);
  }

  return (
    <div className="border-b border-gray-800 py-1 last:border-b-0">
      <div className="flex items-baseline justify-between gap-1">
        <span className="truncate font-mono text-[11px]">{joint.name}</span>
        <div className="flex items-center gap-1">
          <StatusBadge status={status} detail={statusDetail} />
          <span className="text-[10px] text-gray-500">{joint.type}</span>
        </div>
      </div>
      <div className="mt-0.5 flex items-center gap-2">
        <input
          type="range"
          min={jointRange.min}
          max={jointRange.max}
          step={Math.max((jointRange.max - jointRange.min) / 200, 1e-4)}
          value={sliderValue}
          onChange={(e) => {
            setDraft(e.target.value);
            followJog(e.target.value);
          }}
          onMouseUp={commitTyped}
          onTouchEnd={commitTyped}
          className="flex-1"
        />
        <input
          type="number"
          step={Number.isFinite(step) && step > 0 ? step : 0.01}
          value={draft ?? displayValue.toFixed(2)}
          onChange={(e) => {
            setDraft(e.target.value);
            // Spinner-button (step up/down) clicks fire a synthetic
            // input event with no `inputType` — treat those as
            // immediate jog requests. Typed keystrokes have
            // inputType like `insertText` and defer to onBlur/Enter.
            const native = e.nativeEvent as InputEvent;
            if (!native.inputType) followJog(e.target.value);
          }}
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

function StatusBadge({ status, detail }: { status: JogStatus; detail: string | null }) {
  if (status === 'idle') return null;
  const tone =
    status === 'sending'
      ? 'bg-blue-900 text-blue-200'
      : status === 'success'
        ? 'bg-green-900 text-green-200'
        : 'bg-red-900 text-red-200';
  const label =
    status === 'sending' ? 'sending' : status === 'success' ? 'ok' : 'error';
  return (
    <span
      className={`rounded px-1 text-[9px] uppercase tracking-wide ${tone}`}
      title={detail ?? undefined}
    >
      {label}
      {detail && status !== 'sending' && (
        <span className="ml-1 normal-case">{detail.slice(0, 24)}</span>
      )}
    </span>
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
