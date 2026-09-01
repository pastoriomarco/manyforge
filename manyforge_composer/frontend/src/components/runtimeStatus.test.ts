import { describe, expect, it } from 'vitest';
import { runtimeStatusPresentation } from './runtimeStatus';
import type { RuntimeStatusView } from './runtimeStatus';

function overview(
  values: Pick<RuntimeStatusView, 'readiness' | 'summaryLabel' | 'userMessage'>,
): RuntimeStatusView {
  return {
    schemaVersion: 'manyforge.component_overview.v1',
    overviewRevision: `sha256:${'a'.repeat(64)}`,
    profileLabel: 'native',
    authorityLabel: 'owner-issued scoped facts',
    capabilities: [],
    components: [],
    basis: {
      runtimeProjectionOwnerEpoch: null,
      runtimeProjectionGeneration: null,
      managerSourceOwnerEpoch: null,
      logicalSceneSourceOwnerEpoch: null,
      logicalSceneSourceGeneration: null,
      sceneRuntimeSourceOwnerEpoch: null,
      sceneRuntimeSourceGeneration: null,
      alarmOwnerEpoch: null,
      alarmGeneration: null,
      alarmStateRevision: null,
      admissionGeneration: null,
      programRunnerOwnerEpoch: null,
      programCycleGeneration: null,
      appliedProgramOwnerEpoch: null,
      appliedProgramGeneration: null,
      executionOwnerEpoch: null,
      executionPublicationGeneration: null,
    },
    ...values,
  };
}

describe('Deployment status presentation', () => {
  it('reports unavailable state without inventing readiness', () => {
    const presentation = runtimeStatusPresentation({
      status: null,
      backendConnected: false,
      error: 'Headless status is not qualified.',
      connectionDetail: 'Headless status is not qualified.',
    });
    expect(presentation.readiness).toBe('offline');
    expect(presentation.accessibleLabel).toBe('Off');
    expect(presentation.detail).toBe('Headless status is not qualified.');
  });

  it('uses only adapter-supplied summary and readiness fields', () => {
    const presentation = runtimeStatusPresentation({
      status: overview({
        readiness: 'degraded',
        summaryLabel: 'Partial',
        userMessage: 'One optional component is unavailable.',
      }),
      backendConnected: true,
      connectionDetail: 'Current projection',
    });
    expect(presentation.readiness).toBe('degraded');
    expect(presentation.accessibleLabel).toBe('Warning');
    expect(presentation.detail).toBe('One optional component is unavailable.');
  });

  it('counts only actionable alarm and Composer attention', () => {
    const presentation = runtimeStatusPresentation({
      status: overview({
        readiness: 'degraded',
        summaryLabel: 'Attention required',
        userMessage: 'One client response requires review.',
      }),
      backendConnected: true,
      connectionDetail: 'Current projection',
      unacknowledgedAlarms: 1,
      clientErrorCount: 1,
    });

    expect(presentation.attentionCount).toBe(2);
  });
});
