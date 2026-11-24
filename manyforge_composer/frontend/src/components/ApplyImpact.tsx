import type {
  ApplyCollisionRuleChange,
  ApplyCollisionRuleMember,
  ApplyCollisionRuleState,
  ApplyMeshDepthReport,
  ApplyPreview,
  ApplyResult,
  ApplyUnresolvedReference,
} from '../types/components';
import { codedErrorText, LIFECYCLE_LABELS } from '../state/componentPresentation';

function names(ids: readonly string[]): string {
  if (ids.length <= 1) return ids.join('');
  return `${ids.slice(0, -1).join(', ')} and ${ids[ids.length - 1]}`;
}

/** The Preview's one-line impact. */
export function applyImpactSummary(preview: ApplyPreview): string {
  const rows = preview.components;
  const restarts = rows.filter((row) => row.restarts).map((row) => row.component_id);
  const added = rows.filter((row) => row.disposition === 'add').map((row) => row.component_id);
  const removed = rows.filter((row) => row.disposition === 'remove').map((row) => row.component_id);
  const parts: string[] = [];
  if (restarts.length > 0) parts.push(`restarts ${names(restarts)}`);
  if (added.length > 0) parts.push(`adds ${names(added)}`);
  if (removed.length > 0) parts.push(`removes ${names(removed)}`);
  if (parts.length === 0) return 'No component changes; every component keeps running.';
  const untouched = rows.some((row) => row.disposition === 'retain' && !row.restarts);
  const sentence = parts.join('; ');
  return `${sentence[0]!.toUpperCase()}${sentence.slice(1)}${untouched ? '; the others keep running' : ''}.`;
}

/** Why a Program reference can't be bound after this Apply. */
const UNRESOLVED_REASON_TEXT: Record<ApplyUnresolvedReference['reason'], string> = {
  component_missing: 'the component is not in the Deployment',
  component_incompatible: 'the component is incompatible',
  target_missing: 'the component has no such target',
  target_incompatible: 'the target is incompatible',
  capability_unavailable: 'a feature this Program uses is unavailable',
};

/** The Preview's Program outcome: nothing when it stays executable or there is
 * no Program (a non-executable Program always names a reference). */
function ProgramNotExecutable({ program }: { program: ApplyPreview['program'] }) {
  if (program.executable || program.unresolved_references.length === 0) return null;
  return (
    <div className="inline-message severity-warning" role="alert">
      <p>
        The Program will not be executable after this Apply.
        It is kept unchanged; edit it to run it again.
      </p>
      <ul>
        {program.unresolved_references.map((reference) => (
          <li
            key={`${reference.target.component_id}/${reference.target.command_target_id}/${reference.reason}`}
            title={reference.detail || undefined}
          >
            <code>{reference.target.component_id}/{reference.target.command_target_id}</code>
            {': '}
            {UNRESOLVED_REASON_TEXT[reference.reason]}
          </li>
        ))}
      </ul>
      {program.affected_nodes.length > 0 && (
        <p>
          Affected steps: {program.affected_nodes.join(', ')}
          {program.affected_execution_groups.length > 0
            && `; groups: ${program.affected_execution_groups.join(', ')}`}
        </p>
      )}
    </div>
  );
}

function memberText(member: ApplyCollisionRuleMember): string {
  if (member.kind === 'link') return `${member.component_id}/${member.link}`;
  if (member.kind === 'attached_object') return `object carried by ${member.component_id}`;
  return 'Scene objects';
}

function ruleValueText(rule: Record<string, unknown>): string {
  if (rule.clearance_m !== undefined) return `clearance ${String(rule.clearance_m)} m`;
  if (rule.contact !== undefined) return 'may touch';
  if (rule.release !== undefined) return 'release';
  return '';
}

/** "A ↔ B: value", or the value alone for the row without selections. */
function ruleStateText(state: ApplyCollisionRuleState): string {
  const value = ruleValueText(state.rule);
  const side = (members: ApplyCollisionRuleMember[]) => (
    members.length === 0 ? 'nothing yet' : members.map(memberText).join(', ')
  );
  return state.rule.a === undefined && state.rule.b === undefined
    ? `everything else, ${value}`
    : `${side(state.a)} ↔ ${side(state.b)}, ${value}`;
}

const RULE_CHANGE_LABELS: Record<ApplyCollisionRuleChange['change'], string> = {
  added: 'added',
  removed: 'removed',
  changed: 'changes',
};

/** The cell collision rules the Apply changes, with their resolved pairs' members. */
function CollisionRuleChanges({ rows }: { rows: readonly ApplyCollisionRuleChange[] }) {
  if (rows.length === 0) return null;
  return (
    <div className="inline-message severity-warning" role="alert" aria-label="Collision rule changes">
      <p>This Apply changes collision rules; confirming it confirms these:</p>
      <ul>
        {rows.map((row) => (
          <li key={row.rule_id}>
            Rule <code>{row.rule_id}</code> {RULE_CHANGE_LABELS[row.change]}
            {row.now !== null && <>: now {ruleStateText(row.now)}</>}
            {row.was !== null && <>{row.now !== null ? '; was ' : ': was '}{ruleStateText(row.was)}</>}
          </li>
        ))}
      </ul>
    </div>
  );
}

/** Depth rules and trip depths that select a mesh that is not closed (a report). */
function MeshDepthReports({ rows }: { rows: readonly ApplyMeshDepthReport[] }) {
  if (rows.length === 0) return null;
  return (
    <div className="inline-message severity-warning" role="alert" aria-label="Meshes not closed">
      <p>
        These depths need closed meshes and cannot be measured; a move under the rule stops, and
        the input is unavailable:
      </p>
      <ul>
        {rows.map((row) => {
          const source = row.collision_rule_id !== undefined
            ? <>Rule <code>{row.collision_rule_id}</code></>
            : <>Input <code>{row.signal_id}</code></>;
          const member = row.member.kind === 'link'
            ? `${row.member.component_id}/${row.member.link}`
            : `Scene object ${row.member.scene_resource_id}`;
          return (
            <li key={`${row.collision_rule_id ?? row.signal_id}/${member}`}>
              {source}: {member}, mesh <code>{row.mesh_uri}</code> is not closed
            </li>
          );
        })}
      </ul>
    </div>
  );
}

/** Per-component impact of a Preview, with the delta's reasons. */
export function ApplyImpact({ preview }: { preview: ApplyPreview }) {
  const changed = preview.components.filter((row) => row.disposition !== 'retain' || row.restarts);
  const attachments = preview.attachment_changed_components;
  const detached = preview.detached_components;
  return (
    <div className="apply-impact" role="status">
      <p>{applyImpactSummary(preview)}</p>
      {(changed.length > 0 || attachments.length > 0 || detached.length > 0) && (
        <ul>
          {changed.map((row) => (
            <li key={row.component_id}>
              <strong>{row.component_id}</strong> · {row.disposition}
              {row.reasons.length > 0 && ` (${row.reasons.join(', ')})`}
            </li>
          ))}
          {attachments.length > 0 && <li>Attachments change for {names(attachments)}</li>}
          {detached.map((id) => (
            <li key={`detached-${id}`}><strong>{id}</strong> stays in the cell, detached at its authored position</li>
          ))}
        </ul>
      )}
      <ProgramNotExecutable program={preview.program} />
      <CollisionRuleChanges rows={preview.collision_rules} />
      <MeshDepthReports rows={preview.mesh_depth_reports} />
      <p className="field-help">Affected components come back prepared; Enable them on the Components page.</p>
    </div>
  );
}

function componentResultText(row: ApplyResult['components'][number]): string {
  if (row.error !== undefined) return `${row.component_id}: ${codedErrorText(row.error)}`;
  if (row.lifecycle_after !== undefined)
    return `${row.component_id} ${LIFECYCLE_LABELS[row.lifecycle_after].toLowerCase()}`;
  // Absent after the Apply: removed, or (a failed Apply) never installed.
  return row.disposition === 'remove' ? `${row.component_id} removed` : `${row.component_id} not in the cell`;
}

/**
 * The Apply/Load/New result, as one line: each affected component's
 * state after the Apply, or its failed step (the Apply continues past one).
 */
export function applyResultText(result: ApplyResult): string {
  const affected = result.components.filter((row) => row.disposition !== 'retain' || row.error !== undefined);
  if (result.outcome === 'failed') {
    const rows = affected.map(componentResultText);
    // The Apply's own failure (one no component row carries) is named first,
    // so a long component list can't hide the cause; a component's failure
    // stays on its named row.
    const overall = result.error;
    if (overall !== undefined && !affected.some((row) => row.error?.code === overall.code
      && row.error.message === overall.message)) {
      rows.unshift(codedErrorText(overall));
    }
    return rows.length === 0 ? 'Apply failed.' : `Apply failed: ${rows.join('; ')}.`;
  }
  if (affected.length === 0) return 'Applied; no component changed.';
  return `Applied: ${affected.map(componentResultText).join(', ')}.`;
}
