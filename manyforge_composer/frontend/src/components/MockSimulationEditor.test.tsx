import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { JsonValue } from '../types/api';
import { reactPropsHost } from '../test/reactPropsHost';
import type { MockSignal } from '../utils/mockSimulation';
import { MockSimulationEditor } from './MockSimulationEditor';

const signals: MockSignal[] = [
  { component_id: 'cell', signal_name: 'door' }, { component_id: 'cell', signal_name: 'part' },
];
const door = { component_id: 'cell', signal_name: 'door' };

afterEach(() => vi.unstubAllGlobals());

describe('MockSimulationEditor', () => {
  it('renders one line per read signal, the change, inactive sources and stale rows', () => {
    const html = renderToStaticMarkup(
      <MockSimulationEditor
        value={[
          { signal: door, start_value: false, change: { after_ms: 300, value: true } },
          { signal: { component_id: 'cell', signal_name: 'lamp' }, start_value: true },
        ]}
        signals={signals}
        profiles={new Map([[JSON.stringify(['cell', 'part']), 'ros_bool']])}
        disabled={false}
        onCommit={() => undefined}
        onClear={() => undefined}
      />,
    );
    expect(html).toContain('aria-label="Mock cell · door"');
    expect(html).toContain('aria-label="Mock cell · part"');
    expect(html).toContain('value="300"');
    expect(html).toContain('Inactive: ROS Bool topic source, not Mock');
    expect(html).toContain('mock_simulation_signal_unused: signal [&quot;cell&quot;,&quot;lamp&quot;]');
    expect(html).toContain('Remove Mock row 2');
  });

  it('commits a new start value, a change, and clears the last row', async () => {
    const onCommit = vi.fn();
    const onClear = vi.fn();
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    const render = (value: JsonValue | undefined) => act(() => root.render(
      <MockSimulationEditor value={value} signals={signals} profiles={new Map()} disabled={false}
        onCommit={onCommit} onClear={onClear} />,
    ));
    const control = (label: string) => find((props) => props['aria-label'] === label);
    try {
      await render(undefined);
      await act(() => control('cell · part start value').onChange?.({ target: { value: 'true' } }));
      expect(onCommit).toHaveBeenLastCalledWith([
        { signal: { component_id: 'cell', signal_name: 'part' }, start_value: true },
      ]);
      await render([{ signal: door, start_value: false }]);
      await act(() => control('cell · door change').onChange?.({ target: { value: 'change' } }));
      expect(onCommit).toHaveBeenLastCalledWith([
        { signal: door, start_value: false, change: { after_ms: 0, value: true } },
      ]);
      await act(() => control('cell · door start value').onChange?.({ target: { value: '' } }));
      expect(onClear).toHaveBeenCalledOnce();
    } finally {
      await act(() => root.unmount());
    }
  });
});
