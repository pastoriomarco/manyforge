import { useMemo, useState } from 'react';

import { api } from '../api/client';
import { useSceneStateStore } from '../store/sceneStateStore';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import type {
  PoseSourceStatusState,
  SceneFrameState,
  SceneObjectState,
} from '../types/api';

/**
 * Viewer controls for the native viewer: frame axis
 * gizmo size, per-frame visibility, per-object frame visibility, and
 * a runtime scene tree grouped by source. Rendered as a floating
 * overlay panel in the top-left of the viewer area, mutually
 * exclusive with the Robot / Object overlay panels.
 *
 * All state lives backend-side (ComposerState owns it as of Day 4
 * parity work) so this panel just reads from the shared scene-state
 * store and posts updates via the existing /api/scene/* routes.
 */
interface SceneControlsProps {
  onClose: () => void;
}

export function SceneControls({ onClose }: SceneControlsProps) {
  const frames = useSceneStateStore((s) => s.frames);
  const objects = useSceneStateStore((s) => s.scene?.objects ?? EMPTY_OBJECTS);
  const poseSources = useSceneStateStore(
    (s) => s.scene?.pose_sources ?? EMPTY_POSE_SOURCES,
  );
  const objectAxesGizmoSize = useSceneStateStore(
    (s) => s.scene?.object_axes_gizmo_size_m ?? 0.1,
  );
  const robot = useSceneStateStore((s) => s.scene?.robot ?? null);
  const selectedId = useViewerSelectionStore((s) => s.selectedId);
  const setSelected = useViewerSelectionStore((s) => s.setSelected);
  const lockedIds = useViewerSelectionStore((s) => s.lockedIds);
  const [openSection, setOpenSection] = useState<Section>('frames');

  return (
    <div className="pointer-events-auto absolute right-2 top-2 z-20 flex max-h-[calc(100%-5rem)] w-72 flex-col overflow-hidden rounded-md border border-gray-700 bg-gray-900/50 text-xs text-gray-200 shadow-xl">
      <div className="flex items-center justify-between border-b border-gray-700 bg-gray-800/50 px-2 py-1">
        <div className="text-[11px] font-semibold uppercase tracking-wide text-gray-300">
          Scene Tools
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
        <SectionTab id="frames" current={openSection} onClick={setOpenSection}>
          Frames
        </SectionTab>
        <SectionTab id="objects" current={openSection} onClick={setOpenSection}>
          Object Axes
        </SectionTab>
        <SectionTab id="tree" current={openSection} onClick={setOpenSection}>
          Tree
        </SectionTab>
        <SectionTab id="sources" current={openSection} onClick={setOpenSection}>
          Sources
        </SectionTab>
      </div>
      <div className="flex-1 overflow-y-auto bg-gray-950/50 p-2">
        {openSection === 'frames' && (
          <FramesSection
            frames={frames?.frames ?? EMPTY_FRAMES}
            gizmoSize={frames?.frameGizmoSizeM ?? 0.1}
          />
        )}
        {openSection === 'objects' && (
          <ObjectsSection objects={objects} gizmoSize={objectAxesGizmoSize} />
        )}
        {openSection === 'tree' && (
          <RuntimeTree
            robot={robot}
            objects={objects}
            selectedId={selectedId}
            onSelect={setSelected}
            lockedIds={lockedIds}
          />
        )}
        {openSection === 'sources' && <PoseSourcesSection sources={poseSources} />}
      </div>
    </div>
  );
}

type Section = 'frames' | 'objects' | 'tree' | 'sources';

const EMPTY_FRAMES: SceneFrameState[] = [];
const EMPTY_OBJECTS: SceneObjectState[] = [];
const EMPTY_POSE_SOURCES: PoseSourceStatusState[] = [];

function SectionTab({
  id,
  current,
  onClick,
  children,
}: {
  id: Section;
  current: Section;
  onClick: (id: Section) => void;
  children: React.ReactNode;
}) {
  const active = id === current;
  return (
    <button
      type="button"
      onClick={() => onClick(id)}
      className={`flex-1 rounded px-2 py-1 text-[11px] font-medium uppercase tracking-wide ${
        active
          ? 'bg-gray-700 text-white'
          : 'bg-gray-800 text-gray-400 hover:text-gray-200'
      }`}
    >
      {children}
    </button>
  );
}

function FramesSection({
  frames,
  gizmoSize,
}: {
  frames: SceneFrameState[];
  gizmoSize: number;
}) {
  // Local state on the slider so dragging it feels responsive even
  // while the server-side state hasn't returned the new value yet.
  const [localSize, setLocalSize] = useState<number | null>(null);
  const displaySize = localSize ?? gizmoSize;

  function commitSize(value: number): void {
    setLocalSize(null);
    void api.scene.setFrameGizmoSize(value).catch((err) => {
      console.warn('gizmo size update failed', err);
    });
  }

  return (
    <div className="space-y-2">
      <label className="flex items-center justify-between gap-2 text-[11px]">
        <span className="text-gray-400">Gizmo size</span>
        <span className="font-mono text-gray-300">{displaySize.toFixed(3)} m</span>
      </label>
      <input
        type="range"
        min="0.02"
        max="1.0"
        step="0.01"
        value={displaySize}
        onChange={(e) => setLocalSize(Number(e.target.value))}
        onMouseUp={(e) => commitSize(Number((e.target as HTMLInputElement).value))}
        onTouchEnd={(e) => commitSize(Number((e.target as HTMLInputElement).value))}
        className="w-full"
      />
      <div className="mt-2 space-y-0.5">
        {frames.length === 0 ? (
          <div className="text-[11px] italic text-gray-500">No frames yet (load a program/deployment).</div>
        ) : (
          frames.map((frame) => (
            <FrameRow key={frame.name} frame={frame} />
          ))
        )}
      </div>
    </div>
  );
}

function FrameRow({ frame }: { frame: SceneFrameState }) {
  return (
    <label className="flex items-center gap-2 truncate text-[11px] text-gray-300 hover:text-white">
      <input
        type="checkbox"
        checked={frame.viewerVisible}
        onChange={(e) => {
          void api.scene
            .setFrameVisibility(frame.name, e.target.checked)
            .catch((err) => console.warn('frame visibility failed', err));
        }}
      />
      <span className="flex-1 truncate font-mono">{frame.name}</span>
      {frame.roles.length > 0 && (
        <span className="text-[10px] text-gray-500">{frame.roles.join(',')}</span>
      )}
      {!frame.poseAvailable && (
        <span className="text-[10px] italic text-gray-600">no pose</span>
      )}
    </label>
  );
}

function ObjectsSection({
  objects,
  gizmoSize,
}: {
  objects: SceneObjectState[];
  gizmoSize: number;
}) {
  // Local state on the slider so dragging feels responsive even
  // while the server-side state hasn't returned the new value yet.
  const [localSize, setLocalSize] = useState<number | null>(null);
  const displaySize = localSize ?? gizmoSize;

  function commitSize(value: number): void {
    setLocalSize(null);
    void api.scene.setObjectAxesGizmoSize(value).catch((err) => {
      console.warn('object axes gizmo size update failed', err);
    });
  }

  return (
    <div className="space-y-2">
      <label className="flex items-center justify-between gap-2 text-[11px]">
        <span className="text-gray-400">Gizmo size</span>
        <span className="font-mono text-gray-300">{displaySize.toFixed(3)} m</span>
      </label>
      <input
        type="range"
        min="0.02"
        max="1.0"
        step="0.01"
        value={displaySize}
        onChange={(e) => setLocalSize(Number(e.target.value))}
        onMouseUp={(e) => commitSize(Number((e.target as HTMLInputElement).value))}
        onTouchEnd={(e) => commitSize(Number((e.target as HTMLInputElement).value))}
        className="w-full"
      />
      {objects.length === 0 ? (
        <div className="text-[11px] italic text-gray-500">No scene objects.</div>
      ) : (
        <div className="space-y-0.5">
          {objects.map((obj) => (
            <ObjectFrameRow key={obj.object_id} object={obj} />
          ))}
        </div>
      )}
    </div>
  );
}

function ObjectFrameRow({ object }: { object: SceneObjectState }) {
  return (
    <label className="flex items-center gap-2 truncate text-[11px] text-gray-300 hover:text-white">
      <input
        type="checkbox"
        checked={object.viewer_frame_visible}
        onChange={(e) => {
          void api.scene
            .setObjectFrameVisibility(object.object_id, e.target.checked)
            .catch((err) => console.warn('object frame visibility failed', err));
        }}
      />
      <span className="flex-1 truncate font-mono">{object.object_id}</span>
      <span className="text-[10px] text-gray-500">{object.source}</span>
    </label>
  );
}

function RuntimeTree({
  robot,
  objects,
  selectedId,
  onSelect,
  lockedIds,
}: {
  robot: { robot_id?: string | null; group_id?: string | null; joint_names: string[] } | null;
  objects: SceneObjectState[];
  selectedId: string | null;
  onSelect: (id: string | null) => void;
  lockedIds: Set<string>;
}) {
  // Group scene objects by source so the operator can quickly find
  // authored vs runtime-only vs attached objects. Robot is its own
  // top-level group with its joint list as leaves.
  const groups = useMemo(() => {
    const buckets: Record<string, SceneObjectState[]> = {
      authored: [],
      runtime: [],
      attached: [],
      sensed: [],
    };
    for (const obj of objects) {
      const key = obj.source ?? 'runtime';
      if (key in buckets) buckets[key].push(obj);
      else buckets[key] = [obj];
    }
    return buckets;
  }, [objects]);

  return (
    <div className="space-y-2">
      {robot !== null && (
        <TreeGroup label={`Robot · ${robot.robot_id ?? '?'} (${robot.group_id ?? '?'})`}>
          <ul className="ml-3 space-y-0.5 text-[10px] text-gray-400">
            {robot.joint_names.map((joint) => (
              <li key={joint} className="truncate font-mono">{joint}</li>
            ))}
          </ul>
        </TreeGroup>
      )}
      {(['authored', 'runtime', 'attached', 'sensed'] as const).map((source) => {
        const items = groups[source];
        if (items.length === 0) return null;
        return (
          <TreeGroup key={source} label={`${source} (${items.length})`}>
            <ul className="ml-3 space-y-0.5">
              {items.map((obj) => (
                <TreeObjectRow
                  key={obj.object_id}
                  object={obj}
                  selected={obj.object_id === selectedId}
                  locked={lockedIds.has(obj.object_id)}
                  onSelect={onSelect}
                />
              ))}
            </ul>
          </TreeGroup>
        );
      })}
    </div>
  );
}

function TreeGroup({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div>
      <div className="text-[10px] font-semibold uppercase tracking-wide text-gray-500">
        {label}
      </div>
      {children}
    </div>
  );
}

function TreeObjectRow({
  object,
  selected,
  locked,
  onSelect,
}: {
  object: SceneObjectState;
  selected: boolean;
  locked: boolean;
  onSelect: (id: string | null) => void;
}) {
  return (
    <li>
      <button
        type="button"
        onClick={() => onSelect(selected ? null : object.object_id)}
        className={`block w-full truncate rounded px-1.5 py-0.5 text-left text-[11px] ${
          selected ? 'bg-gray-700 text-white' : 'text-gray-300 hover:bg-gray-800'
        }`}
      >
        <span className="font-mono">{object.object_id}</span>
        {locked && <span className="ml-1 text-[10px] text-orange-400">🔒</span>}
      </button>
    </li>
  );
}

function PoseSourcesSection({ sources }: { sources: PoseSourceStatusState[] }) {
  const [applyState, setApplyState] = useState<
    { status: 'idle' | 'applying' | 'ok' | 'error'; message: string }
  >({ status: 'idle', message: '' });
  const measuredStateGhostVisible = useViewerSelectionStore(
    (s) => s.measuredStateGhostVisible,
  );
  const setMeasuredStateGhostVisible = useViewerSelectionStore(
    (s) => s.setMeasuredStateGhostVisible,
  );

  if (sources.length === 0) {
    return (
      <div className="text-[11px] italic text-gray-500">
        No pose sources declared in deployment.
      </div>
    );
  }

  return (
    <div className="space-y-3">
      <div className="rounded border border-cyan-800/60 bg-cyan-950/20 p-2">
        <label className="flex items-center gap-2 text-[11px] text-gray-200">
          <input
            type="checkbox"
            checked={measuredStateGhostVisible}
            onChange={(event) => setMeasuredStateGhostVisible(event.target.checked)}
          />
          <span>Measured-state ghost</span>
        </label>
        <div className="mt-1 text-[10px] leading-snug text-gray-500">
          Shows fresh external object observations as a cyan translucent overlay.
          Composer's planning scene remains the authoritative state.
        </div>
      </div>
      <div className="rounded border border-gray-800 bg-gray-950/50 p-2">
        <button
          type="button"
          disabled={applyState.status === 'applying'}
          onClick={async () => {
            setApplyState({ status: 'applying', message: 'Applying idle-safe updates...' });
            try {
              const result = await api.scene.applyPoseSources('when_idle');
              await useSceneStateStore.getState().refresh();
              setApplyState({
                status: 'ok',
                message: `Applied ${result.applied_count}, skipped ${result.skipped_count}.`,
              });
            } catch (error) {
              setApplyState({
                status: 'error',
                message: error instanceof Error ? error.message : 'Pose-source apply failed',
              });
            }
          }}
          className="w-full rounded border border-gray-600 px-2 py-1 text-[11px] text-gray-200 hover:bg-gray-800 disabled:cursor-not-allowed disabled:opacity-50"
        >
          Apply when-idle updates
        </button>
        {applyState.message && (
          <div
            className={`mt-1 text-[10px] ${
              applyState.status === 'error' ? 'text-red-300' : 'text-gray-400'
            }`}
          >
            {applyState.message}
          </div>
        )}
      </div>
      {sources.map((source) => (
        <div key={source.pose_source_id} className="space-y-1">
          <div className="flex items-center justify-between gap-2">
            <div className="min-w-0 truncate font-mono text-[11px] text-gray-200">
              {source.pose_source_id}
            </div>
            <StatusPill status={source.status} />
          </div>
          <div className="truncate text-[10px] text-gray-500">
            {source.kind} · max {source.max_update_hz.toFixed(1)} Hz · stale{' '}
            {source.stale_after_s.toFixed(2)} s
            {source.measured_tcp && (
              <>
                {' '}· TCP age {formatMaybeSeconds(source.measured_tcp.age_s)}
              </>
            )}
          </div>
          <ul className="ml-2 space-y-0.5">
            {source.objects.map((objectState) => (
              <li key={objectState.object_id} className="space-y-0.5 text-[11px]">
                <div className="flex items-center gap-2">
                  <span className="min-w-0 flex-1 truncate font-mono text-gray-300">
                    {objectState.object_id}
                  </span>
                  <span className="truncate text-[10px] text-gray-500">
                    {objectState.apply_policy}
                  </span>
                  <StatusPill status={objectState.status} compact />
                </div>
                {objectState.attached_drift && (
                  <div className="ml-2 rounded border border-gray-800 bg-gray-950/60 px-2 py-1 text-[10px] text-gray-400">
                    <div className="flex items-center justify-between gap-2">
                      <span>Drift</span>
                      <StatusPill status={objectState.attached_drift.status} compact />
                    </div>
                    <div className="mt-0.5 grid grid-cols-2 gap-x-2 gap-y-0.5">
                      <span>dx {formatMaybeMeters(objectState.attached_drift.translation_m)}</span>
                      <span>dθ {formatMaybeRadians(objectState.attached_drift.rotation_rad)}</span>
                      <span>dt {formatMaybeSeconds(objectState.attached_drift.sample_time_delta_s)}</span>
                      <span>{objectState.attached_drift.action}</span>
                    </div>
                    {objectState.attached_drift.unverifiable_reason && (
                      <div className="mt-0.5 truncate text-yellow-300/80">
                        {objectState.attached_drift.unverifiable_reason}
                      </div>
                    )}
                  </div>
                )}
              </li>
            ))}
          </ul>
        </div>
      ))}
    </div>
  );
}

function formatMaybeMeters(value: number | null | undefined): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return '-';
  return `${value.toFixed(3)} m`;
}

function formatMaybeRadians(value: number | null | undefined): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return '-';
  return `${value.toFixed(3)} rad`;
}

function formatMaybeSeconds(value: number | null | undefined): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return '-';
  return `${value.toFixed(2)} s`;
}

function StatusPill({
  status,
  compact = false,
}: {
  status: string;
  compact?: boolean;
}) {
  const color =
    status === 'fresh'
      ? 'border-emerald-500/50 bg-emerald-950/80 text-emerald-300'
      : status === 'stale'
        ? 'border-amber-500/50 bg-amber-950/80 text-amber-300'
        : status === 'rejected'
          ? 'border-red-500/50 bg-red-950/80 text-red-300'
          : 'border-gray-600 bg-gray-900 text-gray-400';
  return (
    <span
      className={`shrink-0 rounded border px-1.5 py-0.5 uppercase tracking-wide ${color} ${
        compact ? 'text-[9px]' : 'text-[10px]'
      }`}
    >
      {status}
    </span>
  );
}
