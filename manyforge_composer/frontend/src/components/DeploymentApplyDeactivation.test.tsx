import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { CCLCController } from '../hooks/useCCLC';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { reactPropsHost } from '../test/reactPropsHost';
import { DeploymentDocumentControls } from './DeploymentDocumentControls';

afterEach(() => vi.unstubAllGlobals());

const digest = (value: string) => `sha256:${value.repeat(64)}`;

function controller(components: Array<{ id: string; disposition: string; reasons: string[] }>) {
  return {
    workspace: {
      draft: { document: { api_version: 'manyforge.deployment_intent.v2' }, deploymentRevision: null },
      saved: { path: null, deploymentRevision: null },
      managerApplied: { activationOrder: null, deploymentRevision: digest('1'), scenarioId: 'cell' },
      runAdmission: { ready: false, reason: 'Deployment Draft differs from Manager Applied' },
      savedScenarioCandidates: [],
    },
    busy: false,
    applying: false,
    mutationUnavailableReason: null,
    documentUnavailableReason: null,
    pendingApplyConfirmation: {
      result: {
        status: 'confirmation_required',
        confirmationToken: digest('7'),
        preparedActivation: { deploymentArtifactId: 'cell', deploymentRevision: digest('2') },
        candidatePreview: { impact: {
          components: components.map((row) => ({
            controllable_component_id: row.id, disposition: row.disposition, replacement_reasons: row.reasons,
          })),
          detachedComponents: [],
          attachmentChangedComponents: [],
          program: { executable: true, unresolvedReferences: [], affectedExecutionGroups: [] },
        } },
      },
    },
    confirmApply: vi.fn().mockResolvedValue(null),
    cancelApplyConfirmation: vi.fn(),
  } as unknown as DeploymentWorkspaceController;
}

function lifecycle(states: Record<string, string>, deactivated = true) {
  return {
    readActivationSequence: vi.fn().mockResolvedValue({
      identity: 'identity.1',
      components: Object.entries(states).map(([controllableComponentId, lifecycleState]) => ({
        controllableComponentId, lifecycleState,
      })),
    }),
    deactivateComponents: vi.fn().mockResolvedValue(deactivated
      ? { succeeded: true, consumedAdmissionGeneration: 36 } : { succeeded: false }),
    cancelActivation: vi.fn(),
    activationProgress: null,
  } as unknown as CCLCController;
}

async function mount(deployment: DeploymentWorkspaceController, cclc: CCLCController, generation = 36) {
  const { host, find, all } = reactPropsHost();
  (window as unknown as { requestAnimationFrame: () => number }).requestAnimationFrame = () => 0;
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const flush = () => act(async () => { for (let i = 0; i < 5; ++i) await Promise.resolve(); });
  const render = async (admission: number) => {
    await act(async () => {
      root.render(<DeploymentDocumentControls controller={deployment} cclc={cclc}
        authoringAdmissionGeneration={admission} />);
    });
    await flush();
  };
  await render(generation);
  const button = (text: string) => find((props) => props.type === 'button' && props.children === text);
  return { find, all, button, render, flush, unmount: () => act(async () => { root.unmount(); }) };
}

describe('Deployment Apply that changes active components', () => {
  it('names the active changed components and deactivates them in reverse order before Apply', async () => {
    const deployment = controller([
      { id: 'arm', disposition: 'replace', reasons: ['retained_policies'] },
      { id: 'gripper', disposition: 'replace', reasons: ['retained_policies'] },
      { id: 'station_io', disposition: 'retain', reasons: [] },
    ]);
    const cclc = lifecycle({ arm: 'active', gripper: 'active', station_io: 'active' });
    const mounted = await mount(deployment, cclc);
    try {
      expect(mounted.find((props) => props.id === 'deployment-apply-confirmation-title').children)
        .toBe('Deactivate components and apply?');
      expect(() => mounted.button('Confirm apply')).toThrow('Mounted control not found');
      await act(async () => { mounted.button('Deactivate and apply').onClick?.(); });
      await mounted.flush();
      // Reverse activation order; the unchanged component is never touched.
      expect(cclc.deactivateComponents).toHaveBeenCalledExactlyOnceWith(['gripper', 'arm'], 'identity.1');
      // Each Disable consumed admission 36: Apply waits for a newer one (live SX-2911).
      expect(deployment.confirmApply).not.toHaveBeenCalled();
      await mounted.render(36);
      expect(deployment.confirmApply).not.toHaveBeenCalled();
      await mounted.render(37);
      expect(deployment.confirmApply).toHaveBeenCalledOnce();
    } finally { await mounted.unmount(); }
  });

  it('says which global rules will wait for a deactivated source instead of latching', async () => {
    const deployment = controller([
      { id: 'arm', disposition: 'replace', reasons: ['retained_policies'] },
      { id: 'station_io', disposition: 'replace', reasons: ['retained_policies'] },
    ]);
    (deployment.workspace!.draft as { document: unknown }).document = {
      api_version: 'manyforge.deployment_intent.v2',
      policies: { interrupt_rules: [{
        rule_id: 'light_curtain', scope: 'all_motion',
        condition: { compare: { op: '==', value: true,
          operand: { signal: { controllable_component_id: 'station_io', signal_name: 'curtain_blocked' } } } },
      }, {
        rule_id: 'cell_ready', scope: 'all_motion',
        condition: { compare: { op: '==', value: true, operand: { key: 'cell.ready' } } },
      }] },
    };
    const cclc = lifecycle({ arm: 'active', station_io: 'active' });
    const mounted = await mount(deployment, cclc);
    try {
      const text = mounted.all((props) => Array.isArray(props.children))
        .map((props) => (props.children as unknown[]).filter((part) => typeof part === 'string').join(''));
      expect(text.some((line) => line.includes('Global rule light_curtain will wait for its source (station_io inactive)')))
        .toBe(true);
      expect(text.some((line) => line.includes('cell_ready'))).toBe(false);
    } finally { await mounted.unmount(); }
  });

  it('does not send Apply when deactivation stops', async () => {
    const deployment = controller([{ id: 'arm', disposition: 'replace', reasons: ['domains'] }]);
    const cclc = lifecycle({ arm: 'active' }, false);
    const mounted = await mount(deployment, cclc);
    try {
      await act(async () => { mounted.button('Deactivate and apply').onClick?.(); });
      await mounted.flush();
      await mounted.render(37);
      expect(deployment.confirmApply).not.toHaveBeenCalled();
      expect(mounted.all((props) => props.role === 'alert').map((props) => props.children))
        .toContain('Deactivation stopped; Apply was not sent. See component lifecycle details.');
    } finally { await mounted.unmount(); }
  });

  it('keeps the ordinary confirmation when every changed component is already inactive', async () => {
    const deployment = controller([{ id: 'arm', disposition: 'replace', reasons: ['domains'] }]);
    const cclc = lifecycle({ arm: 'disabled' });
    const mounted = await mount(deployment, cclc);
    try {
      expect(mounted.find((props) => props.id === 'deployment-apply-confirmation-title').children)
        .toBe('Apply Deployment configuration change?');
      await act(async () => { mounted.button('Confirm apply').onClick?.(); });
      expect(cclc.deactivateComponents).not.toHaveBeenCalled();
      expect(deployment.confirmApply).toHaveBeenCalledOnce();
    } finally { await mounted.unmount(); }
  });
});
