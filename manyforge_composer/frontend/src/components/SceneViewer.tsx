import { Suspense, useEffect, useMemo, useRef, useState } from 'react';
import type { MutableRefObject } from 'react';
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
import type { Mesh } from 'three';

import { api } from '../api/client';
import {
  followTargetController,
  type FollowTargetDragToken,
} from '../streaming/followTargetController';
import { usePointCloudStore, usePointCloudSubscription } from '../store/pointCloudStore';
import { useProgramStore } from '../store/programStore';
import { useSceneStateStore, useScenePolling } from '../store/sceneStateStore';
import { useStatusPolling, useStatusStore } from '../store/statusStore';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import type {
  PreviewLinkPosesRequest,
  PreviewLinkPosesResponse,
  SceneFrameState,
  SceneObjectState,
  SceneUrdfManifest,
  SceneUrdfVisual,
  TreeNode,
} from '../types/api';
import { computeMoveToTarget } from '../utils/moveTo';
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

const EMPTY_LINK_POSES: Record<
  string,
  { position_m: number[]; orientation_xyzw: number[] }
> = {};

interface SceneViewerPanelProps {
  onCollapse: () => void;
  blocked: boolean;
  resizing?: boolean;
}

/**
 * Native R3F replacement for ViserPanel.
 *
 * The Canvas runs `frameloop="demand"` so it only paints when the
 * React tree changes (scene-state poll) or the user interacts
 * (OrbitControls invalidates internally). This is the load-bearing
 * difference vs Viser's `frameloop="always"` 60 fps continuous render
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
  // — cleanly aborting any open streaming session — when the operator
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
    const shapeType = shape?.type as 'box' | 'sphere' | 'mesh' | undefined;
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
      <FrameAxes />
      <ObjectFrameAxes />
      <PointCloud />
      <TcpJogGizmo />
      <MoveToTargetGhost />
      <MoveToGhostRobot />
      <CartesianRejectionIndicator />
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
  // State-as-ref: the lint rule `react-hooks/refs` (correctly) forbids
  // reading `someRef.current` during render. We instead store the live
  // Mesh node directly in component state so the `object` prop on
  // <TransformControls> is a normal reactive value. The TS-typed mesh
  // is also handed back to the pose-emit helper unchanged.
  const [meshNode, setMeshNode] = useState<Mesh | null>(null);
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

  const geometry = useMemo(() => {
    if (shapeType === 'box') {
      const dims = numberTriple(shape?.box_dimensions_m, [0.1, 0.1, 0.1]);
      return <boxGeometry args={dims} />;
    }
    if (shapeType === 'sphere') {
      const radius = typeof shape?.sphere_radius_m === 'number' ? shape.sphere_radius_m : 0.05;
      return <sphereGeometry args={[radius, 24, 16]} />;
    }
    return <boxGeometry args={[0.05, 0.05, 0.05]} />;
  }, [shape, shapeType]);

  return (
    <>
      <mesh
        ref={setMeshNode}
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
        {geometry}
        <meshStandardMaterial
          color={color}
          transparent={opacity < 1}
          opacity={opacity}
          wireframe={shapeType !== 'box' && shapeType !== 'sphere'}
        />
      </mesh>
      {objectPanelActive && selected && editable && !locked && meshNode !== null && (
        <TransformControls
          object={meshNode}
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
            emitObjectPoseEdit(object.object_id, meshNode);
          }}
        />
      )}
    </>
  );
}

function emitObjectPoseEdit(objectId: string, mesh: Mesh | null): void {
  if (!mesh) return;
  const position = mesh.position.toArray() as [number, number, number];
  const q = mesh.quaternion;
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
  // this works without the Viser HMI server attached). Frames
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
  // End-effector jog gizmo, ADR-0016 streaming-session edition.
  //
  // The gizmo is anchored to an invisible mesh at the current TCP pose
  // (read from scene state). On mouseDown we open a WebSocket streaming
  // session via /ws/follow-target; while dragging we push the gizmo's
  // live pose to the session as fast as drei fires onObjectChange (the
  // backend coalesces — latest-wins — so there's no need to rate-limit
  // here); on mouseUp we close the session and the backend runs the
  // configured soft-stop trajectory.
  //
  // Why this replaces the old one-shot /api/direct/cartesian-jog path:
  // each one-shot per frame produced fresh plan+execute cycles, which
  // (a) carried plan latency, (b) made the robot's motion chunky from
  // back-to-back acceleration ramps, and (c) raced cycleState transitions
  // such that the chain could deadlock if a WS state edge beat the HTTP
  // response. The streaming session is one long-lived intervention with
  // a mutable target register the C++ backend coalesces against — cycle
  // stays in direct_control for the entire drag, no idle/direct_control
  // flapping, smooth preempt via anchored FJT.
  // Robot-tab focus + cartesian-mode gate. The gizmo only mounts when
  // BOTH conditions hold: the operator has the Robot panel open AND
  // the robot panel's mode is set to Cartesian. Switching the active
  // panel away (Object, Tools, or closed) — or switching robot mode
  // to Joint — unmounts the gizmo, which triggers the cleanup effect
  // below to abort any open streaming session.
  const robotPanelActive = useViewerSelectionStore((s) => s.activePanel === 'robot');
  const robotMode = useViewerSelectionStore((s) => s.robotMode);
  const cartesianFrame = useViewerSelectionStore((s) => s.cartesianFrame);
  const gizmoMode = useViewerSelectionStore((s) => s.cartesianGizmoMode);
  const tcpPose = useSceneStateStore((s) => s.scene?.robot?.tcp_pose_in_base ?? null);
  const tcpFrame = useSceneStateStore((s) => s.scene?.robot?.tcp_frame ?? null);
  const baseFrame = useSceneStateStore((s) => s.scene?.robot?.base_frame ?? null);
  // Joint list for the cartesian streaming session. Sourced from the
  // live scene's robot state (`scene.robot.joint_names`) rather than the
  // URDF manifest because the backend's IK uses scene-state ordering;
  // a URDF/scene mismatch would make the kernel reject every frame with
  // `unknown_joint_name`. Falls back to an empty list when the scene is
  // still loading — the kernel's begin_streaming_session fallback (it
  // populates joint_names from `context_.get_current_state()` when the
  // session list is empty) covers that gap.
  const sceneJointNames = useSceneStateStore(
    (s) => s.scene?.robot?.joint_names ?? null,
  );
  const drivableJointNamesRef = useRef<string[]>([]);
  useEffect(() => {
    drivableJointNamesRef.current = sceneJointNames ?? [];
  }, [sceneJointNames]);
  const [anchor, setAnchor] = useState<Mesh | null>(null);
  const draggingRef = useRef(false);
  const dragTokenRef = useRef<FollowTargetDragToken | null>(null);

  const initialPose = useMemo(() => {
    if (tcpPose === null) return null;
    const pos = (tcpPose as { position_m?: number[] }).position_m;
    const quat = (tcpPose as { orientation_xyzw?: number[] }).orientation_xyzw;
    if (!Array.isArray(pos) || pos.length !== 3) return null;
    if (!Array.isArray(quat) || quat.length !== 4) return null;
    return {
      position: pos as [number, number, number],
      quaternion: quat as [number, number, number, number],
    };
  }, [tcpPose]);

  useEffect(() => {
    if (anchor === null || initialPose === null) return;
    if (draggingRef.current) return;
    anchor.position.set(...initialPose.position);
    anchor.quaternion.set(...initialPose.quaternion);
  }, [anchor, initialPose, robotMode]);

  // Abort any active session on unmount (mode switch away from Cartesian,
  // panel close, viewer remount). The controller owns the WebSocket
  // lifecycle independent of this component; abort() ensures the socket
  // is closed and the kernel session is released regardless of what
  // caused the unmount.
  useEffect(() => {
    return () => {
      if (followTargetController.phase() !== 'idle') {
        followTargetController.abort('TcpJogGizmo unmount');
      }
    };
  }, []);

  // Retry watchdog: while dragging, fire a push at a steady rate
  // independent of drei's onObjectChange cadence. Two failure modes
  // this catches:
  //   1. Operator holds the gizmo still in an unreachable region;
  //      drei stops firing onObjectChange so no pushes go out, so IK
  //      is never re-attempted. With the watchdog, the kernel keeps
  //      retrying every 200 ms — if scene interpolation eventually
  //      brings the kernel's IK seed into a config from which the
  //      target is reachable, the next push lands and the indicator
  //      clears without operator intervention.
  //   2. drei's onObjectChange under fast re-renders firing slower
  //      than the operator's actual hand motion (we observed 1 Hz
  //      cadence in logs); the watchdog backs it up at 5 Hz.
  // No-op when not dragging or when controller phase is not
  // 'streaming' (the controller's push() drops the call cheaply).
  useEffect(() => {
    // 4 Hz watchdog — operator-visible motion stays smooth, kernel +
    // controller see at most one in-flight frame, and the released
    // pose is always covered by an explicit final push on onMouseUp.
    const interval = setInterval(() => {
      if (!draggingRef.current) return;
      if (anchor === null) return;
      const token = dragTokenRef.current;
      if (token === null) return;
      const p = anchor.position;
      const q = anchor.quaternion;
      followTargetController.push(token, {
        kind: 'cartesian',
        positionM: [p.x, p.y, p.z],
        orientationXyzw: [q.x, q.y, q.z, q.w],
      });
    }, 250);
    return () => clearInterval(interval);
  }, [anchor]);

  // Global pointerup safety net: if drei TransformControls drops the
  // mouseUp event (which we've observed under certain raycast / event-
  // capture conditions), the window-level pointerup still fires. When
  // it does and we're still in dragging state, force the release path
  // so the gizmo snaps back, the controller ends the session, and the
  // kernel doesn't stay locked in kStreaming. Without this, releasing
  // in an unreachable region used to hang the chain for ~30 seconds
  // until other timeouts eventually unstuck it.
  useEffect(() => {
    const onPointerUp = () => {
      if (!draggingRef.current) return;
      draggingRef.current = false;
      const token = dragTokenRef.current;
      dragTokenRef.current = null;
      const vs = useViewerSelectionStore.getState();
      vs.setDraggingGizmo(false);
      vs.setCartesianRejection(null, null);
      followTargetController.cancelPendingBegin(token);
      followTargetController.end(token, 'spring_back');
      // Always snap back — same invariant as the drei onMouseUp path.
      if (anchor !== null && initialPose !== null) {
        anchor.position.set(...initialPose.position);
        anchor.quaternion.set(...initialPose.quaternion);
      }
    };
    window.addEventListener('pointerup', onPointerUp);
    window.addEventListener('pointercancel', onPointerUp);
    return () => {
      window.removeEventListener('pointerup', onPointerUp);
      window.removeEventListener('pointercancel', onPointerUp);
    };
  }, [anchor, initialPose]);

  if (!robotPanelActive || robotMode !== 'cartesian' || initialPose === null) {
    return null;
  }

  const gizmoSpace: 'world' | 'local' = cartesianFrame === 'tcp' ? 'local' : 'world';

  return (
    <>
      <mesh
        ref={(m) => {
          if (m !== null && initialPose !== null && !draggingRef.current) {
            m.position.set(...initialPose.position);
            m.quaternion.set(...initialPose.quaternion);
          }
          setAnchor(m);
        }}
        visible={false}
      >
        <sphereGeometry args={[0.005, 8, 6]} />
      </mesh>
      {/*
        Unreachable/rejected indicator lives in a sibling component
        (CartesianRejectionIndicator) subscribed to the viewer-selection
        store. Pulled out of TcpJogGizmo's own render tree to avoid
        re-rendering the TransformControls subtree on every per-frame
        rejection — that triggered spurious onMouseDown events under
        rapid rejection streams.
      */}
      {anchor !== null && (
        <TransformControls
          object={anchor}
          mode={gizmoMode}
          space={gizmoSpace}
          onMouseDown={() => {
            // Guard against drei firing onMouseDown twice before
            // mouseUp. Local UI state (draggingRef + draggingGizmo
            // store flag) is set unconditionally — independent of
            // whether the controller can open a fresh session right
            // now. If the previous session is still ending, the
            // controller QUEUES the begin and auto-fires it once it
            // reaches idle. Pushes during the queue window are
            // dropped at the controller and harmless (backend
            // coalesces). Decoupling the UI lifecycle from the
            // session lifecycle is what fixes the "second drag
            // doesn't snap back on release" bug.
            if (draggingRef.current) return;
            const token = followTargetController.begin({
              inputMode: 'cartesian',
              jointNames: drivableJointNamesRef.current,
              cartesianReferenceFrame:
                cartesianFrame === 'tcp' ? 'tcp' : 'base',
              tcpFrame: tcpFrame ?? '',
              softStopStrategy: 'spring_back',
              maxDispatchRateHz: 30,
              principalId: 'composer_operator',
              authoritySource: 'human',
              triggerReason: 'composer_gizmo_drag',
            });
            if (token === null) return;
            dragTokenRef.current = token;
            draggingRef.current = true;
            useViewerSelectionStore.getState().setDraggingGizmo(true);
            // Push the initial pose. If the controller is still in
            // ending/aborted phase this is dropped; the watchdog will
            // re-attempt once the queued begin fires.
            const p = anchor.position;
            const q = anchor.quaternion;
            followTargetController.push(token, {
              kind: 'cartesian',
              positionM: [p.x, p.y, p.z],
              orientationXyzw: [q.x, q.y, q.z, q.w],
            });
          }}
          onObjectChange={() => {
            if (!draggingRef.current) return;
            // Server pushes are driven by the 4 Hz watchdog above —
            // pushing on every drei frame here is redundant and
            // floods the wire (drei fires at ~60 Hz). The watchdog
            // reads the live anchor pose, so no per-frame motion is
            // lost. We do still update the magenta indicator anchor
            // here in-process (no server hit) so the rejection label
            // tracks the gizmo smoothly through the unreachable
            // region.
            const p = anchor.position;
            const q = anchor.quaternion;
            const vsState = useViewerSelectionStore.getState();
            if (vsState.cartesianRejectionLabel !== null) {
              vsState.setCartesianRejection(vsState.cartesianRejectionLabel, {
                position: [p.x, p.y, p.z],
                quaternion: [q.x, q.y, q.z, q.w],
              });
            }
          }}
          onMouseUp={() => {
            // Unconditional cleanup. Local UI state always resets on
            // release, regardless of whether the session opened or
            // queued, regardless of whether the controller phase is
            // streaming / opening / ending. This guarantees the gizmo
            // snaps back on every release.
            const wasDragging = draggingRef.current;
            draggingRef.current = false;
            const vs = useViewerSelectionStore.getState();
            vs.setDraggingGizmo(false);
            vs.setCartesianRejection(null, null);
            // Cancel any queued begin so a release-then-grab-then-
            // release doesn't leave a phantom session pending.
            const token = dragTokenRef.current;
            dragTokenRef.current = null;
            followTargetController.cancelPendingBegin(token);
            if (wasDragging && token !== null) {
              // Push the release pose and end the active session.
              // push() drops if controller isn't streaming (queued or
              // already ended). end() drops if not streaming/opening.
              // Both safe.
              const p = anchor.position;
              const q = anchor.quaternion;
              followTargetController.push(token, {
                kind: 'cartesian',
                positionM: [p.x, p.y, p.z],
                orientationXyzw: [q.x, q.y, q.z, q.w],
              });
              followTargetController.end(token, 'spring_back');
            }
            // ALWAYS snap the gizmo back to the live TCP. The decisive
            // user-visible bit: never leave the gizmo floating in the
            // unreachable region after a release.
            if (anchor !== null && initialPose !== null) {
              anchor.position.set(...initialPose.position);
              anchor.quaternion.set(...initialPose.quaternion);
            }
            void baseFrame; // reserved for multi-base future work
          }}
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
 * exactly the pose that will be commanded on Go.
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
 * Transparent ghost-robot preview for the Move-To tab.
 *
 * Posts the current Move-To target to /api/scene/preview-link-poses
 * on a 250 ms debounce; on success renders the full URDF at the
 * returned link_poses with 50 % alpha; on IK failure (unreachable
 * cartesian target) renders ONLY the TCP link at the supplied
 * tcp_pose in red 50 % alpha — the visual cue that the rest of
 * the kinematic chain can't reach this pose.
 *
 * Lives as a sibling of MoveToTargetGhost (the axes preview) so
 * the two compose: axes always show the precise TCP point; the
 * ghost robot adds the "will the elbow fight me" reachability
 * answer.
 */
function MoveToGhostRobot() {
  const robotPanelActive = useViewerSelectionStore((s) => s.activePanel === 'robot');
  const robotMode = useViewerSelectionStore((s) => s.robotMode);
  const moveTo = useViewerSelectionStore((s) => s.moveTo);
  const objects = useSceneStateStore((s) => s.scene?.objects ?? EMPTY_OBJECTS);
  const tcpPose = useSceneStateStore((s) => s.scene?.robot?.tcp_pose_in_base ?? null);
  const tcpFrame = useSceneStateStore((s) => s.scene?.robot?.tcp_frame ?? null);
  const robotModelKey = useSceneStateStore(
    (s) => s.scene?.robot?.urdf_model_key ?? s.scene?.robot?.robot_id ?? null,
  );
  const liveJointNames = useSceneStateStore((s) => s.scene?.robot?.joint_names ?? null);
  const liveJointPositions = useSceneStateStore(
    (s) => s.scene?.robot?.joint_positions ?? null,
  );
  const appliedTree = useProgramStore((s) => s.appliedTree);
  const [frames, setFrames] = useState<SceneFrameState[]>([]);
  const [manifest, setManifest] = useState<SceneUrdfManifest | null>(null);
  const [preview, setPreview] = useState<PreviewLinkPosesResponse | null>(null);

  // Fetch frames + URDF manifest once when Move-To becomes active. Both
  // are needed to compute the target (frames feed the Frame source)
  // and to render the ghost (manifest carries the link/visual tree).
  useEffect(() => {
    if (!robotPanelActive || robotMode !== 'move-to') return;
    let cancelled = false;
    void Promise.all([api.scene.frames(), api.urdf.manifest()])
      .then(([f, m]) => {
        if (cancelled) return;
        setFrames(f.frames.filter((fr) => fr.poseAvailable));
        setManifest(m);
      })
      .catch(() => {
        if (cancelled) return;
        setFrames([]);
        setManifest(null);
      });
    return () => {
      cancelled = true;
    };
  }, [robotPanelActive, robotMode, robotModelKey]);

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

  // Debounced preview-link-poses request. Re-fires whenever the
  // computed target changes (slider drag, offset edit, source switch)
  // but always waits the 250 ms quiescence so we don't hammer the
  // kernel during slider drags. IK is ~5-50 ms per call; per-link FK
  // is sub-millisecond — comfortable budget at this rate.
  useEffect(() => {
    if (target === null || manifest === null) {
      setPreview(null);
      return;
    }
    const linkNames = manifest.links.map((l) => l.name);
    const handle = window.setTimeout(() => {
      let req: PreviewLinkPosesRequest;
      if (target.kind === 'cartesian') {
        // Pass the live joint configuration as the IK seed so the
        // solver picks the branch nearest the current arm pose. Without
        // a seed the kernel falls back to context.get_current_state(),
        // which works but produces non-deterministic branch flips as
        // the operator drags an offset slider — adjacent targets can
        // land on opposite elbow-up/elbow-down solutions and visibly
        // pop the ghost robot.
        const seedJointNames: string[] = liveJointNames ?? [];
        const seedJointPositions: number[] =
          liveJointNames && liveJointPositions
            ? liveJointNames.map((n) => Number(liveJointPositions[n] ?? 0))
            : [];
        req = {
          kind: 'cartesian',
          tcp_frame: tcpFrame ?? '',
          tcp_position_m: target.position,
          tcp_orientation_xyzw: target.quaternion,
          link_names: linkNames,
          joint_names: seedJointNames,
          joint_positions_rad: seedJointPositions,
        };
      } else {
        req = {
          kind: 'joint',
          joint_names: target.jointNames,
          joint_positions_rad: target.jointPositionsRad,
          link_names: linkNames,
        };
      }
      void api.scene
        .previewLinkPoses(req)
        .then((r) => setPreview(r))
        .catch(() => setPreview(null));
    }, 250);
    return () => window.clearTimeout(handle);
  }, [target, manifest, tcpFrame, liveJointNames, liveJointPositions]);

  if (!robotPanelActive || robotMode !== 'move-to') return null;
  if (manifest === null || preview === null) return null;
  if (
    robotModelKey !== null &&
    manifest.model_key !== undefined &&
    manifest.model_key !== robotModelKey
  ) {
    return null;
  }

  if (preview.reachable) {
    return (
      <group name="manyforge-ghost-robot" key={manifest.model_key ?? manifest.robot_id ?? 'unknown'}>
        {manifest.links.map((link) => {
          const pose = preview.link_poses[link.name];
          if (!pose) return null;
          const modelKey = manifest.model_key ?? manifest.robot_id ?? 'unknown';
          return (
            <group
              key={`${modelKey}:ghost:${link.name}`}
              position={pose.position_m as [number, number, number]}
              quaternion={pose.orientation_xyzw as [number, number, number, number]}
            >
              {link.visuals.map((visual) => (
                <GhostVisual
                  key={`${modelKey}:ghost:${link.name}:${visual.index}:${visual.mesh_url ?? 'primitive'}`}
                  visual={visual}
                  opacity={0.5}
                />
              ))}
            </group>
          );
        })}
      </group>
    );
  }

  // Unreachable: render only the TCP link (the "gripper") at the
  // supplied target pose, in red. Operator gets a clear "you asked
  // for this TCP pose, the rest of the arm can't reach it" cue
  // without an axes-only ambiguity.
  if (preview.tcp_pose === undefined || tcpFrame === null) return null;
  const tcpLink = manifest.links.find((l) => l.name === tcpFrame);
  if (tcpLink === undefined) return null;
  const modelKey = manifest.model_key ?? manifest.robot_id ?? 'unknown';
  return (
    <group
      name="manyforge-ghost-tcp-unreachable"
      key={`${modelKey}:ghost-unreachable:${tcpFrame}`}
      position={preview.tcp_pose.position_m as [number, number, number]}
      quaternion={preview.tcp_pose.orientation_xyzw as [number, number, number, number]}
    >
      {tcpLink.visuals.map((visual) => (
        <GhostVisual
          key={`${modelKey}:ghost-unreachable:${tcpFrame}:${visual.index}:${visual.mesh_url ?? 'primitive'}`}
          visual={visual}
          opacity={0.5}
          color="#ff3333"
        />
      ))}
    </group>
  );
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

/**
 * Sibling-component rendering for the streaming-session magenta
 * indicator. Subscribed to the viewer-selection store so it re-renders
 * independently of TcpJogGizmo — store updates from per-frame rejections
 * don't touch the gizmo's render tree (which under fast rejection
 * streams produced spurious onMouseDown events and burst WS opens).
 */
function CartesianRejectionIndicator() {
  const label = useViewerSelectionStore((s) => s.cartesianRejectionLabel);
  const anchor = useViewerSelectionStore((s) => s.cartesianRejectionAnchor);
  if (label === null || anchor === null) return null;
  // Label only — no in-scene mesh. The HTML overlay carries all the
  // information the operator needs; rendering a 3D mesh at the gizmo
  // position is decorative and a raycast-intercept risk for drei's
  // TransformControls. `pointerEvents="none"` keeps the label transparent
  // to the gizmo's mouse handling.
  return (
    <group position={anchor.position} quaternion={anchor.quaternion}>
      <Html
        position={[0, 0.07, 0]}
        center
        distanceFactor={1.5}
        pointerEvents="none"
        prepend
        zIndexRange={[0, 0]}
        style={{
          userSelect: 'none',
          pointerEvents: 'none',
          fontFamily: 'monospace',
          fontWeight: 700,
          fontSize: '10px',
          letterSpacing: '0.1em',
          color: '#ffffff',
          background: '#ff00ff',
          padding: '2px 6px',
          borderRadius: '2px',
          whiteSpace: 'nowrap',
          boxShadow: '0 0 4px rgba(0,0,0,0.4)',
        }}
      >
        {label}
      </Html>
    </group>
  );
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
