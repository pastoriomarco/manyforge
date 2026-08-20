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

  if (headlessConnected) {
    return (
      <section className="unavailable-banner availability-notice availability-connected" aria-label="Headless owner status">
        <span className="availability-dot" aria-hidden="true" />
        <div className="availability-copy">
          <p className="eyebrow">Owner boundary</p>
          <strong>Headless runtime truth is connected</strong>
          <span>
            Manager, scene, alarm, history, addressed control, and the bounded
            Program Run surface report owner-issued facts. Complete product
            readiness remains gated.
          </span>
        </div>
        <span className="status-pill status-pill-connected">headless_client_active</span>
        <details className="availability-details">
          <summary>Qualification boundary</summary>
          <span>Missing owners remain unknown; this page does not infer robot readiness.</span>
          <span>Complete I6 qualification at N7 · then L0 · P5 · I · R · T.</span>
        </details>
      </section>
    );
  }

  const title = health.status === 'loading'
    ? 'Reading runtime boundary status'
    : 'Runtime operations are unavailable';
  const reason = health.status === 'loading'
    ? 'Checking the configured structural and headless owner boundary.'
    : health.status === 'error' || health.status === 'absent'
      ? health.message
      : availability?.message ?? (
        'Composer currently exposes authored structure only. It cannot run a '
        + 'program, command a robot, edit a live scene, or report runtime state.'
      );
  const code = health.status === 'ready'
    ? availability?.code ?? 'runtime_unavailable'
    : health.status === 'absent' ? 'health_unavailable' : 'health_read_failed';

  return (
    <section className="unavailable-banner availability-notice" aria-labelledby="cutover-title">
      <div className="unavailable-icon" aria-hidden="true">!</div>
      <div className="unavailable-copy">
        <p className="eyebrow">Runtime boundary</p>
        <strong id="cutover-title">{title}</strong>
        <span>{reason}</span>
      </div>
      <div className="availability-meta">
        <span className="status-pill status-pill-danger">
          {code}
        </span>
        <button type="button" className="text-button" onClick={onRetry}>
          Retry health
        </button>
      </div>
      <details className="availability-details">
        <summary>Details</summary>
        <span>
          {gates.length > 0
            ? `Restore at ${gates.join(' · ')}`
            : 'Check headless API connectivity and configuration'}
        </span>
        {health.status === 'error' && <span>Health read failed: {health.message}</span>}
        <span>
          Available here: immutable catalog inspection, sandboxed artifact-path
          browsing, and a retained-neutral URDF view. No readiness is inferred.
        </span>
      </details>
    </section>
  );
}
