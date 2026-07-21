import {
  Component,
  Profiler,
  Suspense,
  memo,
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  useSyncExternalStore,
} from 'react';
import type { ErrorInfo, MutableRefObject, ReactNode } from 'react';
import { Canvas, useFrame, useThree } from '@react-three/fiber';
import {
  Grid,
  Html,
  OrbitControls,
  PerspectiveCamera,
  Stats,
  TransformControls,
  useGLTF,
} from '@react-three/drei';
import * as THREE from 'three';
import type { Group, Mesh } from 'three';

import { api } from '../api/client';
import {
  applyLatestPointCloudFrame,
  registerPointCloudApplier,
  subscribePointCloudWake,
  usePointCloudSubscription,
} from '../store/pointCloudStore';
import { useProgramStore } from '../store/programStore';
import { useRobotControlCandidateStore } from '../store/robotControlCandidateStore';
import { useRobotCommandAuthority } from '../store/commandAuthority';
import {
  getAuthoritativeViewerState,
  useSceneStateStore,
  useViewerDemand,
} from '../store/sceneStateStore';
import {
  applyLatestViewerFrame,
  getLatestViewerSnapshot,
  registerViewerFrameApplier,
  subscribeViewerFrameWake,
  viewerRobotLinkPose,
} from '../store/viewerFrameStore';
import { useStatusPolling, useStatusStore } from '../store/statusStore';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import type {
  MeshAssetStatus,
  PreviewLinkPosesResponse,
  SceneFrameState,
  SceneGripperState,
  SceneObjectState,
  SceneShapeState,
  SceneUrdfCollision,
  SceneUrdfManifest,
  SceneUrdfVisual,
  TreeNode,
} from '../types/api';
import { authorGizmoRelease } from '../utils/gizmoAuthoring';
import { gripperGhostPreviewFromCandidate } from '../utils/gripperGhost';
import { computeMoveToTarget } from '../utils/moveTo';
import {
  composePose,
  poseFromBasePoseRecord,
  poseFromRecord,
} from '../utils/poseTransforms';
import type { PoseTuple } from '../utils/poseTransforms';
import type {
  ViewerFrameV1,
  ViewerManifestV1,
  ViewerPoseV1,
} from '../types/viewerProtocol.generated';
import { ObjectEditPanel } from './ObjectEditPanel';
import { RobotControlPanel } from './RobotControlPanel';
import { SceneControls } from './SceneControls';
import { SnapshotResponder } from './SnapshotResponder';
import {
  inspectViewerRenderer,
  getViewerRendererProfile,
  publishViewerRendererProfile,
  recordViewerEffectiveDpr,
  recordViewerReactCommit,
  recordViewerRenderFrame,
  recordViewerRenderWork,
  selectViewerPresentationProfile,
  subscribeViewerRendererProfile,
} from '../utils/viewerPerformance';

// Module-level stable references for zustand selector fallbacks. Returning
// a new `[]` or `{}` from a selector each render makes Object.is-based
// equality always false, which can pump the React reconciler into an
// update loop when combined with downstream useMemo deps. Sharing the
// same identity across renders keeps zustand's bailout path working.
const EMPTY_OBJECTS: SceneObjectState[] = [];

const EMPTY_GRIPPERS: SceneGripperState[] = [];

function manifestEntityToSceneObject(
  entity: ViewerManifestV1['entities'][number],
): SceneObjectState {
  const shape = entity.shape === null
    ? null
    : {
      type: entity.shape.type,
      box_dimensions_m: entity.shape.box_dimensions_m ?? undefined,
      sphere_radius_m: entity.shape.sphere_radius_m ?? undefined,
      cylinder_radius_m: entity.shape.cylinder_radius_m ?? undefined,
      cylinder_height_m: entity.shape.cylinder_height_m ?? undefined,
      mesh_resource_uri: entity.shape.mesh_resource_uri ?? undefined,
      mesh_scale: entity.shape.mesh_scale ?? undefined,
      mesh_asset_url: entity.shape.mesh_asset_url,
      mesh_asset_status: entity.shape.mesh_asset_status,
    };
  return {
    object_id: entity.object_id,
    shape,
    pose_in_universe: null,
    attached: entity.attached,
    attached_to: entity.attached_to,
    source: entity.source,
    persistable: entity.persistable,
    viewer_frame_visible: entity.viewer_frame_visible,
    support_contacts: entity.support_contacts,
  };
}

interface SceneViewerPanelProps {
  onCollapse: () => void;
  blocked: boolean;
  resizing?: boolean;
}

/**
 * Native R3F scene viewer.
 *
 * The Canvas runs `frameloop="demand"` so it only paints when the
 * React tree changes (scene-state poll) or the user interacts
 * (OrbitControls invalidates internally). This is the load-bearing
 * difference vs continuous 60 fps rendering
 * loop — the JS-per-frame work that starves Composer's typing on Thor
 * does not happen here.
 *
 * Conventions:
 * - ROS Z-up world frame. Camera `up=[0,0,1]`.
 * - ManyForge poses use `position_m: [x,y,z]` and `orientation_xyzw:
 *   [x,y,z,w]`. Three.js quaternions are xyzw too — no reorder needed.
 */
interface ContextMenuState {
  objectId: string;
  x: number;
  y: number;
}

function HeaderToggle({
  label,
  on,
  onClick,
  title,
}: {
  label: string;
  on: boolean;
  onClick: () => void;
  title: string;
}) {
  return (
    <button
      type="button"
      onClick={onClick}
      className={`rounded px-2 py-1 text-[11px] font-medium ${
        on
          ? 'bg-blue-700 text-white hover:bg-blue-600'
          : 'bg-gray-700 text-gray-200 hover:bg-gray-600'
      }`}
      title={title}
    >
      {label}
    </button>
  );
}

export function SceneViewerPanel({
  onCollapse,
  blocked,
  resizing = false,
}: SceneViewerPanelProps) {
  useStatusPolling();
  useViewerDemand();
  const runtime = useStatusStore((state) => state.runtime);
  const manifest = useSceneStateStore((state) => state.manifest);
  const connectionState = useSceneStateStore((state) => state.connectionState);
  const frameAvailable = useSceneStateStore((state) => state.frameAvailable);
  const sceneError = useSceneStateStore((state) => state.error);
  const [contextMenu, setContextMenu] = useState<ContextMenuState | null>(null);
  // Header panels are mutually exclusive — clicking one closes any
  // other. Modelled as a single `activePanel` discriminator rather
  // than three independent booleans so the radio-group invariant is
  // unambiguous to read. The state lives in viewerSelectionStore so
  // Canvas descendants (TcpJogGizmo, SceneObjectMesh's
  // TransformControls) can gate themselves on panel focus and unmount
  // — resetting gizmo drag state — when the operator
  // switches panels mid-interaction.
  const activePanel = useViewerSelectionStore((s) => s.activePanel);
  const setActivePanel = useViewerSelectionStore((s) => s.setActivePanel);
  const toggleActivePanel = useViewerSelectionStore((s) => s.toggleActivePanel);

  let backendAvailable: boolean | null;
  let unavailableDetail: string | null = null;
  if (manifest === null && sceneError === null && connectionState !== 'disconnected') {
    backendAvailable = null;
  } else if (manifest !== null) {
    backendAvailable = true;
  } else {
    backendAvailable = false;
    unavailableDetail =
      runtime?.userMessage
      || sceneError
      || 'Load a program/deployment to start the embedded 3D viewer.';
  }

  const handleContextMenu = useCallback((objectId: string, x: number, y: number): void => {
    setContextMenu({ objectId, x, y });
  }, []);
  const closeContextMenu = useCallback((): void => setContextMenu(null), []);

  return (
    <div
      className="flex h-full w-full flex-col overflow-hidden bg-gray-900"
      onContextMenu={(event) => {
        // Block native browser menu on the panel chrome; the
        // SceneObjectMesh's own onContextMenu already preventDefaults
        // for mesh hits, this catches everywhere else.
        event.preventDefault();
      }}
    >
      <div className="flex h-10 items-center justify-between border-b border-gray-700 bg-gray-800 px-3">
        <div className="min-w-0">
          <div className="text-xs font-semibold uppercase tracking-wide text-gray-300">
            Scene Viewer
          </div>
          <div className="truncate text-[10px] text-gray-500">
            click object · W/E/R: gizmo · Q: local/world · /: perf · Esc: clear · right-click: menu
          </div>
        </div>
        <div className="flex items-center gap-1">
          <HeaderToggle
            label="Tools"
            on={activePanel === 'tools'}
            onClick={() => toggleActivePanel('tools')}
            title="Frames / object axes / scene tree"
          />
          <HeaderToggle
            label="Robot"
            on={activePanel === 'robot'}
            onClick={() => toggleActivePanel('robot')}
            title="Robot control panel (joint sliders)"
          />
          <HeaderToggle
            label="Object"
            on={activePanel === 'object'}
            onClick={() => toggleActivePanel('object')}
            title="Object edit panel (pose, shape)"
          />
          <button
            type="button"
            onClick={onCollapse}
            className="rounded bg-gray-700 px-2 py-1 text-sm font-medium text-gray-200 hover:bg-gray-600"
            title="Collapse 3D viewer"
          >
            ›
          </button>
        </div>
      </div>
      <div className="relative flex-1 bg-gray-950">
        {resizing && (
          <div
            className="absolute inset-0 z-10 cursor-col-resize bg-transparent"
            aria-hidden="true"
          />
        )}
        {blocked && (
          <div className="absolute inset-0 z-20 flex items-center justify-center bg-gray-950/90 p-6 text-center">
            <div className="max-w-sm rounded-lg border border-orange-700 bg-gray-900/95 p-4 shadow-xl">
              <div className="mb-2 text-sm font-semibold text-orange-300">
                Runtime Out Of Date
              </div>
              <p className="text-sm text-gray-300">
                The editor has unapplied changes. Apply, save, or discard them in the editor bar
                before using the embedded runtime controls.
              </p>
            </div>
          </div>
        )}
        {backendAvailable === false ? (
          <div className="flex h-full items-center justify-center p-6 text-center">
            <div className="max-w-sm rounded-lg border border-gray-700 bg-gray-900/95 p-4 shadow-xl">
              <div className="mb-2 text-sm font-semibold text-gray-200">
                Scene state unavailable
              </div>
              <p className="text-sm text-gray-400">{unavailableDetail}</p>
            </div>
          </div>
        ) : (
          <>
            <Profiler
              id="manyforge-scene-viewer"
              onRender={(_id, _phase, actualDuration) => {
                recordViewerReactCommit(actualDuration);
              }}
            >
              <SceneCanvas
                resizing={resizing}
                frameAvailable={frameAvailable}
                onContextMenu={handleContextMenu}
              />
            </Profiler>
            {manifest !== null && (!frameAvailable || connectionState === 'stale') && (
              <div className="pointer-events-none absolute left-3 top-3 z-10 rounded border border-amber-700 bg-gray-950/90 px-2 py-1 text-xs text-amber-200">
                {connectionState === 'stale'
                  ? 'Viewer state is stale'
                  : 'Waiting for current viewer state'}
              </div>
            )}
            {activePanel === 'tools' && (
              <SceneControls onClose={() => setActivePanel(null)} />
            )}
            {activePanel === 'robot' && (
              <RobotControlPanel onClose={() => setActivePanel(null)} />
            )}
            {activePanel === 'object' && (
              <ObjectEditPanel onClose={() => setActivePanel(null)} />
            )}
            {contextMenu !== null && (
              <SceneObjectContextMenu
                state={contextMenu}
                onClose={closeContextMenu}
              />
            )}
          </>
        )}
      </div>
    </div>
  );
}

function SceneObjectContextMenu({
  state,
  onClose,
}: {
  state: ContextMenuState;
  onClose: () => void;
}) {
  const manifest = useSceneStateStore((s) => s.manifest);
  const objects = useMemo(
    () => manifest?.entities.map(manifestEntityToSceneObject) ?? EMPTY_OBJECTS,
    [manifest],
  );
  const object = useMemo(
    () => objects.find((o) => o.object_id === state.objectId) ?? null,
    [objects, state.objectId],
  );
  const setSelected = useViewerSelectionStore((s) => s.setSelected);
  const toggleLocked = useViewerSelectionStore((s) => s.toggleLocked);
  const locked = useViewerSelectionStore((s) => s.lockedIds.has(state.objectId));

  // Click anywhere outside the menu closes it.
  useEffect(() => {
    function onDocClick(): void {
      onClose();
    }
    window.addEventListener('click', onDocClick);
    window.addEventListener('contextmenu', onDocClick);
    return () => {
      window.removeEventListener('click', onDocClick);
      window.removeEventListener('contextmenu', onDocClick);
    };
  }, [onClose]);

  const persistable = object?.persistable ?? false;
  const editable = persistable && !(object?.attached ?? false);

  function handleEdit(): void {
    setSelected(state.objectId);
    // Day-5 object-edit overlay subscribes to selectedId; selecting
    // here is enough to surface it. Until that overlay exists the
    // gizmo is the visible affordance.
    onClose();
  }

  function handleDuplicate(): void {
    if (object === null) return;
    const newId = `${object.object_id}_copy_${Date.now().toString(36)}`;
    const authoritativeObject = getAuthoritativeViewerState()?.scene.objects.find(
      (candidate) => candidate.object_id === object.object_id,
    );
    const pose = authoritativeObject?.pose_in_universe as Record<string, unknown> | null;
    const position = Array.isArray(pose?.position_m)
      ? (pose!.position_m as number[]).map((v, i) => (i === 0 ? Number(v) + 0.1 : Number(v)))
      : [0.1, 0, 0];
    const orientation = Array.isArray(pose?.orientation_xyzw)
      ? (pose!.orientation_xyzw as number[]).map(Number)
      : [0, 0, 0, 1];
    const shape = object.shape as Record<string, unknown> | null;
    const shapeType = shape?.type as 'box' | 'sphere' | 'cylinder' | 'mesh' | undefined;
    void api.scene
      .edit({
        operation: 'add',
        object_id: newId,
        shape_type: shapeType,
        box_dimensions_m: Array.isArray(shape?.box_dimensions_m)
          ? (shape!.box_dimensions_m as number[]).map(Number)
          : undefined,
        sphere_radius_m:
          typeof shape?.sphere_radius_m === 'number' ? (shape.sphere_radius_m as number) : undefined,
        cylinder_radius_m:
          typeof shape?.cylinder_radius_m === 'number' ? (shape.cylinder_radius_m as number) : undefined,
        cylinder_height_m:
          typeof shape?.cylinder_height_m === 'number' ? (shape.cylinder_height_m as number) : undefined,
        pose_position_m: position as [number, number, number],
        pose_orientation_xyzw: orientation as [number, number, number, number],
      })
      .catch((err) => console.warn('duplicate failed', err));
    onClose();
  }

  function handleRemove(): void {
    void api.scene
      .edit({ operation: 'remove', object_id: state.objectId })
      .catch((err) => console.warn('remove failed', err));
    onClose();
  }

  function handleToggleLock(): void {
    toggleLocked(state.objectId);
    onClose();
  }

  return (
    <div
      className="absolute z-30 min-w-[160px] rounded border border-gray-700 bg-gray-900/95 py-1 text-sm text-gray-200 shadow-xl"
      style={{ left: state.x, top: state.y }}
      onClick={(event) => event.stopPropagation()}
      onContextMenu={(event) => {
        event.preventDefault();
        event.stopPropagation();
      }}
    >
      <div className="border-b border-gray-700 px-3 py-1 text-[10px] uppercase tracking-wide text-gray-500">
        {state.objectId}
      </div>
      <button
        type="button"
        onClick={handleEdit}
        className="block w-full px-3 py-1 text-left hover:bg-gray-800"
        disabled={!editable}
      >
        Edit
        {!editable && <span className="ml-2 text-[10px] text-gray-500">(not editable)</span>}
      </button>
      <button
        type="button"
        onClick={handleDuplicate}
        className="block w-full px-3 py-1 text-left hover:bg-gray-800"
        disabled={object === null}
      >
        Duplicate
      </button>
      <button
        type="button"
        onClick={handleRemove}
        className="block w-full px-3 py-1 text-left text-red-300 hover:bg-gray-800"
        disabled={!persistable}
      >
        Remove
        {!persistable && <span className="ml-2 text-[10px] text-gray-500">(runtime-only)</span>}
      </button>
      <button
        type="button"
        onClick={handleToggleLock}
        className="block w-full px-3 py-1 text-left hover:bg-gray-800"
      >
        {locked ? 'Unlock' : 'Lock'}
      </button>
    </div>
  );
}

type GizmoMode = 'translate' | 'rotate' | 'scale';
type GizmoSpace = 'world' | 'local';

function ViewerRenderMetrics() {
  const gl = useThree((state) => state.gl);
  useEffect(() => {
    const { vendor, renderer, software } = inspectViewerRenderer(gl);
    const presentation = selectViewerPresentationProfile(
      software,
      window.devicePixelRatio,
    );
    publishViewerRendererProfile({
      vendor: vendor || null,
      renderer: renderer || null,
      software,
      ...presentation,
    });
  }, [gl]);
  useFrame(() => {
    recordViewerEffectiveDpr(gl.getPixelRatio());
    recordViewerRenderFrame(performance.now());
    recordViewerRenderWork(
      gl.info.render.calls,
      gl.info.render.triangles,
      gl.domElement.width * gl.domElement.height,
    );
  });
  return null;
}

const HARDWARE_INTERPOLATION_DURATION_MS = 50;
const SOFTWARE_INTERPOLATION_DURATION_MS = 0;
const HARDWARE_INTERPOLATION_RATE_HZ = 60;
const SOFTWARE_INTERPOLATION_RATE_HZ = 15;

function ViewerFrameDriver({ software }: { software: boolean }) {
  const invalidate = useThree((state) => state.invalidate);
  const continuationTimerRef = useRef<number | null>(null);
  const wake = useCallback(() => {
    if (continuationTimerRef.current !== null) {
      window.clearTimeout(continuationTimerRef.current);
      continuationTimerRef.current = null;
    }
    invalidate();
  }, [invalidate]);
  useEffect(() => {
    const unsubscribe = subscribeViewerFrameWake(wake);
    return () => {
      unsubscribe();
      if (continuationTimerRef.current !== null) {
        window.clearTimeout(continuationTimerRef.current);
        continuationTimerRef.current = null;
      }
    };
  }, [wake]);
  useFrame(() => {
    const result = applyLatestViewerFrame(
      Date.now(),
      software
        ? SOFTWARE_INTERPOLATION_DURATION_MS
        : HARDWARE_INTERPOLATION_DURATION_MS,
    );
    if (!result.interpolationPending || continuationTimerRef.current !== null) {
      return;
    }
    const rateHz = software
      ? SOFTWARE_INTERPOLATION_RATE_HZ
      : HARDWARE_INTERPOLATION_RATE_HZ;
    continuationTimerRef.current = window.setTimeout(() => {
      continuationTimerRef.current = null;
      invalidate();
    }, 1_000 / rateHz);
  }, -1_000);
  return null;
}

function useViewerFrameBinding<T extends THREE.Object3D>(
  node: T | null,
  apply: (
    target: T,
    manifest: ViewerManifestV1,
    frame: ViewerFrameV1,
    previousFrame: ViewerFrameV1 | null,
    interpolationAlpha: number,
  ) => number | void,
): void {
  const applyRef = useRef(apply);
  useLayoutEffect(() => {
    applyRef.current = apply;
  }, [apply]);
  useEffect(() => {
    if (node === null) return undefined;
    return registerViewerFrameApplier((manifest, frame, previousFrame, interpolationAlpha) => (
      applyRef.current(node, manifest, frame, previousFrame, interpolationAlpha)
    ));
  }, [node]);
}

const MAX_INTERPOLATED_TRANSLATION_M = 0.25;
const interpolationStartQuaternion = new THREE.Quaternion();
const interpolationEndQuaternion = new THREE.Quaternion();

function finitePoseComponent(value: number, fallback: number): number {
  return Number.isFinite(value) ? value : fallback;
}

function applyPoseComponents(
  target: THREE.Object3D,
  position: ArrayLike<number>,
  quaternion: ArrayLike<number>,
  previousPosition: ArrayLike<number> | null,
  previousQuaternion: ArrayLike<number> | null,
  interpolationAlpha: number,
): number {
  const px = finitePoseComponent(Number(position[0]), 0);
  const py = finitePoseComponent(Number(position[1]), 0);
  const pz = finitePoseComponent(Number(position[2]), 0);
  const qx = finitePoseComponent(Number(quaternion[0]), 0);
  const qy = finitePoseComponent(Number(quaternion[1]), 0);
  const qz = finitePoseComponent(Number(quaternion[2]), 0);
  const qw = finitePoseComponent(Number(quaternion[3]), 1);
  const alpha = Math.min(1, Math.max(0, interpolationAlpha));
  if (alpha >= 1 || previousPosition === null || previousQuaternion === null) {
    target.position.set(px, py, pz);
    target.quaternion.set(qx, qy, qz, qw).normalize();
    return 1;
  }
  const previousPx = Number(previousPosition[0]);
  const previousPy = Number(previousPosition[1]);
  const previousPz = Number(previousPosition[2]);
  const previousQx = Number(previousQuaternion[0]);
  const previousQy = Number(previousQuaternion[1]);
  const previousQz = Number(previousQuaternion[2]);
  const previousQw = Number(previousQuaternion[3]);
  const dx = px - previousPx;
  const dy = py - previousPy;
  const dz = pz - previousPz;
  if (
    !Number.isFinite(previousPx)
    || !Number.isFinite(previousPy)
    || !Number.isFinite(previousPz)
    || !Number.isFinite(previousQx)
    || !Number.isFinite(previousQy)
    || !Number.isFinite(previousQz)
    || !Number.isFinite(previousQw)
    || dx * dx + dy * dy + dz * dz
      > MAX_INTERPOLATED_TRANSLATION_M * MAX_INTERPOLATED_TRANSLATION_M
  ) {
    target.position.set(px, py, pz);
    target.quaternion.set(qx, qy, qz, qw).normalize();
    return 1;
  }
  target.position.set(
    previousPx + (px - previousPx) * alpha,
    previousPy + (py - previousPy) * alpha,
    previousPz + (pz - previousPz) * alpha,
  );
  interpolationStartQuaternion
    .set(previousQx, previousQy, previousQz, previousQw)
    .normalize();
  interpolationEndQuaternion.set(qx, qy, qz, qw).normalize();
  target.quaternion.slerpQuaternions(
    interpolationStartQuaternion,
    interpolationEndQuaternion,
    alpha,
  );
  return 1;
}

function applyViewerPose(
  target: THREE.Object3D,
  pose: ViewerPoseV1 | null,
  previousPose: ViewerPoseV1 | null = null,
  interpolationAlpha = 1,
): number {
  target.visible = pose !== null;
  if (pose === null) return 0;
  return applyPoseComponents(
    target,
    pose.position_m,
    pose.orientation_xyzw,
    previousPose?.position_m ?? null,
    previousPose?.orientation_xyzw ?? null,
    interpolationAlpha,
  );
}

const SceneCanvas = memo(function SceneCanvas({
  resizing,
  frameAvailable,
  onContextMenu,
}: {
  resizing: boolean;
  frameAvailable: boolean;
  onContextMenu: (objectId: string, x: number, y: number) => void;
}) {
  const selectedId = useViewerSelectionStore((s) => s.selectedId);
  const setSelected = useViewerSelectionStore((s) => s.setSelected);
  const [gizmoMode, setGizmoMode] = useState<GizmoMode>('translate');
  // Default to `local` so the operator sees the gizmo axes aligned
  // with the selected object's orientation — that's almost always
  // what feels right when nudging an object's pose. `world` is one
  // keystroke away for axis-aligned moves.
  const [gizmoSpace, setGizmoSpace] = useState<GizmoSpace>('local');
  const [statsVisible, setStatsVisible] = useState(false);
  const rendererProfile = useSyncExternalStore(
    subscribeViewerRendererProfile,
    getViewerRendererProfile,
    getViewerRendererProfile,
  );
  // `isDragging` lives in a ref because we read it from an event
  // handler attached to <Canvas> (onPointerMissed) — a `useState` would
  // make the closure stale unless the Canvas re-rendered between
  // pointerdown and pointerup, and we don't want to force re-renders
  // during a drag.
  const isDraggingRef = useRef(false);

  // Camera URL-pin: ?cam=x,y,z&target=x,y,z (Day-4 contract item).
  // Parsed in `main.tsx` via `seedCameraFromUrl()`; consumed here once
  // on first mount and handed to <PerspectiveCamera> / <OrbitControls>
  // as initial values. `consumePendingCamera` is single-shot so
  // future React StrictMode double-mounts or HMR remounts don't keep
  // resetting the camera while the operator is interacting.
  const initialCamera = useMemo(
    () => useViewerSelectionStore.getState().consumePendingCamera(),
    [],
  );
  const initialCameraPosition: [number, number, number] =
    initialCamera?.position ?? [2, 2, 1.5];
  const initialCameraTarget: [number, number, number] =
    initialCamera?.target ?? [0, 0, 0.3];

  useEffect(() => {
    function onKey(e: KeyboardEvent): void {
      // Don't grab keys while the operator is typing in a side panel.
      const target = e.target as HTMLElement | null;
      if (target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.isContentEditable)) {
        return;
      }
      if (e.key === 'w' || e.key === 'W') setGizmoMode('translate');
      else if (e.key === 'e' || e.key === 'E') setGizmoMode('rotate');
      else if (e.key === 'r' || e.key === 'R') setGizmoMode('scale');
      else if (e.key === 'q' || e.key === 'Q') {
        setGizmoSpace((s) => (s === 'local' ? 'world' : 'local'));
      } else if (e.key === '/') {
        setStatsVisible((v) => !v);
      } else if (e.key === 'Escape') setSelected(null);
    }
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [setSelected]);

  return (
    <Canvas
      // Renderer kind is known only after the first context is inspected. If
      // that context proves to be software WebGL, remount once into the
      // bounded fallback profile; hardware contexts keep MSAA for normal
      // operator-facing visual quality.
      key={[
        rendererProfile.software ? 'software' : 'hardware',
        rendererProfile.antialias ? 'msaa' : 'no-msaa',
        rendererProfile.preserveDrawingBuffer ? 'preserved' : 'transient',
      ].join('-')}
      frameloop="demand"
      dpr={rendererProfile.effectiveDpr}
      gl={{
        antialias: rendererProfile.antialias,
        // The viewer is visually opaque. Avoiding a transparent WebGL surface
        // removes an unnecessary compositor blend and prevents software-canvas
        // tiles from exposing stale page content during camera motion.
        alpha: false,
        powerPreference: 'high-performance',
        // SnapshotResponder renders into a reusable offscreen target. Keeping
        // the visible drawing buffer is disabled on hardware. The software
        // profile preserves it because Firefox/llvmpipe otherwise presents
        // incomplete transient frames during demand-driven camera movement.
        preserveDrawingBuffer: rendererProfile.preserveDrawingBuffer,
      }}
      className={resizing ? 'pointer-events-none' : ''}
      style={{ background: '#0a0f18' }}
      onPointerMissed={() => {
        // Mid-drag, the gizmo handle releases off the geometry and R3F
        // reports onPointerMissed; without this guard the wrapped
        // mesh would deselect, TransformControls would unmount, and
        // the drag would visibly snap.
        if (isDraggingRef.current) return;
        setSelected(null);
      }}
    >
      <ViewerRenderMetrics />
      <ViewerFrameDriver software={rendererProfile.software} />
      <color attach="background" args={['#0a0f18']} />
      <PerspectiveCamera
        makeDefault
        up={[0, 0, 1]}
        position={initialCameraPosition}
        fov={50}
        near={0.01}
        far={100}
      />
      {/* Lighting: ambient stays moderate; a sky/ground hemisphere
          light gives mesh undersides usable shading, and two
          directional lights from opposite quadrants avoid a "half
          the robot is in shadow" look on Z-up cells. Numbers tuned
          for the gray-blue scene background (#0a0f18). */}
      <ambientLight intensity={0.9} />
      <hemisphereLight args={['#dbe6ff', '#1a2530', 0.7]} />
      <directionalLight position={[5, 5, 10]} intensity={1.6} castShadow={false} />
      <directionalLight position={[-5, -5, 6]} intensity={0.6} castShadow={false} />
      {/* GridHelper uses clipped WebGL line segments, which Mesa llvmpipe can
          mis-rasterize as large polygons during camera motion. A finite shader
          grid is one draw call, has no line primitives, and avoids the old
          infinite-grid overdraw. */}
      <Grid
        args={[10, 10]}
        cellSize={0.1}
        cellThickness={0.4}
        cellColor="#202830"
        sectionSize={1}
        sectionThickness={0.8}
        sectionColor="#3a4252"
        fadeDistance={10}
        fadeStrength={1.5}
        infiniteGrid={false}
        followCamera={false}
        rotation={[Math.PI / 2, 0, 0]}
      />
      <axesHelper args={[0.5]} />
      <OrbitControls
        makeDefault
        screenSpacePanning
        enableDamping
        dampingFactor={0.12}
        target={initialCameraTarget}
      />
      <group name="manyforge-authoritative-view" visible={frameAvailable}>
        <Suspense fallback={null}>
          <RobotModel />
        </Suspense>
        <SceneObjects
          selectedId={selectedId}
          onSelect={setSelected}
          gizmoMode={gizmoMode}
          gizmoSpace={gizmoSpace}
          isDraggingRef={isDraggingRef}
          onContextMenu={onContextMenu}
        />
        <MeasuredPoseSourceGhosts />
        <FrameAxes />
        <ObjectFrameAxes />
        <PointCloud />
        <TcpJogGizmo />
        <MoveToTargetGhost />
        <CandidateGhostRobot />
      </group>
      <SnapshotResponder />
      {statsVisible && <Stats className="!left-2 !top-2" />}
    </Canvas>
  );
});

function SceneObjects({
  selectedId,
  onSelect,
  gizmoMode,
  gizmoSpace,
  isDraggingRef,
  onContextMenu,
}: {
  selectedId: string | null;
  onSelect: (id: string | null) => void;
  gizmoMode: GizmoMode;
  gizmoSpace: GizmoSpace;
  isDraggingRef: MutableRefObject<boolean>;
  onContextMenu: (objectId: string, x: number, y: number) => void;
}) {
  const manifest = useSceneStateStore((state) => state.manifest);
  const objects = useMemo(
    () => manifest?.entities.map(manifestEntityToSceneObject) ?? EMPTY_OBJECTS,
    [manifest],
  );
  const lockedIds = useViewerSelectionStore((s) => s.lockedIds);
  return (
    <group name="manyforge-scene-objects">
      {objects.map((obj, entityIndex) => (
        <SceneObjectMesh
          key={obj.object_id}
          object={obj}
          entityIndex={entityIndex}
          selected={obj.object_id === selectedId}
          locked={lockedIds.has(obj.object_id)}
          onSelect={() => onSelect(obj.object_id)}
          onContextMenu={onContextMenu}
          gizmoMode={gizmoMode}
          gizmoSpace={gizmoSpace}
          isDraggingRef={isDraggingRef}
        />
      ))}
    </group>
  );
}

interface MeasuredGhost {
  key: string;
  markerKind: 'object' | 'tcp';
  objectId: string;
  sourceId: string;
  sourceFrame: string;
  observedFrame: string | null;
  shape: SceneShapeState | null;
  shapeType: string | null;
}

function measuredGhostEnabled(
  config: Record<string, unknown> | null | undefined,
  key: 'object' | 'robotTcp',
  defaultEnabled = false,
): boolean {
  if (!config) return defaultEnabled;
  if (config.enabled === false) return false;
  if (config[key] === false) return false;
  return config.enabled === true;
}

function MeasuredPoseSourceGhosts() {
  const visible = useViewerSelectionStore((s) => s.measuredStateGhostVisible);
  const manifest = useSceneStateStore((state) => state.manifest);
  const poseSources = manifest?.pose_sources;
  const objects = useMemo(
    () => manifest?.entities.map(manifestEntityToSceneObject) ?? EMPTY_OBJECTS,
    [manifest],
  );

  const objectById = useMemo(() => {
    const map = new Map<string, SceneObjectState>();
    for (const object of objects) {
      map.set(object.object_id, object);
    }
    return map;
  }, [objects]);

  const ghosts = useMemo<MeasuredGhost[]>(() => {
    if (!visible) return [];
    const next: MeasuredGhost[] = [];
    for (const source of poseSources ?? []) {
      const renderTcp = source.objects.some((observed) => (
        measuredGhostEnabled(observed.measured_state_ghost, 'robotTcp')
      ));
      if (renderTcp) {
        next.push({
          key: `${source.pose_source_id}:measured_tcp`,
          markerKind: 'tcp',
          objectId: 'measured_tcp',
          sourceId: source.pose_source_id,
          sourceFrame: source.frame_id,
          observedFrame: null,
          shape: null,
          shapeType: null,
        });
      }
      for (const observed of source.objects) {
        if (!measuredGhostEnabled(observed.measured_state_ghost, 'object', true)) {
          continue;
        }
        const object = objectById.get(observed.object_id) ?? null;
        const shape = object?.shape ?? null;
        const shapeType = typeof shape?.type === 'string' ? shape.type : null;
        next.push({
          key: `${source.pose_source_id}:${observed.object_id}`,
          markerKind: 'object',
          objectId: observed.object_id,
          sourceId: source.pose_source_id,
          sourceFrame: source.frame_id,
          observedFrame: observed.frame_id,
          shape,
          shapeType,
        });
      }
    }
    return next;
  }, [objectById, poseSources, visible]);

  if (!visible || ghosts.length === 0) {
    return null;
  }

  return (
    <group name="manyforge-measured-state-ghosts" renderOrder={20}>
      {ghosts.map((ghost) => (
        <BoundMeasuredPoseGhost
          key={ghost.key}
          name={`measured-ghost:${ghost.objectId}`}
          ghost={ghost}
        >
          <MeasuredGhostBody
            markerKind={ghost.markerKind}
            shape={ghost.shape}
            shapeType={ghost.shapeType}
          />
        </BoundMeasuredPoseGhost>
      ))}
    </group>
  );
}

function BoundMeasuredPoseGhost({
  name,
  ghost,
  children,
}: {
  name: string;
  ghost: MeasuredGhost;
  children: ReactNode;
}) {
  const [node, setNode] = useState<Group | null>(null);
  useViewerFrameBinding(node, (target, _manifest, frame) => {
    const source = frame.pose_source_states.find(
      (item) => item.pose_source_id === ghost.sourceId,
    );
    let position: [number, number, number] | null = null;
    let quaternion: [number, number, number, number] | null = null;
    if (ghost.markerKind === 'tcp') {
      const measuredTcp = source?.measured_tcp ?? null;
      if (
        measuredTcp !== null
        && isRenderableWorldPoseFrame(measuredTcp.pose.frame_id, ghost.sourceFrame)
      ) {
        position = tuple3(measuredTcp.pose.position_xyz);
        quaternion = tuple4(measuredTcp.pose.orientation_xyzw);
      }
    } else {
      const observed = source?.objects.find((item) => item.object_id === ghost.objectId);
      if (
        observed?.status === 'fresh'
        && observed.update !== null
        && ghost.observedFrame !== null
        && isRenderableWorldPoseFrame(ghost.observedFrame, ghost.sourceFrame)
      ) {
        position = tuple3(observed.update.pose.position_xyz);
        quaternion = tuple4(observed.update.pose.orientation_xyzw);
      }
    }
    target.visible = position !== null && quaternion !== null;
    if (position === null || quaternion === null) return 0;
    target.position.set(...position);
    target.quaternion.set(...quaternion);
    return 1;
  });
  return <group ref={setNode} name={name} visible={false}>{children}</group>;
}

function measuredRobotGhostOpacity(source: {
  objects: Array<{ measured_state_ghost?: Record<string, unknown> | null }>;
}): number | null {
  for (const observed of source.objects) {
    const config = observed.measured_state_ghost;
    if (!measuredGhostEnabled(config, 'robotTcp')) {
      continue;
    }
    const opacity = typeof config?.opacity === 'number' ? config.opacity : 0.35;
    return Math.min(0.8, Math.max(0.08, opacity));
  }
  return null;
}

function MeasuredGhostBody({
  markerKind,
  shape,
  shapeType,
}: {
  markerKind: 'object' | 'tcp';
  shape: SceneShapeState | null;
  shapeType: string | null;
}) {
  const ghostColor = '#22d3ee';
  const ghostOpacity = 0.45;
  if (markerKind === 'tcp') {
    return (
      <group name="measured-tcp-marker" renderOrder={21}>
        <mesh>
          <sphereGeometry args={[0.025, 16, 12]} />
          <meshStandardMaterial
            color={ghostColor}
            emissive={ghostColor}
            emissiveIntensity={0.35}
            transparent
            opacity={0.8}
            depthWrite={false}
          />
        </mesh>
        <mesh position={[0.045, 0, 0]}>
          <boxGeometry args={[0.09, 0.006, 0.006]} />
          <meshBasicMaterial
            color="#ef4444"
            transparent
            opacity={0.85}
            depthWrite={false}
          />
        </mesh>
        <mesh position={[0, 0.045, 0]}>
          <boxGeometry args={[0.006, 0.09, 0.006]} />
          <meshBasicMaterial
            color="#22c55e"
            transparent
            opacity={0.85}
            depthWrite={false}
          />
        </mesh>
        <mesh position={[0, 0, 0.045]}>
          <boxGeometry args={[0.006, 0.006, 0.09]} />
          <meshBasicMaterial
            color="#60a5fa"
            transparent
            opacity={0.85}
            depthWrite={false}
          />
        </mesh>
      </group>
    );
  }
  if (shapeType === 'mesh') {
    return (
      <SceneObjectMeshAsset
        shape={shape}
        color={ghostColor}
        opacity={0.4}
        overlay
      />
    );
  }
  if (shape !== null) {
    return (
      <SceneObjectPrimitive
        shape={shape}
        color={ghostColor}
        opacity={ghostOpacity}
        shapeType={shapeType}
        overlay
      />
    );
  }
  return (
    <mesh renderOrder={20}>
      <boxGeometry args={[0.06, 0.06, 0.06]} />
      <meshStandardMaterial
        color={ghostColor}
        wireframe
        transparent
        opacity={0.7}
        depthWrite={false}
      />
    </mesh>
  );
}

function SceneObjectMesh({
  object,
  entityIndex,
  selected,
  locked,
  onSelect,
  onContextMenu,
  gizmoMode,
  gizmoSpace,
  isDraggingRef,
}: {
  object: SceneObjectState;
  entityIndex: number;
  selected: boolean;
  locked: boolean;
  onSelect: () => void;
  onContextMenu: (objectId: string, x: number, y: number) => void;
  gizmoMode: GizmoMode;
  gizmoSpace: GizmoSpace;
  isDraggingRef: MutableRefObject<boolean>;
}) {
  // State-as-ref: TransformControls needs a stable Object3D. Mesh-backed
  // scene objects can swap their child renderer while GLB assets load or
  // fail, so the controls attach to this stable wrapper group rather than
  // to a primitive mesh or a GLB primitive.
  const [objectNode, setObjectNode] = useState<Group | null>(null);
  // Object-tab focus gate: the TransformControls only mount when the
  // operator has the Object panel open. Selecting an object from a
  // different panel keeps the selection visible (highlight, right-click
  // menu) but does NOT surface the drag gizmo, matching the principle
  // that the gizmo is the Object-panel affordance.
  const objectPanelActive = useViewerSelectionStore((s) => s.activePanel === 'object');
  const shape = object.shape ?? null;
  const color = selected ? '#f5d24a' : colorForSource(object.source);
  const opacity = object.attached ? 0.85 : 1.0;
  const shapeType = typeof shape?.type === 'string' ? (shape.type as string) : null;
  const editable = object.persistable && !object.attached;

  useViewerFrameBinding(objectNode, (
    target,
    _manifest,
    frame,
    previousFrame,
    interpolationAlpha,
  ) => {
    if (selected && isDraggingRef.current) return 0;
    return applyViewerPose(
      target,
      frame.entity_transforms[entityIndex] ?? null,
      previousFrame?.entity_transforms[entityIndex] ?? null,
      interpolationAlpha,
    );
  });

  return (
    <>
      <group
        ref={setObjectNode}
        onClick={(event) => {
          event.stopPropagation();
          onSelect();
        }}
        onContextMenu={(event) => {
          event.stopPropagation();
          // Block the browser's native menu so ours can take over.
          // `nativeEvent` is the synthetic R3F event's underlying
          // DOM event; preventDefault here propagates to the
          // canvas-level handler too.
          event.nativeEvent.preventDefault();
          onSelect();
          onContextMenu(object.object_id, event.nativeEvent.clientX, event.nativeEvent.clientY);
        }}
      >
        {shapeType === 'mesh' ? (
          <SceneObjectMeshAsset shape={shape} color={color} opacity={opacity} />
        ) : (
          <SceneObjectPrimitive shape={shape} color={color} opacity={opacity} shapeType={shapeType} />
        )}
      </group>
      {objectPanelActive && selected && editable && !locked && objectNode !== null && (
        <TransformControls
          object={objectNode}
          mode={gizmoMode}
          space={gizmoSpace}
          // drei TransformControls emits `dragging-changed`. Reflect
          // it into the parent ref so the Canvas's onPointerMissed
          // skips the deselect during the drag — that was the cause
          // of the "drag releases after a fraction of a second" bug
          // reported in the first pass.
          onMouseDown={() => {
            isDraggingRef.current = true;
          }}
          onMouseUp={() => {
            isDraggingRef.current = false;
            emitObjectPoseEdit(object.object_id, objectNode);
          }}
        />
      )}
    </>
  );
}

function SceneObjectPrimitive({
  shape,
  color,
  opacity,
  shapeType,
  overlay = false,
}: {
  shape: SceneShapeState | null;
  color: string;
  opacity: number;
  shapeType: string | null;
  overlay?: boolean;
}) {
  const cylinderRadius = typeof shape?.cylinder_radius_m === 'number' ? shape.cylinder_radius_m : 0.05;
  const cylinderHeight = typeof shape?.cylinder_height_m === 'number' ? shape.cylinder_height_m : 0.1;
  const cylinderGeometry = useMemo(() => {
    if (shapeType !== 'cylinder') {
      return null;
    }
    const geometry = new THREE.CylinderGeometry(cylinderRadius, cylinderRadius, cylinderHeight, 32);
    // RoboPlan/Coal cylinders are Z-axis primitives. Three.js cylinders are
    // Y-axis primitives, so rotate the geometry instead of the scene object;
    // this keeps pose edits and TransformControls in the backend frame.
    geometry.rotateX(Math.PI / 2);
    return geometry;
  }, [cylinderHeight, cylinderRadius, shapeType]);

  useEffect(() => {
    return () => {
      cylinderGeometry?.dispose();
    };
  }, [cylinderGeometry]);

  const geometry = useMemo(() => {
    if (shapeType === 'box') {
      const dims = numberTriple(shape?.box_dimensions_m, [0.1, 0.1, 0.1]);
      return <boxGeometry args={dims} />;
    }
    if (shapeType === 'sphere') {
      const radius = typeof shape?.sphere_radius_m === 'number' ? shape.sphere_radius_m : 0.05;
      return <sphereGeometry args={[radius, 24, 16]} />;
    }
    if (shapeType === 'cylinder') {
      return cylinderGeometry ? <primitive object={cylinderGeometry} attach="geometry" /> : null;
    }
    return <boxGeometry args={[0.05, 0.05, 0.05]} />;
  }, [cylinderGeometry, shape, shapeType]);

  return (
    <mesh renderOrder={overlay ? 20 : 0}>
      {geometry}
      <meshStandardMaterial
        color={color}
        transparent={opacity < 1}
        opacity={opacity}
        depthTest={!overlay}
        depthWrite={opacity >= 1 && !overlay}
        wireframe={overlay || (shapeType !== 'box' && shapeType !== 'sphere' && shapeType !== 'cylinder')}
      />
    </mesh>
  );
}

function SceneObjectMeshAsset({
  shape,
  color,
  opacity,
  overlay = false,
}: {
  shape: SceneShapeState | null;
  color: string;
  opacity: number;
  overlay?: boolean;
}) {
  const status = (shape?.mesh_asset_status ?? 'missing') as MeshAssetStatus | string;
  const url = typeof shape?.mesh_asset_url === 'string' ? shape.mesh_asset_url : null;
  useClearPreviousSceneMeshUrl(url);
  if (status === 'ok' && url) {
    return (
      <SceneMeshErrorBoundary
        resetKey={url}
        fallback={<SceneMeshStatusMarker status="parse_failed" color={color} />}
      >
        <Suspense fallback={<SceneMeshStatusMarker status="pending" color={color} />}>
          <SceneObjectMeshGltf
            url={url}
            color={color}
            opacity={opacity}
            overlay={overlay}
          />
        </Suspense>
      </SceneMeshErrorBoundary>
    );
  }
  return <SceneMeshStatusMarker status={status} color={color} />;
}

function useClearPreviousSceneMeshUrl(url: string | null): void {
  const previousUrlRef = useRef<string | null>(null);
  useEffect(() => {
    const previous = previousUrlRef.current;
    if (previous !== null && previous !== url) {
      (useGLTF as unknown as { clear?: (target: string) => void }).clear?.(previous);
    }
    previousUrlRef.current = url;
  }, [url]);
}

function SceneObjectMeshGltf({
  url,
  color,
  opacity,
  overlay = false,
}: {
  url: string;
  color: string;
  opacity: number;
  overlay?: boolean;
}) {
  const gltf = useGLTF(url);
  const cloned = useMemo(() => {
    const root = gltf.scene.clone(true);
    root.traverse((obj) => {
      obj.renderOrder = overlay ? 20 : obj.renderOrder;
      const mesh = obj as THREE.Mesh;
      if (!mesh.isMesh) return;
      if (overlay) {
        mesh.material = new THREE.MeshBasicMaterial({
          color,
          transparent: true,
          opacity,
          depthTest: false,
          depthWrite: false,
          side: THREE.DoubleSide,
        });
        return;
      }
      const sourceMaterials = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
      const tinted = sourceMaterials.map((source) => {
        const material = source.clone() as THREE.MeshStandardMaterial;
        material.color?.set(color);
        if (opacity < 1) {
          material.transparent = true;
          material.opacity = opacity;
          material.depthWrite = false;
        }
        return material;
      });
      mesh.material = Array.isArray(mesh.material) ? tinted : tinted[0];
    });
    return root;
  }, [gltf.scene, color, opacity, overlay]);
  return <primitive key={url} object={cloned} />;
}

function SceneMeshStatusMarker({ status, color }: { status: string; color: string }) {
  const label = meshStatusLabel(status);
  const markerColor = status === 'pending' ? color : '#f97316';
  return (
    <group>
      <mesh>
        <boxGeometry args={[0.05, 0.05, 0.05]} />
        <meshStandardMaterial color={markerColor} wireframe opacity={0.95} transparent />
      </mesh>
      <Html position={[0, 0, 0.075]} center distanceFactor={8} style={{ pointerEvents: 'none' }}>
        <div className="rounded border border-orange-500 bg-gray-950/90 px-2 py-1 text-[10px] font-semibold uppercase tracking-wide text-orange-200 shadow">
          {label}
        </div>
      </Html>
    </group>
  );
}

function meshStatusLabel(status: string): string {
  switch (status) {
    case 'pending':
      return 'mesh loading';
    case 'missing':
      return 'mesh missing';
    case 'unsupported_format':
      return 'unsupported mesh';
    case 'parse_failed':
      return 'mesh parse failed';
    case 'conversion_timeout':
      return 'mesh timeout';
    case 'too_large':
      return 'mesh too large';
    case 'outside_allowed_roots':
      return 'mesh path blocked';
    default:
      return 'mesh unavailable';
  }
}

class SceneMeshErrorBoundary extends Component<
  { resetKey: string; fallback: ReactNode; children: ReactNode },
  { failed: boolean }
> {
  state = { failed: false };

  static getDerivedStateFromError(): { failed: boolean } {
    return { failed: true };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    console.warn('scene mesh asset failed to load', error, info.componentStack);
  }

  componentDidUpdate(prevProps: { resetKey: string }): void {
    if (prevProps.resetKey !== this.props.resetKey && this.state.failed) {
      this.setState({ failed: false });
    }
  }

  render(): ReactNode {
    if (this.state.failed) {
      return this.props.fallback;
    }
    return this.props.children;
  }
}

function emitObjectPoseEdit(objectId: string, group: Group | null): void {
  if (!group) return;
  const position = group.position.toArray() as [number, number, number];
  const q = group.quaternion;
  void api.scene
    .edit({
      operation: 'update_pose',
      object_id: objectId,
      pose_position_m: position,
      pose_orientation_xyzw: [q.x, q.y, q.z, q.w],
    })
    .catch((err) => {
      console.warn('scene edit failed for', objectId, err);
    });
}

function RobotModel() {
  const [urdfManifest, setUrdfManifest] = useState<SceneUrdfManifest | null>(null);
  const [manifestError, setManifestError] = useState<string | null>(null);
  const viewerManifest = useSceneStateStore((state) => state.manifest);
  const robotDescriptor = viewerManifest?.robots[0] ?? null;
  const robotModelKey = robotDescriptor?.urdf_model_key ?? robotDescriptor?.robot_id ?? null;
  const collisionMeshesVisible = useViewerSelectionStore((s) => s.collisionMeshesVisible);

  useEffect(() => {
    let cancelled = false;
    setUrdfManifest(null);
    setManifestError(null);
    api.urdf
      .manifest()
      .then((data) => {
        if (!cancelled) setUrdfManifest(data);
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

  if (urdfManifest === null) {
    // 404 here is the normal state when the deployment has no URDF;
    // the rest of the viewer still works.
    void manifestError;
    return null;
  }
  if (
    robotModelKey !== null &&
    urdfManifest.model_key !== undefined &&
    urdfManifest.model_key !== robotModelKey
  ) {
    return null;
  }

  const modelKey = urdfManifest.model_key ?? urdfManifest.robot_id ?? 'unknown';
  const linkIndices = new Map(
    (robotDescriptor?.link_order ?? []).map((name, index) => [name, index]),
  );

  return (
    <>
      <group name="manyforge-urdf" key={modelKey}>
        {urdfManifest.links.map((link) => {
          const linkIndex = linkIndices.get(link.name);
          if (linkIndex === undefined) return null;
          return (
            <BoundRobotLink
              key={`${modelKey}:${link.name}`}
              robotIndex={0}
              linkIndex={linkIndex}
            >
              {link.visuals.map((visual) => (
                <RobotLinkVisual
                  key={`${modelKey}:${link.name}:${visual.index}:${visual.mesh_url ?? 'primitive'}`}
                  visual={visual}
                />
              ))}
              {collisionMeshesVisible && (link.collisions ?? []).map((collision) => (
                <RobotLinkCollision
                  key={`${modelKey}:collision:${link.name}:${collision.index}:${collision.mesh_url ?? 'primitive'}`}
                  collision={collision}
                />
              ))}
            </BoundRobotLink>
          );
        })}
      </group>
      <MeasuredRobotGhosts manifest={urdfManifest} modelKey={modelKey} />
    </>
  );
}

function BoundRobotLink({
  robotIndex,
  linkIndex,
  children,
}: {
  robotIndex: number;
  linkIndex: number;
  children: ReactNode;
}) {
  const [node, setNode] = useState<Group | null>(null);
  useViewerFrameBinding(node, (
    target,
    _manifest,
    frame,
    previousFrame,
    interpolationAlpha,
  ) => (
    applyViewerPose(
      target,
      frame.robot_blocks[robotIndex]?.link_transforms[linkIndex] ?? null,
      previousFrame?.robot_blocks[robotIndex]?.link_transforms[linkIndex] ?? null,
      interpolationAlpha,
    )
  ));
  return (
    <group ref={setNode} visible={false}>
      {children}
    </group>
  );
}

function MeasuredRobotGhosts({
  manifest,
  modelKey,
}: {
  manifest: SceneUrdfManifest;
  modelKey: string;
}) {
  const visible = useViewerSelectionStore((s) => s.measuredStateGhostVisible);
  const viewerManifest = useSceneStateStore((state) => state.manifest);
  const ghosts = useMemo(() => {
    if (!visible) return [];
    return (viewerManifest?.pose_sources ?? [])
      .map((source) => {
        const opacity = measuredRobotGhostOpacity(source);
        if (opacity === null) return null;
        return {
          key: source.pose_source_id,
          opacity,
          staleAfterS: source.stale_after_s,
        };
      })
      .filter((item): item is {
        key: string;
        opacity: number;
        staleAfterS: number;
      } => item !== null);
  }, [viewerManifest, visible]);

  if (!visible || ghosts.length === 0) {
    return null;
  }

  return (
    <group name="manyforge-measured-robot-ghosts" renderOrder={18}>
      {ghosts.map((ghost) => (
        <BoundMeasuredRobotGhost
          key={`${modelKey}:measured:${ghost.key}`}
          sourceId={ghost.key}
          staleAfterS={ghost.staleAfterS}
        >
          {manifest.links.map((link) => {
            return (
              <BoundMeasuredRobotLink
                key={`${modelKey}:measured:${ghost.key}:${link.name}`}
                sourceId={ghost.key}
                linkName={link.name}
              >
                {link.visuals.map((visual) => (
                  <GhostVisual
                    key={`${modelKey}:measured:${ghost.key}:${link.name}:${visual.index}`}
                    visual={visual}
                    opacity={ghost.opacity}
                    color="#22d3ee"
                  />
                ))}
              </BoundMeasuredRobotLink>
            );
          })}
        </BoundMeasuredRobotGhost>
      ))}
    </group>
  );
}

function BoundMeasuredRobotGhost({
  sourceId,
  staleAfterS,
  children,
}: {
  sourceId: string;
  staleAfterS: number;
  children: ReactNode;
}) {
  const [node, setNode] = useState<Group | null>(null);
  useViewerFrameBinding(node, (target, _manifest, frame) => {
    const source = frame.pose_source_states.find((item) => item.pose_source_id === sourceId);
    const measured = source?.measured_robot ?? null;
    const ageS = measured?.age_s;
    target.visible = measured !== null
      && measured.link_poses !== undefined
      && Object.keys(measured.link_poses).length > 0
      && !(typeof ageS === 'number' && ageS > staleAfterS);
    return 0;
  });
  return (
    <group ref={setNode} name={`measured-robot:${sourceId}`} visible={false}>
      {children}
    </group>
  );
}

function BoundMeasuredRobotLink({
  sourceId,
  linkName,
  children,
}: {
  sourceId: string;
  linkName: string;
  children: ReactNode;
}) {
  const [node, setNode] = useState<Group | null>(null);
  useViewerFrameBinding(node, (
    target,
    _manifest,
    frame,
    previousFrame,
    interpolationAlpha,
  ) => {
    const source = frame.pose_source_states.find((item) => item.pose_source_id === sourceId);
    const record = source?.measured_robot?.link_poses?.[linkName];
    const pose = poseFromRecord(record);
    target.visible = pose !== null;
    if (pose === null) return 0;
    const previousSource = previousFrame?.pose_source_states.find(
      (item) => item.pose_source_id === sourceId,
    );
    const previousPose = poseFromRecord(
      previousSource?.measured_robot?.link_poses?.[linkName],
    );
    return applyPoseComponents(
      target,
      pose.position,
      pose.quaternion,
      previousPose?.position ?? null,
      previousPose?.quaternion ?? null,
      interpolationAlpha,
    );
  });
  return <group ref={setNode} visible={false}>{children}</group>;
}

function RobotLinkCollision({ collision }: { collision: SceneUrdfCollision }) {
  const offsetPos = collision.collision_origin.position_m as [number, number, number];
  const offsetQuat = urdfOriginQuaternion(collision.collision_origin);
  if (collision.mesh_url !== null) {
    return (
      <group position={offsetPos} quaternion={offsetQuat}>
        <CollisionMeshGltf url={collision.mesh_url} />
      </group>
    );
  }
  if (collision.primitive !== null) {
    return (
      <group position={offsetPos} quaternion={offsetQuat}>
        <CollisionPrimitiveMesh primitive={collision.primitive} />
      </group>
    );
  }
  return null;
}

function RobotLinkVisual({ visual }: { visual: SceneUrdfVisual }) {
  const offsetPos = visual.visual_origin.position_m as [number, number, number];
  const offsetQuat = visualOriginQuaternion(visual);
  // Mesh-backed visual — let drei's useGLTF stream the per-visual GLB.
  if (visual.mesh_url !== null) {
    return (
      <group position={offsetPos} quaternion={offsetQuat}>
        <RobotLinkGltf url={visual.mesh_url} />
      </group>
    );
  }
  // Primitive visual — rendered natively to avoid a server round-trip for
  // shapes like UR5's tool0 boxes that make up the rudimentary gripper.
  if (visual.primitive !== null) {
    return (
      <group position={offsetPos} quaternion={offsetQuat}>
        <PrimitiveMesh primitive={visual.primitive} />
      </group>
    );
  }
  return null;
}

function RobotLinkGltf({ url }: { url: string }) {
  const gltf = useGLTF(url);
  const cloned = useMemo(() => gltf.scene.clone(true), [gltf.scene]);
  return <primitive key={url} object={cloned} />;
}

function PrimitiveMesh({
  primitive,
}: {
  primitive: NonNullable<SceneUrdfVisual['primitive']>;
}) {
  if (primitive.type === 'box') {
    const dims = primitive.size_m as [number, number, number];
    return (
      <mesh>
        <boxGeometry args={dims} />
        <meshStandardMaterial color="#a8aab0" metalness={0.2} roughness={0.6} />
      </mesh>
    );
  }
  if (primitive.type === 'cylinder') {
    // URDF cylinders point along Z by convention; Three.js cylinder
    // default axis is Y — rotate +π/2 about X to align.
    return (
      <mesh rotation={[Math.PI / 2, 0, 0]}>
        <cylinderGeometry
          args={[primitive.radius_m, primitive.radius_m, primitive.length_m, 24]}
        />
        <meshStandardMaterial color="#a8aab0" metalness={0.2} roughness={0.6} />
      </mesh>
    );
  }
  if (primitive.type === 'sphere') {
    return (
      <mesh>
        <sphereGeometry args={[primitive.radius_m, 24, 16]} />
        <meshStandardMaterial color="#a8aab0" metalness={0.2} roughness={0.6} />
      </mesh>
    );
  }
  return null;
}

function CollisionMeshGltf({ url }: { url: string }) {
  const gltf = useGLTF(url);
  const cloned = useMemo(() => {
    const c = gltf.scene.clone(true);
    c.traverse((obj) => {
      const mesh = obj as THREE.Mesh;
      if (!mesh.isMesh) return;
      const sourceMaterials = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
      const collisionMaterials = sourceMaterials.map((src) => {
        const mat = src.clone() as THREE.MeshStandardMaterial;
        if (mat.color) {
          mat.color.set('#f59e0b');
        }
        if (mat.emissive) {
          mat.emissive.set('#7c2d12');
        }
        mat.transparent = true;
        mat.opacity = 0.42;
        mat.wireframe = true;
        mat.depthWrite = false;
        mat.side = THREE.DoubleSide;
        return mat;
      });
      mesh.material = Array.isArray(mesh.material)
        ? collisionMaterials
        : collisionMaterials[0];
      mesh.renderOrder = 19;
    });
    return c;
  }, [gltf.scene]);
  return <primitive key={url} object={cloned} />;
}

function CollisionPrimitiveMesh({
  primitive,
}: {
  primitive: NonNullable<SceneUrdfCollision['primitive']>;
}) {
  const material = (
    <meshStandardMaterial
      color="#f59e0b"
      emissive="#7c2d12"
      transparent
      opacity={0.42}
      wireframe
      depthWrite={false}
      side={THREE.DoubleSide}
    />
  );
  if (primitive.type === 'box') {
    const dims = primitive.size_m as [number, number, number];
    return (
      <mesh renderOrder={19}>
        <boxGeometry args={dims} />
        {material}
      </mesh>
    );
  }
  if (primitive.type === 'cylinder') {
    return (
      <mesh rotation={[Math.PI / 2, 0, 0]} renderOrder={19}>
        <cylinderGeometry
          args={[primitive.radius_m, primitive.radius_m, primitive.length_m, 24]}
        />
        {material}
      </mesh>
    );
  }
  if (primitive.type === 'sphere') {
    return (
      <mesh renderOrder={19}>
        <sphereGeometry args={[primitive.radius_m, 24, 16]} />
        {material}
      </mesh>
    );
  }
  return null;
}

function numberTriple(
  value: unknown,
  fallback: [number, number, number],
): [number, number, number] {
  if (Array.isArray(value) && value.length === 3) {
    return [
      Number(value[0]) || 0,
      Number(value[1]) || 0,
      Number(value[2]) || 0,
    ];
  }
  return fallback;
}

function tuple3(value: unknown): [number, number, number] | null {
  if (!Array.isArray(value) || value.length !== 3) return null;
  const next = value.map(Number);
  if (next.some((entry) => !Number.isFinite(entry))) return null;
  return [next[0], next[1], next[2]];
}

function tuple4(value: unknown): [number, number, number, number] | null {
  if (!Array.isArray(value) || value.length !== 4) return null;
  const next = value.map(Number);
  if (next.some((entry) => !Number.isFinite(entry))) return null;
  return [next[0], next[1], next[2], next[3]];
}

function isRenderableWorldPoseFrame(poseFrameId: string, sourceFrameId: string): boolean {
  const frame = poseFrameId || sourceFrameId;
  return frame === '' || frame === 'universe' || frame === 'world' || frame === '/World';
}

// URDF RPY is fixed-axis ZYX; Three.js's default Euler order is intrinsic
// XYZ. Feeding RPY straight into <group rotation={...}> silently produces
// the wrong matrix for any compound rotation (UR10E upperarm/forearm have
// rpy="π/2 0 -π/2", which is where the disconnected-pieces bug came from).
// The backend now ships an explicit quaternion; we use that when present
// and fall back to a fixed-axis ZYX construction only for old payloads.
function visualOriginQuaternion(
  visual: SceneUrdfVisual,
): [number, number, number, number] {
  return urdfOriginQuaternion(visual.visual_origin);
}

function urdfOriginQuaternion(origin: {
  orientation_xyzw?: number[];
  orientation_rpy_rad: number[];
}): [number, number, number, number] {
  const xyzw = origin.orientation_xyzw;
  if (Array.isArray(xyzw) && xyzw.length === 4) {
    return [
      Number(xyzw[0]) || 0,
      Number(xyzw[1]) || 0,
      Number(xyzw[2]) || 0,
      Number(xyzw[3]) || 1,
    ];
  }
  const rpy = origin.orientation_rpy_rad;
  const roll = Number(rpy?.[0]) || 0;
  const pitch = Number(rpy?.[1]) || 0;
  const yaw = Number(rpy?.[2]) || 0;
  // Quaternion for fixed-axis (extrinsic) X then Y then Z rotation —
  // equivalently URDF "roll-pitch-yaw" convention.
  const cr = Math.cos(roll / 2);
  const sr = Math.sin(roll / 2);
  const cp = Math.cos(pitch / 2);
  const sp = Math.sin(pitch / 2);
  const cy = Math.cos(yaw / 2);
  const sy = Math.sin(yaw / 2);
  return [
    sr * cp * cy - cr * sp * sy,
    cr * sp * cy + sr * cp * sy,
    cr * cp * sy - sr * sp * cy,
    cr * cp * cy + sr * sp * sy,
  ];
}

function FrameAxes() {
  // The manifest owns ordered transforms. Presentation previews the shared
  // program draft used by both controls, then falls back to the applied scene
  // projection, so neither UI surface nor the canvas can drift.
  const manifest = useSceneStateStore((s) => s.manifest);
  const frameProjection = useSceneStateStore((s) => s.frames);
  const viewerSettings = useProgramStore((s) => s.viewerSettings);
  const projectedFrames = frameProjection?.frames ?? EMPTY_FRAME_STATES;
  const frames = manifest?.frames ?? [];
  const gizmoSize = viewerSettings?.frameGizmoSizeM
    ?? frameProjection?.frameGizmoSizeM
    ?? manifest?.frame_axes_gizmo_size_m
    ?? 0.1;
  const visibilityByName = useMemo(
    () => new Map(projectedFrames.map((frame) => [frame.name, frame.viewerVisible])),
    [projectedFrames],
  );
  return (
    <group name="manyforge-frame-axes">
      {frames.map((frame, frameIndex) => (
        (
          viewerSettings?.frameVisibility[frame.name]
          ?? visibilityByName.get(frame.name)
          ?? frame.viewer_visible
        )
          ? <BoundFrameAxes key={frame.name} frameIndex={frameIndex} size={gizmoSize} />
          : null
      ))}
    </group>
  );
}

const EMPTY_FRAME_STATES: import('../types/api').SceneFrameState[] = [];

function BoundFrameAxes({ frameIndex, size }: { frameIndex: number; size: number }) {
  const [node, setNode] = useState<Group | null>(null);
  useViewerFrameBinding(node, (target, _manifest, frame) => (
    applyViewerPose(target, frame.frame_transforms[frameIndex] ?? null)
  ));
  return (
    <group ref={setNode} visible={false}>
      <axesHelper args={[size]} />
    </group>
  );
}

function TcpJogGizmo() {
  // End-effector authoring gizmo (author-only, no motion).
  //
  // The gizmo is anchored to an invisible mesh. With no active draft it rests
  // at the live TCP pose (read from scene state); while a Cartesian draft
  // candidate is active it rests at the authored target. Dragging moves the
  // anchor for visual authoring only: it does NOT move the robot. On release it
  // authors a validated candidate and stays at that target (no snap-back). The
  // robot moves only via an explicit Approve.
  //
  // Robot-tab focus + cartesian-mode gate. The gizmo only mounts when
  // BOTH conditions hold: the operator has the Robot panel open AND the
  // robot panel's mode is set to Cartesian. Switching the active panel
  // away — or switching robot mode to Joint — unmounts the gizmo.
  const robotPanelActive = useViewerSelectionStore((s) => s.activePanel === 'robot');
  const robotMode = useViewerSelectionStore((s) => s.robotMode);
  const cartesianFrame = useViewerSelectionStore((s) => s.cartesianFrame);
  const gizmoMode = useViewerSelectionStore((s) => s.cartesianGizmoMode);
  const commandGate = useRobotCommandAuthority();
  const author = useRobotControlCandidateStore((s) => s.author);
  const rejectDraft = useRobotControlCandidateStore((s) => s.rejectDraft);
  const candidate = useRobotControlCandidateStore((s) => s.candidate);
  const viewerManifest = useSceneStateStore((s) => s.manifest);
  const robotDescriptor = viewerManifest?.robots[0] ?? null;
  const tcpFrame = robotDescriptor?.tcp_frame ?? null;
  const baseFrame = robotDescriptor?.base_frame ?? null;
  const staticBasePose = useMemo(
    () => poseFromBasePoseRecord(robotDescriptor?.base_pose),
    [robotDescriptor?.base_pose],
  );
  const [anchor, setAnchor] = useState<Mesh | null>(null);
  const draggingRef = useRef(false);

  const currentBaseWorldPose = useCallback((): PoseTuple | null => {
    const snapshot = getLatestViewerSnapshot();
    if (snapshot !== null && baseFrame !== null) {
      const pose = viewerRobotLinkPose(snapshot.manifest, snapshot.frame, 0, baseFrame);
      if (pose !== null) {
        return {
          position: tuple3(pose.position_m) ?? [0, 0, 0],
          quaternion: tuple4(pose.orientation_xyzw) ?? [0, 0, 0, 1],
        };
      }
    }
    return staticBasePose;
  }, [baseFrame, staticBasePose]);

  // Author a Cartesian candidate from the gizmo anchor's current world pose
  // (set by TransformControls during drag), expressed in the robot base frame
  // — exactly the shape the numeric Cartesian fields use. AUTHORING ONLY: this
  // creates/updates the server-owned candidate and never moves the robot. The
  // anchor then rests at the authored target (it is not snapped back to the
  // live TCP); the validated candidate ghost shows the planned configuration.
  const authorFromAnchor = useCallback(() => {
    if (!commandGate.canCommand) return;
    if (anchor === null) return;
    // Read the anchor's world transform (top-level scene child; its local
    // position/quaternion are world coordinates).
    const anchorWorld: PoseTuple = {
      position: [anchor.position.x, anchor.position.y, anchor.position.z],
      quaternion: [
        anchor.quaternion.x,
        anchor.quaternion.y,
        anchor.quaternion.z,
        anchor.quaternion.w,
      ],
    };
    // Author the target in the robot base frame, or fail closed when the base
    // frame cannot be resolved (baseWorldPose unknown): authorGizmoRelease then
    // does NOT author and instead makes any prior draft non-approvable via
    // rejectDraft, so a mis-framed pose can never be authored or approved.
    authorGizmoRelease(anchorWorld, currentBaseWorldPose(), tcpFrame, {
      author,
      rejectDraft,
    });
  }, [
    anchor,
    author,
    commandGate.canCommand,
    currentBaseWorldPose,
    rejectDraft,
    tcpFrame,
  ]);

  const initialPose = useMemo(() => {
    const snapshot = getLatestViewerSnapshot();
    const tcpWorldPose = snapshot !== null && tcpFrame !== null
      ? viewerRobotLinkPose(snapshot.manifest, snapshot.frame, 0, tcpFrame)
      : null;
    if (tcpWorldPose !== null) {
      const position = tuple3(tcpWorldPose.position_m);
      const quaternion = tuple4(tcpWorldPose.orientation_xyzw);
      if (position !== null && quaternion !== null) return { position, quaternion };
    }
    const tcpInBase = poseFromRecord(snapshot?.frame.robot_blocks[0]?.tcp_pose_in_base);
    if (tcpInBase === null) return null;
    const baseWorldPose = currentBaseWorldPose();
    if (baseWorldPose !== null) {
      return composePose(baseWorldPose, tcpInBase);
    }
    return tcpInBase;
  }, [currentBaseWorldPose, tcpFrame]);

  // While a Cartesian draft candidate is active, the gizmo rests at the
  // AUTHORED TARGET (the candidate's pose), not the live TCP — so editing is
  // iterative and a release does not snap the gizmo back. The target is in the
  // base frame; convert it to world. Falls back to live TCP when there is no
  // active draft (e.g. after Revert), when the candidate is handing off to
  // execution, or for an exotic non-base frame we cannot convert.
  const draftTargetWorld = useMemo(() => {
    const baseWorldPose = currentBaseWorldPose();
    if (candidate === null || baseWorldPose === null) return null;
    const target = candidate.cartesianTarget;
    if (target == null) return null;
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
    if (target.frameId && target.frameId !== 'base' && target.frameId !== baseFrame) {
      return null;
    }
    return composePose(baseWorldPose, {
      position: target.positionM as [number, number, number],
      quaternion: target.orientationXyzw as [number, number, number, number],
    });
  }, [baseFrame, candidate, currentBaseWorldPose]);

  const anchorPose = draftTargetWorld ?? initialPose;

  useViewerFrameBinding(anchor, (target, manifest, frame) => {
    if (draggingRef.current || draftTargetWorld !== null || tcpFrame === null) return 0;
    const tcpWorldPose = viewerRobotLinkPose(manifest, frame, 0, tcpFrame);
    if (tcpWorldPose !== null) return applyViewerPose(target, tcpWorldPose);
    const tcpInBase = poseFromRecord(frame.robot_blocks[0]?.tcp_pose_in_base);
    if (tcpInBase === null) return 0;
    const basePoseRecord = baseFrame === null
      ? null
      : viewerRobotLinkPose(manifest, frame, 0, baseFrame);
    const basePose = basePoseRecord === null
      ? staticBasePose
      : {
        position: tuple3(basePoseRecord.position_m) ?? [0, 0, 0],
        quaternion: tuple4(basePoseRecord.orientation_xyzw) ?? [0, 0, 0, 1],
      };
    const worldPose = basePose === null ? tcpInBase : composePose(basePose, tcpInBase);
    target.position.set(...worldPose.position);
    target.quaternion.set(...worldPose.quaternion);
    return 1;
  });

  useEffect(() => {
    if (anchor === null || anchorPose === null) return;
    if (draggingRef.current) return;
    anchor.position.set(...anchorPose.position);
    anchor.quaternion.set(...anchorPose.quaternion);
  }, [anchor, anchorPose, robotMode]);

  // Single-shot drag release. Both the inline TransformControls onMouseUp and
  // the window pointerup/pointercancel safety net route through this one path.
  // The draggingRef guard makes whichever fires SECOND a no-op, so a release
  // authors EXACTLY ONCE from the dragged anchor pose. The anchor is NOT
  // snapped back to the live TCP: it stays at the authored target so editing is
  // iterative (the new draft candidate's target keeps `anchorPose` there). It
  // returns to the live TCP only when the draft is discarded (Revert / mode
  // change) or hands off to execution. Authoring only; never moves the robot.
  const finishDrag = useCallback(() => {
    if (!draggingRef.current) return;
    draggingRef.current = false;
    useViewerSelectionStore.getState().setDraggingGizmo(false);
    authorFromAnchor();
  }, [authorFromAnchor]);

  // Window-level safety net: if drei TransformControls drops its mouseUp
  // event, the window pointerup/pointercancel still finishes the drag.
  // Routed through the single-shot finishDrag path so it can never
  // double-author with the inline handler.
  useEffect(() => {
    const onPointerUp = () => finishDrag();
    window.addEventListener('pointerup', onPointerUp);
    window.addEventListener('pointercancel', onPointerUp);
    return () => {
      window.removeEventListener('pointerup', onPointerUp);
      window.removeEventListener('pointercancel', onPointerUp);
    };
  }, [finishDrag]);

  if (
    !robotPanelActive
    || robotMode !== 'cartesian'
    || tcpFrame === null
    || !commandGate.canCommand
  ) {
    return null;
  }

  const gizmoSpace: 'world' | 'local' = cartesianFrame === 'tcp' ? 'local' : 'world';

  return (
    <>
      <mesh
        ref={(m) => {
          if (m !== null && anchorPose !== null && !draggingRef.current) {
            m.position.set(...anchorPose.position);
            m.quaternion.set(...anchorPose.quaternion);
          }
          setAnchor(m);
        }}
        visible={false}
      >
        <sphereGeometry args={[0.005, 8, 6]} />
      </mesh>
      {anchor !== null && (
        <TransformControls
          object={anchor}
          mode={gizmoMode}
          space={gizmoSpace}
          onMouseDown={() => {
            if (!commandGate.canCommand) return;
            if (draggingRef.current) return;
            draggingRef.current = true;
            useViewerSelectionStore.getState().setDraggingGizmo(true);
          }}
          // Single-shot release through finishDrag: authors exactly once and
          // leaves the gizmo at the authored target (no snap-back); never moves
          // the robot.
          onMouseUp={() => finishDrag()}
        />
      )}
    </>
  );
}

/**
 * 3D ghost preview for the Robot panel's Move To tab.
 *
 * Reads the same store state as MoveToBody (source, entity selection,
 * orientation mode, offset) and computes the resulting target pose
 * through the shared `utils/moveTo.ts` helper — so the operator sees
 * exactly the pose that will be validated as a candidate.
 *
 * Renders an axesHelper at the target (red/green/blue = x/y/z of the
 * TCP frame) plus a label tag for orientation context. Joint-goal
 * targets are skipped (no cartesian pose to anchor an axes preview
 * to; the operator gets a less-detailed text preview in the panel).
 */
function MoveToTargetGhost() {
  const robotPanelActive = useViewerSelectionStore((s) => s.activePanel === 'robot');
  const robotMode = useViewerSelectionStore((s) => s.robotMode);
  const moveTo = useViewerSelectionStore((s) => s.moveTo);
  const manifest = useSceneStateStore((s) => s.manifest);
  const appliedTree = useProgramStore((s) => s.appliedTree);

  const storedPoses = useMemo(() => {
    const out: { nodeName: string; motionType: 'pose' | 'joint'; params: Record<string, unknown> }[] = [];
    function visit(node: TreeNode | null): void {
      if (node === null) return;
      if (node.id === 'move_manipulator_action') {
        const mt = node.params['motion_type'];
        if (mt === 'pose' || mt === 'joint') {
          out.push({ nodeName: node.name, motionType: mt, params: node.params });
        }
      }
      for (const child of node.children ?? []) visit(child);
    }
    visit(appliedTree);
    return out;
  }, [appliedTree]);

  const target = useMemo(() => {
    if (!robotPanelActive || robotMode !== 'move-to') return null;
    const authoritative = getAuthoritativeViewerState();
    const objects = authoritative?.scene.objects
      ?? manifest?.entities.map(manifestEntityToSceneObject)
      ?? EMPTY_OBJECTS;
    const frames: SceneFrameState[] = authoritative?.frames.frames.filter(
      (frame) => frame.poseAvailable,
    ) ?? [];
    return computeMoveToTarget({
      moveTo,
      objects: objects.filter((o) => !o.attached),
      frames,
      storedPoses,
      tcpPose: authoritative?.scene.robot?.tcp_pose_in_base ?? null,
    });
  }, [robotPanelActive, robotMode, moveTo, storedPoses, manifest]);

  if (target === null || target.kind !== 'cartesian') return null;

  return (
    <group position={target.position} quaternion={target.quaternion}>
      <axesHelper args={[0.12]} />
    </group>
  );
}

/**
 * Validated Robot Control candidate ghost (Phase 4-P5.2c).
 *
 * Renders the server-owned candidate's final configuration as a translucent
 * overlay, separate from the authoritative robot in `/api/scene/state`:
 *   - a valid (ready) candidate shows a neutral ghost robot at its final
 *     joint configuration;
 *   - a blocked candidate shows only a red TCP marker at the requested target
 *     pose. Do not render a full robot for blocked targets: when IK/collision
 *     validation fails, any prior/reused joint ghost would imply a reachable
 *     configuration that does not actually exist for this candidate.
 *
 * It prefers the per-link poses the backend computes (native FK at a
 * hypothetical configuration); when absent it falls back to a debounced
 * preview-link-poses fetch for the final joint vector. Gripper candidates use
 * that same no-motion FK path with the staged gripper joint and render only the
 * gripper subtree, avoiding a duplicate full robot. Never mutates scene state
 * and never moves the robot.
 */
const _CANDIDATE_GHOST_ACTIVE = new Set([
  'planning',
  'ready',
  'blocked',
  'stale',
  'approving',
]);

function CandidateGhostRobot() {
  const candidate = useRobotControlCandidateStore((s) => s.candidate);
  const viewerManifest = useSceneStateStore((s) => s.manifest);
  const authoritative = getAuthoritativeViewerState();
  const robot = authoritative?.scene.robot ?? null;
  const grippers = authoritative?.scene.grippers ?? EMPTY_GRIPPERS;
  const robotDescriptor = viewerManifest?.robots[0] ?? null;
  const robotModelKey = robotDescriptor?.urdf_model_key ?? robotDescriptor?.robot_id ?? null;
  const tcpFrame = robotDescriptor?.tcp_frame ?? null;
  const [manifest, setManifest] = useState<SceneUrdfManifest | null>(null);
  const [fetchedPreview, setFetchedPreview] = useState<{
    familyKey: string;
    response: PreviewLinkPosesResponse;
  } | null>(null);
  const fetchSeqRef = useRef(0);
  // The joint-vector key the current fetchedPreview was computed for. Used to
  // refetch ONLY when the target configuration actually changes, never on a
  // mere candidate-object identity change (each status poll / debounce tick
  // produces a new object with the same pose) — that churn was the ghost
  // flicker.
  const lastFetchKeyRef = useRef<string | null>(null);

  const status = candidate?.status ?? null;
  const active = status !== null && _CANDIDATE_GHOST_ACTIVE.has(status);
  const gripperPreview = useMemo(
    () =>
      active
        ? gripperGhostPreviewFromCandidate(candidate, robot, grippers)
        : null,
    [active, candidate, robot, grippers],
  );
  // Gripper candidates have no arm trajectory/ghost payload. Render them from
  // a local FK preview of the staged gripper joint instead, and never reuse a
  // carried arm ghost while the active candidate is gripper-mode.
  const ghost =
    active && candidate?.inputMode !== 'gripper'
      ? candidate?.ghost ?? null
      : null;
  const previewFamilyKey = useMemo(() => {
    if (!active) return 'inactive';
    if (gripperPreview !== null) {
      return `gripper:${gripperPreview.gripperId}:${gripperPreview.linkNames.join('\0')}`;
    }
    return candidate?.inputMode === 'gripper' ? 'gripper-unavailable' : 'arm';
  }, [active, candidate?.inputMode, gripperPreview]);

  useEffect(() => {
    setFetchedPreview(null);
    lastFetchKeyRef.current = null;
  }, [previewFamilyKey]);

  useEffect(() => {
    if (!active) {
      setManifest(null);
      // Candidate discarded / handed off: drop the cached preview so the next
      // candidate fetches fresh.
      setFetchedPreview(null);
      lastFetchKeyRef.current = null;
      return;
    }
    let cancelled = false;
    void api.urdf
      .manifest()
      .then((m) => {
        if (!cancelled) setManifest(m);
      })
      .catch(() => {
        if (!cancelled) setManifest(null);
      });
    return () => {
      cancelled = true;
    };
  }, [active, robotModelKey]);

  // Per-link poses the backend already computed (native FK), if present.
  const directLinkPoses = useMemo<
    Record<string, { position_m: number[]; orientation_xyzw: number[] }> | null
  >(() => {
    const linkPoses = ghost?.linkPoses;
    if (!linkPoses || Object.keys(linkPoses).length === 0) return null;
    const out: Record<string, { position_m: number[]; orientation_xyzw: number[] }> = {};
    for (const [name, pose] of Object.entries(linkPoses)) {
      out[name] = {
        position_m: pose.positionM,
        orientation_xyzw: pose.orientationXyzw,
      };
    }
    return out;
  }, [ghost]);

  const finalJointNames =
    gripperPreview?.request.joint_names ?? ghost?.finalJointNames ?? null;
  const finalJointPositions =
    gripperPreview?.request.joint_positions_rad ?? ghost?.finalJointPositionsRad ?? null;
  const previewLinkNames = useMemo(() => {
    if (manifest === null) return null;
    if (gripperPreview !== null) return gripperPreview.linkNames;
    return manifest.links.map((l) => l.name);
  }, [manifest, gripperPreview]);
  const renderLinkNameSet = useMemo(
    () => (gripperPreview !== null ? new Set(gripperPreview.linkNames) : null),
    [gripperPreview],
  );

  // Stable key for the final joint vector (rounded so float jitter does not
  // churn it). Null when there is no usable joint vector to fetch for.
  const jointKey = useMemo(() => {
    if (
      previewLinkNames === null ||
      finalJointNames === null ||
      finalJointPositions === null ||
      finalJointNames.length !== finalJointPositions.length ||
      finalJointPositions.length === 0
    ) {
      return null;
    }
    const rounded = finalJointPositions.map((p) => Math.round(p * 1e6) / 1e6);
    return JSON.stringify([finalJointNames, rounded, previewLinkNames]);
  }, [finalJointNames, finalJointPositions, previewLinkNames]);

  // Fallback: fetch link poses for the final joint vector when the backend did
  // not inline them (e.g. non-native backends). Refetch ONLY when the joint
  // vector actually changes, and KEEP the previous preview while a new fetch is
  // pending — so the ghost stays put instead of blanking on every status poll
  // or debounce tick (that was the flicker).
  useEffect(() => {
    if (
      !active ||
      manifest === null ||
      directLinkPoses !== null ||
      jointKey === null ||
      previewLinkNames === null ||
      previewLinkNames.length === 0 ||
      finalJointNames === null ||
      finalJointPositions === null
    ) {
      return;
    }
    if (lastFetchKeyRef.current === jointKey) {
      return; // already showing this joint vector; do not refetch or blank
    }
    const seq = fetchSeqRef.current + 1;
    fetchSeqRef.current = seq;
    const handle = window.setTimeout(() => {
      void api.scene
        .previewLinkPoses({
          kind: 'joint',
          joint_names: finalJointNames,
          joint_positions_rad: finalJointPositions,
          link_names: previewLinkNames,
        })
        .then((r) => {
          if (seq === fetchSeqRef.current) {
            lastFetchKeyRef.current = jointKey;
            setFetchedPreview({ familyKey: previewFamilyKey, response: r });
          }
        })
        .catch(() => {
          // Keep the previous preview on failure (no blank/flicker).
        });
    }, 200);
    return () => window.clearTimeout(handle);
  }, [
    active,
    manifest,
    directLinkPoses,
    jointKey,
    previewFamilyKey,
    previewLinkNames,
    finalJointNames,
    finalJointPositions,
  ]);

  if (!active || manifest === null) return null;
  if (candidate?.inputMode === 'gripper') {
    if (gripperPreview === null) return null;
  } else if (ghost === null) {
    return null;
  }
  if (
    robotModelKey !== null &&
    manifest.model_key !== undefined &&
    manifest.model_key !== robotModelKey
  ) {
    return null;
  }

  const blocked = ghost?.blocked ?? false;
  // A carried-forward ghost during planning/stale/approving is shown DIMMED and
  // neutral-gray so it reads as "pending, not validated" and never implies a
  // reachable/approvable target (Approve keys off status === 'ready'). A ready
  // ghost is the normal cyan; a blocked ghost is red.
  const pending =
    status === 'planning' || status === 'stale' || status === 'approving';
  const ghostColor = blocked ? '#ff3333' : pending ? '#9ca3af' : undefined;
  const ghostOpacity = blocked ? 0.5 : pending ? 0.28 : 0.5;
  const modelKey = manifest.model_key ?? manifest.robot_id ?? 'unknown';

  if (blocked) {
    const targetPose = ghost?.blockedTargetPose ?? ghost?.finalTcpPose ?? null;
    if (targetPose === null) return null;
    const tcpLink = tcpFrame
      ? manifest.links.find((l) => l.name === tcpFrame)
      : undefined;
    return (
      <group
        name="manyforge-candidate-ghost-tcp"
        key={`${modelKey}:candidate-tcp-blocked`}
        position={targetPose.positionM as [number, number, number]}
        quaternion={targetPose.orientationXyzw as [number, number, number, number]}
      >
        {tcpLink?.visuals.map((visual) => (
          <GhostVisual
            key={`${modelKey}:candidate-tcp-blocked:${visual.index}`}
            visual={visual}
            opacity={ghostOpacity}
            color="#ff3333"
          />
        ))}
        <axesHelper args={[0.12]} />
      </group>
    );
  }

  const fetchedPreviewForFamily =
    fetchedPreview?.familyKey === previewFamilyKey ? fetchedPreview.response : null;
  const fetchedLinkPoses =
    fetchedPreviewForFamily !== null && fetchedPreviewForFamily.reachable
      ? fetchedPreviewForFamily.link_poses
      : null;
  const linkPoses =
    directLinkPoses ??
    fetchedLinkPoses;

  if (linkPoses !== null) {
    return (
      <group name="manyforge-candidate-ghost" key={`${modelKey}:candidate`}>
        {manifest.links.map((link) => {
          if (renderLinkNameSet !== null && !renderLinkNameSet.has(link.name)) {
            return null;
          }
          const pose = linkPoses[link.name];
          if (!pose) return null;
          return (
            <group
              key={`${modelKey}:candidate:${link.name}`}
              position={pose.position_m as [number, number, number]}
              quaternion={
                pose.orientation_xyzw as [number, number, number, number]
              }
            >
              {link.visuals.map((visual) => (
                <GhostVisual
                  key={`${modelKey}:candidate:${link.name}:${visual.index}`}
                  visual={visual}
                  opacity={ghostOpacity}
                  color={ghostColor}
                />
              ))}
            </group>
          );
        })}
      </group>
    );
  }

  // No per-link configuration available. Fall back to a TCP marker so the
  // candidate remains visible even when FK link poses are unavailable.
  if (ghost === null) return null;
  const targetPose = ghost.finalTcpPose ?? null;
  if (targetPose !== null) {
    const tcpLink = tcpFrame
      ? manifest.links.find((l) => l.name === tcpFrame)
      : undefined;
    return (
      <group
        name="manyforge-candidate-ghost-tcp"
        key={`${modelKey}:candidate-tcp`}
        position={targetPose.positionM as [number, number, number]}
        quaternion={targetPose.orientationXyzw as [number, number, number, number]}
      >
        {tcpLink?.visuals.map((visual) => (
          <GhostVisual
            key={`${modelKey}:candidate-tcp:${visual.index}`}
            visual={visual}
            opacity={ghostOpacity}
            color={ghostColor}
          />
        ))}
        <axesHelper args={[0.12]} />
      </group>
    );
  }
  return null;
}

function GhostVisual({
  visual,
  opacity,
  color,
}: {
  visual: SceneUrdfVisual;
  opacity: number;
  color?: string;
}) {
  const offsetPos = visual.visual_origin.position_m as [number, number, number];
  const offsetQuat = visualOriginQuaternion(visual);
  if (visual.mesh_url !== null) {
    return (
      <group position={offsetPos} quaternion={offsetQuat}>
        <GhostMeshGltf url={visual.mesh_url} opacity={opacity} color={color} />
      </group>
    );
  }
  if (visual.primitive !== null) {
    return (
      <group position={offsetPos} quaternion={offsetQuat}>
        <GhostPrimitiveMesh primitive={visual.primitive} opacity={opacity} color={color} />
      </group>
    );
  }
  return null;
}

function GhostMeshGltf({
  url,
  opacity,
  color,
}: {
  url: string;
  opacity: number;
  color?: string;
}) {
  const gltf = useGLTF(url);
  const cloned = useMemo(() => {
    const c = gltf.scene.clone(true);
    c.traverse((obj) => {
      const m = obj as THREE.Mesh;
      if (!m.isMesh) return;
      const sourceMaterials = Array.isArray(m.material) ? m.material : [m.material];
      const tinted = sourceMaterials.map((src) => {
        const mat = src.clone() as THREE.MeshStandardMaterial;
        mat.transparent = true;
        mat.opacity = opacity;
        // depthWrite=false avoids the ghost writing its own depth
        // and causing nested-mesh transparency ordering artefacts
        // (a UR5 link has multiple visual submeshes that would
        // otherwise self-occlude when both transparent).
        mat.depthWrite = false;
        if (color !== undefined && (mat as THREE.MeshStandardMaterial).color) {
          (mat as THREE.MeshStandardMaterial).color.set(color);
        }
        return mat;
      });
      m.material = Array.isArray(m.material) ? tinted : tinted[0];
    });
    return c;
  }, [gltf.scene, opacity, color]);
  return <primitive key={url} object={cloned} />;
}

function GhostPrimitiveMesh({
  primitive,
  opacity,
  color,
}: {
  primitive: NonNullable<SceneUrdfVisual['primitive']>;
  opacity: number;
  color?: string;
}) {
  const matColor = color ?? '#a8aab0';
  if (primitive.type === 'box') {
    const dims = primitive.size_m as [number, number, number];
    return (
      <mesh>
        <boxGeometry args={dims} />
        <meshStandardMaterial
          color={matColor}
          metalness={0.1}
          roughness={0.7}
          transparent
          opacity={opacity}
          depthWrite={false}
        />
      </mesh>
    );
  }
  if (primitive.type === 'cylinder') {
    return (
      <mesh rotation={[Math.PI / 2, 0, 0]}>
        <cylinderGeometry
          args={[primitive.radius_m, primitive.radius_m, primitive.length_m, 24]}
        />
        <meshStandardMaterial
          color={matColor}
          metalness={0.1}
          roughness={0.7}
          transparent
          opacity={opacity}
          depthWrite={false}
        />
      </mesh>
    );
  }
  if (primitive.type === 'sphere') {
    return (
      <mesh>
        <sphereGeometry args={[primitive.radius_m, 24, 16]} />
        <meshStandardMaterial
          color={matColor}
          metalness={0.1}
          roughness={0.7}
          transparent
          opacity={opacity}
          depthWrite={false}
        />
      </mesh>
    );
  }
  return null;
}

function PointCloud() {
  usePointCloudSubscription();
  const pointsRef = useRef<THREE.Points>(null);
  const materialRef = useRef<THREE.PointsMaterial>(null);
  const invalidate = useThree((state) => state.invalidate);
  const capacityRef = useRef(0);

  useEffect(() => subscribePointCloudWake(invalidate), [invalidate]);
  useFrame(() => applyLatestPointCloudFrame(), -999);

  useEffect(() => {
    const node = pointsRef.current;
    if (node === null) return undefined;
    const geometry = node.geometry as THREE.BufferGeometry;
    return registerPointCloudApplier((frame) => {
      const material = materialRef.current;
      if (frame === null || frame.pointCount === 0) {
        node.visible = false;
        geometry.setDrawRange(0, 0);
        return;
      }
      node.visible = true;
      if (frame.pointCount > capacityRef.current) {
        let capacity = Math.max(256, capacityRef.current);
        while (capacity < frame.pointCount) capacity *= 2;
        capacityRef.current = capacity;
        geometry.setAttribute(
          'position',
          new THREE.BufferAttribute(new Float32Array(capacity * 3), 3),
        );
        geometry.setAttribute(
          'color',
          new THREE.BufferAttribute(new Uint8Array(capacity * 3), 3, true),
        );
      }
      const position = geometry.getAttribute('position') as THREE.BufferAttribute;
      (position.array as Float32Array).set(frame.positions);
      position.needsUpdate = true;
      const color = geometry.getAttribute('color') as THREE.BufferAttribute;
      const hasColors = frame.colors !== null;
      if (hasColors) (color.array as Uint8Array).set(frame.colors!);
      color.needsUpdate = hasColors;
      if (material !== null && material.vertexColors !== hasColors) {
        material.vertexColors = hasColors;
        material.color.set(hasColors ? '#ffffff' : '#8aaaff');
        material.needsUpdate = true;
      }
      geometry.setDrawRange(0, frame.pointCount);
    });
  }, []);

  return (
    <points ref={pointsRef} visible={false} frustumCulled={false}>
      <bufferGeometry />
      <pointsMaterial
        ref={materialRef}
        size={0.01}
        sizeAttenuation
        color="#8aaaff"
        vertexColors={false}
      />
    </points>
  );
}

function ObjectFrameAxes() {
  // Ordered transforms stay on the high-rate manifest/frame path. Object-axis
  // presentation previews the shared program draft, then falls back through
  // the applied scene projection to the initial manifest value.
  const manifest = useSceneStateStore((s) => s.manifest);
  const sceneProjection = useSceneStateStore((s) => s.scene);
  const viewerSettings = useProgramStore((s) => s.viewerSettings);
  const objects = manifest?.entities ?? [];
  const projectedObjects = sceneProjection?.objects ?? EMPTY_OBJECTS;
  const visibilityById = useMemo(
    () => new Map(projectedObjects.map((object) => [
      object.object_id,
      object.viewer_frame_visible,
    ])),
    [projectedObjects],
  );
  const gizmoSize = viewerSettings?.objectAxesGizmoSizeM
    ?? sceneProjection?.object_axes_gizmo_size_m
    ?? manifest?.object_axes_gizmo_size_m
    ?? 0.1;
  return (
    <group name="manyforge-object-frame-axes">
      {objects.map((obj, entityIndex) => {
        if (!(
          viewerSettings?.objectFrameVisibility[obj.object_id]
          ?? visibilityById.get(obj.object_id)
          ?? obj.viewer_frame_visible
        )) return null;
        return (
          <BoundObjectFrameAxes
            key={obj.object_id}
            entityIndex={entityIndex}
            size={gizmoSize}
          />
        );
      })}
    </group>
  );
}

function BoundObjectFrameAxes({
  entityIndex,
  size,
}: {
  entityIndex: number;
  size: number;
}) {
  const [node, setNode] = useState<Group | null>(null);
  useViewerFrameBinding(node, (
    target,
    _manifest,
    frame,
    previousFrame,
    interpolationAlpha,
  ) => (
    applyViewerPose(
      target,
      frame.entity_transforms[entityIndex] ?? null,
      previousFrame?.entity_transforms[entityIndex] ?? null,
      interpolationAlpha,
    )
  ));
  return (
    <group ref={setNode} visible={false}>
      <axesHelper args={[size]} />
    </group>
  );
}

function colorForSource(source: SceneObjectState['source']): string {
  switch (source) {
    case 'authored':
      return '#d18f3a';
    case 'attached':
      return '#5fa84a';
    case 'sensed':
      return '#4a8fd1';
    case 'runtime':
    default:
      return '#a07a3a';
  }
}
