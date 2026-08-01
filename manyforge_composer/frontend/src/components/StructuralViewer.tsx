import { Component, Suspense, useMemo, useState } from 'react';
import type { ReactNode } from 'react';
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
import type {
  SceneUrdfCollision,
  SceneUrdfManifest,
  SceneUrdfPrimitive,
  SceneUrdfVisual,
} from '../types/api';
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
}

interface AssetErrorBoundaryProps {
  resetKey: string;
  onError: (error: Error) => void;
  children: ReactNode;
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

function AuthoredNeutralModel({
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

function ViewerCanvas({
  manifest,
  collisionsVisible,
  onAssetError,
  camera,
}: {
  manifest: SceneUrdfManifest;
  collisionsVisible: boolean;
  onAssetError: (error: Error) => void;
  camera: ViewerCamera | null;
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
      <AssetErrorBoundary
        resetKey={manifest.model_key}
        onError={onAssetError}
      >
        <Suspense fallback={null}>
          <Bounds fit={camera === null} clip observe margin={1.25}>
            <AuthoredNeutralModel
              manifest={manifest}
              collisionsVisible={collisionsVisible}
            />
          </Bounds>
        </Suspense>
      </AssetErrorBoundary>
    </Canvas>
  );
}

export function StructuralViewer({ manifest, onRetry }: StructuralViewerProps) {
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
    + (collisionsVisible ? collisionCount : 0);

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
          <p className="eyebrow">Authored structure · not live state</p>
          <h2 id="viewer-title">Structural model</h2>
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

      <div className="viewer-stage" role="img" aria-label="Authored-neutral URDF model">
        {manifest.status === 'ready' && visibleGeometryCount > 0 && (
          <ViewerCanvas
            key={`${manifest.data.model_key}:${collisionsVisible ? 'collision' : 'visual'}`}
            manifest={manifest.data}
            collisionsVisible={collisionsVisible}
            onAssetError={setAssetError}
            camera={camera}
          />
        )}

        {manifest.status === 'loading' && (
          <div className="viewer-state" role="status">
            <strong>Loading authored model…</strong>
            <span>Waiting for a complete authored-neutral projection.</span>
          </div>
        )}
        {manifest.status === 'absent' && (
          <div className="viewer-state">
            <strong>No authored deployment model</strong>
            <span>{manifest.message}</span>
            <span>No empty or placeholder robot is substituted.</span>
          </div>
        )}
        {manifest.status === 'error' && (
          <div className="viewer-state error-state" role="alert">
            <strong>Structural manifest unavailable</strong>
            <span>{manifest.message}</span>
            <button type="button" onClick={retry}>Retry manifest</button>
          </div>
        )}
        {manifest.status === 'ready' && visibleGeometryCount === 0 && (
          <div className="viewer-state">
            <strong>No geometry selected for display</strong>
            <span>
              {collisionCount > 0
                ? 'This URDF has no visual geometry. Enable collision geometry to inspect its authored collision shapes.'
                : 'The authored link tree contains no renderable visual or collision geometry.'}
            </span>
            <span>No placeholder geometry is substituted.</span>
          </div>
        )}
        {manifest.status === 'ready' && assetError !== null && (
          <div className="viewer-state viewer-state-overlay error-state" role="alert">
            <strong>
              {classifyStructuralAssetFailure(assetError) === 'stale_model'
                ? 'Authored model changed'
                : 'Structural asset unavailable'}
            </strong>
            <span>
              {classifyStructuralAssetFailure(assetError) === 'stale_model'
                ? 'A keyed mesh belongs to an inactive model. Reload the manifest.'
                : assetError.message}
            </span>
            <button type="button" onClick={retry}>Reload authored model</button>
          </div>
        )}

        {manifest.status === 'ready' && assetError === null && visibleGeometryCount > 0 && (
          <div className="viewer-legend">
            <span>{manifest.data.robot_id ?? 'unnamed authored robot'}</span>
            <span>{manifest.data.links.length} links</span>
            <span>{manifest.data.joints.length} joints</span>
            <span>neutral coordinates</span>
          </div>
        )}
      </div>

      {manifest.status === 'ready' && (
        <footer className="viewer-contract">
          <span>Root: <code>{manifest.data.projection.root_frame}</code></span>
          <span>Model key: <code>{manifest.data.model_key}</code></span>
          <span>Runtime state used: <strong>no</strong></span>
          <span>Operational validity: <strong>not asserted</strong></span>
        </footer>
      )}
    </section>
  );
}
