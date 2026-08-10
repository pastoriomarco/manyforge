import type { HealthResponse } from '../types/api';
import type { StructuralResourceState } from '../state/structuralResource';

interface UnavailableBannerProps {
  health: StructuralResourceState<HealthResponse>;
  onRetry: () => void;
}

const FALLBACK_GATES = ['N7', 'L0', 'P5', 'I', 'R', 'T'];

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
            ? 'Manager, scene, alarm, history, addressed control, and the bounded Program Run surface no longer depend on Composer. Complete product readiness remains gated.'
            : availability?.message ?? (
            'Composer currently exposes authored structure only. It cannot run a '
            + 'program, command a robot, edit a live scene, or report runtime state.'
            )}
        </p>
        <p className="structural-scope">
          {headlessConnected
            ? 'Each runtime pane reports strict owner-issued facts. Missing owners remain unknown; this page does not infer robot readiness or a complete product lane.'
            : 'Available here: immutable v2 catalog inspection, sandboxed artifact-path browsing, and an active retained-neutral URDF view. Nothing on this page asserts live or operational validity.'}
        </p>
      </div>
      <div className="availability-meta">
        <span className="status-pill status-pill-danger">
          {headlessConnected
            ? 'headless_client_active'
            : availability?.code ?? 'runtime_unavailable'}
        </span>
        <span className="gate-list">
          {headlessConnected
            ? 'Complete I6 qualification at N7 · then L0 · P5 · I · R · T'
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
