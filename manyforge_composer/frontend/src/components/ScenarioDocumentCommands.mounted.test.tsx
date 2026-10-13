import { act } from 'react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { ManagerActiveProjection } from '../types/api';
import { ScenarioDocumentCommands } from './ScenarioDocumentCommands';

const apiMock = vi.hoisted(() => ({
  manager: { status: vi.fn() }, deploymentWorkspace: { get: vi.fn() },
  scenarioDocuments: { list: vi.fn() }, files: { browse: vi.fn() },
}));
vi.mock('../api/client', () => ({ api: apiMock }));

// Existing mounted-regression approach: real React handlers/state/effects,
// with a minimal DOM host. The only owner effects are explicitly mocked below.
type Props = Record<string, unknown> & {
  onClick?: (event: unknown) => void; onChange?: (event: unknown) => void;
};
type FakeNode = Record<string, unknown> & { children: FakeNode[]; parentNode: FakeNode | null };
function mountedHost() {
  const document: Record<string, unknown> = { nodeType: 9, activeElement: null,
    addEventListener: () => undefined, removeEventListener: () => undefined };
  function node(tag: string): FakeNode {
    const element: FakeNode = {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document,
      style: {}, options: [], children: [], parentNode: null,
      addEventListener: () => undefined, removeEventListener: () => undefined,
      setAttribute: () => undefined, removeAttribute: () => undefined, focus: () => undefined,
      appendChild: (child: FakeNode) => { element.children.push(child); child.parentNode = element; },
      insertBefore: (child: FakeNode, before: FakeNode) => {
        element.children.splice(element.children.indexOf(before), 0, child); child.parentNode = element;
      },
      removeChild: (child: FakeNode) => {
        element.children.splice(element.children.indexOf(child), 1); child.parentNode = null;
      },
    };
    return element;
  }
  document.createElement = node;
  document.createElementNS = (_namespace: string, tag: string) => node(tag);
  document.createTextNode = (text: string) => ({ ...node('text'), nodeType: 3, nodeValue: text });
  document.documentElement = node('html');
  const window = { document, HTMLIFrameElement: class {}, requestAnimationFrame: (fn: () => void) => fn() };
  document.defaultView = window;
  vi.stubGlobal('window', window); vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const host = node('div');
  const props = (element: FakeNode) => element[
    Object.keys(element).find((key) => key.startsWith('__reactProps$'))!
  ] as Props | undefined;
  const walk = (element: FakeNode): FakeNode[] => [element, ...element.children.flatMap(walk)];
  const find = (match: (p: Props) => boolean) => {
    const value = walk(host).map(props).find((p) => p !== undefined && match(p));
    if (value === undefined) throw new Error('Mounted Scenario control not found');
    return value;
  };
  return { host, find };
}
const active: ManagerActiveProjection = {
  canonicalEmpty: false, retainedBundleId: 'bundle.current', graphRevision: 'graph.current',
  cellGraph: { controllable_components: [{ controllable_component_id: 'arm' }] }, publication: {},
};
const empty: ManagerActiveProjection = { ...active, canonicalEmpty: true,
  retainedBundleId: 'bundle.empty', graphRevision: 'graph.empty', cellGraph: {} };
function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}
let unmount: (() => Promise<void>) | null = null;
afterEach(async () => { await unmount?.(); unmount = null; vi.unstubAllGlobals(); });
beforeEach(() => {
  vi.clearAllMocks();
  apiMock.manager.status.mockResolvedValue({ source: { available: true, freshness: 'fresh' },
    status: { managerReady: true, active } });
  apiMock.deploymentWorkspace.get.mockResolvedValue({ managerApplied: {
    scenarioId: 'current', activationOrder: { retainedBundleId: active.retainedBundleId,
      graphRevision: active.graphRevision } } });
  apiMock.files.browse.mockResolvedValue({ entries: [] });
  apiMock.scenarioDocuments.list.mockResolvedValue({ directory: '/projects', entries: [
    { sourceKind: 'saved', path: '/projects/other', scenarioId: 'other', name: 'Other',
      description: '', displayStatus: 'ready', statusDetail: '', savedManyforgeVersion: null },
    { sourceKind: 'saved', path: '/projects/current', scenarioId: 'current', name: 'Current',
      description: '', displayStatus: 'ready', statusDetail: '', savedManyforgeVersion: null },
  ] });
});
async function mount(closeScenario = vi.fn(async () => empty as ManagerActiveProjection | null),
  documentUnavailableReason: string | null = null) {
  const dom = mountedHost();
  const controller = {
    documentUnavailableReason, state: { status: 'ready', pendingAction: null, workspace: {
      authoringWorkspace: { revision: 1 }, draft: { authoring: null }, hasUnsavedChanges: true,
      hasUnappliedChanges: true } }, scenarioDocumentPath: '/projects/current',
    scenarioDocument: { id: 'current', name: 'Current', description: '' },
    saveScenarioDocument: vi.fn(), exportScenarioDocument: vi.fn(),
    newScenarioDocument: vi.fn(async () => ({})), loadScenarioDocument: vi.fn(async () => ({})),
  } as unknown as ProgramWorkspaceController;
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(dom.host as unknown as Element);
  await act(() => root.render(<ScenarioDocumentCommands controller={controller} closeScenario={closeScenario} />));
  unmount = async () => { await act(() => root.unmount()); };
  const click = async (props: Props) => { await act(async () => props.onClick?.({ currentTarget: { focus: () => undefined } })); };
  const open = async (label: string) => click(dom.find((p) => p['aria-label'] === label));
  const confirm = async () => { await act(() => dom.find((p) => p.type === 'checkbox').onChange?.({ target: { checked: true } })); };
  const choose = async (id: string) => { await act(() => dom.find((p) => p.children instanceof Array
    && (p.children as Array<unknown>).some((c) => (c as { props?: Props })?.props?.children === 'Select a Scenario'))
    .onChange?.({ target: { value: `saved:/projects/${id}` } })); };
  const primary = (label: string) => dom.find((p) => p.className === 'primary-action' && p.children === label);
  return { ...dom, controller, closeScenario, click, open, confirm, choose, primary };
}

it('Cancel before confirmation sends no Close, creation or Load effect', async () => {
  const test = await mount(); await test.open('New');
  await test.click(test.find((p) => p.children === 'Cancel'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  expect(() => test.find((p) => p.role === 'dialog')).toThrow();
});

it('different Load requires one confirmation and waits for successful Close', async () => {
  const close = deferred<ManagerActiveProjection | null>();
  const test = await mount(vi.fn(() => close.promise));
  await test.open('Load Scenario'); await test.choose('other');
  expect(test.primary('Load Scenario').disabled).toBe(true);
  await test.click(test.primary('Load Scenario'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  await test.confirm(); await test.click(test.primary('Load Scenario'));
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(active);
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  await test.click(test.find((p) => p.children === 'Cancel'));
  expect(test.find((p) => p.role === 'dialog')).toBeDefined();
  await act(() => close.resolve(empty));
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/other', true);
  expect(() => test.find((p) => p.role === 'dialog')).toThrow();
});

it.each(['unknown', 'failure', 'not_empty'])('does not load a replacement after Close %s', async (kind) => {
  const close = vi.fn(async () => {
    if (kind === 'failure') throw new Error('process drain is unknown');
    return kind === 'unknown' ? null : active;
  });
  const test = await mount(close); await test.open('Load Scenario'); await test.choose('other');
  await test.confirm(); await test.click(test.primary('Load Scenario'));
  expect(close).toHaveBeenCalledOnce();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  expect(test.find((p) => p.role === 'dialog')).toBeDefined();
});

it('same-scenario reload applies differences without Close', async () => {
  const test = await mount(); await test.open('Load Scenario'); await test.choose('current');
  await test.confirm(); await test.click(test.primary('Load Scenario'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/current', true);
});

it('New creates only after Close and names its resulting empty publication', async () => {
  const close = deferred<ManagerActiveProjection | null>();
  const test = await mount(vi.fn(() => close.promise)); await test.open('New');
  await act(() => test.find((p) => p.placeholder === 'e.g. lite6_pick_and_place')
    .onChange?.({ target: { value: 'next_cell' } }));
  await test.confirm(); await test.click(test.primary('Close and create Scenario'));
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
  await act(() => close.resolve(empty));
  expect(test.controller.newScenarioDocument).toHaveBeenCalledExactlyOnceWith({
    path: '/projects/next_cell.scenario', name: 'next_cell', deploymentChoice: 'empty',
    expectedRetainedBundleId: empty.retainedBundleId, expectedGraphRevision: empty.graphRevision,
    confirmDiscard: true,
  });
});

it('Close stays available when ordinary document admission needs stopped-operation Reset', async () => {
  const test = await mount(undefined, 'Reset stopped operations first');
  expect(test.find((p) => p['aria-label'] === 'Close Scenario').disabled).toBe(false);
  await test.open('Close Scenario'); await test.confirm(); await test.click(test.primary('Close Scenario'));
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(active);
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
});


it('does not infer full Close completion from an already-empty owner publication', async () => {
  apiMock.manager.status.mockResolvedValue({ source: { available: true, freshness: 'fresh' },
    status: { managerReady: true, active: empty } });
  const test = await mount(); await test.open('Close Scenario'); await test.confirm();
  await test.click(test.primary('Close Scenario'));
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(empty);
});

it('blocks same-scenario reload on ordinary document admission without using Close', async () => {
  const test = await mount(undefined, 'Reset stopped operations first');
  await test.open('Load Scenario'); await test.choose('current'); await test.confirm();
  expect(test.primary('Load Scenario').disabled).toBe(true);
  await test.click(test.primary('Load Scenario'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
});

it('keeps the dialog after an unresolved replacement without sending it again', async () => {
  const test = await mount();
  vi.mocked(test.controller.loadScenarioDocument).mockResolvedValue(null);
  await test.open('Load Scenario'); await test.choose('other'); await test.confirm();
  await test.click(test.primary('Load Scenario'));
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledOnce();
  expect(test.find((p) => p.role === 'dialog')).toBeDefined();
});
