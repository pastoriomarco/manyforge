import { act } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { DraftInputResetProvider, DraftTextarea, DraftTextInput } from './DraftInputs';

type HostProps = Record<string, unknown> & { value?: string };
type HostNode = Record<string, unknown>;

function mountedInputHost() {
  const elements: HostNode[] = [];
  const document: HostNode = {
    nodeType: 9,
    activeElement: null,
    defaultView: {},
    addEventListener: () => undefined,
    removeEventListener: () => undefined,
    createTextNode: (text: string) => ({ nodeType: 3, nodeValue: text, ownerDocument: document }),
  };
  const node = (tag: string): HostNode => {
    const element: HostNode = {
      nodeType: 1,
      nodeName: tag.toUpperCase(),
      tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml',
      ownerDocument: document,
      style: { setProperty: () => undefined, removeProperty: () => undefined },
      addEventListener: () => undefined,
      removeEventListener: () => undefined,
      appendChild: () => undefined,
      insertBefore: () => undefined,
      removeChild: () => undefined,
      setAttribute: () => undefined,
      removeAttribute: () => undefined,
      focus: () => { document.activeElement = element; },
      blur: () => { document.activeElement = null; },
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

  const props = (element: HostNode): HostProps | undefined => element[
    Object.keys(element).find((key) => key.startsWith('__reactProps$')) ?? ''
  ] as HostProps | undefined;
  const findElement = (label: string) => {
    const found = elements.filter((element) => props(element)?.['aria-label'] === label).at(-1);
    if (found === undefined) throw new Error(`Mounted input not found: ${label}`);
    return found;
  };
  const findProps = (label: string) => {
    const found = props(findElement(label));
    if (found === undefined) throw new Error(`Mounted input props not found: ${label}`);
    return found;
  };
  const host = node('div');
  return { document, host, findElement, findProps };
}

function Harness({ globalKey, localKey, value, onInvalidChange }: {
  globalKey: string;
  localKey: string;
  value: number;
  onInvalidChange: (message: string | null) => void;
}) {
  return (
    <DraftInputResetProvider resetKey={globalKey}>
      <DraftTextInput
        aria-label="Probe value"
        value={value}
        resetKey={localKey}
        validateOnInput
        validate={(candidate) => Number.isFinite(Number(candidate)) ? null : 'Enter a number.'}
        onInvalidChange={onInvalidChange}
        onCommit={() => undefined}
      />
    </DraftInputResetProvider>
  );
}

function ScopedInput({ label, value, onInvalidChange }: {
  label: string;
  value: number;
  onInvalidChange: (message: string | null) => void;
}) {
  return <DraftTextInput aria-label={label} value={value} validateOnInput
    validate={(candidate) => Number.isFinite(Number(candidate)) ? null : 'Enter a number.'}
    onInvalidChange={onInvalidChange} onCommit={() => undefined} />;
}

function ScopedHarness({ scenarioKey, programKey, deploymentKey, onInvalidChange }: {
  scenarioKey: string;
  programKey: string;
  deploymentKey: string;
  onInvalidChange: (message: string | null) => void;
}) {
  return (
    <DraftInputResetProvider scope="scenario" resetKey={scenarioKey}>
      <DraftInputResetProvider scope="program" resetKey={programKey}>
        <ScopedInput label="Program value" value={1} onInvalidChange={onInvalidChange} />
      </DraftInputResetProvider>
      <DraftInputResetProvider scope="deployment" resetKey={deploymentKey}>
        <ScopedInput label="Deployment value" value={2} onInvalidChange={onInvalidChange} />
      </DraftInputResetProvider>
    </DraftInputResetProvider>
  );
}

function TextareaHarness({ scenarioKey, programKey, deploymentKey, value, onInvalidChange }: {
  scenarioKey: string;
  programKey: string;
  deploymentKey: string;
  value: string;
  onInvalidChange: (message: string | null) => void;
}) {
  return (
    <DraftInputResetProvider scope="scenario" resetKey={scenarioKey}>
      <DraftInputResetProvider scope="program" resetKey={programKey}>
        <div />
      </DraftInputResetProvider>
      <DraftInputResetProvider scope="deployment" resetKey={deploymentKey}>
        <DraftTextarea
          aria-label="Touch links"
          value={value}
          validate={(candidate) => candidate.includes(',') ? null : 'Enter comma-separated names.'}
          onInvalidChange={onInvalidChange}
          onCommit={() => undefined}
        />
      </DraftInputResetProvider>
    </DraftInputResetProvider>
  );
}

afterEach(() => vi.unstubAllGlobals());

describe('DraftTextInput explicit reset identities', () => {
  it('preserves owner observations and focus, then clears invalid text on global or local reset', async () => {
    const host = mountedInputHost();
    const onInvalidChange = vi.fn();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host.host as unknown as Element);
    const render = (globalKey: string, localKey: string, value: number) => act(() => root.render(
      <Harness globalKey={globalKey} localKey={localKey} value={value} onInvalidChange={onInvalidChange} />,
    ));
    try {
      await render('global-0', 'robot-local-0', 7);
      const input = host.findElement('Probe value');
      (input.focus as () => void)();
      const props = () => host.findProps('Probe value');
      await act(() => (props().onFocus as (event: unknown) => void)({ target: { value: '7' } }));
      await act(() => (props().onChange as (event: unknown) => void)({ target: { value: 'invalid' } }));
      expect(props().value).toBe('invalid');
      expect(props()['aria-invalid']).toBe(true);

      // Neither an unchanged refresh nor an unrelated backing observation
      // is an explicit Draft reset, so the focused invalid text stays visible.
      await render('global-0', 'robot-local-0', 7);
      expect(props().value).toBe('invalid');
      await render('global-0', 'robot-local-0', 8);
      expect(props().value).toBe('invalid');

      // A global Draft reset clears text even while the Robot panel's own
      // reset key is unchanged, and does not replace or blur the input node.
      await render('global-1', 'robot-local-0', 8);
      expect(host.findElement('Probe value')).toBe(input);
      expect(host.document.activeElement).toBe(input);
      expect(props().value).toBe('8');
      expect(props()['aria-invalid']).not.toBe(true);
      expect(onInvalidChange).toHaveBeenLastCalledWith(null);

      await act(() => (props().onChange as (event: unknown) => void)({ target: { value: 'still-invalid' } }));
      expect(props()['aria-invalid']).toBe(true);
      // RobotNumberInput's existing local rebase key also resets independently.
      await render('global-1', 'robot-local-1', 8);
      expect(host.findElement('Probe value')).toBe(input);
      expect(host.document.activeElement).toBe(input);
      expect(props().value).toBe('8');
      expect(props()['aria-invalid']).not.toBe(true);
    } finally {
      await act(() => root.unmount());
    }
  });

  it('rebases an invalid focused textarea only for its scope or the Scenario reset', async () => {
    const host = mountedInputHost();
    const onInvalidChange = vi.fn();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host.host as unknown as Element);
    const render = (scenarioKey: string, programKey: string, deploymentKey: string, value: string) => act(() => (
      root.render(<TextareaHarness scenarioKey={scenarioKey} programKey={programKey} deploymentKey={deploymentKey}
        value={value} onInvalidChange={onInvalidChange} />)
    ));
    try {
      await render('scenario-0', 'program-0', 'deployment-0', 'saved_link');
      const textarea = host.findElement('Touch links');
      const props = () => host.findProps('Touch links');
      (textarea.focus as () => void)();
      await act(() => (props().onFocus as (event: unknown) => void)({ target: { value: 'saved_link' } }));
      await act(() => (props().onChange as (event: unknown) => void)({ target: { value: 'unfinished link' } }));
      const preventDefault = vi.fn();
      await act(() => (props().onKeyDown as (event: unknown) => void)({ key: 'Tab', preventDefault }));
      expect(preventDefault).toHaveBeenCalledOnce();
      expect(props().value).toBe('unfinished link');
      expect(props()['aria-invalid']).toBe(true);

      // Ordinary owner refreshes, unrelated scope resets, and an unchanged
      // deployment epoch (including a failed reset) preserve this edit.
      await render('scenario-0', 'program-1', 'deployment-0', 'new_saved_link');
      expect(props().value).toBe('unfinished link');
      expect(props()['aria-invalid']).toBe(true);
      expect(host.document.activeElement).toBe(textarea);

      // A successful deployment reset clears text and its error without
      // replacing the focused textarea.
      await render('scenario-0', 'program-1', 'deployment-1', 'new_saved_link');
      expect(host.findElement('Touch links')).toBe(textarea);
      expect(host.document.activeElement).toBe(textarea);
      expect(props().value).toBe('new_saved_link');
      expect(props()['aria-invalid']).not.toBe(true);
      expect(onInvalidChange).toHaveBeenLastCalledWith(null);

      await act(() => (props().onChange as (event: unknown) => void)({ target: { value: 'another unfinished' } }));
      const nextPreventDefault = vi.fn();
      await act(() => (props().onKeyDown as (event: unknown) => void)({ key: 'Tab', preventDefault: nextPreventDefault }));
      expect(props()['aria-invalid']).toBe(true);
      await render('scenario-1', 'program-1', 'deployment-1', 'scenario_saved_link');
      expect(host.findElement('Touch links')).toBe(textarea);
      expect(host.document.activeElement).toBe(textarea);
      expect(props().value).toBe('scenario_saved_link');
      expect(props()['aria-invalid']).not.toBe(true);
    } finally {
      await act(() => root.unmount());
    }
  });

  it('resets only the matching document scope unless the Scenario epoch changes', async () => {
    const host = mountedInputHost();
    const onInvalidChange = vi.fn();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host.host as unknown as Element);
    const render = (scenarioKey: string, programKey: string, deploymentKey: string) => act(() => root.render(
      <ScopedHarness scenarioKey={scenarioKey} programKey={programKey} deploymentKey={deploymentKey}
        onInvalidChange={onInvalidChange} />,
    ));
    const props = (label: string) => host.findProps(label);
    try {
      await render('scenario-0', 'program-0', 'deployment-0');
      await act(() => (props('Program value').onChange as (event: unknown) => void)(
        { target: { value: 'program invalid' } },
      ));
      await act(() => (props('Deployment value').onChange as (event: unknown) => void)(
        { target: { value: 'deployment invalid' } },
      ));

      await render('scenario-0', 'program-1', 'deployment-0');
      expect(props('Program value').value).toBe('1');
      expect(props('Deployment value').value).toBe('deployment invalid');

      await render('scenario-0', 'program-1', 'deployment-1');
      expect(props('Deployment value').value).toBe('2');

      await act(() => (props('Program value').onChange as (event: unknown) => void)(
        { target: { value: 'again invalid' } },
      ));
      await render('scenario-1', 'program-1', 'deployment-1');
      expect(props('Program value').value).toBe('1');
      expect(props('Deployment value').value).toBe('2');
    } finally {
      await act(() => root.unmount());
    }
  });
});
