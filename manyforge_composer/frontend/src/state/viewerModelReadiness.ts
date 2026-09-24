import type { SceneUrdfManifest, SourceModelProjection } from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { StructuralResourceState } from './structuralResource';

export type ViewerModelPresentation =
  | { status: 'ready'; manifest: SceneUrdfManifest }
  | { status: 'synchronizing'; message: string }
  | { status: 'unavailable'; message: string };

type ControlTransportStatus =
  | 'connecting'
  | 'connected'
  | 'delayed'
  | 'stale'
  | 'disconnected';

type ControlSemanticStatus = 'current' | 'synchronizing' | 'unavailable';

function recordValue(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

function pendingManagerApply(payload: Record<string, unknown>): boolean | null {
  const apply = recordValue(payload.apply);
  if (apply === null || !Array.isArray(apply.records)) return null;
  for (const value of apply.records) {
    const record = recordValue(value);
    if (record === null || typeof record.incomplete !== 'boolean') return null;
    if (record.incomplete) return true;
  }
  return false;
}

function resourceStateMessage<T>(
  resource: StructuralResourceState<T>,
): string | null {
  return resource.status === 'error' || resource.status === 'absent'
    ? resource.message
    : null;
}

/**
 * Admit robot geometry only when the current Manager publication, source
 * projection, and cached URDF all name the same ready composition.
 */
export function viewerModelPresentation(
  snapshot: ComposerHeadlessState | null,
  transportStatus: ControlTransportStatus,
  semanticStatus: ControlSemanticStatus,
  acceptedTransitionPending: boolean,
  sourceModel: StructuralResourceState<SourceModelProjection>,
  manifest: StructuralResourceState<SceneUrdfManifest>,
): ViewerModelPresentation {
  if (acceptedTransitionPending) {
    return {
      status: 'synchronizing',
      message: 'Scenario activation is pending. The previous robot model is hidden until Manager publication settles.',
    };
  }
  if (snapshot === null || transportStatus !== 'connected'
      || semanticStatus === 'synchronizing') {
    return {
      status: 'synchronizing',
      message: 'Synchronizing with Manager. The previous robot model is hidden until its active identity is verified.',
    };
  }
  if (semanticStatus === 'unavailable') {
    return {
      status: 'unavailable',
      message: 'The Manager control projection is unavailable; robot geometry is hidden.',
    };
  }

  const manager = snapshot.runtime.sections.manager;
  if (!manager.available || manager.freshness === 'unavailable' || manager.payload === null) {
    return {
      status: 'unavailable',
      message: 'The active Manager composition is unavailable; robot geometry is hidden.',
    };
  }
  if (!snapshot.runtime.complete || manager.freshness === 'stale') {
    return {
      status: 'synchronizing',
      message: 'The Manager projection is stale. The previous robot model is hidden while it synchronizes.',
    };
  }
  if (manager.knownGaps.length > 0 || manager.sourceGeneration === null
      || manager.sourceOwnerEpoch === null || manager.sourceOwnerEpoch.length === 0
      || manager.sourceOwnerId === null || manager.sourceOwnerId.length === 0) {
    return {
      status: 'unavailable',
      message: 'The Manager composition identity is incomplete; robot geometry is hidden.',
    };
  }

  const payload = manager.payload;
  const pending = pendingManagerApply(payload);
  if (pending === null) {
    return {
      status: 'unavailable',
      message: 'Manager Apply status is incomplete; robot geometry is hidden until it can be verified.',
    };
  }
  if (pending) {
    return {
      status: 'synchronizing',
      message: 'A Manager Apply is in progress. The previous robot model is hidden until publication settles.',
    };
  }

  const active = recordValue(payload.active);
  const runtime = recordValue(payload.runtime);
  const cellId = payload.cell_id;
  const retainedBundleId = active?.retained_bundle_id;
  const graphRevision = active?.graph_revision;
  if (active === null || typeof cellId !== 'string' || cellId.trim().length === 0
      || typeof retainedBundleId !== 'string' || retainedBundleId.trim().length === 0
      || typeof graphRevision !== 'string' || graphRevision.trim().length === 0) {
    return {
      status: 'unavailable',
      message: 'Manager has no complete active composition; robot geometry is hidden.',
    };
  }
  if (payload.manager_ready !== true || runtime?.converged !== true) {
    return {
      status: 'unavailable',
      message: 'The active Manager composition is not ready and converged; robot geometry is hidden.',
    };
  }

  if (sourceModel.status !== 'ready') {
    const message = resourceStateMessage(sourceModel);
    return message === null
      ? {
          status: 'synchronizing',
          message: 'Synchronizing the active source-model projection; the previous robot model is hidden.',
        }
      : { status: 'unavailable', message: `The active source-model projection is unavailable: ${message}` };
  }
  const source = sourceModel.data;
  if (source.cell_id !== cellId
      || source.retained_bundle_id !== retainedBundleId
      || source.graph_revision !== graphRevision) {
    return {
      status: 'synchronizing',
      message: 'The source-model identity does not match Manager. The previous robot model is hidden while it refreshes.',
    };
  }

  if (manifest.status !== 'ready') {
    const message = resourceStateMessage(manifest);
    return message === null
      ? {
          status: 'synchronizing',
          message: 'Synchronizing the active robot model; the previous model is hidden.',
        }
      : { status: 'unavailable', message: `The active robot model is unavailable: ${message}` };
  }
  if (manifest.data.cell_id !== cellId
      || manifest.data.retained_bundle_id !== retainedBundleId
      || manifest.data.graph_revision !== graphRevision
      || manifest.data.cell_id !== source.cell_id
      || manifest.data.retained_bundle_id !== source.retained_bundle_id
      || manifest.data.graph_revision !== source.graph_revision) {
    return {
      status: 'synchronizing',
      message: 'The robot model identity does not match the active Manager composition. The previous model is hidden while it refreshes.',
    };
  }
  return { status: 'ready', manifest: manifest.data };
}
