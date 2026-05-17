import { Suspense, useEffect, useMemo, useRef, useState } from 'react';
import type { MutableRefObject } from 'react';
import { Canvas } from '@react-three/fiber';
import {
  Grid,
  OrbitControls,
  PerspectiveCamera,
  Stats,
  TransformControls,
  useGLTF,
} from '@react-three/drei';
import * as THREE from 'three';
import type { Mesh } from 'three';

import { api } from '../api/client';
import { useCycleStore } from '../store/cycleStore';
import { usePointCloudStore, usePointCloudSubscription } from '../store/pointCloudStore';
import { useSceneStateStore, useScenePolling } from '../store/sceneStateStore';
import { useStatusPolling, useStatusStore } from '../store/statusStore';
import { useViewerSelectionStore } from '../store/viewerSelectionStore';
import type {
  SceneObjectState,
  SceneUrdfManifest,
  SceneUrdfVisual,
} from '../types/api';
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

// Pose equality with a small floating-point tolerance — used by the
// TCP jog follow chain to decide whether a fresh jog request is
// needed. Position threshold ~0.1 mm; quaternion threshold matches.
function posesClose(
  a: { pos: readonly number[]; quat: readonly number[] },
  b: { pos: readonly number[]; quat: readonly number[] },
): boolean {
  const POS_EPS = 1e-4;
  const ROT_EPS = 1e-4;
  for (let i = 0; i < 3; i += 1) {
    if (Math.abs(a.pos[i] - b.pos[i]) > POS_EPS) return false;
  }
  for (let i = 0; i < 4; i += 1) {
    if (Math.abs(a.quat[i] - b.quat[i]) > ROT_EPS) return false;
  }
  return true;
}
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
  // unambiguous to read.
  const [activePanel, setActivePanel] = useState<'tools' | 'robot' | 'object' | null>(null);
  const togglePanel = (kind: 'tools' | 'robot' | 'object'): void => {
    setActivePanel((current) => (current === kind ? null : kind));
  };

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
            onClick={() => togglePanel('tools')}
            title="Frames / object axes / scene tree"
          />
          <HeaderToggle
            label="Robot"
            on={activePanel === 'robot'}
            onClick={() => togglePanel('robot')}
            title="Robot control panel (joint sliders)"
          />
          <HeaderToggle
            label="Object"
            on={activePanel === 'object'}
            onClick={() => togglePanel('object')}
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
      {selected && editable && !locked && meshNode !== null && (
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

  if (manifest === null) {
    // 404 here is the normal state when the deployment has no URDF;
    // the rest of the viewer still works.
    void manifestError;
    return null;
  }

  return (
    <group name="manyforge-urdf">
      {manifest.links.map((link) => {
        const pose = linkPoses[link.name];
        if (pose === undefined) {
          return null;
        }
        return (
          <group
            key={link.name}
            position={pose.position_m as [number, number, number]}
            quaternion={pose.orientation_xyzw as [number, number, number, number]}
          >
            {link.visuals.map((visual) => (
              <RobotLinkVisual
                key={`${link.name}-${visual.index}`}
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
  const offsetRpy = visual.visual_origin.orientation_rpy_rad as [number, number, number];
  // Mesh-backed visual — let drei's useGLTF stream the per-visual GLB.
  if (visual.mesh_url !== null) {
    return (
      <group position={offsetPos} rotation={offsetRpy}>
        <RobotLinkGltf url={visual.mesh_url} />
      </group>
    );
  }
  // Primitive visual — rendered natively to avoid a server round-trip for
  // shapes like UR5's tool0 boxes that make up the rudimentary gripper.
  if (visual.primitive !== null) {
    return (
      <group position={offsetPos} rotation={offsetRpy}>
        <PrimitiveMesh primitive={visual.primitive} />
      </group>
    );
  }
  return null;
}

function RobotLinkGltf({ url }: { url: string }) {
  const gltf = useGLTF(url);
  const cloned = useMemo(() => gltf.scene.clone(true), [gltf.scene]);
  return <primitive object={cloned} />;
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
  // Day-5c.2: end-effector jog gizmo. Mounted when the robot panel
  // is in Cartesian mode. The gizmo is anchored to a small invisible
  // mesh placed at the current TCP pose (read from scene state's
  // tcp_pose_in_base; the demo's base_link is at world origin so
  // base ≈ world). On the gizmo's mouseUp we read the target
  // transform and POST to /api/direct/cartesian-jog.
  //
  // V1 simplification: target pose is sent verbatim under the
  // assumption that base_frame ≈ world. A future tweak will convert
  // through the robot's base_pose for non-fixed-base setups.
  const robotMode = useViewerSelectionStore((s) => s.robotMode);
  const cartesianFrame = useViewerSelectionStore((s) => s.cartesianFrame);
  const gizmoMode = useViewerSelectionStore((s) => s.cartesianGizmoMode);
  const tcpPose = useSceneStateStore((s) => s.scene?.robot?.tcp_pose_in_base ?? null);
  const [anchor, setAnchor] = useState<Mesh | null>(null);
  // Track whether the operator is actively dragging so we don't
  // re-seed the anchor from scene state mid-drag.
  const draggingRef = useRef(false);
  // State-aware coalesce. The backend's `execute_intervention`
  // returns immediately after starting the tick thread (it does
  // NOT block until motion completes), so a Promise.finally chain
  // fires follow-up jogs while the robot is still moving — all of
  // which get 409'd because the state machine is still in
  // DIRECT_CONTROL. The fix: gate firing on cycleState === 'idle'
  // and drive retries off the WS-pushed state edge from
  // DIRECT_CONTROL → IDLE. `lastFiredPoseRef` is only updated on
  // a successful 200 response, so a rejected/retried jog can't
  // poison the "already-sent" check and make us drop later
  // gizmo positions.
  // `shouldFollowRef`: true from mouseDown until the robot has
  // caught up to the latest target post-release. Prevents the
  // gizmo from issuing jogs in response to background state
  // transitions (e.g. an autonomous program toggling idle/run)
  // when the operator isn't actually interacting.
  const inFlightRef = useRef(false);
  const lastFiredPoseRef = useRef<{
    pos: [number, number, number];
    quat: [number, number, number, number];
  } | null>(null);
  const shouldFollowRef = useRef(false);

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

  // When entering Cartesian mode (or when tcpPose updates outside of
  // a drag), re-seed the anchor mesh to the current TCP pose.
  // Critical: we do NOT pass `position`/`quaternion` as JSX props on
  // the anchor mesh, because React would re-apply them every render
  // (each tcpPose poll) — yanking the gizmo back to the robot's
  // current pose mid-drag and causing a visible flicker between
  // mouse position and robot position. All seeding is imperative
  // and guarded by `draggingRef`.
  useEffect(() => {
    if (anchor === null || initialPose === null) return;
    if (draggingRef.current) return;
    anchor.position.set(...initialPose.position);
    anchor.quaternion.set(...initialPose.quaternion);
  }, [anchor, initialPose, robotMode]);

  // Fire a jog toward the gizmo's live pose. Gated by:
  //   - `shouldFollowRef`: operator is interacting (or robot still
  //     catching up to the release pose)
  //   - `inFlightRef`: at most one HTTP request outstanding
  //   - `cycleState === 'idle'`: the backend will accept a new
  //     intervention (avoids 409 storms during in-flight motion)
  // `lastFiredPoseRef` is only updated on success. The chain is
  // re-armed by the cycleStore subscription (below) on each
  // transition into IDLE, not by Promise.finally.
  function tryFire(): void {
    if (!shouldFollowRef.current) return;
    if (inFlightRef.current) return;
    if (anchor === null) return;
    if (useCycleStore.getState().state !== 'idle') return;
    const p = anchor.position;
    const q = anchor.quaternion;
    const target = {
      pos: [p.x, p.y, p.z] as [number, number, number],
      quat: [q.x, q.y, q.z, q.w] as [number, number, number, number],
    };
    const last = lastFiredPoseRef.current;
    if (last !== null && posesClose(target, last)) {
      // Robot already at (or commanded to) the gizmo's live pose.
      // If the operator has released, the follow window closes.
      if (!draggingRef.current) shouldFollowRef.current = false;
      return;
    }
    inFlightRef.current = true;
    void api.direct
      .cartesianJog(target.pos, target.quat)
      .then(() => {
        lastFiredPoseRef.current = target;
      })
      .catch((err: unknown) => {
        // 409 / validation errors leave `lastFiredPoseRef`
        // untouched so the next tryFire (from the cycleState
        // listener or onObjectChange) can retry the same pose.
        console.warn('cartesian follow-jog failed', err);
      })
      .finally(() => {
        inFlightRef.current = false;
      });
  }

  // Drive the chain off WS-pushed cycleState transitions to IDLE.
  // The `execute_intervention` HTTP response resolves before the
  // motion finishes, so we use the state transition as the only
  // reliable "the previous segment really completed" signal.
  useEffect(() => {
    const unsub = useCycleStore.subscribe((s, prev) => {
      if (s.state === 'idle' && prev.state !== 'idle') {
        tryFire();
      }
    });
    return unsub;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [anchor]);

  if (robotMode !== 'cartesian' || initialPose === null) {
    return null;
  }

  const gizmoSpace: 'world' | 'local' = cartesianFrame === 'tcp' ? 'local' : 'world';

  return (
    <>
      <mesh
        ref={(m) => {
          // Seed pose synchronously on mount so the gizmo doesn't
          // flash at the origin before the useEffect fires.
          if (m !== null && initialPose !== null && !draggingRef.current) {
            m.position.set(...initialPose.position);
            m.quaternion.set(...initialPose.quaternion);
          }
          setAnchor(m);
        }}
        // Make the anchor invisible but not hidden (drei TransformControls
        // needs the underlying Object3D to compute its handles).
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
            draggingRef.current = true;
            shouldFollowRef.current = true;
            // Reset the last-fired marker so a re-grab from the
            // same pose can still issue a jog if needed.
            lastFiredPoseRef.current = null;
            useViewerSelectionStore.getState().setDraggingGizmo(true);
            tryFire();
          }}
          onObjectChange={() => {
            if (!draggingRef.current) return;
            // Will fire only if state is idle. Otherwise the
            // cycleStore subscriber will catch the next IDLE
            // transition and fire with the live pose then.
            tryFire();
          }}
          onMouseUp={() => {
            draggingRef.current = false;
            useViewerSelectionStore.getState().setDraggingGizmo(false);
            // `shouldFollowRef` stays true; tryFire keeps firing
            // until the robot reaches the release pose, at which
            // point posesClose() trips the early-return that
            // clears the follow flag.
            tryFire();
          }}
        />
      )}
    </>
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
  // visibility from .viewer_frame_visible (now state-owned).
  const objects = useSceneStateStore((s) => s.scene?.objects ?? EMPTY_OBJECTS);
  const gizmoSize = useSceneStateStore((s) => s.frames?.frameGizmoSizeM ?? 0.1);
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
