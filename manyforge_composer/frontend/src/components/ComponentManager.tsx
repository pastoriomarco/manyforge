import { useState, type CSSProperties } from 'react';

import {
  cellApplyErrorText,
  type DeploymentWorkspaceController,
} from '../hooks/useDeploymentWorkspace';
import { deploymentActionUnavailableReason } from '../state/documentStates';
import { busyLabel, busyReason } from '../state/busyOperation';
import type { ApplyPreview, ComponentState } from '../types/components';
import {
  componentStateDetail,
  componentSummary,
  componentTone,
  LIFECYCLE_LABELS,
  TONE_BADGE_CLASSES,
} from '../state/componentPresentation';
import { ApplyImpact } from './ApplyImpact';
import type {
  ManagerDiffProjection,
  ManagerGraphProjection,
} from '../types/api';
import {
  componentRowText,
  type ComponentManagerRow,
  type ComponentManagerTab,
} from '../utils/componentManager';

/**
 * The Deployment tab pieces (Overview, Program binding, Cell settings, Changes) and the
 * Components section's list: a thin client of the Deployment Draft routes,
 * the server-side Preview/Apply and the pushed component states. It owns no
 * lifecycle or Applied state.
 */

const TAB_LABELS: Record<ComponentManagerTab, string> = {
  overview: 'Overview',
  binding: 'Program binding',
  cell: 'Cell settings',
  changes: 'Changes',
};

function shortIdentity(value: string | null | undefined): string {
  if (!value) return 'none';
  return value.length > 32 ? value.slice(0, 28) + '…' : value;
}

export const CHANGE_LABELS: Record<ComponentManagerRow['change'], string> = {
  applied: 'Applied',
  added: 'Added in Draft · runs after Apply',
  removed: 'Removed in Draft · stops at Apply',
};

export function ComponentManagerTabs({ tab, onTab, pendingChanges }: {
  tab: ComponentManagerTab;
  onTab: (tab: ComponentManagerTab) => void;
  pendingChanges: boolean;
}) {
  return (
    <div className="dseg" role="tablist" aria-label="Deployment">
      {(Object.keys(TAB_LABELS) as ComponentManagerTab[]).map((value) => (
        <button key={value} type="button" role="tab" aria-selected={tab === value}
          aria-controls={`component-manager-${value}`} onClick={() => onTab(value)}>
          {TAB_LABELS[value]}
          {value === 'changes' && pendingChanges ? ' · pending' : ''}
        </button>
      ))}
    </div>
  );
}

/** Desired (Draft) against active (Manager Applied), with routes to the detail. */
export function ComponentManagerSummary({ controller, rows, graph, diff, onNavigate, onOpenComponents }: {
  controller: DeploymentWorkspaceController;
  rows: readonly ComponentManagerRow[];
  graph: ManagerGraphProjection | null;
  diff: ManagerDiffProjection | null;
  onNavigate: (tab: ComponentManagerTab) => void;
  /** Opens the Components section (per-component status and settings). */
  onOpenComponents?: () => void;
}) {
  const workspace = controller.workspace;
  const added = rows.filter((row) => row.change === 'added').length;
  const removed = rows.filter((row) => row.change === 'removed').length;
  const desired = rows.length - removed;
  const applyReason = deploymentActionUnavailableReason(workspace, 'apply', null);
  return (
    <section className="dcard" aria-labelledby="component-manager-summary-title">
      <h4 id="component-manager-summary-title">Desired and active</h4>
      <dl>
        <div><dt>Deployment Draft</dt><dd>
          {desired} component{desired === 1 ? '' : 's'}
          {added > 0 ? ` · ${added} to add` : ''}{removed > 0 ? ` · ${removed} to remove` : ''}
        </dd></div>
        <div><dt>Manager Applied</dt><dd>
          {workspace?.managerApplied.activationOrder?.componentIds.length ?? 0} components
          {' · graph '}{shortIdentity(graph?.graphRevision ?? workspace?.managerApplied.activationOrder?.graphRevision)}
        </dd></div>
        <div><dt>Draft against Applied</dt><dd>
          {applyReason ?? 'Changes waiting for Apply'}
        </dd></div>
        <div><dt>Runtime convergence</dt><dd>
          {diff === null ? 'unknown' : diff.converged ? 'converged'
            : `${diff.desiredUnits.length} desired · ${diff.observedUnits.length} observed units`}
        </dd></div>
      </dl>
      {workspace?.runAdmission.ready === false && workspace.runAdmission.reason && (
        <p className="inline-message severity-info" role="status">
          {workspace.runAdmission.reason}. Run stays disabled until Manager Applied matches the Draft.
        </p>
      )}
      <div className="dactions">
        {onOpenComponents !== undefined && (
          <button type="button" title="Each component's status, settings, frames and I/O."
            onClick={onOpenComponents}>{`Components (${desired})`}</button>
        )}
        <button type="button" disabled={applyReason !== null} title={applyReason ?? undefined}
          onClick={() => onNavigate('changes')}>Review changes</button>
      </div>
    </section>
  );
}

const DEACTIVATED_TITLE = 'Deactivated: it stays in the Deployment and is left out of the cell at the next Apply.';

const CHANGE_BADGES: Record<Exclude<ComponentManagerRow['change'], 'applied'>, string> = {
  added: 'Added',
  removed: 'Removed',
};

/**
 * The Draft's components and Applied-only ones as one tree:
 * a machine's members under it, a mounted tool under the component it is
 * mounted on. Each row (the house row of Scene objects and Catalog): the id
 * over its kind and model, its state as a badge at the right; depth indents
 * the whole row; a deactivated member greyed with an Off badge. A row
 * selects the component's page.
 */
export function ComponentList({ rows, states = {}, selectedId, onSelect }: {
  rows: readonly ComponentManagerRow[];
  /** The pushed state of each component in the cell (by id); others have none yet. */
  states?: Readonly<Record<string, ComponentState>>;
  selectedId: string | null;
  onSelect: (componentId: string) => void;
}) {
  const all = Object.values(states);
  return (
    <section className="dcard" aria-labelledby="component-manager-list-title">
      <header>
        <h4 id="component-manager-list-title">Components</h4>
        <span className="dcard-meta">{rows.length}</span>
      </header>
      {rows.length === 0 && (
        <p className="muted">The Deployment has no components yet. Add one below.</p>
      )}
      <div className="drows component-list">
        {rows.map((row) => {
          const state = row.active && row.change === 'applied' ? states[row.componentId] : undefined;
          const detail = state === undefined ? null : componentStateDetail(state, all);
          const badge = !row.active ? 'Off'
            : row.change !== 'applied' ? CHANGE_BADGES[row.change]
              : state === undefined ? 'Applied' : LIFECYCLE_LABELS[state.lifecycle];
          return (
            <button key={row.componentId} type="button"
              className={`dpick component-row${selectedId === row.componentId ? ' selected' : ''}${row.active ? '' : ' component-inactive'}`}
              style={{ '--component-depth': row.depth } as CSSProperties}
              aria-pressed={selectedId === row.componentId}
              title={!row.active ? DEACTIVATED_TITLE
                : state === undefined ? CHANGE_LABELS[row.change] : componentSummary(state, all)}
              onClick={() => onSelect(row.componentId)}>
              <strong>{row.componentId}</strong>
              <span>
                <small>{componentRowText(row, row.owner === null && row.depth > 0)}</small>
                {detail !== null && <small>{detail}</small>}
              </span>
              <span className={!row.active ? 'status-pill status-pill-off'
                : state === undefined ? TONE_BADGE_CLASSES.neutral
                  : TONE_BADGE_CLASSES[componentTone(state)]}>{badge}</span>
            </button>
          );
        })}
      </div>
    </section>
  );
}

interface PreviewState {
  draftId: string;
  draftRevision: number;
  preview: ApplyPreview;
}

/** Changes: the Preview's per-component impact, then one confirmed Apply. */
export function ComponentManagerChanges({ controller, rows }: {
  controller: DeploymentWorkspaceController;
  rows: readonly ComponentManagerRow[];
}) {
  const [preview, setPreview] = useState<PreviewState | null>(null);
  const [error, setError] = useState<string | null>(null);
  const workspace = controller.workspace;
  const draft = workspace?.draft ?? null;
  const current = preview !== null && draft !== null
    && preview.draftId === draft.id && preview.draftRevision === draft.revision;
  const applyReason = deploymentActionUnavailableReason(
    workspace, 'apply', controller.documentUnavailableReason,
  );
  const pending = rows.filter((row) => row.change !== 'applied');

  const runPreview = async () => {
    if (draft === null) return;
    const captured = { draftId: draft.id, draftRevision: draft.revision };
    setError(null);
    try {
      const next = await controller.previewApply();
      if (next !== null) setPreview({ ...captured, preview: next });
    } catch (reason: unknown) {
      setError(cellApplyErrorText(reason, 'Preview failed.'));
    }
  };

  return (
    <section className="dcard" aria-labelledby="component-manager-changes-title">
      <h4 id="component-manager-changes-title">Changes</h4>
      <dl>
        <div><dt>Components</dt><dd>
          {pending.length === 0 ? 'No component added or removed'
            : pending.map((row) => `${row.change === 'added' ? '+' : '−'} ${row.componentId}`).join(', ')}
        </dd></div>
        <div><dt>Draft against Saved</dt><dd>{draft?.differsFromSaved ? 'unsaved changes' : 'saved'}</dd></div>
        <div><dt>Saved against Applied</dt><dd>
          {workspace?.saved.differsFromApplied === null || workspace === null ? 'unknown'
            : workspace.saved.differsFromApplied ? 'different' : 'same'}
        </dd></div>
      </dl>
      <div className="dactions">
        <button type="button" disabled={draft?.document == null || controller.busy}
          title={busyReason(controller.busyOperation) ?? undefined}
          onClick={() => { void runPreview(); }}>{busyLabel(controller.busyOperation, 'preview', 'Preview changes')}</button>
        <button type="button" className="primary-action" disabled={applyReason !== null || controller.busy}
          title={applyReason ?? busyReason(controller.busyOperation)
            ?? 'Preview this Draft and confirm its impact; the affected components come back prepared.'}
          onClick={() => { void controller.applyDraft(); }}>
          {busyLabel(controller.busyOperation, ['apply_preview', 'apply'], 'Apply…')}
        </button>
      </div>
      {/* Yellow: a refused Preview or Apply changed nothing. */}
      {error !== null && <p className="inline-message severity-warning" role="alert">{error}</p>}
      {preview !== null && !current && (
        <p className="inline-message severity-warning" role="status">
          The Draft changed after this preview. Preview again before Apply.
        </p>
      )}
      {preview !== null && <ApplyImpact preview={preview.preview} />}
      <p className="field-help">
        Preview shows which components the Draft adds, removes or restarts; it changes nothing.
        Apply asks once to confirm that impact; the server disables, applies and prepares the
        affected components, and the others keep running. Enable them afterwards.
      </p>
    </section>
  );
}
