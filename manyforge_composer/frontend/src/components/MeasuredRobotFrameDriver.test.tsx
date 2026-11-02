import { act } from 'react';
import * as THREE from 'three';
import { afterEach, expect, it, vi } from 'vitest';

import type {
  MeasuredRobotPresentation,
  MeasuredRobotPresentationReader,
} from '../state/runtimeScenePresentation';
import { MeasuredRobotFrameDriver } from './StructuralViewer';

const frameLoop = vi.hoisted(() => ({
  render: null as (() => void) | null,
  invalidate: vi.fn(),
}));

vi.mock('@react-three/fiber', async (importOriginal) => ({
  ...await importOriginal<typeof import('@react-three/fiber')>(),
  // Run the actual component and its effects/timers; only the renderer's
  // frame ingress is controlled, without requiring WebGL in this unit test.
  useFrame: (render: () => void) => { frameLoop.render = render; },
  useThree: (select: (state: { invalidate: () => void }) => unknown) => (
    select({ invalidate: frameLoop.invalidate })
  ),
}));

afterEach(() => {
  vi.useRealTimers();
  vi.unstubAllGlobals();
  frameLoop.render = null;
  frameLoop.invalidate.mockClear();
});

function installFakeReactDom() {
  // The driver renders no DOM. Reuse the tiny React DOM host pattern from
  // useHeadlessState.test.ts so real commit/layout effects run with no new
  // DOM package or synthetic copy of the driver's scheduling logic.
  type FakeNode = {
    nodeType: number;
    nodeName: string;
    tagName: string;
    namespaceURI: string;
    ownerDocument: FakeDocument;
    style: Record<string, string>;
    addEventListener: () => void;
    removeEventListener: () => void;
    appendChild: () => void;
    insertBefore: () => void;
    removeChild: () => void;
    setAttribute: () => void;
    removeAttribute: () => void;
  };
  type FakeDocument = {
    nodeType: number;
    documentElement: FakeNode | null;
    activeElement: null;
    defaultView: Record<string, unknown>;
    addEventListener: () => void;
    removeEventListener: () => void;
    createElement: (tagName: string) => FakeNode;
  };
  const document = {
    nodeType: 9,
    documentElement: null,
    activeElement: null,
    defaultView: {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
  } as FakeDocument;
  const makeNode = (tagName: string): FakeNode => ({
    nodeType: 1,
    nodeName: tagName.toUpperCase(),
    tagName: tagName.toUpperCase(),
    namespaceURI: 'http://www.w3.org/1999/xhtml',
    ownerDocument: document,
    style: {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
    appendChild: () => undefined,
    insertBefore: () => undefined,
    removeChild: () => undefined,
    setAttribute: () => undefined,
    removeAttribute: () => undefined,
  });
  document.createElement = makeNode;
  document.documentElement = makeNode('html');
  const window = {
    document,
    HTMLIFrameElement: class {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
    setTimeout: globalThis.setTimeout,
    clearTimeout: globalThis.clearTimeout,
  };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  return makeNode;
}

it('credits the rendered reader A and preserves reader B until its own render', async () => {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'performance'] });

  const makeNode = installFakeReactDom();

  const presentationA: MeasuredRobotPresentation = {
    objectId: 'robot',
    sourceId: 'arm',
    sourceEpoch: 'owner:source',
    sourceSequence: 1,
    observedAtNs: '1000',
    linkTransforms: {
      arm: { position_m: [1, 0, 0], orientation_xyzw: [0, 0, 0, 1] },
    },
  };
  // A semantically unchanged descriptor/model refresh creates a new reader
  // and presentation object for the SAME outstanding native frame.
  const presentationB = { ...presentationA };
  const credits: string[] = [];
  const listenersA = new Set<() => void>();
  const listenersB = new Set<() => void>();
  const reader = (
    name: string,
    presentation: MeasuredRobotPresentation,
    listeners: Set<() => void>,
  ): MeasuredRobotPresentationReader => ({
    readMeasuredRobotPresentation: () => presentation,
    subscribeMeasuredRobotPresentation: (listener) => {
      listeners.add(listener);
      return () => { listeners.delete(listener); };
    },
    markMeasuredRobotPresentationApplied: vi.fn((candidate) => {
      // The production reader's identity check rejects another reader's
      // presentation even when both contain the same measured pose.
      if (candidate === presentation) credits.push(name);
    }),
  });
  const readerA = reader('A', presentationA, listenersA);
  const readerB = reader('B', presentationB, listenersB);
  const group = new THREE.Group();
  const linkGroups = new Map([['arm', { current: group }]]);
  const latestFrameAppliers = new Set<() => void>();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(makeNode('div') as unknown as Element);
  const renderReader = (livePresentation: MeasuredRobotPresentationReader) => (
    root.render(<MeasuredRobotFrameDriver
      manifestModelKey="model"
      linkGroups={linkGroups}
      presentation={presentationA}
      livePresentation={livePresentation}
      latestFrameAppliers={latestFrameAppliers}
      interpolationDurationMs={0}
      minimumCreditIntervalMs={40}
    />)
  );

  try {
    await act(async () => { renderReader(readerA); });
    expect(group.position.x).toBe(1);
    expect(listenersA.size).toBe(1);
    expect(credits).toEqual([]);
    vi.advanceTimersByTime(0);
    expect(frameLoop.invalidate).toHaveBeenCalled();
    expect(frameLoop.render).not.toBeNull();
    await act(async () => { frameLoop.render!(); });
    // A has rendered, but its zero-delay after-render task is still held.
    expect(credits).toEqual([]);

    await act(async () => { renderReader(readerB); });
    expect(listenersA.size).toBe(0);
    expect(listenersB.size).toBe(1);
    expect(latestFrameAppliers.size).toBe(1);
    vi.advanceTimersByTime(0);
    expect(readerA.markMeasuredRobotPresentationApplied)
      .toHaveBeenCalledExactlyOnceWith(presentationA);
    expect(readerB.markMeasuredRobotPresentationApplied).not.toHaveBeenCalled();
    expect(credits).toEqual(['A']);

    // A's callback must not consume B's pending entry. B returns its own
    // credit only after another actual component frame and the credit bound.
    await act(async () => { frameLoop.render!(); });
    vi.advanceTimersByTime(39);
    expect(credits).toEqual(['A']);
    vi.advanceTimersByTime(1);
    expect(readerB.markMeasuredRobotPresentationApplied)
      .toHaveBeenCalledExactlyOnceWith(presentationB);
    expect(credits).toEqual(['A', 'B']);
  } finally {
    await act(async () => root.unmount());
  }
  expect(listenersA.size).toBe(0);
  expect(listenersB.size).toBe(0);
  expect(latestFrameAppliers.size).toBe(0);
  expect(vi.getTimerCount()).toBe(0);
});

it('never re-applies a stored presentation over a live reader without a usable frame', async () => {
  vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout', 'performance'] });
  const makeNode = installFakeReactDom();
  const stored = (x: number): MeasuredRobotPresentation => ({
    objectId: 'robot',
    sourceId: 'arm',
    sourceEpoch: 'owner:source',
    sourceSequence: x,
    observedAtNs: String(1000 + x),
    linkTransforms: {
      arm: { position_m: [x, 0, 0], orientation_xyzw: [0, 0, 0, 1] },
    },
  });
  // The live reader exists but its latest frame is not usable (for example a
  // model refresh in progress): it returns no presentation.
  const live: MeasuredRobotPresentationReader = {
    readMeasuredRobotPresentation: () => null,
    subscribeMeasuredRobotPresentation: () => () => undefined,
    markMeasuredRobotPresentationApplied: vi.fn(),
  };
  const group = new THREE.Group();
  group.position.set(9, 0, 0); // the pose the robot is currently shown at
  const linkGroups = new Map([['arm', { current: group }]]);
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(makeNode('div') as unknown as Element);
  const render = (presentation: MeasuredRobotPresentation, reader?: MeasuredRobotPresentationReader) => (
    root.render(<MeasuredRobotFrameDriver
      manifestModelKey="model"
      linkGroups={linkGroups}
      presentation={presentation}
      livePresentation={reader}
      latestFrameAppliers={new Set()}
      interpolationDurationMs={0}
      minimumCreditIntervalMs={40}
    />)
  );
  try {
    // An older stored pose arrives while the live frame is unusable.
    await act(async () => { render(stored(2), live); });
    expect(group.position.x).toBe(9);
    await act(async () => { render(stored(3), live); });
    expect(group.position.x).toBe(9);
    // Without a live stream the stored presentation remains the source.
    await act(async () => { render(stored(4)); });
    expect(group.position.x).toBe(4);
  } finally {
    await act(async () => root.unmount());
  }
});
