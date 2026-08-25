import { memo, useEffect, useLayoutEffect, useMemo, useState } from 'react';
import { createPortal } from 'react-dom';

import { api } from '../api/client';
import { useShellHeadlessState, useViewerHeadlessState } from '../hooks/useHeadlessState';
import type { HeadlessStateSource } from '../hooks/useHeadlessState';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { RobotControlController } from '../hooks/useRobotControl';
import {
  candidateTcpPoseInCell,
  livePoseFromProgramResource,
  programResourceWithLivePose,
  sceneOwnerFacts,
  scenePoseTransaction,
} from '../state/sceneAuthoring';
import { runtimeScenePresentation } from '../state/runtimeScenePresentation';
import type { StructuralResourceState } from '../state/structuralResource';
import { useStructuralResource } from '../state/structuralResource';
import {
  programViewerMetadata,
  viewerPresentationDraftFromProgram,
  viewerPresentationDraftReducer,
} from '../state/viewerPresentationDraft';
import type {
  JsonValue,
  LogicalSceneObject,
  LogicalScenePose,
  NodeEnvironmentResponse,
  ProgramDocument,
  ProgramSceneResource,
  ProgramViewerMetadata,
  SceneUrdfManifest,
  SourceModelProjection,
  SourceModelOccurrence,
} from '../types/api';
import type { Pose3Value } from '../types/robotControl';
import { StructuralViewer } from './StructuralViewer';

interface SceneAuthoringWorkspaceProps {
  editorTarget: HTMLElement | null;
  enabled: boolean;
  environment: StructuralResourceState<NodeEnvironmentResponse>;
  headlessStateSource: HeadlessStateSource;
  manifest: StructuralResourceState<SceneUrdfManifest>;
  onRetryManifest: () => void;
  programController: ProgramWorkspaceController;
  robotControl: RobotControlController;
  viewerActive: boolean;
}

const EMPTY_PROGRAM_RESOURCES: ProgramSceneResource[] = [];

function requestId(): string {
  return globalThis.crypto?.randomUUID?.()
    ?? `composer-scene-${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

function programRoleIds(
  program: ProgramDocument | null,
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
  if (object.collision_shapes.some((shape) => shape.type === 'mesh')) {
    return 'Live mesh-object transforms remain read-only in this bounded authoring slice.';
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

function ProgramSceneEditor({
  program,
  busy,
  mutate,
}: {
  program: ProgramDocument;
  busy: boolean;
  mutate: ProgramWorkspaceController['mutate'];
}) {
  const resources = program.scene ?? EMPTY_PROGRAM_RESOURCES;
  const [selectedId, setSelectedId] = useState(resources[0]?.scene_resource_id ?? null);
  const selected = resources.find((resource) => resource.scene_resource_id === selectedId) ?? null;
  const [staged, setStaged] = useState<ProgramSceneResource | null>(selected);

  useEffect(() => {
    const next = resources.find((resource) => resource.scene_resource_id === selectedId)
      ?? resources[0]
      ?? null;
    setSelectedId(next?.scene_resource_id ?? null);
    setStaged(next);
  }, [program.program_revision, resources, selectedId]);

  const dirty = staged !== null && selected !== null
    && JSON.stringify(staged) !== JSON.stringify(selected);
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
  const setShapeType = (type: 'box' | 'sphere' | 'cylinder') => {
    if (staged === null) return;
    const shape = type === 'box'
      ? { type, box_dimensions_m: [0.1, 0.1, 0.1] as [number, number, number] }
      : type === 'sphere'
        ? { type, sphere_radius_m: 0.05 }
        : { type, cylinder_radius_m: 0.05, cylinder_height_m: 0.1 };
    setStaged({ ...staged, shape });
  };
  const setBoxDimension = (index: number, value: number) => {
    if (staged?.shape.type !== 'box' || !Number.isFinite(value)) return;
    const dimensions = [...staged.shape.box_dimensions_m] as [number, number, number];
    dimensions[index] = value;
    setStaged({ ...staged, shape: { ...staged.shape, box_dimensions_m: dimensions } });
  };

  return (
    <div className="scene-authoring-column" data-testid="program-scene-editor">
      <div className="scene-authoring-heading">
        <div>
          <p className="eyebrow">Program Draft</p>
          <h3>Initial scene</h3>
        </div>
        <button
          type="button"
          disabled={busy}
          onClick={() => mutate('scene_draft_add_object', {
            scene_resource: defaultResource(
              new Set(resources.map((resource) => resource.scene_resource_id)),
            ) as unknown as JsonValue,
          })}
        >
          Add box
        </button>
      </div>
      <p className="muted">
        These resources belong to the editable Program. They are not the current runtime scene.
      </p>
      <select
        aria-label="Program scene resource"
        value={selectedId ?? ''}
        disabled={resources.length === 0}
        onChange={(event) => setSelectedId(event.target.value)}
      >
        {resources.length === 0 && <option value="">No Program resources</option>}
        {resources.map((resource) => (
          <option value={resource.scene_resource_id} key={resource.scene_resource_id}>
            {resource.scene_resource_id}
          </option>
        ))}
      </select>
      {staged !== null && (
        <>
          <label className="scene-shape-select">
            <span>Primitive shape</span>
            <select
              value={staged.shape.type}
              disabled={busy || staged.shape.type === 'mesh'}
              onChange={(event) => setShapeType(
                event.target.value as 'box' | 'sphere' | 'cylinder',
              )}
            >
              <option value="box">Box</option>
              <option value="sphere">Sphere</option>
              <option value="cylinder">Cylinder</option>
              {staged.shape.type === 'mesh' && <option value="mesh">Mesh (read-only)</option>}
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
          <div className="scene-pose-grid scene-quaternion-grid">
            {(['Qx', 'Qy', 'Qz', 'Qw'] as const).map((axis, index) => {
              const orientation = 'xyzw' in staged.pose.orientation
                ? staged.pose.orientation.xyzw
                : [0, 0, 0, 1];
              return (
                <label key={axis}>
                  <span>{axis}</span>
                  <input
                    type="number"
                    step={0.001}
                    value={orientation[index]}
                    disabled={busy}
                    onChange={(event) => setQuaternion(index, event.currentTarget.valueAsNumber)}
                  />
                </label>
              );
            })}
          </div>
          <p className="scene-contract-note">
            Frame: <code>{staged.pose.frame_ref.kind}</code> · shape:{' '}
            <code>{staged.shape.type}</code>
          </p>
          <div className="scene-authoring-actions">
            <button
              type="button"
              disabled={busy || !dirty}
              onClick={() => mutate('scene_draft_update_object', {
                scene_resource: staged as unknown as JsonValue,
              })}
            >
              Commit to Draft
            </button>
            <button
              type="button"
              disabled={busy || !dirty}
              onClick={() => setStaged(selected)}
            >
              Discard local edit
            </button>
            <button
              className="danger-button"
              type="button"
              disabled={busy}
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
  busy,
  mutate,
}: {
  program: ProgramDocument;
  busy: boolean;
  mutate: ProgramWorkspaceController['mutate'];
}) {
  const [draft, setDraft] = useState(() => viewerPresentationDraftFromProgram(program.viewer));
  useEffect(() => {
    setDraft(viewerPresentationDraftFromProgram(program.viewer));
  }, [program.program_revision, program.viewer]);
  const exact = programViewerMetadata(draft);
  const dirty = JSON.stringify(exact) !== JSON.stringify(
    programViewerMetadata(viewerPresentationDraftFromProgram(program.viewer)),
  );
  const dispatch = (action: Parameters<typeof viewerPresentationDraftReducer>[1]) => {
    setDraft((current) => viewerPresentationDraftReducer(current, action));
  };
  const frameRoles = programRoleIds(program, 'required_frame_roles');
  const resourceRoles = programRoleIds(program, 'required_resource_roles');
  return (
    <div className="scene-authoring-column" data-testid="viewer-metadata-editor">
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
              onChange={(event) => dispatch({
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
              onChange={(event) => dispatch({
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
      <div className="scene-authoring-actions">
        <button
          type="button"
          disabled={busy || !dirty}
          onClick={() => mutate('viewer_draft_replace', {
            viewer: exact as unknown as JsonValue,
          })}
        >
          Commit viewer metadata
        </button>
        <button
          type="button"
          disabled={busy || !dirty}
          onClick={() => setDraft(viewerPresentationDraftFromProgram(program.viewer))}
        >
          Discard local edit
        </button>
      </div>
    </div>
  );
}

function SourceOccurrencePanel({
  occurrence,
  occurrences,
  selectedId,
  onSelect,
}: {
  occurrence: SourceModelOccurrence | null;
  occurrences: SourceModelOccurrence[];
  selectedId: string | null;
  onSelect: (id: string) => void;
}) {
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
  viewer,
  selectedObjectId,
  livePose,
  selectionLocked,
  onSelect,
  ghost,
  ghostStale,
}: {
  active: boolean;
  source: HeadlessStateSource;
  sourceValue: SourceModelProjection | null;
  environmentValue: NodeEnvironmentResponse | null;
  manifest: StructuralResourceState<SceneUrdfManifest>;
  onRetry: () => void;
  program: ProgramDocument | null;
  viewer: ProgramViewerMetadata | undefined;
  selectedObjectId: string | null;
  livePose: LogicalScenePose | null;
  selectionLocked: boolean;
  onSelect: (identity: string | null) => void;
  ghost: Pose3Value | null;
  ghostStale: boolean;
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
  const visibleObjects = objects.filter((object) => {
    const resource = program?.scene?.find(
      (candidate) => candidate.scene_resource_id === object.scene_resource_id,
    );
    return !(resource?.resource_roles ?? []).some(
      (role) => viewer?.resource_role_visibility?.[role] === false,
    );
  });
  const viewportObjects = visibleObjects.map((object) => (
    object.scene_resource_id === selectedObjectId
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
  useLayoutEffect(() => {
    source.markViewerPresented(headlessState.snapshot);
  }, [headlessState.snapshot, source]);
  return (
    <StructuralViewer
      viewerActive={active}
      manifest={manifest}
      onRetry={onRetry}
      sceneObjects={viewportObjects}
      selectedSceneResourceId={selectedObjectId}
      onSelectSceneResource={(identity) => {
        if (!selectionLocked) onSelect(identity);
      }}
      candidateTcpPose={ghost}
      candidateStale={ghostStale}
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
    />
  );
});

export function SceneAuthoringWorkspace({
  editorTarget,
  enabled,
  environment,
  headlessStateSource,
  manifest,
  onRetryManifest,
  programController,
  robotControl,
  viewerActive,
}: SceneAuthoringWorkspaceProps) {
  const headlessState = useShellHeadlessState(headlessStateSource);
  const [sourceModel, retrySourceModel] = useStructuralResource(api.sourceModel.get, {
    enabled,
    disabledMessage: 'Source-model facts require a connected headless owner.',
    failureMessage: 'Could not read the verified active source-model projection.',
  });
  const programWorkspace = programController.state.workspace;
  const program = programWorkspace?.draft.program ?? null;
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
  const [selectedObjectId, setSelectedObjectId] = useState<string | null>(null);
  const [livePose, setLivePose] = useState<LogicalScenePose | null>(null);
  const [selectionLocked, setSelectionLocked] = useState(false);
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
    if (selectionLocked && selectedObjectId !== null) return;
    if (!objects.some((object) => object.scene_resource_id === selectedObjectId)) {
      setSelectedObjectId(objects[0]?.scene_resource_id ?? null);
    }
  }, [objects, selectedObjectId, selectionLocked]);
  const selectedObject = objects.find(
    (object) => object.scene_resource_id === selectedObjectId,
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
    selectedObjectId,
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
  const busyDraft = programController.state.status === 'loading'
    || (programController.state.status === 'ready'
      && programController.state.pendingAction !== null);
  const selectedProgramResource = program?.scene?.find(
    (resource) => resource.scene_resource_id === selectedObjectId,
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

  const viewerSurface = (
    <SceneViewerBridge
      active={viewerActive}
      source={headlessStateSource}
      sourceValue={sourceValue}
      environmentValue={environmentValue}
      manifest={manifest}
      onRetry={onRetryManifest}
      program={program}
      viewer={viewer}
      selectedObjectId={selectedObjectId}
      livePose={livePose}
      selectionLocked={selectionLocked}
      onSelect={setSelectedObjectId}
      ghost={ghost}
      ghostStale={robotControl.state.ghostStale}
    />
  );

  const commitLivePose = async () => {
    if (facts === null || selectedObject === null || livePose === null) return;
    const id = requestId();
    const transaction = scenePoseTransaction(facts, selectedObject, livePose, id);
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

  const editorSurface = (
      <section className="panel scene-authoring-workspace" aria-labelledby="scene-authoring-title">
        <header className="panel-header">
          <div>
            <p className="eyebrow">Draft authoring + owner projection</p>
            <h2 id="scene-authoring-title">Scene and viewer workspace</h2>
          </div>
          <button type="button" className="quiet-button" onClick={retrySourceModel}>
            Refresh source facts
          </button>
        </header>

        {sourceModel.status === 'error' && (
          <div className="panel-state error-state" role="alert">
            <strong>Source-model projection unavailable</strong>
            <span>{sourceModel.message}</span>
          </div>
        )}
        {sourceValue !== null && (
          <SourceOccurrencePanel
            occurrences={sourceValue.occurrences}
            selectedId={selectedOccurrenceId}
            occurrence={sourceValue.occurrences.find(
              (item) => item.model_instance_id === selectedOccurrenceId,
            ) ?? null}
            onSelect={setSelectedOccurrenceId}
          />
        )}

        {program === null && (
          <div className="panel-state">
            <strong>No Program Draft is loaded.</strong>
            <span>Open a Program below before authoring its initial scene or viewer metadata.</span>
          </div>
        )}
        {program !== null && (
          <div className="scene-authoring-grid">
            <ProgramSceneEditor program={program} busy={busyDraft} mutate={programController.mutate} />
            <ViewerMetadataEditor program={program} busy={busyDraft} mutate={programController.mutate} />
          </div>
        )}

        {ghostVisible && cellGhost !== null && !cellGhost.available && (
          <p className="scene-contract-note warning-state" role="status">
            Candidate marker unavailable: {cellGhost.reason}
          </p>
        )}

        <div className="scene-authoring-column current-scene-editor" data-testid="current-scene-editor">
          <div className="scene-authoring-heading">
            <div>
              <p className="eyebrow">Logical-scene owner</p>
              <h3>Current scene object</h3>
            </div>
            <label className="toggle-control">
              <input
                type="checkbox"
                checked={selectionLocked}
                onChange={(event) => setSelectionLocked(event.target.checked)}
              />
              <span>Lock selection</span>
            </label>
          </div>
          {!factsResult.available && (
            <div className="panel-state error-state" role="alert">
              <strong>Current scene is unavailable</strong>
              <span>{factsResult.reason}</span>
            </div>
          )}
          {facts !== null && (
            <>
              <div className="scene-owner-identity">
                <span>scene source <code>{facts.sourceOwnerEpoch}</code> / {facts.sourceGeneration}</span>
                <span>runtime source <code>{facts.runtimeSourceOwnerEpoch}</code> / {facts.runtimeSourceGeneration}</span>
                <span>revision <code>{facts.logicalScene.snapshot?.revision.revision_id}</code></span>
                <span>
                  observations <strong>{runtimeStatus.freshGood}</strong> fresh / {runtimeStatus.total}
                </span>
                <span>{runtimeStatus.staleGood} stale</span>
                <span>{runtimeStatus.unknown} unknown</span>
                <span>{runtimeStatus.buffered} buffered</span>
              </div>
              {runtimeStatus.warnings.map((warning) => (
                <p
                  className="scene-contract-note warning-state"
                  key={warning}
                  role="status"
                >
                  Measured overlay: {warning}
                </p>
              ))}
              <select
                aria-label="Current scene object"
                value={selectedObjectId ?? ''}
                disabled={objects.length === 0 || selectionLocked}
                onChange={(event) => setSelectedObjectId(event.target.value)}
              >
                {objects.length === 0 && <option value="">No current scene objects</option>}
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
              <div className="scene-authoring-actions">
                <button
                  type="button"
                  disabled={
                    selectedObject === null
                    || livePose === null
                    || editReason !== null
                    || !facts.writeAvailable
                    || commitState.kind === 'busy'
                  }
                  onClick={() => void commitLivePose()}
                >
                  Commit current pose
                </button>
                <button
                  type="button"
                  disabled={selectedObject?.placement.kind !== 'cell_fixed' || commitState.kind === 'busy'}
                  onClick={() => setLivePose(
                    selectedObject?.placement.kind === 'cell_fixed'
                      ? selectedObject.placement.pose_in_cell
                      : null,
                  )}
                >
                  Discard local edit
                </button>
                <button
                  type="button"
                  disabled={
                    draftPoseForLive === null
                    || editReason !== null
                    || commitState.kind === 'busy'
                  }
                  title={draftPoseForLive === null
                    ? 'A matching Program resource with an exact cell-frame pose is required.'
                    : undefined}
                  onClick={() => {
                    if (draftPoseForLive !== null) setLivePose(draftPoseForLive);
                  }}
                >
                  Stage Draft pose for live Commit
                </button>
                <button
                  type="button"
                  disabled={copyToDraft === null || busyDraft}
                  title={copyToDraft === null
                    ? 'A matching Program resource and lossless cell-frame pose are required.'
                    : undefined}
                  onClick={() => {
                    if (copyToDraft !== null) {
                      programController.mutate('scene_draft_update_object', {
                        scene_resource: copyToDraft as unknown as JsonValue,
                      });
                    }
                  }}
                >
                  Copy live pose to Draft
                </button>
              </div>
              {commitState.message !== null && (
                <p
                  className={commitState.kind === 'error' ? 'scene-commit-error' : 'scene-commit-success'}
                  role={commitState.kind === 'error' ? 'alert' : 'status'}
                >
                  {commitState.message}
                </p>
              )}
            </>
          )}
        </div>
      </section>
  );

  return (
    <>
      {viewerSurface}
      {editorTarget === null ? null : createPortal(editorSurface, editorTarget)}
    </>
  );
}
