import { Component, Suspense, useEffect, useMemo, useRef, useState } from 'react';
import type { ErrorInfo, MutableRefObject, ReactNode } from 'react';
import { Canvas } from '@react-three/fiber';
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
import { usePointCloudStore, usePointCloudSubscription } from '../store/pointCloudStore';
import { useProgramStore } from '../store/programStore';
import { useRobotControlCandidateStore } from '../store/robotControlCandidateStore';
import { useRobotCommandAuthority } from '../store/commandAuthority';
import { useSceneStateStore, useScenePolling } from '../store/sceneStateStore';
import { useStatusPolling, useStatusStore } from '../store/statusStore';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import type {
  MeshAssetStatus,
  PoseSourceStatusState,
  PreviewLinkPosesResponse,
  SceneFrameState,
  SceneGripperState,
  SceneObjectState,
  SceneShapeState,
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
import { ObjectEditPanel } from './ObjectEditPanel';
import { RobotControlPanel } from './RobotControlPanel';
import { SceneControls } from './SceneControls';
import { SnapshotResponder } from './SnapshotResponder';

// Module-level stable references for zustand selector fallbacks. Returning
// a new `[]` or `{}` from a selector each render makes Object.is-based
// equality always false, which can pump the React reconciler into an
// update loop when combined with downstream useMemo deps. Sharing the
// same identity across renders keeps zustand's bailout path working.
const EMPTY_OBJECTS: SceneObjectState[] = [];

const EMPTY_POSE_SOURCES: PoseSourceStatusState[] = [];

const EMPTY_LINK_POSES: Record<
  string,
  { position_m: number[]; orientation_xyzw: number[] }
> = {};

const EMPTY_GRIPPERS: SceneGripperState[] = [];

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
  useScenePolling();
  const runtime = useStatusStore((state) => state.runtime);
  const scene = useSceneStateStore((state) => state.scene);
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
  if (scene === null && sceneError === null) {
    backendAvailable = null;
  } else if (scene !== null) {
    backendAvailable = true;
  } else {
    backendAvailable = false;
    unavailableDetail =
      runtime?.userMessage
      || sceneError
      || 'Load a program/deployment to start the embedded 3D viewer.';
  }

  const handleContextMenu = (objectId: string, x: number, y: number): void => {
    setContextMenu({ objectId, x, y });
  };
  const closeContextMenu = (): void => setContextMenu(null);

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
            <SceneCanvas resizing={resizing} onContextMenu={handleContextMenu} />
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
  const objects = useSceneStateStore((s) => s.scene?.objects ?? EMPTY_OBJECTS);
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
    const pose = object.pose_in_universe as Record<string, unknown> | null;
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

function SceneCanvas({
  resizing,
  onContextMenu,
}: {
  resizing: boolean;
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
      frameloop="demand"
      gl={{
        antialias: true,
        powerPreference: 'high-performance',
        // Required for client-side scene-snapshot capture (Day 3): the
        // backend SnapshotBroker round-trips a render request to this
        // Canvas and reads pixels back via `toDataURL`, which yields
        // a transparent frame on browsers that auto-clear the
        // drawing buffer between paints.
        preserveDrawingBuffer: true,
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
      <Grid
        args={[10, 10]}
        cellSize={0.1}
        cellThickness={0.4}
        cellColor="#202830"
        sectionSize={1}
        sectionThickness={0.8}
        sectionColor="#3a4252"
        fadeDistance={20}
        fadeStrength={1.5}
        infiniteGrid
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
      <SnapshotResponder />
      {statsVisible && <Stats className="!left-2 !top-2" />}
    </Canvas>
  );
}

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
  const objects = useSceneStateStore((state) => state.scene?.objects ?? EMPTY_OBJECTS);
  const lockedIds = useViewerSelectionStore((s) => s.lockedIds);
  return (
    <group name="manyforge-scene-objects">
      {objects.map((obj) => (
        <SceneObjectMesh
          key={obj.object_id}
          object={obj}
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
  objectId: string;
  sourceId: string;
  position: [number, number, number];
  quaternion: [number, number, number, number];
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
  const poseSources = useSceneStateStore(
    (state) => state.scene?.pose_sources ?? EMPTY_POSE_SOURCES,
  );
  const objects = useSceneStateStore((state) => state.scene?.objects ?? EMPTY_OBJECTS);

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
    for (const source of poseSources) {
      const renderTcp = source.objects.some((observed) => (
        measuredGhostEnabled(observed.measured_state_ghost, 'robotTcp')
      ));
      if (renderTcp && source.measured_tcp) {
        const tcpPose = source.measured_tcp.pose;
        if (isRenderableWorldPoseFrame(tcpPose.frame_id, source.frame_id)) {
          const position = tuple3(tcpPose.position_xyz);
          const quaternion = tuple4(tcpPose.orientation_xyzw);
          if (position !== null && quaternion !== null) {
            next.push({
              key: `${source.pose_source_id}:measured_tcp`,
              objectId: source.measured_tcp.tcp_frame_id || 'measured_tcp',
              sourceId: source.pose_source_id,
              position,
              quaternion,
              shape: null,
              shapeType: null,
            });
          }
        }
      }
      for (const observed of source.objects) {
        if (observed.status !== 'fresh' || observed.update === null || observed.update === undefined) {
          continue;
        }
        if (!measuredGhostEnabled(observed.measured_state_ghost, 'object', true)) {
          continue;
        }
        const pose = observed.update.pose;
        // `observed.frame_id` is the configured coordinate frame for the
        // binding. `pose.frame_id` is provider metadata and may carry the
        // source entity frame/name (Isaac reports e.g. "mac_n_cheese_centered")
        // even though the numeric pose is expressed in the source frame.
        if (!isRenderableWorldPoseFrame(observed.frame_id, source.frame_id)) {
          continue;
        }
        const position = tuple3(pose.position_xyz);
        const quaternion = tuple4(pose.orientation_xyzw);
        if (position === null || quaternion === null) {
          continue;
        }
        const object = objectById.get(observed.object_id) ?? null;
        const shape = object?.shape ?? null;
        const shapeType = typeof shape?.type === 'string' ? shape.type : null;
        next.push({
          key: `${source.pose_source_id}:${observed.object_id}`,
          objectId: observed.object_id,
          sourceId: source.pose_source_id,
          position,
          quaternion,
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
        <group
          key={ghost.key}
          name={`measured-ghost:${ghost.objectId}`}
          position={ghost.position}
          quaternion={ghost.quaternion}
        >
          <MeasuredGhostBody shape={ghost.shape} shapeType={ghost.shapeType} />
        </group>
      ))}
    </group>
  );
}

function MeasuredGhostBody({
  shape,
  shapeType,
}: {
  shape: SceneShapeState | null;
  shapeType: string | null;
}) {
  const ghostColor = '#22d3ee';
  const ghostOpacity = 0.45;
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
  selected,
  locked,
  onSelect,
  onContextMenu,
  gizmoMode,
  gizmoSpace,
  isDraggingRef,
}: {
  object: SceneObjectState;
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
  const pose = useMemo(() => extractPose(object.pose_in_universe), [object.pose_in_universe]);
  const shape = object.shape ?? null;
  const color = selected ? '#f5d24a' : colorForSource(object.source);
  const opacity = object.attached ? 0.85 : 1.0;
  const shapeType = typeof shape?.type === 'string' ? (shape.type as string) : null;
  const editable = object.persistable && !object.attached;

  return (
    <>
      <group
        ref={setObjectNode}
        position={pose.position}
        quaternion={pose.quaternion}
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
  const [manifest, setManifest] = useState<SceneUrdfManifest | null>(null);
  const [manifestError, setManifestError] = useState<string | null>(null);
  const linkPoses = useSceneStateStore((state) => state.scene?.link_poses ?? EMPTY_LINK_POSES);
  const robotModelKey = useSceneStateStore(
    (state) => state.scene?.robot?.urdf_model_key ?? state.scene?.robot?.robot_id ?? null,
  );

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

  if (manifest === null) {
    // 404 here is the normal state when the deployment has no URDF;
    // the rest of the viewer still works.
    void manifestError;
    return null;
  }
  if (
    robotModelKey !== null &&
    manifest.model_key !== undefined &&
    manifest.model_key !== robotModelKey
  ) {
    return null;
  }

  return (
    <group name="manyforge-urdf" key={manifest.model_key ?? manifest.robot_id ?? 'unknown'}>
      {manifest.links.map((link) => {
        const pose = linkPoses[link.name];
        if (pose === undefined) {
          return null;
        }
        const modelKey = manifest.model_key ?? manifest.robot_id ?? 'unknown';
        return (
          <group
            key={`${modelKey}:${link.name}`}
            position={pose.position_m as [number, number, number]}
            quaternion={pose.orientation_xyzw as [number, number, number, number]}
          >
            {link.visuals.map((visual) => (
              <RobotLinkVisual
                key={`${modelKey}:${link.name}:${visual.index}:${visual.mesh_url ?? 'primitive'}`}
                visual={visual}
              />
            ))}
          </group>
        );
      })}
    </group>
  );
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

interface ExtractedPose {
  position: [number, number, number];
  quaternion: [number, number, number, number];
}

function extractPose(pose: Record<string, unknown> | null | undefined): ExtractedPose {
  return {
    position: numberTriple(pose?.position_m, [0, 0, 0]),
    quaternion: numberQuad(pose?.orientation_xyzw, [0, 0, 0, 1]),
  };
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
  const xyzw = visual.visual_origin.orientation_xyzw;
  if (Array.isArray(xyzw) && xyzw.length === 4) {
    return [
      Number(xyzw[0]) || 0,
      Number(xyzw[1]) || 0,
      Number(xyzw[2]) || 0,
      Number(xyzw[3]) || 1,
    ];
  }
  const rpy = visual.visual_origin.orientation_rpy_rad;
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
  // Render an axes triad at every frame the operator has flagged
  // visible. Pose + size come from /api/scene/frames (Day-4 parity
  // work moved frame visibility ownership into ComposerState so
  // this works without an external viewer server attached). Frames
  // without a kernel-resolved pose are skipped silently.
  const frames = useSceneStateStore((s) => s.frames?.frames ?? EMPTY_FRAMES);
  const gizmoSize = useSceneStateStore((s) => s.frames?.frameGizmoSizeM ?? 0.1);
  return (
    <group name="manyforge-frame-axes">
      {frames.map((frame) => {
        if (!frame.viewerVisible) return null;
        const pose = frame.poseInWorld;
        if (!pose) return null;
        const position = pose.position_m as [number, number, number];
        const quaternion = pose.orientation_xyzw as [number, number, number, number];
        return (
          <group key={frame.name} position={position} quaternion={quaternion}>
            <axesHelper args={[gizmoSize]} />
          </group>
        );
      })}
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
  const scene = useSceneStateStore((s) => s.scene ?? null);
  const tcpPose = scene?.robot?.tcp_pose_in_base ?? null;
  const tcpFrame = scene?.robot?.tcp_frame ?? null;
  const baseFrame = scene?.robot?.base_frame ?? null;
  const baseWorldPose = useMemo(() => {
    if (scene?.link_poses !== undefined && baseFrame !== null) {
      const pose = poseFromRecord(scene.link_poses[baseFrame]);
      if (pose !== null) return pose;
    }
    return poseFromBasePoseRecord(scene?.robot?.base_pose);
  }, [scene?.link_poses, scene?.robot?.base_pose, baseFrame]);
  const [anchor, setAnchor] = useState<Mesh | null>(null);
  const draggingRef = useRef(false);

  // Author a Cartesian candidate from the gizmo anchor's current world pose
  // (set by TransformControls during drag), expressed in the robot base frame
  // — exactly the shape the numeric Cartesian fields use. AUTHORING ONLY: this
  // creates/updates the server-owned candidate and never moves the robot. The
  // anchor then rests at the authored target (it is not snapped back to the
  // live TCP); the validated candidate ghost shows the planned configuration.
  const authorFromAnchorRef = useRef<() => void>(() => {});
  authorFromAnchorRef.current = () => {
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
    authorGizmoRelease(anchorWorld, baseWorldPose, tcpFrame, {
      author,
      rejectDraft,
    });
  };

  const initialPose = useMemo(() => {
    const tcpWorldPose =
      tcpFrame !== null && scene?.link_poses !== undefined
        ? poseFromRecord(scene.link_poses[tcpFrame])
        : null;
    if (tcpWorldPose !== null) return tcpWorldPose;
    const tcpInBase = poseFromRecord(tcpPose);
    if (tcpInBase === null) return null;
    if (baseWorldPose !== null) {
      return composePose(baseWorldPose, tcpInBase);
    }
    return tcpInBase;
  }, [scene?.link_poses, tcpFrame, tcpPose, baseWorldPose]);

  // While a Cartesian draft candidate is active, the gizmo rests at the
  // AUTHORED TARGET (the candidate's pose), not the live TCP — so editing is
  // iterative and a release does not snap the gizmo back. The target is in the
  // base frame; convert it to world. Falls back to live TCP when there is no
  // active draft (e.g. after Revert), when the candidate is handing off to
  // execution, or for an exotic non-base frame we cannot convert.
  const draftTargetWorld = useMemo(() => {
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
  }, [candidate, baseWorldPose, baseFrame]);

  const anchorPose = draftTargetWorld ?? initialPose;

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
  const finishDragRef = useRef<() => void>(() => {});
  finishDragRef.current = () => {
    if (!draggingRef.current) return;
    draggingRef.current = false;
    useViewerSelectionStore.getState().setDraggingGizmo(false);
    authorFromAnchorRef.current();
  };

  // Window-level safety net: if drei TransformControls drops its mouseUp
  // event, the window pointerup/pointercancel still finishes the drag.
  // Routed through the single-shot finishDrag path so it can never
  // double-author with the inline handler.
  useEffect(() => {
    const onPointerUp = () => finishDragRef.current();
    window.addEventListener('pointerup', onPointerUp);
    window.addEventListener('pointercancel', onPointerUp);
    return () => {
      window.removeEventListener('pointerup', onPointerUp);
      window.removeEventListener('pointercancel', onPointerUp);
    };
  }, []);

  if (!robotPanelActive || robotMode !== 'cartesian' || anchorPose === null || !commandGate.canCommand) {
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
          onMouseUp={() => finishDragRef.current()}
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
  const objects = useSceneStateStore((s) => s.scene?.objects ?? EMPTY_OBJECTS);
  const tcpPose = useSceneStateStore((s) => s.scene?.robot?.tcp_pose_in_base ?? null);
  const appliedTree = useProgramStore((s) => s.appliedTree);
  const [frames, setFrames] = useState<SceneFrameState[]>([]);

  // Independent frames fetch so the ghost works even if MoveToBody
  // isn't mounted (e.g. operator toggles the panel chrome but keeps
  // the Move-To selection live). Same cleanup-on-unmount pattern.
  useEffect(() => {
    if (!robotPanelActive || robotMode !== 'move-to') return;
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
  }, [robotPanelActive, robotMode]);

  const storedPoses = useMemo(() => {
    const out: { nodeName: string; motionType: 'pose_goal' | 'joint_goal'; params: Record<string, unknown> }[] = [];
    function visit(node: TreeNode | null): void {
      if (node === null) return;
      if (node.id === 'move_manipulator_action') {
        const mt = node.params['motion_type'];
        if (mt === 'pose_goal' || mt === 'joint_goal') {
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
    return computeMoveToTarget({
      moveTo,
      objects: objects.filter((o) => !o.attached),
      frames,
      storedPoses,
      tcpPose,
    });
  }, [robotPanelActive, robotMode, moveTo, objects, frames, storedPoses, tcpPose]);

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
  const robot = useSceneStateStore((s) => s.scene?.robot ?? null);
  const grippers = useSceneStateStore((s) => s.scene?.grippers ?? EMPTY_GRIPPERS);
  const robotModelKey = useSceneStateStore(
    (s) => s.scene?.robot?.urdf_model_key ?? s.scene?.robot?.robot_id ?? null,
  );
  const tcpFrame = useSceneStateStore((s) => s.scene?.robot?.tcp_frame ?? null);
  const [manifest, setManifest] = useState<SceneUrdfManifest | null>(null);
  const [fetchedPreview, setFetchedPreview] = useState<PreviewLinkPosesResponse | null>(
    null,
  );
  const fetchSeqRef = useRef(0);
  // The joint-vector key the current fetchedPreview was computed for. Used to
  // refetch ONLY when the target configuration actually changes, never on a
  // mere candidate-object identity change (each status poll / debounce tick
  // produces a new object with the same pose) — that churn was the ghost
  // flicker.
  const lastFetchKeyRef = useRef<string | null>(null);
  const fetchedPreviewFamilyRef = useRef<string | null>(null);

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
    fetchedPreviewFamilyRef.current = null;
  }, [previewFamilyKey]);

  useEffect(() => {
    if (!active) {
      setManifest(null);
      // Candidate discarded / handed off: drop the cached preview so the next
      // candidate fetches fresh.
      setFetchedPreview(null);
      lastFetchKeyRef.current = null;
      fetchedPreviewFamilyRef.current = null;
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
            fetchedPreviewFamilyRef.current = previewFamilyKey;
            setFetchedPreview(r);
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
    fetchedPreviewFamilyRef.current === previewFamilyKey ? fetchedPreview : null;
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
  // WS-pushed point-cloud / octomap frames (Day-4 contract). Idle
  // until a perception source registers; while idle the WS stays
  // open but no frames arrive and nothing renders.
  usePointCloudSubscription();
  const frame = usePointCloudStore((s) => s.frame);
  const pointsRef = useRef<THREE.Points>(null);

  useEffect(() => {
    const node = pointsRef.current;
    if (node === null || frame === null) return;
    const geom = node.geometry as THREE.BufferGeometry;
    const positions = new Float32Array(frame.points.length * 3);
    for (let i = 0; i < frame.points.length; i += 1) {
      positions[i * 3] = frame.points[i][0];
      positions[i * 3 + 1] = frame.points[i][1];
      positions[i * 3 + 2] = frame.points[i][2];
    }
    geom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    if (frame.colors !== undefined && frame.colors.length === frame.points.length) {
      const colors = new Float32Array(frame.colors.length * 3);
      for (let i = 0; i < frame.colors.length; i += 1) {
        colors[i * 3] = frame.colors[i][0];
        colors[i * 3 + 1] = frame.colors[i][1];
        colors[i * 3 + 2] = frame.colors[i][2];
      }
      geom.setAttribute('color', new THREE.BufferAttribute(colors, 3));
    } else {
      geom.deleteAttribute('color');
    }
    geom.computeBoundingSphere();
  }, [frame]);

  if (frame === null || frame.points.length === 0) {
    return null;
  }
  return (
    <points ref={pointsRef}>
      <bufferGeometry />
      <pointsMaterial
        size={0.01}
        sizeAttenuation
        color="#8aaaff"
        vertexColors={frame.colors !== undefined}
      />
    </points>
  );
}

function ObjectFrameAxes() {
  // Sibling of FrameAxes for per-scene-object frame helpers. Pose
  // comes from /api/scene/state.objects[].pose_in_universe;
  // visibility from .viewer_frame_visible (now state-owned). Size
  // is tracked independently of the named-frame gizmo size so the
  // operator can dial each axis style separately.
  const objects = useSceneStateStore((s) => s.scene?.objects ?? EMPTY_OBJECTS);
  const gizmoSize = useSceneStateStore(
    (s) => s.scene?.object_axes_gizmo_size_m ?? 0.1,
  );
  return (
    <group name="manyforge-object-frame-axes">
      {objects.map((obj) => {
        if (!obj.viewer_frame_visible) return null;
        const pose = extractPose(obj.pose_in_universe);
        return (
          <group key={obj.object_id} position={pose.position} quaternion={pose.quaternion}>
            <axesHelper args={[gizmoSize]} />
          </group>
        );
      })}
    </group>
  );
}

const EMPTY_FRAMES: import('../types/api').SceneFrameState[] = [];

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
