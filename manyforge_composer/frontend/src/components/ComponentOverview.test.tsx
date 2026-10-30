import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import fixture from '../testFixtures/backendSnapshots/component-overview.ready.json';
import { parseComponentOverviewProjection } from '../api/parsers/headless';
import { ComponentOverview } from './ComponentOverview';
import { runtimeStatusPresentation } from './runtimeStatus';

describe('empty Scenario component overview', () => {
  it('parses and presents neutral rows without adding attention', () => {
    const raw = structuredClone(fixture);
    raw.components[0]!.status = 'not_applicable';
    raw.components[0]!.detail = 'No robot in this Scenario';
    raw.components[0]!.required = false;
    const status = parseComponentOverviewProjection(raw, 'overview');
    const html = renderToStaticMarkup(<ComponentOverview
      request={0} status={status} backendConnected connectionLabel="Connected"
      connectionDetail="Current" contextFacts={[]} unacknowledgedAlarms={1}
      activeAcknowledgedAlarms={0} clientErrorCount={0} error={null}
      onReconnect={() => {}} onOpenRuntimeHistory={() => {}}
    />);
    expect(html).toContain('status-not_applicable');
    expect(html).toContain('No robot in this Scenario');
    expect(html).toContain('Not applicable');
    expect(runtimeStatusPresentation({
      status, backendConnected: true, connectionDetail: 'Current', unacknowledgedAlarms: 1,
    }).attentionCount).toBe(1);
  });
});
