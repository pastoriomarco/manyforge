import type { HealthResponse } from '../types/api';
import type { StructuralResourceState } from '../state/structuralResource';

interface UnavailableBannerProps {
  health: StructuralResourceState<HealthResponse>;
  onRetry: () => void;
}

const FALLBACK_GATES = ['H0', 'N', 'I', 'R'];

export function UnavailableBanner({ health, onRetry }: UnavailableBannerProps) {
  const availability = health.status === 'ready'
    ? health.data.runtimeAvailability
    : null;
  const gates = availability?.restorationGates.length
    ? availability.restorationGates
    : FALLBACK_GATES;

  return (
    <section className="unavailable-banner" aria-labelledby="cutover-title">
      <div className="unavailable-icon" aria-hidden="true">!</div>
      <div className="unavailable-copy">
        <p className="eyebrow">Permanent cutover boundary</p>
        <h1 id="cutover-title">Runtime operations are unavailable</h1>
        <p>
          {availability?.message ?? (
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
          {availability?.code ?? 'runtime_unavailable'}
        </span>
        <span className="gate-list">Restore at {gates.join(' · ')}</span>
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
