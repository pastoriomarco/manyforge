import { act } from 'react';
import { afterEach, expect, it, vi } from 'vitest';
import type { CatalogEntry } from '../types/api';
import { NodePaletteItem } from './NodePaletteItem';

const glyph = vi.hoisted(() => vi.fn(() => null));
vi.mock('./programNodeGlyphs', () => ({ ProgramCategoryIcon: glyph }));
afterEach(() => { vi.unstubAllGlobals(); glyph.mockClear(); });

// A minimal DOM host, as used by the other mounted component tests.
function mountedHost() {
  const elements: Record<string, unknown>[] = [];
  const document: Record<string, unknown> = {
    nodeType: 9, activeElement: null, defaultView: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    createTextNode: (text: string) => ({ nodeType: 3, nodeValue: text, ownerDocument: document }),
  };
  const node = (tag: string): Record<string, unknown> => {
    const element = {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {},
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
  const props = (element: Record<string, unknown>) => element[
    Object.keys(element).find((key) => key.startsWith('__reactProps$'))!
  ] as Record<string, unknown> | undefined;
  return { host: node('div'), elements, props };
}

const entry: CatalogEntry = {
  id: 'sequence', name: 'Sequence', category: 'control_flow', description: 'Run children.',
  stability: 'stable', kind: 'composite', childrenMin: 1, childrenMax: -1,
  parameters: [], requiredCapabilities: [],
  execution: { blocking: true, cancellable: true, ownerApi: 'program' },
  runners: { py_trees: 'sequence_v1' }, tags: [], minProgramProfile: null,
  blackboard: { reads: [], writes: [] }, ownerContext: null, outcomes: {}, eventFamily: null,
};

it('skips unchanged items while applying changes to catalog and availability', async () => {
  const { host, elements, props } = mountedHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  const activate = vi.fn();
  const render = (current: CatalogEntry, disabled: boolean, available: boolean) => (
    <NodePaletteItem entry={current} disabled={disabled} onActivate={activate}
      executionAvailable={available} executionNote={available ? 'Available' : 'Unavailable'} allowDrag />
  );
  try {
    await act(() => root.render(render(entry, false, true)));
    expect(glyph).toHaveBeenCalledTimes(1);
    await act(() => root.render(render(entry, false, true)));
    expect(glyph).toHaveBeenCalledTimes(1);
    await act(() => root.render(render(entry, true, false)));
    expect(glyph).toHaveBeenCalledTimes(2);
    const button = elements.map(props).find((p) => p?.className === 'node-palette-item disabled draggable');
    expect(button?.disabled).toBe(true);
    const info = elements.map(props).find((p) => p?.className === 'icon-button node-info-button execution-unavailable');
    expect(info?.['aria-label']).toContain('execution unavailable');
    await act(() => root.render(render({ ...entry, name: 'Changed sequence' }, true, false)));
    expect(glyph).toHaveBeenCalledTimes(3);
    expect(elements.map(props).find((p) => p?.className === 'node-palette-item disabled draggable')?.['aria-label'])
      .toBe('Changed sequence');
  } finally {
    await act(() => root.unmount());
  }
});
