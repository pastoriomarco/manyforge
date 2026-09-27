import { describe, expect, it, vi } from 'vitest';

import type { ComposerHeadlessState } from '../types/headless';
import {
  nodeEnvironmentRefreshKey,
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
