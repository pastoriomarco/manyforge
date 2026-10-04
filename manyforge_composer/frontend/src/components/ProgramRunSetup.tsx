import type {
  AppliedProgramSelection,
  JsonValue,
  ProgramTreeNode,
} from '../types/api';
import type { DirectControlMotionStatus, ProgramRunAdmittedSpeeds } from '../types/robotControl';

/**
 * The owner-admitted speeds for Run details, as read from the backend. The
 * browser only displays them; it never derives an admitted speed.
 */
export type AdmittedSpeedsView =
  | { kind: 'no_run' }
  | { kind: 'loading' }
  | { kind: 'unavailable'; reason: string }
  | { kind: 'ready'; speeds: ProgramRunAdmittedSpeeds };

const ADMITTED_HEADER_HINT = 'TCP speed the execution owner admitted for this robot\'s most recent '
  + 'prepared motion in this run (the request capped by the applied Deployment maximum), read from '
  + 'operation history. Motions not yet prepared have no admitted value; nothing is estimated here.';

/** One motion node's TCP-speed request, as authored. */
interface SpeedRequest {
  targetRoleId: string | null;
  value: number | null;
  parameterId: string | null;
}

const targetKey = (componentId: string, commandTargetId: string) => `${componentId}/${commandTargetId}`;

/**
 * Program target role -> exact command target (controllable component and
 * command target), from the Applied Binding's target-role rows.
 */
function targetRoleTargets(applied: AppliedProgramSelection): Map<string, string> {
  return new Map(applied.programBinding.target_role_bindings.map((row) => [
    row.target_role_id,
    targetKey(row.controllable_component_id, row.command_target_id),
  ]));
}

function speedRequests(applied: AppliedProgramSelection): SpeedRequest[] {
  const parameters = new Map(
    (applied.program.parameters ?? []).map((row) => [row.parameter_id, row.default]),
  );
  const requests: SpeedRequest[] = [];
  const walk = (node: ProgramTreeNode) => {
    const literal = node.parameters.max_tcp_linear_speed_mps;
    const source = node.value_sources?.max_tcp_linear_speed_mps;
    const role = node.parameters.target_role;
    const targetRoleId = typeof role === 'string' ? role : null;
    if (source !== undefined && source.kind === 'program_parameter') {
      const value: JsonValue | undefined = parameters.get(source.reference);
      requests.push({
        targetRoleId,
        value: typeof value === 'number' ? value : null,
        parameterId: source.reference,
      });
    } else if (typeof literal === 'number') {
      requests.push({ targetRoleId, value: literal, parameterId: null });
    }
    node.children.forEach(walk);
  };
  walk(applied.program.tree);
  return requests;
}

function requestSummary(requests: SpeedRequest[]): { value: string; note: string | null } {
  if (requests.length === 0) return { value: '—', note: 'no request: Deployment maximum' };
  // A Program parameter's value is always its Program default: there is no
  // Scenario or launch-time override.
  const values = [...new Set(requests.map((request) => `${request.value ?? '?'} m/s`))];
  const sources = [...new Set(requests
    .filter((request) => request.parameterId !== null)
    .map((request) => `default of ${request.parameterId}`))];
  return { value: values.join(', '), note: sources.length === 0 ? null : sources.join(', ') };
}

function AdmittedSpeedCell({
  admitted,
  target,
}: {
  admitted: AdmittedSpeedsView;
  target: DirectControlMotionStatus;
}) {
  const row = admitted.kind === 'ready'
    ? admitted.speeds.targets.find((candidate) => (
      candidate.controllableComponentId === target.controllableComponentId
      && candidate.commandTargetId === target.commandTargetId
    ))
    : undefined;
  if (row !== undefined) {
    const requested = row.requestedTcpLinearSpeedMps === null
      ? 'no per-motion request'
      : `requested ${row.requestedTcpLinearSpeedMps} m/s`;
    return (
      <div className="program-run-setup-stat" title={`Admitted by the owner for operation ${row.operationId} (prepared ${row.occurredAtWall}): ${requested}, Deployment maximum ${row.deploymentMaximumTcpLinearSpeedMps} m/s.`}>
        <span className="program-run-setup-stat-label" title={ADMITTED_HEADER_HINT}>Last admitted</span>
        <span className="program-run-setup-stat-value">{row.admittedTcpLinearSpeedMps} m/s</span>
        <span className="program-run-setup-stat-note">by the Kernel</span>
      </div>
    );
  }
  const reason = admitted.kind === 'no_run'
    ? 'No Program run, so no motion has been admitted.'
    : admitted.kind === 'loading'
      ? 'Reading operation history.'
      : admitted.kind === 'unavailable'
        ? `Operation history could not be read: ${admitted.reason}`
        : 'No motion of this robot has been prepared in this run yet, or it is older than the recent '
          + 'history read. The owner admits a speed only when it prepares a motion.';
  return (
    <div className="program-run-setup-stat" title={reason}>
      <span className="program-run-setup-stat-label" title={ADMITTED_HEADER_HINT}>Last admitted</span>
      <span className="program-run-setup-stat-value">—</span>
      <span className="program-run-setup-stat-note">no motion yet</span>
    </div>
  );
}

/**
 * What is running, in plain terms: the Applied Program and, per robot, the
 * Deployment speed limit next to the speeds the Program asks for, plus the
 * motion groups with their velocity. Read-only.
 */
export function ProgramRunSetup({
  applied,
  targets,
  groups,
  admitted,
}: {
  applied: AppliedProgramSelection | null;
  targets: DirectControlMotionStatus[];
  groups: Array<{ groupId: string; nodeCount: number; state: string; velocity: string }>;
  admitted: AdmittedSpeedsView;
}) {
  if (applied === null) {
    return <p className="hint">No Program is applied.</p>;
  }
  const requests = speedRequests(applied);
  const targetByRole = targetRoleTargets(applied);
  const rolesFor = (key: string) => [...targetByRole]
    .filter(([, target]) => target === key)
    .map(([role]) => role);
  return (
    <div className="program-run-setup">
      <p>
        Program <strong>{applied.programId}</strong> in Scenario{' '}
        <strong>{applied.scenarioId}</strong>
        <span className="hint"> · revision {applied.programRevision.slice(0, 15)}…</span>
      </p>
      <section className="program-run-setup-robots" aria-label="Robots">
        <h5>Robots</h5>
        {targets.map((target) => {
          const key = targetKey(target.controllableComponentId, target.commandTargetId);
          return (
            <div key={key} className="program-run-setup-robot">
              <h6>
                {rolesFor(key).join(', ') || '—'}
                <span className="program-run-setup-component">{target.controllableComponentId}</span>
              </h6>
              <div className="program-run-setup-stats">
                <div className="program-run-setup-stat">
                  <span className="program-run-setup-stat-label">Deployment max</span>
                  <span className="program-run-setup-stat-value">
                    {target.targetDescriptor.operationalLimit === null
                      ? '—'
                      : `${target.targetDescriptor.operationalLimit.maximumTcpLinearSpeedMps} m/s`}
                  </span>
                  <span className="program-run-setup-stat-note">TCP speed limit</span>
                </div>
                {(() => {
                  const request = requestSummary(requests.filter(
                    (candidate) => candidate.targetRoleId !== null
                      && targetByRole.get(candidate.targetRoleId) === key,
                  ));
                  return (
                    <div className="program-run-setup-stat">
                      <span className="program-run-setup-stat-label">Program request</span>
                      <span className="program-run-setup-stat-value">{request.value}</span>
                      {request.note !== null && (
                        <span className="program-run-setup-stat-note">{request.note}</span>
                      )}
                    </div>
                  );
                })()}
                <AdmittedSpeedCell admitted={admitted} target={target} />
              </div>
            </div>
          );
        })}
      </section>
      {groups.length > 0 && (
        <div className="program-run-setup-table-wrap">
          <table className="program-run-setup-table">
            <caption>Motion groups</caption>
            <thead>
              <tr>
                <th scope="col">Group</th>
                <th scope="col">Nodes</th>
                <th scope="col">State</th>
                <th scope="col">Velocity</th>
              </tr>
            </thead>
            <tbody>
              {groups.map((group) => (
                <tr key={group.groupId}>
                  <td>{group.groupId}</td>
                  <td>{group.nodeCount}</td>
                  <td>{group.state === 'unavailable' ? 'no active run' : group.state}</td>
                  <td>{group.velocity}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}
