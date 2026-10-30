import { act } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import type {
  CatalogEntry, CatalogParameter, ProgramAuthoringDocument, ProgramTreeNode,
} from '../types/api';
import { reactPropsHost } from '../test/reactPropsHost';
import { ProgramPropertyEditor } from './ProgramPropertyEditor';
import {
  createdReferenceValue,
  type ProgramReferenceSource,
} from './property/ProgramSchemaValueEditor';

const DEFAULT_POLICY = { raise_alarm: true, stop_motion: true, set_key: null, detach: true };
const onGraspLost: CatalogParameter = {
  id: 'on_grasp_lost',
  required: false,
  schemaHash: 'sha256:on-grasp-lost',
  hasDefault: true,
  default: DEFAULT_POLICY,
  schema: {
    type: 'object',
    additionalProperties: false,
    properties: {
      raise_alarm: { type: 'boolean' },
      stop_motion: { type: 'boolean' },
      set_key: {
        anyOf: [
          { type: 'string', 'x-manyforge-semantic-type': 'blackboard_key' },
          { type: 'null' },
        ],
      },
      detach: { type: 'boolean' },
    },
  },
};

const attachCatalog: CatalogEntry = {
  id: 'scene.attach_carried_object',
  name: 'Attach object',
  category: 'scene',
  description: 'Attach a scene resource to a carrier.',
  stability: 'stable',
  kind: 'leaf',
  childrenMin: 0,
  childrenMax: 0,
  parameters: [onGraspLost],
  requiredCapabilities: [],
  execution: { blocking: true, cancellable: false, ownerApi: 'scene.transaction' },
  runners: { py_trees: 'scene_attach_carried_object_v1' },
  tags: ['scene', 'attachment'],
  minProgramProfile: 'composable-cell-v1',
  blackboard: { reads: [], writes: [] },
  ownerContext: null,
  outcomes: {},
  eventFamily: null,
};

const attachNode: ProgramTreeNode = {
  node_id: 'scene.attach_carried_object',
  instance_id: 'attach_part',
  parameters: {},
  children: [],
};

const program = {
  api_version: 'manyforge.program.v2',
  program_id: 'grasp_lost_ui',
  program_profile: 'composable-cell-v1',
  name: 'Grasp lost UI',
  catalog: { api_version: 'manyforge.node_catalog.v2', content_version: '2.7.0' },
  component_roles: {},
  target_roles: {},
  blackboard: {
    namespaces: [],
    keys: [
      { key: 'loss_flag', type: 'bool', access: 'read_write', persistence: 'run' },
      { key: 'write_only_flag', type: 'bool', access: 'write', persistence: 'run' },
      { key: 'read_only_flag', type: 'bool', access: 'read', persistence: 'run' },
      { key: 'number_flag', type: 'float64', access: 'read_write', persistence: 'run' },
      { key: 'computed_flag', type: 'bool', access: 'read_write', persistence: 'run', expression: 'true' },
    ],
  },
  schema_version: 'manyforge.composer_authoring_forest.v1',
  forest: [attachNode],
  positions: {},
} as unknown as ProgramAuthoringDocument;

afterEach(() => vi.unstubAllGlobals());

beforeEach(() => {
  // `useCardPanelLayout` schedules browser measurements that this React-props
  // host intentionally does not model. Leave the callback pending and stub
  // the observers so the tests exercise editor behavior without a DOM layout.
  vi.stubGlobal('requestAnimationFrame', () => 1);
  vi.stubGlobal('cancelAnimationFrame', () => undefined);
  vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} });
  vi.stubGlobal('MutationObserver', class { observe() {} disconnect() {} });
});

async function mount(node: ProgramTreeNode = attachNode) {
  const onMutate = vi.fn().mockResolvedValue(null);
  const onCreateReference = vi.fn();
  const { host, find, all } = reactPropsHost();
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host as unknown as Element);
  await act(() => root.render(<ProgramPropertyEditor
    roots={[node]}
    selectedId={node.instance_id}
    catalogEntries={[attachCatalog]}
    program={{ ...program, forest: [node] }}
    busy={false}
    onMutate={onMutate}
    onUpdateNodePolicies={vi.fn()}
    onCreateReference={onCreateReference}
  />));
  return {
    onMutate,
    onCreateReference,
    all,
    control: (label: string) => find((props) => props['aria-label'] === label),
    unmount: () => act(() => root.unmount()),
  };
}

function mutationParameters(mounted: Awaited<ReturnType<typeof mount>>) {
  const call = mounted.onMutate.mock.calls.at(-1);
  const args = call?.[1] as Record<string, unknown> | undefined;
  return args?.parameters as Record<string, unknown> | undefined;
}

function renderedText(value: unknown): string {
  if (typeof value === 'string' || typeof value === 'number') return String(value);
  if (Array.isArray(value)) return value.map(renderedText).join('');
  const element = value as { props?: { children?: unknown } } | null;
  return element?.props === undefined ? '' : renderedText(element.props.children);
}

describe('Composer attach-node grasp-lost policy', () => {
  it('shows the current default response without writing an explicit policy', async () => {
    const mounted = await mount();
    try {
      expect(mounted.control('Raise alarm on grasp loss').checked).toBe(true);
      expect(mounted.control('Stop motion on grasp loss').checked).toBe(true);
      expect(mounted.control('Stop motion on grasp loss').disabled).toBe(true);
      expect(mounted.control('Set key on grasp loss').checked).toBe(false);
      expect(mounted.control('Detach object on grasp loss').checked).toBe(true);
      expect(mounted.onMutate).not.toHaveBeenCalled();
    } finally {
      await mounted.unmount();
    }
  });

  it('shows defaults for omitted children in a partial policy without rewriting it', async () => {
    const node: ProgramTreeNode = {
      ...attachNode,
      parameters: { on_grasp_lost: { detach: false } },
    };
    const mounted = await mount(node);
    try {
      expect(mounted.control('Raise alarm on grasp loss').checked).toBe(true);
      expect(mounted.control('Stop motion on grasp loss').checked).toBe(true);
      expect(mounted.control('Set key on grasp loss').checked).toBe(false);
      expect(mounted.control('Detach object on grasp loss').checked).toBe(false);
      expect(mounted.onMutate).not.toHaveBeenCalled();
    } finally {
      await mounted.unmount();
    }
  });

  it('forces Stop with Alarm, and sets a selected writable bool key only after selection', async () => {
    const mounted = await mount();
    try {
      const checkbox = (label: string) => mounted.control(label);
      await act(() => checkbox('Raise alarm on grasp loss').onChange?.({ target: { checked: false } }));
      await act(() => checkbox('Stop motion on grasp loss').onChange?.({ target: { checked: false } }));
      expect(mutationParameters(mounted)?.on_grasp_lost).toEqual({
        raise_alarm: false, stop_motion: false, set_key: null, detach: true,
      });

      await act(() => checkbox('Raise alarm on grasp loss').onChange?.({ target: { checked: true } }));
      expect(mutationParameters(mounted)?.on_grasp_lost).toEqual({
        raise_alarm: true, stop_motion: true, set_key: null, detach: true,
      });
      expect(checkbox('Stop motion on grasp loss').checked).toBe(true);
      expect(checkbox('Stop motion on grasp loss').disabled).toBe(true);

      await act(() => checkbox('Set key on grasp loss').onChange?.({ target: { checked: true } }));
      expect(mounted.onMutate).toHaveBeenCalledTimes(3);
      expect(mounted.all((props) => props['aria-label'] === 'Choose blackboard key for on grasp lost.set key'))
        .toHaveLength(1);
      const optionValues = mounted.all((props) => typeof props.value === 'string'
        && typeof props.children === 'string').map((props) => props.value);
      expect(optionValues).toContain('loss_flag');
      expect(optionValues).toContain('write_only_flag');
      expect(optionValues).not.toContain('read_only_flag');
      expect(optionValues).not.toContain('number_flag');
      expect(optionValues).not.toContain('computed_flag');

      const picker = mounted.control('Choose blackboard key for on grasp lost.set key');
      await act(() => picker.onChange?.({ target: { value: 'loss_flag' } }));
      expect(mutationParameters(mounted)?.on_grasp_lost).toEqual({
        raise_alarm: true, stop_motion: true, set_key: 'loss_flag', detach: true,
      });
      await act(() => checkbox('Set key on grasp loss').onChange?.({ target: { checked: false } }));
      expect(mutationParameters(mounted)?.on_grasp_lost).toEqual({
        raise_alarm: true, stop_motion: true, set_key: null, detach: true,
      });
    } finally {
      await mounted.unmount();
    }
  });

  it('does not persist a stale non-writable key and lets the author clear it', async () => {
    const node: ProgramTreeNode = {
      ...attachNode,
      parameters: { on_grasp_lost: { ...DEFAULT_POLICY, set_key: 'read_only_flag' } },
    };
    const mounted = await mount(node);
    try {
      await act(() => mounted.control('Detach object on grasp loss').onChange?.({ target: { checked: false } }));
      expect(mounted.onMutate).not.toHaveBeenCalled();
      expect(mounted.all((props) => props.role === 'alert').length).toBeGreaterThan(0);
      const keyPicker = mounted.control('Choose blackboard key for on grasp lost.set key');
      expect(keyPicker.title).toBe('Current blackboard key is not a declared writable bool key.');
      expect(mounted.all((props) => props.value === 'read_only_flag'
        && renderedText(props.children) === 'Current (not a declared writable bool key) · read_only_flag'))
        .toHaveLength(1);

      await act(() => mounted.control('Set key on grasp loss').onChange?.({ target: { checked: false } }));
      expect(mutationParameters(mounted)?.on_grasp_lost).toEqual({
        raise_alarm: true, stop_motion: true, set_key: null, detach: true,
      });
    } finally {
      await mounted.unmount();
    }
  });

  it('writes a key created from the picker into set_key only, keeping the other settings', async () => {
    const node: ProgramTreeNode = {
      ...attachNode,
      parameters: { on_grasp_lost: { ...DEFAULT_POLICY, detach: false } },
    };
    const mounted = await mount(node);
    try {
      await act(() => mounted.control('Set key on grasp loss').onChange?.({ target: { checked: true } }));
      await act(() => mounted.control('Choose blackboard key for on grasp lost.set key')
        .onChange?.({ target: { value: '__create__' } }));
      expect(mounted.onMutate).not.toHaveBeenCalled();
      expect(mounted.onCreateReference).toHaveBeenCalledTimes(1);
      const [target, , , source] = mounted.onCreateReference.mock.calls[0] as
        [string, unknown, unknown, ProgramReferenceSource];
      expect(target).toBe('blackboard_key');
      expect(source).toMatchObject({ nodeId: 'attach_part', parameterId: 'on_grasp_lost' });
      // Regression: the created key used to replace the whole on_grasp_lost value.
      expect(createdReferenceValue(source, node.parameters.on_grasp_lost, 'part_dropped')).toEqual({
        raise_alarm: true, stop_motion: true, set_key: 'part_dropped', detach: false,
      });
      expect(createdReferenceValue(source, undefined, 'part_dropped')).toEqual({
        ...DEFAULT_POLICY, set_key: 'part_dropped',
      });
    } finally {
      await mounted.unmount();
    }
  });

  it('keeps a whole-parameter picker writing the created id itself', () => {
    expect(createdReferenceValue(
      { nodeId: 'n', parameterId: 'key', semanticType: 'blackboard_key' }, 'old_key', 'new_key',
    )).toBe('new_key');
  });
});
