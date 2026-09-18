import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api } from '../api/client';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type {
  ManagerApplyRequest,
  ManagerApplyResponse,
  ManagerDeploymentCandidatePreviewResponse,
  ManagerDiffProjection,
  ManagerGraphProjection,
  ManagerStatusProjection,
  ScenarioCatalogEntry,
  ScenarioCatalogResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import { CCLCPanel } from './CCLCPanel';
import { useOperationalErrorMirror } from '../state/operationalErrors';
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
}

interface DeploymentReadModel {
  status: ManagerStatusProjection;
  graph: ManagerGraphProjection;
  diff: ManagerDiffProjection;
  scenarios: ScenarioCatalogResponse;
}

function errorMessage(error: unknown): string {
  return error instanceof Error && error.message.trim()
    ? error.message
    : 'Could not read the deployment composition boundary.';
}

function requestId(): string {
  if (typeof globalThis.crypto?.randomUUID === 'function') {
    return 'composer-manager-' + globalThis.crypto.randomUUID();
  }
  return 'composer-manager-' + Date.now().toString(36);
}

function shortIdentity(value: string | null | undefined): string {
  if (!value) return 'none';
  return value.length > 32 ? value.slice(0, 28) + '…' : value;
}

function sourceReady(readModel: DeploymentReadModel): boolean {
  const { status, graph, diff } = readModel;
  return [status.source, graph.source, diff.source].every(
    (source) => source.available
      && source.freshness === 'fresh'
      && source.knownGaps.length === 0,
  );
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

function currentIdentityConsistent(readModel: DeploymentReadModel): boolean {
  const { status, graph } = readModel;
  return status.status.active.graphRevision === graph.graphRevision
    && status.status.active.retainedBundleId === graph.retainedBundleId;
}

function sourceIdentityConsistent(readModel: DeploymentReadModel): boolean {
  const sources = [readModel.status.source, readModel.graph.source, readModel.diff.source];
  const ownerEpochs = new Set(sources.map((source) => source.sourceOwnerEpoch));
  const ownerIds = new Set(sources.map((source) => source.sourceOwnerId));
  return ownerEpochs.size === 1
    && ownerIds.size === 1
    && sources[0].sourceOwnerEpoch !== null
    && sources[0].sourceOwnerId !== null;
}

function previewValidation(
  readModel: DeploymentReadModel,
  scenario: ScenarioCatalogEntry | undefined,
): string | null {
  if (!scenario) return 'Choose one curated deployment preset first.';
  if (!sourceReady(readModel)) {
    return 'Manager source is unavailable, stale, or has known gaps.';
  }
  if (!sourceIdentityConsistent(readModel)) {
    return 'Manager projections do not share one source owner identity; refresh before preview.';
  }
  if (!currentIdentityConsistent(readModel)) {
    return 'Manager status and graph revisions disagree; refresh before preview.';
  }
  if (!scenario.deploymentArtifactId || !scenario.deploymentRevision) {
    return 'The selected preset has no complete deployment identity.';
  }
  return null;
}

function applyReceiptMessage(response: ManagerApplyResponse): string {
  const identity = 'Manager Apply ' + shortIdentity(response.applyId)
    + ' at generation ' + response.applyGeneration;
  return response.state === 'succeeded'
    ? identity + ' succeeded; the authoritative projections were refreshed.'
    : identity + ' was accepted. Manager owns its continuation and the Apply inventory below reflects its progress.';
}

export function DeploymentCompositionPanel({
  controller: deploymentController,
  enabled,
  snapshot,
  componentOverview,
  sourceModel,
  motionDescriptors,
  gripperDescriptors,
}: DeploymentCompositionPanelProps) {
  const refreshDeploymentWorkspace = deploymentController.refresh;
  const [readModel, setReadModel] = useState<DeploymentReadModel | null>(null);
  const [selectedScenarioId, setSelectedScenarioId] = useState('');
  const [candidatePreview, setCandidatePreview] = useState<
    ManagerDeploymentCandidatePreviewResponse | null
  >(null);
  const [loading, setLoading] = useState(enabled);
  const [busy, setBusy] = useState<'preview' | 'apply' | null>(null);
  const [error, setError] = useState<string | null>(null);
  useOperationalErrorMirror({
    source: 'composer.manager_composition',
    title: 'Manager preview or Apply failed',
    message: error,
    onDismiss: () => setError(null),
  });
  const [feedback, setFeedback] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);
  const mountedRef = useRef(true);
  const applyControllerRef = useRef<AbortController | null>(null);

  const refresh = useCallback(() => {
    setReloadToken((current) => current + 1);
    void refreshDeploymentWorkspace();
  }, [refreshDeploymentWorkspace]);

  useEffect(() => {
    mountedRef.current = true;
    return () => {
      mountedRef.current = false;
      applyControllerRef.current?.abort();
    };
  }, []);

  useEffect(() => {
    if (!enabled) {
      applyControllerRef.current?.abort();
    }
  }, [enabled]);

  useEffect(() => {
    if (!enabled) {
      setReadModel(null);
      setLoading(false);
      setError(null);
      setFeedback(null);
      setCandidatePreview(null);
      return undefined;
    }
    const controller = new AbortController();
    setLoading(true);
    setError(null);
    void Promise.all([
      api.manager.status(controller.signal),
      api.manager.graph(controller.signal),
      api.manager.diff(controller.signal),
      api.programs.scenarios(controller.signal),
    ])
      .then(([status, graph, diff, scenarios]) => {
        if (controller.signal.aborted) return;
        setReadModel({ status, graph, diff, scenarios });
        setLoading(false);
        setCandidatePreview(null);
      })
      .catch((reason: unknown) => {
        if (controller.signal.aborted) return;
        setLoading(false);
        setError(errorMessage(reason));
      });
    return () => controller.abort();
  }, [enabled, reloadToken]);

  const selectedScenario = useMemo(
    () => readModel?.scenarios.scenarios.find(
      (scenario) => scenario.scenarioId === selectedScenarioId,
    ),
    [readModel, selectedScenarioId],
  );
  const componentIds = useMemo(
    () => readModel === null ? [] : controllableComponentIds(readModel.graph),
    [readModel],
  );
  const validationError = readModel
    ? previewValidation(readModel, selectedScenario)
    : 'Manager projections are not loaded.';
  const canApply = readModel !== null
    && selectedScenario !== undefined
    && candidatePreview !== null
    && candidatePreview.candidateReceipt.deployment_artifact_id
      === selectedScenario.deploymentArtifactId
    && candidatePreview.candidateReceipt.deployment_revision
      === selectedScenario.deploymentRevision
    && candidatePreview.candidateReceipt.expected_current.graph_revision
      === readModel.graph.graphRevision
    && candidatePreview.candidateReceipt.expected_current.retained_bundle_id
      === readModel.graph.retainedBundleId
    && snapshot !== null
    && snapshot.authoringAdmission.authoringAllowed
    && candidatePreview.candidateReceipt.expected_admission_generation
      === snapshot.authoringAdmission.admissionGeneration
    && validationError === null
    && readModel.status.status.apply.mutationAvailable
    && readModel.status.source.sourceOwnerEpoch !== null
    && busy === null;

  const preview = async () => {
    if (!readModel || !selectedScenario || snapshot === null) return;
    setBusy('preview');
    setFeedback(null);
    const reason = previewValidation(readModel, selectedScenario);
    if (reason) {
      setCandidatePreview(null);
      setFeedback(reason);
      setBusy(null);
      return;
    }
    try {
      const result = await api.manager.previewDeploymentCandidate({
        deployment_artifact_id: selectedScenario.deploymentArtifactId,
        deployment_revision: selectedScenario.deploymentRevision,
        expected_admission_generation:
          snapshot.authoringAdmission.admissionGeneration,
        expected_current: {
          graph_revision: readModel.graph.graphRevision,
          retained_bundle_id: readModel.graph.retainedBundleId,
        },
        request_owner_epoch: readModel.status.source.sourceOwnerEpoch!,
      });
      if (!mountedRef.current) return;
      setCandidatePreview(result);
      const changes = Object.values(result.diff).reduce(
        (count, row) => count + row.added.length + row.changed.length
          + row.removed.length,
        0,
      );
      setFeedback(
        result.converged
          ? 'Manager validated this exact Deployment; it is already active.'
          : `Manager validated this exact Deployment with ${changes} graph change${changes === 1 ? '' : 's'}.`,
      );
    } catch (reason: unknown) {
      if (!mountedRef.current) return;
      setCandidatePreview(null);
      setError(errorMessage(reason));
    } finally {
      if (mountedRef.current) setBusy(null);
    }
  };

  const apply = async () => {
    if (
      !readModel
      || !selectedScenario
      || candidatePreview === null
      || !canApply
      || applyControllerRef.current !== null
    ) return;
    const ownerEpoch = readModel.status.source.sourceOwnerEpoch;
    if (ownerEpoch === null) return;
    const request: ManagerApplyRequest = {
      request_id: requestId(),
      request_owner_epoch: ownerEpoch,
      source_label: 'composer_ui',
      correlation_id: null,
      deployment_artifact_id: selectedScenario.deploymentArtifactId,
      deployment_revision: selectedScenario.deploymentRevision,
      expected_current: {
        graph_revision: readModel.graph.graphRevision,
        retained_bundle_id: readModel.graph.retainedBundleId,
      },
      reject_if_affected_busy: true,
      expected_admission_generation:
        candidatePreview.candidateReceipt.expected_admission_generation,
      candidate_receipt: candidatePreview.candidateReceipt,
    };
    const controller = new AbortController();
    applyControllerRef.current = controller;
    setBusy('apply');
    setError(null);
    setFeedback(null);
    try {
      const response = await api.manager.apply(request, controller.signal);
      if (!mountedRef.current || controller.signal.aborted) return;
      setFeedback(applyReceiptMessage(response));
      setCandidatePreview(null);
      refresh();
    } catch (reason: unknown) {
      if (!mountedRef.current || controller.signal.aborted) return;
      setError(errorMessage(reason));
      // A lost HTTP response is not evidence that Manager rejected the
      // command. Re-read only the authoritative projection; never resubmit or
      // infer completion in this browser.
      refresh();
    } finally {
      if (applyControllerRef.current === controller) {
        applyControllerRef.current = null;
      }
      if (mountedRef.current) setBusy(null);
    }
  };

  return (
    <section className="panel deployment-composition-panel" aria-labelledby="deployment-composition-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">Separate addressed artifact</p>
          <h2 id="deployment-composition-title">Deployment workspace</h2>
        </div>
        <button
          type="button"
          className="quiet-button"
          disabled={!enabled || loading || busy !== null}
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

      {deploymentController.mutationUnavailableReason && (
        <div className="authoring-unavailable-notice">
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
          <details className="deployment-advanced-document">
            <summary>Advanced compiled Deployment artifact</summary>
            <p className="field-help">
              Generated identities and compiler-expanded policy rows are read-only here.
            </p>
            <DeploymentDocumentEditor controller={deploymentController} readOnly />
          </details>
        </>
      )}

      <details className="deployment-manager-boundary">
        <summary>Manager Applied state and lifecycle</summary>

      {!enabled && (
        <div className="panel-state">
          Deployment composition requires the configured headless product.
        </div>
      )}
      {enabled && loading && (
        <div className="panel-state" role="status">
          Reading Manager status, graph, diff, and deployment presets…
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

          <div className="program-declaration-form compact">
            <label htmlFor="deployment-preset">Desired deployment preset</label>
            <select
              id="deployment-preset"
              value={selectedScenarioId}
              onChange={(event) => {
                setSelectedScenarioId(event.target.value);
                setCandidatePreview(null);
                setFeedback(null);
              }}
              disabled={busy !== null}
            >
              <option value="">Choose a preset…</option>
              {readModel.scenarios.scenarios.map((scenario) => (
                <option key={scenario.scenarioId} value={scenario.scenarioId}>
                  {scenario.scenarioId + ' · ' + shortIdentity(scenario.deploymentRevision)}
                </option>
              ))}
            </select>
          </div>

          {selectedScenario && (
            <div className="program-feedback" role="status">
              Desired artifact <code>{selectedScenario.deploymentArtifactId}</code>
              {' · '}revision <code>{selectedScenario.deploymentRevision}</code>
              {' · '}binding <code>{selectedScenario.programBindingId}</code>
            </div>
          )}

          <div className="button-row">
            <button
              type="button"
              disabled={
                busy !== null
                || readModel === null
                || selectedScenario === undefined
                || snapshot === null
                || readModel.status.source.sourceOwnerEpoch === null
              }
              title={snapshot === null
                ? 'Current backend admission is unavailable.'
                : selectedScenario === undefined
                  ? 'Choose a Deployment candidate to validate.'
                  : undefined}
              onClick={() => { void preview(); }}
            >
              {busy === 'preview' ? 'Validating…' : 'Preview / Validate'}
            </button>
            <button
              type="button"
              disabled={!canApply}
              onClick={() => { void apply(); }}
            >
              {busy === 'apply' ? 'Applying…' : 'Apply desired deployment'}
            </button>
          </div>

          {feedback && (
            <div className="program-feedback" role="status">
              {feedback}
            </div>
          )}
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
