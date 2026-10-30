import { act } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import type {
  CatalogEntry, JsonValue, ProgramAuthoringDocument, ProgramTreeNode,
} from '../types/api';
import { reactPropsHost } from '../test/reactPropsHost';
import { ProgramPropertyEditor } from './ProgramPropertyEditor';
import {
  createdReferenceValue,
  type ProgramReferenceSource,
  type ProgramSchemaValueEditorContext,
} from './property/ProgramSchemaValueEditor';
import {
  currentTripPreset,
  TRIP_PRESET_LABELS,
  TripPresetEditor,
  tripPresetNode,
  tripPresetsFor,
  tripRetryLimitNode,
} from './TripPresetEditor';

const CONDITION = { compare: { operand: { key: 'part_seen' }, op: '==', value: true } };

const moveNode: ProgramTreeNode = {
  node_id: 'motion.move_until_signal',
  instance_id: 'approach',
  parameters: { target_role: 'arm', interrupt_condition: CONDITION, stop_mode: 'soft_on_path' },
  value_sources: { speed: { kind: 'program_parameter', reference: 'approach_speed' } },
  operator_stop_policy: 'hold',
  execution_groups: ['left'],
  children: [],
};

const attachNode: ProgramTreeNode = {
  node_id: 'scene.attach_carried_object',
  instance_id: 'attach_part',
  parameters: { target_role: 'gripper' },
  children: [],
};

const entry = (id: string, name: string): CatalogEntry => ({
  id, name, category: 'motion', description: name, stability: 'stable',
  kind: 'leaf', childrenMin: 0, childrenMax: 0, parameters: [],
  requiredCapabilities: [],
  execution: { blocking: true, cancellable: true, ownerApi: 'kernel.operation' },
  runners: { py_trees: `${id}_v1` }, tags: [],
  minProgramProfile: 'composable-cell-v1', blackboard: { reads: [], writes: [] },
  ownerContext: null, outcomes: {}, eventFamily: null,
});

const program = {
  api_version: 'manyforge.program.v2',
  program_id: 'trip_presets',
  program_profile: 'composable-cell-v1',
  name: 'Trip presets',
  catalog: { api_version: 'manyforge.node_catalog.v2', content_version: '2.7.0' },
  component_roles: {},
  target_roles: {},
  parameters: [],
  blackboard: {
    namespaces: [],
    keys: [
      { key: 'part_lost', type: 'bool', access: 'read_write', persistence: 'run' },
      { key: 'read_only_flag', type: 'bool', access: 'read', persistence: 'run' },
    ],
  },
  schema_version: 'manyforge.composer_authoring_forest.v1',
  forest: [moveNode, attachNode],
  positions: {},
} as unknown as ProgramAuthoringDocument;

const keyContext = (node: ProgramTreeNode): ProgramSchemaValueEditorContext => ({
  entry: entry(node.node_id, 'Node'), node, parameterId: 'on_grasp_lost', program,
});

const customMoveNode = (): ProgramTreeNode => ({
  ...moveNode, parameters: { ...moveNode.parameters, on_signal: 'maybe' },
});

describe('trip presets: offered choices', () => {
  it('offers the grasp-lost presets only on Attach object, and the Stop presets only on interrupt nodes', () => {
    expect(tripPresetsFor('motion.move_until_signal'))
      .toEqual(['stop_and_succeed', 'stop_and_fail', 'stop_and_ask_operator']);
    expect(tripPresetsFor('motion.probe_search'))
      .toEqual(['stop_and_succeed', 'stop_and_fail', 'stop_and_ask_operator']);
    expect(tripPresetsFor('scene.attach_carried_object'))
      .toEqual(['stop_and_raise_alarm', 'keep_moving_set_key']);
    expect(tripPresetsFor('timer')).toEqual([]);
    expect(tripPresetsFor('motion.move_to')).toEqual([]);
  });

  it('renders the dropdown with exactly the node\'s choices, and nothing for other nodes', () => {
    const render = (node: ProgramTreeNode) => renderToStaticMarkup(
      <TripPresetEditor node={node} keyContext={keyContext(node)} disabled={false} onReplace={vi.fn()} />,
    );
    const move = render(moveNode);
    expect(move).toContain(TRIP_PRESET_LABELS.stop_and_succeed);
    expect(move).toContain(TRIP_PRESET_LABELS.stop_and_fail);
    expect(move).toContain(TRIP_PRESET_LABELS.stop_and_ask_operator);
    expect(move).not.toContain(TRIP_PRESET_LABELS.keep_moving_set_key);
    expect(move).not.toContain(TRIP_PRESET_LABELS.stop_and_raise_alarm);
    expect(move).toContain('scope everything');
    expect(move).toContain('Open global rules');

    const attach = render(attachNode);
    expect(attach).toContain(TRIP_PRESET_LABELS.keep_moving_set_key);
    expect(attach).toContain(TRIP_PRESET_LABELS.stop_and_raise_alarm);
    expect(attach).not.toContain(TRIP_PRESET_LABELS.stop_and_succeed);
    expect(attach).not.toContain(TRIP_PRESET_LABELS.stop_and_fail);
    expect(attach).not.toContain(TRIP_PRESET_LABELS.stop_and_ask_operator);
    // Out of the box the default alarm + Stop + detach is its own preset, not Custom.
    expect(attach).toContain(
      `<option value="stop_and_raise_alarm" selected="">${TRIP_PRESET_LABELS.stop_and_raise_alarm}</option>`,
    );
    expect(attach).not.toContain('>Custom<');
    expect(render(customMoveNode())).toContain('<option value="custom" disabled="" selected="">Custom</option>');

    expect(render({ ...moveNode, node_id: 'timer' })).toBe('');
  });

  it('disables Open global rules while a Program field edit is invalid, like the Deployment button', () => {
    const markup = (invalidEdit: boolean) => renderToStaticMarkup(
      <TripPresetEditor node={moveNode} keyContext={keyContext(moveNode)} disabled={false}
        invalidEdit={invalidEdit} onReplace={vi.fn()} />,
    );
    expect(markup(false)).not.toContain('Review or discard the invalid Program field edit first.');
    expect(markup(true)).toContain(
      'disabled="" title="Review or discard the invalid Program field edit first.">Open global rules',
    );
  });
});

describe('trip presets: reading the current fields', () => {
  const move = (parameters: Record<string, JsonValue>, extra: Partial<ProgramTreeNode> = {}) => ({
    ...moveNode, parameters: { ...moveNode.parameters, ...parameters }, ...extra,
  }) as ProgramTreeNode;
  const attach = (onGraspLost: JsonValue | undefined, extra: Partial<ProgramTreeNode> = {}) => ({
    ...attachNode,
    parameters: onGraspLost === undefined ? {} : { on_grasp_lost: onGraspLost },
    ...extra,
  }) as ProgramTreeNode;

  it.each([
    ['omitted on_signal (success)', move({}), 'stop_and_succeed'],
    ['on_signal success', move({ on_signal: 'success' }), 'stop_and_succeed'],
    ['success with intervention allowed', move({ on_signal: 'success' },
      { recovery_policy: { allow_intervention: true, max_retries: 2 } }), 'stop_and_succeed'],
    ['failure, default policy', move({ on_signal: 'failure' }), 'stop_and_fail'],
    ['failure, retry limit without intervention', move({ on_signal: 'failure' },
      { recovery_policy: { allow_intervention: false, max_retries: 4 } }), 'stop_and_fail'],
    ['failure with intervention', move({ on_signal: 'failure' },
      { recovery_policy: { allow_intervention: true, max_retries: 0 } }), 'stop_and_ask_operator'],
    ['failure with intervention, own groups', move({ on_signal: 'failure' }, {
      recovery_policy: { allow_intervention: true, max_retries: 3, intervention_stop_scope: 'own_groups' },
    }), 'stop_and_ask_operator'],
    ['an unknown on_signal literal', move({ on_signal: 'maybe' }), 'custom'],
    ['a sourced on_signal', move({}, {
      value_sources: { on_signal: { kind: 'blackboard_key', reference: 'outcome' } },
    }), 'custom'],
    ['probe search, failure', { ...move({ on_signal: 'failure' }), node_id: 'motion.probe_search' },
      'stop_and_fail'],
    ['attach, omitted policy (the default)', attach(undefined), 'stop_and_raise_alarm'],
    ['attach, the explicit default', attach({
      raise_alarm: true, stop_motion: true, set_key: null, detach: true,
    }), 'stop_and_raise_alarm'],
    ['attach, the default without detach', attach({ detach: false }), 'custom'],
    ['attach, Stop without the alarm', attach({ raise_alarm: false }), 'custom'],
    ['attach, the default plus a key', attach({ set_key: 'part_lost' }), 'custom'],
    ['attach, keep moving with a key', attach({
      raise_alarm: false, stop_motion: false, set_key: 'part_lost', detach: true,
    }), 'keep_moving_set_key'],
    ['attach, keep moving without detach', attach({
      raise_alarm: false, stop_motion: false, set_key: 'part_lost', detach: false,
    }), 'keep_moving_set_key'],
    ['attach, partial policy naming a key', attach({
      raise_alarm: false, stop_motion: false, set_key: 'part_lost',
    }), 'keep_moving_set_key'],
    ['attach, keep moving with no key', attach({
      raise_alarm: false, stop_motion: false, set_key: null, detach: true,
    }), 'custom'],
    ['attach, Stop and set a key', attach({
      raise_alarm: false, stop_motion: true, set_key: 'part_lost', detach: true,
    }), 'custom'],
    ['attach, a sourced key', attach({
      raise_alarm: false, stop_motion: false, set_key: 'part_lost', detach: true,
    }, { value_sources: { 'on_grasp_lost.set_key': { kind: 'program_parameter', reference: 'k' } } }), 'custom'],
    ['a node without trip settings', { ...moveNode, node_id: 'timer' }, 'custom'],
  ] as Array<[string, ProgramTreeNode, string]>)('%s', (_label, node, expected) => {
    expect(currentTripPreset(node)).toBe(expected);
  });
});

describe('trip presets: the fields each one writes', () => {
  it('Stop and succeed writes on_signal only', () => {
    const node: ProgramTreeNode = {
      ...moveNode,
      parameters: { ...moveNode.parameters, on_signal: 'failure' },
      recovery_policy: { allow_intervention: true, max_retries: 2 },
    };
    expect(tripPresetNode(node, 'stop_and_succeed')).toEqual({
      ...node, parameters: { ...moveNode.parameters, on_signal: 'success' },
    });
  });

  it('Stop and fail writes on_signal failure and turns intervention off', () => {
    expect(tripPresetNode(moveNode, 'stop_and_fail')).toEqual({
      ...moveNode, parameters: { ...moveNode.parameters, on_signal: 'failure' },
    });
    const held: ProgramTreeNode = {
      ...moveNode,
      recovery_policy: { allow_intervention: true, max_retries: 3, intervention_stop_scope: 'own_groups' },
    };
    // The retry limit is kept; the stop scope goes with intervention, as in the policy editor.
    expect(tripPresetNode(held, 'stop_and_fail')).toEqual({
      ...moveNode,
      parameters: { ...moveNode.parameters, on_signal: 'failure' },
      recovery_policy: { allow_intervention: false, max_retries: 3 },
    });
    // The default policy is omitted, never written explicitly.
    const unlimited: ProgramTreeNode = {
      ...moveNode, recovery_policy: { allow_intervention: true, max_retries: 0 },
    };
    expect(tripPresetNode(unlimited, 'stop_and_fail')).not.toHaveProperty('recovery_policy');
  });

  it('Stop and ask the operator writes on_signal failure and allows intervention', () => {
    expect(tripPresetNode(moveNode, 'stop_and_ask_operator')).toEqual({
      ...moveNode,
      parameters: { ...moveNode.parameters, on_signal: 'failure' },
      recovery_policy: { allow_intervention: true, max_retries: 0 },
    });
    const limited: ProgramTreeNode = {
      ...moveNode,
      parameters: { ...moveNode.parameters, on_signal: 'success' },
      recovery_policy: { allow_intervention: false, max_retries: 5 },
    };
    expect(tripPresetNode(limited, 'stop_and_ask_operator')).toEqual({
      ...limited,
      parameters: { ...moveNode.parameters, on_signal: 'failure' },
      recovery_policy: { allow_intervention: true, max_retries: 5 },
    });
    const scoped: ProgramTreeNode = {
      ...moveNode,
      recovery_policy: { allow_intervention: true, max_retries: 1, intervention_stop_scope: 'own_groups' },
    };
    expect(tripPresetNode(scoped, 'stop_and_ask_operator').recovery_policy)
      .toEqual({ allow_intervention: true, max_retries: 1, intervention_stop_scope: 'own_groups' });
  });

  it('the retry limit writes recovery_policy.max_retries only', () => {
    const held = tripPresetNode(moveNode, 'stop_and_ask_operator');
    expect(tripRetryLimitNode(held, 4)).toEqual({
      ...held, recovery_policy: { allow_intervention: true, max_retries: 4 },
    });
    expect(tripRetryLimitNode(tripRetryLimitNode(held, 4), 0)).toEqual(held);
  });

  it('Stop and raise the alarm writes the default on_grasp_lost', () => {
    const keepMoving: ProgramTreeNode = {
      ...attachNode,
      parameters: {
        ...attachNode.parameters,
        on_grasp_lost: { raise_alarm: false, stop_motion: false, set_key: 'part_lost', detach: false },
      },
      value_sources: { 'on_grasp_lost.set_key': { kind: 'program_parameter', reference: 'k' } },
    };
    expect(tripPresetNode(keepMoving, 'stop_and_raise_alarm')).toEqual({
      ...attachNode,
      parameters: {
        ...attachNode.parameters,
        on_grasp_lost: { raise_alarm: true, stop_motion: true, set_key: null, detach: true },
      },
    });
  });

  it('Keep moving writes no Stop, no alarm and the key, keeping detach', () => {
    expect(tripPresetNode(attachNode, 'keep_moving_set_key', 'part_lost')).toEqual({
      ...attachNode,
      parameters: {
        ...attachNode.parameters,
        on_grasp_lost: { raise_alarm: false, stop_motion: false, set_key: 'part_lost', detach: true },
      },
    });
    const noDetach: ProgramTreeNode = {
      ...attachNode,
      parameters: { ...attachNode.parameters, on_grasp_lost: { detach: false, set_key: 'part_lost' } },
      value_sources: {
        'on_grasp_lost.raise_alarm': { kind: 'program_parameter', reference: 'alarm' },
        speed: { kind: 'program_parameter', reference: 'speed' },
      },
    };
    // The current key is kept when none is given; sources inside on_grasp_lost are dropped.
    expect(tripPresetNode(noDetach, 'keep_moving_set_key')).toEqual({
      ...attachNode,
      parameters: {
        ...attachNode.parameters,
        on_grasp_lost: { raise_alarm: false, stop_motion: false, set_key: 'part_lost', detach: false },
      },
      value_sources: { speed: { kind: 'program_parameter', reference: 'speed' } },
    });
  });
});

describe('TripPresetEditor', () => {
  afterEach(() => vi.unstubAllGlobals());

  async function mount(node: ProgramTreeNode) {
    const onReplace = vi.fn();
    const { host, find, all } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(
      <TripPresetEditor node={node} keyContext={keyContext(node)} disabled={false} onReplace={onReplace} />,
    ));
    return {
      onReplace,
      all,
      control: (label: string) => find((props) => props['aria-label'] === label),
      unmount: () => act(() => root.unmount()),
    };
  }

  it('writes each Stop preset as one replacement and shows the retry limit only for the operator', async () => {
    const mounted = await mount(moveNode);
    try {
      expect(mounted.control('When the signal trips').value).toBe('stop_and_succeed');
      expect(mounted.all((props) => props['aria-label'] === 'Trip retry limit')).toEqual([]);
      await act(() => mounted.control('When the signal trips')
        .onChange?.({ target: { value: 'stop_and_ask_operator' } }));
      expect(mounted.onReplace).toHaveBeenCalledTimes(1);
      expect(mounted.onReplace).toHaveBeenLastCalledWith(tripPresetNode(moveNode, 'stop_and_ask_operator'));
      await act(() => mounted.control('When the signal trips')
        .onChange?.({ target: { value: 'stop_and_fail' } }));
      expect(mounted.onReplace).toHaveBeenLastCalledWith(tripPresetNode(moveNode, 'stop_and_fail'));
    } finally {
      await mounted.unmount();
    }
  });

  it('shows and writes the retry limit while the operator is asked', async () => {
    const held = tripPresetNode(moveNode, 'stop_and_ask_operator');
    const mounted = await mount(held);
    try {
      expect(mounted.control('When the signal trips').value).toBe('stop_and_ask_operator');
      expect(mounted.control('Trip retry limit').value).toBe('unlimited');
      await act(() => mounted.control('Trip retry limit').onChange?.({ target: { value: 'limited' } }));
      expect(mounted.onReplace).not.toHaveBeenCalled();
      const input = mounted.control('Trip retries per run');
      await act(() => (input.onFocus as ((event: unknown) => void) | undefined)?.({}));
      await act(() => input.onChange?.({ target: { value: '3' } }));
      await act(() => (mounted.control('Trip retries per run').onBlur as (event: unknown) => void)({}));
      expect(mounted.onReplace).toHaveBeenCalledTimes(1);
      expect(mounted.onReplace).toHaveBeenLastCalledWith(tripRetryLimitNode(held, 3));
    } finally {
      await mounted.unmount();
    }
  });

  it('asks for a key before Keep moving writes anything, then writes it once', async () => {
    const mounted = await mount(attachNode);
    try {
      expect(mounted.control('When the signal trips').value).toBe('stop_and_raise_alarm');
      expect(mounted.all((props) => props['aria-label'] === 'Choose blackboard key for trip key'))
        .toEqual([]);
      await act(() => mounted.control('When the signal trips')
        .onChange?.({ target: { value: 'keep_moving_set_key' } }));
      expect(mounted.onReplace).not.toHaveBeenCalled();
      expect(mounted.control('When the signal trips').value).toBe('keep_moving_set_key');
      const optionValues = mounted.all((props) => typeof props.value === 'string'
        && typeof props.children === 'string').map((props) => props.value);
      expect(optionValues).toContain('part_lost');
      expect(optionValues).not.toContain('read_only_flag');
      await act(() => mounted.control('Choose blackboard key for trip key')
        .onChange?.({ target: { value: 'part_lost' } }));
      expect(mounted.onReplace).toHaveBeenCalledTimes(1);
      expect(mounted.onReplace).toHaveBeenLastCalledWith(
        tripPresetNode(attachNode, 'keep_moving_set_key', 'part_lost'),
      );
    } finally {
      await mounted.unmount();
    }
  });
});

describe('TripPresetEditor key creation', () => {
  afterEach(() => vi.unstubAllGlobals());

  it('creates a key with the Keep moving fields, writing nothing itself', async () => {
    const onReplace = vi.fn();
    const onCreateReference = vi.fn();
    const node: ProgramTreeNode = {
      ...attachNode,
      parameters: { ...attachNode.parameters, on_grasp_lost: { detach: false } },
    };
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<TripPresetEditor node={node}
      keyContext={{ ...keyContext(node), onCreateReference }} disabled={false} onReplace={onReplace} />));
    try {
      await act(() => find((props) => props['aria-label'] === 'When the signal trips')
        .onChange?.({ target: { value: 'keep_moving_set_key' } }));
      await act(() => find((props) => props['aria-label'] === 'Choose blackboard key for trip key')
        .onChange?.({ target: { value: '__create__' } }));
      expect(onReplace).not.toHaveBeenCalled();
      expect(onCreateReference).toHaveBeenCalledTimes(1);
      const source = onCreateReference.mock.calls[0]?.[3] as ProgramReferenceSource;
      expect(source).toMatchObject({ nodeId: 'attach_part', parameterId: 'on_grasp_lost' });
      expect(createdReferenceValue(source, node.parameters.on_grasp_lost, 'part_dropped')).toEqual({
        raise_alarm: false, stop_motion: false, set_key: 'part_dropped', detach: false,
      });
    } finally {
      await act(() => root.unmount());
    }
  });
});

describe('ProgramPropertyEditor trip preset', () => {
  afterEach(() => vi.unstubAllGlobals());
  beforeEach(() => {
    // See GraspLostPolicyEditor.test.tsx: no DOM layout in the props host.
    vi.stubGlobal('requestAnimationFrame', () => 1);
    vi.stubGlobal('cancelAnimationFrame', () => undefined);
    vi.stubGlobal('ResizeObserver', class { observe() {} disconnect() {} });
    vi.stubGlobal('MutationObserver', class { observe() {} disconnect() {} });
  });

  it('sends a preset as one tree_draft_replace_subtree of the selected node', async () => {
    const node: ProgramTreeNode = {
      node_id: 'motion.move_until_signal', instance_id: 'approach', parameters: {}, children: [],
    };
    const onMutate = vi.fn().mockResolvedValue(null);
    const onUpdateNodePolicies = vi.fn();
    const { host, find } = reactPropsHost();
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(host as unknown as Element);
    await act(() => root.render(<ProgramPropertyEditor
      roots={[node]}
      selectedId={node.instance_id}
      catalogEntries={[entry('motion.move_until_signal', 'Move until signal')]}
      program={{ ...program, forest: [node] }}
      busy={false}
      onMutate={onMutate}
      onUpdateNodePolicies={onUpdateNodePolicies}
    />));
    try {
      await act(() => find((props) => props['aria-label'] === 'When the signal trips')
        .onChange?.({ target: { value: 'stop_and_ask_operator' } }));
      expect(onMutate).toHaveBeenCalledTimes(1);
      expect(onMutate).toHaveBeenCalledWith('tree_draft_replace_subtree', {
        instance_id: 'approach',
        subtree: tripPresetNode(node, 'stop_and_ask_operator'),
      });
      expect(onUpdateNodePolicies).not.toHaveBeenCalled();
    } finally {
      await act(() => root.unmount());
    }
  });
});
