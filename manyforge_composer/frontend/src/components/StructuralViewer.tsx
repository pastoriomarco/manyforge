import {
  Component,
  memo,
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
  Grid,
  OrbitControls,
  PerspectiveCamera,
  useGLTF,
} from '@react-three/drei';
import * as THREE from 'three';
import { AuthoringTransformControls } from './AuthoringTransformControls';
import type { GizmoNoChange } from './AuthoringTransformControls';
import { GizmoSteps, useGizmoSettings } from './common/GizmoControls';
import type { GizmoController } from './common/GizmoControls';
import type { GizmoSettings } from '../utils/gizmo';
import { api, resolveStructuralAssetUrl } from '../api/client';
import {
  subscribeSnapshotObserver,
  uploadSnapshotObserverResult,
} from '../api/snapshotObserverClient';
import type { PointCloudFrame } from '../api/pointcloudObserverClient';
import { usePointCloudFrame } from '../hooks/usePointCloudFrame';
import { SceneOutline } from './SceneOutline';
import { PendingApplyNote, SettingsSubheading } from './SettingsScope';
import { useComposerSettings } from '../hooks/useComposerSettings';
import { useCardPanelLayout } from '../hooks/useCardPanelLayout';
import { ViewerGraphicsSettings } from './ViewerGraphicsSettings';
import { onOpenSettingsRequest } from '../state/settingsNavigation';
import {
  DEFAULT_COMPOSER_SETTINGS,
  type ComposerSettings,
  type ViewerRendererKind,
} from '../state/composerSettings';
import { SnapshotResponder } from './SnapshotResponder';
import {
  MAX_AUTHORITATIVE_INTERPOLATION_MS,
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
import type { ViewerModelPresentation } from '../state/viewerModelReadiness';
import type {
  AttachedObjectPresentation,
  CandidateRobotPresentation,
  MeasuredRobotPresentationReader,
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
import type { Nanoseconds } from '../types/nanoseconds';
import type { SceneObjectMutationAvailability } from '../state/sceneAuthoring';
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
  pageViewerRendererInspection,
  rememberViewerRendererInspection,
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
  type ViewerRendererInspection,
} from '../utils/viewerPerformance';
import { usePublishWorkbenchNotices } from '../state/workbenchNotices';
import { viewerNotices } from '../state/viewerNotices';
import { VIEWPORT_COLORS } from '../styles/tokens';

interface StructuralViewerProps {
  viewerActive: boolean;
  manifest: StructuralResourceState<SceneUrdfManifest>;
  modelPresentation: ViewerModelPresentation;
  onRetry: () => void;
  sceneObjects?: readonly LogicalSceneObject[];
  programDraftPreview?: boolean;
  staleScenePreview?: boolean;
  sceneSourceControl?: ReactNode;
  programDraftIdentity?: {
    draftId: string;
    draftRevision: number;
  };
  selectedSceneResourceId?: string | null;
  onSelectSceneResource?: (sceneResourceId: string | null) => void;
  selectedStructureName?: string | null;
  onSelectStructure?: (structureName: string | null) => void;
  /** Object edits commit only into the Program Draft, fenced by its identity. */
  programDraftEditIdentity?: {
    draftId: string;
    draftRevision: number;
  };
  onCommitProgramDraftTransform?: (
    sceneResourceId: string,
    pose: LogicalScenePose,
    scale: [number, number, number],
    expectedDraftIdentity: { ownerEpoch: string; generation: number },
  ) => Promise<boolean>;
  onDuplicateSceneResource?: (sceneResourceId: string) => Promise<boolean>;
  onRemoveSceneResource?: (sceneResourceId: string) => Promise<boolean>;
  onEditSceneResource?: (sceneResourceId: string) => void;
  selectionLocked?: boolean;
  sceneMutationAvailability?: Record<string, SceneObjectMutationAvailability>;
  candidateTcpPose?: Pose3Value | null;
  candidateStale?: boolean;
  candidateTcpEditable?: boolean;
  onEditCandidateTcpPose?: (pose: Pose3Value | null, noChange?: GizmoNoChange) => void;
  onBeginCandidateTcpDrag?: () => boolean;
  candidateContextKey?: string;
  gizmo?: GizmoController;
  candidateRobot?: CandidateRobotPresentation | null;
  candidateTool?: CandidateRobotPresentation | null;
  candidateRejected?: boolean;
  /** The advisory failed-node correction preview (D4), independent of the Robot panel. */
  correctionRobot?: CandidateRobotPresentation | null;
  correctionStale?: boolean;
  runtimePresentation?: RuntimeScenePresentation;
  /** Exact, fresh scene-runtime sources for the optional diagnostic overlays. */
  measuredStateGhostPresentation?: RuntimeScenePresentation;
  /** Measured comparison robot from the viewer stream's measured rows. */
  measuredStateGhostRobot?: MeasuredRobotPresentationReader;
  /** Applied logical objects matched by measured scene-runtime poses. */
  measuredStateGhostObjects?: readonly LogicalSceneObject[];
  executionObservationPresentation?: MeasuredRobotPresentationReader;
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
  frameAxes?: ReadonlyArray<{
    linkName: string;
    sourceLinkName: string;
    sourceJointName: string | null;
    viewerJointName: string | null;
    componentId: string | null;
    modelInstanceId: string | null;
    frameRoleIds: string[];
    defaultVisible: boolean;
    poseAvailable: boolean;
  }>;
  objectAxesVisibleIds?: string[];
  objectAxesRows?: ReadonlyArray<{
    sceneResourceId: string;
    attached: boolean;
    poseAvailable: boolean;
    persistent: boolean;
    defaultVisible: boolean;
    sourceLabel: string;
  }>;
  onObjectAxesVisibleIdsChange?: (ids: string[]) => void;
  onAxesSizesCommit?: (frameSizeM: number, objectSizeM: number) => void;
  robotControlSurface?: ReactNode;
  robotControlBlocked?: boolean;
  /** A manual robot move is active: the Robot panel stays open and visible
   * so its Stop is at hand. */
  robotMotionActive?: boolean;
  objectControlSurface?: ReactNode;
  /** Tool & I/O signals; mounted (and polling) only while its panel is open. */
  ioControlSurface?: ReactNode;
  transportControls?: ReactNode | ((
    openRobotControls: () => void,
    viewerSettings: { open: () => void; previewLayerActive: boolean },
  ) => ReactNode);
  /** Motion panel settings shown in the viewer settings panel. */
  controlPanelSettings?: ReactNode;
  /** Program viewer settings whose Draft value is not applied yet. */
  programViewerPendingApply?: { frameAxes: boolean; objectAxes: boolean };
  runtimeOverlay?: ReactNode;
  inspectorTarget?: HTMLElement | null;
  inspectorMinimized?: boolean;
  onInspectorMinimizedChange?: (minimized: boolean) => void;
  onInspectorMaximize?: () => void;
  onInspectorPanelChange?: (panel: ViewerInspectorPanel) => void;
}

export type ViewerInspectorPanel = 'tools' | 'robot' | 'object' | 'io' | null;

const READ_ONLY_SCENE_REASON = 'Read-only: the Program Draft cannot be edited now, or this Applied view hides unapplied Draft changes (switch to the Draft Scene).';

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

export const DEFAULT_WIREFRAME_VISIBLE = DEFAULT_COMPOSER_SETTINGS.viewer.wireframeOverlays;
export const DEFAULT_MEASURED_STATE_GHOSTS_VISIBLE = false;

export type SceneObjectAppearance =
  | 'authoritative'
  | 'draft'
  | 'attached'
  | 'comparison';

export function sceneObjectMaterial(
  appearance: SceneObjectAppearance,
  selected: boolean,
): {
  color: string;
  emissive: string;
  opacity: number;
  transparent: boolean;
  renderOrder: number;
} {
  if (selected) return {
    color: VIEWPORT_COLORS.objectSelected,
    emissive: VIEWPORT_COLORS.objectSelectedEmissive,
    opacity: 1,
    transparent: false,
    renderOrder: 12,
  };
  if (appearance === 'attached') return {
    color: VIEWPORT_COLORS.objectAttached,
    emissive: VIEWPORT_COLORS.objectAttachedEmissive,
    opacity: 0.85,
    transparent: true,
    renderOrder: 13,
  };
  if (appearance === 'draft') return {
    color: VIEWPORT_COLORS.objectDraft,
    emissive: VIEWPORT_COLORS.objectDraftEmissive,
    opacity: 0.62,
    transparent: true,
    renderOrder: 14,
  };
  if (appearance === 'comparison') return {
    color: VIEWPORT_COLORS.objectComparison,
    emissive: VIEWPORT_COLORS.objectComparisonEmissive,
    opacity: 0.45,
    transparent: true,
    renderOrder: 16,
  };
  return {
    color: VIEWPORT_COLORS.object,
    emissive: VIEWPORT_COLORS.objectEmissive,
    opacity: 1,
    transparent: false,
    renderOrder: 8,
  };
}

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

function measuredRobotStreamIdentity(
  manifestModelKey: string,
  presentation: NonNullable<RuntimeScenePresentation['measuredRobot']>,
): string {
  // The logical-scene generation is intentionally absent. Robot motion is
  // owned by the exact execution observation stream and retained model; a
  // scene attachment/revision must not restart or snap that motion stream.
  return JSON.stringify([
    manifestModelKey,
    presentation.objectId,
    presentation.sourceId,
    presentation.sourceEpoch,
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
  liveFrame,
  latestFrameAppliers,
  onClick,
  children,
}: {
  name: string;
  streamIdentity: string;
  sourceSequence: number;
  observedAtNs: Nanoseconds | null;
  positionM: Vector3Tuple;
  orientationXyzw: QuaternionXyzwTuple;
  liveFrame?: {
    read: () => AuthoritativeTransformFrame | null;
    subscribe: (listener: () => void) => () => void;
  };
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

  const acceptFrame = useCallback((next: AuthoritativeTransformFrame) => {
    if (group.current === null) return;
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
  }, [invalidate]);

  useLayoutEffect(() => {
    latestFrameAppliers.add(applyLatestFrame);
    return () => {
      latestFrameAppliers.delete(applyLatestFrame);
    };
  }, [applyLatestFrame, latestFrameAppliers]);

  useLayoutEffect(() => {
    acceptFrame({
      streamIdentity,
      sourceSequence,
      observedAtNs,
      positionM: [px, py, pz],
      orientationXyzw: [qx, qy, qz, qw],
    });
  }, [
    acceptFrame,
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

  useLayoutEffect(() => {
    if (liveFrame === undefined) return undefined;
    const apply = () => {
      const frame = liveFrame.read();
      if (frame !== null) acceptFrame(frame);
    };
    apply();
    return liveFrame.subscribe(apply);
  }, [acceptFrame, liveFrame]);

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
        material.color?.set(VIEWPORT_COLORS.collision);
        material.emissive?.set(VIEWPORT_COLORS.collisionEmissive);
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
  appearance,
  selected,
  wireframe,
}: {
  url: string;
  appearance: SceneObjectAppearance;
  selected: boolean;
  wireframe: boolean;
}) {
  const presentation = useMemo(
    () => sceneObjectMaterial(appearance, selected),
    [appearance, selected],
  );
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
        material.color?.set(presentation.color);
        material.emissive?.set(presentation.emissive);
        material.transparent = presentation.transparent || wireframe;
        material.opacity = presentation.opacity;
        material.wireframe = wireframe;
        material.depthWrite = !material.transparent;
        material.side = THREE.DoubleSide;
        return material;
      });
      mesh.material = Array.isArray(mesh.material) ? materials : materials[0];
      mesh.renderOrder = presentation.renderOrder;
    });
    return root;
  }, [gltf.scene, presentation, wireframe]);
  useEffect(() => () => disposeOwnedMaterials(clone), [clone]);
  return <primitive object={clone} />;
}

/** Drop a failed GLB from the loader cache so a later remount refetches it. */
function forgetSceneMeshAsset(url: string): void {
  (useGLTF as unknown as { clear?: (url: string) => void })
    .clear?.(resolveStructuralAssetUrl(url));
}

function SceneObjectMesh({
  shape,
  ownerIdentity,
  draftIdentity,
  appearance = 'authoritative',
  selected,
  wireframe,
}: {
  shape: Extract<LogicalSceneShape, { type: 'mesh' }>;
  ownerIdentity?: NonNullable<StructuralViewerProps['sceneOwnerIdentity']>;
  draftIdentity?: NonNullable<StructuralViewerProps['programDraftIdentity']>;
  appearance?: SceneObjectAppearance;
  selected: boolean;
  wireframe: boolean;
}) {
  const [url, setUrl] = useState<string | null>(null);
  const [error, setError] = useState<Error | null>(null);
  const draftId = draftIdentity?.draftId;
  const draftRevision = draftIdentity?.draftRevision;
  // Owner-scope URLs are content-addressed and stay valid across owner-epoch
  // rotation and pose generations, so only presence of an owner matters.
  const ownerBound = ownerIdentity !== undefined;
  const scaleX = shape.scale.x;
  const scaleY = shape.scale.y;
  const scaleZ = shape.scale.z;
  const isolateError = useCallback((failure: Error) => {
    reportViewerQualificationError(failure);
    setError(failure);
  }, [setError]);
  const isolateAssetError = useCallback((failure: Error) => {
    if (url !== null) forgetSceneMeshAsset(url);
    isolateError(failure);
  }, [isolateError, url]);
  useEffect(() => {
    const controller = new AbortController();
    setUrl(null);
    setError(null);
    const scale = { x: scaleX, y: scaleY, z: scaleZ };
    const resolution = draftId === undefined || draftRevision === undefined
      ? !ownerBound
        ? Promise.reject(new Error('Scene mesh has no exact source identity.'))
        : api.scene.resolveMesh(
            shape.resource_uri,
            scale,
            controller.signal,
          ).then((asset) => asset.mesh_asset_url)
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
      if (controller.signal.aborted) return;
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
    ownerBound,
    scaleX,
    scaleY,
    scaleZ,
    shape.resource_uri,
  ]);
  return error !== null
    ? (
      <mesh renderOrder={20}>
        <boxGeometry args={[0.06, 0.06, 0.06]} />
        <meshBasicMaterial color={VIEWPORT_COLORS.error} wireframe />
      </mesh>
    )
    : url === null
    ? null
    : (
      <AssetErrorBoundary resetKey={url} onError={isolateAssetError}>
        <Suspense fallback={null}>
          <SceneObjectMeshAsset
            url={url}
            appearance={appearance}
            selected={selected}
            wireframe={wireframe}
          />
        </Suspense>
      </AssetErrorBoundary>
    );
}

export function clonePreviewMesh(source: THREE.Object3D, color: string, opacity: number): THREE.Object3D {
  const root = source.clone(true);
  root.traverse((object) => {
    const mesh = object as THREE.Mesh;
    if (!mesh.isMesh) return;
    const sourceMaterials = Array.isArray(mesh.material)
      ? mesh.material
      : [mesh.material];
    const materials = sourceMaterials.map(() => new THREE.MeshBasicMaterial({
      color,
      transparent: true,
      opacity,
      toneMapped: false,
      depthWrite: false,
    }));
    mesh.material = Array.isArray(mesh.material) ? materials : materials[0];
    mesh.renderOrder = 14;
    mesh.raycast = ignoreViewerRaycast;
  });
  return root;
}

function OverlayVisualMesh({ url, color, opacity }: { url: string; color: string; opacity: number }) {
  const gltf = useGLTF(resolveStructuralAssetUrl(url));
  const clone = useMemo(() => clonePreviewMesh(gltf.scene, color, opacity), [color, gltf.scene, opacity]);
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
      <meshStandardMaterial color={VIEWPORT_COLORS.primitive} metalness={0.18} roughness={0.62} />
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
        color={VIEWPORT_COLORS.collision}
        emissive={VIEWPORT_COLORS.collisionEmissive}
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
  opacity,
}: {
  primitive: SceneUrdfPrimitive;
  color: string;
  opacity: number;
}) {
  const cylinderRotation: [number, number, number] | undefined =
    primitive.type === 'cylinder'
    ? [Math.PI / 2, 0, 0]
    : undefined;
  return (
    <mesh rotation={cylinderRotation} renderOrder={14} raycast={ignoreViewerRaycast}>
      <PrimitiveGeometry primitive={primitive} />
      <meshBasicMaterial
        color={color}
        transparent
        opacity={opacity}
        toneMapped={false}
        depthWrite={false}
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

export function AttachedLogicalSceneObject({
  presentation,
  selected,
  axesVisible,
  axesSizeM,
  wireframe,
  ownerIdentity,
  onSelect,
}: {
  presentation: AttachedObjectPresentation;
  selected: boolean;
  axesVisible: boolean;
  axesSizeM: number;
  wireframe: boolean;
  ownerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
  onSelect: (event: ThreeEvent<MouseEvent>, sceneResourceId: string) => void;
}) {
  const object = presentation.object;
  const pose = presentation.poseInCarrierFrame;
  return (
    <group
      name={`scene:${object.scene_resource_id}`}
      position={[pose.position_m.x, pose.position_m.y, pose.position_m.z]}
      quaternion={normalizeQuaternion(pose.orientation_xyzw)}
      onClick={(event) => onSelect(event, object.scene_resource_id)}
    >
      {object.collision_shapes.map((shape, index) => shape.type === 'mesh'
        ? ownerIdentity === undefined ? null : (
            <SceneObjectMesh
              key={`attached:${object.scene_resource_id}:${index}`}
              shape={shape}
              ownerIdentity={ownerIdentity}
              appearance="attached"
              selected={selected}
              wireframe={wireframe}
            />
          )
        : (
            <SceneObjectPrimitive
              key={`attached:${object.scene_resource_id}:${index}`}
              shape={shape}
              appearance="attached"
              selected={selected}
              wireframe={wireframe}
            />
          ))}
      {(selected || axesVisible) && (
        <axesHelper args={[axesSizeM]} raycast={ignoreViewerRaycast} />
      )}
    </group>
  );
}

function MeasuredRuntimeModel({
  manifest,
  presentation,
  livePresentation,
  attachedObjects,
  collisionsVisible,
  wireframe,
  materializationStartedAtMs,
  onMaterialized,
  ownerIdentity,
  latestFrameAppliers,
  frameAxesByLink,
  frameAxesSizeM,
  objectAxesSizeM,
  selectedStructureName,
  selectedSceneResourceId,
  objectAxesVisibleIds,
  onSelect,
  onSelectSceneResource,
  onAssetError,
  poseRateHz,
  motionSmoothing,
}: {
  manifest: SceneUrdfManifest;
  presentation: NonNullable<RuntimeScenePresentation['measuredRobot']>;
  livePresentation?: MeasuredRobotPresentationReader;
  attachedObjects: AttachedObjectPresentation[];
  collisionsVisible: boolean;
  wireframe: boolean;
  materializationStartedAtMs: { current: number | null };
  onMaterialized: (durationMs: number) => void;
  ownerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
  latestFrameAppliers: Set<() => void>;
  frameAxesByLink: ReadonlySet<string>;
  frameAxesSizeM: number;
  objectAxesSizeM: number;
  selectedStructureName: string | null;
  selectedSceneResourceId: string | null;
  objectAxesVisibleIds: ReadonlySet<string>;
  onSelect: (event: ThreeEvent<MouseEvent>, structureName: string) => void;
  onSelectSceneResource: (
    event: ThreeEvent<MouseEvent>,
    sceneResourceId: string,
  ) => void;
  onAssetError: (error: Error) => void;
  /** How often live robot poses are redrawn. */
  poseRateHz: number;
  /** Interpolate between pose updates. */
  motionSmoothing: boolean;
}) {
  const reported = useRef(false);
  const linkGroups = useMemo(() => new Map(
    manifest.links.map((link) => [
      link.name,
      { current: null as THREE.Group | null },
    ]),
  ), [manifest]);
  const attachedObjectsByLink = useMemo(() => {
    const result = new Map<string, AttachedObjectPresentation[]>();
    for (const object of attachedObjects) {
      const current = result.get(object.carrierCompiledFrameName) ?? [];
      current.push(object);
      result.set(object.carrierCompiledFrameName, current);
    }
    return result;
  }, [attachedObjects]);
  useEffect(() => {
    const startedAtMs = materializationStartedAtMs.current;
    if (reported.current || startedAtMs === null) return;
    reported.current = true;
    onMaterialized(Math.max(0, performance.now() - startedAtMs));
  }, [materializationStartedAtMs, onMaterialized]);
  return (
    <group name={`owner-measured-robot:${presentation.objectId}`}>
      <MeasuredRobotFrameDriver
        manifestModelKey={manifest.model_key}
        linkGroups={linkGroups}
        presentation={presentation}
        livePresentation={livePresentation}
        latestFrameAppliers={latestFrameAppliers}
        interpolationDurationMs={motionSmoothing ? MAX_AUTHORITATIVE_INTERPOLATION_MS : 0}
        minimumCreditIntervalMs={1_000 / poseRateHz}
      />
      {manifest.links.map((link) => {
        const transform = presentation.linkTransforms[link.name];
        if (transform === undefined) return null;
        return (
          <MeasuredRuntimeLink
            key={`measured:${manifest.model_key}:${link.name}`}
            link={link}
            initialTransform={transform}
            groupRef={linkGroups.get(link.name)!}
            attachedObjects={attachedObjectsByLink.get(link.name) ?? []}
            ownerIdentity={ownerIdentity}
            collisionsVisible={collisionsVisible}
            wireframe={wireframe}
            frameAxesVisible={selectedStructureName === link.name
              || frameAxesByLink.has(link.name)}
            frameAxesSizeM={frameAxesSizeM}
            objectAxesSizeM={objectAxesSizeM}
            selectedSceneResourceId={selectedSceneResourceId}
            objectAxesVisibleIds={objectAxesVisibleIds}
            onSelect={onSelect}
            onSelectSceneResource={onSelectSceneResource}
            onAssetError={onAssetError}
          />
        );
      })}
    </group>
  );
}

/**
 * One measured-robot data-plane consumer. It applies every link from the same
 * admitted owner sample before one demand-render wake, so links cannot tear or
 * race one another and slower shell projections cannot overwrite live motion.
 */
export function MeasuredRobotFrameDriver({
  manifestModelKey,
  linkGroups,
  presentation,
  livePresentation,
  latestFrameAppliers,
  interpolationDurationMs,
  minimumCreditIntervalMs,
}: {
  manifestModelKey: string;
  linkGroups: ReadonlyMap<string, { current: THREE.Group | null }>;
  presentation: NonNullable<RuntimeScenePresentation['measuredRobot']>;
  livePresentation?: MeasuredRobotPresentationReader;
  latestFrameAppliers: Set<() => void>;
  interpolationDurationMs: number;
  minimumCreditIntervalMs: number;
}) {
  const previous = useRef(new Map<string, AuthoritativeTransformFrame>());
  const active = useRef(new Map<string, AuthoritativeTransformTransition>());
  const acceptedLiveFrame = useRef(false);
  const continuationTimer = useRef<number | null>(null);
  const appliedAfterRenderTimer = useRef<number | null>(null);
  const lastAppliedCreditAtMs = useRef<number | null>(null);
  const pendingAppliedPresentation = useRef<{
    presentation: NonNullable<RuntimeScenePresentation['measuredRobot']>;
    reader: MeasuredRobotPresentationReader;
  } | null>(null);
  const invalidate = useThree((state) => state.invalidate);
  const wake = useCallback(() => {
    if (continuationTimer.current !== null) {
      window.clearTimeout(continuationTimer.current);
    }
    // Observation listeners themselves run on a browser animation callback.
    // Defer invalidation by one task so React Three Fiber cannot consume it in
    // an already-closing frame and leave a capacity-one stream without credit.
    continuationTimer.current = window.setTimeout(() => {
      continuationTimer.current = null;
      invalidate();
    }, 0);
  }, [invalidate]);

  const acceptPresentation = useCallback((
    next: NonNullable<RuntimeScenePresentation['measuredRobot']>,
  ): boolean => {
    // Admit the robot as one coherent batch. A partial transform set must not
    // move a subset of links and visually invent a torn robot state.
    for (const [linkName, target] of linkGroups) {
      if (target.current !== null && next.linkTransforms[linkName] === undefined) {
        return false;
      }
    }

    const streamIdentity = measuredRobotStreamIdentity(manifestModelKey, next);
    for (const [linkName, target] of linkGroups) {
      const group = target.current;
      const transform = next.linkTransforms[linkName];
      if (group === null || transform === undefined) continue;
      const frame: AuthoritativeTransformFrame = {
        streamIdentity: `${streamIdentity}:${linkName}`,
        sourceSequence: next.sourceSequence,
        observedAtNs: next.observedAtNs,
        positionM: transform.position_m,
        orientationXyzw: transform.orientation_xyzw,
      };
      const prior = previous.current.get(linkName) ?? null;
      if (prior !== null && sameAuthoritativeTransformFrame(prior, frame)) continue;
      if (
        prior !== null
        && authoritativeInterpolationDurationMs(prior, frame) > 0
        && sameAuthoritativeTransformPose(prior, frame)
      ) {
        previous.current.set(linkName, frame);
        continue;
      }
      const transition = interpolationDurationMs > 0
        ? createAuthoritativeTransformTransition(
          prior,
          frame,
          group.position,
          group.quaternion,
          performance.now(),
        )
        : null;
      previous.current.set(linkName, frame);
      if (transition === null) {
        const orientation = normalizeQuaternion(frame.orientationXyzw);
        group.position.set(...frame.positionM);
        group.quaternion.set(...orientation);
        active.current.delete(linkName);
      } else {
        // Old main's proven robot presentation used one fixed 50 ms hardware
        // transition. Source timestamps still decide eligibility; the fixed
        // visual window avoids turning transport jitter into varying speed.
        transition.durationMs = interpolationDurationMs;
        active.current.set(linkName, transition);
      }
    }
    return true;
  }, [interpolationDurationMs, linkGroups, manifestModelKey]);

  const acceptLatestLivePresentation = useCallback((wakeAfterApply = true) => {
    const current = livePresentation?.readMeasuredRobotPresentation() ?? null;
    if (current === null) return false;
    const accepted = acceptPresentation(current);
    if (!accepted) return false;
    acceptedLiveFrame.current = true;
    pendingAppliedPresentation.current = { presentation: current, reader: livePresentation! };
    // Publish the presentation to the render callback before waking it.  An
    // invalidate may be serviced immediately by an already-running Canvas;
    // waking first can therefore render a pose without returning its credit.
    if (wakeAfterApply) wake();
    return accepted;
  }, [acceptPresentation, livePresentation, wake]);

  const applyLatestFrame = useCallback(() => {
    acceptLatestLivePresentation(false);
    for (const [linkName, transition] of active.current) {
      const group = linkGroups.get(linkName)?.current ?? null;
      if (group === null) continue;
      group.position.copy(transition.toPosition);
      group.quaternion.copy(transition.toQuaternion);
    }
    active.current.clear();
    wake();
  }, [acceptLatestLivePresentation, linkGroups, wake]);

  useLayoutEffect(() => {
    previous.current.clear();
    active.current.clear();
    acceptedLiveFrame.current = false;
    pendingAppliedPresentation.current = null;
    lastAppliedCreditAtMs.current = null;
    if (appliedAfterRenderTimer.current !== null) {
      window.clearTimeout(appliedAfterRenderTimer.current);
      appliedAfterRenderTimer.current = null;
    }
  }, [linkGroups, manifestModelKey]);

  useLayoutEffect(() => {
    // The slower structural/runtime projection is bootstrap-only after the
    // dedicated observation stream has supplied its first exact frame.
    if (!acceptLatestLivePresentation() && !acceptedLiveFrame.current) {
      // With a live stream, the stored presentation is only the pose the
      // links were mounted with. Never re-apply it over live frames: a stored
      // pose can be older than what the robot is doing now (sim and real
      // lanes can move the robot without any control-state change). Keep the
      // current pose until the next usable live frame.
      if (livePresentation === undefined && acceptPresentation(presentation)) wake();
    }
  }, [acceptLatestLivePresentation, acceptPresentation, livePresentation, presentation, wake]);

  useLayoutEffect(() => {
    if (livePresentation === undefined) return undefined;
    const apply = () => { acceptLatestLivePresentation(); };
    apply();
    return livePresentation.subscribeMeasuredRobotPresentation(apply);
  }, [acceptLatestLivePresentation, livePresentation]);

  useLayoutEffect(() => {
    latestFrameAppliers.add(applyLatestFrame);
    return () => {
      latestFrameAppliers.delete(applyLatestFrame);
    };
  }, [applyLatestFrame, latestFrameAppliers]);

  useEffect(() => () => {
    if (continuationTimer.current !== null) {
      window.clearTimeout(continuationTimer.current);
      continuationTimer.current = null;
    }
    if (appliedAfterRenderTimer.current !== null) {
      window.clearTimeout(appliedAfterRenderTimer.current);
      appliedAfterRenderTimer.current = null;
    }
  }, []);

  useFrame(() => {
    if (active.current.size !== 0) {
      const nowMs = performance.now();
      let pending = false;
      for (const [linkName, transition] of active.current) {
        const group = linkGroups.get(linkName)?.current ?? null;
        if (group === null) {
          active.current.delete(linkName);
          continue;
        }
        if (applyAuthoritativeTransformTransition(
          transition,
          nowMs,
          group.position,
          group.quaternion,
        )) active.current.delete(linkName);
        else pending = true;
      }
      if (pending && continuationTimer.current === null) {
        continuationTimer.current = window.setTimeout(() => {
          continuationTimer.current = null;
          invalidate();
        }, 1_000 / 60);
      }
    }
    if (
      active.current.size === 0
      && pendingAppliedPresentation.current !== null
      && appliedAfterRenderTimer.current === null
    ) {
      // React Three Fiber invokes useFrame callbacks before gl.render().  A
      // task therefore runs only after this admitted pose has completed its
      // Canvas render. Bound the next credit as well as waiting for rendering:
      // an applied-credit loop that runs at the renderer's absolute maximum
      // leaves no browser-thread headroom for heartbeat or control messages.
      const previousCreditAtMs = lastAppliedCreditAtMs.current;
      // Pin both values from this render. A model/descriptor refresh can
      // replace the reader before the after-render task returns its credit.
      const rendered = pendingAppliedPresentation.current;
      const delayMs = previousCreditAtMs === null
        ? 0
        : Math.max(
          0,
          minimumCreditIntervalMs - (performance.now() - previousCreditAtMs),
        );
      appliedAfterRenderTimer.current = window.setTimeout(() => {
        appliedAfterRenderTimer.current = null;
        if (pendingAppliedPresentation.current === rendered) {
          pendingAppliedPresentation.current = null;
        }
        lastAppliedCreditAtMs.current = performance.now();
        rendered.reader.markMeasuredRobotPresentationApplied(rendered.presentation);
        if (pendingAppliedPresentation.current !== null) wake();
      }, delayMs);
    }
  }, -1_000);

  return null;
}

function MeasuredRuntimeLink({
  link,
  initialTransform,
  groupRef,
  attachedObjects,
  ownerIdentity,
  collisionsVisible,
  wireframe,
  frameAxesVisible,
  frameAxesSizeM,
  objectAxesSizeM,
  selectedSceneResourceId,
  objectAxesVisibleIds,
  onSelect,
  onSelectSceneResource,
  onAssetError,
}: {
  link: SceneUrdfManifest['links'][number];
  initialTransform: {
    position_m: Vector3Tuple;
    orientation_xyzw: QuaternionXyzwTuple;
  };
  groupRef: { current: THREE.Group | null };
  attachedObjects: AttachedObjectPresentation[];
  ownerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
  collisionsVisible: boolean;
  wireframe: boolean;
  frameAxesVisible: boolean;
  frameAxesSizeM: number;
  objectAxesSizeM: number;
  selectedSceneResourceId: string | null;
  objectAxesVisibleIds: ReadonlySet<string>;
  onSelect: (event: ThreeEvent<MouseEvent>, structureName: string) => void;
  onSelectSceneResource: (
    event: ThreeEvent<MouseEvent>,
    sceneResourceId: string,
  ) => void;
  onAssetError: (error: Error) => void;
}) {
  const [bootstrapTransform] = useState(() => ({
    position: [...initialTransform.position_m] as Vector3Tuple,
    quaternion: normalizeQuaternion(initialTransform.orientation_xyzw),
  }));
  // The slower shell pose is mount-only bootstrap. Once mounted, the credited
  // observation driver is the sole writer of measured link transforms;
  // unrelated React commits must never restore an older pose.
  return (
    <group
      ref={groupRef}
      name={`measured:${link.name}`}
      position={bootstrapTransform.position}
      quaternion={bootstrapTransform.quaternion}
      onClick={(event) => onSelect(event, link.name)}
    >
      <group>
        {link.visuals.map((visual) => (
          <RobotVisual
            key={`measured-visual:${link.name}:${visual.index}`}
            visual={visual}
            onAssetError={onAssetError}
          />
        ))}
        {collisionsVisible && link.collisions.map((collision) => (
          <RobotCollision
            key={`measured-collision:${link.name}:${collision.index}`}
            collision={collision}
            onAssetError={onAssetError}
          />
        ))}
        {frameAxesVisible && (
          <axesHelper args={[frameAxesSizeM]} raycast={ignoreViewerRaycast} />
        )}
        {attachedObjects.map((object) => (
          <AttachedLogicalSceneObject
            // A carried pose update advances the attachment revision; keep the
            // mounted object (and its mesh) and update its pose in place.
            key={object.attachmentId}
            presentation={object}
            selected={selectedSceneResourceId === object.object.scene_resource_id}
            axesVisible={objectAxesVisibleIds.has(object.object.scene_resource_id)}
            axesSizeM={objectAxesSizeM}
            wireframe={wireframe}
            ownerIdentity={ownerIdentity}
            onSelect={onSelectSceneResource}
          />
        ))}
      </group>
    </group>
  );
}

export function candidateGhostColor(receiptDigest: string | null, stale: boolean, rejected = false): string {
  return rejected
    ? VIEWPORT_COLORS.previewRejected
    : stale || receiptDigest === null ? VIEWPORT_COLORS.previewPending : VIEWPORT_COLORS.previewCurrent;
}

function CandidateRobotGhost({
  manifest,
  presentation,
  stale,
  rejected = false,
  onAssetError,
}: {
  manifest: SceneUrdfManifest;
  presentation: CandidateRobotPresentation;
  stale: boolean;
  rejected?: boolean;
  onAssetError: (error: Error) => void;
}) {
  const color = candidateGhostColor(presentation.receiptDigest, stale, rejected);
  const opacity = stale ? 0.6 : 0.5;
  return (
    <group name={`candidate-robot:${presentation.receiptDigest ?? 'unvalidated-tool-target'}`}>
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
                          opacity={opacity}
                        />
                      </Suspense>
                    </AssetErrorBoundary>
                  )}
                  {visual.mesh_url === null && visual.primitive !== null && (
                    <PrimitiveOverlay
                      primitive={visual.primitive}
                      color={color}
                      opacity={opacity}
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

/**
 * The measured comparison robot follows the viewer stream's measured rows
 * through the same frame driver as the solid robot: links move through refs,
 * never a React render per frame. It mounts on the first measured
 * presentation and is hidden while the latest frame has none.
 */
function MeasuredRobotStateGhost({
  manifest,
  reader,
  latestFrameAppliers,
  interpolationDurationMs,
  onAssetError,
}: {
  manifest: SceneUrdfManifest;
  reader: MeasuredRobotPresentationReader;
  latestFrameAppliers: Set<() => void>;
  interpolationDurationMs: number;
  onAssetError: (error: Error) => void;
}) {
  const [initial, setInitial] = useState(() => reader.readMeasuredRobotPresentation());
  const root = useRef<THREE.Group | null>(null);
  const invalidate = useThree((state) => state.invalidate);
  const linkGroups = useMemo(() => new Map(
    manifest.links.map((link) => [link.name, { current: null as THREE.Group | null }]),
  ), [manifest]);
  useLayoutEffect(() => {
    const update = () => {
      const current = reader.readMeasuredRobotPresentation();
      if (initial === null) {
        // One React render for the first measured pose only.
        if (current !== null) setInitial(current);
        return;
      }
      const group = root.current;
      if (group !== null && group.visible !== (current !== null)) {
        group.visible = current !== null;
        invalidate();
      }
    };
    update();
    return reader.subscribeMeasuredRobotPresentation(update);
  }, [initial, invalidate, reader]);
  // A comparison robot is one coherent retained-model pose. Hide it entirely
  // if a projection ever supplies only a subset of the model links.
  if (initial === null
    || !manifest.links.every((link) => initial.linkTransforms[link.name] !== undefined)) {
    return null;
  }
  const color = VIEWPORT_COLORS.objectComparison;
  const opacity = 0.35;
  return (
    <group ref={root} name={`measured-state-robot:${initial.objectId}:${initial.sourceId}`}>
      <MeasuredRobotFrameDriver
        manifestModelKey={manifest.model_key}
        linkGroups={linkGroups}
        presentation={initial}
        livePresentation={reader}
        latestFrameAppliers={latestFrameAppliers}
        interpolationDurationMs={interpolationDurationMs}
        minimumCreditIntervalMs={0}
      />
      {manifest.links.map((link) => {
        const transform = initial.linkTransforms[link.name];
        if (transform === undefined) return null;
        return (
          <group
            key={`comparison:${manifest.model_key}:${link.name}`}
            ref={linkGroups.get(link.name)!}
            name={`measured-state-link:${link.name}`}
            position={transform.position_m}
            quaternion={normalizeQuaternion(transform.orientation_xyzw)}
            raycast={ignoreViewerRaycast}
          >
            {link.visuals.map((visual) => {
              const origin = visual.visual_origin;
              return (
                <group
                  key={`comparison-visual:${link.name}:${visual.index}`}
                  position={origin.position_m}
                  quaternion={normalizeQuaternion(origin.orientation_xyzw)}
                  raycast={ignoreViewerRaycast}
                >
                  {visual.mesh_url !== null && (
                    <AssetErrorBoundary resetKey={visual.mesh_url} onError={onAssetError}>
                      <Suspense fallback={null}>
                        <OverlayVisualMesh
                          url={visual.mesh_url}
                          color={color}
                          opacity={opacity}
                        />
                      </Suspense>
                    </AssetErrorBoundary>
                  )}
                  {visual.mesh_url === null && visual.primitive !== null && (
                    <PrimitiveOverlay
                      primitive={visual.primitive}
                      color={color}
                      opacity={opacity}
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

function MeasuredObjectStateGhost({
  object,
  presentation,
  ownerIdentity,
}: {
  object: LogicalSceneObject;
  presentation: NonNullable<RuntimeScenePresentation['measuredObjects'][number]>;
  ownerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
}) {
  const { position_m: position, orientation_xyzw: orientation } = presentation.pose;
  return (
    <group
      name={`measured-state-object:${object.scene_resource_id}:${presentation.sourceId}`}
      position={position}
      quaternion={normalizeQuaternion(orientation)}
      raycast={ignoreViewerRaycast}
    >
      {object.collision_shapes.map((shape, index) => shape.type === 'mesh'
        ? ownerIdentity === undefined ? null : (
            <SceneObjectMesh
              key={`measured-state:${object.scene_resource_id}:${index}`}
              shape={shape}
              ownerIdentity={ownerIdentity}
              appearance="comparison"
              selected={false}
              wireframe={false}
            />
          )
        : (
            <SceneObjectPrimitive
              key={`measured-state:${object.scene_resource_id}:${index}`}
              shape={shape}
              appearance="comparison"
              selected={false}
              wireframe={false}
            />
          ))}
    </group>
  );
}

export function MeasuredStateGhostLayers({
  enabled,
  presentation,
  measuredRobot,
  latestFrameAppliers,
  interpolationDurationMs = 0,
  manifest,
  objects,
  ownerIdentity,
  onAssetError,
  children,
}: {
  enabled: boolean;
  presentation: RuntimeScenePresentation | undefined;
  /** Measured comparison robot read from the viewer stream frames. */
  measuredRobot?: MeasuredRobotPresentationReader;
  latestFrameAppliers?: Set<() => void>;
  interpolationDurationMs?: number;
  manifest: SceneUrdfManifest | null;
  objects: readonly LogicalSceneObject[];
  ownerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
  onAssetError: (error: Error) => void;
  children?: ReactNode;
}) {
  if (!enabled || presentation === undefined) return null;
  const objectById = new Map(objects.map((object) => [object.scene_resource_id, object]));
  const robot = manifest !== null && manifest.links.length > 0
    && latestFrameAppliers !== undefined ? measuredRobot ?? null : null;
  const measuredObjects = presentation.measuredObjects.flatMap((measurement) => {
    const object = objectById.get(measurement.objectId);
    // The measured world pose is shown whether the object is fixed or carried,
    // so it can be compared with the attached pose during a carry.
    return object !== undefined
      && object.collision_shapes.some((shape) => shape.type !== 'mesh' || ownerIdentity !== undefined)
      ? [{ measurement, object }]
      : [];
  });
  const measuredTcp = presentation.measuredTcp === null ? null : children ?? null;
  if (robot === null && measuredTcp === null && measuredObjects.length === 0) return null;
  return (
    <group name="measured-state-ghost-layers" renderOrder={20} raycast={ignoreViewerRaycast}>
      {manifest !== null && robot !== null && latestFrameAppliers !== undefined && (
        <MeasuredRobotStateGhost
          key={manifest.model_key}
          manifest={manifest}
          reader={robot}
          latestFrameAppliers={latestFrameAppliers}
          interpolationDurationMs={interpolationDurationMs}
          onAssetError={onAssetError}
        />
      )}
      {measuredTcp}
      {measuredObjects.map(({ measurement, object }) => {
        return (
          <MeasuredObjectStateGhost
            key={`${measurement.objectId}:${measurement.policyId}:${measurement.sourceEpoch}:${measurement.sourceId}`}
            object={object}
            presentation={measurement}
            ownerIdentity={ownerIdentity}
          />
        );
      })}
    </group>
  );
}

export function MeasuredStateGhostToggle({
  enabled,
  onChange,
}: {
  enabled: boolean;
  onChange: (enabled: boolean) => void;
}) {
  return (
    <label className="toggle-control">
      <input
        type="checkbox"
        aria-label="Measured state ghosts"
        checked={enabled}
        onChange={(event) => onChange(event.target.checked)}
      />
      <span>Measured state ghosts</span>
    </label>
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

function SceneObjectPrimitive({
  shape,
  appearance,
  selected,
  wireframe,
}: {
  shape: Exclude<LogicalSceneShape, { type: 'mesh' }>;
  appearance: SceneObjectAppearance;
  selected: boolean;
  wireframe: boolean;
}) {
  const material = sceneObjectMaterial(appearance, selected);
  return (
    <mesh
      rotation={shape.type === 'cylinder' ? [Math.PI / 2, 0, 0] : undefined}
      renderOrder={material.renderOrder}
    >
      <LogicalShapeGeometry shape={shape} />
      <meshStandardMaterial
        color={material.color}
        emissive={material.emissive}
        transparent={material.transparent || wireframe}
        opacity={material.opacity}
        wireframe={wireframe}
        depthWrite={!material.transparent && !wireframe}
      />
    </mesh>
  );
}

type SceneTransformIdentity = { ownerEpoch: string; generation: number };
type SceneTransformCommit = (
  sceneResourceId: string,
  pose: LogicalScenePose,
  scale: [number, number, number],
  expectedIdentity: SceneTransformIdentity,
) => Promise<boolean>;

const ignoreSceneTransformCommit: SceneTransformCommit = async () => false;

function EditableLogicalSceneObject({
  object,
  selected,
  gizmoSettings,
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
  gizmoSettings: GizmoSettings;
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
        <AuthoringTransformControls
          object={transformTarget}
          settings={gizmoSettings}
          contextKey={JSON.stringify([transformIdentity, sourceKey])}
          onBegin={() => {
            dragging.current = true;
            onTransformDraggingChange(true);
            dragTransformIdentity.current = transformIdentity;
            return true;
          }}
          onCommit={() => { void commit(); }}
          onCancel={() => {
            dragging.current = false;
            onTransformDraggingChange(false);
            latestRestoreOwnerTransform.current();
          }}
        />
      )}
    </>
  );
}

function LogicalSceneObjects({
  objects,
  appearance,
  selectedSceneResourceId,
  onSelect,
  axesSizeM,
  wireframe,
  ownerIdentity,
  draftIdentity,
  transformIdentity,
  objectAxesVisibleIds,
  gizmoSettings,
  transformEnabled,
  onCommitTransform,
  onTransformDraggingChange,
  onContextPointerDown,
}: {
  objects: readonly LogicalSceneObject[];
  appearance: Extract<SceneObjectAppearance, 'authoritative' | 'draft'>;
  selectedSceneResourceId: string | null;
  onSelect: (event: ThreeEvent<MouseEvent>, sceneResourceId: string) => void;
  axesSizeM: number;
  wireframe: boolean;
  ownerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
  draftIdentity: StructuralViewerProps['programDraftIdentity'];
  transformIdentity: SceneTransformIdentity | undefined;
  objectAxesVisibleIds: ReadonlySet<string>;
  gizmoSettings: GizmoSettings;
  transformEnabled: boolean;
  onCommitTransform: SceneTransformCommit;
  onTransformDraggingChange: (dragging: boolean) => void;
  onContextPointerDown: (
    event: ThreeEvent<PointerEvent>,
    sceneResourceId: string,
  ) => void;
}) {
  return (
    <group name={appearance === 'draft' ? 'program-draft-scene' : 'owner-logical-scene'}>
      {objects.map((object) => {
        if (object.placement.kind !== 'cell_fixed') return null;
        const selected = selectedSceneResourceId === object.scene_resource_id;
        return (
          <EditableLogicalSceneObject
            key={object.scene_resource_id}
            object={object as LogicalSceneObject & { placement: Extract<LogicalSceneObject['placement'], { kind: 'cell_fixed' }> }}
            selected={selected}
            gizmoSettings={gizmoSettings}
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
                    appearance={appearance}
                    selected={selected}
                    wireframe={wireframe}
                  />
                )
              : (
                  <SceneObjectPrimitive
                    // Three cylinders use Y as their long axis; scene shapes use Z.
                    key={`${object.scene_resource_id}:${index}`}
                    shape={shape}
                    appearance={appearance}
                    selected={selected}
                    wireframe={wireframe}
                  />
                ))}
            {(selected || objectAxesVisibleIds.has(object.scene_resource_id))
              && <axesHelper args={[axesSizeM]} raycast={ignoreViewerRaycast} />}
          </EditableLogicalSceneObject>
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
  gizmoSettings,
  contextKey,
  onBegin,
  manifest,
  robot,
  tool,
  rejected,
  onAssetError,
  onEdit,
  onTransformDraggingChange,
}: {
  pose: Pose3Value;
  stale: boolean;
  axesSizeM: number;
  editable: boolean;
  gizmoSettings: GizmoSettings;
  contextKey: string;
  onBegin: () => boolean;
  manifest: SceneUrdfManifest | null;
  robot: CandidateRobotPresentation | null;
  tool: CandidateRobotPresentation | null;
  rejected: boolean;
  onAssetError: (error: Error) => void;
  onEdit: (pose: Pose3Value | null, noChange?: GizmoNoChange) => void;
  onTransformDraggingChange: (dragging: boolean) => void;
}) {
  const group = useRef<THREE.Group | null>(null);
  const [target, setTarget] = useState<THREE.Group | null>(null);
  const dragging = useRef(false);
  const [dragVisible, setDragVisible] = useState(false);
  const toolOrigin = useMemo(() => new THREE.Matrix4().compose(
    new THREE.Vector3(pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m),
    new THREE.Quaternion(pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w).normalize(),
    new THREE.Vector3(1, 1, 1),
  ).invert(), [pose]);
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
    setDragVisible(false);
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
            color={candidateGhostColor(robot?.receiptDigest ?? null, stale || dragVisible, !dragVisible && rejected)}
            emissive={dragVisible || robot === null ? VIEWPORT_COLORS.previewTargetEmissive : VIEWPORT_COLORS.previewSolvedEmissive}
            transparent
            opacity={0.82}
            depthWrite={false}
          />
        </mesh>
        <axesHelper args={[axesSizeM]} raycast={ignoreViewerRaycast} />
        {manifest !== null && tool !== null && (dragVisible || robot === null) && (
          <group name="candidate-tool-at-target" matrix={toolOrigin} matrixAutoUpdate={false}>
            <CandidateRobotGhost manifest={manifest} presentation={tool}
              stale={dragVisible || !rejected} rejected={!dragVisible && rejected} onAssetError={onAssetError} />
          </group>
        )}
      </group>
      {manifest !== null && robot !== null && !dragVisible && (
        <CandidateRobotGhost manifest={manifest} presentation={robot} stale={stale} onAssetError={onAssetError} />
      )}
      {editable && target !== null && (
        <AuthoringTransformControls
          object={target}
          settings={gizmoSettings.mode === 'scale' ? { ...gizmoSettings, mode: 'translate' } : gizmoSettings}
          contextKey={contextKey}
          onBegin={() => {
            if (!onBegin()) return false;
            dragging.current = true;
            setDragVisible(true);
            onTransformDraggingChange(true);
            return true;
          }}
          onCommit={commit}
          onCancel={(noChange) => {
            dragging.current = false;
            setDragVisible(false);
            restore();
            onEdit(null, noChange);
            onTransformDraggingChange(false);
          }}
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
          color={VIEWPORT_COLORS.frameTarget}
          emissive={VIEWPORT_COLORS.frameTargetEmissive}
          transparent={false}
          opacity={1}
          depthWrite
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
        color={frame.colors === null ? VIEWPORT_COLORS.pointCloud : VIEWPORT_COLORS.pointCloudVertexTint}
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
      || object.type === 'AxesHelper'
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

function clipCameraToBounds(
  camera: THREE.PerspectiveCamera,
  bounds: THREE.Box3,
): void {
  const sphere = bounds.getBoundingSphere(new THREE.Sphere());
  const distance = camera.position.distanceTo(sphere.center);
  camera.near = Math.max(0.001, distance / 1_000);
  camera.far = Math.max(100, distance + sphere.radius * 20);
  camera.updateProjectionMatrix();
}

function CameraCommand({
  request,
  contentRevision,
  defaultPresentation,
  onPresentation,
}: {
  request: CameraCommandRequest;
  contentRevision: string;
  defaultPresentation: RetainedCameraPresentation;
  onPresentation: (presentation: RetainedCameraPresentation) => void;
}) {
  const { camera, controls, scene, invalidate } = useThree();
  const processed = useRef(0);
  useEffect(() => {
    if (request.id === 0 || processed.current === request.id) return;
    const frameContent = scene.getObjectByName('viewer-frame-content');
    const perspective = camera as THREE.PerspectiveCamera;
    if (request.kind === 'reset') {
      camera.position.fromArray(defaultPresentation.camera.position);
      const orbit = controls as { target?: THREE.Vector3; update?: () => void } | null;
      orbit?.target?.fromArray(defaultPresentation.camera.target);
      if (frameContent !== undefined) {
        const contentBounds = frameableContentBounds(frameContent);
        if (!contentBounds.isEmpty()) {
          clipCameraToBounds(perspective, contentBounds);
        }
      }
      orbit?.update?.();
      onPresentation(defaultPresentation);
      invalidate();
      processed.current = request.id;
      return;
    }
    let framedObject: THREE.Object3D | undefined;
    if (request.kind === 'frame_selection' && request.target !== null) {
      framedObject = request.target.kind === 'scene_resource'
        ? scene.getObjectByName(`scene:${request.target.id}`)
          ?? scene.getObjectByName(`observed-scene:${request.target.id}`)
        : scene.getObjectByName(`measured:${request.target.id}`)
          ?? scene.getObjectByName(`link:${request.target.id}`);
      if (framedObject === undefined) return;
    }
    const framedBounds = framedObject === undefined
      ? frameContent === undefined ? null : frameableContentBounds(frameContent)
      : frameableContentBounds(framedObject);
    if (framedBounds === null || framedBounds.isEmpty()) return;
    const orbit = controls as { target?: THREE.Vector3; update?: () => void } | null;
    const currentTarget = orbit?.target ?? new THREE.Vector3(...defaultPresentation.camera.target);
    const presentation = framedCameraPresentation(
      framedBounds,
      {
        position: [camera.position.x, camera.position.y, camera.position.z],
        target: [currentTarget.x, currentTarget.y, currentTarget.z],
      },
      perspective.fov,
      perspective.aspect,
    );
    camera.position.fromArray(presentation.camera.position);
    orbit?.target?.fromArray(presentation.camera.target);
    clipCameraToBounds(perspective, framedBounds);
    orbit?.update?.();
    onPresentation(presentation);
    invalidate();
    processed.current = request.id;
  }, [
    camera,
    contentRevision,
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

/**
 * Commits only together with the whole model subtree of its Suspense
 * boundary: its effect means that load finished, after any mesh boundary in
 * the same commit has already reported its error.
 */
function ModelLoadedSignal({ generation, onLoaded }: {
  generation: number;
  onLoaded: (generation: number) => void;
}) {
  useEffect(() => {
    onLoaded(generation);
  }, [generation, onLoaded]);
  return null;
}

/** Viewer link name -> model instance. Keyed by content, so an equal but
 * new frame list keeps the same map. */
function useOutlineComponents(
  frameAxes: NonNullable<StructuralViewerProps['frameAxes']>,
): ReadonlyMap<string, string> {
  const key = frameAxes.flatMap((row) => (
    row.modelInstanceId === null ? [] : [`${row.linkName}\u0000${row.modelInstanceId}`]
  )).join('\n');
  return useMemo(() => new Map(key
    .split('\n')
    .filter((line) => line !== '')
    .map((line) => line.split('\u0000') as [string, string])), [key]);
}

const UNINSPECTED_RENDERER: ViewerRendererInspection = {
  vendor: null,
  renderer: null,
  software: false,
};

const ViewerCanvas = memo(function ViewerCanvas({
  manifest,
  collisionsVisible,
  wireframeVisible,
  outlineVisible,
  outlineComponents,
  retryGeneration,
  onModelLoaded,
  onAssetError,
  onIsolatedAssetError,
  onPreviewAssetError,
  sceneObjects,
  programDraftPreview,
  selectedSceneResourceId,
  onSelectSceneResource,
  selectedStructureName,
  onSelectStructure,
  candidateTcpPose,
  candidateStale,
  candidateTcpEditable,
  onEditCandidateTcpPose,
  onBeginCandidateTcpDrag,
  candidateContextKey,
  gizmoSettings,
  candidateRobot,
  candidateTool,
  candidateRejected,
  correctionRobot,
  correctionStale,
  runtimePresentation,
  measuredStateGhostPresentation,
  measuredStateGhostRobot,
  measuredStateGhostObjects,
  measuredStateGhostsEnabled,
  executionObservationPresentation,
  sceneOwnerIdentity,
  programDraftIdentity,
  viewerFrameIdentity,
  readViewerFrameIdentity,
  pointCloudFrame,
  viewer,
  frameAxes,
  objectAxesVisibleIds,
  cameraPresentation,
  cameraContentRevision,
  onModelMaterialized,
  onViewerCommit,
  transformEnabled,
  objectEditorActive,
  robotEditorActive,
  transformIdentity,
  onCommitSceneTransform,
  onSceneContext,
  cameraCommand,
  graphics,
  onRendererKind,
}: {
  manifest: SceneUrdfManifest | null;
  collisionsVisible: boolean;
  wireframeVisible: boolean;
  outlineVisible: boolean;
  /** Installation graphics profiles; this browser uses the one it renders with. */
  graphics: ComposerSettings['viewerGraphics'];
  /** Reports whether this browser renders with the GPU or in software. */
  onRendererKind?: (kind: ViewerRendererKind) => void;
  /** Viewer link name -> model instance, for one outline per component. */
  outlineComponents: ReadonlyMap<string, string>;
  /** Incremented by Retry/Reload: remounts the model subtree to load again. */
  retryGeneration: number;
  onModelLoaded: (generation: number) => void;
  onAssetError: (error: Error) => void;
  onIsolatedAssetError: (error: Error) => void;
  onPreviewAssetError: (error: Error) => void;
  sceneObjects: readonly LogicalSceneObject[];
  programDraftPreview: boolean;
  selectedSceneResourceId: string | null;
  onSelectSceneResource: (sceneResourceId: string | null) => void;
  selectedStructureName: string | null;
  onSelectStructure: (structureName: string | null) => void;
  candidateTcpPose: Pose3Value | null;
  candidateStale: boolean;
  candidateTcpEditable: boolean;
  onEditCandidateTcpPose: (pose: Pose3Value | null, noChange?: GizmoNoChange) => void;
  onBeginCandidateTcpDrag: () => boolean;
  candidateContextKey: string;
  gizmoSettings: GizmoSettings;
  candidateRobot: CandidateRobotPresentation | null;
  candidateTool: CandidateRobotPresentation | null;
  candidateRejected: boolean;
  correctionRobot: CandidateRobotPresentation | null;
  correctionStale: boolean;
  runtimePresentation: RuntimeScenePresentation | undefined;
  measuredStateGhostPresentation: RuntimeScenePresentation | undefined;
  measuredStateGhostRobot: MeasuredRobotPresentationReader | undefined;
  measuredStateGhostObjects: readonly LogicalSceneObject[];
  measuredStateGhostsEnabled: boolean;
  executionObservationPresentation: MeasuredRobotPresentationReader | undefined;
  sceneOwnerIdentity: StructuralViewerProps['sceneOwnerIdentity'];
  programDraftIdentity: StructuralViewerProps['programDraftIdentity'];
  viewerFrameIdentity: StructuralViewerProps['viewerFrameIdentity'];
  readViewerFrameIdentity: StructuralViewerProps['readViewerFrameIdentity'];
  pointCloudFrame: PointCloudFrame | null;
  viewer: ProgramViewerMetadata | undefined;
  frameAxes: NonNullable<StructuralViewerProps['frameAxes']>;
  objectAxesVisibleIds: ReadonlySet<string>;
  cameraPresentation: RetainedCameraPresentationStore;
  cameraContentRevision: string;
  onModelMaterialized: (durationMs: number) => void;
  onViewerCommit: (
    id: string,
    phase: 'mount' | 'update' | 'nested-update',
    actualDuration: number,
  ) => void;
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
  // Seeded from the page-level inspection so every mount is created with
  // its final context settings; onCreated never flips the Canvas key.
  const [inspection, setInspection] = useState<ViewerRendererInspection>(
    () => pageViewerRendererInspection() ?? UNINSPECTED_RENDERER,
  );
  const graphicsProfile = graphics[inspection.software ? 'software' : 'gpu'];
  const renderer = useMemo(() => ({
    ...inspection,
    ...selectViewerPresentationProfile(devicePixelRatio, graphicsProfile),
  }), [devicePixelRatio, graphicsProfile, inspection]);
  useEffect(() => {
    if (inspection.renderer !== null) setViewerQualificationRenderer(renderer);
  }, [inspection.renderer, renderer]);
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
      // Only antialiasing is fixed when the WebGL context is created. The
      // renderer kind is known before mount (page inspection), so the key does
      // not depend on anything discovered in onCreated.
      key={renderer.antialias ? 'aa' : 'no-aa'}
      frameloop="demand"
      dpr={renderer.effectiveDpr}
      gl={{
        antialias: renderer.antialias,
        alpha: false,
        powerPreference: 'high-performance',
      }}
      onCreated={({ gl }) => {
        if (qualificationEnabled && materializationStartedAtMs.current === null) {
          materializationStartedAtMs.current = performance.now();
        }
        const next = rememberViewerRendererInspection(inspectViewerRenderer(gl));
        setInspection(next);
        onRendererKind?.(next.software ? 'software' : 'gpu');
      }}
      style={{ background: VIEWPORT_COLORS.background }}
    >
      <ViewerQualificationProfiler
        enabled={qualificationEnabled}
        onRender={onViewerCommit}
      >
      <color attach="background" args={[VIEWPORT_COLORS.background]} />
      <PerspectiveCamera
        makeDefault
        up={[0, 0, 1]}
        position={selectedCamera.position}
        fov={48}
        near={0.005}
        far={250}
      />
      <ambientLight intensity={0.9} />
      <hemisphereLight args={[VIEWPORT_COLORS.hemisphereSky, VIEWPORT_COLORS.hemisphereGround, 0.75]} />
      <directionalLight position={[5, -4, 9]} intensity={1.45} />
      <directionalLight position={[-4, 5, 5]} intensity={0.55} />
      <Grid
        args={[12, 12]}
        cellSize={0.1}
        cellThickness={0.35}
        cellColor={VIEWPORT_COLORS.gridCell}
        sectionSize={1}
        sectionThickness={0.85}
        sectionColor={VIEWPORT_COLORS.gridSection}
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
        enableDamping={graphicsProfile.cameraDamping}
        dampingFactor={0.1}
        target={selectedCamera.target}
      />
      <CaptureCameraPresentation store={cameraPresentation} />
      <CameraCommand
        request={cameraCommand}
        contentRevision={cameraContentRevision}
        defaultPresentation={{
          camera: {
            position: [...DEFAULT_VIEWER_CAMERA.position],
            target: [...DEFAULT_VIEWER_CAMERA.target],
          },
        }}
        onPresentation={captureCameraPresentation}
      />
      {outlineVisible && (
        <SceneOutline multisample={graphicsProfile.outlineSmoothing} componentByLink={outlineComponents} />
      )}
      {/* SceneOutline outlines this group's solid meshes by name. */}
      <group name="viewer-frame-content">
        {manifest !== null && (
          <AssetErrorBoundary
            resetKey={`${manifest.model_key}:${retryGeneration}`}
            onError={onAssetError}
          >
            <Suspense key={retryGeneration} fallback={null}>
              <ModelLoadedSignal generation={retryGeneration} onLoaded={onModelLoaded} />
              {runtimePresentation?.measuredRobot === null
                || runtimePresentation?.measuredRobot === undefined ? (
                  <RetainedNeutralModel
                    manifest={manifest}
                    collisionsVisible={collisionsVisible}
                    materializationStartedAtMs={materializationStartedAtMs}
                    onMaterialized={onModelMaterialized}
                    frameAxesByLink={frameAxesByLink}
                    frameAxesSizeM={viewer?.frame_gizmo_size_m ?? 0.1}
                    showFrameAxes
                    selectedStructureName={selectedStructureName}
                    onSelect={selectStructure}
                    onAssetError={onIsolatedAssetError}
                  />
                ) : (
                  <MeasuredRuntimeModel
                    manifest={manifest}
                    presentation={runtimePresentation.measuredRobot}
                    livePresentation={executionObservationPresentation}
                    attachedObjects={runtimePresentation.attachedObjects}
                    collisionsVisible={collisionsVisible}
                    wireframe={wireframeVisible}
                    materializationStartedAtMs={materializationStartedAtMs}
                    onMaterialized={onModelMaterialized}
                    ownerIdentity={sceneOwnerIdentity}
                    latestFrameAppliers={latestFrameAppliers}
                    frameAxesByLink={frameAxesByLink}
                    frameAxesSizeM={viewer?.frame_gizmo_size_m ?? 0.1}
                    objectAxesSizeM={viewer?.object_axes_gizmo_size_m ?? 0.1}
                    selectedStructureName={selectedStructureName}
                    selectedSceneResourceId={selectedSceneResourceId}
                    objectAxesVisibleIds={objectAxesVisibleIds}
                    onSelect={selectStructure}
                    onSelectSceneResource={selectSceneResource}
                    onAssetError={onIsolatedAssetError}
                    poseRateHz={graphicsProfile.poseRateHz}
                    motionSmoothing={graphicsProfile.motionSmoothing}
                  />
                )}
            </Suspense>
          </AssetErrorBoundary>
        )}
        <LogicalSceneObjects
          key={`logical-scene:${retryGeneration}`}
          objects={sceneObjects}
          appearance={programDraftPreview ? 'draft' : 'authoritative'}
          selectedSceneResourceId={selectedSceneResourceId}
          onSelect={selectSceneResource}
          axesSizeM={viewer?.object_axes_gizmo_size_m ?? 0.1}
          wireframe={wireframeVisible}
          ownerIdentity={sceneOwnerIdentity}
          draftIdentity={programDraftIdentity}
          transformIdentity={transformIdentity}
          objectAxesVisibleIds={objectAxesVisibleIds}
          gizmoSettings={gizmoSettings}
          transformEnabled={transformEnabled && objectEditorActive}
          onCommitTransform={onCommitSceneTransform}
          onTransformDraggingChange={setTransformDragging}
          onContextPointerDown={registerContextPointer}
        />

        <MeasuredStateGhostLayers
          enabled={measuredStateGhostsEnabled}
          presentation={measuredStateGhostPresentation}
          measuredRobot={measuredStateGhostRobot}
          latestFrameAppliers={latestFrameAppliers}
          interpolationDurationMs={graphicsProfile.motionSmoothing
            ? MAX_AUTHORITATIVE_INTERPOLATION_MS : 0}
          manifest={manifest}
          objects={measuredStateGhostObjects}
          ownerIdentity={sceneOwnerIdentity}
          onAssetError={onPreviewAssetError}
        >
          {measuredStateGhostPresentation?.measuredTcp !== null
            && measuredStateGhostPresentation?.measuredTcp !== undefined && (
            <MeasuredTcpMarker
              presentation={measuredStateGhostPresentation.measuredTcp}
              axesSizeM={viewer?.frame_gizmo_size_m ?? 0.1}
              ownerIdentity={sceneOwnerIdentity}
              latestFrameAppliers={latestFrameAppliers}
            />
          )}
        </MeasuredStateGhostLayers>
        {candidateTcpPose !== null && robotEditorActive && (
          <CandidateTcpGhost
            pose={candidateTcpPose}
            stale={candidateStale}
            axesSizeM={viewer?.frame_gizmo_size_m ?? 0.1}
            editable={candidateTcpEditable && robotEditorActive}
            gizmoSettings={gizmoSettings}
            contextKey={candidateContextKey}
            onBegin={onBeginCandidateTcpDrag}
            manifest={manifest}
            robot={candidateRobot}
            tool={candidateTool}
            rejected={candidateRejected}
            onAssetError={onPreviewAssetError}
            onEdit={onEditCandidateTcpPose}
            onTransformDraggingChange={setTransformDragging}
          />
        )}
        {manifest !== null && candidateRobot !== null && candidateTcpPose === null && robotEditorActive && (
          <CandidateRobotGhost
            manifest={manifest}
            presentation={candidateRobot}
            stale={candidateStale}
            onAssetError={onPreviewAssetError}
          />
        )}
        {manifest !== null && correctionRobot !== null && (
          <CandidateRobotGhost
            manifest={manifest}
            presentation={correctionRobot}
            stale={correctionStale}
            onAssetError={onPreviewAssetError}
          />
        )}
        {pointCloudFrame !== null && <PointCloudPoints frame={pointCloudFrame} />}
      </group>
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
});

// Stable default: a fresh `[]` per render would defeat the Canvas memo.
const NO_FRAME_AXES: NonNullable<StructuralViewerProps['frameAxes']> = [];

function StructuralViewerContent({
  viewerActive,
  manifest,
  modelPresentation,
  onRetry,
  sceneObjects = [],
  programDraftPreview = false,
  staleScenePreview = false,
  sceneSourceControl,
  programDraftIdentity,
  selectedSceneResourceId = null,
  onSelectSceneResource = () => undefined,
  selectedStructureName = null,
  onSelectStructure = () => undefined,
  programDraftEditIdentity,
  onCommitProgramDraftTransform,
  onDuplicateSceneResource,
  onRemoveSceneResource,
  onEditSceneResource,
  selectionLocked = false,
  sceneMutationAvailability,
  candidateTcpPose = null,
  candidateStale = false,
  candidateTcpEditable = false,
  onEditCandidateTcpPose = () => undefined,
  onBeginCandidateTcpDrag = () => false,
  candidateContextKey = '',
  gizmo: sharedGizmo,
  candidateRobot = null,
  candidateTool = null,
  candidateRejected = false,
  correctionRobot = null,
  correctionStale = false,
  runtimePresentation,
  measuredStateGhostPresentation,
  measuredStateGhostRobot,
  measuredStateGhostObjects = [],
  executionObservationPresentation,
  sceneOwnerIdentity,
  viewerFrameIdentity,
  readViewerFrameIdentity,
  pointCloudCellId,
  viewer,
  frameAxes = NO_FRAME_AXES,
  objectAxesVisibleIds = [],
  objectAxesRows = [],
  onObjectAxesVisibleIdsChange,
  onAxesSizesCommit,
  robotControlSurface,
  robotControlBlocked = false,
  robotMotionActive = false,
  objectControlSurface,
  ioControlSurface,
  transportControls,
  controlPanelSettings,
  programViewerPendingApply,
  runtimeOverlay,
  inspectorTarget = null,
  inspectorMinimized = false,
  onInspectorMinimizedChange,
  onInspectorMaximize,
  onInspectorPanelChange,
}: StructuralViewerProps) {
  // The diagnostic ghost preference is local and independent of motion state
  // or sample availability. Current presentations decide only what can draw.
  const composerSettings = useComposerSettings();
  const [measuredStateGhostsEnabled, setMeasuredStateGhostsEnabled] = useState(
    DEFAULT_MEASURED_STATE_GHOSTS_VISIBLE,
  );
  const [rendererKind, setRendererKind] = useState<ViewerRendererKind | null>(null);
  const {
    collisionGeometry: collisionsVisible,
    wireframeOverlays: wireframeVisible,
    pointCloud: pointCloudVisible,
    objectOutline: outlineVisible,
  } = composerSettings.settings.viewer;
  const updateComposerSettings = composerSettings.update;
  const setViewerSetting = useCallback(
    (key: keyof ComposerSettings['viewer'], value: boolean) => updateComposerSettings(
      (current) => ({ ...current, viewer: { ...current.viewer, [key]: value } }),
    ),
    [updateComposerSettings],
  );
  const [sessionFrameAxes, setSessionFrameAxes] = useState<Set<string>>(
    () => new Set(frameAxes.filter((frame) => (
      frame.defaultVisible && frame.poseAvailable
    )).map((frame) => frame.linkName)),
  );
  const [frameAxesSizeM, setFrameAxesSizeM] = useState(
    viewer?.frame_gizmo_size_m ?? 0.1,
  );
  const [objectAxesSizeM, setObjectAxesSizeM] = useState(
    viewer?.object_axes_gizmo_size_m ?? 0.1,
  );
  const [assetError, setAssetError] = useState<Error | null>(null);
  const [assetWarning, setAssetWarning] = useState<string | null>(null);
  const [previewAssetWarning, setPreviewAssetWarning] = useState<string | null>(null);
  // Retry/Reload keeps the geometry errors until the reload verifiably
  // succeeds: a remounted model that commits with no new error.
  const [geometryRetryGeneration, setGeometryRetryGeneration] = useState(0);
  /** Generation of the reload still being awaited, if any. */
  const [pendingGeometryRetry, setPendingGeometryRetry] = useState<number | null>(null);
  const [loadedGeometryGeneration, setLoadedGeometryGeneration] = useState(0);
  const geometryRetrying = pendingGeometryRetry !== null;
  if (pendingGeometryRetry !== null && loadedGeometryGeneration === pendingGeometryRetry) {
    // That exact reload committed with no new error: the conditions resolve.
    setPendingGeometryRetry(null);
    setAssetError(null);
    setAssetWarning(null);
    setPreviewAssetWarning(null);
  }
  const [materializationGeneration, setMaterializationGeneration] = useState(0);
  const localGizmo = useGizmoSettings();
  const gizmo = sharedGizmo ?? localGizmo;
  const transformMode = gizmo.settings.mode;
  const transformSpace = gizmo.settings.space;
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
  const viewerSettingsPanel = useCardPanelLayout();
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
  const transformCommit = onCommitProgramDraftTransform;
  const transformIdentity = useMemo(() => (
    programDraftEditIdentity === undefined
      ? undefined
      : {
          ownerEpoch: programDraftEditIdentity.draftId,
          generation: programDraftEditIdentity.draftRevision,
        }
  ), [programDraftEditIdentity]);
  const transformBaseEnabled = transformCommit !== undefined
    && transformIdentity !== undefined
    && (selectedSceneResourceId === null || !editLockedObjectIds.has(selectedSceneResourceId))
    && selectedObject?.placement.kind === 'cell_fixed'
    && selectedObject.provider_evidence === null
    && sceneMutationAvailability?.[selectedObject.scene_resource_id]?.transform === true;
  const candidateTransformEnabled = candidateTcpEditable
    && candidateTcpPose !== null
    && inspectorPanel === 'robot';
  const generalTransformEnabled = (inspectorPanel === 'object' && transformBaseEnabled) || candidateTransformEnabled;
  const scaleEnabled = inspectorPanel === 'object' && transformBaseEnabled
    && selectedObjectAvailability?.shape === true
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
  const presentedObjectAxesRows = objectAxesRows.length > 0
    ? objectAxesRows
    : sceneObjects.map((object) => ({
        sceneResourceId: object.scene_resource_id,
        attached: object.placement.kind === 'carried',
        poseAvailable: true,
        persistent: false,
        defaultVisible: false,
        sourceLabel: programDraftPreview ? 'Program Draft' : 'owner scene',
      }));
  const modelIdentity = manifest.status === 'ready' ? manifest.data.model_key : null;
  const defaultFrameAxesKey = frameAxes
    .filter((frame) => frame.defaultVisible && frame.poseAvailable)
    .map((frame) => frame.linkName)
    .sort()
    .join('\u0000');
  const modelAxisRows = (() => {
    if (manifest.status !== 'ready') return [];
    const viewerLinks = new Set(manifest.data.links.map((link) => link.name));
    const viewerJoints = new Map(manifest.data.joints.map((joint) => [joint.name, joint]));
    return frameAxes.map((frame) => {
      const joint = frame.viewerJointName === null
        ? null
        : viewerJoints.get(frame.viewerJointName) ?? null;
      return {
        ...frame,
        poseAvailable: frame.poseAvailable
          && viewerLinks.has(frame.linkName)
          && (frame.viewerJointName === null
            || (joint !== null && joint.child_link === frame.linkName)),
      };
    });
  })();
  useEffect(() => {
    setSessionFrameAxes(new Set(
      defaultFrameAxesKey === '' ? [] : defaultFrameAxesKey.split('\u0000'),
    ));
  }, [defaultFrameAxesKey, modelIdentity]);
  useEffect(() => {
    setFrameAxesSizeM(viewer?.frame_gizmo_size_m ?? 0.1);
    setObjectAxesSizeM(viewer?.object_axes_gizmo_size_m ?? 0.1);
  }, [viewer?.frame_gizmo_size_m, viewer?.object_axes_gizmo_size_m]);
  // A memoized Canvas prop: a fresh array on every parent render would
  // re-render the R3F root, whose configure() then forces a WebGL frame.
  const presentedFrameAxes = useMemo(() => frameAxes
    .filter((frame) => sessionFrameAxes.has(frame.linkName))
    .sort((left, right) => left.sourceLinkName.localeCompare(right.sourceLinkName)),
  [frameAxes, sessionFrameAxes]);
  // One outline per component: every robot link (not only those with axes
  // shown) resolves to its model instance.
  const outlineComponents = useOutlineComponents(frameAxes);
  // Runtime measured state is the primary robot pose. Only the independent,
  // unmerged scene-runtime fact may be rendered as its optional comparison.
  const auxiliaryViewerSourcesActive = programDraftPreview
    || candidateTcpPose !== null
    || candidateRobot !== null;
  const presentedRuntime = runtimePresentation;
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
  // During a manual move the Robot panel is forced open, restored and cannot
  // be closed or replaced until the move ends.
  const robotPanelLocked = robotMotionActive && robotControlSurface !== undefined;
  useEffect(() => {
    if (!robotPanelLocked) return;
    setInspectorPanel('robot');
    if (inspectorMinimized) onInspectorMinimizedChange?.(false);
  }, [inspectorMinimized, onInspectorMinimizedChange, robotPanelLocked]);
  const toggleInspectorPanel = useCallback((panel: Exclude<ViewerInspectorPanel, null>) => {
    if (robotPanelLocked) return;
    if (inspectorPanel === panel) {
      if (inspectorMinimized) onInspectorMinimizedChange?.(false);
      else setInspectorPanel(null);
      return;
    }
    setInspectorPanel(panel);
    onInspectorMinimizedChange?.(false);
  }, [inspectorMinimized, inspectorPanel, onInspectorMinimizedChange, robotPanelLocked]);
  const robotLockTitle = 'A robot move is active: Robot control stays open until it ends.';
  const openViewerSettings = useCallback(() => {
    // A manual move keeps Robot control open (its Stop at hand).
    if (robotPanelLocked) return;
    setInspectorPanel('tools');
    onInspectorMinimizedChange?.(false);
  }, [onInspectorMinimizedChange, robotPanelLocked]);
  useEffect(() => onOpenSettingsRequest('viewer', openViewerSettings), [openViewerSettings]);

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
    setMaterializationGeneration((current) => current + 1);
  }, []);
  const cameraContentRevision = `${modelIdentity ?? 'no-model'}:${materializationGeneration}:${visibleGeometryCount}:${sceneObjects
    .map((object) => object.scene_resource_id)
    .sort()
    .join(',')}`;
  // A new error ends a pending retry as failed; its condition stays.
  const endFailedRetry = useCallback(() => setPendingGeometryRetry(null), []);
  const reportAssetError = useCallback((error: Error) => {
    reportViewerQualificationError(error);
    endFailedRetry();
    setAssetError(error);
  }, [endFailedRetry, setAssetError]);
  const reportIsolatedAssetError = useCallback((error: Error) => {
    reportViewerQualificationError(error);
    endFailedRetry();
    setAssetWarning(error.message || 'One viewer mesh could not be rendered.');
  }, [endFailedRetry, setAssetWarning]);
  const reportPreviewAssetError = useCallback((error: Error) => {
    reportViewerQualificationError(error);
    setPreviewAssetWarning(error.message || 'One preview mesh could not be rendered.');
  }, [setPreviewAssetWarning]);
  const reportModelLoaded = useCallback((generation: number) => {
    setLoadedGeometryGeneration(generation);
  }, []);
  const reportViewerCommit = useCallback((
    _id: string,
    _phase: 'mount' | 'update' | 'nested-update',
    actualDuration: number,
  ) => {
    recordViewerReactCommit(actualDuration);
  }, []);
  const canvasViewer = useMemo(() => ({
    ...viewer,
    frame_gizmo_size_m: frameAxesSizeM,
    object_axes_gizmo_size_m: objectAxesSizeM,
  }), [frameAxesSizeM, objectAxesSizeM, viewer]);
  const canvasSceneContext = useCallback((
    sceneResourceId: string,
    x: number,
    y: number,
  ) => {
    if (selectionLocked && sceneResourceId !== selectedSceneResourceId) return;
    setContextMenu({ sceneResourceId, x, y });
  }, [selectedSceneResourceId, selectionLocked]);

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
      if (key === 'escape') {
        setContextMenu(null);
        onSelectSceneResource(null);
        onSelectStructure(null);
      }
      else if (!generalTransformEnabled) return;
      else if (key === 'w') gizmo.update({ mode: 'translate' });
      else if (key === 'e') gizmo.update({ mode: 'rotate' });
      else if (key === 'r' && scaleEnabled) gizmo.update({ mode: 'scale' });
      else if (key === 'q') gizmo.update({ space: transformSpace === 'world' ? 'local' : 'world' });
      else return;
      event.preventDefault();
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [generalTransformEnabled, gizmo, transformSpace, onSelectSceneResource, onSelectStructure, scaleEnabled, viewerActive]);
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
    const generation = geometryRetryGeneration + 1;
    // Only a mounted model can be reloaded; without one there is nothing to
    // wait for, and the next model mount reports its own result.
    if (manifest.status === 'ready') setPendingGeometryRetry(generation);
    setGeometryRetryGeneration(generation);
    onRetry();
  };
  usePublishWorkbenchNotices('viewer', viewerNotices({
    modelPresentation,
    staleScenePreview,
    robotMeshFailure: assetWarning,
    assetFailure: manifest.status === 'ready' && assetError !== null
      ? {
          kind: classifyStructuralAssetFailure(assetError) === 'stale_model'
            ? 'stale_model'
            : 'unavailable',
          message: assetError.message,
        }
      : null,
    previewMeshFailure: previewAssetWarning,
    pointCloudFailure: pointCloudEnabled ? pointCloud.error : null,
    geometryRetrying,
    retryModel: retry,
    reloadModel: retry,
  }));

  return (
    <section className="panel viewer-panel" aria-labelledby="viewer-title">
      <header className="panel-header viewer-header">
        <strong id="viewer-title" className="sr-only">3D viewer</strong>
        <div className="viewer-header-controls">
          <div className="viewer-tool-strip" aria-label="Scene viewport controls">
            <button
              type="button"
              aria-label="Frame all visible content"
              onClick={() => issueCameraCommand('frame_all')}
              title="Frame all visible content"
            >
              {/* Drawn, like the crosshair, so it is centered regardless of font metrics. */}
              <svg className="viewer-camera-icon" viewBox="0 0 16 16" aria-hidden="true">
                <rect x="2.25" y="2.25" width="11.5" height="11.5" rx="1.5" />
                <path d="M8 2.25v11.5M2.25 8h11.5" />
              </svg>
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
              <svg className="viewer-camera-icon" viewBox="0 0 16 16" aria-hidden="true">
                <path d="M2.7 8.3a5.3 5.3 0 1 0 1.55-3.75" />
                <path d="M4.25 1.75v2.8h2.8" />
              </svg>
            </button>
            {(inspectorPanel === 'object' || inspectorPanel === 'robot') && (
              <>
                <span className="viewer-tool-separator" />
                <button
                  type="button"
                  aria-pressed={transformMode === 'translate' || (inspectorPanel === 'robot' && transformMode === 'scale')}
                  disabled={!generalTransformEnabled}
                  onClick={() => gizmo.update({ mode: 'translate' })}
                  title="Translate target (W)"
                >W</button>
                <button
                  type="button"
                  aria-pressed={transformMode === 'rotate'}
                  disabled={!generalTransformEnabled}
                  onClick={() => gizmo.update({ mode: 'rotate' })}
                  title="Rotate target (E)"
                >E</button>
                <button
                  type="button"
                  aria-pressed={inspectorPanel === 'object' && transformMode === 'scale'}
                  disabled={!scaleEnabled}
                  onClick={() => gizmo.update({ mode: 'scale' })}
                  title="Scale a selected box or mesh object (R)"
                >R</button>
                <button
                  type="button"
                  className="viewer-text-control"
                  disabled={!generalTransformEnabled}
                  onClick={() => gizmo.update({ space: transformSpace === 'world' ? 'local' : 'world' })}
                  title="Toggle local/world coordinates (Q)"
                >{transformSpace === 'world' ? 'World' : 'Local'}</button>
                <button type="button" className="viewer-text-control" aria-pressed={gizmo.settings.snap}
                  disabled={!generalTransformEnabled || (inspectorPanel === 'object' && transformMode === 'scale')}
                  onClick={() => gizmo.update({ snap: !gizmo.settings.snap })}
                  title={`Snap by ${gizmo.settings.linearStepM} m / ${gizmo.settings.angularStepDeg} deg from drag start. Scale is unchanged.`}>Snap</button>
              </>
            )}
          </div>
          {sceneSourceControl}
          <div className="viewer-surface-launchers">
            <button
              type="button"
              aria-pressed={inspectorPanel === 'robot'}
              disabled={robotControlSurface === undefined || robotPanelLocked}
              title={robotPanelLocked ? robotLockTitle : undefined}
              onClick={() => toggleInspectorPanel('robot')}
            >Robot</button>
            <button
              type="button"
              aria-pressed={inspectorPanel === 'object'}
              disabled={robotPanelLocked}
              title={robotPanelLocked ? robotLockTitle : undefined}
              onClick={() => toggleInspectorPanel('object')}
            >Object</button>
            {ioControlSurface !== undefined && (
              <button
                type="button"
                aria-pressed={inspectorPanel === 'io'}
                disabled={robotPanelLocked}
                title={robotPanelLocked ? robotLockTitle : 'Tool and I/O signals'}
                onClick={() => toggleInspectorPanel('io')}
              >Tool &amp; I/O</button>
            )}
          </div>
        </div>
      </header>

      <div
        className="viewer-stage"
        role="img"
        aria-label="Scene authoring viewport"
        data-scene-presentation={programDraftPreview ? 'program-draft' : 'owner'}
        data-scene-object-count={sceneObjects.length}
      >
        {runtimeOverlay}
        {inspectorPanel === 'robot' && candidateTcpPose !== null && candidateTool === null && candidateRobot === null && (
          <div className="viewer-target-status" role="status">
            <span>Target marker only: mapped tool geometry unavailable. Target is unvalidated.</span>
          </div>
        )}
        {viewerActive && (
          visibleGeometryCount > 0
          || viewerFrameIdentity !== undefined
          || pointCloudEnabled
        ) && (
          <ViewerCanvas
              manifest={manifest.status === 'ready' ? manifest.data : null}
              collisionsVisible={collisionsVisible}
              wireframeVisible={wireframeVisible}
              outlineVisible={outlineVisible}
              outlineComponents={outlineComponents}
              graphics={composerSettings.settings.viewerGraphics}
              onRendererKind={setRendererKind}
              retryGeneration={geometryRetryGeneration}
              onModelLoaded={reportModelLoaded}
              onAssetError={reportAssetError}
              onIsolatedAssetError={reportIsolatedAssetError}
              onPreviewAssetError={reportPreviewAssetError}
              sceneObjects={sceneObjects}
              programDraftPreview={programDraftPreview}
              selectedSceneResourceId={selectedSceneResourceId}
              onSelectSceneResource={onSelectSceneResource}
              selectedStructureName={selectedStructureName}
              onSelectStructure={onSelectStructure}
              candidateTcpPose={candidateTcpPose}
              candidateStale={candidateStale}
              candidateTcpEditable={candidateTransformEnabled}
              onEditCandidateTcpPose={onEditCandidateTcpPose}
              onBeginCandidateTcpDrag={onBeginCandidateTcpDrag}
              candidateContextKey={candidateContextKey}
              gizmoSettings={gizmo.settings}
              candidateRobot={candidateRobot}
              candidateTool={candidateTool}
              candidateRejected={candidateRejected}
              correctionRobot={correctionRobot}
              correctionStale={correctionStale}
              runtimePresentation={presentedRuntime}
              measuredStateGhostPresentation={measuredStateGhostPresentation}
              measuredStateGhostRobot={measuredStateGhostRobot}
              measuredStateGhostObjects={measuredStateGhostObjects}
              measuredStateGhostsEnabled={measuredStateGhostsEnabled}
              executionObservationPresentation={executionObservationPresentation}
              sceneOwnerIdentity={sceneOwnerIdentity}
              programDraftIdentity={programDraftIdentity}
              viewerFrameIdentity={viewerFrameIdentity}
              readViewerFrameIdentity={readViewerFrameIdentity}
              pointCloudFrame={pointCloud.frame}
              frameAxes={presentedFrameAxes}
              objectAxesVisibleIds={objectAxesVisible}
              viewer={canvasViewer}
              cameraPresentation={cameraPresentation}
              cameraContentRevision={cameraContentRevision}
              onModelMaterialized={reportModelMaterialized}
              onViewerCommit={reportViewerCommit}
              transformEnabled={transformEnabled}
              objectEditorActive={inspectorPanel === 'object'}
              robotEditorActive={inspectorPanel === 'robot'}
              transformIdentity={transformIdentity}
              onCommitSceneTransform={transformCommit ?? ignoreSceneTransformCommit}
              onSceneContext={canvasSceneContext}
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
              title={onDuplicateSceneResource === undefined
                ? READ_ONLY_SCENE_REASON
                : contextAvailability?.duplicateReason ?? 'Add a copy of this object to the Program Draft.'}
              onClick={() => {
                void onDuplicateSceneResource?.(contextMenu.sceneResourceId);
                setContextMenu(null);
              }}
            >Duplicate object</button>
            <button
              type="button"
              role="menuitem"
              className="danger-button"
              disabled={
                onRemoveSceneResource === undefined
                || contextAvailability?.remove !== true
              }
              title={onRemoveSceneResource === undefined
                ? READ_ONLY_SCENE_REASON
                : contextAvailability?.removeReason ?? 'Remove this object from the Program Draft.'}
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
        {typeof transportControls === 'function' ? transportControls(() => {
          setInspectorPanel('robot');
          onInspectorMinimizedChange?.(false);
        }, {
          open: openViewerSettings,
          previewLayerActive: auxiliaryViewerSourcesActive,
        }) : transportControls}
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
              ? 'Viewer settings'
              : inspectorPanel === 'robot'
                ? 'Robot control'
                : inspectorPanel === 'object'
                  ? 'Selected object'
                  : inspectorPanel === 'io'
                    ? 'Tool & I/O'
                    : 'Viewer settings'}</strong>
            {inspectorPanel === 'robot' && robotControlBlocked && (
              <span className="central-inspector-warning" title="The current manual target has no executable preview. Check the Robot panel; this is not a Program fault.">Check preview</span>
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
                disabled={inspectorPanel === null || robotPanelLocked}
                title={robotPanelLocked ? robotLockTitle : undefined}
                aria-label={inspectorMinimized ? 'Restore viewer inspector' : 'Minimize viewer inspector'}
                onClick={() => onInspectorMinimizedChange?.(!inspectorMinimized)}
              >{inspectorMinimized ? '⌃' : '⌄'}</button>
              <button
                type="button"
                disabled={inspectorPanel === null || robotPanelLocked}
                title={robotPanelLocked ? robotLockTitle : undefined}
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
              ) : inspectorPanel === 'io' ? (
                ioControlSurface ?? <p className="panel-state">Tool and I/O signals are unavailable.</p>
              ) : inspectorPanel === 'tools' ? (
                <div ref={viewerSettingsPanel} className="viewer-options-body dpanel">
                  {auxiliaryViewerSourcesActive && (
                    <div className="viewer-active-sources" aria-label="Visible non-primary sources">
                      <strong>Visible non-primary sources</strong>
                      {programDraftPreview && (
                        <span>
                          <i className="viewer-source-swatch draft" aria-hidden="true" />
                          <span>
                            <b>Program Draft scene</b>
                            Preview only; this is not the current owner scene or execution state.
                          </span>
                        </span>
                      )}
                      {(candidateTcpPose !== null || candidateRobot !== null) && (
                        <span>
                          <i className="viewer-source-swatch candidate" aria-hidden="true" />
                          <span>
                            <b>Motion candidate</b>
                            The validated no-motion preview owned by Robot Control.
                          </span>
                        </span>
                      )}
                    </div>
                  )}
                  <section className="dcard">
                  <SettingsSubheading
                    title="Display"
                    scope="installation"
                    onRestoreDefaults={() => composerSettings.restoreSection('viewer')}
                  />
                  <label className="toggle-control">
                    <input
                      type="checkbox"
                      aria-label="Wireframe overlays"
                      checked={wireframeVisible}
                      onChange={(event) => setViewerSetting('wireframeOverlays', event.target.checked)}
                    />
                    <span>Wireframe overlays</span>
                  </label>
                  <label
                    className="toggle-control"
                    title="Outline robots and objects so dark ones stay visible. Adds rendering work; slower with software rendering."
                  >
                    <input
                      type="checkbox"
                      checked={outlineVisible}
                      onChange={(event) => setViewerSetting('objectOutline', event.target.checked)}
                    />
                    <span>Object outline</span>
                  </label>
                  <label className="toggle-control">
                    <input
                      type="checkbox"
                      checked={collisionsVisible}
                      onChange={(event) => setViewerSetting('collisionGeometry', event.target.checked)}
                      disabled={manifest.status !== 'ready'}
                    />
                    <span>Collision geometry</span>
                  </label>
                  <label className="toggle-control">
                    <input
                      type="checkbox"
                      checked={pointCloudVisible}
                      onChange={(event) => setViewerSetting('pointCloud', event.target.checked)}
                      disabled={pointCloudCellId === undefined}
                    />
                    <span>Point cloud</span>
                  </label>
                  <MeasuredStateGhostToggle
                    enabled={measuredStateGhostsEnabled}
                    onChange={setMeasuredStateGhostsEnabled}
                  />
                  </section>
                  <ViewerGraphicsSettings rendererKind={rendererKind} />
                  <details className="dcard" open>
                    <summary>Frames and joints</summary>
                    <SettingsSubheading title="Size" scope="program" />
                    <label>
                      <span>Axis length</span>
                      <span className="dunit">
                        <input
                          type="range"
                          min={0.02}
                          max={1}
                          step={0.01}
                          value={frameAxesSizeM}
                          onChange={(event) => setFrameAxesSizeM(event.currentTarget.valueAsNumber)}
                          onPointerUp={(event) => onAxesSizesCommit?.(
                            event.currentTarget.valueAsNumber,
                            objectAxesSizeM,
                          )}
                          onKeyUp={(event) => onAxesSizesCommit?.(
                            event.currentTarget.valueAsNumber,
                            objectAxesSizeM,
                          )}
                        />
                        <output>{frameAxesSizeM.toFixed(2)} m</output>
                        <button
                          type="button"
                          onClick={() => {
                            const reset = viewer?.frame_gizmo_size_m ?? 0.1;
                            setFrameAxesSizeM(reset);
                            onAxesSizesCommit?.(reset, objectAxesSizeM);
                          }}
                        >Reset</button>
                        </span>
                    </label>
                    {programViewerPendingApply?.frameAxes === true && <PendingApplyNote />}
                    <SettingsSubheading title="Shown" scope="session" />
                    <div className="dactions">
                      <button
                        type="button"
                        onClick={() => setSessionFrameAxes(new Set(
                          modelAxisRows.filter((row) => row.poseAvailable)
                            .map((row) => row.linkName),
                        ))}
                      >Show all</button>
                      <button
                        type="button"
                        onClick={() => setSessionFrameAxes(new Set())}
                      >Hide all</button>
                      <button
                        type="button"
                        onClick={() => setSessionFrameAxes(new Set(
                          modelAxisRows.filter((row) => (
                            row.defaultVisible && row.poseAvailable
                          )).map((row) => row.linkName),
                        ))}
                      >Reset visibility</button>
                    </div>
                    <div className="viewer-axes-list">
                      {modelAxisRows.map((row) => (
                        <label
                          key={row.linkName}
                          className={row.poseAvailable ? undefined : 'unavailable'}
                          title={row.poseAvailable
                            ? [
                                row.componentId ?? 'cell',
                                row.modelInstanceId,
                                row.frameRoleIds.length > 0
                                  ? `roles ${row.frameRoleIds.join(', ')}` : null,
                              ].filter(Boolean).join(' · ')
                            : 'No exact pose exists in the current retained viewer model.'}
                        >
                          <input
                            type="checkbox"
                            disabled={!row.poseAvailable}
                            checked={sessionFrameAxes.has(row.linkName)}
                            onChange={(event) => setSessionFrameAxes((current) => {
                              const next = new Set(current);
                              if (event.target.checked) next.add(row.linkName);
                              else next.delete(row.linkName);
                              return next;
                            })}
                          />
                          <span>
                            <strong>{row.sourceLinkName}</strong>
                            <small>{row.sourceJointName === null
                              ? 'link frame'
                              : `joint ${row.sourceJointName}`}</small>
                          </span>
                        </label>
                      ))}
                    </div>
                  </details>
                  <details className="dcard" open>
                    <summary>Object axes</summary>
                    <SettingsSubheading title="Size" scope="program" />
                    <label>
                      <span>Axis length</span>
                      <span className="dunit">
                        <input
                          type="range"
                          min={0.02}
                          max={1}
                          step={0.01}
                          value={objectAxesSizeM}
                          onChange={(event) => setObjectAxesSizeM(event.currentTarget.valueAsNumber)}
                          onPointerUp={(event) => onAxesSizesCommit?.(
                            frameAxesSizeM,
                            event.currentTarget.valueAsNumber,
                          )}
                          onKeyUp={(event) => onAxesSizesCommit?.(
                            frameAxesSizeM,
                            event.currentTarget.valueAsNumber,
                          )}
                        />
                        <output>{objectAxesSizeM.toFixed(2)} m</output>
                        <button
                          type="button"
                          onClick={() => {
                            const reset = viewer?.object_axes_gizmo_size_m ?? 0.1;
                            setObjectAxesSizeM(reset);
                            onAxesSizesCommit?.(frameAxesSizeM, reset);
                          }}
                        >Reset</button>
                        </span>
                    </label>
                    {programViewerPendingApply?.objectAxes === true && <PendingApplyNote />}
                    <SettingsSubheading title="Shown" />
                    <div className="dactions">
                      <button
                        type="button"
                        onClick={() => onObjectAxesVisibleIdsChange?.(
                          presentedObjectAxesRows.filter((row) => row.poseAvailable)
                            .map((row) => row.sceneResourceId),
                        )}
                      >Show all</button>
                      <button
                        type="button"
                        onClick={() => onObjectAxesVisibleIdsChange?.([])}
                      >Hide all</button>
                      <button
                        type="button"
                        onClick={() => onObjectAxesVisibleIdsChange?.(
                          presentedObjectAxesRows.filter((row) => (
                            row.defaultVisible && row.poseAvailable
                          )).map((row) => row.sceneResourceId),
                        )}
                      >Reset visibility</button>
                    </div>
                    {([
                      ['Program objects', 'program', presentedObjectAxesRows.filter((row) => row.persistent)],
                      ['Other objects', 'session', presentedObjectAxesRows.filter((row) => !row.persistent)],
                    ] as const).map(([title, scope, rows]) => rows.length > 0 && (
                      <div className="viewer-axes-group" key={title}>
                        <SettingsSubheading title={title} scope={scope} minor />
                        <div className="viewer-axes-list">
                          {rows.map((row) => (
                            <label
                              key={row.sceneResourceId}
                              className={row.poseAvailable ? undefined : 'unavailable'}
                              title={row.poseAvailable
                                ? row.persistent
                                  ? 'Stored in the Program Draft.'
                                  : 'Session presentation only.'
                                : 'The object is hidden, so its axes have no visible pose.'}
                            >
                              <input
                                type="checkbox"
                                disabled={!row.poseAvailable}
                                checked={objectAxesVisible.has(row.sceneResourceId)}
                                onChange={(event) => {
                                  const next = new Set(objectAxesVisible);
                                  if (event.target.checked) next.add(row.sceneResourceId);
                                  else next.delete(row.sceneResourceId);
                                  onObjectAxesVisibleIdsChange?.([...next].sort());
                                }}
                              />
                              <span>
                                <strong>{row.sceneResourceId}</strong>
                                <small>{[
                                  row.attached ? 'Attached' : null,
                                  row.persistent ? null : row.sourceLabel,
                                ].filter(Boolean).join(' · ')}</small>
                              </span>
                            </label>
                          ))}
                        </div>
                      </div>
                    ))}
                    {presentedObjectAxesRows.length === 0 && (
                      <span className="muted">No current objects</span>
                    )}
                  </details>
                  {controlPanelSettings !== undefined && (
                    <details className="dcard" open>
                      <summary>Control panels</summary>
                      {controlPanelSettings}
                    </details>
                  )}
                  <section className="dcard">
                  <h4>Layers and model</h4>
                  <div className="dactions">
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
                  {pointCloud.frame !== null && (
                    <span
                      className="status-pill"
                      title={`${pointCloud.frame.ownerId} · sequence ${pointCloud.frame.sequence.toString()}`}
                    >{pointCloud.frame.pointCount} points</span>
                  )}
                  {manifest.status === 'ready' && (
                    <details className="dcard">
                      <summary>Model and source facts</summary>
                      <dl>
                        <div><dt>Cell</dt><dd><code>{manifest.data.cell_id}</code></dd></div>
                        <div><dt>Model size</dt><dd>{manifest.data.links.length} links · {manifest.data.joints.length} joints</dd></div>
                        <div><dt>Objects</dt><dd>{sceneObjects.length} {programDraftPreview ? 'Draft-preview' : 'owner-scene'}</dd></div>
                        <div><dt>Root</dt><dd><code>{manifest.data.projection.root_frame}</code></dd></div>
                        <div><dt>Model</dt><dd><code>{manifest.data.model_key}</code></dd></div>
                        <div><dt>Bundle</dt><dd><code>{manifest.data.retained_bundle_id}</code></dd></div>
                        <div><dt>Retained model</dt><dd>neutral coordinates</dd></div>
                        <div><dt>Owner observation</dt><dd>{runtimePresentation === undefined
                          ? 'unavailable'
                          : `${runtimePresentation.status.freshGood} fresh / ${runtimePresentation.status.total} sources`}</dd></div>
                        <div><dt>Scene overlay</dt><dd>owner-issued</dd></div>
                        <div><dt>Operational validity</dt><dd>not asserted</dd></div>
                      </dl>
                    </details>
                  )}
                  </section>
                </div>
              ) : objectControlSurface !== undefined ? (
                <>
                  <div className="robot-control-step-controls" aria-label="Object control increments"><GizmoSteps gizmo={gizmo} /></div>
                  <div className="viewer-object-global-actions">
                    <button
                      type="button"
                      disabled={selectedCameraTarget === null}
                      title="Move the camera to the selected item. Nothing is changed."
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
          <div className="viewer-object-quick-panel dpanel">
            <div className="robot-control-step-controls" aria-label="Object control increments"><GizmoSteps gizmo={gizmo} /></div>
            <section className="dcard">
            <h4>Object</h4>
            <dl>
              <div><dt>Scene resource</dt><dd><code>{selectedObject.scene_resource_id}</code></dd></div>
              <div><dt>Placement</dt><dd>{selectedObject.placement.kind.replaceAll('_', ' ')}</dd></div>
              <div><dt>Geometry</dt><dd>{selectedObject.collision_shapes.length} collision shape(s)</dd></div>
              <div><dt>Source</dt><dd>{programDraftPreview ? 'Program Draft preview' : 'owner-issued scene'}</dd></div>
            </dl>
            <div className="dactions">
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
                title={onDuplicateSceneResource === undefined
                  ? READ_ONLY_SCENE_REASON
                  : selectedObjectAvailability?.duplicateReason ?? 'Add a copy of this object to the Program Draft.'}
                onClick={() => { void onDuplicateSceneResource?.(selectedObject.scene_resource_id); }}
              >Duplicate</button>
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
                title={onRemoveSceneResource === undefined
                  ? READ_ONLY_SCENE_REASON
                  : selectedObjectAvailability?.removeReason ?? 'Remove this object from the Program Draft.'}
                onClick={() => { void onRemoveSceneResource?.(selectedObject.scene_resource_id); }}
              >Remove</button>
            </div>
            </section>
          </div>
              ) : selectedStructureName !== null ? (
          <div className="viewer-object-quick-panel dpanel">
            <section className="dcard">
            <h4>Robot link</h4>
            <dl>
              <div><dt>Link</dt><dd><code>{selectedStructureName}</code></dd></div>
              <div><dt>Source</dt><dd>retained structure</dd></div>
            </dl>
            <div className="dactions">
              <button type="button" onClick={() => issueCameraCommand('frame_selection')}>
                Frame link
              </button>
            </div>
            </section>
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
        {props.runtimeOverlay}
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
