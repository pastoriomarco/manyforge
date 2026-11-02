import type { ComponentState } from '../types/components';

/**
 * The adapter's read-only status lines (CONTRACTS §1 `details`, AUD-2), e.g.
 * an xArm controller's mode, state and error codes, shown as the adapter
 * reports them even without a fault. Display only: nothing reads them for
 * gating. Nothing is shown for a component that reports none.
 */
export function ComponentDetailsCard({ state }: { state: ComponentState }) {
  const details = state.details ?? [];
  if (details.length === 0) return null;
  const id = state.component_id;
  return (
    <section className="dcard" aria-label={`${id} controller status`}>
      <h4>Controller status</h4>
      <dl>
        {details.map((detail) => <div key={detail.name}><dt>{detail.name}</dt><dd>{detail.value}</dd></div>)}
      </dl>
      <p className="field-help">As the controller reports it; shown for diagnosis only.</p>
    </section>
  );
}
