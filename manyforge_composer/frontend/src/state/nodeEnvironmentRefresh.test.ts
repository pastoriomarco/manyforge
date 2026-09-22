import { describe, expect, it } from 'vitest';

import type { ComposerHeadlessState } from '../types/headless';
import { nodeEnvironmentRefreshKey } from './nodeEnvironmentRefresh';

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
