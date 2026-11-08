import { describe, expect, it, vi } from 'vitest';

import type { ComposerHeadlessState } from '../types/headless';
import {
  MANAGER_ACTIVATION_PENDING,
  managerActivationSettlementKey,
  managerReadinessTransientRetryKey,
  nodeEnvironmentRefreshKey,
  refreshOnManagerActivationSettled,
  refreshTransientResourcesOnManagerReadiness,
  isTransientManagerReadinessErrorCode,
  type ReadinessTransientRefreshAttempts,
  refreshViewerModelsOnManagerChange,
  viewerModelRefreshKey,
} from './nodeEnvironmentRefresh';

function snapshot(): ComposerHeadlessState {
  return {
    receiverEpoch: 'receiver.1',
    runtime: {
      sections: {
        manager: {
          available: true,
          freshness: 'fresh',
          knownGaps: [],
          sourceOwnerEpoch: 'manager.1',
          sourceOwnerId: 'component-manager.cell',
          payload: {
            active: {
              retained_bundle_id: 'sha256:bundle',
              graph_revision: 'sha256:graph',
            },
          },
        },
      },
    },
    componentOverview: {
      overviewRevision: 'sha256:overview-1',
      readiness: 'ready',
      capabilities: ['Execute / Control', 'Observe / Render'],
      components: [{
        id: 'execution_owner',
        ownerEpoch: 'kernel.1',
        ownerId: 'execution-owner.cell',
        reasonCode: null,
        required: true,
        status: 'healthy',
        generation: 1,
        detail: 'first observation age',
      }],
    },
    authoringAdmission: { admissionGeneration: 1 },
    programCycle: {
      cycleGeneration: 1,
      state: 'idle_after_reset',
      runtimeControlRevision: null,
    },
    authoringWorkspace: {
      workspaceEpoch: 'workspace.epoch.1',
      workspaceRevision: 1,
      programDraftId: 'draft.1',
      programDraftRevision: 1,
      deploymentDraftId: 'deployment-draft.1',
      deploymentDraftRevision: 1,
    },
  } as unknown as ComposerHeadlessState;
}

describe('nodeEnvironmentRefreshKey', () => {
  it('ignores ordinary execution and presentation churn', () => {
    const current = snapshot();
    const next = structuredClone(current);
    next.runtime.projectionGeneration = 99;
    next.componentOverview.overviewRevision = 'sha256:overview-2';
    next.componentOverview.components[0]!.generation = 9;
    next.componentOverview.components[0]!.detail = 'new observation age';
    next.authoringAdmission.admissionGeneration = 8;
    next.programCycle.cycleGeneration = 7;
    next.programCycle.state = 'running';
    next.programCycle.runtimeControlRevision = 4;
    next.authoringWorkspace.programDraftRevision = 5;
    expect(nodeEnvironmentRefreshKey(next)).toBe(nodeEnvironmentRefreshKey(current));
  });

  it('changes for a new binding or a semantic owner-health transition', () => {
    const current = snapshot();
    const rebound = structuredClone(current);
    const active = rebound.runtime.sections.manager.payload?.active as Record<string, unknown>;
    active.graph_revision = 'sha256:other-graph';
    expect(nodeEnvironmentRefreshKey(rebound)).not.toBe(
      nodeEnvironmentRefreshKey(current),
    );

    const degraded = structuredClone(current);
    degraded.componentOverview.components[0]!.status = 'offline';
    degraded.componentOverview.components[0]!.reasonCode = 'owner_unavailable';
    expect(nodeEnvironmentRefreshKey(degraded)).not.toBe(
      nodeEnvironmentRefreshKey(current),
    );
  });
});

describe('viewerModelRefreshKey', () => {
  it('refreshes after Apply settles, but ignores terminal history and heartbeat churn', () => {
    const pending = snapshot();
    pending.runtime.sections.manager.sourceGeneration = 1;
    pending.runtime.sections.manager.payload!.apply = {
      records: [{ incomplete: true }],
    };
    const settled = structuredClone(pending);
    settled.runtime.sections.manager.payload!.apply = {
      records: [{ incomplete: false }],
    };
    expect(viewerModelRefreshKey(settled)).not.toBe(viewerModelRefreshKey(pending));

    const heartbeat = structuredClone(settled);
    heartbeat.runtime.sections.manager.sourceGeneration = 2;
    heartbeat.runtime.sections.manager.receiverAgeNs = 500;
    heartbeat.runtime.sections.manager.payload!.apply = {
      records: [{ incomplete: false }, { incomplete: false }],
    };
    expect(viewerModelRefreshKey(heartbeat)).toBe(viewerModelRefreshKey(settled));
  });

  it('re-reads source-model and manifest when a dual-robot composition becomes single-robot', () => {
    const dualRobot = snapshot();
    const dualActive = dualRobot.runtime.sections.manager.payload?.active as Record<string, unknown>;
    dualActive.retained_bundle_id = 'sha256:dual-robot-bundle';
    dualActive.graph_revision = 'sha256:dual-robot-graph';

    const singleRobot = structuredClone(dualRobot);
    const singleActive = singleRobot.runtime.sections.manager.payload?.active as Record<string, unknown>;
    singleActive.retained_bundle_id = 'sha256:single-robot-bundle';
    singleActive.graph_revision = 'sha256:single-robot-graph';

    const refreshSourceModel = vi.fn();
    const refreshManifest = vi.fn();
    const previousKey = viewerModelRefreshKey(dualRobot);
    const nextKey = viewerModelRefreshKey(singleRobot);
    expect(nextKey).not.toBe(previousKey);

    let wakeKey = refreshViewerModelsOnManagerChange(
      null,
      previousKey,
      refreshSourceModel,
      refreshManifest,
    );
    expect(refreshSourceModel).not.toHaveBeenCalled();
    expect(refreshManifest).not.toHaveBeenCalled();

    wakeKey = refreshViewerModelsOnManagerChange(
      wakeKey,
      nextKey,
      refreshSourceModel,
      refreshManifest,
    );
    expect(refreshSourceModel).toHaveBeenCalledOnce();
    expect(refreshManifest).toHaveBeenCalledOnce();
    expect(wakeKey).toBe(nextKey);

    refreshViewerModelsOnManagerChange(
      wakeKey,
      nextKey,
      refreshSourceModel,
      refreshManifest,
    );
    expect(refreshSourceModel).toHaveBeenCalledOnce();
    expect(refreshManifest).toHaveBeenCalledOnce();
  });

  it('refreshes for Manager readiness transitions even when the bundle identity stays put', () => {
    const converged = snapshot();
    const managerPayload = converged.runtime.sections.manager.payload as Record<string, unknown>;
    managerPayload.manager_ready = true;
    managerPayload.runtime = { converged: true };

    const reconciling = structuredClone(converged);
    const reconcilingPayload = reconciling.runtime.sections.manager.payload as Record<string, unknown>;
    reconcilingPayload.manager_ready = false;
    reconcilingPayload.runtime = { converged: false };

    expect(viewerModelRefreshKey(reconciling)).not.toBe(
      viewerModelRefreshKey(converged),
    );
  });
});

const connectedCurrentControl = { transportStatus: 'connected', semanticStatus: 'current' } as const;
const currentRetryKey = (state: ComposerHeadlessState) => (
  managerReadinessTransientRetryKey(state, connectedCurrentControl)
);

function activeManager(): ComposerHeadlessState {
  const state = snapshot();
  state.runtime.sections.manager.sourceGeneration = 1;
  const payload = managerPayload(state);
  payload.cell_id = 'cell';
  payload.manager_ready = true;
  payload.runtime = { converged: true };
  const active = payload.active as Record<string, unknown>;
  active.publication = { transaction: {
    kind: 'named_apply', apply_id: 'apply.1', apply_generation: 1,
  } };
  payload.apply = { records: [currentApplyRecord('apply.1', 1)] };
  state.componentOverview.maintenanceOperation = null;
  return state;
}

function managerPayload(state: ComposerHeadlessState): Record<string, unknown> {
  return state.runtime.sections.manager.payload as Record<string, unknown>;
}

function currentApplyRecord(
  applyId: string,
  applyGeneration: number,
  retainedBundleId = 'sha256:bundle',
  graphRevision = 'sha256:graph',
  overrides: Record<string, unknown> = {},
): Record<string, unknown> {
  return {
    incomplete: false,
    apply_id: applyId,
    apply_generation: applyGeneration,
    committed: true,
    terminal_state: 'succeeded',
    prepared_activation: {
      retained_bundle_id: retainedBundleId,
      graph_revision: graphRevision,
    },
    ...overrides,
  };
}

function setCurrentApply(state: ComposerHeadlessState, applyId: string, applyGeneration: number): void {
  const payload = managerPayload(state);
  const active = payload.active as Record<string, unknown>;
  active.publication = { transaction: {
    kind: 'named_apply', apply_id: applyId, apply_generation: applyGeneration,
  } };
  payload.apply = {
    records: [currentApplyRecord(
      applyId,
      applyGeneration,
      String(active.retained_bundle_id),
      String(active.graph_revision),
    )],
  };
}

describe('managerActivationSettlementKey', () => {
  it('reports pending while an Apply, maintenance, readiness or convergence is outstanding', () => {
    const applying = activeManager();
    managerPayload(applying).apply = { records: [{ incomplete: true }] };
    expect(managerActivationSettlementKey(applying)).toBe(MANAGER_ACTIVATION_PENDING);

    const maintenance = activeManager();
    maintenance.componentOverview.maintenanceOperation = {
      applyId: 'apply.1',
      applyGeneration: 1,
      phase: 'intent_persisted',
      affectedRuntimeUnitIds: [],
      reasonCode: 'planned_maintenance_in_progress',
    };
    expect(managerActivationSettlementKey(maintenance)).toBe(MANAGER_ACTIVATION_PENDING);

    const notReady = activeManager();
    managerPayload(notReady).manager_ready = false;
    expect(managerActivationSettlementKey(notReady)).toBe(MANAGER_ACTIVATION_PENDING);

    const reconciling = activeManager();
    managerPayload(reconciling).runtime = { converged: false };
    expect(managerActivationSettlementKey(reconciling)).toBe(MANAGER_ACTIVATION_PENDING);

    const inactive = activeManager();
    managerPayload(inactive).active = null;
    expect(managerActivationSettlementKey(inactive)).toBe(MANAGER_ACTIVATION_PENDING);
  });

  it('carries no evidence for an unavailable, stale, gapped or malformed Manager section', () => {
    const stale = activeManager();
    stale.runtime.sections.manager.freshness = 'stale';
    expect(managerActivationSettlementKey(stale)).toBeNull();

    const unavailable = activeManager();
    unavailable.runtime.sections.manager.available = false;
    expect(managerActivationSettlementKey(unavailable)).toBeNull();

    const gapped = activeManager();
    gapped.runtime.sections.manager.knownGaps = ['apply'];
    expect(managerActivationSettlementKey(gapped)).toBeNull();

    const malformed = activeManager();
    delete managerPayload(malformed).apply;
    expect(managerActivationSettlementKey(malformed)).toBeNull();
  });

  it('names the active identity and ignores heartbeat, history and execution churn', () => {
    const settled = activeManager();
    const key = managerActivationSettlementKey(settled);
    expect(key).not.toBeNull();
    expect(key).not.toBe(MANAGER_ACTIVATION_PENDING);

    const churn = structuredClone(settled);
    churn.runtime.sections.manager.sourceGeneration = 7;
    churn.runtime.sections.manager.receiverAgeNs = 500;
    managerPayload(churn).apply = {
      records: [{ incomplete: false }, { incomplete: false }],
    };
    churn.runtime.projectionGeneration = 99;
    churn.componentOverview.overviewRevision = 'sha256:overview-2';
    churn.componentOverview.components[0]!.status = 'offline';
    churn.programCycle.cycleGeneration = 3;
    churn.authoringWorkspace.deploymentDraftRevision = 4;
    expect(managerActivationSettlementKey(churn)).toBe(key);

    const rebound = structuredClone(settled);
    (managerPayload(rebound).active as Record<string, unknown>).retained_bundle_id = 'sha256:other';
    expect(managerActivationSettlementKey(rebound)).not.toBe(key);
  });
});

describe('refreshOnManagerActivationSettled', () => {
  function observe(states: ComposerHeadlessState[]) {
    const refreshCatalog = vi.fn();
    const refreshDeploymentWorkspace = vi.fn();
    let wakeKey: string | null = null;
    for (const state of states) {
      wakeKey = refreshOnManagerActivationSettled(
        wakeKey,
        managerActivationSettlementKey(state),
        [refreshCatalog, refreshDeploymentWorkspace],
      );
    }
    return { refreshCatalog, refreshDeploymentWorkspace };
  }

  it('refreshes the catalog and Deployment workspace exactly once when a Scenario Load settles', () => {
    const before = activeManager();
    const accepted = structuredClone(before);
    managerPayload(accepted).apply = { records: [{ incomplete: true }] };
    const published = structuredClone(accepted);
    (managerPayload(published).active as Record<string, unknown>).retained_bundle_id = 'sha256:loaded';
    managerPayload(published).manager_ready = false;
    const reconciling = structuredClone(published);
    managerPayload(reconciling).apply = { records: [{ incomplete: false }] };
    managerPayload(reconciling).manager_ready = true;
    managerPayload(reconciling).runtime = { converged: false };
    const settled = structuredClone(reconciling);
    managerPayload(settled).runtime = { converged: true };
    const heartbeat = structuredClone(settled);
    heartbeat.runtime.sections.manager.sourceGeneration = 9;

    const { refreshCatalog, refreshDeploymentWorkspace } = observe([
      before, accepted, published, reconciling, settled, heartbeat, settled,
    ]);
    expect(refreshCatalog).toHaveBeenCalledOnce();
    expect(refreshDeploymentWorkspace).toHaveBeenCalledOnce();
  });

  it('refreshes when a pending first observation settles, but not on the first settled observation', () => {
    const pending = activeManager();
    managerPayload(pending).apply = { records: [{ incomplete: true }] };
    const settled = activeManager();

    const fromPending = observe([pending, settled]);
    expect(fromPending.refreshCatalog).toHaveBeenCalledOnce();
    expect(fromPending.refreshDeploymentWorkspace).toHaveBeenCalledOnce();

    const fromSettled = observe([settled]);
    expect(fromSettled.refreshCatalog).not.toHaveBeenCalled();
    expect(fromSettled.refreshDeploymentWorkspace).not.toHaveBeenCalled();
  });

  it('does not refresh for unrelated deltas or a stale gap around the same identity', () => {
    const settled = activeManager();
    const churn = structuredClone(settled);
    churn.runtime.sections.manager.sourceGeneration = 5;
    churn.componentOverview.components[0]!.status = 'offline';
    churn.programCycle.state = 'running';
    churn.authoringWorkspace.workspaceRevision = 12;
    const stale = structuredClone(settled);
    stale.runtime.sections.manager.freshness = 'stale';

    const { refreshCatalog, refreshDeploymentWorkspace } = observe([
      settled, churn, stale, settled, churn,
    ]);
    expect(refreshCatalog).not.toHaveBeenCalled();
    expect(refreshDeploymentWorkspace).not.toHaveBeenCalled();
  });
});

describe('managerReadinessTransientRetryKey', () => {
  it('requires strict readiness and the exact active committed Apply transaction', () => {
    expect(isTransientManagerReadinessErrorCode('program_applied_selection_not_active')).toBe(true);
    expect(isTransientManagerReadinessErrorCode('headless_http_timeout')).toBe(true);
    expect(isTransientManagerReadinessErrorCode('catalog_digest_mismatch')).toBe(false);
    const settled = activeManager();
    const key = currentRetryKey(settled);
    expect(key).not.toBeNull();
    expect(managerReadinessTransientRetryKey(settled, {
      transportStatus: 'disconnected', semanticStatus: 'current',
    })).toBeNull();
    expect(managerReadinessTransientRetryKey(settled, {
      transportStatus: 'stale', semanticStatus: 'current',
    })).toBeNull();
    expect(managerReadinessTransientRetryKey(settled, {
      transportStatus: 'connected', semanticStatus: 'synchronizing',
    })).toBeNull();
    expect(managerReadinessTransientRetryKey(settled, connectedCurrentControl)).toBe(key);

    const missingReady = structuredClone(settled);
    delete managerPayload(missingReady).manager_ready;
    expect(currentRetryKey(missingReady)).toBeNull();

    const missingConvergence = structuredClone(settled);
    delete managerPayload(missingConvergence).runtime;
    expect(currentRetryKey(missingConvergence)).toBeNull();

    const missingGeneration = structuredClone(settled);
    missingGeneration.runtime.sections.manager.sourceGeneration = null;
    expect(currentRetryKey(missingGeneration)).toBeNull();

    const missingOwner = structuredClone(settled);
    missingOwner.runtime.sections.manager.sourceOwnerEpoch = null;
    expect(currentRetryKey(missingOwner)).toBeNull();

    const missingCurrentRecord = structuredClone(settled);
    managerPayload(missingCurrentRecord).apply = { records: [] };
    expect(currentRetryKey(missingCurrentRecord)).toBeNull();

    // Apply generations are per Apply ID. A failed newer request with the same
    // generation must not displace the active publication's committed Apply.
    const failedNewerApply = structuredClone(settled);
    const failedPayload = managerPayload(failedNewerApply);
    const apply = failedPayload.apply as { records: Record<string, unknown>[] };
    apply.records.push(currentApplyRecord('apply.failed', 1, 'sha256:failed', 'sha256:failed', {
      committed: false,
      terminal_state: 'failed',
    }));
    expect(currentRetryKey(failedNewerApply)).toBe(key);

    // A later successful Apply can reuse generation 1 under a different ID.
    // The active publication pointer, rather than generation/history order,
    // identifies the current committed Apply.
    const distinctApplyIdsSameGeneration = structuredClone(settled);
    const distinctPayload = managerPayload(distinctApplyIdsSameGeneration);
    (distinctPayload.apply as { records: Record<string, unknown>[] }).records.push(
      currentApplyRecord('apply.2', 1),
    );
    (distinctPayload.active as Record<string, unknown>).publication = { transaction: {
      kind: 'named_apply', apply_id: 'apply.2', apply_generation: 1,
    } };
    const apply2Key = currentRetryKey(distinctApplyIdsSameGeneration);
    expect(apply2Key).not.toBeNull();
    expect(apply2Key).not.toBe(key);
    expect(apply2Key).toContain('"applyId":"apply.2"');

    const duplicateCurrentApply = structuredClone(distinctApplyIdsSameGeneration);
    (managerPayload(duplicateCurrentApply).apply as { records: Record<string, unknown>[] })
      .records.push(currentApplyRecord('apply.2', 1));
    expect(currentRetryKey(duplicateCurrentApply)).toBeNull();

    const failedCurrentApply = structuredClone(failedNewerApply);
    const failedCurrentPayload = managerPayload(failedCurrentApply);
    const failedActive = failedCurrentPayload.active as Record<string, unknown>;
    failedActive.publication = { transaction: {
      kind: 'named_apply', apply_id: 'apply.failed', apply_generation: 1,
    } };
    expect(currentRetryKey(failedCurrentApply)).toBeNull();

    const mismatchedPrepared = structuredClone(settled);
    const record = ((managerPayload(mismatchedPrepared).apply as { records: Record<string, unknown>[] })
      .records[0]!);
    record.prepared_activation = {
      retained_bundle_id: 'sha256:other',
      graph_revision: 'sha256:graph',
    };
    expect(currentRetryKey(mismatchedPrepared)).toBeNull();

    // A bootstrap publication has no named Apply history and is not retried.
    const bootstrap = structuredClone(settled);
    const bootstrapPayload = managerPayload(bootstrap);
    const bootstrapActive = bootstrapPayload.active as Record<string, unknown>;
    bootstrapActive.publication = { transaction: {
      kind: 'bootstrap', bootstrap_id: 'bootstrap.cell', bootstrap_generation: 1,
    } };
    bootstrapPayload.apply = { records: [] };
    expect(currentRetryKey(bootstrap)).toBeNull();

    const uiReceiverReconnect = structuredClone(settled);
    uiReceiverReconnect.receiverEpoch = 'receiver.2';
    expect(currentRetryKey(uiReceiverReconnect)).toBe(key);
  });
});

describe('refreshTransientResourcesOnManagerReadiness', () => {
  const idleResource = {
    failureRequestId: null,
    latestRequestId: 0,
    retryable: false,
    refresh: () => 0,
  };

  it('ignores an old error after settlement refresh, then retries its late transient failure once', () => {
    const readinessKey = currentRetryKey(activeManager());
    const retryCatalog = vi.fn(() => 2);
    let attempts: ReadinessTransientRefreshAttempts = {
      catalog: new Set(), nodeEnvironment: new Set(),
    };

    // Settlement started request 1; request 0 is stale in this render.
    attempts = refreshTransientResourcesOnManagerReadiness(attempts, readinessKey, {
      failureRequestId: 0,
      latestRequestId: 1,
      retryable: true,
      refresh: retryCatalog,
    }, idleResource);
    expect(retryCatalog).not.toHaveBeenCalled();

    // Request 1 fails after readiness is current, so exactly one request 2 follows.
    attempts = refreshTransientResourcesOnManagerReadiness(attempts, readinessKey, {
      failureRequestId: 1,
      latestRequestId: 1,
      retryable: true,
      refresh: retryCatalog,
    }, idleResource);
    expect(retryCatalog).toHaveBeenCalledOnce();
    expect(attempts.catalog.has(readinessKey!)).toBe(true);

    attempts = refreshTransientResourcesOnManagerReadiness(attempts, readinessKey, {
      failureRequestId: 2,
      latestRequestId: 2,
      retryable: true,
      refresh: retryCatalog,
    }, idleResource);
    expect(retryCatalog).toHaveBeenCalledOnce();
  });

  it('keeps one budget per exact Apply across flapping, receiver reconnects, and identity revisits', () => {
    const active = activeManager();
    const firstKey = currentRetryKey(active);
    const retryCatalog = vi.fn(() => 2);
    let attempts: ReadinessTransientRefreshAttempts = {
      catalog: new Set(), nodeEnvironment: new Set(),
    };
    const transientFailure = {
      failureRequestId: 1,
      latestRequestId: 1,
      retryable: true,
      refresh: retryCatalog,
    };

    attempts = refreshTransientResourcesOnManagerReadiness(
      attempts, firstKey, transientFailure, idleResource,
    );
    const reconnect = structuredClone(active);
    reconnect.receiverEpoch = 'receiver.2';
    attempts = refreshTransientResourcesOnManagerReadiness(
      attempts, currentRetryKey(reconnect), transientFailure, idleResource,
    );
    expect(retryCatalog).toHaveBeenCalledOnce();

    const secondApply = structuredClone(active);
    setCurrentApply(secondApply, 'apply.2', 1);
    const secondKey = currentRetryKey(secondApply);
    expect(secondKey).not.toBe(firstKey);
    attempts = refreshTransientResourcesOnManagerReadiness(
      attempts,
      secondKey,
      { ...transientFailure, failureRequestId: 2, latestRequestId: 2 },
      idleResource,
    );
    expect(retryCatalog).toHaveBeenCalledTimes(2);

    // Returning to the first active identity cannot reopen its exhausted budget.
    refreshTransientResourcesOnManagerReadiness(
      attempts,
      firstKey,
      { ...transientFailure, failureRequestId: 3, latestRequestId: 3 },
      idleResource,
    );
    expect(retryCatalog).toHaveBeenCalledTimes(2);

    const ordinaryFailure = { ...transientFailure, retryable: false };
    const ordinaryAttempts = refreshTransientResourcesOnManagerReadiness(
      { catalog: new Set(), nodeEnvironment: new Set() },
      secondKey,
      ordinaryFailure,
      idleResource,
    );
    expect(ordinaryAttempts.catalog.size).toBe(0);
    expect(retryCatalog).toHaveBeenCalledTimes(2);
  });
});
