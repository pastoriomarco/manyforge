import type { HealthResponse } from '../types/api';
import type { StructuralResourceState } from '../state/structuralResource';

interface UnavailableBannerProps {
  health: StructuralResourceState<HealthResponse>;
  onRetry: () => void;
}

const FALLBACK_GATES = ['N', 'L0', 'P5', 'I', 'R', 'T'];

export function UnavailableBanner({ health, onRetry }: UnavailableBannerProps) {
  const headlessConnected = health.status === 'ready'
    && health.data.mode === 'headless_client'
    && health.data.runtimeAvailability === null;
  const availability = health.status === 'ready'
    ? health.data.runtimeAvailability
    : null;
  // An explicit empty list means no implementation gate can repair the
  // current failure (for example, the configured H0 API is unreachable).
  // Reserve the historical fallback for responses that do not provide an
  // availability record at all.
  const gates = availability === null
    ? FALLBACK_GATES
    : availability.restorationGates;

  return (
    <section className="unavailable-banner" aria-labelledby="cutover-title">
      <div className="unavailable-icon" aria-hidden="true">!</div>
      <div className="unavailable-copy">
        <p className="eyebrow">Composable-cell cutover boundary</p>
        <h1 id="cutover-title">
          {headlessConnected
            ? 'Headless runtime truth is connected'
            : 'Runtime operations are unavailable'}
        </h1>
        <p>
          {headlessConnected
            ? 'Manager, scene, alarm, and history projections no longer depend on Composer. Program execution and motion remain unavailable until their named gates.'
            : availability?.message ?? (
            'Composer currently exposes authored structure only. It cannot run a '
            + 'program, command a robot, edit a live scene, or report runtime state.'
            )}
        </p>
        <p className="structural-scope">
          Available here: immutable v2 catalog inspection, sandboxed artifact-path
          browsing, and an authored-neutral URDF view. Nothing on this page asserts
          live or operational validity.
        </p>
      </div>
      <div className="availability-meta">
        <span className="status-pill status-pill-danger">
          {headlessConnected
            ? 'headless_h0_active'
            : availability?.code ?? 'runtime_unavailable'}
        </span>
        <span className="gate-list">
          {headlessConnected
            ? 'Later capabilities restore at N · L0 · P5 · I · R · T'
            : gates.length > 0
              ? `Restore at ${gates.join(' · ')}`
              : 'Check headless API connectivity and configuration'}
        </span>
        {health.status === 'error' && (
          <>
            <span className="health-error">Health read failed: {health.message}</span>
            <button type="button" className="text-button" onClick={onRetry}>
              Retry health
            </button>
          </>
        )}
      </div>
    </section>
  );
}
