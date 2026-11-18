import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import { managerSectionOutstandingExchanges } from '../api/parsers/manager';
import { ManagerWaitingOn } from './ManagerWaitingOn';

describe('Manager waiting on', () => {
  it('reads the live Manager section and names each peer and exchange', () => {
    const exchanges = managerSectionOutstandingExchanges({ runtime: {
      converged: true, desired_units: [], observed_units: [],
      outstanding_exchanges: [
        { peer: 'execution_host', channel: 'management', exchange: 'status', since_ns: 10 },
        { peer: 'program_runner', channel: 'liveness', exchange: 'probe', since_ns: 20 },
      ],
    } });
    const markup = renderToStaticMarkup(<ManagerWaitingOn exchanges={exchanges} />);
    expect(markup).toContain('Execution host · status (management)');
    expect(markup).toContain('Program Runner · probe (liveness)');
  });

  it('has nothing outstanding without the field or a payload', () => {
    expect(managerSectionOutstandingExchanges(null)).toEqual([]);
    expect(managerSectionOutstandingExchanges({ runtime: { converged: true } })).toEqual([]);
  });
});
