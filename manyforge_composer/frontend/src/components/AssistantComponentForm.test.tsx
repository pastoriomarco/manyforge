import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { api } from '../api/client';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { reactPropsHost } from '../test/reactPropsHost';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';

const aiModel = (componentId: string) => ({
  component_id: componentId,
  component_profile_id: 'ai_model_v0',
  target_settings: {},
  configuration: {
    runtime_kind: 'ai_model',
    runtime_configuration: { base_url: 'http://127.0.0.1:8000/v1', model: 'qwen', api_key_env: null },
  },
});

const assistant = (runtimeConfiguration: Record<string, JsonValue>) => ({
  component_id: 'assistant',
  component_profile_id: 'manyforge.assistant',
  target_settings: {},
  configuration: { runtime_kind: 'assistant', runtime_configuration: runtimeConfiguration },
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

async function mount(components: JsonValue[], scope: 'all' | { componentId: string } = { componentId: 'assistant' }) {
  vi.spyOn(api.deploymentWorkspace, 'assistantProfiles').mockResolvedValue({
    assistant_scene_authoring: { chat: true, modelEndpointAliases: [] },
    semantic_shape_selection: { chat: true, modelEndpointAliases: ['selection_model'] },
    vision_bin_selection: { chat: false, modelEndpointAliases: ['selection_model'] },
  });
  const controller = {
    workspace: {
      draft: {
        document: {
          api_version: 'manyforge.deployment_intent.v2',
          deployment_artifact_id: 'cell-a',
          name: 'Cell A',
          components,
          attachments: [],
          policies: {},
        },
      },
    },
    mutationAvailable: true,
    busy: false,
    mutate: vi.fn().mockResolvedValue({}),
    updateMotionTargetLimit: vi.fn(),
  } as unknown as DeploymentWorkspaceController;
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<DeploymentFocusedEditor controller={controller} scope={scope}
    sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} programSceneIds={null} />));
  await act(async () => { await Promise.resolve(); });
  const control = (label: string) => find((props) => props['aria-label'] === label);
  const texts = () => all((props) => typeof props.children === 'string').map((props) => props.children as string);
  return { controller, control, all, texts, unmount: () => act(() => root.unmount()) };
}

describe('AI assistant component form', () => {
  const path = ['components', 2, 'configuration', 'runtime_configuration'];
  const configured = { model_component_id: 'chat_model', assistant_profile_id: 'assistant_scene_authoring' };

  it('chooses its AI model among the Draft\'s AI models and its profile among those with a chat mode', async () => {
    const mounted = await mount([aiModel('chat_model'), aiModel('selection_model'), assistant(configured)]);
    try {
      expect(mounted.control('assistant AI model').value).toBe('chat_model');
      expect(mounted.control('assistant assistant profile').value).toBe('assistant_scene_authoring');
      const profileOptions = mounted.all((props) => props.value === 'vision_bin_selection');
      expect(profileOptions).toHaveLength(0);
      expect(mounted.control('assistant lifecycle command time limit')).toBeDefined();
      await act(() => mounted.control('assistant AI model').onChange?.({ target: { value: 'selection_model' } }));
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(path, 'set',
        { ...configured, model_component_id: 'selection_model' });
      await act(() => mounted.control('assistant assistant profile').onChange?.({
        target: { value: 'semantic_shape_selection' } }));
      expect(mounted.controller.mutate).toHaveBeenLastCalledWith(path, 'set',
        { ...configured, assistant_profile_id: 'semantic_shape_selection' });
      // It is no robot: no motion or runtime-unit sections.
      expect(mounted.all((props) => props.className === 'deployment-runtime-unit')).toHaveLength(0);
    } finally { await mounted.unmount(); }
  });

  it('refuses inline without an AI model and asks for a choice among several', async () => {
    const none = await mount([assistant({ assistant_profile_id: 'assistant_scene_authoring' })]);
    try {
      expect(none.control('assistant AI model').disabled).toBe(true);
      expect(none.texts()).toContain('Add an AI model component first.');
    } finally { await none.unmount(); }
    const several = await mount([aiModel('chat_model'), aiModel('selection_model'),
      assistant({ assistant_profile_id: 'assistant_scene_authoring' })]);
    try {
      expect(several.control('assistant AI model').value).toBe('');
      expect(several.texts()).toContain('Choose the AI model for the chat.');
    } finally { await several.unmount(); }
  });

  it('flags a profile without a chat mode', async () => {
    const mounted = await mount([aiModel('chat_model'),
      assistant({ model_component_id: 'chat_model', assistant_profile_id: 'vision_bin_selection' })]);
    try {
      expect(mounted.texts()).toContain('Choose a profile with a chat mode.');
    } finally { await mounted.unmount(); }
  });
});
