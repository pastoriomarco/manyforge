import { useCallback, useEffect, useMemo, useState } from 'react';

import { api } from '../api/client';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { useManagerApplyStatus } from '../hooks/useManagerApplyStatus';
import type {
  ManagerDiffProjection,
  ManagerGraphProjection,
  ManagerStatusProjection,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import { CCLCPanel } from './CCLCPanel';
import { useOperationalErrorMirror } from '../state/operationalErrors';
import {
  selectManagerApplyInventoryIdentity,
} from '../state/managerApply';
import { DeploymentBindingProjection } from './DeploymentBindingProjection';
import { DeploymentDocumentEditor } from './DeploymentDocumentEditor';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';
import { ComponentOverview } from './ComponentOverview';
import type { RuntimeStatusView } from './runtimeStatus';
import type { StructuralResourceState } from '../state/structuralResource';
import type { SourceModelProjection } from '../types/api';
import type {
  ManualControlGripperDescriptor,
  ManualControlMotionDescriptor,
} from '../types/robotControl';

export interface ComponentOverviewInput {
  request: number;
  status: RuntimeStatusView | null;
  backendConnected: boolean;
  connectionLabel: string;
  connectionDetail: string;
  contextFacts: string[];
  unacknowledgedAlarms: number;
  activeAcknowledgedAlarms: number;
  clientErrorCount: number;
  error: string | null;
  onReconnect: () => void;
  onOpenRuntimeHistory: () => void;
}

interface DeploymentCompositionPanelProps {
  controller: DeploymentWorkspaceController;
  enabled: boolean;
  snapshot: ComposerHeadlessState | null;
  componentOverview: ComponentOverviewInput;
  sourceModel: StructuralResourceState<SourceModelProjection>;
  motionDescriptors: ManualControlMotionDescriptor[];
  gripperDescriptors: ManualControlGripperDescriptor[];
  /** The workbench's single observation of the advertised maintenance
   * operation; this panel observes only an inventory-selected operation. */
  maintenanceStatus: ReturnType<typeof useManagerApplyStatus>;
}

interface DeploymentReadModel {
  status: ManagerStatusProjection;
  graph: ManagerGraphProjection;
  diff: ManagerDiffProjection;
}

function errorMessage(error: unknown): string {
  return error instanceof Error && error.message.trim()
    ? error.message
    : 'Could not read the deployment composition boundary.';
}

function shortIdentity(value: string | null | undefined): string {
  if (!value) return 'none';
  return value.length > 32 ? value.slice(0, 28) + '…' : value;
}

export function controllableComponentIds(graph: ManagerGraphProjection): string[] {
  const value = graph.cellGraph.controllable_components;
  if (!Array.isArray(value)) return [];
  const identities = value.flatMap((candidate) => {
    if (candidate === null || typeof candidate !== 'object' || Array.isArray(candidate)) {
      return [];
    }
    const identity = candidate.controllable_component_id;
    return typeof identity === 'string' && identity.trim() ? [identity] : [];
  });
  return [...new Set(identities)].sort();
}

export function DeploymentCompositionPanel({
  controller: deploymentController,
  enabled,
  snapshot,
  componentOverview,
  sourceModel,
  motionDescriptors,
  gripperDescriptors,
  maintenanceStatus: advertisedStatus,
}: DeploymentCompositionPanelProps) {
  const refreshDeploymentWorkspace = deploymentController.refresh;
  const [readModel, setReadModel] = useState<DeploymentReadModel | null>(null);
  const [loading, setLoading] = useState(enabled);
  const [error, setError] = useState<string | null>(null);
  useOperationalErrorMirror({
    source: 'composer.manager_composition',
    title: 'Manager status could not be read',
    message: error,
    onDismiss: () => setError(null),
  });
  const maintenance = snapshot?.componentOverview.maintenanceOperation ?? null;
  const inventoryApply = readModel === null
    ? null
    : selectManagerApplyInventoryIdentity(readModel.status.status.apply.records);
  const inventoryStatus = useManagerApplyStatus(
    maintenance === null ? inventoryApply : null,
    snapshot?.snapshotSequence ?? null,
  );
  const maintenanceStatus = maintenance === null ? inventoryStatus : advertisedStatus;
  const [reloadToken, setReloadToken] = useState(0);

  const refresh = useCallback(() => {
    setReloadToken((current) => current + 1);
    void refreshDeploymentWorkspace();
  }, [refreshDeploymentWorkspace]);

  useEffect(() => {
    if (!enabled) {
      setReadModel(null);
      setLoading(false);
      setError(null);
      return undefined;
    }
    const controller = new AbortController();
    setLoading(true);
    setError(null);
    void Promise.all([
      api.manager.status(controller.signal),
      api.manager.graph(controller.signal),
      api.manager.diff(controller.signal),
    ])
      .then(([status, graph, diff]) => {
        if (controller.signal.aborted) return;
        setReadModel({ status, graph, diff });
        setLoading(false);
      })
      .catch((reason: unknown) => {
        if (controller.signal.aborted) return;
        setLoading(false);
        setError(errorMessage(reason));
      });
    return () => controller.abort();
  }, [enabled, reloadToken]);

  const componentIds = useMemo(
    () => readModel === null ? [] : controllableComponentIds(readModel.graph),
    [readModel],
  );
  const maintenanceMessage = maintenanceStatus.reason ?? (
    maintenanceStatus.status?.state === 'failed'
      || maintenanceStatus.status?.state === 'unknown_effect'
      ? `Deployment maintenance stopped: ${
        maintenanceStatus.status.failure?.message
          ?? 'Manager reported a terminal failure.'
      }`
      : `Deployment transition is ${
        maintenanceStatus.status?.phase
          ?? maintenance?.phase.replaceAll('_', ' ')
          ?? 'in progress'
      }. Controls remain disabled.`
  );
  return (
    <section className="panel deployment-composition-panel dpanel" aria-labelledby="deployment-composition-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">Separate addressed artifact</p>
          <h2 id="deployment-composition-title">Deployment workspace</h2>
        </div>
        <button
          type="button"
          className="quiet-button"
          disabled={!enabled || loading}
          onClick={refresh}
        >
          Refresh current state
        </button>
      </header>

      <p className="panel-intro">
        Edit and save a compiler-validated Deployment Draft independently from
        Program and Manager Applied state. The header Apply command validates
        and activates this Draft.
      </p>

      {(maintenance !== null
        || maintenanceStatus.reason !== null
        || maintenanceStatus.status?.state === 'pending'
        || maintenanceStatus.status?.state === 'failed'
        || maintenanceStatus.status?.state === 'unknown_effect') && (
        <div className="deployment-run-gate" role="status">
          {maintenanceMessage}
        </div>
      )}

      {deploymentController.mutationUnavailableReason && (
        <div className="notice severity-info notice-strip">
          {deploymentController.mutationUnavailableReason}
        </div>
      )}
      {!deploymentController.workspace?.runAdmission.ready
        && deploymentController.workspace?.runAdmission.reason && (
        <div className="deployment-run-gate" role="status">
          {deploymentController.workspace.runAdmission.reason}. Run remains
          disabled until Manager Applied matches this Draft.
        </div>
      )}
      {deploymentController.feedback && (
        <div className="program-feedback" role="status">
          {deploymentController.feedback}
          <button
            type="button"
            className="quiet-button"
            onClick={deploymentController.clearFeedback}
          >Dismiss</button>
        </div>
      )}

      <ComponentOverview {...componentOverview} />

      {deploymentController.workspace !== null && (
        <DeploymentBindingProjection workspace={deploymentController.workspace} />
      )}
      {deploymentController.loading ? (
        <div className="panel-state" role="status">Reading Deployment Draft…</div>
      ) : (
        <>
          <DeploymentFocusedEditor
            controller={deploymentController}
            sourceModel={sourceModel}
            motionDescriptors={motionDescriptors}
            gripperDescriptors={gripperDescriptors}
          />
          <details className="dcard">
            <summary>Advanced compiled Deployment artifact</summary>
            <p className="field-help">
              Generated identities and compiler-expanded policy rows are read-only here.
            </p>
            <DeploymentDocumentEditor controller={deploymentController} readOnly />
          </details>
        </>
      )}

      <details className="dcard">
        <summary>Manager Applied state and lifecycle</summary>

      {!enabled && (
        <div className="panel-state">
          Deployment composition requires the configured headless product.
        </div>
      )}
      {enabled && loading && (
        <div className="panel-state" role="status">
          Reading Manager status, graph, and diff…
        </div>
      )}
      {enabled && error && (
        <div className="panel-state error-state" role="alert">
          {error}
        </div>
      )}

      {enabled && readModel && !loading && !error && (
        <>
          <div className="program-runtime-grid">
            <div className="program-runtime-fact">
              <span>Active graph revision</span>
              <strong>{shortIdentity(readModel.graph.graphRevision)}</strong>
              <small>Manager-owned active graph identity</small>
            </div>
            <div className="program-runtime-fact">
              <span>Active retained bundle</span>
              <strong>{shortIdentity(readModel.graph.retainedBundleId)}</strong>
              <small>Expected-current CAS fence for Apply</small>
            </div>
            <div className="program-runtime-fact">
              <span>Runtime convergence</span>
              <strong>{readModel.diff.converged ? 'converged' : 'different'}</strong>
              <small>
                {readModel.diff.desiredUnits.length} desired · {readModel.diff.observedUnits.length} observed units
              </small>
            </div>
            <div className="program-runtime-fact">
              <span>Manager source</span>
              <strong>{readModel.status.status.managerReady ? 'ready' : 'unavailable'}</strong>
              <small>
                {readModel.status.source.sourceOwnerId ?? 'unknown owner'} · {readModel.status.source.freshness}
              </small>
            </div>
          </div>

          <p className="panel-intro">
            Deployment changes use the header Apply/Revert workflow. This
            section observes the resulting Manager-owned operation and does not
            submit a second catalog-based transition.
          </p>
          {!readModel.status.status.apply.mutationAvailable && (
            <p className="panel-intro">
              Manager Apply is unavailable: {readModel.status.status.apply.unavailableReasonCode ?? 'unknown reason'}.
            </p>
          )}
          <div className="program-declaration-form compact">
            <h3>Manager Apply inventory</h3>
            {readModel.status.status.apply.records.length === 0 ? (
              <p className="panel-intro">No Manager Apply has been recorded.</p>
            ) : (
              <ol className="program-history-list">
                {[...readModel.status.status.apply.records]
                  .sort((left, right) => (
                    right.applyGeneration - left.applyGeneration
                    || right.applyId.localeCompare(left.applyId)
                  ))
                  .map((record) => (
                    <li key={record.applyId + ':' + record.applyGeneration}>
                      <strong>{shortIdentity(record.applyId)}</strong>
                      {' · generation ' + record.applyGeneration}
                      {' · ' + (record.terminalState ?? record.phase)}
                      <small>
                        {' request ' + shortIdentity(record.request.requestId)}
                        {' · target ' + shortIdentity(record.candidateRetainedBundleId)}
                      </small>
                    </li>
                  ))}
              </ol>
            )}
          </div>
          <p className="panel-intro">
            Program Run and manual control remain explicit owner-issued actions
            in the persistent viewer controls.
          </p>
          <section className="deployment-component-section" aria-labelledby="deployment-components-title">
            <header className="scene-authoring-heading">
              <div>
                <p className="eyebrow">Active Manager graph</p>
                <h3 id="deployment-components-title">Components and lifecycle</h3>
              </div>
              <span>{componentIds.length} controllable</span>
            </header>
            {componentIds.length === 0 ? (
              <p className="panel-intro">
                The current graph publishes no controllable component identities.
              </p>
            ) : (
              <div className="deployment-component-chips" aria-label="Current graph components">
                {componentIds.map((identity) => <code key={identity}>{identity}</code>)}
              </div>
            )}
            <CCLCPanel
              enabled={enabled}
              componentIds={componentIds}
              admissionGeneration={snapshot?.authoringAdmission.admissionGeneration ?? null}
              mutationAvailable={snapshot?.authoringAdmission.authoringAllowed === true}
              mutationUnavailableReason={snapshot === null
                ? 'Current backend admission is unavailable.'
                : snapshot.programCycle.state === 'terminal_awaiting_reset'
                  ? 'Reset the completed Program cycle before changing lifecycle or routes.'
                  : 'Lifecycle and route changes require current Idle admission.'}
            />
          </section>
        </>
      )}
      </details>
    </section>
  );
}
