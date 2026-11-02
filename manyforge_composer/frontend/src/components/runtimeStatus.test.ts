import { describe, expect, it } from 'vitest';
import { componentsTabStatus, runtimeStatusPresentation } from './runtimeStatus';
import type { RuntimeStatusView } from './runtimeStatus';
import type { CellLatch, ComponentState } from '../types/components';

function overview(
  values: Pick<RuntimeStatusView, 'readiness' | 'summaryLabel' | 'userMessage'>,
): RuntimeStatusView {
  return {
    schemaVersion: 'manyforge.component_overview.v2',
    overviewRevision: `sha256:${'a'.repeat(64)}`,
    profileLabel: 'standard',
    authorityLabel: 'owner-issued scoped facts',
    capabilities: [],
    components: [],
    maintenanceOperation: null,
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
    expect(presentation.accessibleLabel).toBe('Unavailable');
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
    expect(presentation.accessibleLabel).toBe('Partial');
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

function component(
  component_id: string,
  lifecycle: ComponentState['lifecycle'],
  values: Partial<ComponentState> = {},
): ComponentState {
  return {
    component_id, lifecycle, activity: 'idle', streams: [], capabilities: [], observed_at_ns: 1, ...values,
  };
}

const UNLATCHED: CellLatch = { latched: false, causes: [] };

/** The live cell: owner prepared, active members and plc/station I/O enabled; controller_io and tool_io off. */
function liveCell(): ComponentState[] {
  return [
    component('lite6', 'prepared'),
    component('lite6_robot', 'enabled', { owner_id: 'lite6', role: 'arm' }),
    component('lite_gripper', 'enabled'),
    component('plc_io', 'enabled'),
    component('station_io', 'enabled'),
  ];
}

describe('Components tab status', () => {
  it('is ready with the owner prepared and every other active component enabled', () => {
    expect(componentsTabStatus({ components: liveCell(), latch: UNLATCHED }))
      .toEqual({ readiness: 'ready', summary: 'All active components ready' });
    // A busy component is at its running state too.
    const busy = liveCell().map((state) => state.component_id === 'lite6_robot'
      ? { ...state, activity: 'busy' as const } : state);
    expect(componentsTabStatus({ components: busy, latch: UNLATCHED }).readiness).toBe('ready');
  });

  it('counts only the components in the cell: deactivated members never reach it', () => {
    // controller_io and tool_io are off: absent from the pushed states, so not counted.
    const ids = liveCell().map((state) => state.component_id);
    expect(ids).not.toContain('lite6_controller_io');
    expect(componentsTabStatus({ components: liveCell(), latch: null }).readiness).toBe('ready');
  });

  it('wants attention for a non-owner only prepared, not prepared, offline or unknown activity', () => {
    const cases: Array<[ComponentState, string]> = [
      [component('plc_io', 'prepared'), 'plc_io: Prepared'],
      [component('plc_io', 'not_prepared'), 'plc_io: Not prepared'],
      [component('plc_io', 'offline'), 'plc_io: Offline'],
      [component('plc_io', 'enabled', { activity: 'unknown' }), 'plc_io: Enabled · activity unknown'],
    ];
    for (const [changed, summary] of cases) {
      const states = liveCell().map((state) => state.component_id === 'plc_io' ? changed : state);
      expect(componentsTabStatus({ components: states, latch: UNLATCHED })).toEqual({ readiness: 'degraded', summary });
    }
    // An owner not prepared is away from its running state.
    const owner = liveCell().map((state) => state.component_id === 'lite6' ? component('lite6', 'not_prepared') : state);
    expect(componentsTabStatus({ components: owner, latch: UNLATCHED }))
      .toEqual({ readiness: 'degraded', summary: 'lite6: Not prepared' });
  });

  it('shows a fault over any attention, and the latch as a fault', () => {
    const faulted = liveCell().map((state) => state.component_id === 'plc_io'
      ? component('plc_io', 'not_prepared')
      : state.component_id === 'lite6_robot'
        ? component('lite6_robot', 'fault', {
          owner_id: 'lite6',
          fault: {
            code: 'fault_stream_stale', message: '', clearable: true, operator_action: 'recover', since_ns: 1,
          },
        })
        : state);
    expect(componentsTabStatus({ components: faulted, latch: UNLATCHED }))
      .toEqual({ readiness: 'fault', summary: 'lite6_robot: Fault · Device stopped reporting' });
    const offline = liveCell().map((state) => state.component_id === 'plc_io' ? component('plc_io', 'offline') : state);
    expect(componentsTabStatus({
      components: offline,
      latch: { latched: true, causes: [{ component_id: 'plc_io', kind: 'offline' }] },
    })).toEqual({ readiness: 'fault', summary: 'plc_io: Offline' });
    // The latch alone, every component already back at its running state.
    expect(componentsTabStatus({
      components: liveCell(),
      latch: { latched: true, causes: [{ component_id: 'plc_io', kind: 'offline' }] },
    })).toEqual({ readiness: 'fault', summary: 'plc_io: offline' });
  });

  it('waits while the component states are unread, and an empty cell is ready', () => {
    expect(componentsTabStatus({ components: null, latch: null }).readiness).toBe('waiting');
    expect(componentsTabStatus({ components: [], latch: null }))
      .toEqual({ readiness: 'ready', summary: 'No components in the cell' });
  });
});
