import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type { AttachedObjectPoseMeasurement } from '../state/runtimeScenePresentation';
import type { CarriedObjectAttachment } from '../types/api';
import { CarriedObjectPoseCard } from './CarriedObjectPoseCard';

const measurement: AttachedObjectPoseMeasurement = {
  fresh: true,
  observedAtNs: '1000000000',
  sourceId: 'pose-source',
  sourceEpoch: 'pose-source.1',
  sourceSequence: 12,
  holder: {
    controllable_component_id: 'lite6_robot',
    command_target_id: 'gripper',
    gripper_link: 'robotiq_base_link',
  },
  measured_against: { entity: '/World/lite6/tool', link: 'robotiq_base_link' },
  source_header_frame_id: 'robotiq_header',
  frame: {
    kind: 'component',
    controllable_component_id: 'lite6_robot',
    model_instance_id: 'lite6.model',
    frame_role_id: 'tcp',
  },
  current_offset: {
    position_m: { x: 0.02, y: -0.01, z: 0.25 },
    orientation_xyzw: [0, 0, 0, 1],
  },
  baseline_offset: {
    position_m: { x: 0.01, y: -0.01, z: 0.24 },
    orientation_xyzw: [0, 0, 0, 1],
  },
  drift: { translation_m: 0.0141, rotation_rad: 0.02 },
};

const attachment: CarriedObjectAttachment = {
  carried_object_attachment_id: 'attachment.1',
  carried_object_attachment_revision: 'attachment.revision.1',
  scene_resource_id: 'part.1',
  controllable_component_id: 'lite6_robot',
  command_target_id: 'gripper',
  carrier_frame_ref: {
    kind: 'component', controllable_component_id: 'lite6_robot', model_instance_id: 'lite6.model',
    frame_role_id: 'flange',
  },
  pose_in_carrier_frame: { position_m: { x: 0, y: 0, z: 0 }, orientation_xyzw: [0, 0, 0, 1] },
  touch_body_roles: [],
};

type Props = Record<string, unknown> & { onClick?: () => unknown };
type FakeNode = Record<string, unknown> & { children: FakeNode[]; parentNode: FakeNode | null };
function mountedHost() {
  const document: Record<string, unknown> = { nodeType: 9, activeElement: null,
    addEventListener: () => undefined, removeEventListener: () => undefined };
  let focused: FakeNode | null = null;
  function node(tag: string): FakeNode {
    const element: FakeNode = {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document,
      style: {}, options: [], children: [], parentNode: null,
      addEventListener: () => undefined, removeEventListener: () => undefined,
      // React sets `disabled` as an attribute; the card reads the property.
      setAttribute: (name: string) => { if (name === 'disabled') element.disabled = true; },
      removeAttribute: (name: string) => { if (name === 'disabled') element.disabled = false; },
      focus: () => {
        focused = element;
        document.activeElement = element;
      },
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
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const host = node('div');
  const props = (element: FakeNode) => element[
    Object.keys(element).find((key) => key.startsWith('__reactProps$'))!
  ] as Props | undefined;
  const walk = (element: FakeNode): FakeNode[] => [element, ...element.children.flatMap(walk)];
  const find = (match: (props: Props) => boolean) => {
    const value = walk(host).map(props).find((props) => props !== undefined && match(props));
    if (value === undefined) throw new Error('Mounted carried-object control not found');
    return value;
  };
  const findAll = (match: (props: Props) => boolean) => walk(host)
    .map(props)
    .filter((props): props is Props => props !== undefined && match(props));
  const keydownListeners = new Set<(event: KeyboardEvent) => void>();
  document.addEventListener = (type: string, listener: EventListenerOrEventListenerObject) => {
    if (type === 'keydown' && typeof listener === 'function') {
      keydownListeners.add(listener as (event: KeyboardEvent) => void);
    }
  };
  document.removeEventListener = (type: string, listener: EventListenerOrEventListenerObject) => {
    if (type === 'keydown' && typeof listener === 'function') {
      keydownListeners.delete(listener as (event: KeyboardEvent) => void);
    }
  };
  const dispatchKeyDown = (key: string) => {
    const event = { key, preventDefault: () => undefined } as KeyboardEvent;
    for (const listener of keydownListeners) listener(event);
  };
  return {
    host,
    find,
    findAll,
    focusedProps: () => focused === null ? undefined : props(focused),
    dispatchKeyDown,
  };
}

afterEach(() => vi.unstubAllGlobals());

describe('Carried object pose card', () => {
  it('shows owner-reported holder, body, frame, offset, baseline and drift', () => {
    const html = renderToStaticMarkup(<CarriedObjectPoseCard measurement={measurement} />);

    expect(html).toContain('lite6_robot');
    expect(html).toContain('tcp');
    expect(html).toContain('gripper');
    expect(html).toContain('robotiq_base_link');
    expect(html).toContain('/World/lite6/tool');
    expect(html).toContain('robotiq_base_link');
    expect(html).toContain('lite6.model');
    expect(html).toContain('Frame (offset expressed in)');
    expect(html).toContain('Current offset');
    expect(html).toContain('position [0.02, -0.01, 0.25] m');
    expect(html).toContain('Grasp baseline offset');
    expect(html).toContain('Drift from grasp baseline');
    expect(html).toContain('0.0141 m translation');
    expect(html).toContain('0.02 rad rotation');
    expect(html).toContain('Source header frame');
  });

  it('labels stale offsets as last reported and omits an absent baseline and drift', () => {
    const stale: AttachedObjectPoseMeasurement = {
      ...measurement,
      fresh: false,
    };
    delete stale.baseline_offset;
    delete stale.drift;
    if ('source_header_frame_id' in stale) delete stale.source_header_frame_id;
    const html = renderToStaticMarkup(<CarriedObjectPoseCard measurement={stale} />);

    expect(html).toContain('Stale pose sample; showing the last reported values.');
    expect(html).toContain('Last reported offset (stale)');
    expect(html).not.toContain('Grasp baseline offset');
    expect(html).not.toContain('Drift from grasp baseline');
    expect(html).not.toContain('Source header frame');
  });

  it('does not invent values while no resolved measurement is available', () => {
    const html = renderToStaticMarkup(<CarriedObjectPoseCard measurement={null} />);

    expect(html).toContain('No resolved carried-object pose measurement is available for this object.');
    expect(html).not.toContain('Current offset');
    expect(html).not.toContain('0.000');
  });

  it('shows a continuous grasp-loss measurement against the cell frame without a holder', () => {
    const continuous: AttachedObjectPoseMeasurement = {
      fresh: true,
      observedAtNs: '1000000000',
      sourceId: 'pose-source',
      sourceEpoch: 'pose-source.1',
      sourceSequence: 12,
      measured_against: 'cell frame',
      frame: measurement.frame,
      current_offset: measurement.current_offset,
      baseline_offset: measurement.baseline_offset,
      drift: measurement.drift,
    };
    const html = renderToStaticMarkup(<CarriedObjectPoseCard measurement={continuous} />);

    expect(html).toContain('Measured against');
    expect(html).toContain('cell frame');
    expect(html).toContain('Drift from grasp baseline');
    expect(html).not.toContain('<dt>Holder</dt>');
    expect(html).not.toContain('Source header frame');
  });

  it('offers Detach and explains when Program motion makes it unavailable', () => {
    const available = renderToStaticMarkup(<CarriedObjectPoseCard measurement={measurement}
      attachment={attachment} detachUnavailableReason={null} onDetach={async () => {}} />);
    expect(available).toContain('>Detach</button>');
    expect(available).not.toContain('disabled=""');

    const moving = renderToStaticMarkup(<CarriedObjectPoseCard measurement={measurement}
      attachment={attachment} detachUnavailableReason="Not available while the Program is moving."
      onDetach={async () => {}} />);
    expect(moving).toContain('>Detach</button>');
    expect(moving).toContain('disabled=""');
    expect(moving).toContain('Not available while the Program is moving.');
  });

  it('requires confirmation and sends nothing when Cancel is chosen', async () => {
    const dom = mountedHost();
    const onDetach = vi.fn(async () => {});
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(dom.host as unknown as Element);
    await act(() => root.render(<CarriedObjectPoseCard measurement={measurement}
      attachment={attachment} detachUnavailableReason={null} onDetach={onDetach} />));
    try {
      const click = async (label: string, last = false) => act(async () => {
        const buttons = dom.findAll((props) => (
          props.children === label && typeof props.onClick === 'function'
        ));
        const button = last ? buttons.at(-1) : buttons[0];
        if (button === undefined) throw new Error(`Mounted ${label} button not found`);
        await button.onClick?.();
      });
      await click('Detach');
      expect(dom.find((props) => props.role === 'dialog')['aria-modal']).toBe(true);
      expect(dom.find((props) => props.children
        === 'Detach part.1 from lite6_robot / gripper at its current pose?')).toBeDefined();
      await click('Cancel');
      expect(onDetach).not.toHaveBeenCalled();
      expect(dom.focusedProps()?.children).toBe('Detach');

      await click('Detach');
      await click('Detach', true);
      expect(onDetach).toHaveBeenCalledExactlyOnceWith(attachment);
      expect(dom.focusedProps()?.children).toBe('Detach');
    } finally {
      await act(() => root.unmount());
    }
  });

  it('returns focus to the card when Escape closes after the attachment disappears', async () => {
    const dom = mountedHost();
    const onDetach = vi.fn(async () => {});
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(dom.host as unknown as Element);
    const renderCard = (currentAttachment: CarriedObjectAttachment | null) => (
      <CarriedObjectPoseCard measurement={measurement} attachment={currentAttachment}
        detachUnavailableReason={currentAttachment === null ? 'The carried-object attachment is unavailable in the current Scene.' : null}
        onDetach={onDetach} />
    );
    await act(() => root.render(renderCard(attachment)));
    try {
      const click = async (label: string) => act(async () => {
        await dom.find((props) => props.children === label && typeof props.onClick === 'function').onClick?.();
      });
      await click('Detach');
      await act(() => root.render(renderCard(null)));
      expect(dom.find((props) => props.children === 'Detach').disabled).toBe(true);
      await act(() => dom.dispatchKeyDown('Escape'));
      expect(dom.focusedProps()?.['aria-label']).toBe('Carried object pose');
      expect(onDetach).not.toHaveBeenCalled();
    } finally {
      await act(() => root.unmount());
    }
  });

  it('shows a typed Scene refusal through the shared error catalog', async () => {
    const dom = mountedHost();
    const refusal = Object.assign(new Error('Scene owner refused detach.'), {
      structuredError: {
        code: 'scene_object_state_conflict', message: 'Scene owner refused detach.', details: null, origin: null,
      },
    });
    const onDetach = vi.fn(async () => { throw refusal; });
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(dom.host as unknown as Element);
    await act(() => root.render(<CarriedObjectPoseCard measurement={measurement}
      attachment={attachment} detachUnavailableReason={null} onDetach={onDetach} />));
    try {
      const click = async (label: string) => act(async () => {
        const buttons = dom.findAll((props) => (
          props.children === label && typeof props.onClick === 'function'
        ));
        const button = buttons.at(-1);
        if (button === undefined) throw new Error(`Mounted ${label} button not found`);
        await button.onClick?.();
      });
      await click('Detach');
      await click('Detach');
      expect(dom.find((props) => props.role === 'alert').children).toContain('Object state conflict');
    } finally {
      await act(() => root.unmount());
    }
  });
});
