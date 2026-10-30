import type { SceneUrdfManifest, SourceModelProjection } from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { StructuralResourceState } from './structuralResource';

/**
 * Why the previous model is hidden while synchronizing. ``transition`` and
 * ``loading`` are expected (an accepted activation or an initial/refresh read);
 * ``connecting`` follows the separately reported control connection; the
 * remaining reasons mean the current identity cannot be verified and are
 * errors unless a known controlled transition explains them.
 */
export type ViewerModelSynchronizingReason =
  | 'transition'
  | 'connecting'
  | 'loading'
  | 'projection_stale'
  | 'identity_mismatch';

/** Why a retained robot model cannot currently be verified. */
export type ViewerModelRetainedReason = 'transport_delayed' | 'transport_stale';

export type ViewerModelPresentation =
  | { status: 'ready'; manifest: SceneUrdfManifest }
  | {
      /**
       * The model verified against this control session's last accepted
       * snapshot stays mounted, but it is not current: the control transport
       * is delayed or stale, so its Manager identity cannot be reconfirmed.
       */
      status: 'retained';
      manifest: SceneUrdfManifest;
      reason: ViewerModelRetainedReason;
      message: string;
    }
  | {
      status: 'synchronizing';
      reason: ViewerModelSynchronizingReason;
      message: string;
    }
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

export function pendingManagerApply(payload: Record<string, unknown>): boolean | null {
  const apply = recordValue(payload.apply);
  if (apply === null || !Array.isArray(apply.records)) return null;
  for (const value of apply.records) {
    const record = recordValue(value);
    if (record === null || typeof record.incomplete !== 'boolean') return null;
    if (record.incomplete) return true;
  }
  return false;
}

/** Current identity-verified Applied cell graph, independent of Scenario/Close origin. */
function verifiedAppliedCellGraph(
  snapshot: ComposerHeadlessState | null,
  transportStatus: ControlTransportStatus,
): Record<string, unknown> | null {
  if (snapshot === null || transportStatus !== 'connected' || !snapshot.runtime.complete) return null;
  const manager = snapshot.runtime.sections.manager;
  const payload = manager.payload;
  if (!manager.available || manager.freshness !== 'fresh' || manager.knownGaps.length !== 0
      || !Number.isSafeInteger(manager.sourceGeneration) || (manager.sourceGeneration ?? 0) <= 0
      || !manager.sourceOwnerId || !manager.sourceOwnerEpoch || payload === null
      || payload.manager_ready !== true || pendingManagerApply(payload) !== false) return null;
  const active = recordValue(payload.active);
  const graph = recordValue(active?.cell_graph);
  const publication = recordValue(active?.publication);
  const revision = recordValue(publication?.projection_revision);
  const runtime = recordValue(payload.runtime);
  const configuration = snapshot.runtime.activeConfiguration;
  const verified = typeof active?.canonical_empty === 'boolean' && runtime?.converged === true
    && typeof payload.cell_id === 'string' && payload.cell_id.length > 0
    && graph?.cell_id === payload.cell_id
    && typeof active.retained_bundle_id === 'string' && active.retained_bundle_id.length > 0
    && typeof active.graph_revision === 'string' && active.graph_revision.length > 0
    && graph?.graph_revision === active.graph_revision
    && configuration?.retainedBundleId === active.retained_bundle_id
    && configuration?.graphRevision === active.graph_revision
    && revision?.retained_bundle_id === active.retained_bundle_id
    && revision?.graph_revision === active.graph_revision
    && Number.isSafeInteger(publication?.publication_generation)
    && (publication?.publication_generation as number) > 0;
  return verified ? graph : null;
}

/** True only when the current, identity-verified Applied graph has no model instances. */
export function verifiedAppliedGraphHasNoModels(
  snapshot: ComposerHeadlessState | null,
  transportStatus: ControlTransportStatus,
): boolean {
  const graph = verifiedAppliedCellGraph(snapshot, transportStatus);
  const models = graph?.model_instances;
  return Array.isArray(models) && models.length === 0;
}

/** Current zero-physical graph evidence, independent of its Scenario/Close origin. */
export function verifiedNoPhysicalComponents(
  snapshot: ComposerHeadlessState | null,
  transportStatus: ControlTransportStatus,
): boolean {
  const graph = verifiedAppliedCellGraph(snapshot, transportStatus);
  return graph !== null
    && ['model_instances', 'controllable_components', 'control_bindings', 'control_domains']
      .every((key) => {
        const rows = graph[key];
        return Array.isArray(rows) && rows.length === 0;
      });
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
 *
 * A delayed or stale control transport keeps the same control session (an
 * actual close moves it to ``disconnected`` with a new snapshot barrier). In
 * that case the robot model that the session's last accepted snapshot
 * verifies is retained, mounted and marked not current (``retained``),
 * instead of being unmounted and rebuilt on recovery: commands are disabled
 * by the transport condition itself, and the not-verified notice keeps the
 * view from being presented as healthy (spec 450 §5). Geometry stays hidden
 * for a new or closed session (``connecting``/``disconnected``), semantic
 * synchronization, an accepted or Manager transition, a stale projection, an
 * identity mismatch and every unavailable case.
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
      reason: 'transition',
      message: 'Scenario activation is pending. The previous robot model is hidden until Manager publication settles.',
    };
  }
  const transportUnverified = transportStatus === 'delayed'
    || transportStatus === 'stale';
  if (snapshot === null
      || (transportStatus !== 'connected' && !transportUnverified)
      || semanticStatus === 'synchronizing') {
    return {
      status: 'synchronizing',
      reason: 'connecting',
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
      reason: 'projection_stale',
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
      reason: 'transition',
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
      reason: 'loading',
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
      reason: 'identity_mismatch',
      message: 'The source-model identity does not match Manager. The previous robot model is hidden while it refreshes.',
    };
  }

  if (manifest.status !== 'ready') {
    const message = resourceStateMessage(manifest);
    return message === null
      ? {
          status: 'synchronizing',
      reason: 'loading',
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
      reason: 'identity_mismatch',
      message: 'The robot model identity does not match the active Manager composition. The previous model is hidden while it refreshes.',
    };
  }
  if (transportUnverified) {
    const condition = transportStatus === 'delayed' ? 'delayed' : 'stale';
    return {
      status: 'retained',
      manifest: manifest.data,
      reason: transportStatus === 'delayed' ? 'transport_delayed' : 'transport_stale',
      message: `Control connection ${condition}: the last verified robot model is shown, but its current Manager identity cannot be confirmed. Do not rely on it for the current cell state.`,
    };
  }
  return { status: 'ready', manifest: manifest.data };
}
