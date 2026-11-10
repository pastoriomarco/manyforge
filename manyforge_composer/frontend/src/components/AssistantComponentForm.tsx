import { useEffect, useState } from 'react';

import { api, type AssistantProfiles } from '../api/client';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { EffectiveValues } from '../types/components';
import { jsonObject, type JsonObject } from '../utils/poseSourceAuthoring';
import { aiModelComponents } from './AiModelComponentForm';
import type { FocusedEditorScope } from './DeploymentFocusedEditor';
import { LifecycleBudgetField } from './LifecycleBudgetField';

/**
 * Deployment authoring of the AI assistant component (`runtime_kind`
 * `assistant`): the AI tab's chat. It names the AI model component that
 * serves the chat and the maintained assistant profile with its chat mode.
 */

export const DEFAULT_ASSISTANT_PROFILE_ID = 'assistant_scene_authoring';
export const NO_AI_MODEL_FOR_ASSISTANT = 'Add an AI model component first.';

interface AssistantComponent {
  index: number;
  componentId: string;
  runtimeConfiguration: JsonObject;
}

const text = (value: JsonValue | undefined) => (typeof value === 'string' ? value : '');

export function assistantComponents(document: JsonObject | null): AssistantComponent[] {
  const components = Array.isArray(document?.components) ? document.components : [];
  return components.flatMap((value, index) => {
    const component = jsonObject(value);
    const configuration = jsonObject(component?.configuration);
    const componentId = component?.component_id;
    return configuration?.runtime_kind === 'assistant' && typeof componentId === 'string' ? [{
      index,
      componentId,
      runtimeConfiguration: jsonObject(configuration.runtime_configuration) ?? {},
    }] : [];
  });
}

function AssistantComponentForm({ controller, effective, component, profiles, profilesError }: {
  controller: DeploymentWorkspaceController;
  effective: EffectiveValues | null;
  component: AssistantComponent;
  profiles: AssistantProfiles | null;
  profilesError: string | null;
}) {
  const configuration = component.runtimeConfiguration;
  const prefix = component.componentId;
  const document = controller.workspace?.draft.document ?? null;
  const models = aiModelComponents(document);
  const modelId = text(configuration.model_component_id);
  const profileId = text(configuration.assistant_profile_id);
  const chatProfiles = Object.entries(profiles ?? {})
    .filter(([, profile]) => profile.chat).map(([id]) => id).sort();
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const write = async (name: string, value: string) => {
    // A refusal shows once, in the yellow Deployment notice row.
    await controller.mutate(
      ['components', component.index, 'configuration', 'runtime_configuration'], 'set',
      { ...configuration, [name]: value });
  };
  return (
    <section className="deployment-pose-source" aria-label={`${prefix} AI assistant`}>
      <header><strong>{prefix}</strong></header>
      <LifecycleBudgetField controller={controller} effective={effective} componentIndex={component.index}
        componentId={component.componentId} disabled={disabled} />
      <label className="deployment-focused-field">
        <span>AI model</span>
        <select aria-label={`${prefix} AI model`} value={modelId} disabled={disabled || models.length === 0}
          onChange={(event) => { void write('model_component_id', event.target.value); }}>
          {!models.some((model) => model.componentId === modelId) && (
            <option value={modelId} disabled>{modelId === '' ? 'Choose an AI model' : modelId}</option>
          )}
          {models.map((model) => (
            <option key={model.componentId} value={model.componentId}>{model.componentId}</option>
          ))}
        </select>
        {models.length === 0 ? (
          <small className="inline-message severity-error">{NO_AI_MODEL_FOR_ASSISTANT}</small>
        ) : !models.some((model) => model.componentId === modelId) && (
          <small className="inline-message severity-error">Choose the AI model for the chat.</small>
        )}
      </label>
      <label className="deployment-focused-field">
        <span>Assistant profile</span>
        <select aria-label={`${prefix} assistant profile`} value={profileId}
          disabled={disabled || profiles === null}
          onChange={(event) => { void write('assistant_profile_id', event.target.value); }}>
          {!chatProfiles.includes(profileId) && <option value={profileId} disabled>{profileId || 'Choose a profile'}</option>}
          {chatProfiles.map((id) => <option key={id} value={id}>{id}</option>)}
        </select>
        {profiles !== null && !chatProfiles.includes(profileId) && (
          <small className="inline-message severity-error">Choose a profile with a chat mode.</small>
        )}
      </label>
      {/* Yellow: a failed read blocks nothing (notice rule). */}
      {profilesError && <p className="deployment-field-error inline-message severity-warning" role="alert">{profilesError}</p>}
      <div className="button-row">
        <button type="button" disabled={disabled}
          onClick={() => { void controller.mutate(['components', component.index], 'remove'); }}>Remove AI assistant</button>
      </div>
    </section>
  );
}

/**
 * The Deployment's AI assistant component (one per cell). It is added from
 * Add component; one component's page shows only its form.
 */
export function AssistantComponentSection({ controller, effective = null, scope = 'all' }: {
  controller: DeploymentWorkspaceController;
  /** `GET /api/deployment/effective`, read once by the editor. */
  effective?: EffectiveValues | null;
  scope?: FocusedEditorScope;
}) {
  const document = controller.workspace?.draft.document ?? null;
  const components = assistantComponents(document).filter((component) => (
    typeof scope !== 'object' || component.componentId === scope.componentId));
  const shown = scope !== 'cell' && components.length > 0;
  const [profiles, setProfiles] = useState<AssistantProfiles | null>(null);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    if (!shown) return undefined;
    const controllerSignal = new AbortController();
    api.deploymentWorkspace.assistantProfiles(controllerSignal.signal)
      .then(setProfiles)
      .catch((reason: unknown) => {
        if (!controllerSignal.signal.aborted) {
          setError(reason instanceof Error ? reason.message : 'Could not read the assistant profiles.');
        }
      });
    return () => controllerSignal.abort();
  }, [shown]);
  if (!shown) return null;
  return (
    <details className="dcard" open>
      <summary>AI assistant</summary>
      <div className="deployment-focused-body">
        {components.map((component) => (
          <AssistantComponentForm key={component.componentId} controller={controller} effective={effective}
            component={component} profiles={profiles} profilesError={error} />
        ))}
        <p className="field-help">The AI tab's chat runs in this component. It answers only while it and its AI model are enabled and no Program run exists; it edits the Program Draft only.</p>
      </div>
    </details>
  );
}
