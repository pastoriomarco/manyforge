import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { ProgramAuthoringDocument } from '../types/api';

const sceneCommit = vi.hoisted(() => vi.fn());
vi.mock('../api/client', () => ({
  api: { scene: { commit: sceneCommit }, files: { browse: vi.fn() } },
}));

import { ProgramSceneEditor } from './SceneAuthoringWorkspace';

afterEach(() => {
  vi.unstubAllGlobals();
});

type FakeNode = Record<string, unknown>;
type ReactProps = Record<string, unknown> & {
  onChange?: (event: unknown) => void;
  onBlurCapture?: (event: unknown) => void;
};

// The minimal React DOM host of FrameSelector.mounted.test.tsx: real commits,
// state and effects; every created element is kept so the test calls the
// React handlers of one mounted control.
function stubDom() {
  const created: FakeNode[] = [];
  const node = (tag: string): FakeNode => {
    const element: FakeNode = {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {},
      options: [],
      addEventListener: () => undefined, removeEventListener: () => undefined,
      appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
      setAttribute: () => undefined, removeAttribute: () => undefined,
    };
    created.push(element);
    return element;
  };
  const document: Record<string, unknown> = {
    nodeType: 9, activeElement: null, defaultView: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    createTextNode: (text: string) => ({ nodeType: 3, nodeValue: text, ownerDocument: document }),
  };
  document.createElement = node;
  document.documentElement = node('html');
  const window = {
    document, HTMLIFrameElement: class {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout,
  };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  return { host: node('div'), created };
}

function reactProps(element: FakeNode): ReactProps | undefined {
  const key = Object.keys(element).find((name) => name.startsWith('__reactProps$'));
  return key === undefined ? undefined : element[key] as ReactProps;
}

const program = {
  component_roles: {},
  target_roles: {},
  scene: [{
    scene_resource_id: 'part',
    shape: { type: 'box', box_dimensions_m: [0.1, 0.1, 0.1] },
    pose: {
      frame_ref: { kind: 'cell_frame' },
      position_m: [0.4, 0.1, 0.2],
      orientation: { xyzw: [0, 0, 0, 1] },
    },
  }],
} as unknown as ProgramAuthoringDocument;

describe('viewer object panel', () => {
  it('commits a pose field change as one Draft update and no live Scene transaction', async () => {
    const { host, created } = stubDom();
    const mutate = vi.fn(async () => null);
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => {
      root.render(
        <ProgramSceneEditor
          program={program}
          draftRevision={3}
          busy={false}
          busyReason={null}
          mutate={mutate}
          liveObjects={[]}
          selectedResourceId="part"
          onSelectResource={() => undefined}
        />,
      );
    });
    const mounted = (predicate: (props: ReactProps) => boolean) => {
      const element = created.find((candidate) => {
        const props = reactProps(candidate);
        return props !== undefined && predicate(props);
      });
      if (element === undefined) throw new Error('control is not mounted');
      return reactProps(element)!;
    };
    const x = mounted((props) => props['aria-label'] === 'Draft X position (m)');
    await act(() => {
      x.onChange!({ currentTarget: { valueAsNumber: 0.5 } });
    });
    // Editing the field alone sends nothing; committing it (blur) is one edit.
    expect(mutate).not.toHaveBeenCalled();
    const editor = mounted((props) => props['data-testid'] === 'program-scene-editor');
    await act(() => {
      editor.onBlurCapture!({ target: { tagName: 'INPUT' } });
    });

    expect(mutate).toHaveBeenCalledTimes(1);
    expect(mutate).toHaveBeenCalledWith('scene_draft_update_object', {
      scene_resource: expect.objectContaining({
        scene_resource_id: 'part',
        pose: expect.objectContaining({ position_m: [0.5, 0.1, 0.2] }),
      }),
    });
    expect(sceneCommit).not.toHaveBeenCalled();
    await act(() => { root.unmount(); });
  });
});
