export type LifecycleFactValue = 'true' | 'false' | 'unknown';

export interface RobotLifecycleFactView {
  id: string;
  label: string;
  value: LifecycleFactValue;
}

export interface RobotLifecycleView {
  resourceId: string;
  readiness: string;
  fresh: boolean;
  userMessage: string;
  commandInFlight?: string | null;
  facts: RobotLifecycleFactView[];
  faultMessage?: string | null;
}

/**
 * Observation-only lifecycle card. I7 reconnects resource-addressed actions,
 * confirmations, reconciliation, and capability admission; this component
 * cannot make a robot commandable.
 */
export function RobotLifecycleCard({
  status,
  loading = false,
  error = null,
}: {
  status: RobotLifecycleView | null;
  loading?: boolean;
  error?: string | null;
}) {
  return (
    <section
      className={`robot-lifecycle-card readiness-${status?.readiness ?? 'unavailable'}`}
      aria-labelledby="robot-lifecycle-title"
    >
      <header>
        <span id="robot-lifecycle-title">Robot state</span>
        <code>{status?.resourceId ?? (loading ? 'loading…' : 'unavailable')}</code>
      </header>
      <div className="robot-lifecycle-facts">
        {(status?.facts ?? []).map((fact) => (
          <div key={fact.id}>
            <span>{fact.label}</span>
            <code className={`fact-${fact.value}`}>{fact.value}</code>
          </div>
        ))}
      </div>
      <p>{status?.commandInFlight
        ? `Working: ${status.commandInFlight}`
        : status?.userMessage ?? 'Waiting for robot lifecycle status.'}</p>
      {(status?.faultMessage || error) && (
        <p className="error-state" role="alert">{status?.faultMessage || error}</p>
      )}
      {status && !status.fresh && (
        <p className="muted">Observed lifecycle state is stale; actions remain unavailable.</p>
      )}
    </section>
  );
}
