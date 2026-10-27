import { act } from 'react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';

import type { FileBrowserResponse } from '../types/api';
import { FolderPicker } from './FolderPicker';

const apiMock = vi.hoisted(() => ({ files: { browse: vi.fn() } }));
vi.mock('../api/client', () => ({ api: apiMock }));

// The same minimal mounted host as the Scenario command tests: real React
// handlers, state and effects over a tiny fake DOM.
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
      style: {}, children: [], parentNode: null,
      addEventListener: () => undefined, removeEventListener: () => undefined,
      setAttribute: () => undefined, removeAttribute: () => undefined,
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
  const window = { document, HTMLIFrameElement: class {} };
  document.defaultView = window;
  vi.stubGlobal('window', window); vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const host = node('div');
  const props = (element: FakeNode) => element[
    Object.keys(element).find((key) => key.startsWith('__reactProps$'))!
  ] as Props | undefined;
  const walk = (element: FakeNode): FakeNode[] => [element, ...element.children.flatMap(walk)];
  const all = () => walk(host).map(props).filter((p): p is Props => p !== undefined);
  const find = (match: (p: Props) => boolean) => {
    const value = all().find(match);
    if (value === undefined) throw new Error('Folder picker control not found');
    return value;
  };
  return { host, find, all };
}

const text = (p: Props) => [p.children].flat(3).filter((part) => typeof part === 'string').join('');

function listing(currentPath: string, parentPath: string | null, folders: Array<[string, boolean]>): FileBrowserResponse {
  return {
    currentPath, parentPath, roots: ['/root', '/user/projects'],
    entries: folders.map(([name, scenarioProject]) => ({
      name, path: `${currentPath}/${name}`, entryType: 'directory', selectable: false, scenarioProject,
    })),
  };
}

const tree: Record<string, FileBrowserResponse> = {
  '/root': listing('/root', null, [['cells', false]]),
  '/root/cells': listing('/root/cells', '/root', [['lab', false], ['pick.scenario', true]]),
  '/root/cells/lab': listing('/root/cells/lab', '/root/cells', []),
  '/root/cells/pick.scenario': listing('/root/cells/pick.scenario', '/root/cells', []),
  '/user/projects': listing('/user/projects', null, [['other.scenario', true]]),
};

let unmount: (() => Promise<void>) | null = null;
afterEach(async () => { await unmount?.(); unmount = null; vi.unstubAllGlobals(); });
beforeEach(() => {
  vi.clearAllMocks();
  const storage = new Map<string, string>();
  vi.stubGlobal('sessionStorage', {
    getItem: (key: string) => storage.get(key) ?? null,
    setItem: (key: string, value: string) => { storage.set(key, value); },
  });
  apiMock.files.browse.mockImplementation(async (_kind: string, path?: string) => tree[path ?? '/root']);
});

async function mount(chooseProject = false) {
  const dom = mountedHost();
  const onChoose = vi.fn();
  const onCancel = vi.fn();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(dom.host as unknown as Element);
  await act(async () => root.render(<FolderPicker onChoose={onChoose} onCancel={onCancel} chooseProject={chooseProject} />));
  unmount = async () => { await act(() => root.unmount()); };
  const click = async (p: Props) => { await act(async () => p.onClick?.({})); };
  const button = (label: string) => dom.find((p) => text(p).includes(label) && typeof p.onClick === 'function'
    || (text(p).includes(label) && p.disabled === true));
  const rootButton = (path: string) => dom.find((p) => p.title === path && typeof p.onClick === 'function');
  const typePath = async (path: string) => {
    await act(async () => dom.find((p) => p['aria-label'] === 'Folder path').onChange?.({ target: { value: path } }));
  };
  return { ...dom, onChoose, onCancel, click, button, rootButton, typePath };
}

it('starts with the allowed roots before browsing any folder', async () => {
  const test = await mount();
  expect(apiMock.files.browse).toHaveBeenCalledWith('scenario', undefined, expect.any(AbortSignal), true);
  expect(test.find((p) => p['aria-label'] === 'Allowed roots')).toBeDefined();
  expect(text(test.rootButton('/root'))).toBe('root');
  expect(text(test.rootButton('/user/projects'))).toBe('projects');
  expect(() => test.find((p) => p.role === 'listitem')).toThrow();
  expect(() => test.find((p) => text(p) === 'Use this folder')).toThrow();
});

it('explains project folder locations when no host mappings are available', async () => {
  const test = await mount();
  await test.click(test.find((p) => p['aria-label'] === 'Project folder information'));
  const help = text(test.find((p) => p.role === 'tooltip'));
  expect(help).toContain('bundled Examples');
  expect(help).toContain('project_folders in the site settings file');
  expect(help).toContain('/projects/<folder name>');
  expect(help).toContain('Stop and Start');
  expect(help).toContain('Pasting a host path is not supported');
});

it('shows actual host-to-container pairs when the backend supplies them', async () => {
  apiMock.files.browse.mockImplementation(async (_kind: string, path?: string) => ({
    ...tree[path ?? '/root'],
    projectRoots: [{ hostPath: '/home/user/robot projects', containerPath: '/projects/robot projects' }],
  }));
  const test = await mount();
  await test.click(test.find((p) => p['aria-label'] === 'Project folder information'));
  const help = text(test.find((p) => p.role === 'tooltip'));
  expect(help).toContain('/home/user/robot projects → /projects/robot projects');
  expect(help).not.toContain('See project_folders in the site settings file');
});

it('forgets host mappings when a later browse response omits them', async () => {
  apiMock.files.browse.mockImplementation(async (_kind: string, path?: string) => (
    path === undefined
      ? { ...tree['/root'], projectRoots: [{ hostPath: '/home/user/cells', containerPath: '/root' }] }
      : tree[path]
  ));
  const test = await mount();
  await test.click(test.find((p) => p['aria-label'] === 'Project folder information'));
  expect(text(test.find((p) => p.role === 'tooltip'))).toContain('/home/user/cells → /root');
  await test.click(test.rootButton('/root'));
  const help = text(test.find((p) => p.role === 'tooltip'));
  expect(help).toContain('See project_folders in the site settings file');
  expect(help).not.toContain('/home/user/cells');
});

it('navigates down and up within the roots and chooses the current folder', async () => {
  const test = await mount();
  await test.click(test.rootButton('/root'));
  expect(test.find((p) => p.title === 'Up one folder').disabled).toBe(true);
  await test.click(test.button('cells'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root/cells', expect.any(AbortSignal), false);
  await test.click(test.button('lab'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root/cells/lab', expect.any(AbortSignal), false);
  await test.click(test.find((p) => p.title === 'Up one folder'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root/cells', expect.any(AbortSignal), false);
  await test.click(test.rootButton('/root'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root', expect.any(AbortSignal), false);
  await test.click(test.button('cells'));
  await test.click(test.button('Use this folder'));
  expect(test.onChoose).toHaveBeenCalledExactlyOnceWith('/root/cells');
});

it('shows Scenario projects without entering them', async () => {
  const test = await mount();
  await test.click(test.rootButton('/root'));
  await test.click(test.button('cells'));
  const project = test.find((p) => p.title === 'Scenario project');
  expect(project.disabled).toBe(true);
  expect(project.onClick).toBeUndefined();
});

it('refuses to choose a folder inside a Scenario project', async () => {
  apiMock.files.browse.mockImplementation(async (_kind: string, path?: string) => ({
    ...tree[path ?? '/root'], roots: ['/root', '/root/cells/pick.scenario'],
  }));
  const test = await mount();
  await test.click(test.rootButton('/root/cells/pick.scenario'));
  expect(test.find((p) => text(p) === 'Use this folder').disabled).toBe(true);
  expect(test.find((p) => p.role === 'status')).toBeDefined();
  await test.click(test.button('Cancel'));
  expect(test.onCancel).toHaveBeenCalledOnce();
  expect(test.onChoose).not.toHaveBeenCalled();
});

it('in project mode chooses a Scenario project and only navigates ordinary folders', async () => {
  const test = await mount(true);
  expect(test.find((p) => p['aria-label'] === 'Choose a Scenario project')).toBeDefined();
  expect(() => test.find((p) => text(p) === 'Use this folder')).toThrow();
  await test.click(test.rootButton('/root'));
  await test.click(test.button('cells'));
  await test.click(test.button('lab'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root/cells/lab', expect.any(AbortSignal), false);
  expect(test.onChoose).not.toHaveBeenCalled();
  await test.click(test.find((p) => p.title === 'Up one folder'));
  const project = test.find((p) => p.title === 'Choose this Scenario project');
  expect(project.disabled).toBe(false);
  await test.click(project);
  expect(test.onChoose).toHaveBeenCalledExactlyOnceWith('/root/cells/pick.scenario');
});

it('can choose a project from a second allowed root', async () => {
  const test = await mount(true);
  await test.click(test.rootButton('/user/projects'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/user/projects', expect.any(AbortSignal), false);
  await test.click(test.button('other.scenario'));
  expect(test.onChoose).toHaveBeenCalledExactlyOnceWith('/user/projects/other.scenario');
});

it('accepts a relative path and remembers the last folder for this tab session', async () => {
  const test = await mount();
  await test.click(test.rootButton('/root'));
  await test.typePath('cells');
  await test.click(test.button('Go'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root/cells', expect.any(AbortSignal), false);
  await test.click(test.button('Locations'));
  expect(() => test.find((p) => p.role === 'listitem')).toThrow();
  await test.click(test.button('Last folder'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root/cells', expect.any(AbortSignal), false);
  await unmount?.();
  unmount = null;
  const reopened = await mount();
  const lastFolder = reopened.button('Last folder');
  expect(lastFolder.title).toBe('/root/cells');
  await reopened.click(lastFolder);
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root/cells', expect.any(AbortSignal), false);
});

it('accepts a pasted Scenario project path and offers one explicit selection', async () => {
  const test = await mount(true);
  await test.typePath('/root/cells/pick.scenario');
  await test.click(test.button('Go'));
  await test.click(test.button('Select this Scenario'));
  expect(test.onChoose).toHaveBeenCalledExactlyOnceWith('/root/cells/pick.scenario');
});

it('shows a path error while keeping configured locations available', async () => {
  apiMock.files.browse.mockImplementation(async (_kind: string, path?: string) => {
    if (path === '/outside') throw new Error('Path is outside allowed roots');
    return tree[path ?? '/root'];
  });
  const test = await mount();
  await test.typePath('/outside');
  await test.click(test.button('Go'));
  expect(text(test.find((p) => p.role === 'alert'))).toContain('Path is outside allowed roots');
  await test.click(test.button('About project folders'));
  expect(text(test.find((p) => p.role === 'tooltip'))).toContain('Pasting a host path is not supported');
  await test.click(test.rootButton('/root'));
  expect(test.find((p) => p.title === 'Up one folder').disabled).toBe(true);
});

it('shows an unreadable-folder error and lets the user return to locations', async () => {
  apiMock.files.browse.mockImplementation(async (_kind: string, path?: string) => {
    if (path === '/root/private') throw new Error('Permission denied');
    return tree[path ?? '/root'];
  });
  const test = await mount();
  await test.typePath('/root/private');
  await test.click(test.button('Go'));
  expect(text(test.find((p) => p.role === 'alert'))).toContain('Permission denied');
  await test.click(test.button('Locations'));
  expect(test.rootButton('/root')).toBeDefined();
});
