import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api } from '../api/client';
import { useCycleStore, useEditLocked } from '../store/cycleStore';
import { useProgramStore } from '../store/programStore';
import type {
  ProgramSceneResource,
  SceneEditRequest,
  SceneEditResponse,
  SceneFrameState,
  SceneObjectState,
  SceneStateResponse,
} from '../types/api';

const SCENE_MUTATED_EVENT = 'manyforge:scene-mutated';
const SCENE_SYNC_STATUS_EVENT = 'manyforge:scene-sync-status';
const SCENE_SYNC_ERROR_PREFIX = 'Live scene sync failed: ';
type SceneShapeType = 'box' | 'sphere' | 'mesh';

function emitSceneMutated(): void {
  window.dispatchEvent(new CustomEvent(SCENE_MUTATED_EVENT));
}

function emitSceneSyncStatus(error: string | null): void {
  window.dispatchEvent(new CustomEvent(SCENE_SYNC_STATUS_EVENT, { detail: { error } }));
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function assertSceneEditSuccess(response: SceneEditResponse): void {
  if (!response.success) {
    throw new Error(response.error ?? 'Scene edit failed');
  }
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div>
      <h3 className="mb-2 text-xs font-semibold uppercase tracking-wide text-gray-400">
        {title}
      </h3>
      <div className="space-y-2">{children}</div>
    </div>
  );
}

function NumericInput({
  label,
  value,
  onChange,
  step = '0.001',
  disabled = false,
}: {
  label: string;
  value: number;
  onChange: (value: number) => void;
  step?: string;
  disabled?: boolean;
}) {
  // Internally a draft-style input: local state during typing, commit on
  // blur/Enter, revert on Escape. Same pattern as NumericParamEditor in
  // ../property/PrimitiveParameterEditors.tsx — see DraftInputs.tsx for
  // the full rationale on skipNextCommit. Step is kept for visual hints
  // but ignored at parse time (we accept any finite number on commit).
  const [draft, setDraft] = useState(() => (Number.isFinite(value) ? String(value) : '0'));
  const [isFocused, setIsFocused] = useState(false);
  const skipNextCommit = useRef(false);

  useEffect(() => {
    if (isFocused) {
      return;
    }
    setDraft(Number.isFinite(value) ? String(value) : '0');
  }, [value, isFocused]);

  const commitDraft = () => {
    const trimmed = draft.trim();
    if (trimmed === '' || trimmed === '-' || trimmed === '+' || trimmed === '.' || trimmed === '-.' || trimmed === '+.') {
      setDraft(Number.isFinite(value) ? String(value) : '0');
      return;
    }
    const next = Number(trimmed);
    if (!Number.isFinite(next)) {
      setDraft(Number.isFinite(value) ? String(value) : '0');
      return;
    }
    if (next !== value) {
      onChange(next);
    }
    setDraft(String(next));
  };

  return (
    <label className="block">
      <span className="text-[11px] text-gray-400">{label}</span>
      <input
        type="text"
        inputMode="decimal"
        step={step}
        value={draft}
        onFocus={() => setIsFocused(true)}
        onChange={(event) => setDraft(event.target.value)}
        onBlur={() => {
          setIsFocused(false);
          if (skipNextCommit.current) {
            skipNextCommit.current = false;
            setDraft(Number.isFinite(value) ? String(value) : '0');
            return;
          }
          commitDraft();
        }}
        onKeyDown={(event) => {
          if (event.key === 'Enter') {
            event.preventDefault();
            event.currentTarget.blur();
            return;
          }
          if (event.key === 'Escape') {
            event.preventDefault();
            skipNextCommit.current = true;
            setDraft(Number.isFinite(value) ? String(value) : '0');
            event.currentTarget.blur();
          }
        }}
        disabled={disabled}
        className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
      />
    </label>
  );
}

function TextInput({
  label,
  value,
  onChange,
  disabled = false,
  placeholder,
}: {
  label: string;
  value: string;
  onChange: (value: string) => void;
  disabled?: boolean;
  placeholder?: string;
}) {
  // Same draft-style pattern as NumericInput above. The component owns
  // the local draft while focused so per-keystroke renders don't
  // cascade into the program store and every other panel subscriber.
  const [draft, setDraft] = useState(value);
  const [isFocused, setIsFocused] = useState(false);
  const skipNextCommit = useRef(false);

  useEffect(() => {
    if (isFocused) {
      return;
    }
    setDraft(value);
  }, [value, isFocused]);

  return (
    <label className="block">
      <span className="text-[11px] text-gray-400">{label}</span>
      <input
        type="text"
        value={draft}
        onFocus={() => setIsFocused(true)}
        onChange={(event) => setDraft(event.target.value)}
        onBlur={() => {
          setIsFocused(false);
          if (skipNextCommit.current) {
            skipNextCommit.current = false;
            setDraft(value);
            return;
          }
          if (draft !== value) {
            onChange(draft);
          }
        }}
        onKeyDown={(event) => {
          if (event.key === 'Enter') {
            event.preventDefault();
            event.currentTarget.blur();
            return;
          }
          if (event.key === 'Escape') {
            event.preventDefault();
            skipNextCommit.current = true;
            setDraft(value);
            event.currentTarget.blur();
          }
        }}
        disabled={disabled}
        placeholder={placeholder}
        className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
      />
    </label>
  );
}

function readPoseVector(
  pose: Record<string, unknown> | null | undefined,
  key: 'position_m' | 'orientation_xyzw' | 'orientation_rpy_deg',
  expectedLength: number,
): number[] | null {
  const raw = pose?.[key];
  if (!Array.isArray(raw)) {
    return null;
  }
  const values = raw.map((entry) => Number(entry));
  if (values.length !== expectedLength || !values.every((entry) => Number.isFinite(entry))) {
    console.warn(`Invalid pose vector for ${key}; expected ${expectedLength} finite values`, raw);
    return null;
  }
  return values;
}

function buildPoseOrientationQuaternion(
  pose: Record<string, unknown> | null | undefined,
): number[] {
  const quaternion = readPoseVector(pose, 'orientation_xyzw', 4);
  if (quaternion != null) {
    return quaternion;
  }

  const rpyDegrees = readPoseVector(pose, 'orientation_rpy_deg', 3);
  if (rpyDegrees != null) {
    return rpyDegreesToQuaternion(rpyDegrees);
  }

  return [0, 0, 0, 1];
}

function degToRad(value: number): number {
  return (value * Math.PI) / 180.0;
}

function radToDeg(value: number): number {
  return (value * 180.0) / Math.PI;
}

function quaternionToRpyDegrees(quaternion: number[]): number[] {
  const [x, y, z, w] = quaternion;

  const sinrCosp = 2 * (w * x + y * z);
  const cosrCosp = 1 - 2 * (x * x + y * y);
  const roll = Math.atan2(sinrCosp, cosrCosp);

  const sinp = 2 * (w * y - z * x);
  const pitch = Math.abs(sinp) >= 1 ? Math.sign(sinp) * Math.PI / 2 : Math.asin(sinp);

  const sinyCosp = 2 * (w * z + x * y);
  const cosyCosp = 1 - 2 * (y * y + z * z);
  const yaw = Math.atan2(sinyCosp, cosyCosp);

  return [roll, pitch, yaw].map((value) => Number(radToDeg(value).toFixed(6)));
}

function rpyDegreesToQuaternion(rpyDegrees: number[]): number[] {
  const [roll, pitch, yaw] = rpyDegrees.map(degToRad);
  const cy = Math.cos(yaw * 0.5);
  const sy = Math.sin(yaw * 0.5);
  const cp = Math.cos(pitch * 0.5);
  const sp = Math.sin(pitch * 0.5);
  const cr = Math.cos(roll * 0.5);
  const sr = Math.sin(roll * 0.5);

  const w = cr * cp * cy + sr * sp * sy;
  const x = sr * cp * cy - cr * sp * sy;
  const y = cr * sp * cy + sr * cp * sy;
  const z = cr * cp * sy - sr * sp * cy;
  return [x, y, z, w];
}

function sceneResourcePose(resource: ProgramSceneResource): {
  frameId: string;
  position: number[];
  rpyDegrees: number[];
} {
  const pose = resource.pose ?? {};
  return {
    frameId: typeof pose.frame_id === 'string' && pose.frame_id.trim() ? pose.frame_id : 'base_link',
    position: readPoseVector(pose, 'position_m', 3) ?? [0, 0, 0],
    rpyDegrees: quaternionToRpyDegrees(buildPoseOrientationQuaternion(pose)),
  };
}

function buildSceneResource(
  objectId: string,
  shape: Record<string, unknown>,
  frameId: string,
  position: number[],
  rpyDegrees: number[],
): ProgramSceneResource {
  return {
    objectId,
    shape,
    pose: {
      frame_id: frameId,
      position_m: [...position],
      orientation_xyzw: rpyDegreesToQuaternion(rpyDegrees),
    },
  };
}

function shapeSummary(shape: Record<string, unknown> | null | undefined): string {
  if (!shape?.type) {
    return 'unknown';
  }
  if (shape.type === 'box') {
    const dims = Array.isArray(shape.box_dimensions_m)
      ? shape.box_dimensions_m.map((value) => Number(value).toFixed(3)).join(', ')
      : '?';
    return `box (${dims})`;
  }
  if (shape.type === 'sphere') {
    const radius = Number(shape.sphere_radius_m ?? 0).toFixed(3);
    return `sphere (r=${radius})`;
  }
  if (shape.type === 'mesh') {
    const uri = typeof shape.mesh_resource_uri === 'string' ? shape.mesh_resource_uri : '?';
    return `mesh (${uri})`;
  }
  return String(shape.type);
}

function sameShape(
  liveShape: Record<string, unknown> | null | undefined,
  desiredShape: Record<string, unknown> | null | undefined,
): boolean {
  return JSON.stringify(liveShape ?? null) === JSON.stringify(desiredShape ?? null);
}

function buildAddPayload(resource: ProgramSceneResource): SceneEditRequest {
  const pose = sceneResourcePose(resource);
  const shape = resource.shape ?? {};
  const shapeType = shape.type === 'sphere' || shape.type === 'mesh' ? shape.type : 'box';
  return {
    operation: 'add',
    object_id: resource.objectId,
    shape_type: shapeType,
    box_dimensions_m:
      shape.type === 'box' && Array.isArray(shape.box_dimensions_m)
        ? (shape.box_dimensions_m as number[])
        : undefined,
    sphere_radius_m:
      shape.type === 'sphere' && Number.isFinite(Number(shape.sphere_radius_m))
        ? Number(shape.sphere_radius_m)
        : undefined,
    mesh_resource_uri:
      shape.type === 'mesh' && typeof shape.mesh_resource_uri === 'string'
        ? shape.mesh_resource_uri
        : undefined,
    mesh_scale:
      shape.type === 'mesh' && Array.isArray(shape.mesh_scale)
        ? (shape.mesh_scale as number[])
        : undefined,
    pose_position_m: pose.position,
    pose_orientation_xyzw: buildPoseOrientationQuaternion(resource.pose ?? {}),
    frame_id: pose.frameId,
  };
}

function buildUpdatePosePayload(resource: ProgramSceneResource): SceneEditRequest {
  const pose = sceneResourcePose(resource);
  return {
    operation: 'update_pose',
    object_id: resource.objectId,
    pose_position_m: pose.position,
    pose_orientation_xyzw: buildPoseOrientationQuaternion(resource.pose ?? {}),
    frame_id: pose.frameId,
  };
}

function authoredSceneSignature(sceneResources: ProgramSceneResource[]): string {
  return JSON.stringify(sceneResources);
}

function sourceBadge(source: SceneObjectState['source']): { label: string; classes: string } {
  if (source === 'authored') {
    return { label: 'AUTHORED', classes: 'bg-emerald-900/50 text-emerald-200 border-emerald-700' };
  }
  if (source === 'attached') {
    return { label: 'ATTACHED', classes: 'bg-violet-900/50 text-violet-200 border-violet-700' };
  }
  if (source === 'sensed') {
    return { label: 'SENSED', classes: 'bg-sky-900/50 text-sky-200 border-sky-700' };
  }
  return { label: 'RUNTIME', classes: 'bg-amber-900/50 text-amber-200 border-amber-700' };
}

function AuthoredSceneCard({
  resource,
  frameOptions,
  disabled,
  hmiAvailable,
  objectFrameVisible,
  onUpdate,
  onRemove,
  onObjectFrameVisibilityChange,
}: {
  resource: ProgramSceneResource;
  frameOptions: string[];
  disabled: boolean;
  hmiAvailable: boolean;
  objectFrameVisible: boolean;
  onUpdate: (previousObjectId: string, resource: ProgramSceneResource) => void;
  onRemove: (objectId: string) => void;
  onObjectFrameVisibilityChange: (objectId: string, visible: boolean) => void;
}) {
  const resourceSignature = JSON.stringify(resource);
  const initialPose = sceneResourcePose(resource);
  const [objectId, setObjectId] = useState(resource.objectId);
  const [shapeType, setShapeType] = useState<SceneShapeType>(
    resource.shape?.type === 'sphere' || resource.shape?.type === 'mesh'
      ? resource.shape.type
      : 'box',
  );
  const [boxDimensions, setBoxDimensions] = useState<number[]>(
    Array.isArray(resource.shape?.box_dimensions_m)
      ? (resource.shape.box_dimensions_m as number[]).map((value) => Number(value))
      : [0.05, 0.05, 0.1],
  );
  const [sphereRadius, setSphereRadius] = useState<number>(
    Number(resource.shape?.sphere_radius_m ?? 0.05),
  );
  const [meshResourceUri, setMeshResourceUri] = useState<string>(
    typeof resource.shape?.mesh_resource_uri === 'string' ? resource.shape.mesh_resource_uri : '',
  );
  const [meshScale, setMeshScale] = useState<number[]>(
    Array.isArray(resource.shape?.mesh_scale)
      ? (resource.shape.mesh_scale as number[]).map((value) => Number(value))
      : [1, 1, 1],
  );
  const [frameId, setFrameId] = useState(initialPose.frameId);
  const [position, setPosition] = useState(initialPose.position);
  const [rpyDegrees, setRpyDegrees] = useState(initialPose.rpyDegrees);

  useEffect(() => {
    const pose = sceneResourcePose(resource);
    setObjectId(resource.objectId);
    setShapeType(
      resource.shape?.type === 'sphere' || resource.shape?.type === 'mesh'
        ? resource.shape.type
        : 'box',
    );
    setBoxDimensions(
      Array.isArray(resource.shape?.box_dimensions_m)
        ? (resource.shape.box_dimensions_m as number[]).map((value) => Number(value))
        : [0.05, 0.05, 0.1],
    );
    setSphereRadius(Number(resource.shape?.sphere_radius_m ?? 0.05));
    setMeshResourceUri(
      typeof resource.shape?.mesh_resource_uri === 'string' ? resource.shape.mesh_resource_uri : '',
    );
    setMeshScale(
      Array.isArray(resource.shape?.mesh_scale)
        ? (resource.shape.mesh_scale as number[]).map((value) => Number(value))
        : [1, 1, 1],
    );
    setFrameId(pose.frameId);
    setPosition(pose.position);
    setRpyDegrees(pose.rpyDegrees);
  }, [resourceSignature]);

  const commit = useCallback(() => {
    const shape =
      shapeType === 'box'
        ? { type: 'box', box_dimensions_m: boxDimensions.map((value) => Number(value)) }
        : shapeType === 'sphere'
          ? { type: 'sphere', sphere_radius_m: Number(sphereRadius) }
          : {
              type: 'mesh',
              mesh_resource_uri: meshResourceUri.trim(),
              mesh_scale: meshScale.map((value) => Number(value)),
            };
    onUpdate(resource.objectId, buildSceneResource(objectId.trim(), shape, frameId, position, rpyDegrees));
  }, [boxDimensions, frameId, meshResourceUri, meshScale, objectId, onUpdate, position, resource.objectId, rpyDegrees, shapeType, sphereRadius]);

  return (
    <div className="rounded border border-gray-700 bg-gray-900/40 p-3">
      <div className="mb-2 flex items-start justify-between gap-2">
        <div>
          <div className="text-sm font-medium text-gray-100">{resource.objectId}</div>
          <div className="text-[11px] text-gray-500">{shapeSummary(resource.shape)}</div>
        </div>
        <div className="flex flex-col items-end gap-2">
          <button
            type="button"
            onClick={() => onRemove(resource.objectId)}
            disabled={disabled}
            className="rounded border border-red-700 px-2 py-1 text-xs text-red-200 hover:bg-red-900/40 disabled:cursor-not-allowed disabled:opacity-50"
          >
            Remove
          </button>
          <label className="flex items-center gap-2 text-[11px] text-gray-400">
            <input
              type="checkbox"
              checked={objectFrameVisible}
              onChange={(event) => onObjectFrameVisibilityChange(resource.objectId, event.target.checked)}
              disabled={!hmiAvailable}
              className="h-3 w-3"
            />
            <span>Gizmo frame</span>
          </label>
        </div>
      </div>

      <TextInput
        label="Object ID"
        value={objectId}
        onChange={setObjectId}
        disabled={disabled}
      />

      <label className="mt-2 block">
        <span className="text-[11px] text-gray-400">Shape</span>
        <select
          value={shapeType}
          onChange={(event) => setShapeType(event.target.value as SceneShapeType)}
          disabled={disabled}
          className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
        >
          <option value="box">box</option>
          <option value="sphere">sphere</option>
          <option value="mesh">mesh</option>
        </select>
      </label>

      {shapeType === 'box' ? (
        <div className="mt-2 grid grid-cols-3 gap-2">
          {boxDimensions.map((value, index) => (
            <NumericInput
              key={`box-${resource.objectId}-${index}`}
              label={`Dim ${['x', 'y', 'z'][index]}`}
              value={value}
              onChange={(next) => {
                const updated = [...boxDimensions];
                updated[index] = next;
                setBoxDimensions(updated);
              }}
              disabled={disabled}
            />
          ))}
        </div>
      ) : shapeType === 'sphere' ? (
        <div className="mt-2">
          <NumericInput
            label="Radius"
            value={sphereRadius}
            onChange={setSphereRadius}
            disabled={disabled}
          />
        </div>
      ) : (
        <div className="mt-2 space-y-2">
          <TextInput
            label="Mesh URI"
            value={meshResourceUri}
            onChange={setMeshResourceUri}
            disabled={disabled}
          />
          <div className="grid grid-cols-3 gap-2">
            {meshScale.map((value, index) => (
              <NumericInput
                key={`mesh-scale-${resource.objectId}-${index}`}
                label={`Scale ${['x', 'y', 'z'][index]}`}
                value={value}
                onChange={(next) => {
                  const updated = [...meshScale];
                  updated[index] = next;
                  setMeshScale(updated);
                }}
                disabled={disabled}
              />
            ))}
          </div>
        </div>
      )}

      <label className="mt-2 block">
        <span className="text-[11px] text-gray-400">Reference Frame</span>
        <select
          value={frameId}
          onChange={(event) => setFrameId(event.target.value)}
          disabled={disabled}
          className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
        >
          {frameOptions.map((frameName) => (
            <option key={frameName} value={frameName}>
              {frameName}
            </option>
          ))}
        </select>
      </label>

      <div className="mt-2 grid grid-cols-3 gap-2">
        {position.map((value, index) => (
          <NumericInput
            key={`pos-${resource.objectId}-${index}`}
            label={`P${['x', 'y', 'z'][index]}`}
            value={value}
            onChange={(next) => {
              const updated = [...position];
              updated[index] = next;
              setPosition(updated);
            }}
            disabled={disabled}
          />
        ))}
      </div>

      <div className="mt-2 grid grid-cols-3 gap-2">
        {rpyDegrees.map((value, index) => (
          <NumericInput
            key={`rpy-${resource.objectId}-${index}`}
            label={['Roll°', 'Pitch°', 'Yaw°'][index]}
            value={value}
            onChange={(next) => {
              const updated = [...rpyDegrees];
              updated[index] = next;
              setRpyDegrees(updated);
            }}
            step="1"
            disabled={disabled}
          />
        ))}
      </div>

      <button
        type="button"
        onClick={commit}
        disabled={disabled || !objectId.trim()}
        className="mt-2 w-full rounded border border-gray-600 px-3 py-2 text-xs text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
      >
        Update Program Scene Draft
      </button>
    </div>
  );
}

function LiveSceneObjectCard({
  objectState,
  hmiAvailable,
  onObjectFrameVisibilityChange,
}: {
  objectState: SceneObjectState;
  hmiAvailable: boolean;
  onObjectFrameVisibilityChange: (objectId: string, visible: boolean) => void;
}) {
  const pose = objectState.pose_in_universe as Record<string, unknown> | undefined;
  const position = readPoseVector(pose, 'position_m', 3) ?? [0, 0, 0];
  const rpyDegrees = quaternionToRpyDegrees(
    readPoseVector(pose, 'orientation_xyzw', 4) ?? [0, 0, 0, 1],
  );
  const badge = sourceBadge(objectState.source);

  return (
    <div className="rounded border border-gray-700 bg-gray-900/40 p-3">
      <div className="mb-2 flex items-start justify-between gap-2">
        <div>
          <div className="text-sm font-medium text-gray-100">{objectState.object_id}</div>
          <div className="text-[11px] text-gray-500">{shapeSummary(objectState.shape as Record<string, unknown> | null | undefined)}</div>
        </div>
        <div className="flex flex-col items-end gap-2">
          <span className={`rounded border px-2 py-1 text-[10px] font-semibold tracking-wide ${badge.classes}`}>
            {badge.label}
          </span>
          <label className="flex items-center gap-2 text-[11px] text-gray-400">
            <input
              type="checkbox"
              checked={objectState.viewer_frame_visible}
              onChange={(event) => onObjectFrameVisibilityChange(objectState.object_id, event.target.checked)}
              disabled={!hmiAvailable}
              className="h-3 w-3"
            />
            <span>Gizmo frame</span>
          </label>
        </div>
      </div>
      <div className="space-y-1 text-[11px] text-gray-400">
        <div>Persistable: {objectState.persistable ? 'yes' : 'no'}</div>
        <div>
          Attachment:{' '}
          {objectState.attached && objectState.attached_to
            ? `attached to ${objectState.attached_to}`
            : 'detached'}
        </div>
        <div>
          Position: [{position.map((value) => value.toFixed(3)).join(', ')}]
        </div>
        <div>
          RPY°: [{rpyDegrees.map((value) => value.toFixed(1)).join(', ')}]
        </div>
      </div>
    </div>
  );
}

export function SceneDraftMirror() {
  const cycleState = useCycleStore((state) => state.state);
  const programInfo = useProgramStore((state) => state.programInfo);
  const loading = useProgramStore((state) => state.loading);
  const sceneResources = useProgramStore((state) => state.sceneResources);
  const setProgramError = useProgramStore((state) => state.setError);
  const sceneSignature = useMemo(() => authoredSceneSignature(sceneResources), [sceneResources]);
  const inFlightRef = useRef(false);
  const lastMirroredKeyRef = useRef<string | null>(null);

  useEffect(() => {
    const safeToMirror = cycleState === 'idle' || cycleState === 'stopped';
    // Do not treat the store's default empty scene as authoritative before the
    // loaded program draft has hydrated, or we can wipe the backend-seeded
    // authored scene on first load.
    const hasLoadedProgramScene = programInfo !== null && !loading;
    const mirrorKey = hasLoadedProgramScene
      ? `${programInfo?.sourcePath ?? '<memory>'}::${sceneSignature}`
      : null;
    if (!safeToMirror || !hasLoadedProgramScene || inFlightRef.current || mirrorKey === lastMirroredKeyRef.current) {
      return;
    }

    let cancelled = false;
    const sync = async () => {
      inFlightRef.current = true;
      try {
        const liveState = await api.scene.state();
        if (cancelled) {
          return;
        }

        const desiredById = new Map(sceneResources.map((resource) => [resource.objectId, resource]));
        const liveById = new Map(liveState.objects.map((objectState) => [objectState.object_id, objectState]));

        for (const objectState of liveState.objects) {
          if (objectState.source === 'authored' && !desiredById.has(objectState.object_id)) {
            assertSceneEditSuccess(
              await api.scene.edit({ operation: 'remove', object_id: objectState.object_id }),
            );
          }
        }

        for (const resource of sceneResources) {
          const liveObject = liveById.get(resource.objectId);
          if (liveObject == null) {
            assertSceneEditSuccess(await api.scene.edit(buildAddPayload(resource)));
            continue;
          }

          const requiresRecreate =
            liveObject.attached
            || liveObject.source !== 'authored'
            || !sameShape(liveObject.shape as Record<string, unknown> | null | undefined, resource.shape);

          if (requiresRecreate) {
            assertSceneEditSuccess(
              await api.scene.edit({ operation: 'remove', object_id: resource.objectId }),
            );
            assertSceneEditSuccess(await api.scene.edit(buildAddPayload(resource)));
            continue;
          }

          assertSceneEditSuccess(await api.scene.edit(buildUpdatePosePayload(resource)));
        }

        emitSceneMutated();
        emitSceneSyncStatus(null);
        if (useProgramStore.getState().error?.startsWith(SCENE_SYNC_ERROR_PREFIX)) {
          setProgramError(null);
        }
        lastMirroredKeyRef.current = mirrorKey;
      } catch (error) {
        const message = `${SCENE_SYNC_ERROR_PREFIX}${errorMessage(error)}`;
        emitSceneSyncStatus(message);
        setProgramError(message);
        console.error('Failed to mirror authored scene into live scene', error);
      } finally {
        inFlightRef.current = false;
      }
    };

    void sync();
    return () => {
      cancelled = true;
    };
  }, [cycleState, loading, programInfo, sceneResources, sceneSignature, setProgramError]);

  return null;
}

export function ScenePanel() {
  const sceneResources = useProgramStore((state) => state.sceneResources);
  const setSceneResources = useProgramStore((state) => state.setSceneResources);
  const editLocked = useEditLocked();
  const [sceneState, setSceneState] = useState<SceneStateResponse | null>(null);
  const [frames, setFrames] = useState<SceneFrameState[]>([]);
  const [hmiAvailable, setHmiAvailable] = useState(false);
  const [frameGizmoSizeM, setFrameGizmoSizeM] = useState(0.1);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [syncError, setSyncError] = useState<string | null>(null);
  const [newObjectId, setNewObjectId] = useState('');
  const [newShapeType, setNewShapeType] = useState<SceneShapeType>('box');
  const [newBoxDimensions, setNewBoxDimensions] = useState<number[]>([0.05, 0.05, 0.1]);
  const [newSphereRadius, setNewSphereRadius] = useState(0.05);
  const [newMeshResourceUri, setNewMeshResourceUri] = useState('');
  const [newMeshScale, setNewMeshScale] = useState<number[]>([1, 1, 1]);
  const [newFrameId, setNewFrameId] = useState('base_link');
  const [newPosition, setNewPosition] = useState<number[]>([0.4, 0.0, 0.1]);
  const [newRpyDegrees, setNewRpyDegrees] = useState<number[]>([0, 0, 0]);
  const [newObjectOpen, setNewObjectOpen] = useState(false);

  const refresh = useCallback(async () => {
    setLoading(true);
    try {
      const [nextState, frameState] = await Promise.all([
        api.scene.state(),
        api.scene.frames(),
      ]);
      setSceneState(nextState);
      setFrames(frameState.frames);
      setHmiAvailable(frameState.hmiAvailable);
      setFrameGizmoSizeM(frameState.frameGizmoSizeM ?? 0.1);
      setError(null);
    } catch (refreshError) {
      setError((refreshError as Error).message);
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  useEffect(() => {
    const handle = () => {
      void refresh();
    };
    window.addEventListener(SCENE_MUTATED_EVENT, handle);
    return () => {
      window.removeEventListener(SCENE_MUTATED_EVENT, handle);
    };
  }, [refresh]);

  useEffect(() => {
    const handle = (event: Event) => {
      const detail = (event as CustomEvent<{ error?: string | null }>).detail;
      setSyncError(detail?.error ?? null);
    };
    window.addEventListener(SCENE_SYNC_STATUS_EVENT, handle);
    return () => {
      window.removeEventListener(SCENE_SYNC_STATUS_EVENT, handle);
    };
  }, []);

  const frameOptions = useMemo(() => {
    const names = new Set<string>(['universe', 'base_link', 'tool0']);
    frames.forEach((frame) => names.add(frame.name));
    return Array.from(names).sort();
  }, [frames]);
  const liveObjectsById = useMemo(
    () => new Map((sceneState?.objects ?? []).map((objectState) => [objectState.object_id, objectState])),
    [sceneState],
  );

  const replaceSceneResource = useCallback(
    (previousObjectId: string, updated: ProgramSceneResource) => {
      const nextScene = sceneResources.filter(
        (resource) => resource.objectId !== previousObjectId && resource.objectId !== updated.objectId,
      );
      nextScene.push(updated);
      setSceneResources(nextScene);
      setError(null);
    },
    [sceneResources, setSceneResources],
  );

  const removeSceneResource = useCallback(
    (objectId: string) => {
      setSceneResources(sceneResources.filter((resource) => resource.objectId !== objectId));
      setError(null);
    },
    [sceneResources, setSceneResources],
  );

  const addSceneResource = useCallback(() => {
    const shape =
      newShapeType === 'box'
        ? { type: 'box', box_dimensions_m: newBoxDimensions.map((value) => Number(value)) }
        : newShapeType === 'sphere'
          ? { type: 'sphere', sphere_radius_m: Number(newSphereRadius) }
          : {
              type: 'mesh',
              mesh_resource_uri: newMeshResourceUri.trim(),
              mesh_scale: newMeshScale.map((value) => Number(value)),
            };
    const nextResource = buildSceneResource(newObjectId.trim(), shape, newFrameId, newPosition, newRpyDegrees);
    const nextScene = sceneResources.filter((resource) => resource.objectId !== nextResource.objectId);
    nextScene.push(nextResource);
    setSceneResources(nextScene);
    setNewObjectId('');
    setNewObjectOpen(false);
    setError(null);
  }, [
    newBoxDimensions,
    newFrameId,
    newMeshResourceUri,
    newMeshScale,
    newObjectId,
    newPosition,
    newRpyDegrees,
    newShapeType,
    newSphereRadius,
    sceneResources,
    setSceneResources,
  ]);

  const toggleFrameVisibility = useCallback(
    async (frameName: string, visible: boolean) => {
      setError(null);
      try {
        await api.scene.setFrameVisibility(frameName, visible);
        await refresh();
      } catch (toggleError) {
        setError((toggleError as Error).message);
      }
    },
    [refresh],
  );

  const updateFrameGizmoSize = useCallback(
    async (axesLengthM: number) => {
      if (!Number.isFinite(axesLengthM) || axesLengthM <= 0) {
        return;
      }
      setFrameGizmoSizeM(axesLengthM);
      setError(null);
      try {
        await api.scene.setFrameGizmoSize(axesLengthM);
        await refresh();
      } catch (sizeError) {
        setError((sizeError as Error).message);
      }
    },
    [refresh],
  );

  const toggleObjectFrameVisibility = useCallback(
    async (objectId: string, visible: boolean) => {
      setError(null);
      try {
        await api.scene.setObjectFrameVisibility(objectId, visible);
        await refresh();
      } catch (toggleError) {
        setError((toggleError as Error).message);
      }
    },
    [refresh],
  );

  return (
    <div className="flex-1 overflow-y-auto p-3 space-y-4">
      <div className="rounded border border-gray-700 bg-gray-900/60 p-3 text-xs text-gray-300 space-y-1">
        <div className="font-medium text-gray-100">Program Scene</div>
        <div>
          Scene edits in this tab change the authored program scene draft. While the cycle is idle or
          stopped, those authored changes are mirrored into the live scene immediately.
        </div>
        <div>
          Global <span className="font-mono text-gray-100">Apply</span>,{' '}
          <span className="font-mono text-gray-100">Discard</span>,{' '}
          <span className="font-mono text-gray-100">Revert</span>, and{' '}
          <span className="font-mono text-gray-100">Save</span> include scene changes too.
        </div>
        <div>
          Manual scene editing is disabled while running. Runtime behavior edits still appear below as
          live/runtime state and are not persisted automatically.
        </div>
        <div>
          Authored scene objects:{' '}
          <span className="font-mono text-gray-100">{sceneResources.length}</span>
        </div>
        {editLocked && (
          <div className="rounded bg-yellow-800 px-2 py-1 font-mono text-yellow-100">
            PROGRAM SCENE EDITING DISABLED WHILE EXECUTION IS ACTIVE
          </div>
        )}
        {error && (
          <div className="rounded bg-red-900/60 px-2 py-1 text-red-200">{error}</div>
        )}
        {syncError && (
          <div className="rounded bg-red-900/60 px-2 py-1 text-red-200">{syncError}</div>
        )}
      </div>

      <div className="flex gap-2">
        <button
          type="button"
          onClick={() => void refresh()}
          disabled={loading}
          className="flex-1 rounded border border-gray-600 px-3 py-2 text-sm text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
        >
          Refresh Live Scene
        </button>
      </div>

      <Section title="Viewer Frames">
        {!hmiAvailable && (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
            HMI frame toggles require the Viser HMI to be running.
          </div>
        )}
        <div className="rounded border border-gray-700 bg-gray-900/40 p-3">
          <NumericInput
            label="Frame gizmo size (m)"
            value={frameGizmoSizeM}
            onChange={(next) => void updateFrameGizmoSize(next)}
            step="0.01"
            disabled={!hmiAvailable}
          />
        </div>
        <div className="space-y-2">
          {frames.length === 0 && (
            <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
              No frames discovered yet.
            </div>
          )}
          {frames.map((frame) => (
            <label
              key={frame.name}
              className="flex items-start gap-3 rounded border border-gray-700 bg-gray-900/40 px-3 py-2"
            >
              <input
                type="checkbox"
                checked={frame.viewerVisible}
                onChange={(event) => void toggleFrameVisibility(frame.name, event.target.checked)}
                disabled={!hmiAvailable}
                className="mt-0.5"
              />
              <div className="min-w-0 flex-1">
                <div className="text-sm text-gray-100">{frame.name}</div>
                <div className="text-[11px] text-gray-500">
                  {frame.roles.length > 0 ? frame.roles.join(', ') : 'model frame'}
                  {frame.poseAvailable ? ' • pose available' : ' • pose unavailable'}
                </div>
              </div>
            </label>
          ))}
        </div>
      </Section>

      <Section title="Add Program Scene Primitive">
        {!newObjectOpen ? (
          <button
            type="button"
            onClick={() => setNewObjectOpen(true)}
            disabled={editLocked}
            className="w-full rounded border border-gray-600 px-3 py-2 text-sm text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
          >
            New Object
          </button>
        ) : (
          <div className="space-y-2 rounded border border-gray-700 bg-gray-900/40 p-3">
            <TextInput
              label="Object ID"
              value={newObjectId}
              onChange={setNewObjectId}
              disabled={editLocked}
              placeholder="new_box"
            />
            <label className="block">
              <span className="text-[11px] text-gray-400">Shape</span>
              <select
                value={newShapeType}
                onChange={(event) => setNewShapeType(event.target.value as SceneShapeType)}
                disabled={editLocked}
                className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
              >
                <option value="box">box</option>
                <option value="sphere">sphere</option>
                <option value="mesh">mesh</option>
              </select>
            </label>
            {newShapeType === 'box' ? (
              <div className="grid grid-cols-3 gap-2">
                {newBoxDimensions.map((value, index) => (
                  <NumericInput
                    key={`new-box-${index}`}
                    label={`Dim ${['x', 'y', 'z'][index]}`}
                    value={value}
                    onChange={(next) => {
                      const updated = [...newBoxDimensions];
                      updated[index] = next;
                      setNewBoxDimensions(updated);
                    }}
                    disabled={editLocked}
                  />
                ))}
              </div>
            ) : newShapeType === 'sphere' ? (
              <NumericInput
                label="Radius"
                value={newSphereRadius}
                onChange={setNewSphereRadius}
                disabled={editLocked}
              />
            ) : (
              <div className="space-y-2">
                <TextInput
                  label="Mesh URI"
                  value={newMeshResourceUri}
                  onChange={setNewMeshResourceUri}
                  disabled={editLocked}
                  placeholder="file:///workspace/scene/part.obj"
                />
                <div className="grid grid-cols-3 gap-2">
                  {newMeshScale.map((value, index) => (
                    <NumericInput
                      key={`new-mesh-scale-${index}`}
                      label={`Scale ${['x', 'y', 'z'][index]}`}
                      value={value}
                      onChange={(next) => {
                        const updated = [...newMeshScale];
                        updated[index] = next;
                        setNewMeshScale(updated);
                      }}
                      disabled={editLocked}
                    />
                  ))}
                </div>
              </div>
            )}
            <label className="block">
              <span className="text-[11px] text-gray-400">Reference Frame</span>
              <select
                value={newFrameId}
                onChange={(event) => setNewFrameId(event.target.value)}
                disabled={editLocked}
                className="mt-0.5 w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-gray-200 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
              >
                {frameOptions.map((frameName) => (
                  <option key={frameName} value={frameName}>
                    {frameName}
                  </option>
                ))}
              </select>
            </label>
            <div className="grid grid-cols-3 gap-2">
              {newPosition.map((value, index) => (
                <NumericInput
                  key={`new-pos-${index}`}
                  label={`P${['x', 'y', 'z'][index]}`}
                  value={value}
                  onChange={(next) => {
                    const updated = [...newPosition];
                    updated[index] = next;
                    setNewPosition(updated);
                  }}
                  disabled={editLocked}
                />
              ))}
            </div>
            <div className="grid grid-cols-3 gap-2">
              {newRpyDegrees.map((value, index) => (
                <NumericInput
                  key={`new-rpy-${index}`}
                  label={['Roll°', 'Pitch°', 'Yaw°'][index]}
                  value={value}
                  onChange={(next) => {
                    const updated = [...newRpyDegrees];
                    updated[index] = next;
                    setNewRpyDegrees(updated);
                  }}
                  step="1"
                  disabled={editLocked}
                />
              ))}
            </div>
            <div className="flex gap-2">
              <button
                type="button"
                onClick={addSceneResource}
                disabled={editLocked || !newObjectId.trim()}
                className="flex-1 rounded border border-gray-600 px-3 py-2 text-sm text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
              >
                Add To Program Scene Draft
              </button>
              <button
                type="button"
                onClick={() => setNewObjectOpen(false)}
                disabled={editLocked}
                className="rounded border border-gray-600 px-3 py-2 text-sm text-gray-300 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
              >
                Cancel
              </button>
            </div>
          </div>
        )}
      </Section>

      <Section title="Program Scene Objects">
        {sceneResources.length === 0 && (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
            No authored scene objects in the current program draft.
          </div>
        )}
        {sceneResources.map((resource) => (
          <AuthoredSceneCard
            key={`${resource.objectId}:${JSON.stringify(resource.shape)}:${JSON.stringify(resource.pose)}`}
            resource={resource}
            frameOptions={frameOptions}
            disabled={editLocked}
            hmiAvailable={hmiAvailable}
            objectFrameVisible={liveObjectsById.get(resource.objectId)?.viewer_frame_visible ?? false}
            onUpdate={replaceSceneResource}
            onRemove={removeSceneResource}
            onObjectFrameVisibilityChange={toggleObjectFrameVisibility}
          />
        ))}
      </Section>

      <Section title="Live Runtime Scene">
        {(sceneState?.objects ?? []).length === 0 && (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-xs text-gray-500">
            No live scene objects available.
          </div>
        )}
        {(sceneState?.objects ?? []).map((objectState) => (
          <LiveSceneObjectCard
            key={objectState.object_id}
            objectState={objectState}
            hmiAvailable={hmiAvailable}
            onObjectFrameVisibilityChange={toggleObjectFrameVisibility}
          />
        ))}
      </Section>
    </div>
  );
}
