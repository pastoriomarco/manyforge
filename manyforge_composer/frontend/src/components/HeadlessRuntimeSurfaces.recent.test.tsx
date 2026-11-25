import { act } from 'react';
import { afterEach, expect, it, vi } from 'vitest';

import { api } from '../api/client';
import { reactPropsHost, type MountedProps } from '../test/reactPropsHost';
import { recordRecentNotice } from '../state/workbenchNotices';
import { HeadlessRuntimeSurfacesView } from './HeadlessRuntimeSurfaces';

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
  vi.unstubAllEnvs();
});

const text = (props: MountedProps): string => (Array.isArray(props.children)
  ? props.children.filter((part) => typeof part === 'string' || typeof part === 'number').join('')
  : typeof props.children === 'string' ? props.children : '');

it('lists the recent notices newest first; see History opens History filtered by the notice\'s run', async () => {
  vi.stubEnv('TZ', 'Asia/Kolkata');
  const { host, find, all } = reactPropsHost();
  const list = vi.spyOn(api.history, 'list').mockResolvedValue({
    events: [], retention: null, truncated: false, nextBeforeSequence: null,
    timeWindow: { startUtcNs: null, endUtcNs: null },
  } as unknown as Awaited<ReturnType<typeof api.history.list>>);
  vi.spyOn(api.history, 'discovery').mockResolvedValue({ kind: 'application_start_id', values: [] } as never);
  recordRecentNotice({ id: 'a', severity: 'info', domain: 'runtime', title: 'Program', message: 'Saved.' }, 1_795_514_400_123);
  recordRecentNotice({
    id: 'b', severity: 'warning', domain: 'runtime', title: 'Retrying pick', message: '',
    history: { programRunId: 3 },
  }, 1_795_514_401_000);
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<HeadlessRuntimeSurfacesView enabled expanded headlessState={{
    // Only what the overlay reads: no alarms, the current start and run.
    snapshot: {
      applicationStartId: '3f9a0000000000a1', alarms: { alarms: [], generation: 1 }, programCycle: { programRunId: 3 },
    },
    error: null, transportStatus: 'connected', transportError: null,
    commandLatch: false, semanticStatus: 'current', semanticReason: null, reconnect: () => undefined,
  } as never} />));
  await act(() => find((props) => text(props) === 'Recent 2').onClick!());
  // Local time (UTC+05:30 here) with milliseconds; UTC in the tooltip.
  const times = all((props) => typeof props.title === 'string' && props.title.endsWith(' UTC'));
  expect(times.map((props) => [props.children, props.title])).toEqual([
    ['2026-11-24 15:30:01.000', '2026-11-24 10:00:01.000 UTC'],
    ['2026-11-24 15:30:00.123', '2026-11-24 10:00:00.123 UTC'],
  ]);
  const links = all((props) => props.children === 'see History');
  expect(links).toHaveLength(2);
  await act(async () => { links[0].onClick!(); });
  expect(list).toHaveBeenCalledWith(
    expect.objectContaining({ programRunId: 3, applicationStartId: '3f9a0000000000a1' }), expect.anything());
  await act(() => root.unmount());
});
