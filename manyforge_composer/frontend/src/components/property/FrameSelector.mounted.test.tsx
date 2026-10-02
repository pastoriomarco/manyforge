import { act, useState, type ReactElement } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import type {
  BlackboardKeyDeclaration,
  CatalogEntry,
  CatalogJsonSchema,
  JsonValue,
  ProgramAuthoringDocument,
  ProgramTreeNode,
} from '../../types/api';
import { ProgramDeclarationsEditor } from '../ProgramDeclarationsEditor';
import { ProgramSchemaValueEditor } from './ProgramSchemaValueEditor';

afterEach(() => {
  vi.unstubAllGlobals();
});

type FakeNode = Record<string, unknown>;
type Handler = (event: unknown) => void;
type ReactProps = Record<string, unknown> & {
  onChange?: Handler;
  onClick?: Handler;
  onBlur?: Handler;
};

// The minimal React DOM host of ProgramSchemaValueEditor.mounted.test.tsx:
// real commits, state and effects; every created element is kept so a test
// finds a mounted control and calls its React handlers.
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
  // Card-panel layout only measures; it never runs its frame here.
  vi.stubGlobal('requestAnimationFrame', () => 0);
  vi.stubGlobal('cancelAnimationFrame', () => undefined);
  const observer = class { observe() {} disconnect() {} };
  vi.stubGlobal('ResizeObserver', observer);
  vi.stubGlobal('MutationObserver', observer);
  return { host: node('div'), created };
}

function reactProps(element: FakeNode): ReactProps | undefined {
  const key = Object.keys(element).find((name) => name.startsWith('__reactProps$'));
  return key === undefined ? undefined : element[key] as ReactProps;
}

async function mount(element: ReactElement) {
  const { host, created } = stubDom();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => { root.render(element); });
  const find = (predicate: (props: ReactProps) => boolean) => created
    .filter((candidate) => {
      const props = reactProps(candidate);
      return props !== undefined && predicate(props);
    })
    .at(-1);
  const control = (name: string) => {
    const found = find((props) => props['aria-label'] === name || props.id === name);
    if (found === undefined) throw new Error(`No control ${name}`);
    return found;
  };
  return {
    control,
    exists: (name: string) => find((props) => props['aria-label'] === name) !== undefined,
    find,
    choose: async (name: string, value: string) => {
      const target = { value };
      await act(() => { reactProps(control(name))!.onChange!({ target, currentTarget: target }); });
    },
    click: async (name: string) => {
      await act(() => { reactProps(control(name))!.onClick!({}); });
    },
    unmount: async () => { await act(() => { root.unmount(); }); },
  };
}

/** The `<option>` values one mounted select renders, in order, with their groups. */
function optionValues(select: FakeNode): Array<[string, string]> {
  const rows: Array<[string, string]> = [];
  const visit = (child: unknown, group: string) => {
    if (Array.isArray(child)) {
      child.forEach((item) => visit(item, group));
      return;
    }
    if (child === null || typeof child !== 'object' || !('props' in child)) return;
    const element = child as { type: unknown; props: Record<string, unknown> };
    if (element.type === 'optgroup') {
      visit(element.props.children, String(element.props.label));
    } else if (element.type === 'option') {
      rows.push([group, String(element.props.value)]);
    }
  };
  visit(reactProps(select)!.children, '');
  return rows;
}

const program = {
  target_roles: {
    arm: { required_frame_roles: ['base', 'tcp'] },
    peer: { required_frame_roles: ['base', 'flange'] },
  },
  scene: [{ scene_resource_id: 'table' }],
  blackboard: { namespaces: [], keys: [] },
  parameters: [],
} as unknown as ProgramAuthoringDocument;

function entry(id: string, parameters: CatalogEntry['parameters']): CatalogEntry {
  return {
    id,
    name: id,
    category: 'motion',
    description: '',
    stability: 'stable',
    kind: 'leaf',
    childrenMin: 0,
    childrenMax: 0,
    parameters,
    requiredCapabilities: [],
    execution: { blocking: true, cancellable: true, ownerApi: 'kernel.operation' },
    runners: {},
    tags: [],
    minProgramProfile: null,
    blackboard: { reads: [], writes: [] },
    ownerContext: null,
    outcomes: {},
    eventFamily: null,
  };
}

const MOTION_REFERENCE: CatalogJsonSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['type'],
  properties: {
    type: {
      type: 'string',
      enum: ['frame_role', 'component_body_role', 'scene_resource', 'runtime_object', 'pose', 'cell_frame'],
    },
    frame_role_id: { type: 'string', minLength: 1 },
    target_role: { type: 'string', minLength: 1 },
    collision_body_role_id: { type: 'string', minLength: 1 },
    scene_resource_id: { type: 'string', minLength: 1 },
    runtime_object_id: { type: 'string', minLength: 1 },
    pose: { type: 'object' },
  },
};

async function mountReference(initial: JsonValue) {
  const committed: JsonValue[] = [];
  const move = entry('motion.move_to_pose', [
    { id: 'target_role', required: true, schema: { type: 'string', 'x-manyforge-semantic-type': 'target_role' }, schemaHash: 'sha256:t', hasDefault: false },
    { id: 'target_reference', required: false, schema: MOTION_REFERENCE, schemaHash: 'sha256:r', hasDefault: false },
  ]);
  function Host() {
    const [value, setValue] = useState<JsonValue>(initial);
    const node: ProgramTreeNode = {
      node_id: move.id,
      instance_id: 'move',
      parameters: { target_role: 'arm', target_reference: value },
      children: [],
    };
    return (
      <ProgramSchemaValueEditor
        schema={MOTION_REFERENCE}
        value={value}
        disabled={false}
        context={{ entry: move, node, parameterId: 'target_reference', program }}
        path="target_reference"
        onCommit={(next) => {
          committed.push(next);
          setValue(next);
        }}
      />
    );
  }
  return { ...(await mount(<Host />)), committed };
}

describe('one frame selector', () => {
  it('lists the moving robot first, other robots by name, then the cell and Scene objects', async () => {
    const editor = await mountReference({ type: 'frame_role', frame_role_id: 'tcp' });
    expect(optionValues(editor.control('Motion reference frame'))).toEqual([
      ['Moving robot · arm', 'robot:arm:base'],
      ['Moving robot · arm', 'robot:arm:tcp'],
      ['Other robots', 'robot:peer:base'],
      ['Other robots', 'robot:peer:flange'],
      ['Cell', 'cell'],
      ['Scene objects', 'scene:table'],
    ]);
    expect(reactProps(editor.control('Motion reference frame'))!.value).toBe('robot:arm:tcp');
    // No free-text reference field remains for frames.
    expect(editor.exists('Motion reference frame_role_id')).toBe(false);
    await editor.unmount();
  });

  it('writes the own-robot, other-robot, cell and Scene object reference forms', async () => {
    const editor = await mountReference({ type: 'frame_role', frame_role_id: 'tcp' });
    await editor.choose('Motion reference frame', 'robot:peer:flange');
    await editor.choose('Motion reference frame', 'robot:arm:base');
    await editor.choose('Motion reference frame', 'cell');
    await editor.choose('Motion reference frame', 'scene:table');
    expect(editor.committed).toEqual([
      { type: 'frame_role', frame_role_id: 'flange', target_role: 'peer' },
      { type: 'frame_role', frame_role_id: 'base' },
      { type: 'cell_frame' },
      { type: 'scene_resource', scene_resource_id: 'table' },
    ]);
    expect(reactProps(editor.control('Motion reference frame'))!.value).toBe('scene:table');
    await editor.unmount();
  });
});

const POSE_VALUE: CatalogJsonSchema = {
  anyOf: [
    { type: 'number' },
    {
      type: 'object',
      properties: { frame_ref: { type: 'object' }, position_m: { type: 'array' }, orientation: { type: 'object' } },
    },
  ],
  'x-manyforge-value-type-from-key': 'key',
};

describe('a key-typed pose with allowed_frames', () => {
  it('offers only the allowed frames and refuses another until one is chosen', async () => {
    const keys: BlackboardKeyDeclaration[] = [{
      key: 'place',
      type: 'stored_pose',
      access: 'read_write',
      persistence: 'run',
      allowed_frames: [
        { kind: 'scene_resource', scene_resource_id: 'table' },
        { kind: 'target_frame', target_role: 'peer', frame_role: 'flange' },
      ],
    }];
    const setValue = entry('logic.set_value', [
      { id: 'key', required: true, schema: { type: 'string', 'x-manyforge-semantic-type': 'blackboard_key' }, schemaHash: 'sha256:k', hasDefault: false },
      { id: 'value', required: true, schema: POSE_VALUE, schemaHash: 'sha256:v', hasDefault: false },
    ]);
    const committed: JsonValue[] = [];
    let invalid: string | null = null;
    const initial: JsonValue = {
      frame_ref: { kind: 'cell_frame' },
      position_m: [0, 0, 0],
      orientation: { rpy_deg: [0, 0, 0] },
    };
    function Host() {
      const [value, setStored] = useState<JsonValue>(initial);
      return (
        <ProgramSchemaValueEditor
          schema={POSE_VALUE}
          value={value}
          disabled={false}
          context={{
            entry: setValue,
            node: { node_id: setValue.id, instance_id: 'set', parameters: { key: 'place', value }, children: [] },
            parameterId: 'value',
            program: { ...program, blackboard: { namespaces: [], keys } },
          }}
          path="value"
          onCommit={(next) => {
            committed.push(next);
            setStored(next);
          }}
          onInvalidChange={(message) => { invalid = message; }}
        />
      );
    }
    const editor = await mount(<Host />);
    const select = editor.control('Frame reference');
    expect(optionValues(select)).toEqual([
      ['', 'cell'],
      ['Robots', 'robot:peer:flange'],
      ['Scene objects', 'scene:table'],
    ]);
    // The current cell frame is shown as not available and blocks the edit.
    expect(reactProps(select)!['aria-invalid']).toBe(true);
    expect(invalid).toBe('Choose a frame this value may use.');

    await editor.choose('Frame reference', 'scene:table');
    expect(committed).toEqual([{
      ...initial as object,
      frame_ref: { kind: 'scene_resource', scene_resource_id: 'table' },
    }]);
    expect(invalid).toBeNull();
    await editor.unmount();
  });
});

describe('a map with entries supplied only by a value source', () => {
  it('lists the sourced entries and refuses a typed entry of the same name', async () => {
    const MAP: CatalogJsonSchema = { type: 'object', additionalProperties: { type: 'string' } };
    const query = entry('assistant.query_model', [
      { id: 'template_variables', required: true, schema: MAP, schemaHash: 'sha256:m', hasDefault: false },
    ]);
    const node: ProgramTreeNode = {
      node_id: query.id,
      instance_id: 'query',
      parameters: { template_variables: { count: '1' } },
      value_sources: {
        'template_variables.label': { kind: 'program_parameter', reference: 'label' },
        'template_variables.ratio': { kind: 'blackboard_key', reference: 'ratio' },
      },
      children: [],
    } as unknown as ProgramTreeNode;
    const committed: JsonValue[] = [];
    const editor = await mount(
      <ProgramSchemaValueEditor
        schema={MAP}
        value={{ count: '1' }}
        disabled={false}
        context={{ entry: query, node, parameterId: 'template_variables', program }}
        path="template_variables"
        onCommit={(next) => committed.push(next)}
      />,
    );
    expect(reactProps(editor.control('template_variables key count'))!.value).toBe('count');
    for (const key of ['label', 'ratio']) {
      const row = editor.control(`template_variables key ${key}`);
      expect(reactProps(row)!.readOnly).toBe(true);
      expect(reactProps(row)!.value).toBe(key);
    }

    await editor.choose('New template_variables key', 'label');
    const add = editor.find((props) => props.children === 'Add');
    await act(() => { reactProps(add!)!.onClick!({}); });
    expect(committed).toEqual([]);
    expect(editor.find((props) => props.role === 'alert')).toBeDefined();
    await editor.unmount();
  });
});

describe('the allowed_frames editor of a pose key', () => {
  it('adds frames in canonical order and drops the list when the last frame is removed', async () => {
    const mutations: Array<[string, Record<string, JsonValue>]> = [];
    const editor = await mount(
      <ProgramDeclarationsEditor
        embedded
        selection={{ kind: 'key', id: 'place' }}
        showNavigation={false}
        roots={[]}
        parameters={[]}
        namespaces={[]}
        keys={[{
          key: 'place',
          type: 'stored_pose',
          access: 'read_write',
          persistence: 'run',
          allowed_frames: [{ kind: 'target_frame', target_role: 'arm', frame_role: 'tcp' }],
        }]}
        formulaReferences={{ computedKeys: {}, nodes: {} }}
        busy={false}
        frameProgram={program}
        onMutate={(mutation, args) => { mutations.push([mutation, args]); }}
      />,
    );
    const save = async () => {
      const form = editor.find((props) =>
        typeof props.className === 'string' && props.className.startsWith('program-declaration-form'));
      await act(() => { reactProps(form!)!.onBlur!({}); });
    };
    // Already allowed frames are not offered again.
    expect(optionValues(editor.control('Add allowed frame')).map(([, value]) => value))
      .toEqual(['', 'robot:arm:base', 'robot:peer:base', 'robot:peer:flange', 'cell', 'scene:table']);

    await editor.choose('Add allowed frame', 'scene:table');
    await editor.choose('Add allowed frame', 'cell');
    await save();
    expect(mutations.at(-1)).toEqual(['blackboard_draft_upsert_keys', {
      keys: [{
        key: 'place',
        type: 'stored_pose',
        access: 'read_write',
        persistence: 'run',
        allowed_frames: [
          { kind: 'cell_frame' },
          { kind: 'scene_resource', scene_resource_id: 'table' },
          { kind: 'target_frame', target_role: 'arm', frame_role: 'tcp' },
        ],
      }],
    }]);

    for (const label of ['Cell', 'table', 'arm · tcp']) {
      await editor.click(`Remove allowed frame ${label}`);
    }
    await save();
    expect(mutations.at(-1)).toEqual(['blackboard_draft_upsert_keys', {
      keys: [{ key: 'place', type: 'stored_pose', access: 'read_write', persistence: 'run' }],
    }]);
    await editor.unmount();
  });
});
