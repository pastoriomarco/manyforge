import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import {
  fallbackCandidates, pruneFallbacks, recordedFallbacks, toggleFallback,
} from '../utils/stopFallbackSequence';
import { DeploymentFocusedEditor } from './DeploymentFocusedEditor';
import { StopFallbackSequence } from './StopFallbackSequence';

const digest = `sha256:${'1'.repeat(64)}`;
const HARD = 'Hard deceleration (always last)';
const HOLD = 'aria-label="Fallback: Spring-back on path"';
const FAST = 'aria-label="Fallback: Spring-back fast"';

describe('stop fallback sequence rules', () => {
  it('offers only modes below the primary, in the fixed order', () => {
    expect(fallbackCandidates('soft_on_path')).toEqual(['spring_back_hold', 'spring_back_fast']);
    expect(fallbackCandidates('spring_back_hold')).toEqual(['spring_back_fast']);
    expect(fallbackCandidates('spring_back_fast')).toEqual([]);
    expect(fallbackCandidates('hard_deceleration')).toEqual([]);
  });

  it('prunes and toggles into a strict ordered subsequence', () => {
    expect(pruneFallbacks('spring_back_hold', ['spring_back_hold', 'spring_back_fast'])).toEqual(['spring_back_fast']);
    expect(pruneFallbacks('spring_back_fast', ['spring_back_fast'])).toEqual([]);
    expect(pruneFallbacks('soft_on_path', ['spring_back_fast', 'spring_back_hold', 'hard_deceleration']))
      .toEqual(['spring_back_hold', 'spring_back_fast']);
    expect(toggleFallback('soft_on_path', ['spring_back_fast'], 'spring_back_hold', true))
      .toEqual(['spring_back_hold', 'spring_back_fast']);
    expect(toggleFallback('soft_on_path', ['spring_back_hold', 'spring_back_fast'], 'spring_back_hold', false))
      .toEqual(['spring_back_fast']);
  });

  it('reads the list, the legacy substitution, and refuses both', () => {
    expect(recordedFallbacks({ stop_mode: 'soft_on_path', fallback_stop_modes: ['spring_back_fast'] }))
      .toEqual(['spring_back_fast']);
    expect(recordedFallbacks({ stop_mode: 'soft_on_path', recoverable_substitution_mode: 'spring_back_hold' }))
      .toEqual(['spring_back_hold']);
    expect(recordedFallbacks({ stop_mode: 'spring_back_hold', recoverable_substitution_mode: 'spring_back_hold' }))
      .toEqual([]);
    expect(recordedFallbacks({ stop_mode: 'soft_on_path' })).toEqual([]);
    expect(recordedFallbacks({
      stop_mode: 'soft_on_path', recoverable_substitution_mode: 'spring_back_hold', fallback_stop_modes: [],
    })).toBeNull();
  });
});

describe('StopFallbackSequence', () => {
  const render = (primary: string, value: string[] = []) => renderToStaticMarkup(
    <StopFallbackSequence primary={primary} value={value} disabled={false} onChange={vi.fn()} />,
  );

  it('lists the modes below the primary, then the fixed hard row', () => {
    const soft = render('soft_on_path', ['spring_back_fast']);
    expect(soft).toMatch(/role="group" aria-labelledby="([^"]+)"><span class="deployment-stop-fallbacks-heading" id="\1">Fallback if this Stop can&#x27;t be carried out<\/span>/);
    expect(soft).toContain('returns along the path; needs planning');
    expect(soft).toContain('returns at once, no planning or checks');
    expect(soft.indexOf(HOLD)).toBeLessThan(soft.indexOf(FAST));
    expect(soft.indexOf(FAST)).toBeLessThan(soft.indexOf(HARD));
    expect(soft).toMatch(new RegExp(`<input type="checkbox" ${FAST}[^>]*checked`));
    expect(soft).not.toMatch(new RegExp(`<input type="checkbox" ${HOLD}[^>]*checked`));
    expect(soft).toContain('a fallback gets no extra time');
    const hold = render('spring_back_hold');
    expect(hold).not.toContain(HOLD);
    expect(hold).toContain(FAST);
    const fast = render('spring_back_fast');
    expect(fast).not.toContain(HOLD);
    expect(fast).not.toContain(FAST);
    expect(fast).toContain(HARD);
    expect(render('hard_deceleration')).toBe('');
  });

  it('keeps the hard row checked and not removable', () => {
    const hard = /<input type="checkbox" aria-label="Fallback: Hard deceleration \(always last\)"[^>]*>/
      .exec(render('soft_on_path'))?.[0];
    expect(hard).toContain('checked=""');
    expect(hard).toContain('disabled=""');
  });
});

const limit = {
  resource_id: 'robot.arm.coordinate.000000',
  max_deceleration: { coordinate_kind: 'angular_rad_s2', value_rad_s2: 4 },
  settle_velocity_threshold: { coordinate_kind: 'angular_rad_s', value_rad_s: 0.01 },
};

function compiledDocument(parameters: Record<string, JsonValue>): Record<string, JsonValue> {
  return {
    api_version: 'manyforge.deployment.v1',
    deployment_artifact_id: 'cell-a', deployment_revision: digest, cell_id: 'cell-a', name: 'Cell A',
    graph: {
      control_bindings: [{ control_binding_id: 'arm_binding', control_binding_revision: digest,
        controllable_component_id: 'robot', command_target_id: 'arm',
        capability_id: 'execution.trajectory.execute' }],
      runtime_units: [],
    },
    policies: { stop_stage_parameters: [{
      control_binding_id: 'arm_binding', control_binding_revision: digest,
      stop_escalation_stage_id: 'motion_controlled_stop',
      provider_contract_ref: { provider_contract_role_id: 'stop_contract' },
      default_contract_parameters: {
        stop_mode: 'soft_on_path', deceleration_time_s: 1,
        allow_natural_completion_on_short_path: true, jerk_limits_enabled: false,
        per_resource_limits: [limit], ...parameters,
      },
    }] },
  };
}

function intentDocument(stop: Record<string, JsonValue>): Record<string, JsonValue> {
  return {
    api_version: 'manyforge.deployment_intent.v2',
    deployment_artifact_id: 'cell-a', name: 'Cell A',
    components: [{
      component_id: 'robot', target_settings: { arm: { controlled_stop: {
        stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: true,
        jerk_limits_enabled: false, deceleration_time_s: 1,
        per_joint_limits: [{
          source_joint_name: 'joint1', max_deceleration_rad_s2: 4, settle_velocity_threshold_rad_s: 0.01,
        }],
        ...stop,
      } } },
    }],
    attachments: [], policies: {},
  };
}

function controller(document: Record<string, JsonValue>): DeploymentWorkspaceController {
  return {
    workspace: {
      schemaVersion: 'manyforge.composer_deployment_workspace.v1',
      authoringWorkspace: {
        workspaceEpoch: 'workspace.epoch.1', workspaceRevision: 7,
        programDraftId: 'draft-1', programDraftRevision: 1,
        deploymentDraftId: 'deployment-draft-1', deploymentDraftRevision: 1,
      },
      draft: {
        id: 'deployment-draft-1', revision: 1, document,
        deploymentArtifactId: 'cell-a', deploymentRevision: digest, graphRevision: digest,
        differsFromSaved: false,
      },
      saved: { path: '/workspace/cell-a.deployment.yaml', deploymentRevision: digest, differsFromApplied: null },
      managerApplied: { activationOrder: null, deploymentRevision: digest, scenarioId: 'cell-a' },
      runAdmission: { ready: true, reason: null },
      binding: null,
      scenarioCandidates: [],
      savedScenarioCandidates: [],
      unavailableReasons: [],
    },
    loading: false, busy: false, applying: false, error: null, feedback: null,
    applyPreview: null,
    applyPreviewKind: 'apply',
    applyConfirmationOpen: false,
    busyOperation: null,
    mutationAvailable: true, mutationUnavailableReason: null,
    documentUnavailableReason: null,
    refresh: vi.fn(), clearFeedback: vi.fn(), dismissError: vi.fn(), open: vi.fn(), mutate: vi.fn(),
    updateControlledStop: vi.fn(), updateMotionTargetLimit: vi.fn(), revert: vi.fn(), save: vi.fn(),
    saveAs: vi.fn(), discard: vi.fn(), applyDraft: vi.fn(), applyConfirmedScope: vi.fn(), confirmApply: vi.fn(),
    cancelApplyConfirmation: vi.fn(),
  } as unknown as DeploymentWorkspaceController;
}

type FakeNode = Record<string, unknown>;
type Props = Record<string, unknown> & { onChange?: (event: unknown) => void };

// The mounted-test pattern of DeploymentFocusedEditor.test.tsx: real React
// handlers on a minimal DOM host.
function mountedHost() {
  const elements: FakeNode[] = [];
  const document: FakeNode = {
    nodeType: 9, activeElement: null, defaultView: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    createTextNode: (text: string) => ({ nodeType: 3, nodeValue: text, ownerDocument: document }),
  };
  const node = (tag: string): FakeNode => {
    const element = {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {}, options: [],
      addEventListener: () => undefined, removeEventListener: () => undefined,
      appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
      setAttribute: () => undefined, removeAttribute: () => undefined,
    };
    elements.push(element);
    return element;
  };
  document.createElement = node;
  document.documentElement = node('html');
  const window = { document, HTMLIFrameElement: class {} };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const find = (match: (props: Props) => boolean) => {
    const found = elements.map((element) => element[
      Object.keys(element).find((key) => key.startsWith('__reactProps$'))!
    ] as Props | undefined).find((props) => props !== undefined && match(props));
    if (found === undefined) throw new Error('Mounted control not found');
    return found;
  };
  return { host: node('div'), find };
}

afterEach(() => vi.unstubAllGlobals());

async function mountEditor(value: DeploymentWorkspaceController) {
  const { host, find } = mountedHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<DeploymentFocusedEditor controller={value}
    sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />));
  return { find, unmount: () => act(() => root.unmount()) };
}

const renderEditor = (value: DeploymentWorkspaceController) => renderToStaticMarkup(
  <DeploymentFocusedEditor controller={value}
    sourceModel={{ status: 'loading' }} motionDescriptors={[]} gripperDescriptors={[]} />,
);

describe('Stop settings fallback sequence', () => {
  it.each([
    ['soft_on_path', [HOLD, FAST]],
    ['spring_back_hold', [FAST]],
    ['spring_back_fast', []],
  ])('renders the fallbacks offered under a %s primary', (primary, offered) => {
    const markup = renderEditor(controller(intentDocument({ stop_mode: primary })));
    expect(markup).toContain(HARD);
    for (const label of [HOLD, FAST]) {
      if (offered.includes(label)) expect(markup).toContain(label);
      else expect(markup).not.toContain(label);
    }
  });

  it('commits each toggle with the full profile, in the fixed order', async () => {
    const value = controller(intentDocument({ fallback_stop_modes: ['spring_back_fast'] }));
    const { find, unmount } = await mountEditor(value);
    await act(() => find((props) => props['aria-label'] === 'Fallback: Spring-back on path')
      .onChange?.({ target: { checked: true } }));
    expect(value.updateControlledStop).toHaveBeenCalledWith({
      controllable_component_id: 'robot',
      command_target_id: 'arm',
      profile: {
        stop_mode: 'soft_on_path',
        deceleration_time_s: 1,
        allow_natural_completion_on_short_path: true,
        jerk_limits_enabled: false,
        per_resource_limits: [{
          resource_id: 'joint1',
          max_deceleration: { coordinate_kind: 'angular_rad_s2', value: 4 },
          settle_velocity_threshold: { coordinate_kind: 'angular_rad_s', value: 0.01 },
        }],
        fallback_stop_modes: ['spring_back_hold', 'spring_back_fast'],
      },
    });
    await unmount();
  });

  it('prunes fallbacks no longer below a changed primary', async () => {
    const value = controller(intentDocument({ fallback_stop_modes: ['spring_back_hold', 'spring_back_fast'] }));
    const { find, unmount } = await mountEditor(value);
    const modeSelect = () => find((props) => props.value === 'soft_on_path' && typeof props.onChange === 'function'
      && Array.isArray(props.children));
    await act(() => modeSelect().onChange?.({ target: { value: 'spring_back_hold' } }));
    expect(vi.mocked(value.updateControlledStop).mock.calls[0][0].profile?.fallback_stop_modes)
      .toEqual(['spring_back_fast']);
    await act(() => modeSelect().onChange?.({ target: { value: 'spring_back_fast' } }));
    expect(vi.mocked(value.updateControlledStop).mock.calls[1][0].profile?.fallback_stop_modes).toEqual([]);
    await unmount();
  });

  it('reads a compiled legacy substitution as Spring-back on path and sends the list', async () => {
    const value = controller(compiledDocument({ recoverable_substitution_mode: 'spring_back_hold' }));
    const markup = renderEditor(value);
    expect(markup).toMatch(new RegExp(`<input type="checkbox" ${HOLD}[^>]*checked`));
    expect(markup).not.toMatch(new RegExp(`<input type="checkbox" ${FAST}[^>]*checked`));
    const { find, unmount } = await mountEditor(value);
    await act(() => find((props) => props['aria-label'] === 'Fallback: Spring-back on path')
      .onChange?.({ target: { checked: false } }));
    const profile = vi.mocked(value.updateControlledStop).mock.calls[0][0].profile;
    expect(profile?.fallback_stop_modes).toEqual([]);
    expect(profile).not.toHaveProperty('recoverable_substitution_mode');
    await unmount();
  });

  it('offers no editable Stop for an ambiguous compiled document', () => {
    const markup = renderEditor(controller(compiledDocument({
      recoverable_substitution_mode: 'spring_back_hold', fallback_stop_modes: ['spring_back_hold'],
    })));
    expect(markup).not.toContain(HARD);
  });
});
