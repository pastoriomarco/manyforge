import {
  Fragment,
  lazy,
  memo,
  Suspense,
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useState,
} from 'react';
import type { ReactNode } from 'react';
import { createPortal } from 'react-dom';

import { api } from '../api/client';
import { useShellHeadlessState, useViewerHeadlessState } from '../hooks/useHeadlessState';
import type { HeadlessStateSource } from '../hooks/useHeadlessState';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { RobotControlController } from '../hooks/useRobotControl';
import { ProgramTransportControls } from './ProgramRuntimePanel';
import { RobotControlPanelView } from './RobotControlPanel';
import type { ViewerInspectorPanel } from './StructuralViewer';
import {
  candidateTcpPoseInCell,
  livePoseFromProgramResource,
  programResourceWithLivePose,
  sceneOwnerFacts,
  sceneAttachTransaction,
  sceneObjectTransformTransaction,
  sceneDetachTransaction,
  sceneDuplicateTransaction,
  scenePoseTransaction,
  sceneRemoveTransaction,
  type SceneOwnerFacts,
} from '../state/sceneAuthoring';
import {
  candidateRobotPresentation,
  runtimeScenePresentation,
} from '../state/runtimeScenePresentation';
import {
  quaternionFromRpyDegrees,
  robotControlHasActiveOperation,
} from '../state/robotControl';
import type { StructuralResourceState } from '../state/structuralResource';
import {
  programViewerMetadata,
  viewerPresentationDraftFromProgram,
  viewerPresentationDraftReducer,
} from '../state/viewerPresentationDraft';
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
  SceneTransactionRequest,
  SceneRuntimePoseSource,
  SceneRuntimeProjection,
  SourceModelProjection,
  SourceModelOccurrence,
} from '../types/api';
import type { MotionPreviewReceipt, Pose3Value } from '../types/robotControl';

const StructuralViewer = lazy(async () => {
  const module = await import('./StructuralViewer');
  return { default: module.StructuralViewer };
});

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
  runtimeEnabled: boolean;
  sourceModel: StructuralResourceState<SourceModelProjection>;
  viewerActive: boolean;
  viewerInspectorTarget?: HTMLElement | null;
  viewerInspectorMinimized?: boolean;
  onViewerInspectorMinimizedChange?: (minimized: boolean) => void;
  onViewerInspectorMaximize?: () => void;
  onViewerInspectorPanelChange?: (panel: ViewerInspectorPanel) => void;
}

const EMPTY_PROGRAM_RESOURCES: ProgramSceneResource[] = [];

type SceneSelection =
  | { kind: 'owner_object'; id: string }
  | { kind: 'program_resource'; id: string }
  | { kind: 'structure'; id: string; linkName: string }
  | null;

type SceneWorkspaceTool = 'hierarchy' | 'program' | 'current' | 'sources' | 'viewer';

function sceneSelectionKey(selection: SceneSelection): string | null {
  return selection === null ? null : `${selection.kind}:${selection.id}`;
}

function requestId(): string {
  return globalThis.crypto?.randomUUID?.()
    ?? `composer-scene-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
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

function objectEditReason(object: LogicalSceneObject): string | null {
  if (object.placement.kind === 'carried') {
    return 'Carried-object placement is attachment-owned and cannot be edited here.';
  }
  if (object.provider_evidence !== null) {
    return 'This pose is provider-owned; Composer cannot replace provider evidence.';
  }
  if (object.collision_shapes.length === 0) {
    return 'This object has no collision shape.';
  }
  return null;
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
    <div className="scene-hierarchy" data-testid="scene-hierarchy">
      <div className="scene-authoring-heading">
        <div>
          <p className="eyebrow">Current structure + authoring sources</p>
          <h3>Scene hierarchy</h3>
        </div>
        <span className="scene-hierarchy-count">
          {objects.length} objects · {attachments.length} carried
        </span>
      </div>
      <input
        type="search"
        value={query}
        placeholder="Find link, frame, or object"
        aria-label="Find scene hierarchy item"
        onChange={(event) => setQuery(event.target.value)}
      />
      <details open>
        <summary>Owner scene objects</summary>
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
                    ? 'selected'
                    : undefined}
                  onClick={() => onSelect({
                    kind: 'owner_object',
                    id: object.scene_resource_id,
                  })}
                >
                  <strong>{object.scene_resource_id}</strong>
                  <span>{attachment === undefined
                    ? object.placement.kind
                    : `carried by ${attachment.controllable_component_id} / ${attachment.carrier_frame_ref.frame_role_id}`}</span>
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
      <details>
        <summary>Program Draft initial scene</summary>
        <div className="scene-hierarchy-items">
          {programResources.filter((resource) => matches(resource.scene_resource_id)).map((resource) => (
            <div className="scene-hierarchy-item" key={resource.scene_resource_id}>
              <button
                type="button"
                disabled={selectionLocked && selectedKey !== `program_resource:${resource.scene_resource_id}`}
                className={selectedKey === `program_resource:${resource.scene_resource_id}`
                  ? 'selected'
                  : undefined}
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
      <details>
        <summary>Retained robot links and frames</summary>
        <div className="scene-hierarchy-items scene-hierarchy-static">
          {(manifest?.links ?? []).filter((link) => matches(link.name)).map((link) => (
            <button
              type="button"
              key={link.name}
              disabled={selectionLocked && selectedKey !== `structure:link:${link.name}`}
              className={selectedKey === `structure:link:${link.name}` ? 'selected' : undefined}
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
              className={selectedKey === `structure:frame:${frame.key}` ? 'selected' : undefined}
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

function AttachObjectControls({
  object,
  sourceModel,
  disabled,
  onAttach,
}: {
  object: LogicalSceneObject;
  sourceModel: SourceModelProjection | null;
  disabled: boolean;
  onAttach: (
    target: {
      componentId: string;
      targetId: string;
      frameRoleId: string;
      touchBodyRoles: string[];
    },
    attachmentRevisions: string[],
  ) => Promise<boolean>;
}) {
  const candidates = useMemo(() => (sourceModel?.targets ?? []).flatMap((target) => (
    target.frames
      .filter((frame) => frame.frame_kind === 'component_model_frame')
      .map((frame) => ({
        id: `${target.component_id}\u0000${target.target_id}\u0000${frame.frame_role_id}`,
        componentId: target.component_id,
        targetId: target.target_id,
        frameRoleId: frame.frame_role_id,
        touchBodyRoles: target.collision_bodies.map(
          (body) => body.collision_body_role_id,
        ).sort(),
      }))
  )), [sourceModel]);
  const [candidateId, setCandidateId] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    if (!candidates.some((candidate) => candidate.id === candidateId)) {
      setCandidateId(candidates[0]?.id ?? '');
    }
  }, [candidateId, candidates]);
  const selected = candidates.find((candidate) => candidate.id === candidateId) ?? null;
  return (
    <div className="scene-attach-controls">
      <label>
        <span>Carrier target / frame</span>
        <select
          value={candidateId}
          disabled={disabled || candidates.length === 0 || busy}
          onChange={(event) => setCandidateId(event.target.value)}
        >
          {candidates.length === 0 && <option value="">No compatible carrier target</option>}
          {candidates.map((candidate) => (
            <option value={candidate.id} key={candidate.id}>
              {candidate.componentId} · {candidate.targetId} · {candidate.frameRoleId}
            </option>
          ))}
        </select>
      </label>
      {selected !== null && (
        <span className="muted">
          Touch bodies: {selected.touchBodyRoles.join(', ') || 'none advertised'}
        </span>
      )}
      <button
        type="button"
        disabled={disabled || selected === null || busy}
        onClick={() => {
          if (selected === null) return;
          setBusy(true);
          setError(null);
          void api.manager.graph().then(async (graph) => {
            const raw = graph.cellGraph.component_attachments;
            if (!Array.isArray(raw)) {
              throw new Error('Manager graph omits component-attachment revisions.');
            }
            const revisions = raw.map((entry) => {
              if (
                entry === null || typeof entry !== 'object' || Array.isArray(entry)
                || typeof entry.component_attachment_revision !== 'string'
              ) throw new Error('Manager graph has an invalid component attachment.');
              return entry.component_attachment_revision;
            });
            await onAttach(selected, [...new Set(revisions)].sort());
          }).catch((reason: unknown) => {
            setError(reason instanceof Error ? reason.message : 'Attach failed.');
          }).finally(() => setBusy(false));
        }}
      >
        Attach {object.scene_resource_id}
      </button>
      {error !== null && <span className="scene-commit-error" role="alert">{error}</span>}
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
      {state.kind === 'error' && <span className="scene-commit-error" role="alert">{state.message}</span>}
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

function ProgramSceneEditor({
  program,
  draftRevision,
  busy,
  mutate,
  selectedResourceId,
  onSelectResource,
}: {
  program: ProgramAuthoringDocument;
  draftRevision: number;
  busy: boolean;
  mutate: ProgramWorkspaceController['mutate'];
  selectedResourceId: string | null;
  onSelectResource: (sceneResourceId: string | null) => void;
}) {
  const resources = program.scene ?? EMPTY_PROGRAM_RESOURCES;
  const selected = resources.find(
    (resource) => resource.scene_resource_id === selectedResourceId,
  ) ?? null;
  const [staged, setStaged] = useState<ProgramSceneResource | null>(selected);
  const [meshBrowserOpen, setMeshBrowserOpen] = useState(false);

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
  const setShapeType = (type: 'box' | 'sphere' | 'cylinder' | 'mesh') => {
    if (staged === null) return;
    const shape = type === 'box'
      ? { type, box_dimensions_m: [0.1, 0.1, 0.1] as [number, number, number] }
      : type === 'sphere'
        ? { type, sphere_radius_m: 0.05 }
        : type === 'cylinder'
          ? { type, cylinder_radius_m: 0.05, cylinder_height_m: 0.1 }
          : { type, mesh_resource_uri: '', mesh_scale: [1, 1, 1] as [number, number, number] };
    setStaged({ ...staged, shape });
  };
  const setBoxDimension = (index: number, value: number) => {
    if (staged?.shape.type !== 'box' || !Number.isFinite(value)) return;
    const dimensions = [...staged.shape.box_dimensions_m] as [number, number, number];
    dimensions[index] = value;
    setStaged({ ...staged, shape: { ...staged.shape, box_dimensions_m: dimensions } });
  };
  const setMeshScale = (index: number, value: number) => {
    if (staged?.shape.type !== 'mesh' || !Number.isFinite(value)) return;
    const scale = [...(staged.shape.mesh_scale ?? [1, 1, 1])] as [number, number, number];
    scale[index] = value;
    setStaged({ ...staged, shape: { ...staged.shape, mesh_scale: scale } });
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
      className="scene-authoring-column"
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
      <div className="scene-authoring-heading">
        <div>
          <p className="eyebrow">Program Draft</p>
          <h3>Initial scene</h3>
        </div>
        <button
          type="button"
          disabled={busy}
          onPointerDown={(event) => event.preventDefault()}
          onClick={() => addResource(defaultResource(
            new Set(resources.map((resource) => resource.scene_resource_id)),
          ))}
        >
          Add box
        </button>
      </div>
      <p className="muted">
        These resources belong to the editable Program. They are not the current runtime scene.
      </p>
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
      {staged !== null && (
        <>
          <label className="scene-shape-select">
            <span>Shape</span>
            <select
              value={staged.shape.type}
              disabled={busy}
              onChange={(event) => setShapeType(
                event.target.value as 'box' | 'sphere' | 'cylinder' | 'mesh',
              )}
            >
              <option value="box">Box</option>
              <option value="sphere">Sphere</option>
              <option value="cylinder">Cylinder</option>
              <option value="mesh">Mesh</option>
            </select>
          </label>
          {staged.shape.type === 'box' && (
            <div className="scene-pose-grid">
              {(['Width', 'Depth', 'Height'] as const).map((label, index) => (
                <label key={label}>
                  <span>{label} (m)</span>
                  <input
                    type="number"
                    min={0.000001}
                    step={0.001}
                    value={staged.shape.type === 'box'
                      ? staged.shape.box_dimensions_m[index]
                      : 0}
                    disabled={busy}
                    onChange={(event) => setBoxDimension(index, event.currentTarget.valueAsNumber)}
                  />
                </label>
              ))}
            </div>
          )}
          {staged.shape.type === 'sphere' && (
            <label className="scene-shape-select">
              <span>Radius (m)</span>
              <input
                type="number"
                min={0.000001}
                step={0.001}
                value={staged.shape.sphere_radius_m}
                disabled={busy}
                onChange={(event) => {
                  const value = event.currentTarget.valueAsNumber;
                  if (Number.isFinite(value) && staged.shape.type === 'sphere') {
                    setStaged({ ...staged, shape: { ...staged.shape, sphere_radius_m: value } });
                  }
                }}
              />
            </label>
          )}
          {staged.shape.type === 'cylinder' && (
            <div className="scene-pose-grid">
              <label>
                <span>Radius (m)</span>
                <input
                  type="number"
                  min={0.000001}
                  step={0.001}
                  value={staged.shape.cylinder_radius_m}
                  disabled={busy}
                  onChange={(event) => {
                    const value = event.currentTarget.valueAsNumber;
                    if (Number.isFinite(value) && staged.shape.type === 'cylinder') {
                      setStaged({ ...staged, shape: { ...staged.shape, cylinder_radius_m: value } });
                    }
                  }}
                />
              </label>
              <label>
                <span>Height (m)</span>
                <input
                  type="number"
                  min={0.000001}
                  step={0.001}
                  value={staged.shape.cylinder_height_m}
                  disabled={busy}
                  onChange={(event) => {
                    const value = event.currentTarget.valueAsNumber;
                    if (Number.isFinite(value) && staged.shape.type === 'cylinder') {
                      setStaged({ ...staged, shape: { ...staged.shape, cylinder_height_m: value } });
                    }
                  }}
                />
              </label>
            </div>
          )}
          {staged.shape.type === 'mesh' && (
            <>
              <label className="scene-shape-select">
                <span>Mesh resource URI</span>
                <input
                  type="text"
                  value={staged.shape.mesh_resource_uri}
                  disabled={busy}
                  required
                  placeholder="examples/cell/meshes/object.glb"
                  onChange={(event) => {
                    if (staged.shape.type === 'mesh') {
                      setStaged({
                        ...staged,
                        shape: { ...staged.shape, mesh_resource_uri: event.target.value },
                      });
                    }
                  }}
                />
              </label>
              <button
                type="button"
                className="quiet-button"
                aria-expanded={meshBrowserOpen}
                onPointerDown={(event) => event.preventDefault()}
                onClick={() => setMeshBrowserOpen((current) => !current)}
              >
                {meshBrowserOpen ? 'Close mesh browser' : 'Browse approved meshes'}
              </button>
              {meshBrowserOpen && (
                <MeshResourceBrowser
                  onSelect={(resourceUri) => {
                    if (staged.shape.type !== 'mesh') return;
                    setStaged({
                      ...staged,
                      shape: { ...staged.shape, mesh_resource_uri: resourceUri },
                    });
                    setMeshBrowserOpen(false);
                  }}
                />
              )}
              <div className="scene-pose-grid">
                {(['Scale X', 'Scale Y', 'Scale Z'] as const).map((label, index) => (
                  <label key={label}>
                    <span>{label}</span>
                    <input
                      type="number"
                      min={0.000001}
                      step={0.01}
                      value={staged.shape.type === 'mesh'
                        ? staged.shape.mesh_scale?.[index] ?? 1
                        : 1}
                      disabled={busy}
                      onChange={(event) => setMeshScale(
                        index, event.currentTarget.valueAsNumber,
                      )}
                    />
                  </label>
                ))}
              </div>
            </>
          )}
          <label className="scene-shape-select">
            <span>Pose frame</span>
            <select
              value={selectedFrameId}
              disabled={busy}
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
          <div className="scene-pose-grid">
            {(['X', 'Y', 'Z'] as const).map((axis, index) => (
              <label key={axis}>
                <span>{axis} (m)</span>
                <input
                  type="number"
                  step={0.001}
                  value={staged.pose.position_m[index]}
                  disabled={busy}
                  onChange={(event) => setPosePosition(index, event.currentTarget.valueAsNumber)}
                />
              </label>
            ))}
          </div>
          {'xyzw' in staged.pose.orientation ? (
            <div className="scene-pose-grid scene-quaternion-grid">
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
                    onChange={(event) => setQuaternion(index, event.currentTarget.valueAsNumber)}
                  />
                </label>
              ))}
            </div>
          ) : (
            <div className="scene-pose-grid">
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
                    onChange={(event) => setRpyDegrees(index, event.currentTarget.valueAsNumber)}
                  />
                </label>
              ))}
            </div>
          )}
          <p className="scene-contract-note">
            Frame: <code>{staged.pose.frame_ref.kind}</code> · shape:{' '}
            <code>{staged.shape.type}</code>
          </p>
          {stagedError !== null && (
            <p className="scene-commit-error" role="alert">{stagedError}</p>
          )}
          <details className="scene-object-metadata">
            <summary>Roles and collision metadata</summary>
            <dl>
              <dt>Resource roles</dt>
              <dd>{staged.resource_roles?.join(', ') || 'none'}</dd>
              <dt>Collision policies</dt>
              <dd>{staged.collision_policy_ids?.join(', ') || 'none'}</dd>
              <dt>Support contacts</dt>
              <dd>{Array.isArray(staged.metadata?.support_contacts)
                ? staged.metadata.support_contacts.length
                : 0}</dd>
            </dl>
            {staged.metadata !== undefined && (
              <pre>{JSON.stringify(staged.metadata, null, 2)}</pre>
            )}
          </details>
          <div className="scene-authoring-actions">
            <button
              type="button"
              disabled={busy}
              onPointerDown={(event) => event.preventDefault()}
              onClick={duplicateSelected}
            >
              Duplicate
            </button>
            <button
              className="danger-button"
              type="button"
              disabled={busy}
              onPointerDown={(event) => event.preventDefault()}
              onClick={() => mutate('scene_draft_remove_objects', {
                scene_resource_ids: [staged.scene_resource_id],
              })}
            >
              Remove from Draft
            </button>
          </div>
        </>
      )}
      <p className="scene-contract-note">
        Save persists this Draft. Runtime Apply, Revert-to-Saved, and
        Discard-to-Applied remain unavailable at their named scene-workflow gate.
      </p>
    </div>
  );
}

function ViewerMetadataEditor({
  program,
  draftRevision,
  busy,
  mutate,
}: {
  program: ProgramAuthoringDocument;
  draftRevision: number;
  busy: boolean;
  mutate: ProgramWorkspaceController['mutate'];
}) {
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
      className="scene-authoring-column"
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
      <div className="scene-authoring-heading">
        <div>
          <p className="eyebrow">Same Program Draft</p>
          <h3>Viewer metadata</h3>
        </div>
      </div>
      <div className="viewer-role-controls">
        {frameRoles.map((role) => (
          <label key={`frame:${role}`}>
            <input
              type="checkbox"
              checked={draft.frameRoleVisibility[role] ?? true}
              disabled={busy}
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
      <div className="scene-pose-grid">
        <label>
          <span>Frame axes (m)</span>
          <input
            type="number"
            min={0.02}
            max={1}
            step={0.01}
            value={draft.frameGizmoSizeM}
            disabled={busy}
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
            onChange={(event) => dispatch({
              type: 'set_object_axes_gizmo_size',
              sizeM: event.currentTarget.valueAsNumber,
            })}
          />
        </label>
      </div>
      {dirty && <p className="scene-contract-note">Valid edits commit on blur, Tab, or Enter.</p>}
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
    <div className="source-occurrence-panel">
      <div>
        <p className="eyebrow">Verified source-local model facts</p>
        <h3>Model occurrence</h3>
      </div>
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
      {occurrence !== null && (
        <>
          <div className="source-occurrence-facts">
            <span>component <code>{occurrence.component_id ?? 'passive'}</code></span>
            <span>source model <code>{occurrence.source_model_name}</code></span>
            <span>source robot <code>{occurrence.source_robot_name}</code></span>
            <span>{occurrence.coordinates.length} source coordinates</span>
            <span>{occurrence.srdf_groups.length} SRDF groups</span>
          </div>
          <details className="source-name-details">
            <summary>Source-local URDF/SRDF names</summary>
            <dl>
              <dt>Links</dt>
              <dd><code>{boundedNames(occurrence.source_link_names)}</code></dd>
              <dt>Joints</dt>
              <dd><code>{boundedNames(
                occurrence.coordinates.map((coordinate) => coordinate.source_joint_name),
              )}</code></dd>
              <dt>SRDF groups</dt>
              <dd><code>{boundedNames(
                occurrence.srdf_groups.map((group) => group.source_group_name),
              )}</code></dd>
              <dt>End effectors</dt>
              <dd><code>{boundedNames(
                occurrence.srdf_end_effectors.map(
                  (endEffector) => endEffector.source_end_effector_name,
                ),
              )}</code></dd>
            </dl>
          </details>
        </>
      )}
      <div className="runtime-source-list">
        <div className="scene-authoring-heading">
          <div>
            <p className="eyebrow">Owner-issued observation facts</p>
            <h3>Runtime pose sources</h3>
          </div>
          <span className="scene-hierarchy-count">{runtimeSources.length}</span>
        </div>
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
              <span className="scene-commit-error">{source.payload.source_error.detail}</span>
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
        {runtime !== null && (
          <details className="source-name-details">
            <summary>Runtime source diagnostics</summary>
            <dl>
              <dt>State</dt>
              <dd><code>{runtime.state.state}</code></dd>
              <dt>Reason</dt>
              <dd>{runtime.state.reason || 'none'}</dd>
              <dt>Deferred</dt>
              <dd>{runtime.state.deferred ? 'yes' : 'no'}</dd>
              <dt>Error</dt>
              <dd>{runtime.state.error ?? 'none'}</dd>
              {Object.entries(runtime.state.counters).slice(0, 64).map(([name, value]) => (
                <Fragment key={name}>
                  <dt>{name}</dt>
                  <dd>{value}</dd>
                </Fragment>
              ))}
            </dl>
          </details>
        )}
      </div>
      <p className="scene-contract-note">
        Names remain the original URDF/SRDF names for this occurrence. The authored-neutral mesh below is not claimed as an active multi-instance pose.
      </p>
    </div>
  );
}

const SceneViewerBridge = memo(function SceneViewerBridge({
  active,
  source,
  sourceValue,
  environmentValue,
  manifest,
  onRetry,
  program,
  draftIdentity,
  viewer,
  selection,
  livePose,
  selectionLocked,
  onSelect,
  ghost,
  ghostStale,
  candidatePoseEditable,
  onEditCandidatePose,
  onCommitTransform,
  onDuplicate,
  onRemove,
  onDetach,
  onDuplicateProgramResource,
  onRemoveProgramResource,
  onCommitProgramTransform,
  onEditSelection,
  mutationAvailability,
  hiddenSelectionKeys,
  objectAxesSelectionKeys,
  motionPreview,
  programRuntime,
  robotControl,
  runtimeEnabled,
  objectControlSurface,
  inspectorTarget,
  inspectorMinimized,
  onInspectorMinimizedChange,
  onInspectorMaximize,
  onInspectorPanelChange,
}: {
  active: boolean;
  source: HeadlessStateSource;
  sourceValue: SourceModelProjection | null;
  environmentValue: NodeEnvironmentResponse | null;
  manifest: StructuralResourceState<SceneUrdfManifest>;
  onRetry: () => void;
  program: ProgramDocument | ProgramAuthoringDocument | null;
  draftIdentity: { draftId: string; draftRevision: number } | null;
  viewer: ProgramViewerMetadata | undefined;
  selection: SceneSelection;
  livePose: LogicalScenePose | null;
  selectionLocked: boolean;
  onSelect: (selection: SceneSelection) => void;
  ghost: Pose3Value | null;
  ghostStale: boolean;
  candidatePoseEditable: boolean;
  onEditCandidatePose: (pose: Pose3Value) => void;
  onCommitTransform: (
    sceneResourceId: string,
    pose: LogicalScenePose,
    scale: [number, number, number],
    expectedOwnerIdentity: { ownerEpoch: string; generation: number },
  ) => Promise<boolean>;
  onDuplicate: (sceneResourceId: string) => Promise<boolean>;
  onRemove: (sceneResourceId: string) => Promise<boolean>;
  onDetach: (sceneResourceId: string) => Promise<boolean>;
  onDuplicateProgramResource: (sceneResourceId: string) => Promise<boolean>;
  onRemoveProgramResource: (sceneResourceId: string) => Promise<boolean>;
  onCommitProgramTransform: (
    sceneResourceId: string,
    pose: LogicalScenePose,
    scale: [number, number, number],
    expectedDraftIdentity: { ownerEpoch: string; generation: number },
  ) => Promise<boolean>;
  onEditSelection: (kind: 'owner_object' | 'program_resource', id: string) => void;
  mutationAvailability: Record<string, {
    duplicate: boolean;
    remove: boolean;
    detach: boolean;
    transform: boolean;
    reason: string | null;
    detachReason: string | null;
  }>;
  hiddenSelectionKeys: ReadonlySet<string>;
  objectAxesSelectionKeys: ReadonlySet<string>;
  motionPreview: MotionPreviewReceipt | null;
  programRuntime: ProgramRuntimeController;
  robotControl: RobotControlController;
  runtimeEnabled: boolean;
  objectControlSurface?: ReactNode;
  inspectorTarget?: HTMLElement | null;
  inspectorMinimized?: boolean;
  onInspectorMinimizedChange?: (minimized: boolean) => void;
  onInspectorMaximize?: () => void;
  onInspectorPanelChange?: (panel: ViewerInspectorPanel) => void;
}) {
  const headlessState = useViewerHeadlessState(source, active);
  const factsResult = useMemo(
    () => sceneOwnerFacts(
      headlessState.snapshot?.runtime ?? null,
      sourceValue,
      environmentValue,
    ),
    [environmentValue, headlessState.snapshot?.runtime, sourceValue],
  );
  const facts = factsResult.available ? factsResult.facts : null;
  const objects = facts?.logicalScene.snapshot?.objects ?? [];
  const draftPreviewObjects = objects.length === 0
    ? (program?.scene ?? []).map(programScenePreviewObject).filter(
        (object): object is LogicalSceneObject => object !== null,
      )
    : [];
  const programDraftPreview = objects.length === 0 && draftPreviewObjects.length > 0;
  const candidateRobot = useMemo(() => (
    motionPreview === null || manifest.status !== 'ready'
      ? null
      : candidateRobotPresentation(motionPreview, manifest.data)
  ), [manifest, motionPreview]);
  const selectedSceneResourceId = selection?.kind === 'owner_object'
    ? (programDraftPreview ? null : selection.id)
    : selection?.kind === 'program_resource'
      ? (programDraftPreview ? selection.id : null)
      : null;
  const selectedStructureName = selection?.kind === 'structure'
    ? selection.linkName
    : null;
  const visibleObjects = (objects.length === 0 ? draftPreviewObjects : objects).filter((object) => {
    const sourceKind = programDraftPreview ? 'program_resource' : 'owner_object';
    if (hiddenSelectionKeys.has(`${sourceKind}:${object.scene_resource_id}`)) return false;
    const resource = program?.scene?.find(
      (candidate) => candidate.scene_resource_id === object.scene_resource_id,
    );
    return !(resource?.resource_roles ?? []).some(
      (role) => viewer?.resource_role_visibility?.[role] === false,
    );
  });
  const viewportObjects = visibleObjects.map((object) => (
    object.scene_resource_id === selectedSceneResourceId
      && selection?.kind === 'owner_object'
      && object.placement.kind === 'cell_fixed'
      && livePose !== null
      ? { ...object, placement: { ...object.placement, pose_in_cell: livePose } }
      : object
  ));
  const runtimePresentation = runtimeScenePresentation(
    facts?.sceneRuntime ?? null,
    facts?.logicalScene ?? null,
    sourceValue,
    manifest.status === 'ready' ? manifest.data : null,
  );
  const frameAxes = useMemo(() => {
    const seen = new Set<string>();
    return (sourceValue?.targets ?? []).flatMap((target) => target.frames)
      .filter((frame) => (
        frame.source_link_name !== null
        && viewer?.frame_role_visibility?.[frame.frame_role_id] !== false
      ))
      .flatMap((frame) => {
        const identity = `${frame.source_link_name}\u0000${frame.frame_role_id}`;
        if (seen.has(identity) || frame.source_link_name === null) return [];
        seen.add(identity);
        return [{
          linkName: frame.source_link_name,
          frameRoleId: frame.frame_role_id,
        }];
      });
  }, [sourceValue, viewer?.frame_role_visibility]);
  useLayoutEffect(() => {
    source.markViewerPresented(headlessState.snapshot);
  }, [headlessState.snapshot, source]);
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
      manifest={manifest}
      onRetry={onRetry}
      sceneObjects={viewportObjects}
      programDraftPreview={programDraftPreview}
      programDraftIdentity={programDraftPreview
        ? draftIdentity ?? undefined
        : undefined}
      selectedSceneResourceId={selectedSceneResourceId}
      onSelectSceneResource={(identity) => {
        if (selectionLocked) return;
        onSelect(identity === null
          ? null
          : {
              kind: programDraftPreview ? 'program_resource' : 'owner_object',
              id: identity,
            });
      }}
      selectedStructureName={selectedStructureName}
      onSelectStructure={(name) => {
        if (selectionLocked) return;
        onSelect(name === null
          ? null
          : { kind: 'structure', id: `link:${name}`, linkName: name });
      }}
      onCommitSceneTransform={onCommitTransform}
      onCommitProgramDraftTransform={onCommitProgramTransform}
      onDuplicateSceneResource={programDraftPreview
        ? onDuplicateProgramResource
        : onDuplicate}
      onRemoveSceneResource={programDraftPreview
        ? onRemoveProgramResource
        : onRemove}
      onDetachSceneResource={onDetach}
      onEditSceneResource={(identity) => onEditSelection(
        programDraftPreview ? 'program_resource' : 'owner_object',
        identity,
      )}
      selectionLocked={selectionLocked}
      sceneMutationAvailability={programDraftPreview
        ? Object.fromEntries(viewportObjects.map((object) => [object.scene_resource_id, {
            duplicate: true,
            remove: true,
            detach: false,
            transform: true,
            reason: null,
            detachReason: 'Program Draft resources are not carried runtime objects.',
          }]))
        : mutationAvailability}
      candidateTcpPose={ghost}
      candidateStale={ghostStale}
      candidateTcpEditable={candidatePoseEditable}
      onEditCandidateTcpPose={onEditCandidatePose}
      candidateRobot={candidateRobot}
      runtimePresentation={runtimePresentation}
      sceneOwnerIdentity={facts === null ? undefined : {
        ownerEpoch: facts.sourceOwnerEpoch,
        generation: facts.sourceGeneration,
      }}
      viewerFrameIdentity={headlessState.snapshot?.runtime.complete === true ? {
        ownerEpoch: headlessState.snapshot.runtime.projectionOwnerEpoch,
        generation: headlessState.snapshot.runtime.projectionGeneration,
      } : undefined}
      readViewerFrameIdentity={source.getPresentedRuntimeFrame}
      pointCloudCellId={facts?.logicalScene.snapshot?.cell_id}
      viewer={viewer}
      frameAxes={frameAxes}
      objectAxesVisibleIds={viewportObjects.flatMap((object) => {
        const kind = programDraftPreview ? 'program_resource' : 'owner_object';
        return objectAxesSelectionKeys.has(`${kind}:${object.scene_resource_id}`)
          ? [object.scene_resource_id]
          : [];
      })}
      robotControlSurface={(
        <RobotControlPanelView enabled={runtimeEnabled} controller={robotControl} />
      )}
      robotControlBlocked={robotControl.state.phase === 'blocked'}
      objectControlSurface={objectControlSurface}
      inspectorTarget={inspectorTarget}
      inspectorMinimized={inspectorMinimized}
      onInspectorMinimizedChange={onInspectorMinimizedChange}
      onInspectorMaximize={onInspectorMaximize}
      onInspectorPanelChange={onInspectorPanelChange}
      transportControls={(
        <ProgramTransportControls
          enabled={runtimeEnabled}
          program={programRuntime}
          robot={robotControl}
        />
      )}
    />
    </Suspense>
  );
});

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
  runtimeEnabled,
  sourceModel,
  viewerActive,
  viewerInspectorTarget = null,
  viewerInspectorMinimized = false,
  onViewerInspectorMinimizedChange,
  onViewerInspectorMaximize,
  onViewerInspectorPanelChange,
}: SceneAuthoringWorkspaceProps) {
  const headlessState = useShellHeadlessState(headlessStateSource);
  const programWorkspace = programController.state.workspace;
  const program = programWorkspace?.draft.authoring ?? null;
  const draftRevision = programWorkspace?.draft.draftRevision ?? 0;
  const admissionGeneration = headlessState.snapshot?.authoringAdmission.admissionGeneration ?? null;
  const draftId = programWorkspace?.draft.draftId ?? null;
  const sceneMutationFence = useMemo(() => (
    admissionGeneration === null || draftId === null
      ? null
      : {
          admissionGeneration,
          draftId,
          draftRevision,
        }
  ), [admissionGeneration, draftId, draftRevision]);
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
    ).status,
    [facts, manifest, sourceValue],
  );
  const [selectedOccurrenceId, setSelectedOccurrenceId] = useState<string | null>(null);
  const [activeSceneTool, setActiveSceneTool] = useState<SceneWorkspaceTool>('hierarchy');
  const [selection, setSelection] = useState<SceneSelection>(null);
  const [livePose, setLivePose] = useState<LogicalScenePose | null>(null);
  const [selectionLocked, setSelectionLocked] = useState(false);
  const [hiddenSelectionKeys, setHiddenSelectionKeys] = useState<Set<string>>(
    () => new Set(),
  );
  const [objectAxesSelectionKeys, setObjectAxesSelectionKeys] = useState<Set<string>>(
    () => new Set(),
  );
  const [commitState, setCommitState] = useState<
    { kind: 'idle' | 'busy' | 'success' | 'error'; message: string | null }
  >({ kind: 'idle', message: null });

  useEffect(() => {
    const occurrences = sourceValue?.occurrences ?? [];
    if (!occurrences.some((item) => item.model_instance_id === selectedOccurrenceId)) {
      setSelectedOccurrenceId(occurrences[0]?.model_instance_id ?? null);
    }
  }, [selectedOccurrenceId, sourceValue]);
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
  const selectedProgramResourceId = selection?.kind === 'program_resource'
    ? selection.id
    : selectedOwnerObjectId;
  const selectedObject = objects.find(
    (object) => object.scene_resource_id === selectedOwnerObjectId,
  ) ?? null;
  const selectedObjectPoseKey = selectedObject?.placement.kind === 'cell_fixed'
    ? JSON.stringify(selectedObject.placement.pose_in_cell)
    : null;
  useEffect(() => {
    setLivePose(selectedObjectPoseKey === null
      ? null
      : JSON.parse(selectedObjectPoseKey) as LogicalScenePose);
    setCommitState({ kind: 'idle', message: null });
  }, [
    selectedOwnerObjectId,
    selectedObjectPoseKey,
  ]);

  const viewer = program?.viewer;
  const directControl = robotControl.state.status?.directControl ?? null;
  const tcpRole = directControl?.tcpFrameRoleId ?? null;
  const ghostVisible = tcpRole === null
    || viewer?.frame_role_visibility?.[tcpRole] !== false;
  const previewGhost = robotControl.state.ghost;
  const cellGhost = previewGhost === null
    ? null
    : candidateTcpPoseInCell(
        sourceValue,
        directControl?.controllableComponentId ?? null,
        directControl?.commandTargetId ?? null,
        directControl?.baseFrameRoleId ?? null,
        previewGhost.finalTcpPose,
      );
  const ghost = ghostVisible && cellGhost?.available
    ? cellGhost.pose
    : null;
  const candidatePoseEditable = ghost !== null
    && (robotControl.state.mode === 'pose_goal'
      || robotControl.state.mode === 'cartesian_path')
    && !robotControlHasActiveOperation(robotControl.state);
  const busyDraft = programController.state.status === 'loading'
    || (programController.state.status === 'ready'
      && programController.state.pendingAction !== null);
  const selectedProgramResource = program?.scene?.find(
    (resource) => resource.scene_resource_id === selectedProgramResourceId,
  ) ?? null;
  const copyToDraft = selectedObject !== null && selectedProgramResource !== null
    ? programResourceWithLivePose(selectedProgramResource, selectedObject)
    : null;
  const draftPoseForLive = selectedProgramResource === null
    ? null
    : livePoseFromProgramResource(selectedProgramResource);
  const editReason = selectedObject === null
    ? 'Select an owner-issued scene object.'
    : objectEditReason(selectedObject);

  const editLivePose = (
    field: 'x' | 'y' | 'z' | 0 | 1 | 2 | 3,
    value: number,
  ) => {
    if (livePose === null || !Number.isFinite(value)) return;
    if (typeof field === 'string') {
      setLivePose({
        ...livePose,
        position_m: { ...livePose.position_m, [field]: value },
      });
      return;
    }
    const orientation = [...livePose.orientation_xyzw] as [number, number, number, number];
    orientation[field] = value;
    setLivePose({ ...livePose, orientation_xyzw: orientation });
  };

  const submitSceneTransaction = useCallback(async (
    transaction: SceneTransactionRequest | null,
    id: string,
  ): Promise<boolean> => {
    if (transaction === null) {
      setCommitState({ kind: 'error', message: 'The current owner does not permit this scene edit.' });
      return false;
    }
    setCommitState({ kind: 'busy', message: null });
    try {
      const response = await api.scene.commit(transaction);
      if (
        response.request_id !== id
        || response.request_owner_epoch !== transaction.request_context.request_owner_epoch
      ) throw new Error('The scene owner returned a mismatched transaction receipt.');
      setCommitState({
        kind: 'success',
        message: `Committed scene revision ${response.committed_projection_revision.scene_revision.revision_id}.`,
      });
      return true;
    } catch (error) {
      setCommitState({
        kind: 'error',
        message: error instanceof Error ? error.message : 'Scene commit failed.',
      });
      return false;
    }
  }, []);

  const mutationAvailability = useMemo(() => Object.fromEntries(objects.map((object) => {
    const attachment = facts?.logicalScene.snapshot?.carried_object_attachments.find(
      (candidate) => candidate.scene_resource_id === object.scene_resource_id,
    );
    const writable = facts?.writeAvailable === true && sceneMutationFence !== null;
    const runtimeWritable = facts?.runtimeWriteAvailable === true;
    const fixed = object.placement.kind === 'cell_fixed';
    const owned = object.provider_evidence === null;
    const singleShape = object.collision_shapes.length === 1;
    const reason = !writable
      ? facts?.writeUnavailableReason ?? 'Scene authoring is unavailable.'
      : !owned
        ? 'This object is provider-owned.'
        : attachment !== undefined
          ? 'Detach the carried object before removing or transforming it.'
          : !fixed
            ? 'This placement is attachment-owned.'
            : !singleShape
              ? 'This operation requires one canonical collision shape.'
              : null;
    return [object.scene_resource_id, {
      transform: reason === null,
      duplicate: reason === null,
      remove: writable && owned && attachment === undefined,
      detach: runtimeWritable && attachment !== undefined,
      reason,
      detachReason: runtimeWritable
        ? attachment === undefined ? 'This object is not carried.' : null
        : facts?.runtimeWriteUnavailableReason ?? 'Runtime scene mutation is unavailable.',
    }];
  })), [facts, objects, sceneMutationFence]);

  const commitTransform = useCallback(async (
    sceneResourceId: string,
    pose: LogicalScenePose,
    scale: [number, number, number],
    expectedOwnerIdentity: { ownerEpoch: string; generation: number },
  ) => {
    const object = objects.find(
      (candidate) => candidate.scene_resource_id === sceneResourceId,
    );
    if (
      facts === null
      || sceneMutationFence === null
      || object === undefined
      || facts.sourceOwnerEpoch !== expectedOwnerIdentity.ownerEpoch
      || facts.sourceGeneration !== expectedOwnerIdentity.generation
    ) {
      setCommitState({
        kind: 'error',
        message: 'The scene changed while the transform was being edited; the preview was reset.',
      });
      return false;
    }
    const id = requestId();
    const accepted = await submitSceneTransaction(sceneObjectTransformTransaction(
      facts, sceneMutationFence, object, pose, scale, id,
    ), id);
    if (accepted) setLivePose(pose);
    return accepted;
  }, [facts, objects, sceneMutationFence, submitSceneTransaction]);

  const duplicateObject = useCallback(async (sceneResourceId: string) => {
    const object = objects.find((candidate) => candidate.scene_resource_id === sceneResourceId);
    if (facts === null || sceneMutationFence === null || object === undefined) return false;
    const existing = new Set(objects.map((candidate) => candidate.scene_resource_id));
    let duplicateId = `${sceneResourceId}_copy`;
    let suffix = 2;
    while (existing.has(duplicateId)) duplicateId = `${sceneResourceId}_copy_${suffix++}`;
    const id = requestId();
    return submitSceneTransaction(sceneDuplicateTransaction(
      facts, sceneMutationFence, object, duplicateId, id,
    ), id);
  }, [facts, objects, sceneMutationFence, submitSceneTransaction]);

  const removeObject = useCallback(async (sceneResourceId: string) => {
    const object = objects.find((candidate) => candidate.scene_resource_id === sceneResourceId);
    if (facts === null || sceneMutationFence === null || object === undefined) return false;
    const id = requestId();
    const accepted = await submitSceneTransaction(sceneRemoveTransaction(
      facts, sceneMutationFence, object, id,
    ), id);
    if (accepted && selectedOwnerObjectId === sceneResourceId) setSelection(null);
    return accepted;
  }, [facts, objects, sceneMutationFence, selectedOwnerObjectId, submitSceneTransaction]);

  const detachObject = useCallback(async (sceneResourceId: string) => {
    const attachment = facts?.logicalScene.snapshot?.carried_object_attachments.find(
      (candidate) => candidate.scene_resource_id === sceneResourceId,
    );
    if (facts === null || attachment === undefined) return false;
    const id = requestId();
    return submitSceneTransaction(sceneDetachTransaction(
      facts, attachment, id,
    ), id);
  }, [facts, submitSceneTransaction]);

  const attachObject = useCallback(async (
    target: {
      componentId: string;
      targetId: string;
      frameRoleId: string;
      touchBodyRoles: string[];
    },
    attachmentRevisions: string[],
  ) => {
    if (facts === null || selectedObject === null) return false;
    const id = requestId();
    return submitSceneTransaction(sceneAttachTransaction(
      facts,
      selectedObject,
      target,
      attachmentRevisions,
      id,
    ), id);
  }, [facts, selectedObject, submitSceneTransaction]);

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

  const commitLivePose = async () => {
    if (
      facts === null
      || sceneMutationFence === null
      || selectedObject === null
      || livePose === null
    ) return;
    const id = requestId();
    const transaction = scenePoseTransaction(
      facts,
      sceneMutationFence,
      selectedObject,
      livePose,
      id,
    );
    if (transaction === null) {
      setCommitState({
        kind: 'error',
        message: facts.writeUnavailableReason ?? editReason ?? 'The local pose is invalid.',
      });
      return;
    }
    setCommitState({ kind: 'busy', message: null });
    try {
      const response = await api.scene.commit(transaction);
      if (response.request_id !== id) {
        throw new Error('The scene owner returned a receipt for a different request.');
      }
      if (
        response.request_owner_epoch
        !== transaction.request_context.request_owner_epoch
      ) {
        throw new Error('The scene owner returned a receipt for a different owner epoch.');
      }
      setCommitState({
        kind: 'success',
        message: `Committed scene revision ${response.committed_projection_revision.scene_revision.revision_id}.`,
      });
    } catch (error) {
      setCommitState({
        kind: 'error',
        message: error instanceof Error ? error.message : 'Scene commit failed.',
      });
    }
  };

  const ownerObjectControlSurface = objects.length === 0 ? undefined : (
    <div className="scene-authoring-column current-scene-editor viewer-object-owner-editor">
      {!factsResult.available && (
        <div className="panel-state error-state" role="alert">
          <strong>Current scene is unavailable</strong>
          <span>{factsResult.reason}</span>
        </div>
      )}
      {facts !== null && (
        <>
          <details className="scene-object-metadata">
            <summary>Owner and runtime source facts</summary>
            <dl>
              <dt>Scene source</dt>
              <dd><code>{facts.sourceOwnerEpoch}</code> / {facts.sourceGeneration}</dd>
              <dt>Runtime source</dt>
              <dd><code>{facts.runtimeSourceOwnerEpoch}</code> / {facts.runtimeSourceGeneration}</dd>
              <dt>Scene revision</dt>
              <dd><code>{facts.logicalScene.snapshot?.revision.revision_id}</code></dd>
              <dt>Observations</dt>
              <dd>{runtimeStatus.freshGood} fresh / {runtimeStatus.total} total</dd>
            </dl>
          </details>
          {runtimeStatus.warnings.map((warning) => (
            <p className="scene-contract-note warning-state" key={warning} role="status">
              Measured overlay: {warning}
            </p>
          ))}
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
          {selectedObject !== null && livePose !== null && (
            <>
              <div className="scene-pose-grid">
                {(['x', 'y', 'z'] as const).map((field) => (
                  <label key={field}>
                    <span>{field.toUpperCase()} (m)</span>
                    <input
                      type="number"
                      step={0.001}
                      value={livePose.position_m[field]}
                      disabled={editReason !== null || commitState.kind === 'busy'}
                      onChange={(event) => editLivePose(field, event.currentTarget.valueAsNumber)}
                    />
                  </label>
                ))}
              </div>
              <div className="scene-pose-grid scene-quaternion-grid">
                {['Qx', 'Qy', 'Qz', 'Qw'].map((label, index) => (
                  <label key={label}>
                    <span>{label}</span>
                    <input
                      type="number"
                      step={0.001}
                      value={livePose.orientation_xyzw[index]}
                      disabled={editReason !== null || commitState.kind === 'busy'}
                      onChange={(event) => editLivePose(
                        index as 0 | 1 | 2 | 3,
                        event.currentTarget.valueAsNumber,
                      )}
                    />
                  </label>
                ))}
              </div>
            </>
          )}
          {(editReason ?? facts.writeUnavailableReason) !== null && (
            <p className="scene-contract-note warning-state">
              {editReason ?? facts.writeUnavailableReason}
            </p>
          )}
          {selectedObject !== null && (
            <details className="scene-object-metadata">
              <summary>Ownership, contacts, and collision metadata</summary>
              <dl>
                <dt>Placement</dt>
                <dd>{selectedObject.placement.kind.replaceAll('_', ' ')}</dd>
                <dt>Ownership</dt>
                <dd>{selectedObject.provider_evidence === null
                  ? 'Logical-scene owner'
                  : 'External provider'}</dd>
                <dt>Shapes</dt>
                <dd>{selectedObject.collision_shapes.map((shape) => shape.type).join(', ') || 'none'}</dd>
                <dt>Support contacts</dt>
                <dd>{selectedObject.support_contacts.length}</dd>
              </dl>
            </details>
          )}
          <details className="scene-object-metadata">
            <summary>Active collision policy</summary>
            <pre>{JSON.stringify(
              facts.logicalScene.snapshot?.configured_collision_policy ?? {},
              null,
              2,
            )}</pre>
          </details>
          {selectedObject?.placement.kind === 'cell_fixed' && (
            <AttachObjectControls
              object={selectedObject}
              sourceModel={sourceValue}
              disabled={!facts.runtimeWriteAvailable
                || selectedObject.provider_evidence !== null
                || commitState.kind === 'busy'}
              onAttach={attachObject}
            />
          )}
          <div className="scene-authoring-actions">
            <button
              type="button"
              disabled={selectedObject === null || livePose === null || editReason !== null
                || !facts.writeAvailable || commitState.kind === 'busy'}
              onClick={() => void commitLivePose()}
            >Commit current pose</button>
            <button
              type="button"
              disabled={selectedObject?.placement.kind !== 'cell_fixed' || commitState.kind === 'busy'}
              onClick={() => setLivePose(selectedObject?.placement.kind === 'cell_fixed'
                ? selectedObject.placement.pose_in_cell
                : null)}
            >Discard local edit</button>
            <button
              type="button"
              disabled={draftPoseForLive === null || editReason !== null || commitState.kind === 'busy'}
              onClick={() => {
                if (draftPoseForLive !== null) setLivePose(draftPoseForLive);
              }}
            >Stage Draft pose</button>
            <button
              type="button"
              disabled={copyToDraft === null || busyDraft}
              onClick={() => {
                if (copyToDraft !== null) void programController.mutate('scene_draft_update_object', {
                  scene_resource: copyToDraft as unknown as JsonValue,
                });
              }}
            >Copy live pose to Draft</button>
            <button
              type="button"
              disabled={selectedOwnerObjectId === null
                || mutationAvailability[selectedOwnerObjectId]?.duplicate !== true
                || commitState.kind === 'busy'}
              onClick={() => {
                if (selectedOwnerObjectId !== null) void duplicateObject(selectedOwnerObjectId);
              }}
            >Duplicate</button>
            <button
              type="button"
              disabled={selectedOwnerObjectId === null
                || mutationAvailability[selectedOwnerObjectId]?.detach !== true
                || commitState.kind === 'busy'}
              onClick={() => {
                if (selectedOwnerObjectId !== null) void detachObject(selectedOwnerObjectId);
              }}
            >Detach at current pose</button>
            <button
              type="button"
              className="danger-button"
              disabled={selectedOwnerObjectId === null
                || mutationAvailability[selectedOwnerObjectId]?.remove !== true
                || commitState.kind === 'busy'}
              onClick={() => {
                if (selectedOwnerObjectId !== null) void removeObject(selectedOwnerObjectId);
              }}
            >Remove</button>
          </div>
          {commitState.message !== null && (
            <p
              className={commitState.kind === 'error' ? 'scene-commit-error' : 'scene-commit-success'}
              role={commitState.kind === 'error' ? 'alert' : 'status'}
            >{commitState.message}</p>
          )}
        </>
      )}
    </div>
  );

  const viewerSurface = (
    <SceneViewerBridge
      active={viewerActive}
      source={headlessStateSource}
      sourceValue={sourceValue}
      environmentValue={environmentValue}
      manifest={manifest}
      onRetry={onRetryManifest}
      program={program}
      draftIdentity={draftId === null ? null : { draftId, draftRevision }}
      viewer={viewer}
      selection={selection}
      livePose={livePose}
      selectionLocked={selectionLocked}
      onSelect={setSelection}
      ghost={ghost}
      ghostStale={robotControl.state.ghostStale}
      candidatePoseEditable={candidatePoseEditable}
      onEditCandidatePose={robotControl.editPose}
      onCommitTransform={commitTransform}
      onDuplicate={duplicateObject}
      onRemove={removeObject}
      onDetach={detachObject}
      onDuplicateProgramResource={duplicateProgramResource}
      onRemoveProgramResource={removeProgramResource}
      onCommitProgramTransform={commitProgramTransform}
      onEditSelection={(kind, identity) => {
        setSelection({ kind, id: identity });
        setActiveSceneTool(kind === 'program_resource' ? 'program' : 'current');
      }}
      mutationAvailability={mutationAvailability}
      hiddenSelectionKeys={hiddenSelectionKeys}
      objectAxesSelectionKeys={objectAxesSelectionKeys}
      motionPreview={previewGhost}
      programRuntime={programRuntime}
      robotControl={robotControl}
      runtimeEnabled={runtimeEnabled}
      objectControlSurface={ownerObjectControlSurface}
      inspectorTarget={viewerInspectorTarget}
      inspectorMinimized={viewerInspectorMinimized}
      onInspectorMinimizedChange={onViewerInspectorMinimizedChange}
      onInspectorMaximize={onViewerInspectorMaximize}
      onInspectorPanelChange={onViewerInspectorPanelChange}
    />
  );

  const editorSurface = (
      <section className="panel scene-authoring-workspace" aria-labelledby="scene-authoring-title">
        <header className="panel-header scene-workspace-header">
          <strong id="scene-authoring-title">Scene</strong>
          <nav className="scene-workspace-tabs" aria-label="Scene tools">
            {([
              ['hierarchy', 'Hierarchy'],
              ['program', 'Program scene'],
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

        {(activeSceneTool === 'program' || activeSceneTool === 'viewer')
          && program === null && (
          <div className="panel-state">
            <strong>No Program Draft is loaded.</strong>
            <span>Open a Program before authoring its initial scene or viewer metadata.</span>
          </div>
        )}
        {activeSceneTool === 'program' && program !== null && (
            <ProgramSceneEditor
              program={program}
              draftRevision={draftRevision}
              busy={busyDraft}
              mutate={programController.mutate}
              selectedResourceId={selection?.kind === 'program_resource'
                ? selection.id
                : null}
              onSelectResource={(identity) => setSelection(identity === null
                ? null
                : { kind: 'program_resource', id: identity })}
            />
        )}
        {activeSceneTool === 'viewer' && program !== null && (
          <ViewerMetadataEditor
            program={program}
            draftRevision={draftRevision}
            busy={busyDraft}
            mutate={programController.mutate}
          />
        )}

        {activeSceneTool === 'viewer'
          && ghostVisible && cellGhost !== null && !cellGhost.available && (
          <p className="scene-contract-note warning-state" role="status">
            Candidate marker unavailable: {cellGhost.reason}
          </p>
        )}

        {activeSceneTool === 'current' && (
          <div data-testid="current-scene-editor">
            {ownerObjectControlSurface ?? (
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
