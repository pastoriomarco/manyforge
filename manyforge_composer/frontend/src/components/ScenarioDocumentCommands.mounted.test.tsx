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
  cellGraph: { controllable_components: [{ controllable_component_id: 'arm' }] },
  publication: { publication_generation: 1, durable_public_pointer_content_digest: 'pointer.1',
    projection_revision: { retained_bundle_id: 'bundle.current', graph_revision: 'graph.current',
      scene_revision: { scene_id: 'scene', revision_id: 1, content_hash: 'digest' } },
    transaction: { kind: 'bootstrap', bootstrap_id: 'boot', bootstrap_generation: 1 } },
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
    { sourceKind: 'saved', path: '/projects/other', scenarioId: 'other', name: 'Other', folderName: 'other',
      location: 'projects', description: '', displayStatus: 'ready', statusDetail: '', savedManyforgeVersion: null },
    { sourceKind: 'saved', path: '/projects/current', scenarioId: 'current', name: 'Current', folderName: 'current',
      location: 'projects', description: '', displayStatus: 'ready', statusDetail: '', savedManyforgeVersion: null },
  ] });
});
async function mount(closeScenario = vi.fn(async () => empty as ManagerActiveProjection | null),
  documentUnavailableReason: string | null = null, motionActive = false, documentPath = '/projects/current.scenario',
  readOnly = false, hasChanges = true,
  forceCloseScenario?: (expected: ManagerActiveProjection, confirmed?: unknown, signal?: AbortSignal) => Promise<ManagerActiveProjection | null>,
  closeUnavailableReason: string | null = null) {
  const dom = mountedHost();
  const controller = {
    documentUnavailableReason, state: { status: 'ready', pendingAction: null, workspace: {
      authoringWorkspace: { revision: 1 }, draft: { authoring: null }, hasUnsavedChanges: hasChanges,
      hasUnappliedChanges: hasChanges } }, scenarioDocumentPath: documentPath,
    scenarioDocument: { id: readOnly ? 'Untitled' : 'current', name: 'Current', description: '', readOnly },
    saveScenarioDocument: vi.fn(), exportScenarioDocument: vi.fn(),
    scenarioReplacementIdentity: { authoring: { workspaceEpoch: 'workspace', workspaceRevision: 1,
      programDraftId: 'draft', programDraftRevision: 1, deploymentDraftId: 'deployment', deploymentDraftRevision: 1 },
      managerActive: { canonical_empty: active.canonicalEmpty, retained_bundle_id: active.retainedBundleId,
        graph_revision: active.graphRevision, publication: active.publication } },
    reportCloseCancellation: vi.fn(),
    scenarioCloseHandoff: vi.fn(() => ({ status: 'ready', message: null })),
    newScenarioDocument: vi.fn(async () => ({})), loadScenarioDocument: vi.fn(async () => ({})),
  } as unknown as ProgramWorkspaceController;
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(dom.host as unknown as Element);
  await act(() => root.render(<ScenarioDocumentCommands controller={controller} closeScenario={closeScenario}
    forceCloseScenario={forceCloseScenario} motionActive={motionActive}
    closeUnavailableReason={closeUnavailableReason} />));
  unmount = async () => { await act(() => root.unmount()); };
  const click = async (props: Props) => { await act(async () => props.onClick?.({ currentTarget: { focus: () => undefined } })); };
  const open = async (label: string) => click(dom.find((p) => p['aria-label'] === label));
  const confirm = async () => { await act(() => dom.find((p) => p.type === 'checkbox').onChange?.({ target: { checked: true } })); };
  const ack = async () => click(dom.find((p) => p.children === 'Close and continue'));
  const choose = async (id: string) => { await act(() => dom.find((p) => p.children instanceof Array
    && (p.children as Array<unknown>).some((c) => (c as { props?: Props })?.props?.children === 'Select a Scenario'))
    .onChange?.({ target: { value: `saved:/projects/${id}` } })); };
  // Confirmations use the danger look for Close, the primary look elsewhere.
  const primary = (label: string) => dom.find((p) => (p.className === 'primary-action' || p.className === 'danger-button') && p.children === label);
  return { ...dom, controller, closeScenario, click, open, confirm, choose, primary, ack };
}

it('disables New, Save, Save As and Load while robots are moving; there is no Close command', async () => {
  const test = await mount(undefined, null, true);
  expect(() => test.find((p) => p['aria-label'] === 'Close Scenario')).toThrow();
  for (const label of ['New', 'Save Scenario as', 'Load Scenario']) {
    expect(test.find((p) => p['aria-label'] === label).disabled).toBe(true);
  }
  const save = test.find((p) => typeof p['aria-label'] === 'string'
    && p['aria-label'].startsWith('Save Scenario') && p['aria-label'] !== 'Save Scenario as');
  expect(save.disabled).toBe(true);
});

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
  expect(test.primary('Load Scenario').disabled).toBe(false);
  await test.click(test.primary('Load Scenario'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  await test.ack();
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(active, test.controller.scenarioReplacementIdentity, expect.any(AbortSignal));
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  await test.click(test.find((p) => p.children === 'Cancel'));
  expect(test.find((p) => p.role === 'dialog')).toBeDefined();
  await act(() => close.resolve(empty));
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/other', true, test.controller.scenarioReplacementIdentity!.authoring);
  expect(() => test.find((p) => p.role === 'dialog')).toThrow();
});

it.each(['unknown', 'failure', 'not_empty'])('does not load a replacement after Close %s', async (kind) => {
  const close = vi.fn(async () => {
    if (kind === 'failure') throw new Error('process drain is unknown');
    return kind === 'unknown' ? null : active;
  });
  const test = await mount(close); await test.open('Load Scenario'); await test.choose('other');
  await test.click(test.primary('Load Scenario')); await test.ack();
  expect(close).toHaveBeenCalledOnce();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  expect(test.find((p) => p.role === 'dialog')).toBeDefined();
});

it('Load of the applied Scenario requires the same confirmation and waits for Close', async () => {
  const close = deferred<ManagerActiveProjection | null>();
  const test = await mount(vi.fn(() => close.promise));
  await test.open('Load Scenario'); await test.choose('current');
  expect(() => test.find((p) => p.type === 'checkbox')).toThrow();
  await test.click(test.primary('Load Scenario'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  await test.ack();
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(active, test.controller.scenarioReplacementIdentity, expect.any(AbortSignal));
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  await act(() => close.resolve(empty));
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/current', true, test.controller.scenarioReplacementIdentity!.authoring);
});

it('New asks no name: it confirms, closes first and then opens the Untitled Scenario', async () => {
  const close = deferred<ManagerActiveProjection | null>();
  const test = await mount(vi.fn(() => close.promise)); await test.open('New');
  expect(() => test.find((p) => p.placeholder === 'e.g. lite6_pick_and_place')).toThrow();
  expect(test.closeScenario).not.toHaveBeenCalled();
  await test.ack();
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(active, test.controller.scenarioReplacementIdentity, expect.any(AbortSignal));
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
  await act(() => close.resolve(empty));
  expect(test.controller.newScenarioDocument).toHaveBeenCalledExactlyOnceWith(true, test.controller.scenarioReplacementIdentity!.authoring);
  expect(() => test.find((p) => p.role === 'dialog')).toThrow();
});

it('SX-679: while Recover cell runs, New shows the reason and sends no Close', async () => {
  const reason = 'Recover cell is in progress; wait for it to finish or abort it.';
  const test = await mount(undefined, null, false, undefined, false, true, undefined, reason);
  await test.open('New');
  expect(test.find((p) => p.role === 'status' && p.children === reason)).toBeTruthy();
  expect(test.closeScenario).not.toHaveBeenCalled();
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
});

it('New on an empty cell without Draft changes closes at once, without a popup', async () => {
  apiMock.manager.status.mockResolvedValue({ source: { available: true, freshness: 'fresh' },
    status: { managerReady: true, active: empty } });
  const test = await mount(undefined, null, false, '/projects/current.scenario', false, false);
  await test.open('New');
  expect(() => test.find((p) => p.children === 'Close Scenario?')).toThrow();
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(empty, test.controller.scenarioReplacementIdentity, expect.any(AbortSignal));
  expect(test.controller.newScenarioDocument).toHaveBeenCalledExactlyOnceWith(true, test.controller.scenarioReplacementIdentity!.authoring);
});

it('keeps empty Load Close-first', async () => {
  apiMock.manager.status.mockResolvedValue({ source: { available: true, freshness: 'fresh' },
    status: { managerReady: true, active: empty } });
  const test = await mount();
  if (test.controller.state.workspace !== null) {
    test.controller.state.workspace.hasUnsavedChanges = false;
    test.controller.state.workspace.hasUnappliedChanges = false;
  }
  await test.open('Load Scenario'); await test.choose('other');
  await test.click(test.primary('Load Scenario'));
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(empty, test.controller.scenarioReplacementIdentity, expect.any(AbortSignal));
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/other', true, test.controller.scenarioReplacementIdentity!.authoring);
});

it.each(['current', 'other'])('Load of %s is admitted by the Close path, not by ordinary document admission', async (id) => {
  const test = await mount(undefined, 'Reset stopped operations first');
  await test.open('Load Scenario'); await test.choose(id);
  expect(test.primary('Load Scenario').disabled).toBe(false);
  await test.click(test.primary('Load Scenario')); await test.ack();
  expect(test.closeScenario).toHaveBeenCalledOnce();
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith(`/projects/${id}`, true, test.controller.scenarioReplacementIdentity!.authoring);
});

it('keeps the dialog after an unresolved replacement without sending it again', async () => {
  const test = await mount();
  vi.mocked(test.controller.loadScenarioDocument).mockResolvedValue(null);
  await test.open('Load Scenario'); await test.choose('other');
  await test.click(test.primary('Load Scenario')); await test.ack();
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledOnce();
  expect(test.find((p) => p.role === 'dialog')).toBeDefined();
});


it.each(['new', 'load'])('popup Cancel before %s sends no effects', async (kind) => {
  const test = await mount(); await test.open(kind === 'new' ? 'New' : 'Load Scenario');
  if (kind === 'load') {
    await test.choose('other');
    await test.click(test.primary('Load Scenario'));
  }
  expect(test.find((p) => p.children === 'Close Scenario?')).toBeDefined();
  expect(test.closeScenario).not.toHaveBeenCalled();
  await test.click(test.find((p) => p.children === 'Cancel'));
  expect(test.closeScenario).not.toHaveBeenCalled();
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  if (kind === 'new') expect(() => test.find((p) => p.role === 'dialog')).toThrow();
  else expect(test.primary('Load Scenario')).toBeDefined();
});

it.each(['new', 'load'])('keeps internal Close reachable for %s while stopped operations need Reset', async (kind) => {
  const close = deferred<ManagerActiveProjection | null>();
  const test = await mount(vi.fn(() => close.promise), 'Reset stopped operations first');
  await test.open(kind === 'new' ? 'New' : 'Load Scenario');
  if (kind === 'load') {
    await test.choose('other');
    expect(test.primary('Load Scenario').disabled).toBe(false);
    await test.click(test.primary('Load Scenario'));
  }
  await test.ack();
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(active, test.controller.scenarioReplacementIdentity, expect.any(AbortSignal));
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
});


it('keeps the popup bound to its captured Load path despite an intervening selection callback', async () => {
  const test = await mount(); await test.open('Load Scenario'); await test.choose('other');
  const selection = test.find((p) => p.value === 'saved:/projects/other');
  await test.click(test.primary('Load Scenario'));
  await act(() => selection.onChange?.({ target: { value: 'saved:/projects/current' } }));
  await test.ack();
  expect(test.closeScenario).toHaveBeenCalledExactlyOnceWith(active, test.controller.scenarioReplacementIdentity, expect.any(AbortSignal));
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/other', true, test.controller.scenarioReplacementIdentity!.authoring);
});

it('sends the authoring identity confirmed before Close to the Load request', async () => {
  const close = deferred<ManagerActiveProjection | null>();
  const test = await mount(vi.fn(() => close.promise));
  await test.open('Load Scenario'); await test.choose('other');
  await test.click(test.primary('Load Scenario')); await test.ack();
  const confirmed = structuredClone(test.controller.scenarioReplacementIdentity!.authoring);
  test.controller.scenarioReplacementIdentity = { ...test.controller.scenarioReplacementIdentity!,
    authoring: { ...confirmed, workspaceRevision: confirmed.workspaceRevision + 1 } };
  await act(() => close.resolve(empty));
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/other', true, confirmed);
});


it.each(['unknown', 'failure', 'not_empty'])('New keeps Close %s visible without opening Untitled', async (kind) => {
  const close = vi.fn(async () => {
    if (kind === 'failure') throw new Error('Process settlement failed');
    return kind === 'unknown' ? null : active;
  });
  const test = await mount(close); await test.open('New'); await test.ack();
  expect(close).toHaveBeenCalledExactlyOnceWith(active, test.controller.scenarioReplacementIdentity, expect.any(AbortSignal));
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  expect(test.find((p) => p.className === 'inline-message severity-error')).toBeDefined();
});

it('the read-only Untitled Scenario disables Save; Save As places it in the Scenario folder', async () => {
  const test = await mount(undefined, null, false, '/opt/manyforge/resources/Untitled.scenario', true);
  const save = test.find((p) => p['aria-label'] === 'Save Scenario');
  expect(save.disabled).toBe(true);
  expect(save['data-tooltip']).toBe('Save Scenario: The Untitled Scenario is read-only. Use Save Scenario as.');
  await test.open('Save Scenario as');
  const name = () => test.find((p) => p['aria-describedby'] === 'save-scenario-name-help');
  expect(name().value).toBe('Untitled');
  await act(() => name().onChange?.({ target: { value: 'Lab_Cell' } }));
  await test.click(test.primary('Save Scenario'));
  expect(test.controller.saveScenarioDocument).toHaveBeenCalledExactlyOnceWith({
    directory: '/projects', name: 'Lab_Cell', description: '', confirmOverwrite: false,
  });
});

it('the read-only Untitled Scenario has no portable copy until it is saved', async () => {
  const test = await mount(undefined, null, false, '/opt/manyforge/resources/Untitled.scenario', true);
  await test.open('Save Scenario as');
  await test.click(test.find((p) => p.children === 'Portable copy'));
  expect(test.find((p) => p.children === 'Save the Scenario as a project first.')).toBeDefined();
  expect(test.primary('Export copy').disabled).toBe(true);
});

it('Save As saves <directory>/<name>.scenario with one name and asks before replacing', async () => {
  apiMock.files.browse.mockResolvedValue({ entries: [{ name: 'Renamed.scenario', entryType: 'directory',
    path: '/projects/Renamed.scenario', selectable: false, scenarioProject: true }] });
  const test = await mount(undefined, null, false, '/projects/Lab_Cell.scenario');
  await test.open('Save Scenario as');
  const name = () => test.find((p) => p['aria-describedby'] === 'save-scenario-name-help');
  expect(name().value).toBe('Lab_Cell');
  await act(() => name().onChange?.({ target: { value: 'my cell' } }));
  expect(test.primary('Save Scenario').disabled).toBe(true);
  await act(() => name().onChange?.({ target: { value: 'Renamed' } }));
  // Another project already has this name: replacing it needs the explicit choice.
  expect(test.primary('Save Scenario').disabled).toBe(true);
  await test.confirm();
  expect(test.primary('Save Scenario').disabled).toBe(false);
  await test.click(test.primary('Save Scenario'));
  expect(test.controller.saveScenarioDocument).toHaveBeenCalledExactlyOnceWith({
    directory: '/projects', name: 'Renamed', description: '', confirmOverwrite: true,
  });
});

it('plain Save writes the open project under its folder name', async () => {
  const test = await mount(undefined, null, false, '/projects/Lab_Cell.scenario');
  await test.open('Save Scenario');
  expect(test.controller.saveScenarioDocument).toHaveBeenCalledExactlyOnceWith({
    directory: '/projects', name: 'Lab_Cell', description: '', confirmOverwrite: false,
  });
});

it('Available Scenario shows Scenario names and a refused folder by its folder name', async () => {
  apiMock.scenarioDocuments.list.mockResolvedValue({ directory: '/projects', entries: [
    { sourceKind: 'saved', path: '/projects/pick', scenarioId: null, name: 'pick',
      folderName: 'pick', location: 'projects', description: '', displayStatus: 'unsupported',
      statusDetail: 'a Scenario project must be a folder', savedManyforgeVersion: null },
    { sourceKind: 'saved', path: '/projects/pick.scenario', scenarioId: 'pick', name: 'pick',
      folderName: 'pick', location: 'projects', description: '', displayStatus: 'ready', statusDetail: '',
      savedManyforgeVersion: null },
  ] });
  const test = await mount(); await test.open('Load Scenario');
  const option = (path: string) => test.find((p) => p.value === `saved:${path}`);
  expect(option('/projects/pick.scenario').children).toBe('pick');
  expect(option('/projects/pick').children).toBe('pick');
  expect(option('/projects/pick').disabled).toBe(true);
});

const label = (p: Props) => [p.children].flat(3).filter((part) => typeof part === 'string').join('').trim();
function browseTree(tree: Record<string, Array<[string, boolean]>>, roots = ['/projects']) {
  apiMock.files.browse.mockImplementation(async (_kind: string, path?: string) => {
    const currentPath = path ?? roots[0];
    return { currentPath, parentPath: roots.includes(currentPath) ? null : currentPath.slice(0, currentPath.lastIndexOf('/')),
      roots, entries: (tree[currentPath] ?? []).map(([name, scenarioProject]) => ({
        name, path: `${currentPath}/${name}`, entryType: 'directory', selectable: false, scenarioProject })) };
  });
}

it('Load Browse… selects a Scenario project and loads it through Close', async () => {
  browseTree({ '/projects': [['cells', false]], '/projects/cells': [['lab', false], ['pick.scenario', true]] });
  apiMock.scenarioDocuments.list.mockImplementation(async (directory?: string) => directory === '/projects/cells'
    ? { directory: '/projects/cells', entries: [
      { sourceKind: 'saved', path: '/projects/cells/pick.scenario', scenarioId: 'pick', name: 'pick', folderName: 'pick',
        location: 'cells', description: '', displayStatus: 'ready', statusDetail: '', savedManyforgeVersion: null }] }
    : { directory: '/projects', entries: [] });
  const test = await mount(); await test.open('Load Scenario');
  await test.click(test.find((p) => p.children === 'Browse…'));
  expect(test.find((p) => p['aria-label'] === 'Choose a Scenario project')).toBeDefined();
  expect(() => test.find((p) => p.children === 'Use this folder')).toThrow();
  await test.click(test.find((p) => p.title === '/projects' && typeof p.onClick === 'function'));
  await test.click(test.find((p) => p.role === 'listitem' && label(p) === 'cells'));
  await test.click(test.find((p) => p.role === 'listitem' && label(p).startsWith('pick.scenario')));
  expect(apiMock.scenarioDocuments.list).toHaveBeenLastCalledWith('/projects/cells');
  expect(test.find((p) => p.value === 'saved:/projects/cells/pick.scenario')).toBeDefined();
  await test.click(test.primary('Load Scenario')); await test.ack();
  expect(test.closeScenario).toHaveBeenCalledOnce();
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/cells/pick.scenario', true, test.controller.scenarioReplacementIdentity!.authoring);
});

it('Load Browse… selects a project from another allowed root', async () => {
  const external = '/projects/user projects';
  browseTree({ '/projects': [], [external]: [['pick.scenario', true]] }, ['/projects', external]);
  apiMock.scenarioDocuments.list.mockImplementation(async (directory?: string) => directory === external
    ? { directory: external, entries: [
      { sourceKind: 'saved', path: `${external}/pick.scenario`, scenarioId: 'pick', name: 'pick', folderName: 'pick',
        location: external, description: '', displayStatus: 'ready', statusDetail: '', savedManyforgeVersion: null }] }
    : { directory: '/projects', entries: [] });
  const test = await mount(); await test.open('Load Scenario');
  await test.click(test.find((p) => p.children === 'Browse…'));
  await test.click(test.find((p) => p.title === external && typeof p.onClick === 'function'));
  await test.click(test.find((p) => p.role === 'listitem' && label(p).startsWith('pick.scenario')));
  expect(apiMock.scenarioDocuments.list).toHaveBeenLastCalledWith(external);
  await test.click(test.primary('Load Scenario')); await test.ack();
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith(`${external}/pick.scenario`, true, test.controller.scenarioReplacementIdentity!.authoring);
});

it('Export Browse… picks a parent folder and exports the project there under its name', async () => {
  browseTree({ '/projects': [['out', false], ['Lab_Cell.scenario', true]] });
  const test = await mount(undefined, null, false, '/projects/Lab_Cell.scenario');
  await test.open('Save Scenario as');
  await test.click(test.find((p) => p.children === 'Portable copy'));
  expect(test.primary('Export copy').disabled).toBe(true);
  await test.click(test.find((p) => p.children === 'Browse…'));
  await test.click(test.find((p) => p.title === '/projects' && typeof p.onClick === 'function'));
  // The open project's own folder already holds it: that destination is refused.
  await test.click(test.find((p) => p.children === 'Use this folder'));
  expect(test.primary('Export copy').disabled).toBe(true);
  await test.click(test.find((p) => p.children === 'Browse…'));
  await test.click(test.find((p) => p.title === '/projects' && typeof p.onClick === 'function'));
  expect(test.find((p) => p.role === 'listitem' && label(p) === 'Lab_Cell.scenario').disabled).toBe(true);
  await test.click(test.find((p) => p.role === 'listitem' && label(p) === 'out'));
  await test.click(test.find((p) => p.children === 'Use this folder'));
  expect(test.primary('Export copy').disabled).toBe(false);
  await test.click(test.primary('Export copy'));
  expect(test.controller.exportScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/out/Lab_Cell.scenario');
});

it('Export Browse… can write to another allowed root', async () => {
  const external = '/projects/user projects';
  browseTree({ '/projects': [], [external]: [] }, ['/projects', external]);
  const test = await mount(undefined, null, false, '/projects/Lab_Cell.scenario');
  await test.open('Save Scenario as');
  await test.click(test.find((p) => p.children === 'Portable copy'));
  await test.click(test.find((p) => p.children === 'Browse…'));
  await test.click(test.find((p) => p.title === external && typeof p.onClick === 'function'));
  await test.click(test.find((p) => p.children === 'Use this folder'));
  await test.click(test.primary('Export copy'));
  expect(test.controller.exportScenarioDocument).toHaveBeenCalledExactlyOnceWith(`${external}/Lab_Cell.scenario`);
});

// Item 37: a refused Close offers Force close; one confirmation states the
// risks, then the same replacement continues after the forced Close.
it('a refused Load Close offers Force close, which confirms the risks and then loads', async () => {
  const refused = vi.fn(async () => { throw Object.assign(new Error('Close cannot infer rest from an uncertain terminal'),
    { code: 'cell_close_containment_uncertain' }); });
  const forced = vi.fn(async () => empty as ManagerActiveProjection | null);
  const test = await mount(refused, null, false, '/projects/current.scenario', false, true, forced);
  expect(() => test.find((p) => p.children === 'Force close…')).toThrow();
  await test.open('Load Scenario'); await test.choose('other');
  await test.click(test.primary('Load Scenario')); await test.ack();
  expect(refused).toHaveBeenCalledOnce();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  await test.click(test.find((p) => p.children === 'Force close…'));
  expect(test.find((p) => p.children === 'Force close Scenario?')).toBeDefined();
  expect(forced).not.toHaveBeenCalled();
  await test.click(test.primary('Force close and continue'));
  expect(forced).toHaveBeenCalledExactlyOnceWith(active, test.controller.scenarioReplacementIdentity, expect.any(AbortSignal));
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/other', true, test.controller.scenarioReplacementIdentity!.authoring);
});

it('Cancel on the Force close confirmation sends nothing', async () => {
  const refused = vi.fn(async () => { throw new Error('Close refused'); });
  const forced = vi.fn(async () => empty as ManagerActiveProjection | null);
  const test = await mount(refused, null, false, '/projects/current.scenario', false, true, forced);
  await test.open('New'); await test.ack();
  await test.click(test.find((p) => p.children === 'Force close…'));
  const dialogs = (test.find((p) => p.children === 'Force close Scenario?'));
  expect(dialogs).toBeDefined();
  const cancel = test.find((p) => p.children === 'Cancel' && p.className === undefined && p.onClick !== undefined);
  await test.click(cancel);
  expect(forced).not.toHaveBeenCalled();
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
});
