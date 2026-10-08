import {
  lazy,
  memo,
  Suspense,
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import type { ReactNode } from 'react';
import { createPortal } from 'react-dom';

import { api } from '../api/client';
import { executionGroupScopes, executionGroupTargetLabels } from '../state/programExecutionGroups';
import {
  useExecutionObservationTransport,
  useShellHeadlessState,
  useViewerHeadlessState,
} from '../hooks/useHeadlessState';
import type { HeadlessStateSource } from '../hooks/useHeadlessState';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import { useCardPanelLayout } from '../hooks/useCardPanelLayout';
import { SettingsSubheading } from './SettingsScope';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { RobotControlController } from '../hooks/useRobotControl';
import {
  MotionControlSettings,
  ProgramTransportControls,
  type DraftGroupVisibility,
} from './ProgramRuntimePanel';
import { ProgramContactPolicyEditor } from './ProgramContactPolicyEditor';
import { RobotControlPanelView } from './RobotControlPanel';
import { useGizmoSettings } from './common/GizmoControls';
import type { GizmoController } from './common/GizmoControls';
import type { ViewerInspectorPanel } from './StructuralViewer';
import {
  candidateTcpPoseFromCell,
  candidateTcpPoseInCell,
  sceneOwnerFacts,
  sceneOwnerPresentation,
  type SceneObjectMutationAvailability,
  type SceneOwnerFacts,
  type RetainedSceneOwnerPresentation,
} from '../state/sceneAuthoring';
import {
  candidateRobotPresentation,
  candidateGripperPresentation,
  candidateToolPresentation,
  createExecutionRobotPresentationReader,
  frozenMeasuredRobotPresentationReader,
  executionRobotPresentation,
  runtimeScenePresentation,
  candidateObservationEpoch,
  resolvedPreviewRobotPresentation,
} from '../state/runtimeScenePresentation';
import {
  quaternionFromRpyDegrees,
  robotControlCandidatePresentationAllowed,
  robotControlHasActiveOperation,
  robotControlHasLocalDraft,
} from '../state/robotControl';
import type { StructuralResourceState } from '../state/structuralResource';
import {
  executionLocksObjectEditing,
  sceneSourceIsDraft,
  programViewerMetadata,
  viewerPresentationDraftFromProgram,
  viewerPresentationDraftReducer,
} from '../state/viewerPresentationDraft';
import { viewerModelPresentation } from '../state/viewerModelReadiness';
import type {
  JsonValue,
  FileBrowserResponse,
  LogicalSceneObject,
  LogicalScenePose,
  NodeEnvironmentResponse,
  ProgramAuthoringDocument,
  ProgramDocument,
  ProgramSceneResource,
  ProgramViewerMetadata,
  SceneUrdfManifest,
  SceneRuntimePoseSource,
  SceneRuntimeProjection,
  SourceModelProjection,
  SourceModelOccurrence,
} from '../types/api';
import type {
  ManualControlMotionDescriptor,
  MotionPreviewReceipt,
  Pose3Value,
} from '../types/robotControl';
import type { PoseTuple } from '../utils/poseTransforms';
import { poseValueFromTuple } from '../utils/poseTransforms';

const StructuralViewer = lazy(async () => {
  const module = await import('./StructuralViewer');
  return { default: module.StructuralViewer };
});

interface CorrectionGhost {
  receipt: MotionPreviewReceipt;
  /** The physical target's model, resolved through the Applied Program binding. */
  descriptor: ManualControlMotionDescriptor;
  stale: boolean;
}

interface SceneAuthoringWorkspaceProps {
  editorTarget: HTMLElement | null;
  environment: StructuralResourceState<NodeEnvironmentResponse>;
  headlessStateSource: HeadlessStateSource;
  manifest: StructuralResourceState<SceneUrdfManifest>;
  onRetryManifest: () => void;
  onRetrySourceModel: () => void;
  programController: ProgramWorkspaceController;
  programRuntime: ProgramRuntimeController;
  robotControl: RobotControlController;
  /** The advisory failed-node correction preview, shown as its own ghost. */
  correctionGhost?: CorrectionGhost | null;
  runtimeEnabled: boolean;
  sourceModel: StructuralResourceState<SourceModelProjection>;
  viewerActive: boolean;
  viewerInspectorTarget?: HTMLElement | null;
  viewerInspectorMinimized?: boolean;
  onViewerInspectorMinimizedChange?: (minimized: boolean) => void;
  onViewerInspectorMaximize?: () => void;
  onViewerInspectorPanelChange?: (panel: ViewerInspectorPanel) => void;
  onOpenRuntimeHistory?: () => void;
  runtimeOverlay?: ReactNode;
}

const EMPTY_PROGRAM_RESOURCES: ProgramSceneResource[] = [];
const EMPTY_LOGICAL_SCENE_OBJECTS: LogicalSceneObject[] = [];

type SceneSelection =
  | { kind: 'owner_object'; id: string }
  | { kind: 'program_resource'; id: string }
  | { kind: 'structure'; id: string; linkName: string }
  | null;

type SceneWorkspaceTool =
  | 'hierarchy'
  | 'program'
  | 'contacts'
  | 'current'
  | 'sources'
  | 'viewer';

function sceneSelectionKey(selection: SceneSelection): string | null {
  return selection === null ? null : `${selection.kind}:${selection.id}`;
}

function programRoleIds(
  program: ProgramDocument | ProgramAuthoringDocument | null,
  field: 'required_frame_roles' | 'required_resource_roles',
): string[] {
  if (program === null) return [];
  const result = new Set<string>();
  for (const target of Object.values(program.target_roles)) {
    if (target === null || typeof target !== 'object' || Array.isArray(target)) continue;
    const values = target[field];
    if (!Array.isArray(values)) continue;
    for (const value of values) if (typeof value === 'string') result.add(value);
  }
  if (field === 'required_resource_roles') {
    for (const resource of program.scene ?? []) {
      for (const role of resource.resource_roles ?? []) result.add(role);
    }
  }
  return [...result].sort();
}

interface ProgramComponentFrameOption {
  id: string;
  componentRole: string;
  modelRole: string;
  frameRole: string;
}

function stringList(value: JsonValue | undefined): string[] {
  if (!Array.isArray(value)) return [];
  return value.filter((item): item is string => typeof item === 'string');
}

function programComponentFrameOptions(
  program: ProgramAuthoringDocument,
): ProgramComponentFrameOption[] {
  const options: ProgramComponentFrameOption[] = [];
  for (const [componentRole, raw] of Object.entries(program.component_roles)) {
    if (raw === null || typeof raw !== 'object' || Array.isArray(raw)) continue;
    const models = stringList(raw.required_model_roles);
    const frames = stringList(raw.required_frame_roles);
    for (const modelRole of models) {
      for (const frameRole of frames) {
        options.push({
          id: `${componentRole}\u0000${modelRole}\u0000${frameRole}`,
          componentRole,
          modelRole,
          frameRole,
        });
      }
    }
  }
  return options.sort((left, right) => left.id.localeCompare(right.id));
}

function defaultResource(existingIds: Set<string>): ProgramSceneResource {
  let suffix = 1;
  while (existingIds.has(`scene_object_${suffix}`)) suffix += 1;
  return {
    scene_resource_id: `scene_object_${suffix}`,
    shape: { type: 'box', box_dimensions_m: [0.1, 0.1, 0.1] },
    pose: {
      frame_ref: { kind: 'cell_frame' },
      position_m: [0, 0, 0.05],
      orientation: { xyzw: [0, 0, 0, 1] },
    },
  };
}

function programScenePreviewObject(resource: ProgramSceneResource): LogicalSceneObject | null {
  if (resource.pose.frame_ref.kind !== 'cell_frame') return null;
  const orientation = (() => {
    if ('xyzw' in resource.pose.orientation) return resource.pose.orientation.xyzw;
    const quaternion = quaternionFromRpyDegrees(...resource.pose.orientation.rpy_deg);
    return [
      quaternion.x,
      quaternion.y,
      quaternion.z,
      quaternion.w,
    ] as [number, number, number, number];
  })();
  const shape = resource.shape.type === 'box'
    ? {
        type: 'box' as const,
        dimensions_m: {
          x: resource.shape.box_dimensions_m[0],
          y: resource.shape.box_dimensions_m[1],
          z: resource.shape.box_dimensions_m[2],
        },
      }
    : resource.shape.type === 'sphere'
      ? { type: 'sphere' as const, radius_m: resource.shape.sphere_radius_m }
      : resource.shape.type === 'cylinder'
        ? {
            type: 'cylinder' as const,
            radius_m: resource.shape.cylinder_radius_m,
            height_m: resource.shape.cylinder_height_m,
          }
        : {
            type: 'mesh' as const,
            resource_uri: resource.shape.mesh_resource_uri,
            scale: {
              x: resource.shape.mesh_scale?.[0] ?? 1,
              y: resource.shape.mesh_scale?.[1] ?? 1,
              z: resource.shape.mesh_scale?.[2] ?? 1,
            },
          };
  return {
    scene_resource_id: resource.scene_resource_id,
    collision_shapes: [shape],
    placement: {
      kind: 'cell_fixed',
      cell_id: 'program-draft-preview',
      pose_in_cell: {
        position_m: {
          x: resource.pose.position_m[0],
          y: resource.pose.position_m[1],
          z: resource.pose.position_m[2],
        },
        orientation_xyzw: orientation,
      },
    },
    provider_evidence: null,
    support_contacts: [],
  };
}

function programResourceWithViewportTransform(
  resource: ProgramSceneResource,
  pose: LogicalScenePose,
  scale: [number, number, number],
): ProgramSceneResource | null {
  if (resource.pose.frame_ref.kind !== 'cell_frame') return null;
  const [x, y, z] = scale;
  if (![
    pose.position_m.x,
    pose.position_m.y,
    pose.position_m.z,
    ...pose.orientation_xyzw,
    x,
    y,
    z,
  ].every(Number.isFinite)) return null;
  const norm = Math.hypot(...pose.orientation_xyzw);
  if (Math.abs(norm - 1) > 1e-6 || [x, y, z].some((value) => value <= 0)) return null;
  const shape = resource.shape.type === 'box'
    ? {
        ...resource.shape,
        box_dimensions_m: resource.shape.box_dimensions_m.map(
          (value, index) => value * scale[index],
        ) as [number, number, number],
      }
    : resource.shape.type === 'mesh'
      ? {
          ...resource.shape,
          mesh_scale: (resource.shape.mesh_scale ?? [1, 1, 1]).map(
            (value, index) => value * scale[index],
          ) as [number, number, number],
        }
      : resource.shape;
  if (
    resource.shape.type !== 'box'
    && resource.shape.type !== 'mesh'
    && scale.some((value) => Math.abs(value - 1) > 1e-6)
  ) return null;
  return {
    ...resource,
    shape,
    pose: {
      frame_ref: { kind: 'cell_frame' },
      position_m: [pose.position_m.x, pose.position_m.y, pose.position_m.z],
      orientation: { xyzw: [...pose.orientation_xyzw] },
    },
  };
}

function programSceneResourceError(resource: ProgramSceneResource | null): string | null {
  if (resource === null) return null;
  const positive = (value: number) => Number.isFinite(value) && value > 0;
  if (!resource.scene_resource_id.trim()) return 'Scene resource ID is required.';
  if (!resource.pose.position_m.every(Number.isFinite)) return 'Pose position must be finite.';
  const orientation = 'xyzw' in resource.pose.orientation
    ? resource.pose.orientation.xyzw
    : resource.pose.orientation.rpy_deg;
  if (!orientation.every(Number.isFinite)) return 'Pose orientation must be finite.';
  const shape = resource.shape;
  if (shape.type === 'box' && !shape.box_dimensions_m.every(positive)) {
    return 'Box dimensions must be greater than zero.';
  }
  if (shape.type === 'sphere' && !positive(shape.sphere_radius_m)) {
    return 'Sphere radius must be greater than zero.';
  }
  if (shape.type === 'cylinder'
    && (!positive(shape.cylinder_radius_m) || !positive(shape.cylinder_height_m))) {
    return 'Cylinder radius and height must be greater than zero.';
  }
  if (shape.type === 'mesh') {
    if (!shape.mesh_resource_uri.trim()) return 'Choose or enter a mesh resource.';
    if (!(shape.mesh_scale ?? [1, 1, 1]).every(positive)) {
      return 'Mesh scale must be greater than zero.';
    }
  }
  return null;
}

function SceneHierarchy({
  manifest,
  sourceModel,
  programResources,
  objects,
  attachments,
  selection,
  hiddenSelectionKeys,
  objectAxesSelectionKeys,
  selectionLocked,
  onSelect,
  onObjectVisibilityChange,
  onObjectAxesVisibilityChange,
}: {
  manifest: SceneUrdfManifest | null;
  sourceModel: SourceModelProjection | null;
  programResources: ProgramSceneResource[];
  objects: LogicalSceneObject[];
  attachments: NonNullable<SceneOwnerFacts['logicalScene']['snapshot']>['carried_object_attachments'];
  selection: SceneSelection;
  hiddenSelectionKeys: ReadonlySet<string>;
  objectAxesSelectionKeys: ReadonlySet<string>;
  selectionLocked: boolean;
  onSelect: (selection: SceneSelection) => void;
  onObjectVisibilityChange: (selectionKey: string, visible: boolean) => void;
  onObjectAxesVisibilityChange: (selectionKey: string, visible: boolean) => void;
}) {
  const [query, setQuery] = useState('');
  const normalized = query.trim().toLocaleLowerCase();
  const matches = (value: string) => normalized === ''
    || value.toLocaleLowerCase().includes(normalized);
  const selectedKey = sceneSelectionKey(selection);
  return (
    <div className="scene-hierarchy dpanel" data-testid="scene-hierarchy" aria-label="Scene hierarchy">
      <input
        type="search"
        value={query}
        placeholder="Find link, frame, or object"
        aria-label="Find scene hierarchy item"
        onChange={(event) => setQuery(event.target.value)}
      />
      <details className="dcard" open>
        <summary>
          <span>Owner scene objects</span>
          <span className="dcard-meta">{objects.length} objects · {attachments.length} Attached</span>
        </summary>
        <div className="scene-hierarchy-items">
          {objects.filter((object) => matches(object.scene_resource_id)).map((object) => {
            const attachment = attachments.find(
              (item) => item.scene_resource_id === object.scene_resource_id,
            );
            return (
              <div className="scene-hierarchy-item" key={object.scene_resource_id}>
                <button
                  type="button"
                  disabled={selectionLocked && selectedKey !== `owner_object:${object.scene_resource_id}`}
                  className={selectedKey === `owner_object:${object.scene_resource_id}`
                    ? 'dpick selected'
                    : 'dpick'}
                  onClick={() => onSelect({
                    kind: 'owner_object',
                    id: object.scene_resource_id,
                  })}
                >
                  <strong>{object.scene_resource_id}</strong>
                  <span>{attachment === undefined
                    ? object.placement.kind
                    : `Attached to ${attachment.controllable_component_id} / ${attachment.carrier_frame_ref.frame_role_id}`}</span>
                </button>
                <label title="Show this object in the viewport">
                  <input
                    type="checkbox"
                    checked={!hiddenSelectionKeys.has(
                      `owner_object:${object.scene_resource_id}`,
                    )}
                    onChange={(event) => onObjectVisibilityChange(
                      `owner_object:${object.scene_resource_id}`,
                      event.target.checked,
                    )}
                  />
                  <span>Visible</span>
                </label>
                <label title="Show this object's coordinate axes">
                  <input
                    type="checkbox"
                    checked={objectAxesSelectionKeys.has(
                      `owner_object:${object.scene_resource_id}`,
                    )}
                    onChange={(event) => onObjectAxesVisibilityChange(
                      `owner_object:${object.scene_resource_id}`,
                      event.target.checked,
                    )}
                  />
                  <span>Axes</span>
                </label>
              </div>
            );
          })}
          {objects.length === 0 && <span className="muted">No owner-issued objects</span>}
        </div>
      </details>
      <details className="dcard">
        <summary>Program Draft initial scene</summary>
        <div className="scene-hierarchy-items">
          {programResources.filter((resource) => matches(resource.scene_resource_id)).map((resource) => (
            <div className="scene-hierarchy-item" key={resource.scene_resource_id}>
              <button
                type="button"
                disabled={selectionLocked && selectedKey !== `program_resource:${resource.scene_resource_id}`}
                className={selectedKey === `program_resource:${resource.scene_resource_id}`
                  ? 'dpick selected'
                  : 'dpick'}
                onClick={() => onSelect({
                  kind: 'program_resource',
                  id: resource.scene_resource_id,
                })}
              >
                <strong>{resource.scene_resource_id}</strong>
                <span>
                  {resource.shape.type} · {resource.pose.frame_ref.kind === 'cell_frame'
                    ? 'Program Draft preview, not live'
                    : 'component frame resolves only from owner projection'}
                </span>
              </button>
              <label title={objects.length > 0
                ? 'The owner-issued current scene is displayed; Draft preview geometry is suppressed.'
                : 'Show this Draft resource in the viewport'}>
                <input
                  type="checkbox"
                  disabled={objects.length > 0}
                  checked={!hiddenSelectionKeys.has(
                    `program_resource:${resource.scene_resource_id}`,
                  )}
                  onChange={(event) => onObjectVisibilityChange(
                    `program_resource:${resource.scene_resource_id}`,
                    event.target.checked,
                  )}
                />
                <span>Visible</span>
              </label>
              <label title="Show this Draft object's coordinate axes">
                <input
                  type="checkbox"
                  disabled={objects.length > 0}
                  checked={objectAxesSelectionKeys.has(
                    `program_resource:${resource.scene_resource_id}`,
                  )}
                  onChange={(event) => onObjectAxesVisibilityChange(
                    `program_resource:${resource.scene_resource_id}`,
                    event.target.checked,
                  )}
                />
                <span>Axes</span>
              </label>
            </div>
          ))}
          {programResources.length === 0 && <span className="muted">No Draft scene resources</span>}
        </div>
      </details>
      <details className="dcard">
        <summary>Retained robot links and frames</summary>
        <div className="scene-hierarchy-items scene-hierarchy-static">
          {(manifest?.links ?? []).filter((link) => matches(link.name)).map((link) => (
            <button
              type="button"
              key={link.name}
              disabled={selectionLocked && selectedKey !== `structure:link:${link.name}`}
              className={selectedKey === `structure:link:${link.name}` ? 'dpick selected' : 'dpick'}
              onClick={() => onSelect({
                kind: 'structure',
                id: `link:${link.name}`,
                linkName: link.name,
              })}
            >
              <strong>{link.name}</strong><small>link</small>
            </button>
          ))}
          {(sourceModel?.targets ?? []).flatMap((target) => target.frames.map((frame) => ({
            key: `${target.component_id}:${target.target_id}:${frame.frame_role_id}`,
            label: `${target.component_id} / ${frame.frame_role_id}`,
            kind: frame.frame_kind,
            linkName: frame.source_link_name,
          }))).filter((frame) => matches(frame.label)).map((frame) => (
            <button
              type="button"
              key={frame.key}
              disabled={frame.linkName === null
                || (selectionLocked && selectedKey !== `structure:frame:${frame.key}`)}
              className={selectedKey === `structure:frame:${frame.key}` ? 'dpick selected' : 'dpick'}
              title={frame.linkName === null
                ? 'This frame has no retained source link to select in the viewport.'
                : `Select frame on ${frame.linkName}`}
              onClick={() => {
                if (frame.linkName === null) return;
                onSelect({
                  kind: 'structure',
                  id: `frame:${frame.key}`,
                  linkName: frame.linkName,
                });
              }}
            >
              <strong>{frame.label}</strong><small>{frame.kind}</small>
            </button>
          ))}
          {manifest === null && <span className="muted">Retained structure unavailable</span>}
        </div>
      </details>
    </div>
  );
}

function MeshResourceBrowser({
  onSelect,
}: {
  onSelect: (resourceUri: string) => void;
}) {
  const [path, setPath] = useState<string | undefined>(undefined);
  const [state, setState] = useState<
    { kind: 'loading' }
    | { kind: 'ready'; data: FileBrowserResponse }
    | { kind: 'error'; message: string }
  >({ kind: 'loading' });
  useEffect(() => {
    const controller = new AbortController();
    setState({ kind: 'loading' });
    void api.files.browse('mesh', path, controller.signal).then((data) => {
      setState({ kind: 'ready', data });
    }).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setState({
          kind: 'error',
          message: error instanceof Error ? error.message : 'Mesh directory unavailable.',
        });
      }
    });
    return () => controller.abort();
  }, [path]);
  const choose = (entryPath: string, roots: string[]) => {
    const root = [...roots]
      .sort((left, right) => right.length - left.length)
      .find((candidate) => (
        entryPath === candidate || entryPath.startsWith(`${candidate}/`)
      ));
    onSelect(root === undefined
      ? entryPath
      : entryPath.slice(root.length).replace(/^\/+/, ''));
  };
  return (
    <div
      className="scene-mesh-browser"
      onPointerDownCapture={(event) => event.preventDefault()}
    >
      {state.kind === 'loading' && <span className="muted">Reading approved mesh files…</span>}
      {state.kind === 'error' && <span className="scene-commit-error inline-message severity-error" role="alert">{state.message}</span>}
      {state.kind === 'ready' && (
        <>
          <div className="scene-mesh-browser-path">
            <button
              type="button"
              disabled={state.data.parentPath === null}
              onClick={() => setPath(state.data.parentPath ?? undefined)}
            >Up</button>
            <code>{state.data.currentPath}</code>
          </div>
          <div className="scene-mesh-browser-entries">
            {state.data.entries.map((entry) => (
              <button
                type="button"
                key={entry.path}
                onClick={() => {
                  if (entry.entryType === 'directory') setPath(entry.path);
                  else choose(entry.path, state.data.roots);
                }}
              >
                <span>{entry.entryType === 'directory' ? 'DIR' : 'MESH'}</span>
                <strong>{entry.name}</strong>
              </button>
            ))}
            {state.data.entries.length === 0 && (
              <span className="muted">No compatible meshes in this directory.</span>
            )}
          </div>
        </>
      )}
    </div>
  );
}

function SceneShapeFields({
  shape,
  disabled,
  disabledTitle,
  onChange,
}: {
  shape: ProgramSceneResource['shape'];
  disabled: boolean;
  disabledTitle?: string;
  onChange: (shape: ProgramSceneResource['shape']) => void;
}) {
  const [meshBrowserOpen, setMeshBrowserOpen] = useState(false);
  const setShapeType = (type: ProgramSceneResource['shape']['type']) => {
    const next = type === 'box'
      ? { type, box_dimensions_m: [0.1, 0.1, 0.1] as [number, number, number] }
      : type === 'sphere'
        ? { type, sphere_radius_m: 0.05 }
        : type === 'cylinder'
          ? { type, cylinder_radius_m: 0.05, cylinder_height_m: 0.1 }
          : { type, mesh_resource_uri: '', mesh_scale: [1, 1, 1] as [number, number, number] };
    setMeshBrowserOpen(false);
    onChange(next);
  };
  const finite = (value: number, update: () => void) => {
    if (Number.isFinite(value)) update();
  };
  return (
    <div className="scene-shape-fields">
      <label className="scene-shape-select">
        <span>Shape</span>
        <select
          value={shape.type}
          disabled={disabled}
          title={disabled ? disabledTitle : undefined}
          onChange={(event) => setShapeType(
            event.target.value as ProgramSceneResource['shape']['type'],
          )}
        >
          <option value="box">Box</option>
          <option value="sphere">Sphere</option>
          <option value="cylinder">Cylinder</option>
          <option value="mesh">Mesh</option>
        </select>
      </label>
      {shape.type === 'box' && (
        <div className="scene-pose-grid dgrid">
          {(['Width', 'Depth', 'Height'] as const).map((label, index) => (
            <label key={label}>
              <span>{label} (m)</span>
              <input
                type="number"
                min={0.000001}
                step={0.001}
                value={shape.box_dimensions_m[index]}
                disabled={disabled}
                title={disabled ? disabledTitle : undefined}
                onChange={(event) => finite(event.currentTarget.valueAsNumber, () => {
                  const dimensions = [...shape.box_dimensions_m] as [number, number, number];
                  dimensions[index] = event.currentTarget.valueAsNumber;
                  onChange({ ...shape, box_dimensions_m: dimensions });
                })}
              />
            </label>
          ))}
        </div>
      )}
      {shape.type === 'sphere' && (
        <label className="scene-shape-select">
          <span>Radius (m)</span>
          <input
            type="number"
            min={0.000001}
            step={0.001}
            value={shape.sphere_radius_m}
            disabled={disabled}
            title={disabled ? disabledTitle : undefined}
            onChange={(event) => finite(event.currentTarget.valueAsNumber, () => {
              onChange({ ...shape, sphere_radius_m: event.currentTarget.valueAsNumber });
            })}
          />
        </label>
      )}
      {shape.type === 'cylinder' && (
        <div className="scene-pose-grid dgrid">
          {(['Radius', 'Height'] as const).map((label) => (
            <label key={label}>
              <span>{label} (m)</span>
              <input
                type="number"
                min={0.000001}
                step={0.001}
                value={label === 'Radius'
                  ? shape.cylinder_radius_m
                  : shape.cylinder_height_m}
                disabled={disabled}
                title={disabled ? disabledTitle : undefined}
                onChange={(event) => finite(event.currentTarget.valueAsNumber, () => {
                  onChange(label === 'Radius'
                    ? { ...shape, cylinder_radius_m: event.currentTarget.valueAsNumber }
                    : { ...shape, cylinder_height_m: event.currentTarget.valueAsNumber });
                })}
              />
            </label>
          ))}
        </div>
      )}
      {shape.type === 'mesh' && (
        <>
          <label className="scene-shape-select">
            <span>Mesh resource URI</span>
            <input
              type="text"
              value={shape.mesh_resource_uri}
              disabled={disabled}
              title={disabled ? disabledTitle : undefined}
              required
              placeholder="examples/cell/meshes/object.glb"
              onChange={(event) => onChange({
                ...shape,
                mesh_resource_uri: event.target.value,
              })}
            />
          </label>
          <button
            type="button"
            className="quiet-button"
            aria-expanded={meshBrowserOpen}
            disabled={disabled}
            title={disabled ? disabledTitle : 'Pick the mesh for this object from the approved mesh files.'}
            onPointerDown={(event) => event.preventDefault()}
            onClick={() => setMeshBrowserOpen((current) => !current)}
          >
            {meshBrowserOpen ? 'Close mesh browser' : 'Browse approved meshes'}
          </button>
          {meshBrowserOpen && (
            <MeshResourceBrowser
              onSelect={(resourceUri) => {
                onChange({ ...shape, mesh_resource_uri: resourceUri });
                setMeshBrowserOpen(false);
              }}
            />
          )}
          <div className="scene-pose-grid dgrid">
            {(['Scale X', 'Scale Y', 'Scale Z'] as const).map((label, index) => (
              <label key={label}>
                <span>{label}</span>
                <input
                  type="number"
                  min={0.000001}
                  step={0.01}
                  value={shape.mesh_scale?.[index] ?? 1}
                  disabled={disabled}
                  title={disabled ? disabledTitle : undefined}
                  onChange={(event) => finite(event.currentTarget.valueAsNumber, () => {
                    const scale = [
                      ...(shape.mesh_scale ?? [1, 1, 1]),
                    ] as [number, number, number];
                    scale[index] = event.currentTarget.valueAsNumber;
                    onChange({ ...shape, mesh_scale: scale });
                  })}
                />
              </label>
            ))}
          </div>
        </>
      )}
    </div>
  );
}

/**
 * Why the live object cannot be copied into the Draft, or null when it can.
 * Mirrors the backend resolver's conditions for display only; the backend
 * stays the authority. A parsed cell-fixed object always has a complete pose
 * (finite position, non-zero quaternion), so that condition needs no check.
 */
function observedCopyUnavailableReason(
  object: LogicalSceneObject | undefined,
): string | null {
  if (object === undefined) return 'This object is not in the live cell.';
  if (object.placement.kind !== 'cell_fixed') {
    return 'This object is Attached in the live cell; copy it once it is Detached.';
  }
  if (object.collision_shapes.length !== 1) {
    return 'Only a live object with exactly one collision shape can be copied.';
  }
  return null;
}

/**
 * The single capture action: the object as observed now in the live cell, into
 * the Draft. Enabled only while Draft editing is allowed (the same admission
 * as every other Draft edit) and the object is copyable.
 */
export function CopyObservedToDraftButton({
  object,
  blockedReason,
  mutate,
}: {
  object: LogicalSceneObject | undefined;
  /** The shared Draft edit gate reason; null while the Draft is editable. */
  blockedReason: string | null;
  mutate: ProgramWorkspaceController['mutate'];
}) {
  const reason = blockedReason ?? observedCopyUnavailableReason(object);
  return (
    <button
      type="button"
      disabled={reason !== null}
      title={reason ?? 'Copy this object as observed now in the live cell into the Program Draft, replacing the Draft copy. Apply or Save to keep it.'}
      onPointerDown={(event) => event.preventDefault()}
      onClick={() => {
        if (reason === null && object !== undefined) {
          void mutate('program_draft_store_observed_scene_resource', {
            scene_resource_id: object.scene_resource_id,
          });
        }
      }}
    >Copy current position to Draft</button>
  );
}

export function ProgramSceneEditor({
  program,
  draftRevision,
  busy,
  busyReason,
  mutate,
  liveObjects,
  selectedResourceId,
  onSelectResource,
}: {
  program: ProgramAuthoringDocument;
  draftRevision: number;
  busy: boolean;
  busyReason: string | null;
  mutate: ProgramWorkspaceController['mutate'];
  /** The live cell objects, for copying an observed position into the Draft. */
  liveObjects: LogicalSceneObject[];
  selectedResourceId: string | null;
  onSelectResource: (sceneResourceId: string | null) => void;
}) {
  const blockedTitle = busy ? busyReason ?? undefined : undefined;
  const resources = program.scene ?? EMPTY_PROGRAM_RESOURCES;
  const selected = resources.find(
    (resource) => resource.scene_resource_id === selectedResourceId,
  ) ?? null;
  const [staged, setStaged] = useState<ProgramSceneResource | null>(selected);

  useEffect(() => {
    setStaged(selected);
  }, [draftRevision, selected]);

  const dirty = staged !== null && selected !== null
    && JSON.stringify(staged) !== JSON.stringify(selected);
  const stagedError = programSceneResourceError(staged);
  const frameOptions = useMemo(() => programComponentFrameOptions(program), [program]);
  const commitStaged = () => {
    if (busy || !dirty || staged === null || stagedError !== null) return;
    mutate('scene_draft_update_object', {
      scene_resource: staged as unknown as JsonValue,
    });
  };
  const setPosePosition = (index: number, value: number) => {
    if (staged === null || !Number.isFinite(value)) return;
    const position = [...staged.pose.position_m] as [number, number, number];
    position[index] = value;
    setStaged({ ...staged, pose: { ...staged.pose, position_m: position } });
  };
  const setQuaternion = (index: number, value: number) => {
    if (staged === null || !Number.isFinite(value)) return;
    const source = 'xyzw' in staged.pose.orientation
      ? staged.pose.orientation.xyzw
      : [0, 0, 0, 1] as [number, number, number, number];
    const xyzw = [...source] as [number, number, number, number];
    xyzw[index] = value;
    setStaged({
      ...staged,
      pose: { ...staged.pose, orientation: { xyzw } },
    });
  };
  const setRpyDegrees = (index: number, value: number) => {
    if (staged === null || !Number.isFinite(value)) return;
    const source = 'rpy_deg' in staged.pose.orientation
      ? staged.pose.orientation.rpy_deg
      : [0, 0, 0] as [number, number, number];
    const rpy = [...source] as [number, number, number];
    rpy[index] = value;
    setStaged({
      ...staged,
      pose: { ...staged.pose, orientation: { rpy_deg: rpy } },
    });
  };
  const addResource = (resource: ProgramSceneResource) => {
    onSelectResource(resource.scene_resource_id);
    if (staged !== null && dirty) {
      mutate('scene_draft_upsert_objects', {
        scene_resources: [staged, resource] as unknown as JsonValue,
      });
      return;
    }
    mutate('scene_draft_add_object', {
      scene_resource: resource as unknown as JsonValue,
    });
  };
  const duplicateSelected = () => {
    if (staged === null) return;
    const ids = new Set(resources.map((resource) => resource.scene_resource_id));
    let identity = `${staged.scene_resource_id}_copy`;
    let suffix = 2;
    while (ids.has(identity)) identity = `${staged.scene_resource_id}_copy_${suffix++}`;
    const duplicate = structuredClone(staged);
    duplicate.scene_resource_id = identity;
    duplicate.pose.position_m = [
      duplicate.pose.position_m[0] + 0.05,
      duplicate.pose.position_m[1],
      duplicate.pose.position_m[2],
    ];
    addResource(duplicate);
  };
  const selectedFrameId = staged?.pose.frame_ref.kind === 'component_frame'
    ? `${staged.pose.frame_ref.component_role}\u0000${staged.pose.frame_ref.model_role}\u0000${staged.pose.frame_ref.frame_role}`
    : 'cell';
  const effectiveFrameOptions = staged?.pose.frame_ref.kind !== 'component_frame'
    || frameOptions.some((option) => option.id === selectedFrameId)
    ? frameOptions
    : [
        ...frameOptions,
        {
          id: selectedFrameId,
          componentRole: staged.pose.frame_ref.component_role,
          modelRole: staged.pose.frame_ref.model_role,
          frameRole: staged.pose.frame_ref.frame_role,
        },
      ];

  return (
    <div
      className="scene-authoring-column dpanel"
      data-testid="program-scene-editor"
      data-invalid={stagedError !== null}
      onBlurCapture={(event) => {
        if (['INPUT', 'SELECT', 'TEXTAREA'].includes(event.target.tagName)) commitStaged();
      }}
      onKeyDownCapture={(event) => {
        if (event.key === 'Enter' && event.target instanceof HTMLElement) {
          event.preventDefault();
          event.target.blur();
        }
      }}
    >
      <section className="dcard">
      <header>
        <span>Initial scene</span>
        <button
          type="button"
          disabled={busy}
          onPointerDown={(event) => event.preventDefault()}
          onClick={() => addResource(defaultResource(
            new Set(resources.map((resource) => resource.scene_resource_id)),
          ))}
          title={blockedTitle ?? 'Add a new 10 cm box to the Program Draft.'}
        >
          Add box
        </button>
      </header>
      <p>
        These objects belong to the Program Draft, not the live cell. Each committed field
        change is one Draft edit; Apply, Revert, or Discard in the top bar.
      </p>
      <label>
      <span>Resource</span>
      <select
        aria-label="Program scene resource"
        value={selected?.scene_resource_id ?? ''}
        disabled={resources.length === 0}
        onChange={(event) => onSelectResource(event.target.value || null)}
      >
        <option value="">
          {resources.length === 0 ? 'No Program resources' : 'Select a Program resource'}
        </option>
        {resources.map((resource) => (
          <option value={resource.scene_resource_id} key={resource.scene_resource_id}>
            {resource.scene_resource_id}
          </option>
        ))}
      </select>
      </label>
      </section>
      {staged !== null && (
        <>
          <section className="dcard">
          <h4>Shape and pose</h4>
          <SceneShapeFields
            shape={staged.shape}
            disabled={busy}
            disabledTitle={blockedTitle}
            onChange={(shape) => setStaged({ ...staged, shape })}
          />
          <label className="scene-shape-select">
            <span>Pose frame</span>
            <select
              value={selectedFrameId}
              disabled={busy}
              title={blockedTitle}
              onChange={(event) => {
                if (staged === null) return;
                if (event.target.value === 'cell') {
                  setStaged({
                    ...staged,
                    pose: { ...staged.pose, frame_ref: { kind: 'cell_frame' } },
                  });
                  return;
                }
                const selectedFrame = effectiveFrameOptions.find(
                  (option) => option.id === event.target.value,
                );
                if (selectedFrame !== undefined) {
                  setStaged({
                    ...staged,
                    pose: {
                      ...staged.pose,
                      frame_ref: {
                        kind: 'component_frame',
                        component_role: selectedFrame.componentRole,
                        model_role: selectedFrame.modelRole,
                        frame_role: selectedFrame.frameRole,
                      },
                    },
                  });
                }
              }}
            >
              <option value="cell">Cell frame</option>
              {effectiveFrameOptions.map((option) => (
                <option value={option.id} key={option.id}>
                  {option.componentRole} · {option.modelRole} · {option.frameRole}
                </option>
              ))}
            </select>
          </label>
          <div className="scene-pose-grid dgrid">
            {(['X', 'Y', 'Z'] as const).map((axis, index) => (
              <label key={axis}>
                <span>{axis} (m)</span>
                <input
                  type="number"
                  aria-label={`Draft ${axis} position (m)`}
                  step={0.001}
                  value={staged.pose.position_m[index]}
                  disabled={busy}
                  title={blockedTitle}
                  onChange={(event) => setPosePosition(index, event.currentTarget.valueAsNumber)}
                />
              </label>
            ))}
          </div>
          {'xyzw' in staged.pose.orientation ? (
            <div className="scene-pose-grid scene-quaternion-grid dgrid">
              {(['Qx', 'Qy', 'Qz', 'Qw'] as const).map((axis, index) => (
                <label key={axis}>
                  <span>{axis}</span>
                  <input
                    type="number"
                    step={0.001}
                    value={'xyzw' in staged.pose.orientation
                      ? staged.pose.orientation.xyzw[index]
                      : 0}
                    disabled={busy}
                    title={blockedTitle}
                    onChange={(event) => setQuaternion(index, event.currentTarget.valueAsNumber)}
                  />
                </label>
              ))}
            </div>
          ) : (
            <div className="scene-pose-grid dgrid">
              {(['Roll', 'Pitch', 'Yaw'] as const).map((axis, index) => (
                <label key={axis}>
                  <span>{axis} (deg)</span>
                  <input
                    type="number"
                    step={0.1}
                    value={'rpy_deg' in staged.pose.orientation
                      ? staged.pose.orientation.rpy_deg[index]
                      : 0}
                    disabled={busy}
                    title={blockedTitle}
                    onChange={(event) => setRpyDegrees(index, event.currentTarget.valueAsNumber)}
                  />
                </label>
              ))}
            </div>
          )}
          <p className="scene-contract-note hint">
            Frame: <code>{staged.pose.frame_ref.kind}</code> · shape:{' '}
            <code>{staged.shape.type}</code>
          </p>
          {stagedError !== null && (
            <p className="scene-commit-error inline-message severity-error" role="alert">{stagedError}</p>
          )}
          </section>
          <details className="dcard">
            <summary>Roles and collision metadata</summary>
            <dl>
              <div><dt>Resource roles</dt><dd>{staged.resource_roles?.join(', ') || 'none'}</dd></div>
              <div><dt>Collision policies</dt><dd>{staged.collision_policy_ids?.join(', ') || 'none'}</dd></div>
              <div><dt>Support contacts</dt><dd>{Array.isArray(staged.metadata?.support_contacts)
                ? staged.metadata.support_contacts.length
                : 0}</dd></div>
            </dl>
            {staged.metadata !== undefined && (
              <pre className="dcode">{JSON.stringify(staged.metadata, null, 2)}</pre>
            )}
          </details>
          <section className="dcard">
          <h4>Edit</h4>
          <div className="dactions">
            <button
              type="button"
              disabled={busy}
              title={blockedTitle ?? 'Add a copy of this object to the Program Draft, offset by 5 cm.'}
              onPointerDown={(event) => event.preventDefault()}
              onClick={duplicateSelected}
            >
              Duplicate
            </button>
            <CopyObservedToDraftButton
              object={liveObjects.find(
                (object) => object.scene_resource_id === staged.scene_resource_id,
              )}
              blockedReason={busy ? busyReason ?? 'The Program Draft cannot be edited now.' : null}
              mutate={mutate}
            />
            <button
              className="danger-button"
              type="button"
              disabled={busy}
              title={blockedTitle ?? 'Remove this object from the Program Draft.'}
              onPointerDown={(event) => event.preventDefault()}
              onClick={() => mutate('scene_draft_remove_objects', {
                scene_resource_ids: [staged.scene_resource_id],
              })}
            >
              Remove from Draft
            </button>
          </div>
          </section>
        </>
      )}
    </div>
  );
}

function ViewerMetadataEditor({
  program,
  draftRevision,
  busy,
  busyReason,
  mutate,
}: {
  program: ProgramAuthoringDocument;
  draftRevision: number;
  busy: boolean;
  busyReason: string | null;
  mutate: ProgramWorkspaceController['mutate'];
}) {
  const blockedTitle = busy ? busyReason ?? undefined : undefined;
  const [draft, setDraft] = useState(() => viewerPresentationDraftFromProgram(program.viewer));
  useEffect(() => {
    setDraft(viewerPresentationDraftFromProgram(program.viewer));
  }, [draftRevision, program.viewer]);
  const exact = programViewerMetadata(draft);
  const dirty = JSON.stringify(exact) !== JSON.stringify(
    programViewerMetadata(viewerPresentationDraftFromProgram(program.viewer)),
  );
  const dispatch = (action: Parameters<typeof viewerPresentationDraftReducer>[1]) => {
    setDraft((current) => viewerPresentationDraftReducer(current, action));
  };
  const dispatchAndCommit = (action: Parameters<typeof viewerPresentationDraftReducer>[1]) => {
    const next = viewerPresentationDraftReducer(draft, action);
    setDraft(next);
    mutate('viewer_draft_replace', {
      viewer: programViewerMetadata(next) as unknown as JsonValue,
    });
  };
  const commitViewerDraft = () => {
    if (busy || !dirty) return;
    mutate('viewer_draft_replace', { viewer: exact as unknown as JsonValue });
  };
  const frameRoles = programRoleIds(program, 'required_frame_roles');
  const resourceRoles = programRoleIds(program, 'required_resource_roles');
  return (
    <div
      className="scene-authoring-column dpanel"
      data-testid="viewer-metadata-editor"
      onBlurCapture={(event) => {
        if (['INPUT', 'SELECT', 'TEXTAREA'].includes(event.target.tagName)) commitViewerDraft();
      }}
      onKeyDownCapture={(event) => {
        if (event.key === 'Enter' && event.target instanceof HTMLElement) {
          event.preventDefault();
          event.target.blur();
        }
      }}
    >
      <section className="dcard">
      <SettingsSubheading title="Shown roles" scope="program" />
      <div className="viewer-role-controls">
        {frameRoles.map((role) => (
          <label key={`frame:${role}`}>
            <input
              type="checkbox"
              checked={draft.frameRoleVisibility[role] ?? true}
              disabled={busy}
              title={blockedTitle}
              onChange={(event) => dispatchAndCommit({
                type: 'set_frame_visibility',
                frameId: role,
                visible: event.target.checked,
              })}
            />
            frame role <code>{role}</code>
          </label>
        ))}
        {resourceRoles.map((role) => (
          <label key={`resource:${role}`}>
            <input
              type="checkbox"
              checked={draft.resourceRoleVisibility[role] ?? true}
              disabled={busy}
              title={blockedTitle}
              onChange={(event) => dispatchAndCommit({
                type: 'set_resource_visibility',
                resourceRoleId: role,
                visible: event.target.checked,
              })}
            />
            resource role <code>{role}</code>
          </label>
        ))}
      </div>
      </section>
      <section className="dcard">
      <SettingsSubheading title="Axes size" scope="program" />
        <label>
          <span>Frame axes (m)</span>
          <input
            type="number"
            min={0.02}
            max={1}
            step={0.01}
            value={draft.frameGizmoSizeM}
            disabled={busy}
            title={blockedTitle}
            onChange={(event) => dispatch({
              type: 'set_frame_gizmo_size',
              sizeM: event.currentTarget.valueAsNumber,
            })}
          />
        </label>
        <label>
          <span>Object axes (m)</span>
          <input
            type="number"
            min={0.02}
            max={1}
            step={0.01}
            value={draft.objectAxesGizmoSizeM}
            disabled={busy}
            title={blockedTitle}
            onChange={(event) => dispatch({
              type: 'set_object_axes_gizmo_size',
              sizeM: event.currentTarget.valueAsNumber,
            })}
          />
        </label>
      {dirty && <p className="scene-contract-note hint">Valid edits commit on blur, Tab, or Enter.</p>}
      </section>
    </div>
  );
}

function SourceOccurrencePanel({
  occurrence,
  occurrences,
  runtime,
  selectedId,
  onSelect,
}: {
  occurrence: SourceModelOccurrence | null;
  occurrences: SourceModelOccurrence[];
  runtime: SceneRuntimeProjection | null;
  selectedId: string | null;
  onSelect: (id: string) => void;
}) {
  const runtimeSources: SceneRuntimePoseSource[] = runtime?.state.pose_sources ?? [];
  const boundedNames = (values: string[], maximum = 24): string => {
    const shown = values.slice(0, maximum);
    const suffix = values.length > maximum
      ? `, … +${values.length - maximum} more`
      : '';
    return `${shown.join(', ') || 'none'}${suffix}`;
  };
  return (
    <div className="source-occurrence-panel dpanel">
      <section className="dcard">
      <h4>Model occurrence</h4>
      <label>
      <span>Occurrence</span>
      <select
        aria-label="Source model occurrence"
        value={selectedId ?? ''}
        disabled={occurrences.length === 0}
        onChange={(event) => onSelect(event.target.value)}
      >
        {occurrences.length === 0 && <option value="">No model occurrences</option>}
        {occurrences.map((item) => (
          <option key={item.model_instance_id} value={item.model_instance_id}>
            {item.model_instance_id} · {item.source_robot_name}
          </option>
        ))}
      </select>
      </label>
      {occurrence !== null && (
        <dl>
          <div><dt>Component</dt><dd><code>{occurrence.component_id ?? 'passive'}</code></dd></div>
          <div><dt>Source model</dt><dd><code>{occurrence.source_model_name}</code></dd></div>
          <div><dt>Source robot</dt><dd><code>{occurrence.source_robot_name}</code></dd></div>
          <div><dt>Source coordinates</dt><dd>{occurrence.coordinates.length}</dd></div>
          <div><dt>SRDF groups</dt><dd>{occurrence.srdf_groups.length}</dd></div>
        </dl>
      )}
      <p>
        Names remain the original URDF/SRDF names for this occurrence. The authored-neutral mesh below is not claimed as an active multi-instance pose.
      </p>
      </section>
      {occurrence !== null && (
        <details className="dcard">
          <summary>Source-local URDF/SRDF names</summary>
          <dl>
            <div><dt>Links</dt><dd><code>{boundedNames(
              occurrence.links.map((link) => link.source_link_name),
            )}</code></dd></div>
            <div><dt>Joints</dt><dd><code>{boundedNames(
              occurrence.coordinates.map((coordinate) => coordinate.source_joint_name),
            )}</code></dd></div>
            <div><dt>SRDF groups</dt><dd><code>{boundedNames(
              occurrence.srdf_groups.map((group) => group.source_group_name),
            )}</code></dd></div>
            <div><dt>End effectors</dt><dd><code>{boundedNames(
              occurrence.srdf_end_effectors.map(
                (endEffector) => endEffector.source_end_effector_name,
              ),
            )}</code></dd></div>
          </dl>
        </details>
      )}
      <section className="dcard runtime-source-list">
        <header>
          <span>Runtime pose sources</span>
          <span className="dcard-meta">{runtimeSources.length}</span>
        </header>
        {runtimeSources.slice(0, 128).map((source) => (
          <div
            className="runtime-source-row"
            key={`${source.object_id}:${source.policy_id}:${source.source_epoch}:${source.source_id}:${source.source_sequence}`}
          >
            <div>
              <strong>{source.source_id}</strong>
              <span>{source.object_id} · {source.policy_id}</span>
            </div>
            <div>
              <span className={`status-pill ${source.fresh ? 'ready' : 'warning'}`}>
                {source.quality === 'unknown'
                  ? 'unknown'
                  : source.fresh ? 'fresh' : 'stale'}
              </span>
              <span>{source.payload.kind} · sequence {source.source_sequence}</span>
              {source.buffered && <span>buffered</span>}
            </div>
            {source.quality === 'unknown' && (
              <span className="scene-commit-error inline-message severity-error">{source.payload.source_error.detail}</span>
            )}
          </div>
        ))}
        {runtimeSources.length === 0 && (
          <span className="muted">No runtime pose sources are currently projected.</span>
        )}
        {runtimeSources.length > 128 && (
          <span className="muted">
            Showing the first 128 sources; use owner diagnostics for the complete bounded set.
          </span>
        )}
        {runtimeSources.length > 0 && (
          <span className="muted">
            These owner-issued pose sources are observation-only; the current source contract
            advertises no mutation action.
          </span>
        )}
      </section>
      {runtime !== null && (
        <details className="dcard">
          <summary>Runtime source diagnostics</summary>
          <dl>
            <div><dt>State</dt><dd><code>{runtime.state.state}</code></dd></div>
            <div><dt>Reason</dt><dd>{runtime.state.reason || 'none'}</dd></div>
            <div><dt>Deferred</dt><dd>{runtime.state.deferred ? 'yes' : 'no'}</dd></div>
            <div><dt>Error</dt><dd>{runtime.state.error ?? 'none'}</dd></div>
            {Object.entries(runtime.state.counters).slice(0, 64).map(([name, value]) => (
              <div key={name}><dt>{name}</dt><dd>{value}</dd></div>
            ))}
          </dl>
        </details>
      )}
    </div>
  );
}

const SceneViewerBridge = memo(function SceneViewerBridge({
  active,
  source,
  sourceValue,
  sourceModelState,
  controlBasePoseInCell,
  environmentValue,
  manifest,
  onRetry,
  onRetrySourceModel,
  program,
  draftIdentity,
  viewer,
  selection,
  selectionLocked,
  onSelect,
  ghost,
  ghostStale,
  candidatePresentationAllowed,
  candidatePoseEditable,
  gizmo,
  onDuplicateProgramResource,
  onRemoveProgramResource,
  onCommitProgramTransform,
  onEditSelection,
  hiddenSelectionKeys,
  objectAxesSelectionKeys,
  onObjectAxesVisibleIdsChange,
  onAxesSizesCommit,
  motionPreview,
  programController,
  programRuntime,
  robotControl,
  correctionGhost,
  runtimeEnabled,
  objectControlSurface,
  inspectorTarget,
  inspectorMinimized,
  onInspectorMinimizedChange,
  onInspectorMaximize,
  onInspectorPanelChange,
  onOpenRuntimeHistory,
  runtimeOverlay,
  executionActive,
}: {
  active: boolean;
  source: HeadlessStateSource;
  sourceValue: SourceModelProjection | null;
  sourceModelState: StructuralResourceState<SourceModelProjection>;
  controlBasePoseInCell: PoseTuple | null;
  environmentValue: NodeEnvironmentResponse | null;
  manifest: StructuralResourceState<SceneUrdfManifest>;
  onRetry: () => void;
  onRetrySourceModel: () => void;
  program: ProgramDocument | ProgramAuthoringDocument | null;
  draftIdentity: { draftId: string; draftRevision: number } | null;
  viewer: ProgramViewerMetadata | undefined;
  selection: SceneSelection;
  selectionLocked: boolean;
  onSelect: (selection: SceneSelection) => void;
  ghost: Pose3Value | null;
  ghostStale: boolean;
  candidatePresentationAllowed: boolean;
  candidatePoseEditable: boolean;
  gizmo: GizmoController;
  onDuplicateProgramResource: (sceneResourceId: string) => Promise<boolean>;
  onRemoveProgramResource: (sceneResourceId: string) => Promise<boolean>;
  onCommitProgramTransform: (
    sceneResourceId: string,
    pose: LogicalScenePose,
    scale: [number, number, number],
    expectedDraftIdentity: { ownerEpoch: string; generation: number },
  ) => Promise<boolean>;
  onEditSelection: (kind: 'owner_object' | 'program_resource', id: string) => void;
  hiddenSelectionKeys: ReadonlySet<string>;
  objectAxesSelectionKeys: ReadonlySet<string>;
  onObjectAxesVisibleIdsChange: (ids: string[], draftPreview: boolean) => void;
  onAxesSizesCommit: (frameSizeM: number, objectSizeM: number) => void;
  motionPreview: MotionPreviewReceipt | null;
  programController: ProgramWorkspaceController;
  programRuntime: ProgramRuntimeController;
  robotControl: RobotControlController;
  correctionGhost: CorrectionGhost | null;
  runtimeEnabled: boolean;
  /** The object panel: the Draft editor while objects are editable, else the read-only live view. */
  objectControlSurface: (editable: boolean) => ReactNode;
  inspectorTarget?: HTMLElement | null;
  inspectorMinimized?: boolean;
  onInspectorMinimizedChange?: (minimized: boolean) => void;
  onInspectorMaximize?: () => void;
  onInspectorPanelChange?: (panel: ViewerInspectorPanel) => void;
  onOpenRuntimeHistory?: () => void;
  runtimeOverlay?: ReactNode;
  executionActive: boolean;
}) {
  const headlessState = useViewerHeadlessState(source, active);
  const observationTransport = useExecutionObservationTransport(source);
  const modelPresentation = useMemo(() => viewerModelPresentation(
    headlessState.snapshot,
    headlessState.transportStatus,
    headlessState.semanticStatus,
    programController.state.transitionStatus === 'accepted',
    sourceModelState,
    manifest,
  ), [
    headlessState.semanticStatus,
    headlessState.snapshot,
    headlessState.transportStatus,
    manifest,
    programController.state.transitionStatus,
    sourceModelState,
  ]);
  // A retained model stays mounted (marked not current) on the same
  // manifest object, so ready <-> retained changes do not rebuild it.
  const modelRetained = modelPresentation.status === 'retained';
  const presentedManifest = modelPresentation.status === 'ready'
    || modelPresentation.status === 'retained'
    ? modelPresentation.manifest
    : null;
  const viewerManifest = useMemo<StructuralResourceState<SceneUrdfManifest>>(
    () => presentedManifest !== null
      ? { status: 'ready', data: presentedManifest }
      : { status: 'loading' },
    [presentedManifest],
  );
  const retryViewerModel = useCallback(() => {
    onRetry();
    onRetrySourceModel();
  }, [onRetry, onRetrySourceModel]);
  const factsResult = useMemo(
    () => sceneOwnerFacts(
      headlessState.snapshot?.runtime ?? null,
      sourceValue,
      environmentValue,
    ),
    [environmentValue, headlessState.snapshot?.runtime, sourceValue],
  );
  const facts = factsResult.available ? factsResult.facts : null;
  const productIdentity = sourceValue === null
    ? null
    : `${sourceValue.retained_bundle_id}\u0000${sourceValue.graph_revision}`;
  const resetPresentationHeld = programRuntime.cycleState === 'resetting';
  const [retainedPresentation, setRetainedPresentation] = useState<
    RetainedSceneOwnerPresentation | null
  >(null);
  const presentationFacts = useMemo(() => sceneOwnerPresentation(
    resetPresentationHeld ? null : facts,
    retainedPresentation,
    productIdentity,
  ), [facts, productIdentity, resetPresentationHeld, retainedPresentation]);
  useLayoutEffect(() => {
    if (!resetPresentationHeld && facts !== null && productIdentity !== null) {
      setRetainedPresentation((current) => (
        current?.productIdentity === productIdentity
        && current.facts.sourceGeneration === facts.sourceGeneration
        && current.facts.sourceOwnerEpoch === facts.sourceOwnerEpoch
        && current.facts.runtimeSourceGeneration === facts.runtimeSourceGeneration
        && current.facts.runtimeSourceOwnerEpoch === facts.runtimeSourceOwnerEpoch
          ? current
          : { productIdentity, facts }
      ));
    }
  }, [facts, productIdentity, resetPresentationHeld]);
  const readyManifest = presentedManifest;
  const objects = presentationFacts?.logicalScene.snapshot?.objects
    ?? EMPTY_LOGICAL_SCENE_OBJECTS;
  const ownerSceneKnown = presentationFacts !== null;
  // Every object edit goes into the Program Draft. The viewer presents the
  // Draft Scene only while it has unapplied changes (the same comparison the
  // top bar uses for Discard); otherwise the runtime Scene, which is edited
  // through the Draft. Objects are editable while Idle and in a run whose
  // Program motion is confirmed stopped (`heldRunDraftEditAllowed`). In such a
  // run with no unapplied changes the live view shows the objects' current
  // positions (the run may have moved them), and an edit writes that object's
  // current pose plus the change into the Draft: a Draft declaration of where
  // the object is from now on. In any other run the Draft cannot be edited
  // and only the live Scene is shown.
  const draftDiffersFromApplied = programController.state.workspace?.hasUnappliedChanges === true;
  const [sceneSourceChoice, setSceneSourceChoice] = useState<'auto' | 'draft' | 'applied'>('auto');
  // The manual toggle is a pure view override that never sticks: it resets
  // whenever the Draft-vs-Applied difference appears or disappears.
  useEffect(() => {
    setSceneSourceChoice('auto');
  }, [draftDiffersFromApplied]);
  const draftPreviewObjects = useMemo(() => (
    ownerSceneKnown && !executionActive
      ? (program?.scene ?? []).map(programScenePreviewObject).filter(
          (object): object is LogicalSceneObject => object !== null,
        )
      : EMPTY_LOGICAL_SCENE_OBJECTS
  ), [executionActive, ownerSceneKnown, program?.scene]);
  const draftSceneEditable = !executionActive && sceneSourceIsDraft(
    'draft', ownerSceneKnown, draftIdentity !== null, programController.mutationAvailable,
    draftDiffersFromApplied,
  );
  const programDraftPreview = !executionActive && sceneSourceIsDraft(
    sceneSourceChoice, ownerSceneKnown, draftIdentity !== null,
    programController.mutationAvailable, draftDiffersFromApplied,
  );
  // The runtime view is editable through the Draft copy only while it equals
  // the Draft; a runtime view chosen over unapplied changes is read-only.
  const objectsEditable = draftSceneEditable
    && (programDraftPreview || !draftDiffersFromApplied);
  const unresolvedDraftObjects = (program?.scene?.length ?? 0) - draftPreviewObjects.length;
  // One identity names the object in both views, so a selection survives the
  // automatic switch when the first edit creates a Draft difference.
  const selectedSceneResourceId = selection === null || selection.kind === 'structure'
    ? null
    : selection.id;
  const selectedStructureName = selection?.kind === 'structure'
    ? selection.linkName
    : null;
  const visibleObjects = useMemo(() => (
    (programDraftPreview ? draftPreviewObjects : objects).filter((object) => {
      const sourceKind = programDraftPreview ? 'program_resource' : 'owner_object';
      if (hiddenSelectionKeys.has(`${sourceKind}:${object.scene_resource_id}`)) return false;
      const resource = programDraftPreview ? program?.scene?.find(
        (candidate) => candidate.scene_resource_id === object.scene_resource_id,
      ) : undefined;
      return !(resource?.resource_roles ?? []).some(
        (role) => viewer?.resource_role_visibility?.[role] === false,
      );
    })
  ), [
    draftPreviewObjects,
    hiddenSelectionKeys,
    objects,
    program?.scene,
    programDraftPreview,
    viewer?.resource_role_visibility,
  ]);
  const motionDescriptor = robotControl.selectedStatus?.directControl?.targetDescriptor ?? null;
  const sceneRuntimePresentation = useMemo(() => runtimeScenePresentation(
    presentationFacts?.sceneRuntime ?? null,
    presentationFacts?.logicalScene ?? null,
    sourceValue,
    readyManifest,
    motionDescriptor,
  ), [motionDescriptor, presentationFacts, readyManifest, sourceValue]);
  const executionObservationPresentation = useMemo(() => {
    const live = createExecutionRobotPresentationReader(
      source,
      sourceValue,
      readyManifest,
      motionDescriptor,
    );
    // A retained robot is not verified: it keeps its last coherent pose
    // instead of following samples while the control transport is unverified.
    return modelRetained
      ? frozenMeasuredRobotPresentationReader(live.readMeasuredRobotPresentation())
      : live;
  }, [modelRetained, motionDescriptor, readyManifest, source, sourceValue]);
  // The measured robot pose comes only from the live observation stream. The
  // control-state copy of observed joints is refreshed only at control changes
  // and motion ends, so it can be older than the robot (and than a pose
  // changed by external forces); it is never a rendering source.
  // The measured-model driver can only subscribe after it is mounted. Wake
  // this parent only while it still needs a first valid observation, so a
  // frame that arrives during neutral bootstrap can promote the model without
  // rerendering the whole authoring surface for every steady-state sample.
  const [observationBootstrapRevision, setObservationBootstrapRevision] = useState(0);
  const lastBootstrapFrame = useRef(source.getLatestExecutionObservation());
  const liveExecutionPresentation = useMemo(
    () => executionObservationPresentation.readMeasuredRobotPresentation(),
    // The revision is the explicit invalidation key: the presentation reads
    // the observation stream, which React cannot observe as a dependency.
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [executionObservationPresentation, observationBootstrapRevision],
  );
  // Wake only until the first usable live frame (or after a model change that
  // leaves none): steady-state samples are applied by the viewer directly.
  const needsObservationBootstrap = liveExecutionPresentation === null
    && sceneRuntimePresentation.measuredRobot === null;
  useEffect(() => {
    if (!needsObservationBootstrap) return undefined;
    const wakeForObservation = () => {
      const frame = source.getLatestExecutionObservation();
      if (frame === null || frame === lastBootstrapFrame.current) return;
      lastBootstrapFrame.current = frame;
      setObservationBootstrapRevision((revision) => revision + 1);
    };
    wakeForObservation();
    const unsubscribe = source.subscribeExecutionObservation(wakeForObservation);
    // The source's legacy unsubscribe returns a boolean; React effects must
    // expose a void cleanup function (the return value is not meaningful to
    // the subscription owner).
    return () => { void unsubscribe(); };
  }, [needsObservationBootstrap, source]);
  const runtimePresentation = useMemo(() => (
    liveExecutionPresentation !== null
      && sceneRuntimePresentation.measuredRobot === null
      ? {
          ...sceneRuntimePresentation,
          measuredRobot: liveExecutionPresentation,
        }
      : sceneRuntimePresentation
  ), [liveExecutionPresentation, sceneRuntimePresentation]);
  const candidates = useMemo(() => {
    if (readyManifest === null || motionDescriptor === null) return null;
    const observation = source.getLatestExecutionObservation();
    // The cold arm-only bootstrap defaults other joints to neutral. A target
    // ghost needs actual tool coverage, not that presentation-only fallback.
    if (candidateObservationEpoch(observation, robotControl.selectedStatus) === null) return null;
    const measured = executionObservationPresentation.readMeasuredRobotPresentation();
    if (measured === null) return null;
    const tool = ghost === null || robotControl.selectedStatus === null || sourceValue === null
        ? null : candidateToolPresentation(
          ghost, robotControl.selectedStatus, sourceValue, readyManifest, measured.linkTransforms,
        );
    const capabilityPreview = robotControl.state.preview?.schemaVersion
      === 'manyforge.direct_control_capability_command_preview.v1'
      ? robotControl.state.preview
      : null;
    const robot = !candidatePresentationAllowed
      || robotControl.state.phase !== 'ready'
      || robotControl.state.poseDragging
      ? null
      : motionPreview !== null
        ? candidateRobotPresentation(
          motionPreview, readyManifest, motionDescriptor, measured.linkTransforms,
        )
        : capabilityPreview !== null && robotControl.selectedStatus !== null && sourceValue !== null
          ? candidateGripperPresentation(
            capabilityPreview,
            robotControl.selectedStatus,
            sourceValue,
            readyManifest,
            measured.linkTransforms,
          )
          : null;
    return { robot, tool };
  }, [
    candidatePresentationAllowed,
    executionObservationPresentation,
    ghost,
    motionDescriptor,
    motionPreview,
    readyManifest,
    robotControl.state.phase,
    robotControl.state.preview,
    robotControl.selectedStatus,
    robotControl.state.poseDragging,
    source,
    sourceValue,
  ]);
  // The correction preview has its own ghost: it is independent of the manual
  // panel's draft and of manual-control availability, and it never gates.
  const correctionRobot = useMemo(() => {
    if (correctionGhost === null || readyManifest === null) return null;
    const measured = executionObservationPresentation.readMeasuredRobotPresentation();
    if (measured === null) return null;
    return resolvedPreviewRobotPresentation(
      correctionGhost.receipt, readyManifest, correctionGhost.descriptor, measured.linkTransforms,
    );
  }, [correctionGhost, executionObservationPresentation, readyManifest]);
  const dragReference = useRef<string | null>(null);
  const readCandidateReference = () => {
    const status = robotControl.selectedStatus;
    // A candidate TCP needs a selected motion target (none for gripper-only).
    const motion = status?.directControl ?? null;
    if (status === null || motion === null || sourceValue === null) return null;
    const observationEpoch = candidateObservationEpoch(source.getLatestExecutionObservation(), status);
    if (observationEpoch === null) return null;
    const frames = motion.targetDescriptor.frames;
    const base = frames.find((frame) => frame.frameRoleId === motion.baseFrameRoleId);
    const tcp = frames.find((frame) => frame.frameRoleId === motion.tcpFrameRoleId);
    const transforms = executionObservationPresentation.readMeasuredRobotPresentation()?.linkTransforms;
    const basePose = base === undefined ? undefined : transforms?.[base.compiledFrameName];
    const tcpPose = tcp === undefined ? undefined : transforms?.[tcp.compiledFrameName];
    if (base === undefined || basePose === undefined || tcpPose === undefined) return null;
    return {
      key: JSON.stringify([observationEpoch, status.retainedBundleId, status.graphRevision,
        motion.programBindingId, base, tcp, basePose, tcpPose]),
      base: { modelInstanceId: base.modelInstanceId, viewerLinkName: base.compiledFrameName,
        poseInCell: poseValueFromTuple({ position: basePose.position_m, quaternion: basePose.orientation_xyzw }) },
      motion,
    };
  };
  const beginCandidateDrag = () => {
    if (!candidatePresentationAllowed) return false;
    const reference = readCandidateReference();
    if (reference === null || !robotControl.beginPoseDrag()) return false;
    dragReference.current = reference.key;
    return true;
  };
  const finishCandidateDrag = (pose: Pose3Value | null) => {
    if (!robotControl.state.poseDragging) {
      dragReference.current = null;
      robotControl.finishPoseDrag(null);
      return;
    }
    const reference = readCandidateReference();
    const valid = candidatePresentationAllowed
      && reference !== null && reference.key === dragReference.current;
    dragReference.current = null;
    if (!valid) {
      robotControl.finishPoseDrag(null);
      robotControl.revert();
      return;
    }
    const control = reference.motion;
    const local = pose === null ? null : candidateTcpPoseFromCell(sourceValue,
      control.controllableComponentId, control.commandTargetId, control.baseFrameRoleId, pose, reference.base);
    robotControl.finishPoseDrag(local?.available ? local.pose : null);
  };
  // The memoized viewer Canvas receives identity-stable drag callbacks that
  // call the latest closures; fresh closures would re-render the R3F root
  // (and force a WebGL frame) on every unrelated shell publication.
  const candidateDragHandlers = useRef({ begin: beginCandidateDrag, finish: finishCandidateDrag });
  useLayoutEffect(() => {
    candidateDragHandlers.current = { begin: beginCandidateDrag, finish: finishCandidateDrag };
  });
  const beginCandidateDragStable = useCallback(
    () => candidateDragHandlers.current.begin(),
    [],
  );
  const finishCandidateDragStable = useCallback(
    (pose: Pose3Value | null) => candidateDragHandlers.current.finish(pose),
    [],
  );
  const frameAxes = useMemo(() => {
    if (sourceValue === null || readyManifest === null) return [];
    const rolesBySourceLink = new Map<string, string[]>();
    const cellRoles = new Set<string>();
    for (const target of sourceValue.targets) {
      for (const frame of target.frames) {
        if (frame.frame_kind === 'cell_frame') {
          cellRoles.add(frame.frame_role_id);
          continue;
        }
        if (frame.model_instance_id === null || frame.source_link_name === null) continue;
        const key = `${frame.model_instance_id}\u0000${frame.source_link_name}`;
        const roles = rolesBySourceLink.get(key) ?? [];
        if (!roles.includes(frame.frame_role_id)) roles.push(frame.frame_role_id);
        rolesBySourceLink.set(key, roles.sort());
      }
    }
    const viewerLinks = new Set(readyManifest.links.map((link) => link.name));
    const viewerJoints = new Map(readyManifest.joints.map((joint) => [joint.name, joint]));
    const modelRows = sourceValue.occurrences.flatMap((occurrence) => {
      const coordinateBySourceChild = new Map(occurrence.coordinates.map(
        (coordinate) => [coordinate.source_child_link_name, coordinate],
      ));
      return occurrence.links.map((link) => {
        const coordinate = coordinateBySourceChild.get(link.source_link_name) ?? null;
        const roles = rolesBySourceLink.get(
          `${occurrence.model_instance_id}\u0000${link.source_link_name}`,
        ) ?? [];
        const viewerJoint = coordinate === null
          ? null
          : viewerJoints.get(coordinate.viewer_joint_name) ?? null;
        return {
          linkName: link.viewer_link_name,
          sourceLinkName: link.source_link_name,
          sourceJointName: coordinate?.source_joint_name ?? null,
          viewerJointName: coordinate?.viewer_joint_name ?? null,
          componentId: occurrence.component_id,
          modelInstanceId: occurrence.model_instance_id,
          frameRoleIds: roles,
          defaultVisible: roles.some(
            (role) => viewer?.frame_role_visibility?.[role] !== false,
          ),
          poseAvailable: viewerLinks.has(link.viewer_link_name)
            && (coordinate === null || (
              viewerJoint?.child_link === coordinate.viewer_child_link_name
              && coordinate.viewer_child_link_name === link.viewer_link_name
            )),
        };
      });
    });
    const sourceLinks = new Set(modelRows.map((row) => row.linkName));
    const currentCellRoles = [...cellRoles].sort();
    return [
      ...(sourceLinks.has(readyManifest.root_link) ? [] : [{
        linkName: readyManifest.root_link,
        sourceLinkName: 'Cell frame',
        sourceJointName: null,
        viewerJointName: null,
        componentId: null,
        modelInstanceId: null,
        frameRoleIds: currentCellRoles,
        defaultVisible: currentCellRoles.some(
          (role) => viewer?.frame_role_visibility?.[role] !== false,
        ),
        poseAvailable: true,
      }]),
      ...modelRows,
    ];
  }, [readyManifest, sourceValue, viewer?.frame_role_visibility]);
  const sourceFrameNames = useMemo(
    () => Object.fromEntries(frameAxes.map((frame) => [
      frame.linkName,
      frame.sourceLinkName,
    ])),
    [frameAxes],
  );
  const selectSceneResource = useCallback((identity: string | null) => {
    if (selectionLocked) return;
    onSelect(identity === null
      ? null
      : {
          kind: programDraftPreview ? 'program_resource' : 'owner_object',
          id: identity,
        });
  }, [onSelect, programDraftPreview, selectionLocked]);
  const selectStructure = useCallback((name: string | null) => {
    if (selectionLocked) return;
    onSelect(name === null
      ? null
      : { kind: 'structure', id: `link:${name}`, linkName: name });
  }, [onSelect, selectionLocked]);
  const editSceneResource = useCallback((identity: string) => {
    onEditSelection(
      programDraftPreview ? 'program_resource' : 'owner_object',
      identity,
    );
  }, [onEditSelection, programDraftPreview]);
  // Objects are edited only through their Draft copy.
  const presentedMutationAvailability = useMemo<
    Record<string, SceneObjectMutationAvailability>
  >(() => (
    objectsEditable
      ? Object.fromEntries(visibleObjects.filter((object) => (program?.scene ?? []).some(
          (resource) => resource.scene_resource_id === object.scene_resource_id
            && resource.pose.frame_ref.kind === 'cell_frame',
        )).map((object) => [object.scene_resource_id, {
          duplicate: true,
          remove: true,
          transform: true,
          shape: true,
          duplicateReason: null,
          removeReason: null,
          transformReason: null,
          shapeReason: null,
        }]))
      : {}
  ), [objectsEditable, program?.scene, visibleObjects]);
  const sceneOwnerEpoch = facts?.sourceOwnerEpoch;
  const sceneOwnerGeneration = facts?.sourceGeneration;
  const sceneOwnerIdentity = useMemo(() => (
    sceneOwnerEpoch === undefined || sceneOwnerGeneration === undefined
      ? undefined
      : {
          ownerEpoch: sceneOwnerEpoch,
          generation: sceneOwnerGeneration,
        }
  ), [sceneOwnerEpoch, sceneOwnerGeneration]);
  const viewerRuntime = headlessState.snapshot?.runtime;
  const viewerFrameOwnerEpoch = viewerRuntime?.projectionOwnerEpoch;
  const viewerFrameGeneration = viewerRuntime?.projectionGeneration;
  const viewerFrameIdentity = useMemo(() => (
    viewerRuntime?.complete === true
      && viewerFrameOwnerEpoch !== undefined
      && viewerFrameGeneration !== undefined ? {
      ownerEpoch: viewerFrameOwnerEpoch,
      generation: viewerFrameGeneration,
    }
      : undefined
  ), [
    viewerFrameGeneration,
    viewerFrameOwnerEpoch,
    viewerRuntime?.complete,
  ]);
  const objectAxesVisibleIds = useMemo(() => visibleObjects.flatMap((object) => {
    const kind = programDraftPreview ? 'program_resource' : 'owner_object';
    return objectAxesSelectionKeys.has(`${kind}:${object.scene_resource_id}`)
      ? [object.scene_resource_id]
      : [];
  }), [objectAxesSelectionKeys, programDraftPreview, visibleObjects]);
  const objectAxesRows = useMemo(
    () => (objects.length === 0 ? draftPreviewObjects : objects).map((object) => {
      const persistent = (program?.scene ?? []).some(
        (resource) => resource.scene_resource_id === object.scene_resource_id,
      );
      return {
        sceneResourceId: object.scene_resource_id,
        attached: object.placement.kind === 'carried',
        poseAvailable: visibleObjects.some(
          (visible) => visible.scene_resource_id === object.scene_resource_id,
        ),
        persistent,
        defaultVisible: persistent
          && viewer?.scene_resource_axes_visibility?.[object.scene_resource_id] === true,
        sourceLabel: programDraftPreview ? 'Program Draft' : 'owner scene',
      };
    }),
    [draftPreviewObjects, objects, program?.scene, programDraftPreview, viewer, visibleObjects],
  );
  const changeObjectAxesVisibleIds = useCallback((ids: string[]) => {
    onObjectAxesVisibleIdsChange(ids, programDraftPreview);
  }, [onObjectAxesVisibleIdsChange, programDraftPreview]);
  useLayoutEffect(() => {
    source.markViewerPresented(headlessState.snapshot);
  }, [headlessState.snapshot, source]);
  // Shown execution groups are a Program setting: the checkboxes edit the
  // Draft, and the Motion panel follows the Applied Program after Apply.
  const draftProgram = programController.state.workspace?.draft.authoring ?? null;
  const draftGroupScopes = useMemo(() => draftProgram === null ? [] : executionGroupScopes(
    draftProgram.forest, Object.keys(draftProgram.execution_groups ?? {}),
  ), [draftProgram]);
  // Robot names come only from the Applied binding, and only while the Draft
  // is exactly that Applied revision; otherwise the Draft's component roles.
  const programWorkspace = programController.state.workspace;
  const appliedSelection = programRuntime.state.applied;
  const draftBindingAuthoritative = programWorkspace?.applied.matchesDraft === true
    && appliedSelection !== null
    && programWorkspace.applied.programRevision === appliedSelection.programRevision
    && programWorkspace.draft.programRevision === appliedSelection.programRevision;
  const draftGroupTargetLabels = useMemo(() => executionGroupTargetLabels(
    draftProgram?.target_roles,
    draftBindingAuthoritative && appliedSelection !== null ? appliedSelection.programBinding : null,
  ), [appliedSelection, draftBindingAuthoritative, draftProgram]);
  const appliedHidden = programRuntime.state.applied?.program.viewer?.hidden_execution_groups;
  const appliedViewer = programRuntime.state.applied?.program.viewer;
  const differs = (read: (viewer: ProgramViewerMetadata | undefined) => unknown) => (
    draftProgram !== null
    && programRuntime.state.applied !== null
    && JSON.stringify(read(draftProgram.viewer) ?? null) !== JSON.stringify(read(appliedViewer) ?? null)
  );
  const programViewerPendingApply = {
    frameAxes: differs((viewer) => viewer?.frame_gizmo_size_m),
    objectAxes: differs((viewer) => viewer?.object_axes_gizmo_size_m)
      || differs((viewer) => viewer?.scene_resource_axes_visibility),
  };
  const draftGroupVisibility: DraftGroupVisibility | null = draftProgram === null ? null : {
    scopes: draftGroupScopes,
    targetLabels: draftGroupTargetLabels,
    hidden: draftProgram.viewer?.hidden_execution_groups,
    pendingApply: JSON.stringify(draftProgram.viewer?.hidden_execution_groups ?? null)
      !== JSON.stringify(appliedHidden ?? null),
    onChange: programController.mutationAvailable
      ? (hidden) => {
          const presentation = viewerPresentationDraftFromProgram(draftProgram.viewer);
          void programController.mutate('viewer_draft_replace', {
            viewer: programViewerMetadata({
              ...presentation,
              hiddenExecutionGroups: hidden,
            }) as unknown as JsonValue,
          });
        }
      : undefined,
  };

  return (
    <Suspense fallback={(
      <section className="panel viewer-panel">
        <div className="viewer-stage viewer-fallback-stage">
          <div className="viewer-state" role="status">
            <strong id="viewer-title">Loading 3D viewport…</strong>
            <span>Scene authoring and runtime controls remain independent.</span>
          </div>
        </div>
      </section>
    )}>
    <StructuralViewer
      viewerActive={active}
      manifest={viewerManifest}
      modelPresentation={modelPresentation}
      onRetry={retryViewerModel}
      sceneObjects={visibleObjects}
      staleScenePreview={!programDraftPreview && presentationFacts !== null && (
        facts === null
        || headlessState.transportStatus !== 'connected'
        || headlessState.semanticStatus !== 'current'
      )}
      programDraftPreview={programDraftPreview}
      sceneSourceControl={draftSceneEditable && draftDiffersFromApplied ? (
        <button type="button" className="viewer-scene-source-toggle"
          aria-label={programDraftPreview
            ? 'Showing the editable Draft Scene. Robot models and observation status come from Applied Deployment. Switch to the read-only Applied Scene.'
            : 'Showing the Applied Scene, read-only while unapplied Draft changes exist. Robot models and observation status come from Applied Deployment. Switch to the editable Draft Scene.'}
          onClick={() => setSceneSourceChoice(programDraftPreview ? 'applied' : 'draft')}
          title={programDraftPreview && unresolvedDraftObjects > 0
            ? `${unresolvedDraftObjects} Program Draft objects need frame or asset resolution. Object edits go into the Draft; robot models and observation status remain from Applied Deployment.`
            : programDraftPreview
              ? 'Program Draft Scene: object edits go into the Draft; robot models and observation status remain from Applied Deployment. Switch to view the live cell.'
              : 'Applied Scene: the live cell, read-only while unapplied Draft changes exist. Switch to the Draft Scene to edit objects.'}>
          {programDraftPreview ? 'Draft Scene' : 'Applied Scene'} <span aria-hidden="true">⇄</span>
        </button>
      ) : null}
      programDraftIdentity={programDraftPreview
        ? draftIdentity ?? undefined
        : undefined}
      selectedSceneResourceId={selectedSceneResourceId}
      onSelectSceneResource={selectSceneResource}
      selectedStructureName={selectedStructureName}
      onSelectStructure={selectStructure}
      onCommitProgramDraftTransform={objectsEditable ? onCommitProgramTransform : undefined}
      programDraftEditIdentity={objectsEditable ? draftIdentity ?? undefined : undefined}
      onDuplicateSceneResource={objectsEditable
        ? onDuplicateProgramResource
        : undefined}
      onRemoveSceneResource={objectsEditable
        ? onRemoveProgramResource
        : undefined}
      onEditSceneResource={editSceneResource}
      selectionLocked={selectionLocked}
      sceneMutationAvailability={presentedMutationAvailability}
      candidateTcpPose={ghost}
      candidateStale={ghostStale}
      candidateTcpEditable={candidatePoseEditable}
      onEditCandidateTcpPose={finishCandidateDragStable}
      onBeginCandidateTcpDrag={beginCandidateDragStable}
      candidateContextKey={JSON.stringify([robotControl.selectedStatus?.ownerEpoch,
        robotControl.selectedStatus?.retainedBundleId, robotControl.selectedStatus?.graphRevision,
        robotControl.selectedStatus?.directControl?.programBindingId, robotControl.selectedStatus?.sceneRevision,
        robotControl.state.mode, robotControl.state.moveToDraft?.source,
        candidatePresentationAllowed, runtimeEnabled])}
      gizmo={gizmo}
      candidateRobot={candidates?.robot ?? null}
      candidateTool={candidates?.tool ?? null}
      candidateRejected={robotControl.state.previewIssue !== null}
      correctionRobot={correctionRobot}
      correctionStale={correctionGhost?.stale ?? false}
      runtimePresentation={runtimePresentation}
      executionObservationPresentation={executionObservationPresentation}
      sceneOwnerIdentity={sceneOwnerIdentity}
      viewerFrameIdentity={viewerFrameIdentity}
      readViewerFrameIdentity={source.getPresentedRuntimeFrame}
      pointCloudCellId={presentationFacts?.logicalScene.snapshot?.cell_id}
      viewer={viewer}
      frameAxes={frameAxes}
      objectAxesVisibleIds={objectAxesVisibleIds}
      objectAxesRows={objectAxesRows}
      onObjectAxesVisibleIdsChange={changeObjectAxesVisibleIds}
      onAxesSizesCommit={onAxesSizesCommit}
      robotControlSurface={(
        <>
          <DraftScenePreviewNotice
            shown={programDraftPreview}
            onShowRuntimeScene={() => setSceneSourceChoice('applied')}
          />
          <RobotControlPanelView
            enabled={runtimeEnabled}
            controller={robotControl}
            program={programRuntime}
            sourceFrameNames={sourceFrameNames}
            controlBasePoseInCell={controlBasePoseInCell}
            runtimeScene={facts?.logicalScene ?? null}
            gizmo={gizmo}
          />
        </>
      )}
      robotControlBlocked={robotControl.state.phase === 'blocked'}
      robotMotionActive={robotControlHasActiveOperation(robotControl.state)}
      objectControlSurface={objectControlSurface(objectsEditable)}
      inspectorTarget={inspectorTarget}
      inspectorMinimized={inspectorMinimized}
      onInspectorMinimizedChange={onInspectorMinimizedChange}
      onInspectorMaximize={onInspectorMaximize}
      onInspectorPanelChange={onInspectorPanelChange}
      transportControls={(openRobotControls, viewerSettings) => (
        <ProgramTransportControls
          enabled={runtimeEnabled}
          program={programRuntime}
          robot={robotControl}
          observationTransport={observationTransport}
          onOpenRobotControls={openRobotControls}
          onOpenRuntimeHistory={onOpenRuntimeHistory}
          onOpenViewerSettings={viewerSettings.open}
          viewerPreviewActive={viewerSettings.previewLayerActive}
        />
      )}
      controlPanelSettings={<MotionControlSettings groups={draftGroupVisibility} />}
      programViewerPendingApply={programViewerPendingApply}
      runtimeOverlay={runtimeOverlay}
    />
    </Suspense>
  );
});

/**
 * Warn that the viewport shows the unapplied Draft Scene, not the collision
 * Scene motion is planned against, and offer the runtime Scene instead.
 */
export function DraftScenePreviewNotice({
  shown,
  onShowRuntimeScene,
}: {
  shown: boolean;
  onShowRuntimeScene: () => void;
}) {
  if (!shown) return null;
  return (
    <p className="central-inspector-notice notice severity-warning notice-strip" role="status">
      Showing Draft Scene preview, not the collision scene used for motion.
      {' '}<button type="button" onClick={onShowRuntimeScene}>Show runtime Scene</button>
    </p>
  );
}

export function SceneAuthoringWorkspace({
  editorTarget,
  environment,
  headlessStateSource,
  manifest,
  onRetryManifest,
  onRetrySourceModel,
  programController,
  programRuntime,
  robotControl,
  correctionGhost = null,
  runtimeEnabled,
  sourceModel,
  viewerActive,
  viewerInspectorTarget = null,
  viewerInspectorMinimized = false,
  onViewerInspectorMinimizedChange,
  onViewerInspectorMaximize,
  onViewerInspectorPanelChange,
  onOpenRuntimeHistory,
  runtimeOverlay,
}: SceneAuthoringWorkspaceProps) {
  const headlessState = useShellHeadlessState(headlessStateSource);
  // Object editing follows the one owner-published rule: a run whose Program
  // motion is confirmed stopped admits Draft edits like Idle does.
  const executionActive = executionLocksObjectEditing(headlessState.snapshot);
  const programWorkspace = programController.state.workspace;
  const program = programWorkspace?.draft.authoring ?? null;
  const viewer = program?.viewer;
  const draftRevision = programWorkspace?.draft.draftRevision ?? 0;
  const draftId = programWorkspace?.draft.draftId ?? null;
  const draftIdentity = useMemo(
    () => draftId === null ? null : { draftId, draftRevision },
    [draftId, draftRevision],
  );
  const environmentValue = environment.status === 'ready' ? environment.data : null;
  const sourceValue = sourceModel.status === 'ready' ? sourceModel.data : null;
  const factsResult = useMemo(
    () => sceneOwnerFacts(
      headlessState.snapshot?.runtime ?? null,
      sourceValue,
      environmentValue,
    ),
    [environmentValue, headlessState.snapshot?.runtime, sourceValue],
  );
  const facts = factsResult.available ? factsResult.facts : null;
  const objects = useMemo(
    () => facts?.logicalScene.snapshot?.objects ?? [],
    [facts],
  );
  const runtimeStatus = useMemo(
    () => runtimeScenePresentation(
      facts?.sceneRuntime ?? null,
      facts?.logicalScene ?? null,
      sourceValue,
      manifest.status === 'ready' ? manifest.data : null,
      robotControl.selectedStatus?.directControl?.targetDescriptor ?? null,
    ).status,
    [facts, manifest, robotControl.selectedStatus, sourceValue],
  );
  const [selectedOccurrenceId, setSelectedOccurrenceId] = useState<string | null>(null);
  const [activeSceneTool, setActiveSceneTool] = useState<SceneWorkspaceTool>('hierarchy');
  const [selection, setSelection] = useState<SceneSelection>(null);
  const ownerObjectPanel = useCardPanelLayout();
  const sceneTabs = useCardPanelLayout();
  const [selectionLocked, setSelectionLocked] = useState(false);
  const [hiddenSelectionKeys, setHiddenSelectionKeys] = useState<Set<string>>(
    () => new Set(),
  );
  const [objectAxesSelectionKeys, setObjectAxesSelectionKeys] = useState<Set<string>>(
    () => new Set(),
  );
  const stableProgramSceneIds = useMemo(
    () => new Set(
      (program?.scene ?? []).map((resource) => resource.scene_resource_id),
    ),
    [program?.scene],
  );
  const stableProgramAxesKey = useMemo(
    () => [...stableProgramSceneIds].sort().map((sceneResourceId) => (
      `${sceneResourceId}:${viewer?.scene_resource_axes_visibility?.[sceneResourceId] === true}`
    )).join('\u0000'),
    [stableProgramSceneIds, viewer?.scene_resource_axes_visibility],
  );
  const gizmo = useGizmoSettings();
  useEffect(() => {
    const occurrences = sourceValue?.occurrences ?? [];
    if (!occurrences.some((item) => item.model_instance_id === selectedOccurrenceId)) {
      setSelectedOccurrenceId(occurrences[0]?.model_instance_id ?? null);
    }
  }, [selectedOccurrenceId, sourceValue]);
  useEffect(() => {
    setObjectAxesSelectionKeys((current) => {
      const next = new Set(current);
      for (const sceneResourceId of stableProgramSceneIds) {
        for (const kind of ['program_resource', 'owner_object'] as const) {
          const key = `${kind}:${sceneResourceId}`;
          if (viewer?.scene_resource_axes_visibility?.[sceneResourceId] === true) {
            next.add(key);
          } else {
            next.delete(key);
          }
        }
      }
      return next;
    });
  }, [stableProgramAxesKey, stableProgramSceneIds, viewer?.scene_resource_axes_visibility]);
  const currentAxesIdentityKey = useMemo(() => [
    ...objects.map((object) => `owner_object:${object.scene_resource_id}`),
    ...(program?.scene ?? []).map(
      (resource) => `program_resource:${resource.scene_resource_id}`,
    ),
  ].sort().join('\u0000'), [objects, program?.scene]);
  useEffect(() => {
    const currentIdentities = new Set(
      currentAxesIdentityKey === '' ? [] : currentAxesIdentityKey.split('\u0000'),
    );
    setObjectAxesSelectionKeys((current) => new Set(
      [...current].filter((key) => currentIdentities.has(key)),
    ));
  }, [currentAxesIdentityKey]);
  useEffect(() => {
    if (selection === null || selection.kind === 'structure') return;
    const exists = selection.kind === 'owner_object'
      ? objects.some((object) => object.scene_resource_id === selection.id)
      : (program?.scene ?? []).some(
          (resource) => resource.scene_resource_id === selection.id,
        );
    if (!exists && !(selectionLocked && selection.kind === 'owner_object')) {
      setSelection(null);
    }
  }, [objects, program?.scene, selection, selectionLocked]);
  const selectedOwnerObjectId = selection?.kind === 'owner_object' ? selection.id : null;
  const selectedObject = objects.find(
    (object) => object.scene_resource_id === selectedOwnerObjectId,
  ) ?? null;
  const directControl = robotControl.selectedStatus?.directControl ?? null;
  const controlManifest = manifest.status === 'ready' ? manifest.data : null;
  const observedRobotForControl = useMemo(() => executionRobotPresentation(
    robotControl.selectedStatus,
    sourceValue,
    controlManifest,
  ), [controlManifest, robotControl.selectedStatus, sourceValue]);
  const baseDescriptor = directControl?.targetDescriptor.frames.find(
    (frame) => frame.frameRoleId === directControl.baseFrameRoleId,
  ) ?? null;
  const baseTransform = baseDescriptor === null
    ? null
    : observedRobotForControl?.linkTransforms[baseDescriptor.compiledFrameName] ?? null;
  const candidateViewerBase = useMemo(() => (
    baseDescriptor === null || baseTransform === null
      ? null
      : {
        modelInstanceId: baseDescriptor.modelInstanceId,
        viewerLinkName: baseDescriptor.compiledFrameName,
        poseInCell: {
          position_m: {
            x_m: baseTransform.position_m[0],
            y_m: baseTransform.position_m[1],
            z_m: baseTransform.position_m[2],
          },
          orientation: {
            x: baseTransform.orientation_xyzw[0],
            y: baseTransform.orientation_xyzw[1],
            z: baseTransform.orientation_xyzw[2],
            w: baseTransform.orientation_xyzw[3],
          },
        },
      }
  ), [baseDescriptor, baseTransform]);
  const controlBasePoseInCell = useMemo<PoseTuple | null>(() => (
    candidateViewerBase === null
      ? null
      : {
          position: [
            candidateViewerBase.poseInCell.position_m.x_m,
            candidateViewerBase.poseInCell.position_m.y_m,
            candidateViewerBase.poseInCell.position_m.z_m,
          ],
          quaternion: [
            candidateViewerBase.poseInCell.orientation.x,
            candidateViewerBase.poseInCell.orientation.y,
            candidateViewerBase.poseInCell.orientation.z,
            candidateViewerBase.poseInCell.orientation.w,
          ],
        }
  ), [candidateViewerBase]);
  const tcpRole = directControl?.tcpFrameRoleId ?? null;
  const candidatePresentationAllowed = robotControlCandidatePresentationAllowed(
    robotControl.state,
    robotControl.manualControlAllowed,
  );
  const ghostVisible = tcpRole === null
    || viewer?.frame_role_visibility?.[tcpRole] !== false;
  const previewGhost = robotControl.state.ghost;
  const tcpDraftMode = robotControl.state.mode === 'pose_goal'
    || robotControl.state.mode === 'cartesian_path'
    || (robotControl.state.mode === 'move_to'
      && robotControl.state.moveToKind !== 'joint');
  const candidateTcp = tcpDraftMode
      ? robotControlHasLocalDraft(robotControl.state)
        ? robotControl.state.draftPose
        : robotControl.selectedStatus?.observedMotion?.currentTcpPose ?? null
      : previewGhost?.finalTcpPose ?? null;
  const cellGhost = useMemo(() => candidateTcp === null
    ? null
    : candidateTcpPoseInCell(
        sourceValue,
        directControl?.controllableComponentId ?? null,
        directControl?.commandTargetId ?? null,
        directControl?.baseFrameRoleId ?? null,
        candidateTcp,
        candidateViewerBase,
      ), [candidateTcp, candidateViewerBase, directControl, sourceValue]);
  const ghost = candidatePresentationAllowed
    && tcpDraftMode && ghostVisible && cellGhost?.available
    ? cellGhost.pose
    : null;
  const candidatePoseEditable = candidatePresentationAllowed
    && ghost !== null
    && (robotControl.state.mode === 'pose_goal'
      || robotControl.state.mode === 'cartesian_path'
      || (robotControl.state.mode === 'move_to' && robotControl.state.moveToDraft?.source.kind === 'pose'))
    && !robotControl.authoringPending
    && robotControl.currentStatus !== null;
  const busyDraft = programController.state.status === 'loading'
    || (programController.state.status === 'ready'
      && programController.state.pendingAction !== null);
  // Program Draft editors are authoring: blocked while busy and whenever the
  // shared authoring gate is closed (for example while execution is not Idle).
  const draftEditBlocked = busyDraft || !programController.mutationAvailable;
  const draftEditBlockReason = busyDraft
    ? 'Another Program operation is in progress.'
    : programController.mutationUnavailableReason;
  const duplicateProgramResource = useCallback(async (sceneResourceId: string) => {
    const resource = program?.scene?.find(
      (candidate) => candidate.scene_resource_id === sceneResourceId,
    );
    if (resource === undefined || busyDraft) return false;
    const identities = new Set(
      (program?.scene ?? []).map((candidate) => candidate.scene_resource_id),
    );
    let identity = `${sceneResourceId}_copy`;
    let suffix = 2;
    while (identities.has(identity)) identity = `${sceneResourceId}_copy_${suffix++}`;
    const duplicate = structuredClone(resource);
    duplicate.scene_resource_id = identity;
    duplicate.pose.position_m = [
      duplicate.pose.position_m[0] + 0.05,
      duplicate.pose.position_m[1] + 0.05,
      duplicate.pose.position_m[2],
    ];
    const response = await programController.mutate('scene_draft_add_object', {
      scene_resource: duplicate as unknown as JsonValue,
    });
    if (response !== null) setSelection({ kind: 'program_resource', id: identity });
    return response !== null;
  }, [busyDraft, program?.scene, programController]);

  const removeProgramResource = useCallback(async (sceneResourceId: string) => {
    if (
      busyDraft
      || !program?.scene?.some((candidate) => candidate.scene_resource_id === sceneResourceId)
    ) return false;
    const response = await programController.mutate('scene_draft_remove_objects', {
      scene_resource_ids: [sceneResourceId],
    });
    if (response !== null && selection?.kind === 'program_resource'
      && selection.id === sceneResourceId) setSelection(null);
    return response !== null;
  }, [busyDraft, program?.scene, programController, selection]);

  const commitProgramTransform = useCallback(async (
    sceneResourceId: string,
    pose: LogicalScenePose,
    scale: [number, number, number],
    expectedDraftIdentity: { ownerEpoch: string; generation: number },
  ) => {
    if (
      busyDraft
      || draftId === null
      || expectedDraftIdentity.ownerEpoch !== draftId
      || expectedDraftIdentity.generation !== draftRevision
    ) return false;
    const resource = program?.scene?.find(
      (candidate) => candidate.scene_resource_id === sceneResourceId,
    );
    if (resource === undefined) return false;
    const transformed = programResourceWithViewportTransform(resource, pose, scale);
    if (transformed === null) return false;
    return await programController.mutate('scene_draft_update_object', {
      scene_resource: transformed as unknown as JsonValue,
    }) !== null;
  }, [busyDraft, draftId, draftRevision, program?.scene, programController]);

  // The viewer edits objects only through the Program Draft editor; the live
  // (Applied) Scene is shown read-only.
  const programSceneEditor = program === null ? null : (
    <ProgramSceneEditor
      program={program}
      draftRevision={draftRevision}
      busy={draftEditBlocked}
      busyReason={draftEditBlockReason}
      mutate={programController.mutate}
      liveObjects={objects}
      selectedResourceId={selection === null || selection.kind === 'structure'
        ? null
        : selection.id}
      onSelectResource={(identity) => setSelection(identity === null
        ? null
        : { kind: 'program_resource', id: identity })}
    />
  );

  const liveObjectPanel = objects.length === 0 ? undefined : (
    <div ref={ownerObjectPanel} className="viewer-object-owner-editor dpanel">
      {!factsResult.available && (
        <div className="panel-state error-state" role="alert">
          <strong>Current scene is unavailable</strong>
          <span>{factsResult.reason}</span>
        </div>
      )}
      {facts !== null && (
        <>
          <section className="dcard">
          <h4>Object</h4>
          <p className="scene-contract-note hint" role="status">
            {programController.mutationAvailable
              ? 'Applied Scene: the live cell, read-only while unapplied Draft changes exist. Switch the viewer to the Draft Scene to edit objects.'
              : `Read-only: ${programController.mutationUnavailableReason ?? 'The Program Draft cannot be edited now.'}`}
          </p>
          <label>
          <span>Scene object</span>
          <select
            aria-label="Current scene object"
            value={selectedOwnerObjectId ?? ''}
            disabled={selectionLocked}
            onChange={(event) => setSelection(event.target.value === ''
              ? null
              : { kind: 'owner_object', id: event.target.value })}
          >
            <option value="">Select a current scene object</option>
            {objects.map((object) => (
              <option key={object.scene_resource_id} value={object.scene_resource_id}>
                {object.scene_resource_id} · {object.placement.kind}
              </option>
            ))}
          </select>
          </label>
          {runtimeStatus.warnings.map((warning) => (
            <p className="scene-contract-note inline-message severity-warning" key={warning} role="status">
              Measured overlay: {warning}
            </p>
          ))}
          </section>
          {selectedObject !== null && (
            <details className="dcard">
              <summary>Ownership, contacts, and collision metadata</summary>
              <dl>
                <div><dt>Placement</dt><dd>{selectedObject.placement.kind.replaceAll('_', ' ')}</dd></div>
                <div><dt>Ownership</dt><dd>{selectedObject.provider_evidence === null
                  ? 'Logical-scene owner'
                  : 'External provider'}</dd></div>
                <div><dt>Shapes</dt><dd>{selectedObject.collision_shapes.map((shape) => shape.type).join(', ') || 'none'}</dd></div>
                <div><dt>Support contacts</dt><dd>{selectedObject.support_contacts.length}</dd></div>
              </dl>
            </details>
          )}
          <details className="dcard">
            <summary>Owner and runtime source facts</summary>
            <dl>
              <div><dt>Scene source</dt><dd><code>{facts.sourceOwnerEpoch}</code> / {facts.sourceGeneration}</dd></div>
              <div><dt>Runtime source</dt><dd><code>{facts.runtimeSourceOwnerEpoch}</code> / {facts.runtimeSourceGeneration}</dd></div>
              <div><dt>Scene revision</dt><dd><code>{facts.logicalScene.snapshot?.revision.revision_id}</code></dd></div>
              <div><dt>Observations</dt><dd>{runtimeStatus.freshGood} fresh / {runtimeStatus.total} total</dd></div>
            </dl>
          </details>
          <details className="dcard">
            <summary>Active collision policy</summary>
            <pre className="dcode">{JSON.stringify(
              facts.logicalScene.snapshot?.configured_collision_policy ?? {},
              null,
              2,
            )}</pre>
          </details>
          {selectedObject !== null && (
            <section className="dcard">
            <h4>Draft</h4>
            <div className="dactions">
              <CopyObservedToDraftButton
                object={selectedObject}
                blockedReason={draftEditBlocked ? draftEditBlockReason : null}
                mutate={programController.mutate}
              />
            </div>
            </section>
          )}
        </>
      )}
    </div>
  );

  const viewerSurface = (
    <SceneViewerBridge
      active={viewerActive}
      executionActive={executionActive}
      source={headlessStateSource}
      sourceValue={sourceValue}
      sourceModelState={sourceModel}
      controlBasePoseInCell={controlBasePoseInCell}
      environmentValue={environmentValue}
      manifest={manifest}
      onRetry={onRetryManifest}
      onRetrySourceModel={onRetrySourceModel}
      program={program}
      draftIdentity={draftIdentity}
      viewer={viewer}
      selection={selection}
      selectionLocked={selectionLocked}
      onSelect={setSelection}
      ghost={ghost}
      ghostStale={robotControl.state.ghostStale || robotControl.state.phase === 'blocked'}
      candidatePresentationAllowed={candidatePresentationAllowed}
      candidatePoseEditable={candidatePoseEditable}
      gizmo={gizmo}
      onDuplicateProgramResource={duplicateProgramResource}
      onRemoveProgramResource={removeProgramResource}
      onCommitProgramTransform={commitProgramTransform}
      onEditSelection={(kind, identity) => {
        setSelection({ kind, id: identity });
        setActiveSceneTool(kind === 'program_resource' ? 'program' : 'current');
      }}
      hiddenSelectionKeys={hiddenSelectionKeys}
      objectAxesSelectionKeys={objectAxesSelectionKeys}
      onObjectAxesVisibleIdsChange={(ids, draftPreview) => {
        const kind = draftPreview ? 'program_resource' : 'owner_object';
        const currentObjectIds = new Set(
          (draftPreview ? program?.scene ?? [] : objects)
            .map((object) => object.scene_resource_id),
        );
        const selected = new Set(ids);
        setObjectAxesSelectionKeys((current) => {
          const next = new Set([...current].filter((key) => {
            const prefix = `${kind}:`;
            return !key.startsWith(prefix)
              || !currentObjectIds.has(key.slice(prefix.length));
          }));
          for (const identity of selected) next.add(`${kind}:${identity}`);
          return next;
        });
        if (program === null || !programController.mutationAvailable) return;
        const presentation = viewerPresentationDraftFromProgram(program.viewer);
        let changed = false;
        for (const sceneResourceId of currentObjectIds) {
          if (!stableProgramSceneIds.has(sceneResourceId)) continue;
          const visible = selected.has(sceneResourceId);
          if (presentation.sceneResourceAxesVisibility[sceneResourceId] !== visible) {
            presentation.sceneResourceAxesVisibility[sceneResourceId] = visible;
            changed = true;
          }
        }
        if (changed) {
          void programController.mutate('viewer_draft_replace', {
            viewer: programViewerMetadata(presentation) as unknown as JsonValue,
          });
        }
      }}
      onAxesSizesCommit={(frameSizeM, objectSizeM) => {
        if (program === null || !programController.mutationAvailable) return;
        const presentation = viewerPresentationDraftFromProgram(program.viewer);
        void programController.mutate('viewer_draft_replace', {
          viewer: programViewerMetadata({
            ...presentation,
            frameGizmoSizeM: frameSizeM,
            objectAxesGizmoSizeM: objectSizeM,
          }) as unknown as JsonValue,
        });
      }}
      motionPreview={previewGhost}
      programController={programController}
      programRuntime={programRuntime}
      robotControl={robotControl}
      correctionGhost={correctionGhost}
      runtimeEnabled={runtimeEnabled}
      objectControlSurface={(editable) => (editable ? programSceneEditor : liveObjectPanel)}
      inspectorTarget={viewerInspectorTarget}
      inspectorMinimized={viewerInspectorMinimized}
      onInspectorMinimizedChange={onViewerInspectorMinimizedChange}
      onInspectorMaximize={onViewerInspectorMaximize}
      onInspectorPanelChange={onViewerInspectorPanelChange}
      onOpenRuntimeHistory={onOpenRuntimeHistory}
      runtimeOverlay={runtimeOverlay}
    />
  );

  const editorSurface = (
      <section className="panel scene-authoring-workspace" aria-labelledby="scene-authoring-title">
        <header className="panel-header scene-workspace-header">
          <strong id="scene-authoring-title">Scene</strong>
          <nav ref={sceneTabs} className="scene-workspace-tabs dseg" aria-label="Scene tools">
            {([
              ['hierarchy', 'Hierarchy'],
              ['program', 'Program scene'],
              ['contacts', 'Contact policies'],
              ['current', 'Current scene'],
              ['sources', 'Sources'],
              ['viewer', 'Viewer'],
            ] as const).map(([id, label]) => (
              <button
                type="button"
                key={id}
                className={activeSceneTool === id ? 'active' : undefined}
                aria-pressed={activeSceneTool === id}
                onClick={() => setActiveSceneTool(id)}
              >
                {label}
              </button>
            ))}
          </nav>
          <label className="toggle-control scene-selection-lock">
            <input
              type="checkbox"
              checked={selectionLocked}
              onChange={(event) => setSelectionLocked(event.target.checked)}
            />
            <span>Lock selection</span>
          </label>
        </header>

        {activeSceneTool === 'sources' && sourceModel.status === 'error' && (
          <div className="panel-state error-state" role="alert">
            <strong>Source-model projection unavailable</strong>
            <span>{sourceModel.message}</span>
            <button type="button" onClick={onRetrySourceModel}>Retry source facts</button>
          </div>
        )}
        {activeSceneTool === 'sources'
          && sourceValue === null && sourceModel.status !== 'error' && (
          <div className="panel-state" role="status">
            <strong>Source-model facts are not available yet.</strong>
            <span>{sourceModel.status === 'absent'
              ? sourceModel.message
              : 'Waiting for the current source-model projection.'}</span>
            <button type="button" onClick={onRetrySourceModel}>Refresh source facts</button>
          </div>
        )}
        {activeSceneTool === 'sources' && sourceValue !== null && (
          <SourceOccurrencePanel
            occurrences={sourceValue.occurrences}
            runtime={facts?.sceneRuntime ?? null}
            selectedId={selectedOccurrenceId}
            occurrence={sourceValue.occurrences.find(
              (item) => item.model_instance_id === selectedOccurrenceId,
            ) ?? null}
            onSelect={setSelectedOccurrenceId}
          />
        )}

        {activeSceneTool === 'hierarchy' && (
          <SceneHierarchy
            manifest={manifest.status === 'ready' ? manifest.data : null}
            sourceModel={sourceValue}
            programResources={program?.scene ?? EMPTY_PROGRAM_RESOURCES}
            objects={objects}
            attachments={facts?.logicalScene.snapshot?.carried_object_attachments ?? []}
            selection={selection}
            hiddenSelectionKeys={hiddenSelectionKeys}
            objectAxesSelectionKeys={objectAxesSelectionKeys}
            selectionLocked={selectionLocked}
            onSelect={(next) => {
              if (!selectionLocked) setSelection(next);
            }}
            onObjectVisibilityChange={(identity, visible) => {
              setHiddenSelectionKeys((current) => {
                const next = new Set(current);
                if (visible) next.delete(identity);
                else next.add(identity);
                return next;
              });
            }}
            onObjectAxesVisibilityChange={(identity, visible) => {
              setObjectAxesSelectionKeys((current) => {
                const next = new Set(current);
                if (visible) next.add(identity);
                else next.delete(identity);
                return next;
              });
            }}
          />
        )}

        {(activeSceneTool === 'program'
          || activeSceneTool === 'contacts'
          || activeSceneTool === 'viewer')
          && program === null && (
          <div className="panel-state">
            <strong>No Program Draft is loaded.</strong>
            <span>Open a Program before authoring its initial scene or viewer metadata.</span>
          </div>
        )}
        {activeSceneTool === 'program' && programSceneEditor}
        {activeSceneTool === 'contacts' && program !== null && (
          <ProgramContactPolicyEditor
            program={program}
            draftRevision={draftRevision}
            busy={draftEditBlocked}
            busyReason={draftEditBlockReason}
            mutate={programController.mutate}
          />
        )}
        {activeSceneTool === 'viewer' && program !== null && (
          <ViewerMetadataEditor
            program={program}
            draftRevision={draftRevision}
            busy={draftEditBlocked}
            busyReason={draftEditBlockReason}
            mutate={programController.mutate}
          />
        )}

        {activeSceneTool === 'viewer'
          && ghostVisible && cellGhost !== null && !cellGhost.available && (
          <p className="scene-contract-note inline-message severity-warning" role="status">
            Candidate marker unavailable: {cellGhost.reason}
          </p>
        )}

        {activeSceneTool === 'current' && (
          <div data-testid="current-scene-editor">
            {liveObjectPanel ?? (
              <div className="panel-state">
                <strong>No current scene objects</strong>
                <span>The logical-scene owner reports an empty object inventory.</span>
              </div>
            )}
          </div>
        )}
      </section>
  );

  return (
    <>
      {viewerSurface}
      {editorTarget === null ? null : createPortal(editorSurface, editorTarget)}
    </>
  );
}
