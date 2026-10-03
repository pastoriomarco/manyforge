import type {
  AppliedProgramSelection,
  JsonValue,
  ProgramTreeNode,
} from '../types/api';
import type { DirectControlMotionStatus } from '../types/robotControl';

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

function requestSummary(requests: SpeedRequest[]): string {
  if (requests.length === 0) return 'none (Deployment maximum)';
  // A Program parameter's value is always its Program default: there is no
  // Scenario or launch-time override.
  const labels = [...new Set(requests.map((request) => (
    request.parameterId !== null
      ? `${request.value ?? '?'} m/s (default of ${request.parameterId})`
      : `${request.value} m/s`
  )))];
  return labels.join(', ');
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
}: {
  applied: AppliedProgramSelection | null;
  targets: DirectControlMotionStatus[];
  groups: Array<{ groupId: string; nodeCount: number; state: string; velocity: string }>;
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
      <div className="program-run-setup-table-wrap">
        <table className="program-run-setup-table">
          <caption>Robots</caption>
          <thead>
            <tr>
              <th scope="col">Robot role</th>
              <th scope="col">Component</th>
              <th scope="col">Deployment TCP max</th>
              <th scope="col">Program requests</th>
            </tr>
          </thead>
          <tbody>
            {targets.map((target) => {
              const key = targetKey(target.controllableComponentId, target.commandTargetId);
              return (
                <tr key={key}>
                  <td>{rolesFor(key).join(', ') || '—'}</td>
                  <td>{target.controllableComponentId}</td>
                  <td>
                    {target.targetDescriptor.operationalLimit === null
                      ? '—'
                      : `${target.targetDescriptor.operationalLimit.maximumTcpLinearSpeedMps} m/s`}
                  </td>
                  <td>
                    {requestSummary(requests.filter(
                      (request) => request.targetRoleId !== null
                        && targetByRole.get(request.targetRoleId) === key,
                    ))}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
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
