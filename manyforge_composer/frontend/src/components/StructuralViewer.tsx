import { Component, Suspense, useMemo, useState } from 'react';
import type { ErrorInfo, ReactNode } from 'react';
import { Canvas } from '@react-three/fiber';
import {
  Bounds,
  Grid,
  OrbitControls,
  PerspectiveCamera,
  useGLTF,
} from '@react-three/drei';
import * as THREE from 'three';
import { resolveStructuralAssetUrl } from '../api/client';
import type { StructuralResourceState } from '../state/structuralResource';
import type { RuntimeScenePresentation } from '../state/runtimeScenePresentation';
import type {
  LogicalSceneObject,
  LogicalSceneShape,
  ProgramViewerMetadata,
  SceneUrdfCollision,
  SceneUrdfManifest,
  SceneUrdfPrimitive,
  SceneUrdfVisual,
} from '../types/api';
import type { Pose3Value } from '../types/robotControl';
import {
  classifyStructuralAssetFailure,
  normalizeQuaternion,
} from '../utils/structuralTransforms';
import {
  DEFAULT_VIEWER_CAMERA,
  parseViewerCameraQuery,
  type ViewerCamera,
} from '../utils/viewerCamera';
import {
  inspectViewerRenderer,
  selectViewerPresentationProfile,
  type ViewerPresentationProfile,
  type ViewerRendererInspection,
} from '../utils/viewerPerformance';

interface StructuralViewerProps {
  manifest: StructuralResourceState<SceneUrdfManifest>;
  onRetry: () => void;
  sceneObjects?: LogicalSceneObject[];
  selectedSceneResourceId?: string | null;
  onSelectSceneResource?: (sceneResourceId: string | null) => void;
  candidateTcpPose?: Pose3Value | null;
  candidateStale?: boolean;
  runtimePresentation?: RuntimeScenePresentation;
  viewer?: ProgramViewerMetadata;
}

interface AssetErrorBoundaryProps {
  resetKey: string;
  onError: (error: Error) => void;
  children: ReactNode;
}

interface ViewerFailureBoundaryProps {
  fallback: ReactNode;
  children: ReactNode;
}

interface ViewerFailureBoundaryState {
  failed: boolean;
}

/** Keep WebGL/Three failures local so Program authoring remains usable. */
export class ViewerFailureBoundary extends Component<
  ViewerFailureBoundaryProps,
  ViewerFailureBoundaryState
> {
  state: ViewerFailureBoundaryState = { failed: false };

  static getDerivedStateFromError(): ViewerFailureBoundaryState {
    return { failed: true };
  }

  componentDidCatch(error: Error, info: ErrorInfo): void {
    console.error('Scene authoring viewport failed:', error, info.componentStack);
  }

  render(): ReactNode {
    return this.state.failed ? this.props.fallback : this.props.children;
  }
}

class AssetErrorBoundary extends Component<
  AssetErrorBoundaryProps,
  { failed: boolean }
> {
  state = { failed: false };

  static getDerivedStateFromError(): { failed: boolean } {
    return { failed: true };
  }

  componentDidCatch(error: Error): void {
    this.props.onError(error);
  }

  componentDidUpdate(previous: AssetErrorBoundaryProps): void {
    if (previous.resetKey !== this.props.resetKey && this.state.failed) {
      this.setState({ failed: false });
    }
  }

  render(): ReactNode {
    return this.state.failed ? null : this.props.children;
  }
}

function VisualMesh({ url }: { url: string }) {
  const gltf = useGLTF(resolveStructuralAssetUrl(url));
  const clone = useMemo(() => gltf.scene.clone(true), [gltf.scene]);
  return <primitive object={clone} />;
}

function CollisionMesh({ url }: { url: string }) {
  const gltf = useGLTF(resolveStructuralAssetUrl(url));
  const clone = useMemo(() => {
    const root = gltf.scene.clone(true);
    root.traverse((object) => {
      const mesh = object as THREE.Mesh;
      if (!mesh.isMesh) {
        return;
      }
      const sourceMaterials = Array.isArray(mesh.material)
        ? mesh.material
        : [mesh.material];
      const collisionMaterials = sourceMaterials.map((source) => {
        const material = source.clone() as THREE.MeshStandardMaterial;
        material.color?.set('#ffb44a');
        material.emissive?.set('#6f2a12');
        material.transparent = true;
        material.opacity = 0.42;
        material.wireframe = true;
        material.depthWrite = false;
        material.side = THREE.DoubleSide;
        return material;
      });
      mesh.material = Array.isArray(mesh.material)
        ? collisionMaterials
        : collisionMaterials[0];
      mesh.renderOrder = 10;
    });
    return root;
  }, [gltf.scene]);
  return <primitive object={clone} />;
}

function MeasuredVisualMesh({ url }: { url: string }) {
  const gltf = useGLTF(resolveStructuralAssetUrl(url));
  const clone = useMemo(() => {
    const root = gltf.scene.clone(true);
    root.traverse((object) => {
      const mesh = object as THREE.Mesh;
      if (!mesh.isMesh) return;
      const sourceMaterials = Array.isArray(mesh.material)
        ? mesh.material
        : [mesh.material];
      const materials = sourceMaterials.map(() => new THREE.MeshStandardMaterial({
        color: '#43d7ec',
        emissive: '#0c5060',
        transparent: true,
        opacity: 0.34,
        wireframe: true,
        depthWrite: false,
        side: THREE.DoubleSide,
      }));
      mesh.material = Array.isArray(mesh.material) ? materials : materials[0];
      mesh.renderOrder = 14;
    });
    return root;
  }, [gltf.scene]);
  return <primitive object={clone} />;
}

function PrimitiveGeometry({ primitive }: { primitive: SceneUrdfPrimitive }) {
  if (primitive.type === 'box') {
    return <boxGeometry args={primitive.size_m} />;
  }
  if (primitive.type === 'cylinder') {
    return (
      <cylinderGeometry
        args={[
          primitive.radius_m,
          primitive.radius_m,
          primitive.length_m,
          28,
        ]}
      />
    );
  }
  return <sphereGeometry args={[primitive.radius_m, 28, 18]} />;
}

function PrimitiveVisual({ primitive }: { primitive: SceneUrdfPrimitive }) {
  const cylinderRotation: [number, number, number] | undefined =
    primitive.type === 'cylinder'
    ? [Math.PI / 2, 0, 0]
    : undefined;
  return (
    <mesh rotation={cylinderRotation}>
      <PrimitiveGeometry primitive={primitive} />
      <meshStandardMaterial color="#aebdcc" metalness={0.18} roughness={0.62} />
    </mesh>
  );
}

function PrimitiveCollision({ primitive }: { primitive: SceneUrdfPrimitive }) {
  const cylinderRotation: [number, number, number] | undefined =
    primitive.type === 'cylinder'
    ? [Math.PI / 2, 0, 0]
    : undefined;
  return (
    <mesh rotation={cylinderRotation} renderOrder={10}>
      <PrimitiveGeometry primitive={primitive} />
      <meshStandardMaterial
        color="#ffb44a"
        emissive="#6f2a12"
        transparent
        opacity={0.42}
        wireframe
        depthWrite={false}
        side={THREE.DoubleSide}
      />
    </mesh>
  );
}

function PrimitiveMeasured({ primitive }: { primitive: SceneUrdfPrimitive }) {
  const cylinderRotation: [number, number, number] | undefined =
    primitive.type === 'cylinder'
    ? [Math.PI / 2, 0, 0]
    : undefined;
  return (
    <mesh rotation={cylinderRotation} renderOrder={14}>
      <PrimitiveGeometry primitive={primitive} />
      <meshStandardMaterial
        color="#43d7ec"
        emissive="#0c5060"
        transparent
        opacity={0.34}
        wireframe
        depthWrite={false}
        side={THREE.DoubleSide}
      />
    </mesh>
  );
}

function RobotVisual({ visual }: { visual: SceneUrdfVisual }) {
  const origin = visual.visual_origin;
  return (
    <group
      position={origin.position_m}
      quaternion={normalizeQuaternion(origin.orientation_xyzw)}
    >
      {visual.mesh_url !== null && <VisualMesh url={visual.mesh_url} />}
      {visual.mesh_url === null && visual.primitive !== null && (
        <PrimitiveVisual primitive={visual.primitive} />
      )}
    </group>
  );
}

function RobotCollision({ collision }: { collision: SceneUrdfCollision }) {
  const origin = collision.collision_origin;
  return (
    <group
      position={origin.position_m}
      quaternion={normalizeQuaternion(origin.orientation_xyzw)}
    >
      {collision.mesh_url !== null && <CollisionMesh url={collision.mesh_url} />}
      {collision.mesh_url === null && collision.primitive !== null && (
        <PrimitiveCollision primitive={collision.primitive} />
      )}
    </group>
  );
}

function RetainedNeutralModel({
  manifest,
  collisionsVisible,
}: {
  manifest: SceneUrdfManifest;
  collisionsVisible: boolean;
}) {
  return (
    <group name="authored-neutral-urdf">
      {manifest.links.map((link) => (
        <group
          key={`${manifest.model_key}:${link.name}`}
          name={link.name}
          position={link.neutral_transform.position_m}
          quaternion={normalizeQuaternion(
            link.neutral_transform.orientation_xyzw,
          )}
        >
          {link.visuals.map((visual) => (
            <RobotVisual
              key={`visual:${link.name}:${visual.index}`}
              visual={visual}
            />
          ))}
          {collisionsVisible && link.collisions.map((collision) => (
            <RobotCollision
              key={`collision:${link.name}:${collision.index}`}
              collision={collision}
            />
          ))}
        </group>
      ))}
    </group>
  );
}

function MeasuredRuntimeModel({
  manifest,
  presentation,
}: {
  manifest: SceneUrdfManifest;
  presentation: NonNullable<RuntimeScenePresentation['measuredRobot']>;
}) {
  return (
    <group name={`owner-measured-robot:${presentation.objectId}`}>
      {manifest.links.map((link) => {
        const transform = presentation.linkTransforms[link.name];
        if (transform === undefined) return null;
        return (
          <group
            key={`measured:${manifest.model_key}:${link.name}`}
            name={`measured:${link.name}`}
            position={transform.position_m}
            quaternion={normalizeQuaternion(transform.orientation_xyzw)}
          >
            {link.visuals.map((visual) => {
              const origin = visual.visual_origin;
              return (
                <group
                  key={`measured-visual:${link.name}:${visual.index}`}
                  position={origin.position_m}
                  quaternion={normalizeQuaternion(origin.orientation_xyzw)}
                >
                  {visual.mesh_url !== null && (
                    <MeasuredVisualMesh url={visual.mesh_url} />
                  )}
                  {visual.mesh_url === null && visual.primitive !== null && (
                    <PrimitiveMeasured primitive={visual.primitive} />
                  )}
                </group>
              );
            })}
          </group>
        );
      })}
    </group>
  );
}

function LogicalShapeGeometry({ shape }: { shape: LogicalSceneShape }) {
  if (shape.type === 'box') {
    return <boxGeometry args={[shape.dimensions_m.x, shape.dimensions_m.y, shape.dimensions_m.z]} />;
  }
  if (shape.type === 'sphere') {
    return <sphereGeometry args={[shape.radius_m, 28, 18]} />;
  }
  if (shape.type === 'cylinder') {
    return (
      <cylinderGeometry
        args={[shape.radius_m, shape.radius_m, shape.height_m, 28]}
      />
    );
  }
  return null;
}

function LogicalSceneObjects({
  objects,
  selectedSceneResourceId,
  onSelect,
  axesSizeM,
}: {
  objects: LogicalSceneObject[];
  selectedSceneResourceId: string | null;
  onSelect: (sceneResourceId: string) => void;
  axesSizeM: number;
}) {
  return (
    <group name="owner-logical-scene">
      {objects.map((object) => {
        if (object.placement.kind !== 'cell_fixed') return null;
        const pose = object.placement.pose_in_cell;
        const selected = selectedSceneResourceId === object.scene_resource_id;
        return (
          <group
            key={object.scene_resource_id}
            name={`scene:${object.scene_resource_id}`}
            position={[pose.position_m.x, pose.position_m.y, pose.position_m.z]}
            quaternion={normalizeQuaternion(pose.orientation_xyzw)}
            onClick={(event) => {
              event.stopPropagation();
              onSelect(object.scene_resource_id);
            }}
          >
            {object.collision_shapes.map((shape, index) => shape.type === 'mesh'
              ? null
              : (
                  <mesh
                    // Three cylinders use Y as their long axis; scene shapes use Z.
                    rotation={shape.type === 'cylinder' ? [Math.PI / 2, 0, 0] : undefined}
                    key={`${object.scene_resource_id}:${index}`}
                    renderOrder={selected ? 12 : 8}
                  >
                    <LogicalShapeGeometry shape={shape} />
                    <meshStandardMaterial
                      color={selected ? '#65d3f2' : '#8c6fe0'}
                      emissive={selected ? '#144c62' : '#251a48'}
                      transparent
                      opacity={selected ? 0.72 : 0.5}
                      wireframe
                      depthWrite={false}
                    />
                  </mesh>
                ))}
            {selected && <axesHelper args={[axesSizeM]} />}
          </group>
        );
      })}
    </group>
  );
}

function MeasuredLogicalSceneObjects({
  objects,
  presentation,
  onSelect,
}: {
  objects: LogicalSceneObject[];
  presentation: RuntimeScenePresentation;
  onSelect: (sceneResourceId: string) => void;
}) {
  const objectById = new Map(
    objects.map((object) => [object.scene_resource_id, object]),
  );
  return (
    <group name="owner-measured-scene">
      {presentation.measuredObjects.map((measurement) => {
        const object = objectById.get(measurement.objectId);
        if (object === undefined) return null;
        return (
          <group
            key={`measured-scene:${measurement.objectId}:${measurement.sourceSequence}`}
            name={`measured-scene:${measurement.objectId}`}
            position={measurement.pose.position_m}
            quaternion={normalizeQuaternion(measurement.pose.orientation_xyzw)}
            onClick={(event) => {
              event.stopPropagation();
              onSelect(measurement.objectId);
            }}
          >
            {object.collision_shapes.map((shape, index) => shape.type === 'mesh'
              ? null
              : (
                  <mesh
                    key={`measured-scene:${measurement.objectId}:${index}`}
                    rotation={shape.type === 'cylinder' ? [Math.PI / 2, 0, 0] : undefined}
                    renderOrder={16}
                  >
                    <LogicalShapeGeometry shape={shape} />
                    <meshStandardMaterial
                      color="#43d7ec"
                      emissive="#0c5060"
                      transparent
                      opacity={0.4}
                      wireframe
                      depthWrite={false}
                    />
                  </mesh>
                ))}
          </group>
        );
      })}
    </group>
  );
}

function CandidateTcpGhost({
  pose,
  stale,
  axesSizeM,
}: {
  pose: Pose3Value;
  stale: boolean;
  axesSizeM: number;
}) {
  return (
    <group
      name="owner-preview-final-tcp"
      position={[pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m]}
      quaternion={normalizeQuaternion([
        pose.orientation.x,
        pose.orientation.y,
        pose.orientation.z,
        pose.orientation.w,
      ])}
    >
      <mesh renderOrder={20}>
        <sphereGeometry args={[Math.max(axesSizeM * 0.08, 0.006), 18, 12]} />
        <meshStandardMaterial
          color={stale ? '#e0a45c' : '#74e3b2'}
          emissive={stale ? '#5a3216' : '#164f3d'}
          transparent
          opacity={0.82}
          depthWrite={false}
        />
      </mesh>
      <axesHelper args={[axesSizeM]} />
    </group>
  );
}

function MeasuredTcpMarker({
  presentation,
  axesSizeM,
}: {
  presentation: NonNullable<RuntimeScenePresentation['measuredTcp']>;
  axesSizeM: number;
}) {
  return (
    <group
      name={`owner-measured-tcp:${presentation.objectId}`}
      position={presentation.pose.position_m}
      quaternion={normalizeQuaternion(presentation.pose.orientation_xyzw)}
    >
      <mesh renderOrder={18}>
        <octahedronGeometry args={[Math.max(axesSizeM * 0.1, 0.008), 0]} />
        <meshStandardMaterial
          color="#43d7ec"
          emissive="#0c5060"
          transparent
          opacity={0.88}
          depthWrite={false}
        />
      </mesh>
      <axesHelper args={[axesSizeM]} />
    </group>
  );
}

function ViewerCanvas({
  manifest,
  collisionsVisible,
  onAssetError,
  camera,
  sceneObjects,
  selectedSceneResourceId,
  onSelectSceneResource,
  candidateTcpPose,
  candidateStale,
  runtimePresentation,
  viewer,
}: {
  manifest: SceneUrdfManifest | null;
  collisionsVisible: boolean;
  onAssetError: (error: Error) => void;
  camera: ViewerCamera | null;
  sceneObjects: LogicalSceneObject[];
  selectedSceneResourceId: string | null;
  onSelectSceneResource: (sceneResourceId: string | null) => void;
  candidateTcpPose: Pose3Value | null;
  candidateStale: boolean;
  runtimePresentation: RuntimeScenePresentation | undefined;
  viewer: ProgramViewerMetadata | undefined;
}) {
  const devicePixelRatio = typeof window === 'undefined'
    ? 1
    : window.devicePixelRatio;
  const [renderer, setRenderer] = useState<
    ViewerRendererInspection & ViewerPresentationProfile
  >(() => ({
    vendor: null,
    renderer: null,
    software: false,
    ...selectViewerPresentationProfile(false, devicePixelRatio),
  }));
  const selectedCamera = camera ?? DEFAULT_VIEWER_CAMERA;

  return (
    <Canvas
      key={`${renderer.software ? 'software' : 'hardware'}:${renderer.preserveDrawingBuffer ? 'preserved' : 'transient'}`}
      frameloop="demand"
      dpr={renderer.effectiveDpr}
      gl={{
        antialias: renderer.antialias,
        alpha: false,
        powerPreference: 'high-performance',
        preserveDrawingBuffer: renderer.preserveDrawingBuffer,
      }}
      onCreated={({ gl }) => {
        const inspection = inspectViewerRenderer(gl);
        const presentation = selectViewerPresentationProfile(
          inspection.software,
          devicePixelRatio,
        );
        setRenderer((current) => {
          const next = { ...inspection, ...presentation };
          return JSON.stringify(current) === JSON.stringify(next) ? current : next;
        });
      }}
      style={{ background: '#081019' }}
      onPointerMissed={() => onSelectSceneResource(null)}
    >
      <color attach="background" args={['#081019']} />
      <PerspectiveCamera
        makeDefault
        up={[0, 0, 1]}
        position={selectedCamera.position}
        fov={48}
        near={0.005}
        far={250}
      />
      <ambientLight intensity={0.9} />
      <hemisphereLight args={['#dbeaff', '#121d27', 0.75]} />
      <directionalLight position={[5, -4, 9]} intensity={1.45} />
      <directionalLight position={[-4, 5, 5]} intensity={0.55} />
      <Grid
        args={[12, 12]}
        cellSize={0.1}
        cellThickness={0.35}
        cellColor="#243341"
        sectionSize={1}
        sectionThickness={0.85}
        sectionColor="#496071"
        fadeDistance={12}
        fadeStrength={1.6}
        infiniteGrid={false}
        followCamera={false}
        rotation={[Math.PI / 2, 0, 0]}
      />
      <axesHelper args={[0.45]} />
      <OrbitControls
        makeDefault
        screenSpacePanning
        enableDamping
        dampingFactor={0.1}
        target={selectedCamera.target}
      />
      {manifest !== null && (
        <AssetErrorBoundary
          resetKey={manifest.model_key}
          onError={onAssetError}
        >
          <Suspense fallback={null}>
            <Bounds fit={camera === null} clip observe margin={1.25}>
              <RetainedNeutralModel
                manifest={manifest}
                collisionsVisible={collisionsVisible}
              />
              {runtimePresentation?.measuredRobot !== null
                && runtimePresentation?.measuredRobot !== undefined && (
                <MeasuredRuntimeModel
                  manifest={manifest}
                  presentation={runtimePresentation.measuredRobot}
                />
              )}
            </Bounds>
          </Suspense>
        </AssetErrorBoundary>
      )}
      <LogicalSceneObjects
        objects={sceneObjects}
        selectedSceneResourceId={selectedSceneResourceId}
        onSelect={(sceneResourceId) => onSelectSceneResource(sceneResourceId)}
        axesSizeM={viewer?.object_axes_gizmo_size_m ?? 0.1}
      />
      {runtimePresentation !== undefined && (
        <MeasuredLogicalSceneObjects
          objects={sceneObjects}
          presentation={runtimePresentation}
          onSelect={(sceneResourceId) => onSelectSceneResource(sceneResourceId)}
        />
      )}
      {runtimePresentation?.measuredTcp !== null
        && runtimePresentation?.measuredTcp !== undefined && (
        <MeasuredTcpMarker
          presentation={runtimePresentation.measuredTcp}
          axesSizeM={viewer?.frame_gizmo_size_m ?? 0.1}
        />
      )}
      {candidateTcpPose !== null && (
        <CandidateTcpGhost
          pose={candidateTcpPose}
          stale={candidateStale}
          axesSizeM={viewer?.frame_gizmo_size_m ?? 0.1}
        />
      )}
    </Canvas>
  );
}

function StructuralViewerContent({
  manifest,
  onRetry,
  sceneObjects = [],
  selectedSceneResourceId = null,
  onSelectSceneResource = () => undefined,
  candidateTcpPose = null,
  candidateStale = false,
  runtimePresentation,
  viewer,
}: StructuralViewerProps) {
  const [collisionsVisible, setCollisionsVisible] = useState(false);
  const [assetError, setAssetError] = useState<Error | null>(null);
  const camera = useMemo(
    () => parseViewerCameraQuery(
      typeof window === 'undefined' ? '' : window.location.search,
    ),
    [],
  );

  const visualCount = manifest.status === 'ready'
    ? manifest.data.links.reduce((total, link) => total + link.visuals.length, 0)
    : 0;
  const collisionCount = manifest.status === 'ready'
    ? manifest.data.links.reduce(
        (total, link) => total + link.collisions.length,
        0,
      )
    : 0;
  const visibleGeometryCount = visualCount
    + (collisionsVisible ? collisionCount : 0)
    + sceneObjects.reduce(
      (total, object) => total + object.collision_shapes.filter(
        (shape) => shape.type !== 'mesh',
      ).length,
      0,
    )
    + (candidateTcpPose === null ? 0 : 1)
    + (runtimePresentation?.measuredRobot === null
      || runtimePresentation?.measuredRobot === undefined
      ? 0
      : visualCount)
    + (runtimePresentation?.measuredObjects.reduce(
      (total, measurement) => {
        const object = sceneObjects.find(
          (candidate) => candidate.scene_resource_id === measurement.objectId,
        );
        return total + (object?.collision_shapes.filter(
          (shape) => shape.type !== 'mesh',
        ).length ?? 0);
      }, 0,
    ) ?? 0)
    + (runtimePresentation?.measuredTcp === null
      || runtimePresentation?.measuredTcp === undefined ? 0 : 1);

  const retry = () => {
    if (manifest.status === 'ready') {
      const clear = (useGLTF as unknown as {
        clear?: (url: string) => void;
      }).clear;
      for (const link of manifest.data.links) {
        for (const url of [
          ...link.visuals.map((visual) => visual.mesh_url),
          ...link.collisions.map((collision) => collision.mesh_url),
        ]) {
          if (url !== null) {
            clear?.(resolveStructuralAssetUrl(url));
          }
        }
      }
    }
    setAssetError(null);
    onRetry();
  };

  return (
    <section className="panel viewer-panel" aria-labelledby="viewer-title">
      <header className="panel-header viewer-header">
        <div>
          <p className="eyebrow">Active retained structure + owner-issued scene</p>
          <h2 id="viewer-title">Scene authoring viewport</h2>
        </div>
        <div className="viewer-controls">
          <label className="toggle-control">
            <input
              type="checkbox"
              checked={collisionsVisible}
              onChange={(event) => setCollisionsVisible(event.target.checked)}
              disabled={manifest.status !== 'ready'}
            />
            <span>Collision geometry</span>
          </label>
          <button type="button" className="quiet-button" onClick={retry}>
            Reload model
          </button>
        </div>
      </header>

      <div className="viewer-stage" role="img" aria-label="Scene authoring viewport">
        {visibleGeometryCount > 0 && (
          <ViewerCanvas
            key={`${manifest.status === 'ready' ? manifest.data.model_key : 'no-structural-model'}:${collisionsVisible ? 'collision' : 'visual'}`}
            manifest={manifest.status === 'ready' ? manifest.data : null}
            collisionsVisible={collisionsVisible}
            onAssetError={setAssetError}
            camera={camera}
            sceneObjects={sceneObjects}
            selectedSceneResourceId={selectedSceneResourceId}
            onSelectSceneResource={onSelectSceneResource}
            candidateTcpPose={candidateTcpPose}
            candidateStale={candidateStale}
            runtimePresentation={runtimePresentation}
            viewer={viewer}
          />
        )}

        {manifest.status === 'loading' && visibleGeometryCount === 0 && (
          <div className="viewer-state" role="status">
            <strong>Loading active model…</strong>
            <span>Waiting for a complete retained-neutral projection.</span>
          </div>
        )}
        {manifest.status === 'absent' && visibleGeometryCount === 0 && (
          <div className="viewer-state">
            <strong>No active retained deployment model</strong>
            <span>{manifest.message}</span>
            <span>No empty or placeholder robot is substituted.</span>
          </div>
        )}
        {manifest.status === 'error' && visibleGeometryCount === 0 && (
          <div className="viewer-state error-state" role="alert">
            <strong>Structural manifest unavailable</strong>
            <span>{manifest.message}</span>
            <button type="button" onClick={retry}>Retry manifest</button>
          </div>
        )}
        {visibleGeometryCount === 0 && manifest.status === 'ready' && (
          <div className="viewer-state">
            <strong>No geometry selected for display</strong>
            <span>
              {collisionCount > 0
                ? 'This URDF has no visual geometry. Enable collision geometry to inspect its retained collision shapes.'
                : 'The retained link tree contains no renderable visual or collision geometry.'}
            </span>
            <span>No placeholder geometry is substituted.</span>
          </div>
        )}
        {manifest.status === 'ready' && assetError !== null && (
          <div className="viewer-state viewer-state-overlay error-state" role="alert">
            <strong>
              {classifyStructuralAssetFailure(assetError) === 'stale_model'
                ? 'Active model changed'
                : 'Structural asset unavailable'}
            </strong>
            <span>
              {classifyStructuralAssetFailure(assetError) === 'stale_model'
                ? 'A keyed mesh belongs to an inactive model. Reload the manifest.'
                : assetError.message}
            </span>
            <button type="button" onClick={retry}>Reload active model</button>
          </div>
        )}

        {manifest.status === 'ready' && assetError === null && visibleGeometryCount > 0 && (
          <div className="viewer-legend">
            <span>{manifest.data.cell_id}</span>
            <span>{manifest.data.links.length} links</span>
            <span>{manifest.data.joints.length} joints</span>
            <span>neutral coordinates</span>
            <span>{sceneObjects.length} owner scene objects</span>
            {candidateTcpPose !== null && <span>validated TCP candidate</span>}
            {runtimePresentation?.measuredRobot !== null
              && runtimePresentation?.measuredRobot !== undefined
              && <span>measured robot</span>}
            {(runtimePresentation?.measuredObjects.length ?? 0) > 0
              && <span>{runtimePresentation?.measuredObjects.length} measured objects</span>}
            {runtimePresentation?.measuredTcp !== null
              && runtimePresentation?.measuredTcp !== undefined
              && <span>measured TCP</span>}
          </div>
        )}
      </div>

      {manifest.status === 'ready' && (
        <footer className="viewer-contract">
          <span>Root: <code>{manifest.data.projection.root_frame}</code></span>
          <span>Model key: <code>{manifest.data.model_key}</code></span>
          <span>Bundle: <code>{manifest.data.retained_bundle_id}</code></span>
          <span>Retained model: <strong>neutral coordinates</strong></span>
          <span>
            Measured overlay: <strong>
              {runtimePresentation === undefined
                ? 'unavailable'
                : `${runtimePresentation.status.freshGood} fresh / ${runtimePresentation.status.total} sources`}
            </strong>
          </span>
          <span>Scene overlay: <strong>owner-issued</strong></span>
          <span>Operational validity: <strong>not asserted</strong></span>
        </footer>
      )}
    </section>
  );
}

export function StructuralViewer(props: StructuralViewerProps) {
  const [recoveryGeneration, setRecoveryGeneration] = useState(0);
  const retry = () => {
    setRecoveryGeneration((current) => current + 1);
    props.onRetry();
  };
  const fallback = (
    <section className="panel viewer-panel" aria-labelledby="viewer-failure-title">
      <div className="viewer-state error-state" role="alert">
        <strong id="viewer-failure-title">3D viewer isolated</strong>
        <span>
          The viewport failed, but Program Draft and runtime controls remain usable and unchanged.
        </span>
        <button type="button" onClick={retry}>Retry viewer</button>
      </div>
    </section>
  );
  return (
    <ViewerFailureBoundary key={recoveryGeneration} fallback={fallback}>
      <StructuralViewerContent {...props} />
    </ViewerFailureBoundary>
  );
}
