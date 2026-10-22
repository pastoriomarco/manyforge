import { act } from 'react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';

import type { FileBrowserResponse } from '../types/api';
import { FolderPicker } from './FolderPicker';

const apiMock = vi.hoisted(() => ({ files: { browse: vi.fn() } }));
vi.mock('../api/client', () => ({ api: apiMock }));

// The same minimal mounted host as the Scenario command tests: real React
// handlers, state and effects over a tiny fake DOM.
type Props = Record<string, unknown> & { onClick?: (event: unknown) => void };
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
    currentPath, parentPath, roots: ['/root'],
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
};

let unmount: (() => Promise<void>) | null = null;
afterEach(async () => { await unmount?.(); unmount = null; vi.unstubAllGlobals(); });
beforeEach(() => {
  vi.clearAllMocks();
  apiMock.files.browse.mockImplementation(async (_kind: string, path?: string) => tree[path ?? '/root']);
});

async function mount(initialPath: string | null, chooseProject = false) {
  const dom = mountedHost();
  const onChoose = vi.fn();
  const onCancel = vi.fn();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(dom.host as unknown as Element);
  await act(async () => root.render(<FolderPicker initialPath={initialPath} onChoose={onChoose} onCancel={onCancel} chooseProject={chooseProject} />));
  unmount = async () => { await act(() => root.unmount()); };
  const click = async (p: Props) => { await act(async () => p.onClick?.({})); };
  const button = (label: string) => dom.find((p) => text(p).includes(label) && typeof p.onClick === 'function'
    || (text(p).includes(label) && p.disabled === true));
  return { ...dom, onChoose, onCancel, click, button };
}

it('starts at the first allowed root and lists folders as Scenario-only browses', async () => {
  const test = await mount(null);
  expect(apiMock.files.browse).toHaveBeenCalledWith('scenario', undefined, expect.any(AbortSignal));
  expect(test.find((p) => p.title === 'Up one folder').disabled).toBe(true);
  expect(test.find((p) => p['aria-label'] === 'Allowed roots')).toBeDefined();
});

it('navigates down and up within the roots and chooses the current folder', async () => {
  const test = await mount(null);
  await test.click(test.button('cells'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root/cells', expect.any(AbortSignal));
  await test.click(test.button('lab'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root/cells/lab', expect.any(AbortSignal));
  await test.click(test.find((p) => p.title === 'Up one folder'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root/cells', expect.any(AbortSignal));
  await test.click(test.button('/root'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root', expect.any(AbortSignal));
  await test.click(test.button('cells'));
  await test.click(test.button('Use this folder'));
  expect(test.onChoose).toHaveBeenCalledExactlyOnceWith('/root/cells');
});

it('shows Scenario projects without entering them', async () => {
  const test = await mount('/root/cells');
  const project = test.find((p) => p.title === 'Scenario project');
  expect(project.disabled).toBe(true);
  expect(project.onClick).toBeUndefined();
});

it('refuses to choose a folder inside a Scenario project', async () => {
  const test = await mount('/root/cells/pick.scenario');
  expect(test.find((p) => text(p) === 'Use this folder').disabled).toBe(true);
  expect(test.find((p) => p.role === 'status')).toBeDefined();
  await test.click(test.button('Cancel'));
  expect(test.onCancel).toHaveBeenCalledOnce();
  expect(test.onChoose).not.toHaveBeenCalled();
});

it('in project mode chooses a Scenario project and only navigates ordinary folders', async () => {
  const test = await mount('/root/cells', true);
  expect(test.find((p) => p['aria-label'] === 'Choose a Scenario project')).toBeDefined();
  expect(() => test.find((p) => text(p) === 'Use this folder')).toThrow();
  await test.click(test.button('lab'));
  expect(apiMock.files.browse).toHaveBeenLastCalledWith('scenario', '/root/cells/lab', expect.any(AbortSignal));
  expect(test.onChoose).not.toHaveBeenCalled();
  await test.click(test.find((p) => p.title === 'Up one folder'));
  const project = test.find((p) => p.title === 'Choose this Scenario project');
  expect(project.disabled).toBe(false);
  await test.click(project);
  expect(test.onChoose).toHaveBeenCalledExactlyOnceWith('/root/cells/pick.scenario');
});
