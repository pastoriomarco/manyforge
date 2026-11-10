import { useEffect, useState } from 'react';

import { api, type AssistantProfiles } from '../api/client';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { jsonObject, type JsonObject } from '../utils/poseSourceAuthoring';
import { DraftTextInput } from './common/DraftInputs';
import type { FocusedEditorScope } from './DeploymentFocusedEditor';
import type { EffectiveValues } from '../types/components';
import { LifecycleBudgetField } from './LifecycleBudgetField';

/**
 * Deployment authoring of AI model components (`runtime_kind` `ai_model`): an
 * external OpenAI-compatible endpoint. The token is never stored; only the
 * NAME of the environment variable that holds it.
 */

interface AiModelComponent {
  index: number;
  componentId: string;
  runtimeConfiguration: JsonObject;
}

/** Token env var names; the launcher forwards exactly these into the Workbench. */
const ENV_NAME = /^MANYFORGE_AI_[A-Z0-9_]+$/;
const COMPONENT_ID = /^[a-z][a-z0-9._-]{0,127}$/;
const DEFAULT_BASE_URL = 'http://127.0.0.1:8000/v1';

function envNameProblem(value: string): string | null {
  const name = value.trim();
  return name === '' || (ENV_NAME.test(name) && name.length <= 128)
    ? null : 'The variable name must start with MANYFORGE_AI_ (A-Z, 0-9, _).';
}
const text = (value: JsonValue | undefined) => (typeof value === 'string' ? value : '');

export function aiModelComponents(document: JsonObject | null): AiModelComponent[] {
  const components = Array.isArray(document?.components) ? document.components : [];
  return components.flatMap((value, index) => {
    const component = jsonObject(value);
    const configuration = jsonObject(component?.configuration);
    const componentId = component?.component_id;
    return configuration?.runtime_kind === 'ai_model' && typeof componentId === 'string' ? [{
      index,
      componentId,
      runtimeConfiguration: jsonObject(configuration.runtime_configuration) ?? {},
    }] : [];
  });
}

function validBaseUrl(value: string): string | null {
  try {
    const url = new URL(value.trim());
    return url.protocol === 'http:' || url.protocol === 'https:' ? null : 'Enter an http(s) URL.';
  } catch {
    return 'Enter an absolute http(s) URL.';
  }
}

function AiModelComponentForm({ controller, effective, component }: {
  controller: DeploymentWorkspaceController;
  effective: EffectiveValues | null;
  component: AiModelComponent;
}) {
  const configuration = component.runtimeConfiguration;
  const prefix = component.componentId;
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const write = async (name: string, value: JsonValue) => {
    // A refusal shows once, in the yellow Deployment notice row.
    await controller.mutate(
      ['components', component.index, 'configuration', 'runtime_configuration'], 'set',
      { ...configuration, [name]: value });
  };
  const field = (label: string, name: string, validate: (value: string) => string | null,
    toValue: (value: string) => JsonValue) => (
    <label className="deployment-focused-field">
      <span>{label}</span>
      <DraftTextInput aria-label={`${prefix} ${label}`} value={text(configuration[name])}
        disabled={disabled} validate={validate}
        onCommit={(value) => { void write(name, toValue(value)); }} />
    </label>
  );
  return (
    <section className="deployment-pose-source" aria-label={`${prefix} AI model`}>
      <header><strong>{prefix}</strong></header>
      <LifecycleBudgetField controller={controller} effective={effective} componentIndex={component.index}
        componentId={component.componentId} disabled={disabled} />
      {field('Base URL', 'base_url', validBaseUrl, (value) => value.trim())}
      {field('Model', 'model', (value) => (value.trim() ? null : 'Enter the served model name.'),
        (value) => value.trim())}
      {field('Token environment variable', 'api_key_env', envNameProblem,
        (value) => (value.trim() === '' ? null : value.trim()))}
      <p className="field-help">Optional; must start with MANYFORGE_AI_. Export it with the token before starting the workbench.</p>
      <div className="button-row">
        <button type="button" disabled={disabled}
          onClick={() => { void controller.mutate(['components', component.index], 'remove'); }}>Remove AI model</button>
      </div>
    </section>
  );
}

function componentIds(document: JsonObject | null): string[] {
  const components = Array.isArray(document?.components) ? document.components : [];
  return components.flatMap((value) => {
    const id = jsonObject(value)?.component_id;
    return typeof id === 'string' ? [id] : [];
  });
}

/** The `components[]` row of a new AI model component (model-free, empty target settings). */
export function aiModelComponentValue(
  componentId: string, baseUrl: string, model: string, apiKeyEnv: string,
): Record<string, JsonValue> {
  return {
    component_id: componentId,
    component_profile_id: 'ai_model_v0',
    target_settings: {},
    configuration: {
      runtime_kind: 'ai_model',
      runtime_configuration: {
        base_url: baseUrl.trim(),
        model: model.trim(),
        api_key_env: apiKeyEnv.trim() === '' ? null : apiKeyEnv.trim(),
      },
    },
  };
}

export function AddAiModelForm({ controller, existing, onDone }: {
  controller: DeploymentWorkspaceController;
  existing: string[];
  onDone: () => void;
}) {
  const [componentId, setComponentId] = useState(
    existing.includes('assistant_model') ? `ai_model_${existing.length + 1}` : 'assistant_model');
  const [baseUrl, setBaseUrl] = useState(DEFAULT_BASE_URL);
  const [model, setModel] = useState('');
  const [apiKeyEnv, setApiKeyEnv] = useState('');
  const [error, setError] = useState<string | null>(null);
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const create = async () => {
    const problem = !COMPONENT_ID.test(componentId)
      ? "The component id must be 1–128 lowercase letters, digits, '.', '_' or '-', starting with a letter."
      : existing.includes(componentId) ? `Component ${componentId} already exists.`
        : validBaseUrl(baseUrl) ?? (model.trim() ? null : 'Enter the served model name.')
          ?? envNameProblem(apiKeyEnv);
    if (problem !== null) {
      setError(problem);
      return;
    }
    setError(null);
    // A refusal shows once, in the yellow Deployment notice row.
    const result = await controller.mutate(['components'], 'append',
      aiModelComponentValue(componentId, baseUrl, model, apiKeyEnv));
    if (result !== null) onDone();
  };
  const input = (label: string, value: string, set: (value: string) => void) => (
    <label className="deployment-focused-field">
      <span>{label}</span>
      <input aria-label={`New AI model ${label}`} value={value} disabled={disabled}
        onChange={(event) => { set(event.target.value); setError(null); }} />
    </label>
  );
  return (
    <section className="deployment-pose-source" aria-label="New AI model">
      {input('Component id', componentId, setComponentId)}
      {input('Base URL', baseUrl, setBaseUrl)}
      {input('Model', model, setModel)}
      {input('Token environment variable', apiKeyEnv, setApiKeyEnv)}
      {error && <p className="deployment-field-error inline-message severity-error" role="alert">{error}</p>}
      <div className="button-row">
        <button type="button" disabled={disabled} onClick={() => { void create(); }}>Create AI model</button>
        <button type="button" className="quiet-button" onClick={onDone}>Cancel</button>
      </div>
    </section>
  );
}

/**
 * Which AI model component serves each model alias of the Deployment's
 * assistant profile (intent member `assistant_model_components`). Automatic
 * (no entry) is valid only when the Deployment has exactly one AI model.
 */
function AssistantModelAliases({ controller, profileId, components }: {
  controller: DeploymentWorkspaceController;
  profileId: string;
  components: AiModelComponent[];
}) {
  const [profiles, setProfiles] = useState<AssistantProfiles | null>(null);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    const controllerSignal = new AbortController();
    api.deploymentWorkspace.assistantProfiles(controllerSignal.signal)
      .then(setProfiles)
      .catch((reason: unknown) => {
        if (!controllerSignal.signal.aborted) {
          setError(reason instanceof Error ? reason.message : 'Could not read the assistant profiles.');
        }
      });
    return () => controllerSignal.abort();
  }, []);
  const document = controller.workspace?.draft.document ?? null;
  const mapping = jsonObject(document?.assistant_model_components) ?? {};
  const aliases = profiles?.[profileId]?.modelEndpointAliases ?? [];
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const choose = async (alias: string, componentId: string) => {
    const next: JsonObject = { ...mapping };
    if (componentId === '') delete next[alias];
    else next[alias] = componentId;
    // A refusal shows once, in the yellow Deployment notice row.
    if (Object.keys(next).length > 0) await controller.mutate(['assistant_model_components'], 'set', next);
    else if ('assistant_model_components' in (document ?? {})) {
      await controller.mutate(['assistant_model_components'], 'remove');
    }
  };
  return (
    <div className="deployment-pose-source" aria-label="Assistant model aliases">
      <header><strong>Assistant profile {profileId}</strong></header>
      {profiles !== null && !(profileId in profiles) && (
        <p className="muted">Unknown assistant profile.</p>
      )}
      {aliases.map((alias) => {
        const selected = text(mapping[alias]);
        const unresolved = selected === '' && components.length !== 1;
        return (
          <label key={alias} className="deployment-focused-field">
            <span>{alias}</span>
            <select aria-label={`Model alias ${alias}`} value={selected} disabled={disabled}
              onChange={(event) => { void choose(alias, event.target.value); }}>
              <option value="">Automatic (the only AI model)</option>
              {components.map((component) => (
                <option key={component.componentId} value={component.componentId}>{component.componentId}</option>
              ))}
            </select>
            {unresolved && (
              <small className="inline-message severity-error">Choose the AI model for this alias.</small>
            )}
          </label>
        );
      })}
      {/* Yellow: a failed read blocks nothing (notice rule). */}
      {error && <p className="deployment-field-error inline-message severity-warning" role="alert">{error}</p>}
    </div>
  );
}

/**
 * The Deployment's AI model components (OpenAI-compatible endpoints). One
 * component's page shows only its form; the cell settings show only the
 * assistant's model aliases, which choose among the components.
 */
export function AiModelComponentSection({ controller, effective = null, scope = 'all' }: {
  controller: DeploymentWorkspaceController;
  /** `GET /api/deployment/effective`, read once by the editor. */
  effective?: EffectiveValues | null;
  scope?: FocusedEditorScope;
}) {
  const document = controller.workspace?.draft.document ?? null;
  const allComponents = aiModelComponents(document);
  const components = typeof scope === 'object'
    ? allComponents.filter((component) => component.componentId === scope.componentId) : allComponents;
  const [adding, setAdding] = useState(false);
  const profileId = typeof document?.assistant_profile_id === 'string' ? document.assistant_profile_id : null;
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  if (typeof scope === 'object' && components.length === 0) return null;
  if (scope === 'cell') {
    return profileId === null || allComponents.length === 0 ? null : (
      <details className="dcard">
        <summary>AI model aliases</summary>
        <div className="deployment-focused-body">
          <AssistantModelAliases controller={controller} profileId={profileId} components={allComponents} />
        </div>
      </details>
    );
  }
  return (
    <details className="dcard" open={components.length > 0 || adding}>
      <summary>AI models</summary>
      <div className="deployment-focused-body">
        {components.length === 0 && !adding && <p className="muted">No AI model is declared.</p>}
        {components.map((component) => (
          <AiModelComponentForm key={component.componentId} controller={controller} effective={effective}
            component={component} />
        ))}
        {scope === 'all' && (adding ? (
          <AddAiModelForm controller={controller} existing={componentIds(document)}
            onDone={() => setAdding(false)} />
        ) : (
          <div className="button-row">
            <button type="button" disabled={disabled} onClick={() => setAdding(true)}>Add AI model</button>
          </div>
        ))}
        {scope === 'all' && profileId !== null && (
          <AssistantModelAliases controller={controller} profileId={profileId} components={components} />
        )}
        <p className="field-help">An AI model is a model-free component that names an OpenAI-compatible server you run: its base URL (for example <code>http://127.0.0.1:8000/v1</code>), the served model name, and optionally the name of the environment variable that holds its token. ManyForge never starts the server and never refuses Prepare or Enable because of it; its reachability is shown as the <code>endpoint</code> stream warning.</p>
      </div>
    </details>
  );
}
