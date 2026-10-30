import { useCallback, useEffect, useMemo, useState } from 'react';

import { api } from '../api/client';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { ComponentCatalogState } from '../hooks/useComponentCatalog';
import {
  componentManagerRows,
  roleFeedback,
  type ComponentManagerTab,
} from '../utils/componentManager';
import {
  ComponentManagerChanges,
  ComponentManagerSummary,
  ComponentManagerTabs,
  RoleFeedbackCard,
} from './ComponentManager';
import { useManagerApplyStatus } from '../hooks/useManagerApplyStatus';
import type { ControlTransportStatus } from '../hooks/useHeadlessState';
import type {
  BlackboardKeyDeclaration,
  JsonValue,
  ManagerDiffProjection,
  ManagerGraphProjection,
  ManagerStatusProjection,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
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
import type { TargetStopChain } from '../utils/interruptAuthoring';
import type {
  ManualControlGripperDescriptor,
  ManualControlMotionDescriptor,
} from '../types/robotControl';
import type { CarriedObjectLostSignalChoice } from '../utils/systemSignalAuthoring';

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
  /** The workbench's one component catalog (shared with Components). */
  componentCatalog: ComponentCatalogState;
  /** Opens a tab from elsewhere (a Components page link); a new sequence each time. */
  tabRequest?: { tab: ComponentManagerTab; sequence: number } | null;
  /** Opens the Components section. */
  onOpenComponents?: () => void;
  enabled: boolean;
  snapshot: ComposerHeadlessState | null;
  transportStatus: ControlTransportStatus;
  componentOverview: ComponentOverviewInput;
  sourceModel: StructuralResourceState<SourceModelProjection>;
  motionDescriptors: ManualControlMotionDescriptor[];
  /** Published motion targets' qualified Stop chains (SX-2167). */
  stopChains?: TargetStopChain[];
  gripperDescriptors: ManualControlGripperDescriptor[];
  /** Keys a global interrupt rule may read (the Program Draft's keys). */
  programKeys?: readonly BlackboardKeyDeclaration[] | null;
  /** The Program Draft's scene object ids (orphan pose links); null without a Program. */
  programSceneIds?: readonly string[] | null;
  /** The Applied Program's execution group ids; null when no Program is applied. */
  executionGroupIds?: readonly string[] | null;
  systemSignalChoices?: readonly CarriedObjectLostSignalChoice[];
  systemSignalsNeedApply?: boolean;
  /** The workbench's single observation of the advertised maintenance
   * operation; this panel observes only an inventory-selected operation. */
  maintenanceStatus: ReturnType<typeof useManagerApplyStatus>;
  /** The Program Draft's component roles; null without a Program. */
  programComponentRoles?: Record<string, JsonValue> | null;
  /** The Scenario's Program role assignment; null when none is loaded. */
  programRoleAssignment?: Record<string, JsonValue> | null;
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

export function DeploymentCompositionPanel({
  controller: deploymentController,
  componentCatalog,
  tabRequest = null,
  onOpenComponents,
  enabled,
  snapshot,
  transportStatus,
  componentOverview,
  sourceModel,
  motionDescriptors,
  stopChains,
  gripperDescriptors,
  programKeys,
  programSceneIds = null,
  executionGroupIds = null,
  systemSignalChoices = [],
  systemSignalsNeedApply = false,
  maintenanceStatus: advertisedStatus,
  programComponentRoles = null,
  programRoleAssignment = null,
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
    snapshot?.runtime.sections.manager ?? null,
    transportStatus,
  );
  const maintenanceStatus = maintenance === null ? inventoryStatus : advertisedStatus;
  const [reloadToken, setReloadToken] = useState(0);

  const reloadCatalog = componentCatalog.reload;
  const refresh = useCallback(() => {
    setReloadToken((current) => current + 1);
    void refreshDeploymentWorkspace();
    reloadCatalog();
  }, [refreshDeploymentWorkspace, reloadCatalog]);

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

  const [tab, setTab] = useState<ComponentManagerTab>(tabRequest?.tab ?? 'overview');
  // The persistent status button opens Overview; other state stays.
  useEffect(() => {
    if (componentOverview.request > 0) setTab('overview');
  }, [componentOverview.request]);
  useEffect(() => {
    if (tabRequest !== null) setTab(tabRequest.tab);
  }, [tabRequest]);
  const catalog = componentCatalog.catalog;
  const draftDocument = deploymentController.workspace?.draft.document ?? null;
  const appliedIds = deploymentController.workspace?.managerApplied.activationOrder?.componentIds ?? null;
  const rows = useMemo(
    () => componentManagerRows(draftDocument, appliedIds, catalog?.componentProfiles),
    [appliedIds, catalog, draftDocument],
  );
  const feedback = useMemo(() => roleFeedback(
    programComponentRoles, programRoleAssignment, rows, catalog?.componentProfiles ?? {},
  ), [catalog, programComponentRoles, programRoleAssignment, rows]);
  const pendingChanges = rows.some((row) => row.change !== 'applied')
    || deploymentController.workspace?.runAdmission.ready === false;
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
  const managerApplied = (
    <details className="dcard">
      <summary>Manager Applied state</summary>
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
      {/* The read failure itself is the yellow notice mirrored above; here only a neutral pointer. */}
      {enabled && error && (
        <div className="panel-state" role="status">
          Manager status could not be read; see the notice.
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
            Deployment changes use the one Apply workflow (the header Apply, or Apply under
            Changes). This section observes the resulting Manager-owned operation and does not
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
        </>
      )}
    </details>
  );

  return (
    <section className="panel deployment-composition-panel dpanel" aria-labelledby="deployment-composition-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">Deployment</p>
          <h2 id="deployment-composition-title">Cell composition</h2>
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
        Compose the cell in the Deployment Draft, independently from Program and Manager
        Applied state. Nothing runs until the one confirmed Apply. Each component's status
        and settings are under Components.
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

      <ComponentManagerTabs
        tab={tab}
        onTab={setTab}
        pendingChanges={pendingChanges}
        roleIssues={feedback.filter((role) => role.status !== 'satisfied').length}
      />

      {tab === 'overview' && (
        <div id="component-manager-overview" role="tabpanel" className="dpanel">
          <ComponentOverview {...componentOverview} />
          <ComponentManagerSummary
            controller={deploymentController}
            rows={rows}
            feedback={feedback}
            graph={readModel?.graph ?? null}
            diff={readModel?.diff ?? null}
            onNavigate={setTab}
            onOpenComponents={onOpenComponents}
          />
          {managerApplied}
        </div>
      )}

      {tab === 'roles' && (
        <div id="component-manager-roles" role="tabpanel" className="dpanel">
          <RoleFeedbackCard feedback={feedback} />
          {deploymentController.workspace !== null && (
            <DeploymentBindingProjection workspace={deploymentController.workspace} />
          )}
        </div>
      )}

      {tab === 'cell' && (
        <div id="component-manager-cell" role="tabpanel" className="dpanel">
          {deploymentController.loading ? (
            <div className="panel-state" role="status">Reading Deployment Draft…</div>
          ) : (
            <>
              <DeploymentFocusedEditor
                controller={deploymentController}
                scope="cell"
                sourceModel={sourceModel}
                motionDescriptors={motionDescriptors}
                stopChains={stopChains}
                gripperDescriptors={gripperDescriptors}
                programKeys={programKeys}
                executionGroupIds={executionGroupIds}
                programSceneIds={programSceneIds}
                systemSignalChoices={systemSignalChoices}
                systemSignalsNeedApply={systemSignalsNeedApply}
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
        </div>
      )}

      {tab === 'changes' && (
        <div id="component-manager-changes" role="tabpanel" className="dpanel">
          <ComponentManagerChanges controller={deploymentController} rows={rows} />
        </div>
      )}
    </section>
  );
}
