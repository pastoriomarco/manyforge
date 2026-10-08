import { describe, expect, it, vi } from 'vitest';

import type { ComposerHeadlessState } from '../types/headless';
import {
  MANAGER_ACTIVATION_PENDING,
  managerActivationSettlementKey,
  nodeEnvironmentRefreshKey,
  refreshOnManagerActivationSettled,
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

function activeManager(): ComposerHeadlessState {
  const state = snapshot();
  state.runtime.sections.manager.sourceGeneration = 1;
  const payload = state.runtime.sections.manager.payload as Record<string, unknown>;
  payload.cell_id = 'cell';
  payload.manager_ready = true;
  payload.runtime = { converged: true };
  payload.apply = { records: [{ incomplete: false }] };
  state.componentOverview.maintenanceOperation = null;
  return state;
}

function managerPayload(state: ComposerHeadlessState): Record<string, unknown> {
  return state.runtime.sections.manager.payload as Record<string, unknown>;
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
