import { act, useEffect } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { CCLCController } from '../hooks/useCCLC';
import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import { reactPropsHost } from '../test/reactPropsHost';
import { deploymentDocumentHeader } from './DeploymentDocumentControls';
import { DocumentControls } from './DocumentControls';
import { programDocumentHeader } from './ProgramDocumentControls';
import { ScenarioActionControls } from './ScenarioActionControls';

afterEach(() => vi.unstubAllGlobals());

const digest = (value: string) => `sha256:${value.repeat(64)}`;

function controller(
  components: Array<{ id: string; disposition: string; reasons: string[] }>,
  pendingConfirmationAction: 'apply' | 'revert' = 'apply',
) {
  return {
    workspace: {
      draft: { document: { api_version: 'manyforge.deployment_intent.v2' }, deploymentRevision: null },
      saved: { path: null, deploymentRevision: null, differsFromApplied: null },
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
    pendingConfirmationAction,
    confirmApply: vi.fn().mockResolvedValue(null),
    cancelApplyConfirmation: vi.fn(),
  } as unknown as DeploymentWorkspaceController;
}

// The Program side has nothing pending: the Deployment confirmation is shown.
const program = {
  state: { status: 'ready', pendingAction: null, workspace: null },
  documentUnavailableReason: null,
  mutationUnavailableReason: null,
} as unknown as ProgramWorkspaceController;
const runtime = { state: { currentScenario: null } } as unknown as ProgramRuntimeController;

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
      root.render(<ScenarioActionControls program={program} deployment={deployment} runtime={runtime}
        cclc={cclc} authoringAdmissionGeneration={admission} />);
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

  it('words a confirmed Revert as restoring the Saved documents, never as applying the Draft', async () => {
    const ordinary = await mount(
      controller([{ id: 'arm', disposition: 'replace', reasons: ['domains'] }], 'revert'),
      lifecycle({ arm: 'disabled' }),
    );
    try {
      expect(ordinary.find((props) => props.id === 'deployment-apply-confirmation-title').children)
        .toBe('Revert Deployment configuration?');
      expect(() => ordinary.button('Confirm apply')).toThrow('Mounted control not found');
      expect(ordinary.button('Confirm revert')).toBeDefined();
    } finally { await ordinary.unmount(); }
    const deactivating = await mount(
      controller([{ id: 'arm', disposition: 'replace', reasons: ['domains'] }], 'revert'),
      lifecycle({ arm: 'active' }),
    );
    try {
      expect(deactivating.find((props) => props.id === 'deployment-apply-confirmation-title').children)
        .toBe('Deactivate components and revert?');
      expect(deactivating.button('Deactivate and revert')).toBeDefined();
    } finally { await deactivating.unmount(); }
  });

  it('enables a confirmed Revert whose Draft already matches Applied', async () => {
    // Saved differs from Applied while the Draft is the Applied one: nothing to
    // apply, but the Revert of the Saved input is exactly what was confirmed.
    const reverting = (action: 'apply' | 'revert') => {
      const deployment = controller([{ id: 'arm', disposition: 'replace', reasons: ['domains'] }], action);
      Object.assign(deployment.workspace!, {
        draft: { document: { api_version: 'manyforge.deployment_intent.v2' }, deploymentRevision: digest('1') },
        saved: { path: '/cell/cell.deployment.yaml', deploymentRevision: digest('5'), differsFromApplied: null },
      });
      return deployment;
    };
    const revert = await mount(reverting('revert'), lifecycle({ arm: 'active' }));
    try {
      expect(revert.button('Deactivate and revert').disabled).toBe(false);
      expect(() => revert.find((props) => props.children === 'Nothing to apply: the Deployment Draft matches Manager Applied.'))
        .toThrow('Mounted control not found');
    } finally { await revert.unmount(); }
    // The same workspace confirmed as an Apply still has nothing to apply.
    const apply = await mount(reverting('apply'), lifecycle({ arm: 'active' }));
    try {
      expect(apply.button('Deactivate and apply').disabled).toBe(true);
    } finally { await apply.unmount(); }
  });

  it('keeps a deactivation awaiting admission across a Program/Deployment tab switch', async () => {
    const deployment = controller([{ id: 'arm', disposition: 'replace', reasons: ['domains'] }]);
    const cclc = lifecycle({ arm: 'active' });
    const programHeader = {
      ...program, scenarioSourceDiagnostics: [],
    } as unknown as ProgramWorkspaceController;
    let actionMounts = 0;
    function MountProbe() {
      useEffect(() => { actionMounts += 1; }, []);
      return null;
    }
    const { host, find, all } = reactPropsHost();
    (window as unknown as { requestAnimationFrame: () => number }).requestAnimationFrame = () => 0;
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    const flush = () => act(async () => { for (let i = 0; i < 5; ++i) await Promise.resolve(); });
    // As in App: one DocumentControls whose header props follow the tab.
    const render = async (tab: 'program' | 'deployment', admission: number) => {
      await act(async () => {
        root.render(<DocumentControls
          {...(tab === 'deployment' ? deploymentDocumentHeader(deployment) : programDocumentHeader(programHeader))}
          actions={<><MountProbe /><ScenarioActionControls program={program} deployment={deployment}
            runtime={runtime} cclc={cclc} authoringAdmissionGeneration={admission} /></>}
        />);
      });
      await flush();
    };
    const waiting = 'Components deactivated; waiting for updated backend authoring permissions…';
    try {
      await render('program', 36);
      await act(async () => {
        find((props) => props.type === 'button' && props.children === 'Deactivate and apply').onClick?.();
      });
      await flush();
      expect(all((props) => props.role === 'status').map((props) => props.children)).toContain(waiting);
      await render('deployment', 36);
      expect(find((props) => props.role === 'status' && props.children === waiting)).toBeDefined();
      expect(deployment.confirmApply).not.toHaveBeenCalled();
      await render('deployment', 37);
      expect(deployment.confirmApply).toHaveBeenCalledOnce();
      expect(actionMounts).toBe(1);
    } finally { await act(async () => { root.unmount(); }); }
  });
});
