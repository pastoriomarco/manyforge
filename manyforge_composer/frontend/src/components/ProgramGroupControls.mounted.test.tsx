import { act, useState } from 'react';
import { afterEach, expect, it, vi } from 'vitest';

import type { ProgramNodeRecoveryPolicy, ProgramTreeNode } from '../types/api';
import { executionGroupScopes, type ExecutionGroupScope } from '../state/programExecutionGroups';
import { ProgramPolicyEditor } from './ProgramPolicyEditor';
import { MotionControlSettings } from './ProgramRuntimePanel';

afterEach(() => vi.unstubAllGlobals());

// Same minimal React DOM host as the schema-editor mounted regressions: real
// state/effects and handlers, without adding a DOM dependency or browser stack.
type FakeNode = Record<string, unknown>;
type Props = Record<string, unknown> & { onChange?: (event: unknown) => void; onBlur?: () => void; onClick?: () => void };
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
  const props = (element: FakeNode) => element[
    Object.keys(element).find((key) => key.startsWith('__reactProps$'))!
  ] as Props | undefined;
  const find = (match: (p: Props) => boolean) => {
    const found = elements.map(props).find((p) => p !== undefined && match(p));
    if (found === undefined) throw new Error('Mounted control not found');
    return found;
  };
  return { host: node('div'), find };
}

type GroupWrite = ProgramTreeNode['execution_groups'];

/** Mount the node policy editor; `applyWrites` false keeps the Draft round trip pending. */
async function mountPolicy(initial: ProgramTreeNode, inheritedGroups: string[], applyWrites = true) {
  const { host, find } = mountedHost();
  const writes: GroupWrite[] = [];
  const recoveryWrites: ProgramNodeRecoveryPolicy[] = [];
  const createGroup = vi.fn();
  function Host() {
    const [node, setNode] = useState<ProgramTreeNode>(initial);
    return <ProgramPolicyEditor node={node} inheritedGroups={inheritedGroups}
      availableGroups={['left', 'right']} onCreateGroup={createGroup} busy={false}
      onUpdate={(_, recovery_policy, operator_stop_policy, execution_groups) => {
        writes.push(execution_groups);
        recoveryWrites.push(recovery_policy);
        if (applyWrites) {
          setNode({ ...node, recovery_policy, operator_stop_policy, execution_groups });
        }
      }} />;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<Host />));
  const group = (name: string) => {
    const label = find((p) => p.className === 'toggle-control'
      && Array.isArray(p.children) && p.children.includes(name));
    return (label.children as Array<{ props: Props }>)[0].props;
  };
  const inherit = () => find((p) => p.id === `policy-${initial.instance_id}-execution_groups`);
  const change = (props: Props, checked: boolean) => act(() => props.onChange?.({ target: { checked } }));
  const blur = () => act(() => find((p) => p.className === 'program-policy-form').onBlur?.());
  return { writes, recoveryWrites, createGroup, group, inherit, change, blur, find, unmount: () => act(() => root.unmount()) };
}

const move: ProgramTreeNode = { node_id: 'motion.move_to_pose', instance_id: 'move', parameters: {}, children: [] };

it('commits checkbox changes immediately, once, and canonically sorted', async () => {
  const mounted = await mountPolicy(move, ['left']);
  const { writes, group, inherit, change, blur } = mounted;
  try {
    expect(group('left').disabled).toBe(true);
    expect(writes).toEqual([]);
    // Inherit off starts from the inherited named groups, preserving coverage.
    await change(inherit(), false);
    expect(writes).toEqual([['left']]);
    await blur();
    expect(writes).toEqual([['left']]);
    await change(group('left'), false);
    // Clicked out of order: right before left.
    await change(group('right'), true);
    await change(group('left'), true);
    await blur();
    await change(group('left'), false);
    await change(group('right'), false);
    await change(inherit(), true);
    expect(writes).toEqual([['left'], 'all', ['right'], ['left', 'right'], ['right'], 'all', 'inherit']);
    await act(() => mounted.find((p) => p.children === 'Create group…').onClick?.());
    expect(mounted.createGroup).toHaveBeenCalledOnce();
  } finally { await mounted.unmount(); }
});

it('does not resubmit the same change on blur or repeat while the Draft write is pending', async () => {
  const mounted = await mountPolicy(move, [], false);
  const { writes, group, inherit, change, blur } = mounted;
  try {
    // Nothing named is inherited: Inherit off remains All only.
    await change(inherit(), false);
    expect(writes).toEqual(['all']);
    await blur();
    await change(inherit(), false);
    await blur();
    expect(writes).toEqual(['all']);
    await change(group('right'), true);
    await blur();
    expect(writes).toEqual(['all', ['right']]);
  } finally { await mounted.unmount(); }
});

it('shows an undeclared membership as missing and lets it be removed, not re-added', async () => {
  const mounted = await mountPolicy({ ...move, execution_groups: ['ghost', 'left'] }, []);
  const { writes, group, change, find } = mounted;
  try {
    expect(group('ghost — missing').checked).toBe(true);
    expect(group('ghost — missing').disabled).toBe(false);
    await change(group('ghost — missing'), false);
    expect(writes).toEqual([['left']]);
    expect(() => find((p) => p.className === 'toggle-control' && Array.isArray(p.children)
      && p.children.includes('ghost — missing'))).toThrow();
  } finally { await mounted.unmount(); }
});

async function mountVisibility(scopes: ExecutionGroupScope[], initial: string[] | undefined) {
  const { host, find } = mountedHost();
  const changes: string[][] = [];
  function Host() {
    const [hidden, setHidden] = useState<string[] | undefined>(initial);
    return <MotionControlSettings groups={{ scopes, hidden, pendingApply: true,
      onChange: (next) => { changes.push(next); setHidden(next); } }} />;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<Host />));
  const checkbox = (name: string) => {
    const label = find((p) => p.className === 'toggle-control' && Array.isArray(p.children)
      && p.children.some((child) => child?.props?.title === name));
    return (label.children as Array<{ props: Props }>)[0].props;
  };
  const toggle = (name: string, checked: boolean) => act(() => checkbox(name).onChange?.({ target: { checked } }));
  return { changes, checkbox, toggle, unmount: () => act(() => root.unmount()) };
}

it('blocks an uncovered hide in the handler until All is explicitly shown', async () => {
  const scopes = executionGroupScopes([{
    node_id: 'motion.move_to_pose', instance_id: 'move', parameters: {},
    execution_groups: ['robot'], children: [],
  }], ['robot']);
  const { changes, checkbox, toggle, unmount } = await mountVisibility(scopes, undefined);
  try {
    expect(checkbox('robot').disabled).toBe(true);
    await toggle('robot', false);
    expect(changes).toEqual([]);
    await toggle('all', true);
    expect(checkbox('robot').disabled).toBe(false);
    await toggle('robot', false);
    expect(changes).toEqual([[], ['robot']]);
    expect(checkbox('all').disabled).toBe(true);
  } finally { await unmount(); }
});

it('saves only the requested group while All is revealed automatically', async () => {
  const scopes = executionGroupScopes([{
    node_id: 'parallel', instance_id: 'root', parameters: {}, children: [
      { node_id: 'motion.move_to_pose', instance_id: 'a', parameters: {}, execution_groups: ['left'], children: [] },
      { node_id: 'gripper.command_position', instance_id: 'g', parameters: {}, execution_groups: ['right'], children: [] },
      { node_id: 'io.set_output', instance_id: 'new-output', parameters: {}, execution_groups: 'all', children: [] },
    ],
  }], ['left', 'right']);
  // An older saved layout: All hidden, right hidden; the new output node is uncovered.
  const { changes, checkbox, toggle, unmount } = await mountVisibility(scopes, ['all', 'right']);
  try {
    expect(checkbox('all').checked).toBe(false);
    await toggle('right', true);
    expect(changes).toEqual([['all']]);
    // Hiding is refused while the saved choice leaves the output uncovered.
    await toggle('left', false);
    expect(changes).toEqual([['all']]);
    await toggle('all', true);
    expect(changes).toEqual([['all'], []]);
    expect(checkbox('all').checked).toBe(true);
  } finally { await unmount(); }
});

it('authors Allow intervention and a Retry limit of Unlimited or N, never a bare 0', async () => {
  const mounted = await mountPolicy(move, []);
  const { recoveryWrites, find, blur } = mounted;
  try {
    const intervention = () => find((p) => p.id === 'policy-move-allow_intervention');
    const mode = () => find((p) => p.id === 'policy-move-retry_limit_mode');
    // The omitted default is shown as Unlimited, not as a numeric 0.
    expect(mode().value).toBe('unlimited');
    expect(() => find((p) => p.id === 'policy-move-max_retries')).toThrow();
    // Unlimited does not imply intervention: it stays an explicit opt-in.
    expect(intervention().checked).toBe(false);
    await act(() => intervention().onChange?.({ target: { checked: true } }));
    expect(recoveryWrites).toEqual([{ allow_intervention: true, max_retries: 0 }]);
    await act(() => mode().onChange?.({ target: { value: 'limited' } }));
    expect(recoveryWrites.at(-1)).toEqual({ allow_intervention: true, max_retries: 1 });
    const limit = () => find((p) => p.id === 'policy-move-max_retries');
    expect(limit().min).toBe(1);
    await act(() => limit().onChange?.({ target: { value: '0' } }));
    await blur();
    // Zero is not a finite limit: it is refused locally rather than saved.
    expect(limit()['aria-invalid']).toBe(true);
    expect(recoveryWrites).toHaveLength(2);
    await act(() => limit().onChange?.({ target: { value: '3' } }));
    await blur();
    expect(recoveryWrites.at(-1)).toEqual({ allow_intervention: true, max_retries: 3 });
    await act(() => mode().onChange?.({ target: { value: 'unlimited' } }));
    expect(recoveryWrites.at(-1)).toEqual({ allow_intervention: true, max_retries: 0 });
  } finally { await mounted.unmount(); }
});
