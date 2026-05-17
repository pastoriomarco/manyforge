import { useMemo, useState } from 'react';

import { api } from '../api/client';
import { useSceneStateStore } from '../store/sceneStateStore';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import type { SceneFrameState, SceneObjectState } from '../types/api';

/**
 * Viser-parity controls for the native viewer (Day 4): frame axis
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
  const robot = useSceneStateStore((s) => s.scene?.robot ?? null);
  const selectedId = useViewerSelectionStore((s) => s.selectedId);
  const setSelected = useViewerSelectionStore((s) => s.setSelected);
  const lockedIds = useViewerSelectionStore((s) => s.lockedIds);
  const [openSection, setOpenSection] = useState<Section>('frames');

  return (
    <div className="pointer-events-auto absolute right-2 top-2 z-20 flex max-h-[calc(100%-1rem)] w-72 flex-col overflow-hidden rounded-md border border-gray-700 bg-gray-900/50 text-xs text-gray-200 shadow-xl">
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
      </div>
      <div className="flex-1 overflow-y-auto bg-gray-950/50 p-2">
        {openSection === 'frames' && (
          <FramesSection
            frames={frames?.frames ?? EMPTY_FRAMES}
            gizmoSize={frames?.frameGizmoSizeM ?? 0.1}
          />
        )}
        {openSection === 'objects' && <ObjectsSection objects={objects} />}
        {openSection === 'tree' && (
          <RuntimeTree
            robot={robot}
            objects={objects}
            selectedId={selectedId}
            onSelect={setSelected}
            lockedIds={lockedIds}
          />
        )}
      </div>
    </div>
  );
}

type Section = 'frames' | 'objects' | 'tree';

const EMPTY_FRAMES: SceneFrameState[] = [];
const EMPTY_OBJECTS: SceneObjectState[] = [];

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

function ObjectsSection({ objects }: { objects: SceneObjectState[] }) {
  if (objects.length === 0) {
    return <div className="text-[11px] italic text-gray-500">No scene objects.</div>;
  }
  return (
    <div className="space-y-0.5">
      {objects.map((obj) => (
        <ObjectFrameRow key={obj.object_id} object={obj} />
      ))}
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
