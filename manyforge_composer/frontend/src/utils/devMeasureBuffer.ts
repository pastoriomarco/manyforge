/**
 * React 19.2 development builds record a `performance.measure` (carrying a
 * props diff in `detail`) for every committed component render, for the
 * browser profiler's React tracks. Browsers keep user-timing entries until
 * they are cleared, so a dev-served Composer left open for hours grows its
 * heap without bound and then stalls in multi-second garbage collections:
 * control heartbeats go unanswered and Headless reports Control delayed/stale.
 * Composer reads no measures, and profilers record them when created, so
 * clearing the buffer periodically loses nothing.
 */
export const DEV_MEASURE_CLEAR_INTERVAL_MS = 10_000;

export function boundDevMeasureBuffer(
  timeline: Pick<Performance, 'clearMeasures'> = performance,
  every: (callback: () => void, intervalMs: number) => unknown = setInterval,
): void {
  every(() => timeline.clearMeasures(), DEV_MEASURE_CLEAR_INTERVAL_MS);
}
