import { vi } from 'vitest';

/** React props of one mounted host element (no DOM environment needed). */
export type MountedProps = Record<string, unknown> & {
  children?: unknown;
  onClick?: () => void;
  onChange?: (event: { target: { value?: string; checked?: boolean } }) => void;
};

type FakeNode = Record<string, unknown>;

/**
 * A minimal fake document for `react-dom/client`: elements are recorded so a
 * test can find a mounted control by its React props and call its handlers.
 */
export function reactPropsHost() {
  const elements: FakeNode[] = [];
  const document: FakeNode = {
    nodeType: 9, activeElement: null, defaultView: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    createTextNode: (text: string) => ({ nodeType: 3, nodeValue: text, ownerDocument: document }),
  };
  const node = (tag: string, namespaceURI = 'http://www.w3.org/1999/xhtml'): FakeNode => {
    const element = {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI, ownerDocument: document, options: [],
      // React writes CSS custom properties (`--name`) through setProperty.
      style: { setProperty: () => undefined },
      addEventListener: () => undefined, removeEventListener: () => undefined,
      appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
      setAttribute: () => undefined, removeAttribute: () => undefined,
    };
    elements.push(element);
    return element;
  };
  document.createElement = (tag: string) => node(tag);
  // Inline icons (svg, path) are created in the SVG namespace.
  document.createElementNS = (namespaceURI: string, tag: string) => node(tag, namespaceURI);
  document.documentElement = node('html');
  const window = { document, HTMLIFrameElement: class {} };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const all = (match: (props: MountedProps) => boolean): MountedProps[] => elements
    .map((element) => element[
      Object.keys(element).find((key) => key.startsWith('__reactProps$')) ?? ''
    ] as MountedProps | undefined)
    .filter((props): props is MountedProps => props !== undefined && match(props));
  const find = (match: (props: MountedProps) => boolean): MountedProps => {
    // The latest render of a control is the last recorded element.
    const found = all(match).at(-1);
    if (found === undefined) throw new Error('Mounted control not found');
    return found;
  };
  return { host: node('div'), find, all };
}
