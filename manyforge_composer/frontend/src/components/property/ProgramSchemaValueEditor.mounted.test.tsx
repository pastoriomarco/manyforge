import { act, useEffect, useState } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type {
  BlackboardKeyDeclaration,
  CatalogEntry,
  CatalogJsonSchema,
  JsonValue,
  ProgramAuthoringDocument,
  ProgramTreeNode,
} from '../../types/api';
import { ProgramSchemaValueEditor } from './ProgramSchemaValueEditor';
import { firstCatalogSchemaValidationMessage } from '../../utils/catalogSchema';

afterEach(() => {
  vi.unstubAllGlobals();
});

type FakeNode = Record<string, unknown>;

// Minimal React DOM host (the pattern of MeasuredRobotFrameDriver.test.tsx):
// real commits, state and effects, no DOM package. Every created element is
// kept so a test finds a mounted control and calls its React handlers.
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

type Handler = (event: unknown) => void;
type ReactProps = Record<string, unknown> & { onFocus?: Handler; onChange?: Handler; onBlur?: Handler };

function reactProps(element: FakeNode): ReactProps | undefined {
  const key = Object.keys(element).find((name) => name.startsWith('__reactProps$'));
  return key === undefined ? undefined : element[key] as ReactProps;
}

// The value of a key-typed parameter (Set value): its schema follows the key.
const VALUE_SCHEMA: CatalogJsonSchema = {
  anyOf: [
    { type: 'number' },
    {
      type: 'object',
      properties: { frame_ref: { type: 'object' }, position_m: { type: 'array' }, orientation: { type: 'object' } },
    },
  ],
  'x-manyforge-value-type-from-key': 'key',
};

const keys: BlackboardKeyDeclaration[] = [
  { key: 'place_pose', type: 'stored_pose', access: 'read_write', persistence: 'run' },
  { key: 'force', type: 'float64', access: 'read_write', persistence: 'run' },
];

const setValue: CatalogEntry = {
  id: 'logic.set_value',
  name: 'Set value',
  category: 'logic',
  description: '',
  stability: 'stable',
  kind: 'leaf',
  childrenMin: 0,
  childrenMax: 0,
  parameters: [
    { id: 'key', required: true, schema: { type: 'string', 'x-manyforge-semantic-type': 'blackboard_key' }, schemaHash: 'sha256:k', hasDefault: false },
    { id: 'value', required: true, schema: VALUE_SCHEMA, schemaHash: 'sha256:v', hasDefault: false },
  ],
  requiredCapabilities: [],
  execution: { blocking: false, cancellable: false, ownerApi: 'orchestration.blackboard' },
  runners: {},
  tags: [],
  minProgramProfile: null,
  blackboard: { reads: [], writes: [] },
  ownerContext: null,
  outcomes: {},
  eventFamily: null,
};

const program = {
  scene: [],
  blackboard: { namespaces: [], keys },
  parameters: [],
} as unknown as ProgramAuthoringDocument;

const POSE: JsonValue = {
  frame_ref: { kind: 'cell_frame' },
  position_m: [0, 0, 0],
  orientation: { rpy_deg: [0, 0, 0] },
};

const GRIPPER_POSITION_SCHEMA: CatalogJsonSchema = {
  type: 'object', additionalProperties: false, required: ['kind'],
  properties: {
    kind: { type: 'string', enum: ['angular_rad', 'linear_m', 'named_preset'] },
    name: { type: 'string', enum: ['open', 'closed'] },
    value_m: { type: 'number' },
    value_rad: { type: 'number' },
  },
  allOf: [
    ...[['angular_rad', 'value_rad'], ['linear_m', 'value_m'], ['named_preset', 'name']]
      .flatMap(([kind, field]) => [
        { if: { required: ['kind'], properties: { kind: { const: kind } } },
          then: { required: [field] } },
        { if: { not: { required: ['kind'], properties: { kind: { const: kind } } } },
          then: { not: { required: [field] } } },
      ]),
  ],
};

describe('a mounted symbolic gripper coordinate editor', () => {
  it.each([
    ['gripper.command_position', 'position'],
    ['planning.plan_pick_sequence', 'tool_position'],
    ['planning.plan_place_sequence', 'tool_position'],
  ])('keeps preset names in %s.%s until runtime resolution', async (nodeId, parameterId) => {
    const { host, created } = stubDom();
    const committed: JsonValue[] = [];
    const entry: CatalogEntry = { ...setValue, id: nodeId, parameters: [{
      id: parameterId, required: true, schema: GRIPPER_POSITION_SCHEMA,
      schemaHash: 'sha256:coordinate-position', hasDefault: false,
    }] };
    function Host() {
      const [value, setStored] = useState<JsonValue>({ kind: 'linear_m', value_m: 0.01 });
      return <ProgramSchemaValueEditor schema={GRIPPER_POSITION_SCHEMA} value={value}
        disabled={false} path={parameterId}
        context={{ entry, node: { node_id: nodeId, instance_id: 'gripper-node',
          parameters: { [parameterId]: value }, children: [] }, parameterId, program }}
        onCommit={(next) => { committed.push(next); setStored(next); }} />;
    }
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    const control = (label: string) => {
      const props = created.map(reactProps).filter((props) => props?.['aria-label'] === label).at(-1);
      if (props === undefined) throw new Error(`No mounted control ${label}`);
      return props;
    };
    const choose = async (label: string, value: string) => {
      await act(() => control(label).onChange!({ target: { value } }));
    };
    try {
      await act(() => root.render(<Host />));
      expect(committed).toEqual([]);
      await choose('Coordinate kind', 'named_preset');
      expect(committed).toEqual([{ kind: 'named_preset', name: 'open' }]);
      expect(control('Gripper preset').value).toBe('open');
      await choose('Gripper preset', 'closed');
      expect(committed.at(-1)).toEqual({ kind: 'named_preset', name: 'closed' });
      expect(control('Gripper preset').value).toBe('closed');
      const beforeInvalid = committed.length;
      await choose('Gripper preset', 'vendor_open');
      expect(committed).toHaveLength(beforeInvalid);
      await choose('Coordinate kind', 'angular_rad');
      expect(committed.at(-1)).toEqual({ kind: 'angular_rad', value_rad: 0 });
      expect(control('Value (rad)').value).toBe('0');
      for (const value of committed) {
        expect(firstCatalogSchemaValidationMessage(GRIPPER_POSITION_SCHEMA, value, parameterId))
          .toBeNull();
      }
    } finally { await act(() => root.unmount()); }
  });

  it.each(['open', 'closed'])('shows an existing %s name without authoring on mount', async (name) => {
    const { host, created } = stubDom();
    const onCommit = vi.fn();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    try {
      await act(() => root.render(<ProgramSchemaValueEditor schema={GRIPPER_POSITION_SCHEMA}
        value={{ kind: 'named_preset', name }} disabled={true} path="position"
        context={{ entry: setValue, node: { node_id: 'gripper.command_position',
          instance_id: 'gripper-node', parameters: {}, children: [] }, parameterId: 'position', program }}
        onCommit={onCommit} />));
      const preset = created.map(reactProps).find((props) => props?.['aria-label'] === 'Gripper preset');
      expect(preset?.value).toBe(name);
      expect(preset?.disabled).toBe(true);
      expect(onCommit).not.toHaveBeenCalled();
    } finally { await act(() => root.unmount()); }
  });
});

interface Mounted {
  /** The mounted control with this aria-label or id (the most recently created). */
  control: (name: string) => FakeNode;
  type: (name: string, text: string) => Promise<void>;
  /** Replace the stored value as another editor or undo would. */
  replaceValue: (value: JsonValue) => Promise<void>;
  committed: JsonValue[];
  invalid: () => string | null;
  unmount: () => Promise<void>;
}

async function mountKeyValue(key: string, initial: JsonValue): Promise<Mounted> {
  const { host, created } = stubDom();
  const committed: JsonValue[] = [];
  let lastInvalid: string | null = null;
  let replace: (value: JsonValue) => void = () => undefined;
  const node: ProgramTreeNode = {
    node_id: setValue.id, instance_id: 'set', parameters: { key }, children: [],
  };
  function Host() {
    const [value, setStored] = useState<JsonValue>(initial);
    useEffect(() => { replace = setStored; }, []);
    return (
      <ProgramSchemaValueEditor
        schema={VALUE_SCHEMA}
        value={value}
        disabled={false}
        context={{ entry: setValue, node: { ...node, parameters: { key, value } }, parameterId: 'value', program }}
        path="value"
        onCommit={(next) => {
          committed.push(next);
          setStored(next);
        }}
        onInvalidChange={(message) => { lastInvalid = message; }}
      />
    );
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => { root.render(<Host />); });
  const control = (name: string) => {
    const found = created.filter((element) => {
      const props = reactProps(element);
      return props?.['aria-label'] === name || props?.id === name;
    }).at(-1);
    if (found === undefined) throw new Error(`No control ${name}`);
    return found;
  };
  return {
    control,
    type: async (name, text) => {
      // Focus, type, leave: the order a browser delivers to the field.
      const target = { value: text };
      await act(() => { reactProps(control(name))!.onFocus!({ target, currentTarget: target }); });
      await act(() => { reactProps(control(name))!.onChange!({ target, currentTarget: target }); });
      await act(() => {
        reactProps(control(name))!.onBlur!({
          target, currentTarget: target, relatedTarget: null,
        });
      });
    },
    replaceValue: async (value) => { await act(() => { replace(value); }); },
    committed,
    invalid: () => lastInvalid,
    unmount: async () => { await act(() => { root.unmount(); }); },
  };
}

describe('a mounted key-typed value editor', () => {
  it('reports an unfinished pose coordinate to the parameter', async () => {
    const editor = await mountKeyValue('place_pose', POSE);
    expect(editor.invalid()).toBeNull();
    await editor.type('Position x', 'abc');

    // The pose still holds the previous x; Apply must not take it as complete.
    expect(editor.committed).toEqual([]);
    expect(editor.invalid()).toBe('Position x must be a finite number.');

    await editor.type('Position x', '0.5');
    expect(editor.invalid()).toBeNull();
    expect(editor.committed.at(-1)).toMatchObject({ position_m: [0.5, 0, 0] });
    await editor.unmount();
  });

  it('keeps a sibling coordinate unfinished across another coordinate commit', async () => {
    const editor = await mountKeyValue('place_pose', POSE);
    await editor.type('Position x', 'abc');
    const x = editor.control('Position x');

    await editor.type('Position y', '2');

    expect(editor.committed).toEqual([{ ...POSE as object, position_m: [0, 2, 0] }]);
    // Same mounted field, still showing what was typed, still reported.
    expect(editor.control('Position x')).toBe(x);
    expect(reactProps(x)?.value).toBe('abc');
    expect(editor.invalid()).toBe('Position x must be a finite number.');
    await editor.unmount();
  });

  it('shows a value stored elsewhere without remounting the field', async () => {
    const editor = await mountKeyValue('force', 1.5);
    const field = editor.control('property-set-value-value');
    expect(reactProps(field)?.value).toBe('1.5');

    await editor.replaceValue(4);

    expect(editor.control('property-set-value-value')).toBe(field);
    expect(reactProps(field)?.value).toBe('4');
    expect(editor.invalid()).toBeNull();
    await editor.unmount();
  });
});

describe('an absent motion-pose goal', () => {
  const vector: CatalogJsonSchema = {
    type: 'array', minItems: 3, maxItems: 3, items: { type: 'number' },
  };
  const schema: CatalogJsonSchema = {
    type: 'object', additionalProperties: false,
    required: ['position_m', 'orientation'],
    properties: {
      position_m: vector,
      orientation: {
        type: 'object', additionalProperties: false,
        properties: {
          rpy_deg: vector,
          xyzw: { ...vector, minItems: 4, maxItems: 4 },
        },
        allOf: [{ oneOf: [{ required: ['rpy_deg'] }, { required: ['xyzw'] }] }],
      },
      frame_ref: {
        type: 'object', additionalProperties: false, required: ['kind'],
        properties: { kind: { type: 'string', enum: ['cell_frame'] } },
      },
    },
  };

  it.each(['Position y', 'Orientation y', 'Add frame reference'])(
    'commits a complete goal only after explicitly editing %s', async (action) => {
      const { host, created } = stubDom();
      const onCommit = vi.fn();
      const { createRoot } = await import('react-dom/client');
      const root = createRoot(host as unknown as Element);
      try {
        await act(() => root.render(
          <ProgramSchemaValueEditor schema={schema} value={undefined} disabled={false}
            context={{ entry: setValue, node: {
              node_id: 'motion.move_to_pose', instance_id: 'move', parameters: {}, children: [],
            }, parameterId: 'goal', program }}
            path="goal" onCommit={onCommit} />,
        ));
        expect(onCommit).not.toHaveBeenCalled();
        const control = created.find((element) => {
          const props = reactProps(element);
          return props?.['aria-label'] === action
            || (element.tagName === 'BUTTON' && props?.children === action);
        })!;
        expect(control).toBeDefined();
        expect(reactProps(control)?.disabled).toBe(false);
        if (action === 'Add frame reference') {
          await act(() => (reactProps(control)!.onClick as () => void)());
        } else {
          const target = { value: '0.5' };
          await act(() => reactProps(control)!.onFocus!({ target }));
          await act(() => reactProps(control)!.onChange!({ target }));
          await act(() => reactProps(control)!.onBlur!({ target }));
        }
        expect(onCommit).toHaveBeenCalledTimes(1);
        const goal = onCommit.mock.calls[0][0];
        expect(firstCatalogSchemaValidationMessage(schema, goal, 'goal')).toBeNull();
        expect(goal).toEqual({
          position_m: action === 'Position y' ? [0, 0.5, 0] : [0, 0, 0],
          orientation: { rpy_deg: action === 'Orientation y' ? [0, 0, 0.5] : [0, 0, 0] },
          ...(action === 'Add frame reference' ? { frame_ref: { kind: 'cell_frame' } } : {}),
        });
      } finally {
        await act(() => root.unmount());
      }
    },
  );
});

describe('a mounted optional io wait timeout', () => {
  it('omits timeout_s on an empty value and validates a number against the catalog range', async () => {
    const { host, created } = stubDom();
    const onCommit = vi.fn();
    const onOmit = vi.fn();
    const invalid: (string | null)[] = [];
    const schema: CatalogJsonSchema = { type: 'number', minimum: 0.01, maximum: 3600 };
    const entry: CatalogEntry = { ...setValue, id: 'io.wait_for_input', parameters: [{
      id: 'timeout_s', required: false, schema, schemaHash: 'sha256:timeout', hasDefault: false,
    }] };
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    const input = () => {
      const props = created.map(reactProps)
        .filter((props) => props?.placeholder === 'Wait forever').at(-1);
      if (props === undefined) throw new Error('No mounted timeout input');
      return props;
    };
    const type = async (text: string) => {
      await act(() => input().onFocus!({}));
      await act(() => input().onChange!({ target: { value: text } }));
      await act(() => input().onBlur!({}));
    };
    try {
      await act(() => root.render(<ProgramSchemaValueEditor schema={schema} value={5}
        disabled={false} path="timeout_s"
        context={{ entry, node: { node_id: entry.id, instance_id: 'wait_part',
          parameters: { timeout_s: 5 }, children: [] }, parameterId: 'timeout_s', program,
          emptyOmits: { placeholder: 'Wait forever', onOmit } }}
        onCommit={onCommit} onInvalidChange={(message) => invalid.push(message)} />));
      await type('');
      expect(onOmit).toHaveBeenCalledTimes(1);
      expect(onCommit).not.toHaveBeenCalled();
      await type('0');
      expect(onCommit).not.toHaveBeenCalled();
      expect(invalid.at(-1)).toMatch(/timeout_s/);
      await type('2.5');
      expect(onCommit).toHaveBeenCalledWith(2.5);
      expect(onOmit).toHaveBeenCalledTimes(1);
    } finally { await act(() => root.unmount()); }
  });
});
