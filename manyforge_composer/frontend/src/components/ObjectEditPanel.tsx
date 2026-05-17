import { useMemo, useState } from 'react';

import { api } from '../api/client';
import { useSceneStateStore } from '../store/sceneStateStore';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import type { SceneObjectState } from '../types/api';

/**
 * Day-5 object-edit overlay panel. Renders metadata for whichever
 * scene object the viewer-selection store currently points at, and
 * exposes numeric pose edits that POST to `/api/scene/edit
 * update_pose`. Read-only for runtime / sensed / attached objects;
 * the panel labels those clearly instead of disabling silently.
 *
 * The slide-to-position interaction is the gizmo (Day 2); this
 * panel is for keyboard-precise numeric edits and as the place a
 * future Day-5b extension binds the "persist to selected BT node"
 * action (currently scaffolded by `scene_draft_*` tools, but no UI).
 */
interface ObjectEditPanelProps {
  onClose: () => void;
}

export function ObjectEditPanel({ onClose }: ObjectEditPanelProps) {
  const selectedId = useViewerSelectionStore((s) => s.selectedId);
  const objects = useSceneStateStore((s) => s.scene?.objects ?? EMPTY_OBJECTS);
  const object = useMemo(
    () => (selectedId === null ? null : objects.find((o) => o.object_id === selectedId) ?? null),
    [objects, selectedId],
  );

  return (
    <div className="pointer-events-auto absolute right-2 top-2 z-20 flex max-h-[calc(100%-1rem)] w-72 flex-col overflow-hidden rounded-md border border-gray-700 bg-gray-900/50 text-xs text-gray-200 shadow-xl">
      <div className="flex items-center justify-between border-b border-gray-700 bg-gray-800/50 px-2 py-1">
        <div className="text-[11px] font-semibold uppercase tracking-wide text-gray-300">
          Object Edit
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
      <div className="overflow-y-auto p-2">
        {object === null ? (
          <div className="italic text-gray-500">No object selected.</div>
        ) : (
          <ObjectEditor object={object} />
        )}
      </div>
    </div>
  );
}

const EMPTY_OBJECTS: SceneObjectState[] = [];

function ObjectEditor({ object }: { object: SceneObjectState }) {
  const editable = object.persistable && !object.attached;
  const pose = (object.pose_in_universe ?? {}) as Record<string, unknown>;
  const position = numberTriple(pose.position_m, [0, 0, 0]);
  const orientation = numberQuad(pose.orientation_xyzw, [0, 0, 0, 1]);

  return (
    <div className="space-y-2">
      <div>
        <div className="text-[10px] uppercase tracking-wide text-gray-500">Object</div>
        <div className="truncate font-mono">{object.object_id}</div>
        <div className="mt-0.5 flex gap-2 text-[10px] text-gray-500">
          <span>source={object.source}</span>
          {object.attached && <span>attached→{object.attached_to ?? '?'}</span>}
        </div>
        {!editable && (
          <div className="mt-1 rounded bg-gray-800 px-1.5 py-1 text-[10px] text-orange-300">
            {object.attached
              ? 'attached objects cannot be re-posed from here'
              : 'runtime/sensed objects are not persistable'}
          </div>
        )}
      </div>

      <ShapeReadout shape={object.shape ?? null} />

      <PoseEditor
        label="Position (m)"
        axisLabels={['x', 'y', 'z']}
        values={position}
        disabled={!editable}
        commit={(next) => emitPoseEdit(object.object_id, next, orientation)}
      />

      <PoseEditor
        label="Orientation (xyzw)"
        axisLabels={['x', 'y', 'z', 'w']}
        values={orientation}
        disabled={!editable}
        commit={(next) => emitPoseEdit(object.object_id, position, next)}
      />

      <SaveToProgramRow object={object} />
    </div>
  );
}

/**
 * Persistence row for the Object Edit panel (issue #2 followup).
 *
 * On-blur edits go to /api/scene/edit which only mutates the live
 * scene — the underlying program YAML doesn't change. This row
 * lets the operator promote the *current* live values into:
 *
 * - Program scene resource (authored objects): writes the current
 *   pose/shape back to `program.scene[].object_id == ...` via the
 *   same helper the assistant's `scene_draft_update_object` uses.
 *   Operator saves the program through ProgramPanel afterwards to
 *   commit to disk.
 * - Source BT-node override (runtime objects, follow-up): not
 *   wired yet — would require finding the `add_collision_object`
 *   node whose params.object_id matches and runtime-overriding
 *   its pose params. Tracked as the open V1 gap.
 */
function SaveToProgramRow({ object }: { object: SceneObjectState }) {
  const [status, setStatus] = useState<
    | { kind: 'idle' }
    | { kind: 'sending' }
    | { kind: 'success'; detail: string }
    | { kind: 'error'; detail: string }
  >({ kind: 'idle' });

  const authored = object.source === 'authored' && !object.attached;
  const runtime = object.source === 'runtime' && !object.attached;

  async function saveToProgram(): Promise<void> {
    setStatus({ kind: 'sending' });
    const pose = (object.pose_in_universe ?? {}) as Record<string, unknown>;
    const shape = (object.shape ?? {}) as Record<string, unknown>;
    const payload: Record<string, unknown> = {
      objectId: object.object_id,
      position: pose.position_m,
    };
    // Without this the assistant-tool helper interprets omitted
    // orientation as "identity" and the scene resource ends up with
    // its rotation reset — operator-visible as "the box snaps to
    // axis-aligned when I save". Pass the live orientation through.
    if (Array.isArray(pose.orientation_xyzw) && (pose.orientation_xyzw as unknown[]).length === 4) {
      payload.orientation_xyzw = pose.orientation_xyzw;
    }
    const shapeType = typeof shape.type === 'string' ? shape.type : undefined;
    if (shapeType === 'box') {
      payload.shapeType = 'box';
      payload.size = shape.box_dimensions_m;
    } else if (shapeType === 'sphere') {
      payload.shapeType = 'sphere';
      payload.radius = shape.sphere_radius_m;
    }
    try {
      await api.scene.updateProgramSceneResource(payload);
      setStatus({
        kind: 'success',
        detail: 'in draft — Save via ProgramPanel for permanence',
      });
    } catch (err: unknown) {
      setStatus({
        kind: 'error',
        detail: err instanceof Error ? err.message : 'save failed',
      });
    }
  }

  return (
    <div className="mt-2 border-t border-gray-800 pt-2">
      <button
        type="button"
        onClick={saveToProgram}
        disabled={!authored || status.kind === 'sending'}
        title={
          authored
            ? 'Persist live pose into the program scene-resource draft'
            : runtime
              ? 'Runtime objects come from a BT node — edit the source node to persist'
              : object.attached
                ? 'Attached objects follow their parent link; not persistable here'
                : 'Sensed objects aren’t persistable'
        }
        className="w-full rounded bg-blue-700 px-2 py-1 text-[11px] text-white hover:bg-blue-600 disabled:cursor-not-allowed disabled:opacity-50"
      >
        Save to program (scene resource)
      </button>
      <SaveStatus status={status} runtime={runtime} />
    </div>
  );
}

function SaveStatus({
  status,
  runtime,
}: {
  status:
    | { kind: 'idle' }
    | { kind: 'sending' }
    | { kind: 'success'; detail: string }
    | { kind: 'error'; detail: string };
  runtime: boolean;
}) {
  if (status.kind === 'idle') {
    return (
      <div className="mt-1 text-[10px] text-gray-500">
        {runtime
          ? 'runtime-only — source BT-node override coming in a follow-up'
          : 'live edits = on blur; this button writes them to the program draft'}
      </div>
    );
  }
  if (status.kind === 'sending') {
    return <div className="mt-1 text-[10px] text-blue-300">sending…</div>;
  }
  if (status.kind === 'success') {
    return <div className="mt-1 text-[10px] text-green-300">✓ {status.detail}</div>;
  }
  return (
    <div className="mt-1 text-[10px] text-red-300" title={status.detail}>
      ✗ {status.detail.slice(0, 60)}
    </div>
  );
}

function ShapeReadout({ shape }: { shape: Record<string, unknown> | null }) {
  if (shape === null) return null;
  const type = String(shape.type ?? '?');
  let detail = '';
  if (type === 'box' && Array.isArray(shape.box_dimensions_m)) {
    detail = (shape.box_dimensions_m as number[])
      .map((v) => Number(v).toFixed(3))
      .join(' × ');
  } else if (type === 'sphere' && typeof shape.sphere_radius_m === 'number') {
    detail = `r=${(shape.sphere_radius_m as number).toFixed(3)}`;
  } else if (typeof shape.mesh_resource_uri === 'string') {
    detail = String(shape.mesh_resource_uri);
  }
  return (
    <div>
      <div className="text-[10px] uppercase tracking-wide text-gray-500">Shape</div>
      <div className="font-mono text-[11px]">{type}</div>
      {detail && <div className="font-mono text-[10px] text-gray-400">{detail}</div>}
    </div>
  );
}

function PoseEditor<L extends string[]>({
  label,
  axisLabels,
  values,
  disabled,
  commit,
}: {
  label: string;
  axisLabels: L;
  values: number[];
  disabled: boolean;
  commit: (next: number[]) => void;
}) {
  const [drafts, setDrafts] = useState<Record<number, string>>({});

  return (
    <div>
      <div className="text-[10px] uppercase tracking-wide text-gray-500">{label}</div>
      <div className="mt-0.5 grid grid-cols-2 gap-1">
        {axisLabels.map((axis, idx) => {
          const live = Number(values[idx] ?? 0);
          const draft = drafts[idx];
          return (
            <label key={axis} className="flex items-center gap-1 font-mono text-[10px]">
              <span className="w-3 text-gray-500">{axis}</span>
              <input
                type="number"
                step={0.001}
                value={draft ?? live.toFixed(4)}
                disabled={disabled}
                onChange={(e) => setDrafts((d) => ({ ...d, [idx]: e.target.value }))}
                onBlur={() => {
                  if (drafts[idx] === undefined) return;
                  const next = [...values];
                  next[idx] = Number(drafts[idx]);
                  setDrafts((d) => {
                    const copy = { ...d };
                    delete copy[idx];
                    return copy;
                  });
                  if (!Number.isFinite(next[idx])) return;
                  commit(next);
                }}
                onKeyDown={(e) => {
                  if (e.key === 'Enter') {
                    e.preventDefault();
                    (e.target as HTMLInputElement).blur();
                  } else if (e.key === 'Escape') {
                    e.preventDefault();
                    setDrafts((d) => {
                      const copy = { ...d };
                      delete copy[idx];
                      return copy;
                    });
                    (e.target as HTMLInputElement).blur();
                  }
                }}
                className="flex-1 rounded bg-gray-800 px-1 py-0.5 text-right text-gray-200 disabled:cursor-not-allowed disabled:opacity-50"
              />
            </label>
          );
        })}
      </div>
    </div>
  );
}

function emitPoseEdit(
  objectId: string,
  position: number[],
  orientation: number[],
): void {
  if (position.length !== 3 || orientation.length !== 4) return;
  if (![...position, ...orientation].every((v) => Number.isFinite(v))) return;
  void api.scene
    .edit({
      operation: 'update_pose',
      object_id: objectId,
      pose_position_m: [position[0], position[1], position[2]],
      pose_orientation_xyzw: [orientation[0], orientation[1], orientation[2], orientation[3]],
    })
    .catch((err) => {
      console.warn('scene edit failed for', objectId, err);
    });
}

function numberTriple(value: unknown, fallback: [number, number, number]): [number, number, number] {
  if (Array.isArray(value) && value.length === 3) {
    return [Number(value[0]) || 0, Number(value[1]) || 0, Number(value[2]) || 0];
  }
  return fallback;
}

function numberQuad(
  value: unknown,
  fallback: [number, number, number, number],
): [number, number, number, number] {
  if (Array.isArray(value) && value.length === 4) {
    return [
      Number(value[0]) || 0,
      Number(value[1]) || 0,
      Number(value[2]) || 0,
      Number(value[3]) || 1,
    ];
  }
  return fallback;
}
