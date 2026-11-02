import type { ApplyPreview, ApplyResult } from '../types/components';
import { codedErrorText, LIFECYCLE_LABELS } from '../state/componentPresentation';

function names(ids: readonly string[]): string {
  if (ids.length <= 1) return ids.join('');
  return `${ids.slice(0, -1).join(', ')} and ${ids[ids.length - 1]}`;
}

/** The Preview's one-line impact (plan, "Authoring and Apply"). */
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

/** The Preview's Program outcome: nothing when it stays executable. */
function ProgramNotExecutable({ program }: { program: ApplyPreview['program'] }) {
  if (program.executable || program.program_id === null) return null;
  return (
    <div className="inline-message severity-warning" role="alert">
      <p>
        Program <code>{program.program_id}</code> will not be executable after this Apply.
        It is kept unchanged; edit it to run it again.
      </p>
      <ul>
        {program.unresolved_references.map((reference) => (
          <li key={`${reference.component_role_id}/${reference.target_role_id ?? ''}`}>
            {reference.target_role_id !== null
              ? <>Target role <code>{reference.target_role_id}</code></>
              : <>Component role <code>{reference.component_role_id}</code></>}
            {' → '}
            <code>{reference.controllable_component_id}</code>
            {reference.command_target_id !== null && <code>/{reference.command_target_id}</code>}
            {': '}
            {reference.reason.replaceAll('_', ' ')}
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
