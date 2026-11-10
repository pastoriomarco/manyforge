import { act } from 'react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import { authoringOperation, type BusyOperation } from '../state/busyOperation';
import { ScenarioDocumentCommands } from './ScenarioDocumentCommands';

const apiMock = vi.hoisted(() => ({
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
function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((done) => { resolve = done; });
  return { promise, resolve };
}
let unmount: (() => Promise<void>) | null = null;
afterEach(async () => { await unmount?.(); unmount = null; vi.unstubAllGlobals(); });
beforeEach(() => {
  vi.clearAllMocks();
  apiMock.files.browse.mockResolvedValue({ entries: [] });
  apiMock.scenarioDocuments.list.mockResolvedValue({ directory: '/projects', entries: [
    { sourceKind: 'saved', path: '/projects/other', scenarioId: 'other', name: 'Other', folderName: 'other',
      location: 'projects', description: '', displayStatus: 'ready', statusDetail: '', savedManyforgeVersion: null },
    { sourceKind: 'saved', path: '/projects/current', scenarioId: 'current', name: 'Current', folderName: 'current',
      location: 'projects', description: '', displayStatus: 'ready', statusDetail: '', savedManyforgeVersion: null },
  ] });
});
async function mount(documentUnavailableReason: string | null = null, motionActive = false,
  documentPath = '/projects/current.scenario', readOnly = false, hasChanges = true,
  cellComponents: readonly string[] | null = [], scenarioReplacementUnavailableReason: string | null = null,
  busyOperation: BusyOperation | null = null, authoringAdmissionAllowed: boolean | null = null) {
  const dom = mountedHost();
  const controller = {
    busyOperation,
    documentUnavailableReason, scenarioReplacementUnavailableReason, state: { status: 'ready',
      pendingAction: busyOperation === null ? null : 'save_scenario_document', error: null, workspace: {
      authoringWorkspace: { revision: 1 }, draft: { authoring: null }, hasUnsavedChanges: hasChanges,
      hasUnappliedChanges: hasChanges } }, scenarioDocumentPath: documentPath,
    scenarioDocument: { id: readOnly ? 'Untitled' : 'current', name: 'Current', description: '', readOnly },
    saveScenarioDocument: vi.fn(), exportScenarioDocument: vi.fn(),
    newScenarioDocument: vi.fn(async () => ({})), loadScenarioDocument: vi.fn(async () => ({})),
  } as unknown as ProgramWorkspaceController;
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(dom.host as unknown as Element);
  let admissionAllowed = authoringAdmissionAllowed;
  const render = async () => act(() => root.render(<ScenarioDocumentCommands controller={controller}
    motionActive={motionActive} cellComponents={cellComponents}
    authoringAdmissionAllowed={admissionAllowed} />));
  await render();
  unmount = async () => { await act(() => root.unmount()); };
  const rerenderAdmission = async (allowed: boolean | null) => { admissionAllowed = allowed; await render(); };
  const click = async (props: Props) => { await act(async () => props.onClick?.({ currentTarget: { focus: () => undefined } })); };
  const open = async (label: string) => click(dom.find((p) => p['aria-label'] === label));
  const confirm = async () => { await act(() => dom.find((p) => p.type === 'checkbox').onChange?.({ target: { checked: true } })); };
  const ack = async () => click(dom.find((p) => p.className === 'danger-button'));
  const choose = async (id: string) => { await act(() => dom.find((p) => p.children instanceof Array
    && (p.children as Array<unknown>).some((c) => (c as { props?: Props })?.props?.children === 'Select a Scenario'))
    .onChange?.({ target: { value: `saved:/projects/${id}` } })); };
  // Confirmations use the danger look, the primary look elsewhere.
  const primary = (label: string) => dom.find((p) => (p.className === 'primary-action' || p.className === 'danger-button') && p.children === label);
  return { ...dom, controller, click, open, confirm, choose, primary, ack, rerenderAdmission };
}

it('disables New, Save, Save As and Load while robots are moving', async () => {
  const test = await mount(null, true);
  for (const label of ['New', 'Save Scenario as', 'Load Scenario']) {
    expect(test.find((p) => p['aria-label'] === label).disabled).toBe(true);
  }
});

it('Cancel on the discard confirmation sends no New or Load', async () => {
  const test = await mount(); await test.open('New');
  expect(test.find((p) => p.children === 'New Scenario?')).toBeDefined();
  await test.click(test.find((p) => p.children === 'Cancel'));
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  expect(() => test.find((p) => p.role === 'dialog')).toThrow();
});

it('Load with Draft changes confirms the discard, then calls the server Load once', async () => {
  const load = deferred<unknown>();
  const test = await mount();
  vi.mocked(test.controller.loadScenarioDocument).mockImplementation(() => load.promise as never);
  await test.open('Load Scenario'); await test.choose('other');
  await test.click(test.primary('Load Scenario'));
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  await test.ack();
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/other', expect.any(Function));
  await act(async () => load.resolve({}));
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledOnce();
  expect(() => test.find((p) => p.role === 'dialog')).toThrow();
});

it('Load without Draft changes calls the server Load at once', async () => {
  const test = await mount(null, false, '/projects/current.scenario', false, false);
  await test.open('Load Scenario'); await test.choose('current');
  await test.click(test.primary('Load Scenario'));
  expect(() => test.find((p) => p.children === 'Load Scenario?')).toThrow();
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/current', expect.any(Function));
});

it('New without Draft changes calls the server New once, without a popup', async () => {
  const test = await mount(null, false, '/projects/current.scenario', false, false);
  await test.open('New');
  expect(() => test.find((p) => p.children === 'New Scenario?')).toThrow();
  expect(test.controller.newScenarioDocument).toHaveBeenCalledOnce();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
});

it('New with Draft changes asks once, then calls the server New once', async () => {
  const test = await mount(); await test.open('New');
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
  await test.ack();
  expect(test.controller.newScenarioDocument).toHaveBeenCalledOnce();
});

it('keeps the dialog with the refusal after a refused Load and does not resend it', async () => {
  const test = await mount();
  vi.mocked(test.controller.loadScenarioDocument).mockResolvedValue(null);
  await test.open('Load Scenario'); await test.choose('other');
  await test.click(test.primary('Load Scenario')); await test.ack();
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledOnce();
  expect(test.find((p) => p.role === 'dialog')).toBeDefined();
  expect(test.find((p) => p.className === 'inline-message severity-error')).toBeDefined();
});

it('a failed Load offers Retry Load and Load empty scenario; neither runs by itself', async () => {
  const test = await mount();
  vi.mocked(test.controller.loadScenarioDocument).mockImplementation(async (_path, onRefusal) => {
    onRefusal?.('Scenario Load: Apply failed: kernel_apply_fence_request_failed.', true);
    return null;
  });
  await test.open('Load Scenario'); await test.choose('other');
  await test.click(test.primary('Load Scenario')); await test.ack();
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledOnce();
  expect(test.find((p) => p.className === 'inline-message severity-error')).toBeDefined();
  const empty = test.find((p) => p.type === 'button' && p.children === 'Load empty scenario');
  expect(test.primary('Retry Load')).toBeDefined();
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();

  await test.click(test.primary('Retry Load')); await test.ack();
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledTimes(2);
  expect(vi.mocked(test.controller.loadScenarioDocument).mock.calls[1]![0]).toBe('/projects/other');

  await test.click(test.find((p) => p.type === 'button' && p.children === 'Load empty scenario'));
  expect(empty).toBeDefined();
  await test.ack();
  expect(test.controller.newScenarioDocument).toHaveBeenCalledOnce();
});

it('a refused Load offers no Retry Load', async () => {
  const test = await mount();
  vi.mocked(test.controller.loadScenarioDocument).mockImplementation(async (_path, onRefusal) => {
    onRefusal?.('Scenario Load refused: authoring_not_ready.');
    return null;
  });
  await test.open('Load Scenario'); await test.choose('other');
  await test.click(test.primary('Load Scenario')); await test.ack();
  expect(test.primary('Load Scenario')).toBeDefined();
  expect(() => test.find((p) => p.children === 'Load empty scenario')).toThrow();
});

it('the read-only Untitled Scenario disables Save; Save As places it in the Scenario folder', async () => {
  const test = await mount(null, false, '/opt/manyforge/resources/Untitled.scenario', true);
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
  const test = await mount(null, false, '/opt/manyforge/resources/Untitled.scenario', true);
  await test.open('Save Scenario as');
  await test.click(test.find((p) => p.children === 'Portable copy'));
  expect(test.find((p) => p.children === 'Save the Scenario as a project first.')).toBeDefined();
  expect(test.primary('Export copy').disabled).toBe(true);
});

it('Save As saves <directory>/<name>.scenario with one name and asks before replacing', async () => {
  apiMock.files.browse.mockResolvedValue({ entries: [{ name: 'Renamed.scenario', entryType: 'directory',
    path: '/projects/Renamed.scenario', selectable: false, scenarioProject: true }] });
  const test = await mount(null, false, '/projects/Lab_Cell.scenario');
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
  const test = await mount(null, false, '/projects/Lab_Cell.scenario');
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

it('Load Browse… selects a Scenario project and loads it', async () => {
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
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/cells/pick.scenario', expect.any(Function));
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
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith(`${external}/pick.scenario`, expect.any(Function));
});

it('Export Browse… picks a parent folder and exports the project there under its name', async () => {
  browseTree({ '/projects': [['out', false], ['Lab_Cell.scenario', true]] });
  const test = await mount(null, false, '/projects/Lab_Cell.scenario');
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
  const test = await mount(null, false, '/projects/Lab_Cell.scenario');
  await test.open('Save Scenario as');
  await test.click(test.find((p) => p.children === 'Portable copy'));
  await test.click(test.find((p) => p.children === 'Browse…'));
  await test.click(test.find((p) => p.title === external && typeof p.onClick === 'function'));
  await test.click(test.find((p) => p.children === 'Use this folder'));
  await test.click(test.primary('Export copy'));
  expect(test.controller.exportScenarioDocument).toHaveBeenCalledExactlyOnceWith(`${external}/Lab_Cell.scenario`);
});


it('New of a non-empty cell confirms once, then calls the server New once', async () => {
  const test = await mount(null, false, '/projects/current.scenario', false, false, ['lite6']);
  await test.open('New');
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
  expect(test.find((p) => p.children === 'New Scenario?')).toBeDefined();
  await test.ack();
  expect(test.controller.newScenarioDocument).toHaveBeenCalledOnce();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
});

it('shows a refused New and does not resend it', async () => {
  const test = await mount(null, false, '/projects/current.scenario', false, false, ['lite6']);
  vi.mocked(test.controller.newScenarioDocument).mockResolvedValue(null);
  await test.open('New'); await test.ack();
  expect(test.controller.newScenarioDocument).toHaveBeenCalledOnce();
  expect(test.find((p) => p.children === 'New was refused; nothing was retried.')).toBeDefined();
});

// New and Load ask once when the cell isn't empty (one dialog).
const listsComponents = (text: string) => (p: Props) => Array.isArray(p.children)
  && (p.children as unknown[]).includes(text);
const discardLine = (p: Props) => p.children === 'Current unsaved and unapplied Draft changes will be discarded.';

it('New of a non-empty cell lists what gets stopped, without the discard line when nothing is unsaved',
  async () => {
    const test = await mount(null, false, '/projects/current.scenario', false, false, ['lite6', 'camera']);
    await test.open('New');
    expect(test.find((p) => p.children === 'New Scenario?')).toBeDefined();
    expect(test.find(listsComponents(': lite6, camera.'))).toBeDefined();
    expect(() => test.find(discardLine)).toThrow();
    expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
  });

it('Load of a non-empty cell asks once with the components and the discard line, then loads once', async () => {
  const test = await mount(null, false, '/projects/current.scenario', false, true, ['lite6']);
  await test.open('Load Scenario'); await test.choose('other');
  await test.click(test.primary('Load Scenario'));
  expect(test.find((p) => p.children === 'Load Scenario?')).toBeDefined();
  expect(test.find(listsComponents(': lite6.'))).toBeDefined();
  expect(test.find(discardLine)).toBeDefined();
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
  await test.ack();
  expect(test.controller.loadScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/other', expect.any(Function));
});

it('asks when the cell\'s components cannot be read', async () => {
  const test = await mount(null, false, '/projects/current.scenario', false, false, null);
  await test.open('New');
  expect(test.find((p) => p.children === 'New Scenario?')).toBeDefined();
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
});

it('New and Load follow their own gate, not the Idle-only document gate', async () => {
  // A finished run blocks Save, not New or Load (the server Resets it).
  const finished = await mount('Program execution is terminal awaiting reset; authoring requires Idle.');
  for (const label of ['New', 'Load Scenario']) {
    expect(finished.find((p) => p['aria-label'] === label).disabled).toBe(false);
  }
  expect(finished.find((p) => p['aria-label'] === 'Save Scenario as').disabled).toBe(true);
  await unmount?.(); unmount = null;
  const locked = await mount(null, false, '/projects/current.scenario', false, true, [],
    'Authoring is currently locked by the backend.');
  for (const label of ['New', 'Load Scenario']) {
    const button = locked.find((p) => p['aria-label'] === label);
    expect(button.disabled).toBe(true);
    expect(button['data-tooltip']).toBe(`${label}: Authoring is currently locked by the backend.`);
  }
});

it('gates Scenario writes while admission is false and leaves portable export available', async () => {
  browseTree({ '/projects': [['out', false], ['current.scenario', true]] });
  const blocked = await mount(null, false, '/projects/current.scenario', false, true, [], null, null, false);
  const tooltip = 'The cell is finishing a background step; this is available again in a moment.';
  for (const label of ['New', 'Save Scenario', 'Load Scenario']) {
    const button = blocked.find((p) => p['aria-label'] === label);
    expect(button.disabled).toBe(true);
    expect(button['data-tooltip']).toBe(tooltip);
  }
  const saveAsEntry = blocked.find((p) => p['aria-label'] === 'Save Scenario as');
  expect(saveAsEntry.disabled).toBe(false);
  expect(saveAsEntry['data-tooltip']).toBe('Save Scenario as');
  await blocked.open('Save Scenario as');
  expect(blocked.primary('Save Scenario').disabled).toBe(true);
  expect(blocked.primary('Save Scenario').title).toBe(tooltip);
  expect(blocked.find((p) => p.role === 'status' && p.className === 'inline-message severity-info').children)
    .toEqual([
      'Project Save is unavailable. ', tooltip,
      ' Choose Portable copy to export without saving to the project.',
    ]);
  await blocked.click(blocked.find((p) => p.children === 'Portable copy'));
  await blocked.click(blocked.find((p) => p.children === 'Browse…'));
  await blocked.click(blocked.find((p) => p.title === '/projects' && typeof p.onClick === 'function'));
  await blocked.click(blocked.find((p) => p.role === 'listitem' && label(p) === 'out'));
  await blocked.click(blocked.find((p) => p.children === 'Use this folder'));
  expect(blocked.primary('Export copy').disabled).toBe(false);
  await blocked.click(blocked.primary('Export copy'));
  expect(blocked.controller.exportScenarioDocument).toHaveBeenCalledExactlyOnceWith('/projects/out/current.scenario');
  await unmount?.(); unmount = null;

  const unknown = await mount(null, false, '/projects/current.scenario', false, true, [], null, null, null);
  expect(unknown.find((p) => p['aria-label'] === 'New').disabled).toBe(false);
  expect(unknown.find((p) => p['aria-label'] === 'Save Scenario').disabled).toBe(false);
  expect(unknown.find((p) => p['aria-label'] === 'Load Scenario').disabled).toBe(false);
});

it('disables a New confirmation if authoring admission closes while it is open', async () => {
  const test = await mount(null, false, '/projects/current.scenario', false, true, [], null, null, true);
  await test.open('New');
  expect(test.find((p) => p.children === 'New Scenario?')).toBeDefined();
  await test.rerenderAdmission(false);
  const acknowledge = test.find((p) => p.className === 'danger-button');
  expect(acknowledge.disabled).toBe(true);
  expect(acknowledge.title).toBe('The cell is finishing a background step; this is available again in a moment.');
  expect(test.controller.newScenarioDocument).not.toHaveBeenCalled();
});

it('disables a Load confirmation if authoring admission closes while it is open', async () => {
  const test = await mount(null, false, '/projects/current.scenario', false, true, [], null, null, true);
  await test.open('Load Scenario');
  await test.choose('other');
  await test.click(test.primary('Load Scenario'));
  expect(test.find((p) => p.children === 'Load Scenario?')).toBeDefined();
  await test.rerenderAdmission(false);
  const acknowledge = test.find((p) => p.className === 'danger-button');
  expect(acknowledge.disabled).toBe(true);
  expect(acknowledge.title).toBe('The cell is finishing a background step; this is available again in a moment.');
  expect(test.controller.loadScenarioDocument).not.toHaveBeenCalled();
});

it('a refused Load shows its reason, not the generic line', async () => {
  const test = await mount(null, false, '/projects/current.scenario', false, false);
  vi.mocked(test.controller.loadScenarioDocument).mockImplementation(async (_path, onRefusal) => {
    onRefusal?.('Scenario Load refused: A Program run is in progress; Reset it before Apply');
    return null;
  });
  await test.open('Load Scenario'); await test.choose('other');
  await test.click(test.primary('Load Scenario'));
  expect(test.find((p) => p.children === 'Scenario Load refused: A Program run is in progress; Reset it before Apply'))
    .toBeDefined();
  expect(() => test.find((p) => typeof p.children === 'string' && p.children.includes('was refused; nothing was retried')))
    .toThrow();
});

it('a running Save shows its busy label and names itself on every other Scenario command', async () => {
  const test = await mount(null, false, '/projects/current.scenario', false, true, [], null,
    authoringOperation('save'));
  const save = test.find((p) => p['aria-label'] === 'Save Scenario');
  expect(save['data-tooltip']).toBe('Saving…');
  expect(save['aria-busy']).toBe(true);
  expect(save.disabled).toBe(true);
  for (const label of ['New', 'Save Scenario as', 'Load Scenario']) {
    const other = test.find((p) => p['aria-label'] === label);
    expect(other.disabled).toBe(true);
    expect(other['data-tooltip']).toBe(`${label}: Save is in progress.`);
  }
});
