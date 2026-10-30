import { useState } from 'react';

import {
  DeploymentDraftChangedError,
  type DeploymentWorkspaceController,
} from '../hooks/useDeploymentWorkspace';
import { errorMessage } from '../state/structuralResource';
import { deploymentActionUnavailableReason } from '../state/documentStates';
import type {
  DeploymentCandidateOperationResponse,
  ManagerDiffProjection,
  ManagerGraphProjection,
} from '../types/api';
import {
  COMPONENT_KIND_LABELS,
  previewChanges,
  type ComponentManagerRow,
  type ComponentManagerTab,
  type RoleFeedback,
} from '../utils/componentManager';

/**
 * The Deployment tab pieces (Overview, Roles, Cell settings, Changes) and the
 * Components section's list: a thin client of the Deployment Draft routes,
 * Manager status/preview/Apply and CCLC. It owns no lifecycle or Applied state.
 */

const TAB_LABELS: Record<ComponentManagerTab, string> = {
  overview: 'Overview',
  roles: 'Roles',
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

export function ComponentManagerTabs({ tab, onTab, pendingChanges, roleIssues }: {
  tab: ComponentManagerTab;
  onTab: (tab: ComponentManagerTab) => void;
  pendingChanges: boolean;
  roleIssues: number;
}) {
  return (
    <div className="dseg" role="tablist" aria-label="Deployment">
      {(Object.keys(TAB_LABELS) as ComponentManagerTab[]).map((value) => (
        <button key={value} type="button" role="tab" aria-selected={tab === value}
          aria-controls={`component-manager-${value}`} onClick={() => onTab(value)}>
          {TAB_LABELS[value]}
          {value === 'roles' && roleIssues > 0 ? ` · ${roleIssues} role${roleIssues === 1 ? '' : 's'} open` : ''}
          {value === 'changes' && pendingChanges ? ' · pending' : ''}
        </button>
      ))}
    </div>
  );
}

/** Desired (Draft) against active (Manager Applied), with routes to the detail. */
export function ComponentManagerSummary({ controller, rows, feedback, graph, diff, onNavigate, onOpenComponents }: {
  controller: DeploymentWorkspaceController;
  rows: readonly ComponentManagerRow[];
  feedback: readonly RoleFeedback[];
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
  const roleIssues = feedback.filter((role) => role.status !== 'satisfied');
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
        <div><dt>Program roles</dt><dd>
          {feedback.length === 0 ? 'No Program roles'
            : roleIssues.length === 0 ? 'All bound' : `${roleIssues.length} of ${feedback.length} open`}
        </dd></div>
      </dl>
      {workspace?.runAdmission.ready === false && workspace.runAdmission.reason && (
        <p className="inline-message severity-info" role="status">
          {workspace.runAdmission.reason}. Run stays disabled until Manager Applied matches the Draft.
        </p>
      )}
      <div className="dactions">
        {roleIssues.length > 0 && (
          <button type="button" onClick={() => onNavigate('roles')}>Bind Program roles</button>
        )}
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

/** One list row's live health (CCLC): dot class, lifecycle state and verdict. */
export interface ComponentRowHealth {
  dotClass: string;
  state: string;
  message: string;
}

/**
 * The Draft's components and Applied-only ones, each with its kind, mount,
 * lifecycle state and health; a row selects the component's page.
 */
export function ComponentList({ rows, health = {}, selectedId, onSelect }: {
  rows: readonly ComponentManagerRow[];
  /** Health of the components in Manager Applied (by id); others have none yet. */
  health?: Readonly<Record<string, ComponentRowHealth>>;
  selectedId: string | null;
  onSelect: (componentId: string) => void;
}) {
  return (
    <section className="dcard" aria-labelledby="component-manager-list-title">
      <header>
        <h4 id="component-manager-list-title">Components</h4>
        <span className="dcard-meta">{rows.length}</span>
      </header>
      {rows.length === 0 && (
        <p className="muted">The Deployment has no components yet. Add one below.</p>
      )}
      <div className="drows">
        {rows.map((row) => {
          const rowHealth = health[row.componentId];
          return (
            <button key={row.componentId} type="button"
              className={`dpick${selectedId === row.componentId ? ' selected' : ''}`}
              aria-pressed={selectedId === row.componentId}
              title={rowHealth?.message ?? CHANGE_LABELS[row.change]}
              onClick={() => onSelect(row.componentId)}>
              <span>
                <strong>{row.mount === null ? row.componentId : `${row.componentId} → ${row.mount}`}</strong>
                <small>
                  {row.kind === null ? 'Component' : COMPONENT_KIND_LABELS[row.kind]}
                  {row.profileId !== null ? ` · ${row.profileId}` : ''}
                </small>
              </span>
              <small>
                {rowHealth !== undefined && (
                  <><span className={`component-dot ${rowHealth.dotClass}`} aria-hidden="true" />{' '}</>
                )}
                {row.change !== 'applied' ? CHANGE_LABELS[row.change]
                  : rowHealth !== undefined ? `${rowHealth.state} · ${rowHealth.message}` : 'Applied'}
              </small>
            </button>
          );
        })}
      </div>
    </section>
  );
}

/** Each Program component role: satisfied, unbound, incompatible or unknown against the Draft. */
export function RoleFeedbackCard({ feedback }: { feedback: readonly RoleFeedback[] }) {
  return (
    <section className="dcard" aria-labelledby="component-manager-roles-title">
      <header>
        <h4 id="component-manager-roles-title">Program roles</h4>
        <span className="dcard-meta">{feedback.length}</span>
      </header>
      {feedback.length === 0 ? (
        <p className="muted">The Program Draft declares no component roles.</p>
      ) : (
        <dl>
          {feedback.map((role) => (
            <div key={role.roleId}>
              <dt>{role.roleId}{role.requiredTags.length > 0 ? ` (${role.requiredTags.join(', ')})` : ''}</dt>
              <dd>
                {/* Advisory, it blocks nothing: yellow, never red (notice rule). */}
                <span className={`inline-message severity-${role.status === 'satisfied' ? 'info'
                  : 'warning'}`}>{role.status}</span>
                {' '}{role.detail}
              </dd>
            </div>
          ))}
        </dl>
      )}
      <p className="field-help">
        The Scenario's role assignment names the component of each role. Add a component with that id
        to bind the role (Components, Add component marks the components that fit an open role).
      </p>
    </section>
  );
}

interface PreviewState {
  draftId: string;
  draftRevision: number;
  response: DeploymentCandidateOperationResponse;
}

/** Changes: Manager's convergence diff of the Draft against Applied, then one confirmed Apply. */
export function ComponentManagerChanges({ controller, rows }: {
  controller: DeploymentWorkspaceController;
  rows: readonly ComponentManagerRow[];
}) {
  const [preview, setPreview] = useState<PreviewState | null>(null);
  const [previewing, setPreviewing] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const workspace = controller.workspace;
  const draft = workspace?.draft ?? null;
  const current = preview !== null && draft !== null
    && preview.draftId === draft.id && preview.draftRevision === draft.revision;
  const applyReason = deploymentActionUnavailableReason(
    workspace, 'apply', controller.documentUnavailableReason,
  ) ?? (!current ? 'Preview the current Draft first.' : null);
  const pending = rows.filter((row) => row.change !== 'applied');
  const candidate = preview?.response.result.candidatePreview ?? null;
  const changes = candidate === null ? null : previewChanges(candidate);

  const runPreview = async () => {
    if (draft === null) return;
    const captured = { draftId: draft.id, draftRevision: draft.revision };
    setPreviewing(true);
    setError(null);
    try {
      const response = await controller.previewDraft();
      if (response === null) setError(controller.mutationUnavailableReason ?? 'Preview is unavailable now.');
      else setPreview({ ...captured, response });
    } catch (reason: unknown) {
      setError(errorMessage(reason, 'Manager preview failed.'));
    } finally {
      setPreviewing(false);
    }
  };
  const apply = async () => {
    if (preview === null) return;
    setError(null);
    try {
      await controller.applyDraft({
        expectedDraft: { draftId: preview.draftId, draftRevision: preview.draftRevision },
      });
    } catch (reason: unknown) {
      if (!(reason instanceof DeploymentDraftChangedError)) throw reason;
      setError(`${reason.message} Preview again.`);
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
        <button type="button" disabled={draft?.document == null || previewing || controller.busy}
          onClick={() => { void runPreview(); }}>{previewing ? 'Previewing…' : 'Preview changes'}</button>
        <button type="button" className="primary-action" disabled={applyReason !== null || controller.busy}
          title={applyReason ?? 'Apply this exact Draft; Manager asks once to confirm component bring-up or removal.'}
          onClick={() => { void apply(); }}>{controller.applying ? 'Applying' : 'Apply…'}</button>
      </div>
      {/* Yellow: a refused Preview or Apply changed nothing (M55 notice rule). */}
      {error !== null && <p className="inline-message severity-warning" role="alert">{error}</p>}
      {preview !== null && !current && (
        <p className="inline-message severity-warning" role="status">
          The Draft changed after this preview. Preview again before Apply.
        </p>
      )}
      {candidate !== null && changes !== null && (
        <div className="drows" aria-label="Manager preview">
          <p>
            {candidate.converged
              ? 'Manager Applied already runs this Draft: Apply changes no component.'
              : candidate.impact.destructive
                ? 'Apply stops and changes running components (listed below).'
                : 'Apply brings up or reconfigures the components listed below.'}
          </p>
          {changes.components.length > 0 && (
            <dl>
              {changes.components.map((row) => (
                <div key={row.controllable_component_id}>
                  <dt>{row.controllable_component_id}</dt>
                  <dd>{row.disposition}{row.replacement_reasons.length > 0
                    ? ` · ${row.replacement_reasons.join(', ')}` : ''}</dd>
                </div>
              ))}
            </dl>
          )}
          {candidate.impact.detachedComponents.length > 0 && (
            <p>Detached at their authored pose: {candidate.impact.detachedComponents.join(', ')}.</p>
          )}
          {!candidate.impact.program.executable && (
            <p className="inline-message severity-warning">
              The Program will not be executable after this Apply:
              {' '}{candidate.impact.program.unresolvedReferences
                .map((reference) => `${reference.component_role_id} (${reference.reason})`).join(', ')}.
            </p>
          )}
          {changes.records.length > 0 && (
            <details>
              <summary>Compiled record changes ({changes.records.length} kinds)</summary>
              <dl>
                {changes.records.map((record) => (
                  <div key={record.recordKind}>
                    <dt>{record.recordKind}</dt>
                    <dd>
                      {record.added.length > 0 ? `+${record.added.length} ` : ''}
                      {record.changed.length > 0 ? `~${record.changed.length} ` : ''}
                      {record.removed.length > 0 ? `−${record.removed.length}` : ''}
                    </dd>
                  </div>
                ))}
              </dl>
            </details>
          )}
        </div>
      )}
      <p className="field-help">
        Preview compiles the Draft and asks Manager for the exact convergence diff; it changes
        nothing. Apply sends this same Draft once; when components are brought up, replaced or
        removed, Manager asks for one confirmation in the Apply dialog.
      </p>
    </section>
  );
}
