import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import fixture from '../testFixtures/backendSnapshots/component-overview.ready.json';
import { parseComponentOverviewProjection } from '../api/parsers/headless';
import type { CellState, ComponentState } from '../types/components';
import { ComponentOverview } from './ComponentOverview';
import { runtimeStatusPresentation } from './runtimeStatus';

const robot: ComponentState = {
  component_id: 'lite6_robot',
  lifecycle: 'fault',
  fault: { code: 'fault_estop', message: 'C2', clearable: true, operator_action: 'enable', since_ns: 1 },
  activity: 'idle',
  streams: [],
  capabilities: [],
  observed_at_ns: 2,
};

function render(cellState: CellState, mounts?: Record<string, string>) {
  const status = parseComponentOverviewProjection(structuredClone(fixture), 'overview');
  return renderToStaticMarkup(<ComponentOverview
    request={0} status={status} backendConnected connectionLabel="Connected"
    connectionDetail="Current" contextFacts={[]} unacknowledgedAlarms={1}
    activeAcknowledgedAlarms={0} clientErrorCount={0} error={null} cellState={cellState} mounts={mounts}
    onReconnect={() => {}} onOpenRuntimeHistory={() => {}}
  />);
}

describe('component overview', () => {
  it('lists the pushed component states with their fault text', () => {
    const html = render({ components: [robot], latch: null, io: null, readiness: null });
    expect(html).toContain('lite6_robot');
    expect(html).toContain('status-error');
    expect(html).toContain('<small>Emergency stop active</small>');
    expect(html).toContain('<span class="status-pill status-pill-error">Fault</span>');
  });

  it('lists the components as one tree: machine, arm, the tool mounted on the arm', () => {
    const state = (component_id: string, patch: Partial<ComponentState> = {}): ComponentState => ({
      component_id, lifecycle: 'prepared', activity: 'idle', streams: [], capabilities: [], observed_at_ns: 1, ...patch,
    });
    const html = render({ components: [
      state('lite_gripper'), state('lite6'), state('lite6_robot', { owner_id: 'lite6', role: 'arm' }),
    ], latch: null, io: null, readiness: null }, { lite_gripper: 'lite6_robot' });
    const order = ['lite6', 'lite6_robot', 'lite_gripper'].map((id) => html.indexOf(`<strong>${id}</strong>`));
    expect(order).toEqual([...order].sort((left, right) => left - right));
    expect(html).toContain('style="--component-depth:2"><span class="component-dot status-neutral"></span><span><strong>lite_gripper</strong>');
    expect(html).toContain('<strong>lite6_robot</strong><small>arm</small>');
  });

  it('summarizes the cell latch with its leading cause', () => {
    const html = render({
      components: [robot],
      latch: { latched: true, causes: [
        { component_id: 'lite6_robot', kind: 'fault', fault: robot.fault },
        { component_id: 'gripper', kind: 'offline' },
      ] },
      io: null,
      readiness: null,
    });
    expect(html).toContain('2 errors require attention · lite6_robot: Emergency stop active');
    expect(html).toContain('gripper: offline');
  });

  it('waits for component states and counts only alarm attention', () => {
    const html = render({ components: null, latch: null, io: null, readiness: null });
    expect(html).toContain('Component states unavailable');
    const status = parseComponentOverviewProjection(structuredClone(fixture), 'overview');
    expect(runtimeStatusPresentation({
      status, backendConnected: true, connectionDetail: 'Current', unacknowledgedAlarms: 1,
    }).attentionCount).toBe(1);
  });
});
