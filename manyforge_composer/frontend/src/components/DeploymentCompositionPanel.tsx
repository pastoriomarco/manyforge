import { useCallback, useEffect, useMemo, useState } from 'react';

import { api } from '../api/client';
import type {
  ManagerApplyResponse,
  ManagerDiffProjection,
  ManagerGraphProjection,
  ManagerStatusProjection,
  ScenarioCatalogEntry,
  ScenarioCatalogResponse,
} from '../types/api';

interface DeploymentCompositionPanelProps {
  enabled: boolean;
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

function applyMessage(response: ManagerApplyResponse): string {
  if (response.ok) {
    return 'Manager accepted Apply ' + shortIdentity(response.requestId) + '.';
  }
  return response.error
    ? response.error.code + ': ' + response.error.message
    : 'Manager rejected Apply without a structured error.';
}

export function DeploymentCompositionPanel({
  enabled,
}: DeploymentCompositionPanelProps) {
  const [readModel, setReadModel] = useState<DeploymentReadModel | null>(null);
  const [selectedScenarioId, setSelectedScenarioId] = useState('');
  const [previewedScenarioId, setPreviewedScenarioId] = useState<string | null>(null);
  const [loading, setLoading] = useState(enabled);
  const [busy, setBusy] = useState<'preview' | 'apply' | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [feedback, setFeedback] = useState<string | null>(null);
  const [reloadToken, setReloadToken] = useState(0);

  const refresh = useCallback(() => {
    setReloadToken((current) => current + 1);
  }, []);

  useEffect(() => {
    if (!enabled) {
      setReadModel(null);
      setLoading(false);
      setError(null);
      setFeedback(null);
      setPreviewedScenarioId(null);
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
        setPreviewedScenarioId(null);
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
  const validationError = readModel
    ? previewValidation(readModel, selectedScenario)
    : 'Manager projections are not loaded.';
  const canApply = readModel !== null
    && selectedScenario !== undefined
    && previewedScenarioId === selectedScenarioId
    && validationError === null
    && readModel.status.status.apply.mutationAvailable
    && readModel.status.source.sourceOwnerEpoch !== null
    && busy === null;

  const preview = () => {
    if (!readModel) return;
    setBusy('preview');
    setFeedback(null);
    const reason = previewValidation(readModel, selectedScenario);
    if (reason) {
      setPreviewedScenarioId(null);
      setFeedback(reason);
    } else {
      setPreviewedScenarioId(selectedScenarioId);
      setFeedback(
        selectedScenarioId === ''
          ? 'No deployment preset selected.'
          : 'Preview is valid. Apply remains a separate deliberate action.',
      );
    }
    setBusy(null);
  };

  const apply = async () => {
    if (!readModel || !selectedScenario || !canApply) return;
    const ownerEpoch = readModel.status.source.sourceOwnerEpoch;
    if (ownerEpoch === null) return;
    setBusy('apply');
    setError(null);
    setFeedback(null);
    try {
      const response = await api.manager.apply({
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
      });
      setFeedback(applyMessage(response));
      setPreviewedScenarioId(null);
      refresh();
    } catch (reason: unknown) {
      setError(errorMessage(reason));
    } finally {
      setBusy(null);
    }
  };

  return (
    <section className="panel deployment-composition-panel" aria-labelledby="deployment-composition-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">Manager-owned deployment</p>
          <h2 id="deployment-composition-title">Deployment composition</h2>
        </div>
        <button
          type="button"
          className="quiet-button"
          disabled={!enabled || loading || busy !== null}
          onClick={refresh}
        >
          Refresh projections
        </button>
      </header>

      <p className="panel-intro">
        Select a curated preset, inspect the exact active revisions, preview the
        pure diff, and Apply deliberately. This panel never selects, Applies,
        or Runs a deployment implicitly.
      </p>

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
                setPreviewedScenarioId(null);
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
              disabled={busy !== null}
              onClick={preview}
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
          <p className="panel-intro">
            Program Run and manual control remain explicit owner-issued actions
            in the execution panels below.
          </p>
        </>
      )}
    </section>
  );
}
