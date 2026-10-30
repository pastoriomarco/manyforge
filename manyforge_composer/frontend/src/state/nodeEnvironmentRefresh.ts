import type { ControlSemanticStatus, ControlTransportStatus } from '../hooks/useHeadlessState';
import type { ComposerHeadlessState } from '../types/headless';
import { pendingManagerApply } from './viewerModelReadiness';

function objectValue(value: unknown): Record<string, unknown> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : null;
}

/**
 * Identity and health facts that can change executable-node availability.
 *
 * Program-cycle counters, observation ages, and presentation generations are
 * deliberately absent: Run/Stop/Reset must not re-read a large structural
 * environment whose binding and owner health did not change.
 */
export function nodeEnvironmentRefreshKey(
  snapshot: ComposerHeadlessState,
): string {
  const manager = snapshot.runtime.sections.manager;
  const active = objectValue(objectValue(manager.payload)?.active);
  const components = snapshot.componentOverview.components
    .map((component) => ({
      id: component.id,
      ownerEpoch: component.ownerEpoch,
      ownerId: component.ownerId,
      reasonCode: component.reasonCode,
      required: component.required,
      status: component.status,
    }))
    .sort((left, right) => left.id.localeCompare(right.id));
  return JSON.stringify({
    receiverEpoch: snapshot.receiverEpoch,
    manager: {
      available: manager.available,
      freshness: manager.freshness,
      graphRevision: typeof active?.graph_revision === 'string'
        ? active.graph_revision
        : null,
      knownGaps: manager.knownGaps,
      retainedBundleId: typeof active?.retained_bundle_id === 'string'
        ? active.retained_bundle_id
        : null,
      sourceOwnerEpoch: manager.sourceOwnerEpoch,
      sourceOwnerId: manager.sourceOwnerId,
    },
    readiness: snapshot.componentOverview.readiness,
    capabilities: [...snapshot.componentOverview.capabilities].sort(),
    components,
  });
}

/**
 * Identity and health facts that select the active structural viewer model.
 * This deliberately excludes component-level churn so the URDF and source
 * model are re-read only when their Manager-owned composition may have moved.
 */
export function viewerModelRefreshKey(
  snapshot: ComposerHeadlessState,
): string {
  const manager = snapshot.runtime.sections.manager;
  const payload = objectValue(manager.payload);
  const active = objectValue(payload?.active);
  const runtime = objectValue(payload?.runtime);
  return JSON.stringify({
    receiverEpoch: snapshot.receiverEpoch,
    manager: {
      available: manager.available,
      freshness: manager.freshness,
      knownGaps: manager.knownGaps,
      cellId: typeof payload?.cell_id === 'string' ? payload.cell_id : null,
      pendingApply: payload === null ? null : pendingManagerApply(payload),
      hasSourceGeneration: manager.sourceGeneration !== null,
      managerReady: typeof payload?.manager_ready === 'boolean'
        ? payload.manager_ready
        : null,
      graphRevision: typeof active?.graph_revision === 'string'
        ? active.graph_revision
        : null,
      retainedBundleId: typeof active?.retained_bundle_id === 'string'
        ? active.retained_bundle_id
        : null,
      runtimeConverged: typeof runtime?.converged === 'boolean'
        ? runtime.converged
        : null,
      sourceOwnerEpoch: manager.sourceOwnerEpoch,
      sourceOwnerId: manager.sourceOwnerId,
    },
  });
}

/** Re-read both viewer projections after an observed Manager identity change. */
export function refreshViewerModelsOnManagerChange(
  previousKey: string | null,
  nextKey: string,
  refreshSourceModel: () => void,
  refreshManifest: () => void,
): string {
  if (previousKey !== null && previousKey !== nextKey) {
    refreshSourceModel();
    refreshManifest();
  }
  return nextKey;
}

/** The Manager has a pending Apply or has not yet activated/converged. */
export const MANAGER_ACTIVATION_PENDING = 'manager_activation_pending';

/**
 * The settled active identity of the Manager's applied selection.
 *
 * Returns the active identity once the Manager is ready, converged, has no
 * incomplete Apply and no advertised maintenance operation;
 * `MANAGER_ACTIVATION_PENDING` while an activation is still in progress; and
 * `null` when the Manager section carries no usable evidence (unavailable,
 * stale, gapped or malformed), which is not a transition in either direction.
 * Heartbeats, Apply history and observation counters are deliberately absent.
 */
export function managerActivationSettlementKey(
  snapshot: ComposerHeadlessState,
): string | null {
  const manager = snapshot.runtime.sections.manager;
  const payload = objectValue(manager.payload);
  if (!manager.available || manager.freshness !== 'fresh'
      || manager.knownGaps.length > 0 || payload === null) {
    return null;
  }
  const pending = pendingManagerApply(payload);
  if (pending === null) return null;
  const active = objectValue(payload.active);
  const runtime = objectValue(payload.runtime);
  const retainedBundleId = typeof active?.retained_bundle_id === 'string'
    && active.retained_bundle_id.length > 0
    ? active.retained_bundle_id
    : null;
  const graphRevision = typeof active?.graph_revision === 'string'
    && active.graph_revision.length > 0
    ? active.graph_revision
    : null;
  if (pending
      || (snapshot.componentOverview.maintenanceOperation ?? null) !== null
      || payload.manager_ready === false
      || runtime?.converged === false
      || retainedBundleId === null
      || graphRevision === null) {
    return MANAGER_ACTIVATION_PENDING;
  }
  return JSON.stringify({
    receiverEpoch: snapshot.receiverEpoch,
    sourceOwnerId: manager.sourceOwnerId,
    sourceOwnerEpoch: manager.sourceOwnerEpoch,
    cellId: typeof payload.cell_id === 'string' ? payload.cell_id : null,
    retainedBundleId,
    graphRevision,
  });
}

/**
 * Re-read resources that depend on the active applied selection (the node
 * catalog, the Deployment workspace's Applied/runAdmission half) exactly once
 * when the Manager's activation settles: pending -> active, or a different
 * active identity. A read that raced the activation (e.g. a 503
 * `program_applied_selection_not_active` while a Scenario Load was applying)
 * receives a new read after settlement. The separate bounded transient retry
 * handles a failure of that read only when positive Manager readiness is verified.
 * The first observation only establishes the baseline, and an observation
 * without evidence (`null`) keeps it.
 */
export function refreshOnManagerActivationSettled(
  previousKey: string | null,
  nextKey: string | null,
  refreshers: readonly (() => unknown)[],
): string | null {
  if (nextKey === null) return previousKey;
  if (previousKey !== null && previousKey !== nextKey
      && nextKey !== MANAGER_ACTIVATION_PENDING) {
    for (const refresh of refreshers) refresh();
  }
  return nextKey;
}

/** Transient Manager-readiness failures eligible for one read retry per Apply. */
export function isTransientManagerReadinessErrorCode(code: string | null): boolean {
  return code === 'program_applied_selection_not_active'
    || code === 'headless_http_timeout';
}

/** A single resource read failure, tagged with its load token. */
export interface ReadinessTransientResource {
  failureRequestId: number | null;
  latestRequestId: number;
  retryable: boolean;
  refresh: () => number;
}

export interface ReadinessTransientRefreshAttempts {
  catalog: Set<string>;
  nodeEnvironment: Set<string>;
}

function currentCommittedApplyIdentity(
  payload: Record<string, unknown>,
  active: Record<string, unknown>,
  retainedBundleId: string,
  graphRevision: string,
): { applyId: string; applyGeneration: number } | null {
  // The active publication pointer, not Manager Apply history order, names the
  // exact Apply that currently owns this active configuration.
  const publication = objectValue(active.publication);
  const transaction = objectValue(publication?.transaction);
  if (transaction?.kind !== 'named_apply') return null;
  const applyId = typeof transaction.apply_id === 'string' ? transaction.apply_id : '';
  const applyGeneration = transaction.apply_generation;
  if (applyId.trim().length === 0 || typeof applyGeneration !== 'number'
      || !Number.isSafeInteger(applyGeneration) || applyGeneration <= 0) {
    return null;
  }

  const apply = objectValue(payload.apply);
  const records = apply?.records;
  if (!Array.isArray(records)) return null;
  const matching = records.filter((value) => {
    const record = objectValue(value);
    return record?.apply_id === applyId && record.apply_generation === applyGeneration;
  });
  if (matching.length !== 1) return null;
  const record = objectValue(matching[0]);
  const prepared = objectValue(record?.prepared_activation);
  if (record?.incomplete !== false || record.committed !== true
      || record.terminal_state !== 'succeeded'
      || prepared?.retained_bundle_id !== retainedBundleId
      || prepared.graph_revision !== graphRevision) {
    return null;
  }
  return { applyId: applyId.trim(), applyGeneration };
}

/**
 * Positive live-control and Manager evidence for the new transient retry only.
 * The older activation refresh keeps its existing, broader settlement semantics.
 */
export function managerReadinessTransientRetryKey(
  snapshot: ComposerHeadlessState,
  control: {
    transportStatus: ControlTransportStatus;
    semanticStatus: ControlSemanticStatus;
  },
): string | null {
  if (control.transportStatus !== 'connected' || control.semanticStatus !== 'current') {
    return null;
  }
  const manager = snapshot.runtime.sections.manager;
  const sourceOwnerId = manager.sourceOwnerId?.trim() ?? '';
  const sourceOwnerEpoch = manager.sourceOwnerEpoch?.trim() ?? '';
  if (!manager.available || manager.freshness !== 'fresh' || manager.knownGaps.length > 0
      || !Number.isSafeInteger(manager.sourceGeneration) || (manager.sourceGeneration ?? 0) <= 0
      || sourceOwnerId.length === 0 || sourceOwnerEpoch.length === 0) {
    return null;
  }
  const payload = objectValue(manager.payload);
  if (payload === null || payload.manager_ready !== true || pendingManagerApply(payload) !== false
      || (snapshot.componentOverview.maintenanceOperation ?? null) !== null) {
    return null;
  }
  const runtime = objectValue(payload.runtime);
  const active = objectValue(payload.active);
  const cellId = typeof payload.cell_id === 'string' ? payload.cell_id.trim() : '';
  const retainedBundleId = typeof active?.retained_bundle_id === 'string'
    ? active.retained_bundle_id.trim()
    : '';
  const graphRevision = typeof active?.graph_revision === 'string'
    ? active.graph_revision.trim()
    : '';
  if (runtime?.converged !== true || cellId.length === 0 || retainedBundleId.length === 0
      || graphRevision.length === 0 || active === null) {
    return null;
  }
  const applyIdentity = currentCommittedApplyIdentity(
    payload, active, retainedBundleId, graphRevision,
  );
  if (applyIdentity === null) return null;
  return JSON.stringify({
    sourceOwnerId,
    sourceOwnerEpoch,
    cellId,
    retainedBundleId,
    graphRevision,
    applyId: applyIdentity.applyId,
    applyGeneration: applyIdentity.applyGeneration,
  });
}

function retryTransientResource(
  previous: Set<string>,
  activationKey: string,
  resource: ReadinessTransientResource,
): Set<string> {
  if (!resource.retryable || resource.failureRequestId === null
      || resource.failureRequestId < resource.latestRequestId || previous.has(activationKey)) {
    return previous;
  }
  const next = new Set(previous);
  resource.refresh();
  next.add(activationKey);
  return next;
}

/**
 * Retry only the latest failed catalog/environment read, once per valid Apply
 * identity. A failure from an older request is ignored when an activation
 * refresh has already started; its later failure remains eligible. Missing or
 * invalid readiness evidence never retries. The exact committed Apply and
 * stable Manager owner identity de-duplicate readiness flapping and revisiting
 * an older active configuration, while a genuine reapply can retry once.
 */
export function refreshTransientResourcesOnManagerReadiness(
  previous: ReadinessTransientRefreshAttempts,
  activationKey: string | null,
  catalog: ReadinessTransientResource,
  nodeEnvironment: ReadinessTransientResource,
): ReadinessTransientRefreshAttempts {
  if (activationKey === null) return previous;
  return {
    catalog: retryTransientResource(previous.catalog, activationKey, catalog),
    nodeEnvironment: retryTransientResource(previous.nodeEnvironment, activationKey, nodeEnvironment),
  };
}
