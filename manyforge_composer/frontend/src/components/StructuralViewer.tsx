import {
  Component,
  Profiler,
  Suspense,
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import type { ErrorInfo, MouseEvent as ReactMouseEvent, ReactNode } from 'react';
import type { PointerEvent as ReactPointerEvent } from 'react';
import { createPortal } from 'react-dom';
import { Canvas, useFrame, useThree } from '@react-three/fiber';
import type { ThreeEvent } from '@react-three/fiber';
import {
  Bounds,
  Grid,
  OrbitControls,
  PerspectiveCamera,
  TransformControls,
  useBounds,
  useGLTF,
} from '@react-three/drei';
import * as THREE from 'three';
import { api, resolveStructuralAssetUrl } from '../api/client';
import {
  subscribeSnapshotObserver,
  uploadSnapshotObserverResult,
} from '../api/snapshotObserverClient';
import type { PointCloudFrame } from '../api/pointcloudObserverClient';
import { usePointCloudFrame } from '../hooks/usePointCloudFrame';
import { SnapshotResponder } from './SnapshotResponder';
import {
  applyAuthoritativeTransformTransition,
  authoritativeInterpolationDurationMs,
  createAuthoritativeTransformTransition,
  sameAuthoritativeTransformFrame,
  sameAuthoritativeTransformPose,
} from '../state/authoritativeInterpolation';
import type {
  AuthoritativeTransformFrame,
  AuthoritativeTransformTransition,
} from '../state/authoritativeInterpolation';
import type { StructuralResourceState } from '../state/structuralResource';
import type {
  CandidateRobotPresentation,
  RuntimeScenePresentation,
} from '../state/runtimeScenePresentation';
import type {
  LogicalSceneObject,
  LogicalScenePose,
  LogicalSceneShape,
  ProgramViewerMetadata,
  QuaternionXyzwTuple,
  SceneUrdfCollision,
  SceneUrdfManifest,
  SceneUrdfPrimitive,
  SceneUrdfVisual,
  Vector3Tuple,
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
  connectViewerQualification,
  recordViewerModelBuild,
  recordViewerReactCommit,
  recordViewerRenderFrame,
  recordViewerRenderWork,
  selectViewerPresentationProfile,
  setViewerQualificationCanvasMounted,
  setViewerQualificationModel,
  setViewerQualificationRenderer,
  setViewerQualificationWireframeDefault,
  setViewerQualificationViewerActive,
  setViewerQualificationWireframeVisible,
  startViewerPerformanceMonitoring,
  stopViewerPerformanceMonitoring,
  reportViewerQualificationError,
  viewerQualificationOptedIn,
  type ViewerPresentationProfile,
  type ViewerRendererInspection,
} from '../utils/viewerPerformance';

interface StructuralViewerProps {
  viewerActive: boolean;
  manifest: StructuralResourceState<SceneUrdfManifest>;
  onRetry: () => void;
  sceneObjects?: LogicalSceneObject[];
  programDraftPreview?: boolean;
  programDraftIdentity?: {
    draftId: string;
    draftRevision: number;
  };
  selectedSceneResourceId?: string | null;
  onSelectSceneResource?: (sceneResourceId: string | null) => void;
  selectedStructureName?: string | null;
  onSelectStructure?: (structureName: string | null) => void;
  onCommitSceneTransform?: (
    sceneResourceId: string,
    pose: LogicalScenePose,
    scale: [number, number, number],
    expectedOwnerIdentity: { ownerEpoch: string; generation: number },
  ) => Promise<boolean>;
  onCommitProgramDraftTransform?: (
    sceneResourceId: string,
    pose: LogicalScenePose,
    scale: [number, number, number],
    expectedDraftIdentity: { ownerEpoch: string; generation: number },
  ) => Promise<boolean>;
  onDuplicateSceneResource?: (sceneResourceId: string) => Promise<boolean>;
  onRemoveSceneResource?: (sceneResourceId: string) => Promise<boolean>;
  onDetachSceneResource?: (sceneResourceId: string) => Promise<boolean>;
  onEditSceneResource?: (sceneResourceId: string) => void;
  selectionLocked?: boolean;
  sceneMutationAvailability?: Record<string, {
    duplicate: boolean;
    remove: boolean;
    detach: boolean;
    transform: boolean;
    reason: string | null;
    detachReason: string | null;
  }>;
  candidateTcpPose?: Pose3Value | null;
  candidateStale?: boolean;
  candidateTcpEditable?: boolean;
  onEditCandidateTcpPose?: (pose: Pose3Value) => void;
  candidateRobot?: CandidateRobotPresentation | null;
  runtimePresentation?: RuntimeScenePresentation;
  sceneOwnerIdentity?: {
    ownerEpoch: string;
    generation: number;
  };
  viewerFrameIdentity?: {
    ownerEpoch: string;
    generation: number;
  };
  readViewerFrameIdentity?: () => {
    ownerEpoch: string;
    generation: number;
  } | null;
  pointCloudCellId?: string;
  viewer?: ProgramViewerMetadata;
  frameAxes?: Array<{
    linkName: string;
    frameRoleId: string;
  }>;
  objectAxesVisibleIds?: string[];
  robotControlSurface?: ReactNode;
  robotControlBlocked?: boolean;
  objectControlSurface?: ReactNode;
  transportControls?: ReactNode;
  inspectorTarget?: HTMLElement | null;
  inspectorMinimized?: boolean;
  onInspectorMinimizedChange?: (minimized: boolean) => void;
  onInspectorMaximize?: () => void;
  onInspectorPanelChange?: (panel: ViewerInspectorPanel) => void;
}

export type ViewerInspectorPanel = 'tools' | 'robot' | 'object' | null;

const ignoreViewerRaycast: THREE.Object3D['raycast'] = () => undefined;

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

export const DEFAULT_WIREFRAME_VISIBLE = false;

export function supportsIndependentSceneScale(
  object: LogicalSceneObject | null,
): boolean {
  if (object?.collision_shapes.length !== 1) return false;
  const shape = object.collision_shapes[0];
  return shape.type === 'box' || shape.type === 'mesh';
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
    reportViewerQualificationError(error);
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

function authoritativeStreamIdentity(
  ownerIdentity: StructuralViewerProps['sceneOwnerIdentity'],
  ...parts: Array<string | number>
): string {
  return JSON.stringify([
    ownerIdentity?.ownerEpoch ?? 'unbound-owner',
    ownerIdentity?.generation ?? -1,
    ...parts,
  ]);
}

/**
 * Apply authoritative transforms outside React's commit path. Each source
 * update may create one bounded visual transition; render frames only mutate
 * this stable Three.js group and stop invalidating as soon as the newest
 * authoritative transform is reached.
 */
function AuthoritativeTransformGroup({
  name,
  streamIdentity,
  sourceSequence,
  observedAtNs,
  positionM,
  orientationXyzw,
  latestFrameAppliers,
  onClick,
  children,
}: {
  name: string;
  streamIdentity: string;
  sourceSequence: number;
  observedAtNs: number;
  positionM: Vector3Tuple;
  orientationXyzw: QuaternionXyzwTuple;
  latestFrameAppliers: Set<() => void>;
  onClick?: (event: ThreeEvent<MouseEvent>) => void;
  children: ReactNode;
}) {
  const group = useRef<THREE.Group | null>(null);
  const previous = useRef<AuthoritativeTransformFrame | null>(null);
  const active = useRef<AuthoritativeTransformTransition | null>(null);
  const invalidate = useThree((state) => state.invalidate);
  const [px, py, pz] = positionM;
  const [qx, qy, qz, qw] = orientationXyzw;
  const applyLatestFrame = useCallback(() => {
    if (group.current === null || active.current === null) return;
    group.current.position.copy(active.current.toPosition);
    group.current.quaternion.copy(active.current.toQuaternion);
    active.current = null;
    invalidate();
  }, [invalidate]);

  useLayoutEffect(() => {
    latestFrameAppliers.add(applyLatestFrame);
    return () => {
      latestFrameAppliers.delete(applyLatestFrame);
    };
  }, [applyLatestFrame, latestFrameAppliers]);

  useLayoutEffect(() => {
    if (group.current === null) return;
    const next: AuthoritativeTransformFrame = {
      streamIdentity,
      sourceSequence,
      observedAtNs,
      positionM: [px, py, pz],
      orientationXyzw: [qx, qy, qz, qw],
    };
    if (
      previous.current !== null
      && sameAuthoritativeTransformFrame(previous.current, next)
    ) return;

    if (
      previous.current !== null
      && authoritativeInterpolationDurationMs(previous.current, next) > 0
      && sameAuthoritativeTransformPose(previous.current, next)
    ) {
      previous.current = next;
      return;
    }

    const transition = createAuthoritativeTransformTransition(
      previous.current,
      next,
      group.current.position,
      group.current.quaternion,
      performance.now(),
    );
    previous.current = next;
    active.current = transition;
    if (transition === null) {
      const orientation = normalizeQuaternion(next.orientationXyzw);
      group.current.position.set(...next.positionM);
      group.current.quaternion.set(...orientation);
    }
    invalidate();
  }, [
    invalidate,
    observedAtNs,
    px,
    py,
    pz,
    qw,
    qx,
    qy,
    qz,
    sourceSequence,
    streamIdentity,
  ]);

  useFrame(() => {
    if (group.current === null || active.current === null) return;
    const complete = applyAuthoritativeTransformTransition(
      active.current,
      performance.now(),
      group.current.position,
      group.current.quaternion,
    );
    if (complete) active.current = null;
    else invalidate();
  });

  return <group ref={group} name={name} onClick={onClick}>{children}</group>;
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
  useEffect(() => () => disposeOwnedMaterials(clone), [clone]);
  return <primitive object={clone} />;
}

function disposeOwnedMaterials(root: THREE.Object3D): void {
  root.traverse((object) => {
    const mesh = object as THREE.Mesh;
    if (!mesh.isMesh) return;
    const materials = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
    for (const material of materials) material.dispose();
  });
}

function SceneObjectMeshAsset({
  url,
  measured,
  selected,
  wireframe,
}: {
  url: string;
  measured: boolean;
  selected: boolean;
  wireframe: boolean;
}) {
  const gltf = useGLTF(resolveStructuralAssetUrl(url));
  const clone = useMemo(() => {
    const root = gltf.scene.clone(true);
    root.traverse((object) => {
      const mesh = object as THREE.Mesh;
      if (!mesh.isMesh) return;
      const sources = Array.isArray(mesh.material)
        ? mesh.material
        : [mesh.material];
      const materials = sources.map((source) => {
        const material = source.clone() as THREE.MeshStandardMaterial;
        material.color?.set(measured ? '#43d7ec' : selected ? '#65d3f2' : '#8c6fe0');
        material.emissive?.set(measured ? '#0c5060' : selected ? '#144c62' : '#251a48');
        material.transparent = true;
        material.opacity = measured ? 0.55 : selected ? 0.9 : 0.82;
        material.wireframe = wireframe;
        material.depthWrite = !wireframe;
        material.side = THREE.DoubleSide;
        return material;
      });
      mesh.material = Array.isArray(mesh.material) ? materials : materials[0];
      mesh.renderOrder = measured ? 16 : selected ? 12 : 8;
    });
    return root;
  }, [gltf.scene, measured, selected, wireframe]);
  useEffect(() => () => disposeOwnedMaterials(clone), [clone]);
  return <primitive object={clone} />;
}

function SceneObjectMesh({
  shape,
  ownerIdentity,
  draftIdentity,
  measured,
  selected,
  wireframe,
}: {
  shape: Extract<LogicalSceneShape, { type: 'mesh' }>;
  ownerIdentity?: NonNullable<StructuralViewerProps['sceneOwnerIdentity']>;
  draftIdentity?: NonNullable<StructuralViewerProps['programDraftIdentity']>;
  measured: boolean;
  selected: boolean;
  wireframe: boolean;
}) {
  const [url, setUrl] = useState<string | null>(null);
  const [error, setError] = useState<Error | null>(null);
  const draftId = draftIdentity?.draftId;
  const draftRevision = draftIdentity?.draftRevision;
  const ownerEpoch = ownerIdentity?.ownerEpoch;
  const ownerGeneration = ownerIdentity?.generation;
  const scaleX = shape.scale.x;
  const scaleY = shape.scale.y;
  const scaleZ = shape.scale.z;
  const isolateError = useCallback((failure: Error) => {
    reportViewerQualificationError(failure);
    setError(failure);
  }, [setError]);
  useEffect(() => {
    const controller = new AbortController();
    setUrl(null);
    setError(null);
    const scale = { x: scaleX, y: scaleY, z: scaleZ };
    const resolution = draftId === undefined || draftRevision === undefined
      ? ownerEpoch === undefined || ownerGeneration === undefined
        ? Promise.reject(new Error('Scene mesh has no exact source identity.'))
        : api.scene.resolveMesh(
            shape.resource_uri,
            scale,
            controller.signal,
          ).then((asset) => {
            if (
              asset.source_owner_epoch !== ownerEpoch
              || asset.source_generation !== ownerGeneration
            ) {
              throw new Error('Scene mesh belongs to a superseded owner generation.');
            }
            return asset.mesh_asset_url;
          })
      : api.scene.resolveProgramDraftMesh(
          shape.resource_uri,
          scale,
          draftId,
          draftRevision,
          controller.signal,
        ).then((asset) => {
          if (
            asset.draft_id !== draftId
            || asset.draft_revision !== draftRevision
          ) {
            throw new Error('Scene mesh belongs to a superseded Program Draft.');
          }
          return asset.mesh_asset_url;
        });
    void resolution.then((assetUrl) => {
      setUrl(assetUrl);
    }).catch((error: unknown) => {
      if (controller.signal.aborted) return;
      isolateError(
        error instanceof Error ? error : new Error('Scene mesh resolution failed.'),
      );
    });
    return () => controller.abort();
  }, [
    draftId,
    draftRevision,
    isolateError,
    ownerEpoch,
    ownerGeneration,
    scaleX,
    scaleY,
    scaleZ,
    shape.resource_uri,
  ]);
  return error !== null
    ? (
      <mesh renderOrder={20}>
        <boxGeometry args={[0.06, 0.06, 0.06]} />
        <meshBasicMaterial color="#ef766f" wireframe />
      </mesh>
    )
    : url === null
    ? null
    : (
      <AssetErrorBoundary resetKey={url} onError={isolateError}>
        <Suspense fallback={null}>
          <SceneObjectMeshAsset
            url={url}
            measured={measured}
            selected={selected}
            wireframe={wireframe}
          />
        </Suspense>
      </AssetErrorBoundary>
    );
}

function OverlayVisualMesh({
  url,
  color,
  emissive,
  opacity,
}: {
  url: string;
  color: string;
  emissive: string;
  opacity: number;
}) {
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
        color,
        emissive,
        transparent: true,
        opacity,
        wireframe: true,
        depthWrite: false,
        side: THREE.DoubleSide,
      }));
      mesh.material = Array.isArray(mesh.material) ? materials : materials[0];
      mesh.renderOrder = 14;
    });
    return root;
  }, [color, emissive, gltf.scene, opacity]);
  useEffect(() => () => disposeOwnedMaterials(clone), [clone]);
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

function PrimitiveOverlay({
  primitive,
  color,
  emissive,
  opacity,
}: {
  primitive: SceneUrdfPrimitive;
  color: string;
  emissive: string;
  opacity: number;
}) {
  const cylinderRotation: [number, number, number] | undefined =
    primitive.type === 'cylinder'
    ? [Math.PI / 2, 0, 0]
    : undefined;
  return (
    <mesh rotation={cylinderRotation} renderOrder={14}>
      <PrimitiveGeometry primitive={primitive} />
      <meshStandardMaterial
        color={color}
        emissive={emissive}
        transparent
        opacity={opacity}
        wireframe
        depthWrite={false}
        side={THREE.DoubleSide}
      />
    </mesh>
  );
}

function RobotVisual({
  visual,
  onAssetError,
}: {
  visual: SceneUrdfVisual;
  onAssetError: (error: Error) => void;
}) {
  const origin = visual.visual_origin;
  return (
    <group
      position={origin.position_m}
      quaternion={normalizeQuaternion(origin.orientation_xyzw)}
    >
      {visual.mesh_url !== null && (
        <AssetErrorBoundary resetKey={visual.mesh_url} onError={onAssetError}>
          <Suspense fallback={null}>
            <VisualMesh url={visual.mesh_url} />
          </Suspense>
        </AssetErrorBoundary>
      )}
      {visual.mesh_url === null && visual.primitive !== null && (
        <PrimitiveVisual primitive={visual.primitive} />
      )}
    </group>
  );
}

function RobotCollision({
  collision,
  onAssetError,
}: {
  collision: SceneUrdfCollision;
  onAssetError: (error: Error) => void;
}) {
  const origin = collision.collision_origin;
  return (
    <group
      position={origin.position_m}
      quaternion={normalizeQuaternion(origin.orientation_xyzw)}
    >
      {collision.mesh_url !== null && (
        <AssetErrorBoundary resetKey={collision.mesh_url} onError={onAssetError}>
          <Suspense fallback={null}>
            <CollisionMesh url={collision.mesh_url} />
          </Suspense>
        </AssetErrorBoundary>
      )}
      {collision.mesh_url === null && collision.primitive !== null && (
        <PrimitiveCollision primitive={collision.primitive} />
      )}
    </group>
  );
}

function RetainedNeutralModel({
  manifest,
  collisionsVisible,
  materializationStartedAtMs,
  onMaterialized,
  frameAxesByLink,
  frameAxesSizeM,
  showFrameAxes,
  selectedStructureName,
  onSelect,
  onAssetError,
}: {
  manifest: SceneUrdfManifest;
  collisionsVisible: boolean;
  materializationStartedAtMs: { current: number | null };
  onMaterialized: (durationMs: number) => void;
  frameAxesByLink: ReadonlySet<string>;
  frameAxesSizeM: number;
  showFrameAxes: boolean;
  selectedStructureName: string | null;
  onSelect: (event: ThreeEvent<MouseEvent>, structureName: string) => void;
  onAssetError: (error: Error) => void;
}) {
  const reported = useRef(false);
  // This is one browser-local wall-clock interval from renderer creation to
  // the post-commit effect after Suspense resolves; it is not a backend
  // materialization duration.
  useEffect(() => {
    const startedAtMs = materializationStartedAtMs.current;
    if (reported.current || startedAtMs === null) return;
    reported.current = true;
    onMaterialized(Math.max(0, performance.now() - startedAtMs));
  }, [materializationStartedAtMs, onMaterialized]);
  return (
    <group name="authored-neutral-urdf">
      {manifest.links.map((link) => (
        <group
          key={`${manifest.model_key}:${link.name}`}
          name={`link:${link.name}`}
          position={link.neutral_transform.position_m}
          quaternion={normalizeQuaternion(
            link.neutral_transform.orientation_xyzw,
          )}
          onClick={(event) => onSelect(event, link.name)}
        >
          {link.visuals.map((visual) => (
            <RobotVisual
              key={`visual:${link.name}:${visual.index}`}
              visual={visual}
              onAssetError={onAssetError}
            />
          ))}
          {collisionsVisible && link.collisions.map((collision) => (
            <RobotCollision
              key={`collision:${link.name}:${collision.index}`}
              collision={collision}
              onAssetError={onAssetError}
            />
          ))}
          {(selectedStructureName === link.name
            || (showFrameAxes && frameAxesByLink.has(link.name))) && (
            <axesHelper args={[frameAxesSizeM]} raycast={ignoreViewerRaycast} />
          )}
        </group>
      ))}
    </group>
  );
}

function MeasuredRuntimeModel({
  manifest,
  presentation,
  ownerIdentity,
  latestFrameAppliers,
  frameAxesByLink,
  frameAxesSizeM,
  selectedStructureName,
  onSelect,
  onAssetError,
}: {
  manifest: SceneUrdfManifest;
  presentation: NonNullable<RuntimeScenePresentation['measuredRobot']>;
  ownerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
  latestFrameAppliers: Set<() => void>;
  frameAxesByLink: ReadonlySet<string>;
  frameAxesSizeM: number;
  selectedStructureName: string | null;
  onSelect: (event: ThreeEvent<MouseEvent>, structureName: string) => void;
  onAssetError: (error: Error) => void;
}) {
  return (
    <group name={`owner-measured-robot:${presentation.objectId}`}>
      {manifest.links.map((link) => {
        const transform = presentation.linkTransforms[link.name];
        if (transform === undefined) return null;
        return (
          <AuthoritativeTransformGroup
            key={`measured:${manifest.model_key}:${link.name}`}
            name={`measured:${link.name}`}
            streamIdentity={authoritativeStreamIdentity(
              ownerIdentity,
              'robot',
              manifest.model_key,
              presentation.objectId,
              presentation.sourceId,
              presentation.sourceEpoch,
              link.name,
            )}
            sourceSequence={presentation.sourceSequence}
            observedAtNs={presentation.observedAtNs}
            positionM={transform.position_m}
            orientationXyzw={transform.orientation_xyzw}
            latestFrameAppliers={latestFrameAppliers}
            onClick={(event) => onSelect(event, link.name)}
          >
            <group>
              {link.visuals.map((visual) => {
                const origin = visual.visual_origin;
                return (
                  <group
                    key={`measured-visual:${link.name}:${visual.index}`}
                    position={origin.position_m}
                    quaternion={normalizeQuaternion(origin.orientation_xyzw)}
                  >
                    {visual.mesh_url !== null && (
                      <AssetErrorBoundary resetKey={visual.mesh_url} onError={onAssetError}>
                        <Suspense fallback={null}>
                          <OverlayVisualMesh
                            url={visual.mesh_url}
                            color="#43d7ec"
                            emissive="#0c5060"
                            opacity={0.34}
                          />
                        </Suspense>
                      </AssetErrorBoundary>
                    )}
                    {visual.mesh_url === null && visual.primitive !== null && (
                      <PrimitiveOverlay
                        primitive={visual.primitive}
                        color="#43d7ec"
                        emissive="#0c5060"
                        opacity={0.34}
                      />
                    )}
                  </group>
                );
              })}
              {(selectedStructureName === link.name
                || frameAxesByLink.has(link.name)) && (
                <axesHelper args={[frameAxesSizeM]} raycast={ignoreViewerRaycast} />
              )}
            </group>
          </AuthoritativeTransformGroup>
        );
      })}
    </group>
  );
}

function CandidateRobotGhost({
  manifest,
  presentation,
  stale,
  onAssetError,
}: {
  manifest: SceneUrdfManifest;
  presentation: CandidateRobotPresentation;
  stale: boolean;
  onAssetError: (error: Error) => void;
}) {
  const color = stale ? '#ef766f' : '#b7a2ff';
  const emissive = stale ? '#621d22' : '#38236f';
  return (
    <group name={`candidate-robot:${presentation.receiptDigest}`}>
      {manifest.links.map((link) => {
        const transform = presentation.linkTransforms[link.name];
        if (transform === undefined) return null;
        return (
          <group
            key={`candidate:${manifest.model_key}:${link.name}`}
            position={transform.position_m}
            quaternion={normalizeQuaternion(transform.orientation_xyzw)}
          >
            {link.visuals.map((visual) => {
              const origin = visual.visual_origin;
              return (
                <group
                  key={`candidate-visual:${link.name}:${visual.index}`}
                  position={origin.position_m}
                  quaternion={normalizeQuaternion(origin.orientation_xyzw)}
                >
                  {visual.mesh_url !== null && (
                    <AssetErrorBoundary resetKey={visual.mesh_url} onError={onAssetError}>
                      <Suspense fallback={null}>
                        <OverlayVisualMesh
                          url={visual.mesh_url}
                          color={color}
                          emissive={emissive}
                          opacity={stale ? 0.2 : 0.3}
                        />
                      </Suspense>
                    </AssetErrorBoundary>
                  )}
                  {visual.mesh_url === null && visual.primitive !== null && (
                    <PrimitiveOverlay
                      primitive={visual.primitive}
                      color={color}
                      emissive={emissive}
                      opacity={stale ? 0.2 : 0.3}
                    />
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

type SceneTransformMode = 'translate' | 'rotate' | 'scale';
type SceneTransformSpace = 'local' | 'world';
type SceneTransformIdentity = { ownerEpoch: string; generation: number };
type SceneTransformCommit = (
  sceneResourceId: string,
  pose: LogicalScenePose,
  scale: [number, number, number],
  expectedIdentity: SceneTransformIdentity,
) => Promise<boolean>;

function EditableLogicalSceneObject({
  object,
  selected,
  transformMode,
  transformSpace,
  transformEnabled,
  transformIdentity,
  onCommitTransform,
  onSelect,
  onTransformDraggingChange,
  onContextPointerDown,
  children,
}: {
  object: LogicalSceneObject & { placement: Extract<LogicalSceneObject['placement'], { kind: 'cell_fixed' }> };
  selected: boolean;
  transformMode: SceneTransformMode;
  transformSpace: SceneTransformSpace;
  transformEnabled: boolean;
  transformIdentity: SceneTransformIdentity | undefined;
  onCommitTransform: SceneTransformCommit;
  onSelect: (event: ThreeEvent<MouseEvent>, sceneResourceId: string) => void;
  onTransformDraggingChange: (dragging: boolean) => void;
  onContextPointerDown: (
    event: ThreeEvent<PointerEvent>,
    sceneResourceId: string,
  ) => void;
  children: ReactNode;
}) {
  const group = useRef<THREE.Group | null>(null);
  const [transformTarget, setTransformTarget] = useState<THREE.Group | null>(null);
  const captureGroup = useCallback((node: THREE.Group | null) => {
    group.current = node;
    if (node !== null) setTransformTarget(node);
  }, []);
  const dragging = useRef(false);
  const dragTransformIdentity = useRef(transformIdentity);
  const latestTransformIdentity = useRef(transformIdentity);
  const sourceKey = JSON.stringify(object);
  const latestSourceKey = useRef(sourceKey);
  const pose = object.placement.pose_in_cell;
  const restoreOwnerTransform = useCallback(() => {
    if (group.current === null) return;
    group.current.position.set(pose.position_m.x, pose.position_m.y, pose.position_m.z);
    group.current.quaternion.fromArray(normalizeQuaternion(pose.orientation_xyzw));
    group.current.scale.set(1, 1, 1);
  }, [pose.orientation_xyzw, pose.position_m.x, pose.position_m.y, pose.position_m.z]);
  const latestRestoreOwnerTransform = useRef(restoreOwnerTransform);
  useLayoutEffect(() => {
    latestTransformIdentity.current = transformIdentity;
    latestSourceKey.current = sourceKey;
    latestRestoreOwnerTransform.current = restoreOwnerTransform;
  }, [restoreOwnerTransform, sourceKey, transformIdentity]);
  useLayoutEffect(() => {
    if (!dragging.current) restoreOwnerTransform();
  }, [restoreOwnerTransform, sourceKey]);
  const commit = useCallback(async () => {
    const target = group.current;
    if (target === null) return;
    const expectedTransformIdentity = dragTransformIdentity.current;
    if (
      expectedTransformIdentity === undefined
      || latestTransformIdentity.current === undefined
      || latestTransformIdentity.current.ownerEpoch !== expectedTransformIdentity.ownerEpoch
      || latestTransformIdentity.current.generation !== expectedTransformIdentity.generation
    ) {
      dragging.current = false;
      onTransformDraggingChange(false);
      latestRestoreOwnerTransform.current();
      return;
    }
    const quaternion = target.quaternion.clone().normalize();
    const accepted = await onCommitTransform(
      object.scene_resource_id,
      {
        position_m: { x: target.position.x, y: target.position.y, z: target.position.z },
        orientation_xyzw: [quaternion.x, quaternion.y, quaternion.z, quaternion.w],
      },
      [target.scale.x, target.scale.y, target.scale.z],
      expectedTransformIdentity,
    );
    dragging.current = false;
    onTransformDraggingChange(false);
    if (!accepted || latestSourceKey.current !== sourceKey) {
      latestRestoreOwnerTransform.current();
    }
  }, [object.scene_resource_id, onCommitTransform, onTransformDraggingChange, sourceKey]);
  return (
    <>
      <group
        ref={captureGroup}
        name={`scene:${object.scene_resource_id}`}
        position={[pose.position_m.x, pose.position_m.y, pose.position_m.z]}
        quaternion={normalizeQuaternion(pose.orientation_xyzw)}
        onClick={(event) => onSelect(event, object.scene_resource_id)}
        onPointerDown={(event) => {
          if (event.button !== 2) return;
          event.stopPropagation();
          onContextPointerDown(event, object.scene_resource_id);
        }}
      >
        {children}
      </group>
      {selected && transformEnabled && transformTarget !== null && (
        <TransformControls
          object={transformTarget}
          mode={transformMode}
          space={transformSpace}
          onMouseDown={() => {
            dragging.current = true;
            onTransformDraggingChange(true);
            dragTransformIdentity.current = transformIdentity;
          }}
          onMouseUp={() => { void commit(); }}
        />
      )}
    </>
  );
}

function LogicalSceneObjects({
  objects,
  selectedSceneResourceId,
  onSelect,
  axesSizeM,
  wireframe,
  ownerIdentity,
  draftIdentity,
  transformIdentity,
  objectAxesVisibleIds,
  transformMode,
  transformSpace,
  transformEnabled,
  onCommitTransform,
  onTransformDraggingChange,
  onContextPointerDown,
}: {
  objects: LogicalSceneObject[];
  selectedSceneResourceId: string | null;
  onSelect: (event: ThreeEvent<MouseEvent>, sceneResourceId: string) => void;
  axesSizeM: number;
  wireframe: boolean;
  ownerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
  draftIdentity: StructuralViewerProps['programDraftIdentity'];
  transformIdentity: SceneTransformIdentity | undefined;
  objectAxesVisibleIds: ReadonlySet<string>;
  transformMode: SceneTransformMode;
  transformSpace: SceneTransformSpace;
  transformEnabled: boolean;
  onCommitTransform: SceneTransformCommit;
  onTransformDraggingChange: (dragging: boolean) => void;
  onContextPointerDown: (
    event: ThreeEvent<PointerEvent>,
    sceneResourceId: string,
  ) => void;
}) {
  return (
    <group name="owner-logical-scene">
      {objects.map((object) => {
        if (object.placement.kind !== 'cell_fixed') return null;
        const selected = selectedSceneResourceId === object.scene_resource_id;
        return (
          <EditableLogicalSceneObject
            key={object.scene_resource_id}
            object={object as LogicalSceneObject & { placement: Extract<LogicalSceneObject['placement'], { kind: 'cell_fixed' }> }}
            selected={selected}
            transformMode={transformMode}
            transformSpace={transformSpace}
            transformEnabled={transformEnabled}
            transformIdentity={transformIdentity}
            onCommitTransform={onCommitTransform}
            onSelect={onSelect}
            onTransformDraggingChange={onTransformDraggingChange}
            onContextPointerDown={onContextPointerDown}
          >
            {object.collision_shapes.map((shape, index) => shape.type === 'mesh'
              ? ownerIdentity === undefined && draftIdentity === undefined ? null : (
                  <SceneObjectMesh
                    key={`${object.scene_resource_id}:${index}`}
                    shape={shape}
                    ownerIdentity={ownerIdentity}
                    draftIdentity={draftIdentity}
                    measured={false}
                    selected={selected}
                    wireframe={wireframe}
                  />
                )
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
                      opacity={selected ? 0.9 : 0.82}
                      wireframe={wireframe}
                      depthWrite={!wireframe}
                    />
                  </mesh>
                ))}
            {(selected || objectAxesVisibleIds.has(object.scene_resource_id))
              && <axesHelper args={[axesSizeM]} raycast={ignoreViewerRaycast} />}
          </EditableLogicalSceneObject>
        );
      })}
    </group>
  );
}

function MeasuredLogicalSceneObjects({
  objects,
  presentation,
  wireframe,
  ownerIdentity,
  latestFrameAppliers,
  onSelect,
}: {
  objects: LogicalSceneObject[];
  presentation: RuntimeScenePresentation;
  wireframe: boolean;
  ownerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
  latestFrameAppliers: Set<() => void>;
  onSelect: (event: ThreeEvent<MouseEvent>, sceneResourceId: string) => void;
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
          <AuthoritativeTransformGroup
            key={`measured-scene:${measurement.objectId}`}
            name={`measured-scene:${measurement.objectId}`}
            streamIdentity={authoritativeStreamIdentity(
              ownerIdentity,
              'scene-object',
              measurement.objectId,
              measurement.policyId,
              measurement.sourceId,
              measurement.sourceEpoch,
            )}
            sourceSequence={measurement.sourceSequence}
            observedAtNs={measurement.observedAtNs}
            positionM={measurement.pose.position_m}
            orientationXyzw={measurement.pose.orientation_xyzw}
            latestFrameAppliers={latestFrameAppliers}
            onClick={(event) => onSelect(event, measurement.objectId)}
          >
            <group>
              {object.collision_shapes.map((shape, index) => shape.type === 'mesh'
                ? ownerIdentity === undefined ? null : (
                    <SceneObjectMesh
                      key={`measured-scene:${measurement.objectId}:${index}`}
                      shape={shape}
                      ownerIdentity={ownerIdentity}
                      measured
                      selected={false}
                      wireframe={wireframe}
                    />
                  )
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
                        opacity={0.55}
                        wireframe={wireframe}
                        depthWrite={!wireframe}
                      />
                    </mesh>
                  ))}
            </group>
          </AuthoritativeTransformGroup>
        );
      })}
    </group>
  );
}

function CandidateTcpGhost({
  pose,
  stale,
  axesSizeM,
  editable,
  transformMode,
  transformSpace,
  onEdit,
  onTransformDraggingChange,
}: {
  pose: Pose3Value;
  stale: boolean;
  axesSizeM: number;
  editable: boolean;
  transformMode: Exclude<SceneTransformMode, 'scale'>;
  transformSpace: SceneTransformSpace;
  onEdit: (pose: Pose3Value) => void;
  onTransformDraggingChange: (dragging: boolean) => void;
}) {
  const group = useRef<THREE.Group | null>(null);
  const [target, setTarget] = useState<THREE.Group | null>(null);
  const dragging = useRef(false);
  const capture = useCallback((node: THREE.Group | null) => {
    group.current = node;
    if (node !== null) setTarget(node);
  }, []);
  const restore = useCallback(() => {
    if (group.current === null) return;
    group.current.position.set(
      pose.position_m.x_m,
      pose.position_m.y_m,
      pose.position_m.z_m,
    );
    group.current.quaternion.fromArray(normalizeQuaternion([
      pose.orientation.x,
      pose.orientation.y,
      pose.orientation.z,
      pose.orientation.w,
    ]));
  }, [pose]);
  useLayoutEffect(() => {
    if (!dragging.current) restore();
  }, [restore]);
  const commit = useCallback(() => {
    const current = group.current;
    if (current === null) return;
    const orientation = current.quaternion.clone().normalize();
    dragging.current = false;
    onEdit({
      position_m: {
        x_m: current.position.x,
        y_m: current.position.y,
        z_m: current.position.z,
      },
      orientation: {
        x: orientation.x,
        y: orientation.y,
        z: orientation.z,
        w: orientation.w,
      },
    });
    onTransformDraggingChange(false);
  }, [onEdit, onTransformDraggingChange]);
  return (
    <>
      <group
        ref={capture}
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
        <axesHelper args={[axesSizeM]} raycast={ignoreViewerRaycast} />
      </group>
      {editable && target !== null && (
        <TransformControls
          object={target}
          mode={transformMode}
          space={transformSpace}
          onMouseDown={() => {
            dragging.current = true;
            onTransformDraggingChange(true);
          }}
          onMouseUp={commit}
        />
      )}
    </>
  );
}

function BoundSnapshotResponder({
  frameIdentity,
  readFrameIdentity,
  applyLatestFrame,
}: {
  frameIdentity: NonNullable<StructuralViewerProps['viewerFrameIdentity']>;
  readFrameIdentity: NonNullable<StructuralViewerProps['readViewerFrameIdentity']>;
  applyLatestFrame: () => void;
}) {
  const current = useRef(frameIdentity);
  useEffect(() => {
    current.current = frameIdentity;
  }, [frameIdentity]);
  const ensureFrame = useCallback(async (
    required: { ownerEpoch: string; generation: number },
    timeoutMs: number,
  ): Promise<{ ownerEpoch: string; generation: number } | null> => {
    const deadline = performance.now() + timeoutMs;
    while (performance.now() <= deadline) {
      const observed = readFrameIdentity() ?? current.current;
      if (observed.ownerEpoch !== required.ownerEpoch) return null;
      if (observed.generation >= required.generation) return observed;
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    return null;
  }, [readFrameIdentity]);
  return (
    <SnapshotResponder
      subscribe={subscribeSnapshotObserver}
      upload={uploadSnapshotObserverResult}
      ensureFrame={ensureFrame}
      applyFrame={applyLatestFrame}
    />
  );
}

function MeasuredTcpMarker({
  presentation,
  axesSizeM,
  ownerIdentity,
  latestFrameAppliers,
}: {
  presentation: NonNullable<RuntimeScenePresentation['measuredTcp']>;
  axesSizeM: number;
  ownerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
  latestFrameAppliers: Set<() => void>;
}) {
  return (
    <AuthoritativeTransformGroup
      name={`owner-measured-tcp:${presentation.objectId}`}
      streamIdentity={authoritativeStreamIdentity(
        ownerIdentity,
        'tcp',
        presentation.objectId,
        presentation.policyId,
        presentation.sourceId,
        presentation.sourceEpoch,
      )}
      sourceSequence={presentation.sourceSequence}
      observedAtNs={presentation.observedAtNs}
      positionM={presentation.pose.position_m}
      orientationXyzw={presentation.pose.orientation_xyzw}
      latestFrameAppliers={latestFrameAppliers}
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
      <axesHelper args={[axesSizeM]} raycast={ignoreViewerRaycast} />
    </AuthoritativeTransformGroup>
  );
}

function PointCloudPoints({ frame }: { frame: PointCloudFrame }) {
  const geometry = useMemo(() => new THREE.BufferGeometry(), []);
  const capacity = useRef(0);
  const colorCapacity = useRef(0);
  useLayoutEffect(() => {
    const pointCount = Math.floor(frame.positions.length / 3);
    if (capacity.current === 0 || pointCount > capacity.current) {
      capacity.current = Math.max(256, 2 ** Math.ceil(Math.log2(pointCount)));
      geometry.setAttribute(
        'position',
        new THREE.BufferAttribute(new Float32Array(capacity.current * 3), 3),
      );
    }
    const positions = geometry.getAttribute('position') as THREE.BufferAttribute;
    (positions.array as Float32Array).set(frame.positions, 0);
    positions.needsUpdate = true;
    if (frame.colors !== null) {
      if (pointCount > colorCapacity.current) {
        colorCapacity.current = capacity.current;
        geometry.setAttribute(
          'color',
          new THREE.Uint8BufferAttribute(new Uint8Array(colorCapacity.current * 3), 3, true),
        );
      }
      const colors = geometry.getAttribute('color') as THREE.BufferAttribute;
      (colors.array as Uint8Array).set(frame.colors, 0);
      colors.needsUpdate = true;
    } else if (geometry.hasAttribute('color')) {
      geometry.deleteAttribute('color');
      colorCapacity.current = 0;
    }
    geometry.setDrawRange(0, pointCount);
    geometry.computeBoundingSphere();
  }, [frame, geometry]);
  useEffect(() => () => geometry.dispose(), [geometry]);
  return (
    <points geometry={geometry} frustumCulled renderOrder={6}>
      <pointsMaterial
        color={frame.colors === null ? '#75d7ff' : '#ffffff'}
        vertexColors={frame.colors !== null}
        size={0.006}
        sizeAttenuation
        transparent
        opacity={0.9}
        depthWrite={false}
      />
    </points>
  );
}

function ViewerPerformanceProbe() {
  useFrame(({ gl }) => {
    const nowMs = performance.now();
    recordViewerRenderFrame(nowMs);
    recordViewerRenderWork(
      gl.info.render.calls,
      gl.info.render.triangles,
      gl.domElement.width * gl.domElement.height,
    );
  });
  return null;
}

function ViewerQualificationProfiler({
  enabled,
  onRender,
  children,
}: {
  enabled: boolean;
  onRender: (
    id: string,
    phase: 'mount' | 'update' | 'nested-update',
    actualDuration: number,
  ) => void;
  children: ReactNode;
}) {
  if (!enabled) return <>{children}</>;
  return (
    <Profiler id="manyforge-structural-viewer" onRender={onRender}>
      <ViewerPerformanceProbe />
      {children}
    </Profiler>
  );
}

interface RetainedCameraPresentation {
  camera: ViewerCamera;
}

interface RetainedCameraPresentationStore {
  read: () => RetainedCameraPresentation;
  write: (presentation: RetainedCameraPresentation) => void;
  capture: (position: THREE.Vector3, target: THREE.Vector3 | undefined) => void;
}

function createRetainedCameraPresentationStore(
  initial: RetainedCameraPresentation,
): RetainedCameraPresentationStore {
  const retained: RetainedCameraPresentation = {
    camera: {
      position: [...initial.camera.position],
      target: [...initial.camera.target],
    },
  };
  return {
    read: () => retained,
    write: (presentation) => {
      retained.camera.position[0] = presentation.camera.position[0];
      retained.camera.position[1] = presentation.camera.position[1];
      retained.camera.position[2] = presentation.camera.position[2];
      retained.camera.target[0] = presentation.camera.target[0];
      retained.camera.target[1] = presentation.camera.target[1];
      retained.camera.target[2] = presentation.camera.target[2];
    },
    capture: (position, target) => {
      retained.camera.position[0] = position.x;
      retained.camera.position[1] = position.y;
      retained.camera.position[2] = position.z;
      if (target !== undefined) {
        retained.camera.target[0] = target.x;
        retained.camera.target[1] = target.y;
        retained.camera.target[2] = target.z;
      }
    },
  };
}

interface CameraCommandRequest {
  id: number;
  kind: 'frame_all' | 'frame_selection' | 'reset';
  target: {
    kind: 'scene_resource' | 'structure';
    id: string;
  } | null;
}

export function framedCameraPresentation(
  box: THREE.Box3,
  current: ViewerCamera,
  verticalFovDegrees: number,
  aspect: number,
  margin = 1.25,
): RetainedCameraPresentation {
  if (box.isEmpty()) return { camera: current };
  const size = box.getSize(new THREE.Vector3());
  const center = box.getCenter(new THREE.Vector3());
  const maximumSize = Math.max(size.x, size.y, size.z, 0.01);
  const verticalDistance = maximumSize
    / (2 * Math.tan(THREE.MathUtils.degToRad(verticalFovDegrees) / 2));
  const distance = margin * Math.max(
    verticalDistance,
    verticalDistance / Math.max(aspect, Number.EPSILON),
  );
  const direction = new THREE.Vector3(...current.position)
    .sub(new THREE.Vector3(...current.target));
  if (direction.lengthSq() < Number.EPSILON) {
    direction
      .fromArray(DEFAULT_VIEWER_CAMERA.position)
      .sub(new THREE.Vector3(...DEFAULT_VIEWER_CAMERA.target));
  }
  direction.normalize();
  const position = center.clone().addScaledVector(direction, distance);
  return {
    camera: {
      position: [position.x, position.y, position.z],
      target: [center.x, center.y, center.z],
    },
  };
}

export function frameableContentBounds(root: THREE.Object3D): THREE.Box3 {
  const bounds = new THREE.Box3();
  const objectBounds = new THREE.Box3();
  root.updateWorldMatrix(true, true);

  const visit = (object: THREE.Object3D): void => {
    if (
      !object.visible
      || (object as THREE.Object3D & { isTransformControls?: boolean })
        .isTransformControls === true
    ) return;
    const bounded = object as THREE.Object3D & {
      boundingBox?: THREE.Box3 | null;
      computeBoundingBox?: () => void;
      geometry?: THREE.BufferGeometry;
    };
    let localBounds: THREE.Box3 | null | undefined;
    if (bounded.boundingBox !== undefined) {
      if (bounded.boundingBox === null) bounded.computeBoundingBox?.();
      localBounds = bounded.boundingBox;
    } else if (bounded.geometry !== undefined) {
      if (bounded.geometry.boundingBox === null) {
        bounded.geometry.computeBoundingBox();
      }
      localBounds = bounded.geometry.boundingBox;
    }
    if (localBounds !== null && localBounds !== undefined && !localBounds.isEmpty()) {
      objectBounds.copy(localBounds).applyMatrix4(object.matrixWorld);
      bounds.union(objectBounds);
    }
    for (const child of object.children) visit(child);
  };

  visit(root);
  return bounds;
}

function CameraCommand({
  request,
  defaultPresentation,
  onPresentation,
}: {
  request: CameraCommandRequest;
  defaultPresentation: RetainedCameraPresentation;
  onPresentation: (presentation: RetainedCameraPresentation) => void;
}) {
  const bounds = useBounds();
  const { camera, controls, scene, invalidate } = useThree();
  const processed = useRef(0);
  useEffect(() => {
    if (request.id === 0 || processed.current === request.id) return;
    processed.current = request.id;
    const frameContent = scene.getObjectByName('viewer-frame-content');
    if (request.kind === 'reset') {
      camera.position.fromArray(defaultPresentation.camera.position);
      const orbit = controls as { target?: THREE.Vector3; update?: () => void } | null;
      orbit?.target?.fromArray(defaultPresentation.camera.target);
      if (frameContent !== undefined) {
        bounds.refresh(frameableContentBounds(frameContent)).clip();
      }
      orbit?.update?.();
      onPresentation(defaultPresentation);
      invalidate();
      return;
    }
    let framedObject: THREE.Object3D | undefined;
    if (request.kind === 'frame_selection' && request.target !== null) {
      framedObject = request.target.kind === 'scene_resource'
        ? scene.getObjectByName(`scene:${request.target.id}`)
        : scene.getObjectByName(`measured:${request.target.id}`)
          ?? scene.getObjectByName(`link:${request.target.id}`);
      if (framedObject === undefined) return;
    }
    const framedBounds = framedObject === undefined
      ? frameContent === undefined ? null : frameableContentBounds(frameContent)
      : new THREE.Box3().setFromObject(framedObject);
    if (framedBounds === null || framedBounds.isEmpty()) return;
    bounds.refresh(framedBounds);
    const orbit = controls as { target?: THREE.Vector3; update?: () => void } | null;
    const currentTarget = orbit?.target ?? new THREE.Vector3(...defaultPresentation.camera.target);
    const perspective = camera as THREE.PerspectiveCamera;
    const presentation = framedCameraPresentation(
      bounds.getSize().box,
      {
        position: [camera.position.x, camera.position.y, camera.position.z],
        target: [currentTarget.x, currentTarget.y, currentTarget.z],
      },
      perspective.fov,
      perspective.aspect,
    );
    camera.position.fromArray(presentation.camera.position);
    orbit?.target?.fromArray(presentation.camera.target);
    bounds.clip();
    orbit?.update?.();
    onPresentation(presentation);
    invalidate();
  }, [
    bounds,
    camera,
    controls,
    defaultPresentation,
    invalidate,
    onPresentation,
    request,
    scene,
  ]);
  return null;
}

function CaptureCameraPresentation({
  store,
}: {
  store: RetainedCameraPresentationStore;
}) {
  const camera = useThree((state) => state.camera);
  const controls = useThree((state) => state.controls) as {
    target?: THREE.Vector3;
    addEventListener?: (kind: 'change', listener: () => void) => void;
    removeEventListener?: (kind: 'change', listener: () => void) => void;
  } | null;
  useEffect(() => {
    const capture = () => store.capture(camera.position, controls?.target);
    controls?.addEventListener?.('change', capture);
    capture();
    return () => {
      controls?.removeEventListener?.('change', capture);
      capture();
    };
  }, [camera, controls, store]);
  return null;
}

export const VIEWER_CLICK_MAX_DURATION_MS = 250;
export const VIEWER_CONTEXT_DRAG_THRESHOLD_PX = 3;

export function viewerClickWithinDuration({
  startedAtMs,
  releasedAtMs,
}: {
  startedAtMs: number;
  releasedAtMs: number;
}): boolean {
  const durationMs = releasedAtMs - startedAtMs;
  return durationMs >= 0
    && durationMs <= VIEWER_CLICK_MAX_DURATION_MS;
}

export function viewerContextWasDragged({
  startX,
  startY,
  currentX,
  currentY,
}: {
  startX: number;
  startY: number;
  currentX: number;
  currentY: number;
}): boolean {
  return Math.hypot(currentX - startX, currentY - startY)
    > VIEWER_CONTEXT_DRAG_THRESHOLD_PX;
}

function ViewerCanvas({
  manifest,
  collisionsVisible,
  wireframeVisible,
  onAssetError,
  onIsolatedAssetError,
  sceneObjects,
  selectedSceneResourceId,
  onSelectSceneResource,
  selectedStructureName,
  onSelectStructure,
  candidateTcpPose,
  candidateStale,
  candidateTcpEditable,
  onEditCandidateTcpPose,
  candidateRobot,
  runtimePresentation,
  sceneOwnerIdentity,
  programDraftIdentity,
  viewerFrameIdentity,
  readViewerFrameIdentity,
  pointCloudFrame,
  viewer,
  frameAxes,
  objectAxesVisibleIds,
  cameraPresentation,
  onModelMaterialized,
  onViewerCommit,
  transformMode,
  transformSpace,
  transformEnabled,
  objectEditorActive,
  robotEditorActive,
  transformIdentity,
  onCommitSceneTransform,
  onSceneContext,
  cameraCommand,
}: {
  manifest: SceneUrdfManifest | null;
  collisionsVisible: boolean;
  wireframeVisible: boolean;
  onAssetError: (error: Error) => void;
  onIsolatedAssetError: (error: Error) => void;
  sceneObjects: LogicalSceneObject[];
  selectedSceneResourceId: string | null;
  onSelectSceneResource: (sceneResourceId: string | null) => void;
  selectedStructureName: string | null;
  onSelectStructure: (structureName: string | null) => void;
  candidateTcpPose: Pose3Value | null;
  candidateStale: boolean;
  candidateTcpEditable: boolean;
  onEditCandidateTcpPose: (pose: Pose3Value) => void;
  candidateRobot: CandidateRobotPresentation | null;
  runtimePresentation: RuntimeScenePresentation | undefined;
  sceneOwnerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
  programDraftIdentity: StructuralViewerProps['programDraftIdentity'];
  viewerFrameIdentity: StructuralViewerProps['viewerFrameIdentity'];
  readViewerFrameIdentity: StructuralViewerProps['readViewerFrameIdentity'];
  pointCloudFrame: PointCloudFrame | null;
  viewer: ProgramViewerMetadata | undefined;
  frameAxes: NonNullable<StructuralViewerProps['frameAxes']>;
  objectAxesVisibleIds: ReadonlySet<string>;
  cameraPresentation: RetainedCameraPresentationStore;
  onModelMaterialized: (durationMs: number) => void;
  onViewerCommit: (
    id: string,
    phase: 'mount' | 'update' | 'nested-update',
    actualDuration: number,
  ) => void;
  transformMode: SceneTransformMode;
  transformSpace: SceneTransformSpace;
  transformEnabled: boolean;
  objectEditorActive: boolean;
  robotEditorActive: boolean;
  transformIdentity: SceneTransformIdentity | undefined;
  onCommitSceneTransform: SceneTransformCommit;
  onSceneContext: (sceneResourceId: string, x: number, y: number) => void;
  cameraCommand: CameraCommandRequest;
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
  const retainedCameraPresentation = cameraPresentation.read();
  const selectedCamera = retainedCameraPresentation.camera;
  const captureCameraPresentation = useCallback((
    presentation: RetainedCameraPresentation,
  ) => {
    cameraPresentation.write(presentation);
  }, [cameraPresentation]);
  const qualificationEnabled = viewerQualificationOptedIn();
  const materializationStartedAtMs = useRef<number | null>(null);
  const latestFrameAppliers = useMemo(() => new Set<() => void>(), []);
  const frameAxesByLink = useMemo(
    () => new Set(frameAxes.map((frame) => frame.linkName)),
    [frameAxes],
  );
  const applyLatestFrame = useCallback(() => {
    for (const apply of latestFrameAppliers) apply();
  }, [latestFrameAppliers]);
  const readPresentedFrame = useCallback(
    () => readViewerFrameIdentity?.() ?? viewerFrameIdentity ?? null,
    [readViewerFrameIdentity, viewerFrameIdentity],
  );
  const pointerStartedAtMs = useRef<number | null>(null);
  const contextPointer = useRef<{
    pointerId: number;
    sceneResourceId: string | null;
    startX: number;
    startY: number;
    dragged: boolean;
  } | null>(null);
  const transformDragging = useRef(false);
  const capturePointerDown = useCallback((event: ReactPointerEvent<HTMLDivElement>) => {
    pointerStartedAtMs.current = event.isPrimary && event.button === 0
      ? event.timeStamp
      : null;
    if (event.isPrimary && event.button === 2) {
      contextPointer.current = {
        pointerId: event.pointerId,
        sceneResourceId: null,
        startX: event.clientX,
        startY: event.clientY,
        dragged: false,
      };
    }
  }, []);
  const clearPointerIntent = useCallback(() => {
    pointerStartedAtMs.current = null;
    contextPointer.current = null;
  }, []);
  const trackContextPointer = useCallback((event: ReactPointerEvent<HTMLDivElement>) => {
    const current = contextPointer.current;
    if (current === null || current.pointerId !== event.pointerId || current.dragged) return;
    if (viewerContextWasDragged({
      startX: current.startX,
      startY: current.startY,
      currentX: event.clientX,
      currentY: event.clientY,
    })) current.dragged = true;
  }, []);
  const releaseContextPointer = useCallback((event: ReactPointerEvent<HTMLDivElement>) => {
    const current = contextPointer.current;
    if (current === null || current.pointerId !== event.pointerId || event.button !== 2) return;
    contextPointer.current = null;
    if (!current.dragged && current.sceneResourceId !== null) {
      onSceneContext(current.sceneResourceId, event.clientX, event.clientY);
    }
  }, [onSceneContext]);
  const registerContextPointer = useCallback((
    event: ThreeEvent<PointerEvent>,
    sceneResourceId: string,
  ) => {
    const current = contextPointer.current;
    if (current === null || current.pointerId !== event.nativeEvent.pointerId) return;
    current.sceneResourceId = sceneResourceId;
  }, []);
  const consumeShortClick = useCallback((releasedAtMs: number) => {
    const startedAtMs = pointerStartedAtMs.current;
    pointerStartedAtMs.current = null;
    return startedAtMs !== null && viewerClickWithinDuration({
      startedAtMs,
      releasedAtMs,
    });
  }, []);
  const clearSelection = useCallback(() => {
    onSelectSceneResource(null);
    onSelectStructure(null);
  }, [onSelectSceneResource, onSelectStructure]);
  const clearBeforeSemanticClick = useCallback((
    event: ReactMouseEvent<HTMLDivElement>,
  ) => {
    const startedAtMs = pointerStartedAtMs.current;
    if (
      transformDragging.current
      || startedAtMs === null
      || !viewerClickWithinDuration({
        startedAtMs,
        releasedAtMs: event.timeStamp,
      })
    ) return;
    clearSelection();
  }, [clearSelection]);
  const selectSceneResource = useCallback((
    event: ThreeEvent<MouseEvent>,
    sceneResourceId: string,
  ) => {
    event.stopPropagation();
    if (
      transformDragging.current
      || !consumeShortClick(event.nativeEvent.timeStamp)
    ) return;
    onSelectStructure(null);
    onSelectSceneResource(sceneResourceId);
  }, [consumeShortClick, onSelectSceneResource, onSelectStructure]);
  const selectStructure = useCallback((
    event: ThreeEvent<MouseEvent>,
    structureName: string,
  ) => {
    event.stopPropagation();
    if (
      transformDragging.current
      || !consumeShortClick(event.nativeEvent.timeStamp)
    ) return;
    onSelectSceneResource(null);
    onSelectStructure(structureName);
  }, [consumeShortClick, onSelectSceneResource, onSelectStructure]);
  const setTransformDragging = useCallback((dragging: boolean) => {
    transformDragging.current = dragging;
  }, []);

  useEffect(() => {
    setViewerQualificationCanvasMounted(true);
    return () => {
      setViewerQualificationCanvasMounted(false);
      setViewerQualificationRenderer(null);
    };
  }, []);

  return (
    <div
      className="viewer-canvas-input-surface"
      onPointerDownCapture={capturePointerDown}
      onPointerMoveCapture={trackContextPointer}
      onPointerUpCapture={releaseContextPointer}
      onPointerCancelCapture={clearPointerIntent}
      onClickCapture={clearBeforeSemanticClick}
      onContextMenu={(event) => event.preventDefault()}
    >
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
        if (qualificationEnabled && materializationStartedAtMs.current === null) {
          materializationStartedAtMs.current = performance.now();
        }
        const inspection = inspectViewerRenderer(gl);
        const presentation = selectViewerPresentationProfile(
          inspection.software,
          devicePixelRatio,
        );
        setViewerQualificationRenderer({ ...inspection, ...presentation });
        setRenderer((current) => {
          const next = { ...inspection, ...presentation };
          return JSON.stringify(current) === JSON.stringify(next) ? current : next;
        });
      }}
      style={{ background: '#081019' }}
    >
      <ViewerQualificationProfiler
        enabled={qualificationEnabled}
        onRender={onViewerCommit}
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
      <CaptureCameraPresentation store={cameraPresentation} />
      <Bounds clip margin={1.25}>
        <CameraCommand
          request={cameraCommand}
          defaultPresentation={{
            camera: {
              position: [...DEFAULT_VIEWER_CAMERA.position],
              target: [...DEFAULT_VIEWER_CAMERA.target],
            },
          }}
          onPresentation={captureCameraPresentation}
        />
        {manifest !== null && (
          <AssetErrorBoundary
            resetKey={manifest.model_key}
            onError={onAssetError}
          >
            <Suspense fallback={null}>
              <RetainedNeutralModel
                manifest={manifest}
                collisionsVisible={collisionsVisible}
                materializationStartedAtMs={materializationStartedAtMs}
                onMaterialized={onModelMaterialized}
                frameAxesByLink={frameAxesByLink}
                frameAxesSizeM={viewer?.frame_gizmo_size_m ?? 0.1}
                showFrameAxes={runtimePresentation?.measuredRobot === null
                  || runtimePresentation?.measuredRobot === undefined}
                selectedStructureName={selectedStructureName}
                onSelect={selectStructure}
                onAssetError={onIsolatedAssetError}
              />
              {runtimePresentation?.measuredRobot !== null
                && runtimePresentation?.measuredRobot !== undefined && (
                <MeasuredRuntimeModel
                  manifest={manifest}
                  presentation={runtimePresentation.measuredRobot}
                  ownerIdentity={sceneOwnerIdentity}
                  latestFrameAppliers={latestFrameAppliers}
                  frameAxesByLink={frameAxesByLink}
                  frameAxesSizeM={viewer?.frame_gizmo_size_m ?? 0.1}
                  selectedStructureName={selectedStructureName}
                  onSelect={selectStructure}
                  onAssetError={onIsolatedAssetError}
                />
              )}
            </Suspense>
          </AssetErrorBoundary>
        )}
        <LogicalSceneObjects
          objects={sceneObjects}
          selectedSceneResourceId={selectedSceneResourceId}
          onSelect={selectSceneResource}
          axesSizeM={viewer?.object_axes_gizmo_size_m ?? 0.1}
          wireframe={wireframeVisible}
          ownerIdentity={sceneOwnerIdentity}
          draftIdentity={programDraftIdentity}
          transformIdentity={transformIdentity}
          objectAxesVisibleIds={objectAxesVisibleIds}
          transformMode={transformMode}
          transformSpace={transformSpace}
          transformEnabled={transformEnabled && objectEditorActive}
          onCommitTransform={onCommitSceneTransform}
          onTransformDraggingChange={setTransformDragging}
          onContextPointerDown={registerContextPointer}
        />
        {runtimePresentation !== undefined && (
          <MeasuredLogicalSceneObjects
            objects={sceneObjects}
            presentation={runtimePresentation}
            wireframe={wireframeVisible}
            ownerIdentity={sceneOwnerIdentity}
            latestFrameAppliers={latestFrameAppliers}
            onSelect={selectSceneResource}
          />
        )}
        {runtimePresentation?.measuredTcp !== null
          && runtimePresentation?.measuredTcp !== undefined && (
          <MeasuredTcpMarker
            presentation={runtimePresentation.measuredTcp}
            axesSizeM={viewer?.frame_gizmo_size_m ?? 0.1}
            ownerIdentity={sceneOwnerIdentity}
            latestFrameAppliers={latestFrameAppliers}
          />
        )}
        {candidateTcpPose !== null && (
          <CandidateTcpGhost
            pose={candidateTcpPose}
            stale={candidateStale}
            axesSizeM={viewer?.frame_gizmo_size_m ?? 0.1}
            editable={candidateTcpEditable && robotEditorActive
              && transformMode !== 'scale'}
            transformMode={transformMode === 'rotate' ? 'rotate' : 'translate'}
            transformSpace={transformSpace}
            onEdit={onEditCandidateTcpPose}
            onTransformDraggingChange={setTransformDragging}
          />
        )}
        {manifest !== null && candidateRobot !== null && (
          <CandidateRobotGhost
            manifest={manifest}
            presentation={candidateRobot}
            stale={candidateStale}
            onAssetError={onIsolatedAssetError}
          />
        )}
        {pointCloudFrame !== null && <PointCloudPoints frame={pointCloudFrame} />}
      </Bounds>
      {viewerFrameIdentity !== undefined && (
        <BoundSnapshotResponder
          frameIdentity={viewerFrameIdentity}
          readFrameIdentity={readPresentedFrame}
          applyLatestFrame={applyLatestFrame}
        />
      )}
      </ViewerQualificationProfiler>
    </Canvas>
    </div>
  );
}

function StructuralViewerContent({
  viewerActive,
  manifest,
  onRetry,
  sceneObjects = [],
  programDraftPreview = false,
  programDraftIdentity,
  selectedSceneResourceId = null,
  onSelectSceneResource = () => undefined,
  selectedStructureName = null,
  onSelectStructure = () => undefined,
  onCommitSceneTransform,
  onCommitProgramDraftTransform,
  onDuplicateSceneResource,
  onRemoveSceneResource,
  onDetachSceneResource,
  onEditSceneResource,
  selectionLocked = false,
  sceneMutationAvailability,
  candidateTcpPose = null,
  candidateStale = false,
  candidateTcpEditable = false,
  onEditCandidateTcpPose = () => undefined,
  candidateRobot = null,
  runtimePresentation,
  sceneOwnerIdentity,
  viewerFrameIdentity,
  readViewerFrameIdentity,
  pointCloudCellId,
  viewer,
  frameAxes = [],
  objectAxesVisibleIds = [],
  robotControlSurface,
  robotControlBlocked = false,
  objectControlSurface,
  transportControls,
  inspectorTarget = null,
  inspectorMinimized = false,
  onInspectorMinimizedChange,
  onInspectorMaximize,
  onInspectorPanelChange,
}: StructuralViewerProps) {
  const [collisionsVisible, setCollisionsVisible] = useState(false);
  const [wireframeVisible, setWireframeVisible] = useState(
    DEFAULT_WIREFRAME_VISIBLE,
  );
  const [pointCloudVisible, setPointCloudVisible] = useState(false);
  const [measuredStateVisible, setMeasuredStateVisible] = useState(true);
  const [assetError, setAssetError] = useState<Error | null>(null);
  const [assetWarning, setAssetWarning] = useState<string | null>(null);
  const [transformMode, setTransformMode] = useState<SceneTransformMode>('translate');
  const [transformSpace, setTransformSpace] = useState<SceneTransformSpace>('world');
  const [cameraCommand, setCameraCommand] = useState<CameraCommandRequest>({
    id: 0,
    kind: 'frame_all',
    target: null,
  });
  const [contextMenu, setContextMenu] = useState<{
    sceneResourceId: string;
    x: number;
    y: number;
  } | null>(null);
  const [editLockedObjectIds, setEditLockedObjectIds] = useState<Set<string>>(
    () => new Set(),
  );
  const [inspectorPanel, setInspectorPanel] = useState<ViewerInspectorPanel>(null);
  const contextMenuRef = useRef<HTMLDivElement | null>(null);
  const contextAvailability = contextMenu === null
    ? null
    : sceneMutationAvailability?.[contextMenu.sceneResourceId] ?? null;
  const selectedObject = sceneObjects.find(
    (object) => object.scene_resource_id === selectedSceneResourceId,
  ) ?? null;
  const selectedObjectAvailability = selectedObject === null
    ? null
    : sceneMutationAvailability?.[selectedObject.scene_resource_id] ?? null;
  const transformCommit = programDraftPreview
    ? onCommitProgramDraftTransform
    : onCommitSceneTransform;
  const transformIdentity = programDraftPreview
    ? programDraftIdentity === undefined ? undefined : {
        ownerEpoch: programDraftIdentity.draftId,
        generation: programDraftIdentity.draftRevision,
      }
    : sceneOwnerIdentity;
  const transformBaseEnabled = transformCommit !== undefined
    && transformIdentity !== undefined
    && (selectedSceneResourceId === null || !editLockedObjectIds.has(selectedSceneResourceId))
    && selectedObject?.placement.kind === 'cell_fixed'
    && selectedObject.provider_evidence === null
    && selectedObject.collision_shapes.length > 0
    && sceneMutationAvailability?.[selectedObject.scene_resource_id]?.transform === true;
  const candidateTransformEnabled = candidateTcpEditable
    && candidateTcpPose !== null
    && selectedObject === null;
  const generalTransformEnabled = transformBaseEnabled || candidateTransformEnabled;
  const scaleEnabled = transformBaseEnabled
    && supportsIndependentSceneScale(selectedObject);
  const transformEnabled = transformBaseEnabled
    && (transformMode !== 'scale' || scaleEnabled);
  const selectedCameraTarget = useMemo(() => (
    selectedSceneResourceId !== null
      ? { kind: 'scene_resource' as const, id: selectedSceneResourceId }
      : selectedStructureName !== null
        ? { kind: 'structure' as const, id: selectedStructureName }
        : null
  ), [selectedSceneResourceId, selectedStructureName]);
  const issueCameraCommand = useCallback((
    kind: CameraCommandRequest['kind'],
    target = selectedCameraTarget,
  ) => {
    setCameraCommand((current) => ({
      id: current.id + 1,
      kind,
      target,
    }));
  }, [selectedCameraTarget, setCameraCommand]);
  const pointCloudEnabled = viewerActive
    && pointCloudVisible
    && pointCloudCellId !== undefined;
  const pointCloud = usePointCloudFrame(pointCloudEnabled, pointCloudCellId);
  const objectAxesVisible = useMemo(
    () => new Set(objectAxesVisibleIds),
    [objectAxesVisibleIds],
  );
  const presentedRuntime = measuredStateVisible ? runtimePresentation : undefined;
  const camera = useMemo(
    () => parseViewerCameraQuery(
      typeof window === 'undefined' ? '' : window.location.search,
    ),
    [],
  );
  const [cameraPresentation] = useState(() => createRetainedCameraPresentationStore({
    camera: camera ?? {
      position: [...DEFAULT_VIEWER_CAMERA.position],
      target: [...DEFAULT_VIEWER_CAMERA.target],
    },
  }));

  useEffect(() => {
    onInspectorPanelChange?.(inspectorPanel);
  }, [inspectorPanel, onInspectorPanelChange]);
  useEffect(() => () => onInspectorPanelChange?.(null), [onInspectorPanelChange]);
  const toggleInspectorPanel = useCallback((panel: Exclude<ViewerInspectorPanel, null>) => {
    if (inspectorPanel === panel) {
      if (inspectorMinimized) onInspectorMinimizedChange?.(false);
      else setInspectorPanel(null);
      return;
    }
    setInspectorPanel(panel);
    onInspectorMinimizedChange?.(false);
  }, [inspectorMinimized, inspectorPanel, onInspectorMinimizedChange]);

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
      (total, object) => total + object.collision_shapes.filter((shape) => (
        shape.type !== 'mesh'
        || sceneOwnerIdentity !== undefined
        || programDraftIdentity !== undefined
      )).length,
      0,
    )
    + (candidateTcpPose === null ? 0 : 1)
    + (presentedRuntime?.measuredRobot === null
      || presentedRuntime?.measuredRobot === undefined
      ? 0
      : visualCount)
    + (presentedRuntime?.measuredObjects.reduce(
      (total, measurement) => {
        const object = sceneObjects.find(
          (candidate) => candidate.scene_resource_id === measurement.objectId,
        );
        return total + (object?.collision_shapes.filter((shape) => (
          shape.type !== 'mesh' || sceneOwnerIdentity !== undefined
        )).length ?? 0);
      }, 0,
    ) ?? 0)
    + (presentedRuntime?.measuredTcp === null
      || presentedRuntime?.measuredTcp === undefined ? 0 : 1)
    + (candidateRobot === null ? 0 : visualCount);
  const canvasDemandActive = viewerActive && (
    visibleGeometryCount > 0
    || viewerFrameIdentity !== undefined
    || pointCloudEnabled
  );
  const reportModelMaterialized = useCallback((durationMs: number) => {
    recordViewerModelBuild(durationMs);
  }, []);
  const reportAssetError = useCallback((error: Error) => {
    reportViewerQualificationError(error);
    setAssetError(error);
  }, [setAssetError]);
  const reportIsolatedAssetError = useCallback((error: Error) => {
    reportViewerQualificationError(error);
    setAssetWarning(error.message || 'One viewer mesh could not be rendered.');
  }, [setAssetWarning]);
  const reportViewerCommit = useCallback((
    _id: string,
    _phase: 'mount' | 'update' | 'nested-update',
    actualDuration: number,
  ) => {
    recordViewerReactCommit(actualDuration);
  }, []);

  useEffect(() => {
    const available = new Set(
      sceneObjects.map((object) => object.scene_resource_id),
    );
    setEditLockedObjectIds((current) => {
      const retained = new Set([...current].filter((identity) => available.has(identity)));
      return retained.size === current.size ? current : retained;
    });
  }, [sceneObjects]);
  useEffect(() => {
    setViewerQualificationViewerActive(viewerActive);
    setViewerQualificationWireframeVisible(wireframeVisible);
    if (canvasDemandActive && viewerQualificationOptedIn()) {
      startViewerPerformanceMonitoring();
    } else {
      stopViewerPerformanceMonitoring();
    }
    return () => stopViewerPerformanceMonitoring();
  }, [canvasDemandActive, viewerActive, wireframeVisible]);
  useEffect(() => {
    if (!viewerActive) return undefined;
    const onKeyDown = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement | null;
      if (
        event.ctrlKey || event.metaKey || event.altKey
        || target?.isContentEditable
        || ['INPUT', 'SELECT', 'TEXTAREA'].includes(target?.tagName ?? '')
      ) return;
      const key = event.key.toLowerCase();
      if (key === 'w') setTransformMode('translate');
      else if (key === 'e') setTransformMode('rotate');
      else if (key === 'r' && scaleEnabled) setTransformMode('scale');
      else if (key === 'q') setTransformSpace((current) => (
        current === 'world' ? 'local' : 'world'
      ));
      else if (key === 'escape') {
        setContextMenu(null);
        onSelectSceneResource(null);
        onSelectStructure(null);
      }
      else return;
      event.preventDefault();
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [onSelectSceneResource, onSelectStructure, scaleEnabled, viewerActive]);
  useEffect(() => {
    if (contextMenu === null) return undefined;
    const dismissOutside = (event: PointerEvent) => {
      if (
        event.target instanceof Node
        && contextMenuRef.current?.contains(event.target)
      ) return;
      setContextMenu(null);
    };
    window.addEventListener('pointerdown', dismissOutside);
    return () => window.removeEventListener('pointerdown', dismissOutside);
  }, [contextMenu]);
  useEffect(() => {
    setViewerQualificationModel(
      manifest.status === 'ready' && visibleGeometryCount > 0,
      visibleGeometryCount,
    );
  }, [manifest.status, visibleGeometryCount]);

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
    setAssetWarning(null);
    onRetry();
  };

  return (
    <section className="panel viewer-panel" aria-labelledby="viewer-title">
      <header className="panel-header viewer-header">
        <strong id="viewer-title">3D viewer</strong>
        <div className="viewer-header-controls">
          <div className="viewer-tool-strip" aria-label="Scene viewport controls">
            <button
              type="button"
              aria-pressed={transformMode === 'translate'}
              disabled={!generalTransformEnabled}
              onClick={() => setTransformMode('translate')}
              title="Translate selected object (W)"
            >W</button>
            <button
              type="button"
              aria-pressed={transformMode === 'rotate'}
              disabled={!generalTransformEnabled}
              onClick={() => setTransformMode('rotate')}
              title="Rotate selected object (E)"
            >E</button>
            <button
              type="button"
              aria-pressed={transformMode === 'scale'}
              disabled={!scaleEnabled}
              onClick={() => setTransformMode('scale')}
              title="Scale a selected box or mesh object (R)"
            >R</button>
            <button
              type="button"
              className="viewer-space-control"
              onClick={() => setTransformSpace((current) => current === 'world' ? 'local' : 'world')}
              title="Toggle local/world coordinates (Q)"
            >{transformSpace}</button>
            <span className="viewer-tool-separator" />
            <button
              type="button"
              aria-label="Frame all visible content"
              onClick={() => issueCameraCommand('frame_all')}
              title="Frame all visible content"
            >
              <span className="viewer-camera-icon" aria-hidden="true">⊞</span>
            </button>
            <button
              type="button"
              aria-label="Frame selected object"
              disabled={selectedCameraTarget === null}
              onClick={() => issueCameraCommand('frame_selection')}
              title="Frame selected object"
            >
              <svg
                className="viewer-camera-icon"
                viewBox="0 0 16 16"
                aria-hidden="true"
              >
                <circle cx="8" cy="8" r="2.25" />
                <path d="M8 1.5v3M8 11.5v3M1.5 8h3M11.5 8h3" />
              </svg>
            </button>
            <button
              type="button"
              aria-label="Reset camera"
              onClick={() => issueCameraCommand('reset')}
              title="Reset camera"
            >
              <span className="viewer-camera-icon" aria-hidden="true">↺</span>
            </button>
          </div>
          <div className="viewer-surface-launchers">
            <button
              type="button"
              aria-pressed={inspectorPanel === 'robot'}
              disabled={robotControlSurface === undefined}
              onClick={() => toggleInspectorPanel('robot')}
            >Robot</button>
            <button
              type="button"
              aria-pressed={inspectorPanel === 'object'}
              onClick={() => toggleInspectorPanel('object')}
            >Object</button>
            <button
              type="button"
              aria-pressed={inspectorPanel === 'tools'}
              onClick={() => toggleInspectorPanel('tools')}
            >Tools</button>
          </div>
        </div>
      </header>

      <div className="viewer-stage" role="img" aria-label="Scene authoring viewport">
        {viewerActive && (
          visibleGeometryCount > 0
          || viewerFrameIdentity !== undefined
          || pointCloudEnabled
        ) && (
          <ViewerCanvas
              manifest={manifest.status === 'ready' ? manifest.data : null}
              collisionsVisible={collisionsVisible}
              wireframeVisible={wireframeVisible}
              onAssetError={reportAssetError}
              onIsolatedAssetError={reportIsolatedAssetError}
              sceneObjects={sceneObjects}
              selectedSceneResourceId={selectedSceneResourceId}
              onSelectSceneResource={onSelectSceneResource}
              selectedStructureName={selectedStructureName}
              onSelectStructure={onSelectStructure}
              candidateTcpPose={candidateTcpPose}
              candidateStale={candidateStale}
              candidateTcpEditable={candidateTransformEnabled}
              onEditCandidateTcpPose={onEditCandidateTcpPose}
              candidateRobot={candidateRobot}
              runtimePresentation={presentedRuntime}
              sceneOwnerIdentity={sceneOwnerIdentity}
              programDraftIdentity={programDraftIdentity}
              viewerFrameIdentity={viewerFrameIdentity}
              readViewerFrameIdentity={readViewerFrameIdentity}
              pointCloudFrame={pointCloud.frame}
              viewer={viewer}
              frameAxes={frameAxes}
              objectAxesVisibleIds={objectAxesVisible}
              cameraPresentation={cameraPresentation}
              onModelMaterialized={reportModelMaterialized}
              onViewerCommit={reportViewerCommit}
              transformMode={transformMode}
              transformSpace={transformSpace}
              transformEnabled={transformEnabled}
              objectEditorActive={inspectorPanel === 'object'}
              robotEditorActive={inspectorPanel === 'robot'}
              transformIdentity={transformIdentity}
              onCommitSceneTransform={transformCommit ?? (async () => false)}
              onSceneContext={(sceneResourceId, x, y) => {
                if (
                  selectionLocked
                  && sceneResourceId !== selectedSceneResourceId
                ) return;
                setContextMenu({ sceneResourceId, x, y });
              }}
              cameraCommand={cameraCommand}
          />
        )}

        {contextMenu !== null && (
          <div
            ref={contextMenuRef}
            className="viewer-context-menu"
            role="menu"
            style={{ left: contextMenu.x, top: contextMenu.y }}
          >
            <strong>{contextMenu.sceneResourceId}</strong>
            <button
              type="button"
              role="menuitem"
              onClick={() => {
                issueCameraCommand('frame_selection', {
                  kind: 'scene_resource',
                  id: contextMenu.sceneResourceId,
                });
                setContextMenu(null);
              }}
            >Frame selection</button>
            <button
              type="button"
              role="menuitem"
              onClick={() => {
                onEditSceneResource?.(contextMenu.sceneResourceId);
                setContextMenu(null);
              }}
            >Edit properties</button>
            <button
              type="button"
              role="menuitem"
              onClick={() => {
                setEditLockedObjectIds((current) => {
                  const next = new Set(current);
                  if (next.has(contextMenu.sceneResourceId)) {
                    next.delete(contextMenu.sceneResourceId);
                  } else {
                    next.add(contextMenu.sceneResourceId);
                  }
                  return next;
                });
                setContextMenu(null);
              }}
            >
              {editLockedObjectIds.has(contextMenu.sceneResourceId)
                ? 'Unlock object editing'
                : 'Lock object editing'}
            </button>
            <button
              type="button"
              role="menuitem"
              disabled={
                onDuplicateSceneResource === undefined
                || contextAvailability?.duplicate !== true
              }
              title={contextAvailability?.reason ?? undefined}
              onClick={() => {
                void onDuplicateSceneResource?.(contextMenu.sceneResourceId);
                setContextMenu(null);
              }}
            >Duplicate object</button>
            <button
              type="button"
              role="menuitem"
              disabled={
                onDetachSceneResource === undefined
                || programDraftPreview
                || contextAvailability?.detach !== true
              }
              title={contextAvailability?.detachReason ?? undefined}
              onClick={() => {
                void onDetachSceneResource?.(contextMenu.sceneResourceId);
                setContextMenu(null);
              }}
            >Detach carried object</button>
            <button
              type="button"
              role="menuitem"
              className="danger-button"
              disabled={
                onRemoveSceneResource === undefined
                || contextAvailability?.remove !== true
              }
              title={contextAvailability?.reason ?? undefined}
              onClick={() => {
                void onRemoveSceneResource?.(contextMenu.sceneResourceId);
                setContextMenu(null);
              }}
            >Remove object</button>
            <button
              type="button"
              role="menuitem"
              onClick={() => setContextMenu(null)}
            >Close</button>
          </div>
        )}

        {manifest.status === 'loading' && visibleGeometryCount === 0 && !pointCloudEnabled && (
          <div className="viewer-state" role="status">
            <strong>Loading active model…</strong>
            <span>Waiting for a complete retained-neutral projection.</span>
          </div>
        )}
        {assetWarning !== null && pointCloud.error === null && (
          <div className="viewer-layer-warning" role="status">
            One mesh is unavailable; the remaining scene is still usable. {assetWarning}
          </div>
        )}
        {manifest.status === 'absent' && visibleGeometryCount === 0 && !pointCloudEnabled && (
          <div className="viewer-state">
            <strong>No active retained deployment model</strong>
            <span>{manifest.message}</span>
            <span>No empty or placeholder robot is substituted.</span>
          </div>
        )}
        {manifest.status === 'error' && visibleGeometryCount === 0 && !pointCloudEnabled && (
          <div className="viewer-state error-state" role="alert">
            <strong>Structural manifest unavailable</strong>
            <span>{manifest.message}</span>
            <button type="button" onClick={retry}>Retry manifest</button>
          </div>
        )}
        {visibleGeometryCount === 0 && manifest.status === 'ready' && !pointCloudEnabled && (
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
        {pointCloudEnabled && pointCloud.error !== null && (
          <div className="viewer-state viewer-state-overlay error-state" role="alert">
            <strong>Point-cloud observer unavailable</strong>
            <span>{pointCloud.error}</span>
          </div>
        )}

        {manifest.status === 'ready'
          && assetError === null
          && visibleGeometryCount > 0
          && (programDraftPreview
            || candidateTcpPose !== null
            || candidateRobot !== null
            || presentedRuntime?.measuredRobot != null
            || (presentedRuntime?.measuredObjects.length ?? 0) > 0
            || presentedRuntime?.measuredTcp != null) && (
            <div className="viewer-overlay-legend" aria-label="Non-live viewer overlays">
              {programDraftPreview && <span>Draft scene objects · preview only</span>}
              {(candidateTcpPose !== null || candidateRobot !== null)
                && <span>Motion preview</span>}
              {(presentedRuntime?.measuredRobot != null
                || (presentedRuntime?.measuredObjects.length ?? 0) > 0
                || presentedRuntime?.measuredTcp != null)
                && <span>Measured state</span>}
            </div>
          )}
        {transportControls}
      </div>

      {inspectorTarget !== null && createPortal(
        <section
          className={`central-inspector-section viewer-inspector-section${
            inspectorPanel === null ? ' inactive' : ' active'
          }${inspectorMinimized ? ' minimized' : ''}`}
          aria-label="3D viewer inspector"
        >
          <header className="central-inspector-header">
            <strong>{inspectorPanel === 'tools'
              ? 'Viewer tools'
              : inspectorPanel === 'robot'
                ? 'Robot control'
                : inspectorPanel === 'object'
                  ? 'Selected object'
                  : 'Viewer tools'}</strong>
            {inspectorPanel === 'robot' && robotControlBlocked && (
              <span className="central-inspector-warning">Blocked</span>
            )}
            <div className="central-inspector-actions">
              {inspectorPanel === 'robot' && (
                <button
                  type="button"
                  className="inspector-info-button"
                  aria-label="Robot control information"
                  title="Edits create a no-motion validation preview after a short delay. Only Approve can move the robot."
                >i</button>
              )}
              <button
                type="button"
                disabled={inspectorPanel === null}
                aria-label="Maximize viewer inspector"
                title="Maximize viewer inspector"
                onClick={onInspectorMaximize}
              >□</button>
              <button
                type="button"
                disabled={inspectorPanel === null}
                aria-label={inspectorMinimized ? 'Restore viewer inspector' : 'Minimize viewer inspector'}
                onClick={() => onInspectorMinimizedChange?.(!inspectorMinimized)}
              >{inspectorMinimized ? '⌃' : '⌄'}</button>
              <button
                type="button"
                disabled={inspectorPanel === null}
                aria-label="Close viewer inspector"
                onClick={() => setInspectorPanel(null)}
              >×</button>
            </div>
          </header>
          {inspectorPanel !== null && !inspectorMinimized && (
            <div className={`central-inspector-body${
              inspectorPanel === 'robot' ? ' robot-inspector-body' : ''
            }`}>
              {inspectorPanel === 'robot' ? (
                robotControlSurface ?? (
                  <p className="panel-state">Robot control is unavailable.</p>
                )
              ) : inspectorPanel === 'tools' ? (
                <div className="viewer-options-body">
                  <label className="toggle-control">
                    <input
                      type="checkbox"
                      aria-label="Wireframe overlays"
                      checked={wireframeVisible}
                      onChange={(event) => setWireframeVisible(event.target.checked)}
                    />
                    <span>Wireframe overlays</span>
                  </label>
                  <label className="toggle-control">
                    <input
                      type="checkbox"
                      checked={collisionsVisible}
                      onChange={(event) => setCollisionsVisible(event.target.checked)}
                      disabled={manifest.status !== 'ready'}
                    />
                    <span>Collision geometry</span>
                  </label>
                  <label className="toggle-control">
                    <input
                      type="checkbox"
                      checked={pointCloudVisible}
                      onChange={(event) => setPointCloudVisible(event.target.checked)}
                      disabled={pointCloudCellId === undefined}
                    />
                    <span>Point cloud</span>
                  </label>
                  <label className="toggle-control">
                    <input
                      type="checkbox"
                      checked={measuredStateVisible}
                      disabled={runtimePresentation === undefined
                        || runtimePresentation.status.total === 0}
                      onChange={(event) => setMeasuredStateVisible(event.target.checked)}
                    />
                    <span>Measured-state overlay</span>
                  </label>
                  <div className="viewer-options-actions">
                    <button
                      type="button"
                      className="quiet-button"
                      disabled={pointCloud.frame === null}
                      onClick={pointCloud.clear}
                      title="Clear the currently presented point cloud; the next owner frame may repopulate it."
                    >Clear points</button>
                    <button type="button" className="quiet-button" onClick={retry}>
                      Reload model
                    </button>
                  </div>
                  {programDraftPreview && (
                    <p className="viewer-source-explanation">
                      <strong>Draft scene objects are a preview, not live.</strong>
                      The backend reports no current scene objects, so editable
                      Program Draft geometry is shown for authoring only.
                    </p>
                  )}
                  {pointCloud.frame !== null && (
                    <span
                      className="status-pill"
                      title={`${pointCloud.frame.ownerId} · sequence ${pointCloud.frame.sequence.toString()}`}
                    >{pointCloud.frame.pointCount} points</span>
                  )}
                  {manifest.status === 'ready' && (
                    <details className="viewer-model-facts">
                      <summary>Model and source facts</summary>
                      <span>Cell <code>{manifest.data.cell_id}</code></span>
                      <span>{manifest.data.links.length} links · {manifest.data.joints.length} joints</span>
                      <span>{sceneObjects.length} {programDraftPreview ? 'Draft-preview' : 'owner-scene'} objects</span>
                      <span>Root <code>{manifest.data.projection.root_frame}</code></span>
                      <span>Model <code>{manifest.data.model_key}</code></span>
                      <span>Bundle <code>{manifest.data.retained_bundle_id}</code></span>
                      <span>Retained model <strong>neutral coordinates</strong></span>
                      <span>Measured overlay <strong>{runtimePresentation === undefined
                        ? 'unavailable'
                        : `${runtimePresentation.status.freshGood} fresh / ${runtimePresentation.status.total} sources`}</strong></span>
                      <span>Scene overlay <strong>owner-issued</strong></span>
                      <span>Operational validity <strong>not asserted</strong></span>
                    </details>
                  )}
                </div>
              ) : objectControlSurface !== undefined ? (
                <>
                  <div className="viewer-object-global-actions">
                    <button
                      type="button"
                      disabled={selectedCameraTarget === null}
                      onClick={() => issueCameraCommand('frame_selection')}
                    >Frame selection</button>
                    {selectedObject !== null && (
                      <button
                        type="button"
                        onClick={() => setEditLockedObjectIds((current) => {
                          const next = new Set(current);
                          if (next.has(selectedObject.scene_resource_id)) {
                            next.delete(selectedObject.scene_resource_id);
                          } else {
                            next.add(selectedObject.scene_resource_id);
                          }
                          return next;
                        })}
                      >
                        {editLockedObjectIds.has(selectedObject.scene_resource_id)
                          ? 'Unlock editing'
                          : 'Lock editing'}
                      </button>
                    )}
                  </div>
                  {objectControlSurface}
                </>
              ) : selectedObject !== null ? (
          <div className="viewer-object-quick-panel">
            <dl>
              <dt>Scene resource</dt>
              <dd><code>{selectedObject.scene_resource_id}</code></dd>
              <dt>Placement</dt>
              <dd>{selectedObject.placement.kind.replaceAll('_', ' ')}</dd>
              <dt>Geometry</dt>
              <dd>{selectedObject.collision_shapes.length} collision shape(s)</dd>
              <dt>Source</dt>
              <dd>{programDraftPreview ? 'Program Draft preview' : 'owner-issued scene'}</dd>
            </dl>
            <div className="button-row">
              <button
                type="button"
                onClick={() => issueCameraCommand('frame_selection')}
              >Frame object</button>
              <button
                type="button"
                disabled={onEditSceneResource === undefined}
                onClick={() => onEditSceneResource?.(selectedObject.scene_resource_id)}
              >Edit properties</button>
              <button
                type="button"
                disabled={onDuplicateSceneResource === undefined
                  || selectedObjectAvailability?.duplicate !== true}
                title={selectedObjectAvailability?.reason ?? undefined}
                onClick={() => { void onDuplicateSceneResource?.(selectedObject.scene_resource_id); }}
              >Duplicate</button>
              <button
                type="button"
                disabled={onDetachSceneResource === undefined
                  || programDraftPreview
                  || selectedObjectAvailability?.detach !== true}
                title={selectedObjectAvailability?.detachReason ?? undefined}
                onClick={() => { void onDetachSceneResource?.(selectedObject.scene_resource_id); }}
              >Detach</button>
              <button
                type="button"
                onClick={() => setEditLockedObjectIds((current) => {
                  const next = new Set(current);
                  if (next.has(selectedObject.scene_resource_id)) {
                    next.delete(selectedObject.scene_resource_id);
                  } else {
                    next.add(selectedObject.scene_resource_id);
                  }
                  return next;
                })}
              >
                {editLockedObjectIds.has(selectedObject.scene_resource_id)
                  ? 'Unlock editing'
                  : 'Lock editing'}
              </button>
              <button
                type="button"
                className="danger-button"
                disabled={onRemoveSceneResource === undefined
                  || selectedObjectAvailability?.remove !== true}
                title={selectedObjectAvailability?.reason ?? undefined}
                onClick={() => { void onRemoveSceneResource?.(selectedObject.scene_resource_id); }}
              >Remove</button>
            </div>
          </div>
              ) : selectedStructureName !== null ? (
          <div className="viewer-object-quick-panel">
            <dl>
              <dt>Robot link</dt>
              <dd><code>{selectedStructureName}</code></dd>
              <dt>Source</dt>
              <dd>retained structure</dd>
            </dl>
            <button type="button" onClick={() => issueCameraCommand('frame_selection')}>
              Frame link
            </button>
          </div>
              ) : (
          <p className="panel-state">
            Select a scene object or robot link in the viewport to inspect it.
          </p>
              )}
            </div>
          )}
        </section>,
        inspectorTarget,
      )}

    </section>
  );
}

export function StructuralViewer(props: StructuralViewerProps) {
  const [recoveryGeneration, setRecoveryGeneration] = useState(0);
  useEffect(() => {
    setViewerQualificationWireframeDefault(DEFAULT_WIREFRAME_VISIBLE);
    return connectViewerQualification();
  }, []);
  const retry = () => {
    setRecoveryGeneration((current) => current + 1);
    props.onRetry();
  };
  const fallback = (
    <section className="panel viewer-panel" aria-labelledby="viewer-failure-title">
      <div className="viewer-stage viewer-fallback-stage">
        <div className="viewer-state error-state" role="alert">
          <strong id="viewer-failure-title">3D viewer isolated</strong>
          <span>
            The viewport failed, but Program Draft and runtime controls remain usable and unchanged.
          </span>
          <button type="button" onClick={retry}>Retry viewer</button>
        </div>
      </div>
    </section>
  );
  return (
    <ViewerFailureBoundary key={recoveryGeneration} fallback={fallback}>
      <StructuralViewerContent {...props} />
    </ViewerFailureBoundary>
  );
}
