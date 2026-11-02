import { describe, expect, it } from 'vitest';

import type { JsonValue, SourceModelProjection } from '../types/api';
import { fromDraft, fromSourceModel } from './kinematicIndex';

// lite6 + lite_gripper (flange) + a camera on the gripper's tool interface;
// ur10e stands alone.
const applied: Pick<SourceModelProjection, 'components'> = {
  components: [
    { component_id: 'camera', mounted_on: { component_id: 'lite_gripper', interface_id: 'tool' } },
    { component_id: 'lite6_robot', mounted_on: null },
    { component_id: 'lite_gripper', mounted_on: { component_id: 'lite6_robot', interface_id: 'flange' } },
    { component_id: 'ur10e_robot', mounted_on: null },
  ],
};

function draft(attachments: Array<[string, string, string]>): Record<string, JsonValue> {
  return {
    api_version: 'manyforge.deployment_intent.v2',
    components: ['lite6_robot', 'lite_gripper', 'camera', 'ur10e_robot'].map((id) => ({ component_id: id })),
    attachments: attachments.map(([parent, parentInterface, child]) => ({
      parent: { component_id: parent, interface: parentInterface },
      child: { component_id: child, interface: 'mount' },
    })),
  };
}

describe('kinematic index', () => {
  it('answers the mount queries from the applied source model', () => {
    const index = fromSourceModel(applied);

    expect(index.mountedOn('lite_gripper')).toEqual({ componentId: 'lite6_robot', interfaceId: 'flange' });
    expect(index.mountedOn('lite6_robot')).toBeNull();
    expect(index.toolOn('lite6_robot', 'flange')).toBe('lite_gripper');
    expect(index.toolOn('lite6_robot', 'tool')).toBeNull();
    expect(index.childrenOf('lite6_robot')).toEqual(['lite_gripper']);
    expect(index.ancestorsOf('camera')).toEqual(['lite_gripper', 'lite6_robot']);
    expect(index.ancestorsOf('ur10e_robot')).toEqual([]);
    expect(index.carries('lite6_robot', 'camera')).toBe(true);
    expect(index.carries('camera', 'lite6_robot')).toBe(false);
    expect(index.carries('ur10e_robot', 'lite_gripper')).toBe(false);
    expect(index.carries('lite6_robot', 'lite6_robot')).toBe(false);
    expect(index.selfAndMounted('lite6_robot')).toEqual(['lite6_robot', 'lite_gripper', 'camera']);
    expect(index.selfAndMounted('ur10e_robot')).toEqual(['ur10e_robot']);
  });

  it('gives the same answers from the equivalent Draft', () => {
    const index = fromDraft(draft([
      ['lite6_robot', 'flange', 'lite_gripper'],
      ['lite_gripper', 'tool', 'camera'],
    ]));

    expect(index.toolOn('lite6_robot', 'flange')).toBe('lite_gripper');
    expect(index.ancestorsOf('camera')).toEqual(['lite_gripper', 'lite6_robot']);
    expect(index.selfAndMounted('lite6_robot')).toEqual(['lite6_robot', 'lite_gripper', 'camera']);
  });

  it('keeps an unsaved Draft mount out of the Applied view', () => {
    const draftIndex = fromDraft(draft([
      ['lite6_robot', 'flange', 'lite_gripper'],
      ['ur10e_robot', 'flange', 'camera'],
    ]));
    const appliedIndex = fromSourceModel(applied);

    expect(draftIndex.mountedOn('camera')).toEqual({ componentId: 'ur10e_robot', interfaceId: 'flange' });
    expect(draftIndex.toolOn('ur10e_robot', 'flange')).toBe('camera');
    expect(appliedIndex.toolOn('ur10e_robot', 'flange')).toBeNull();
    expect(appliedIndex.mountedOn('camera')).toEqual({ componentId: 'lite_gripper', interfaceId: 'tool' });
  });

  it('takes the primary tool of a shared interface, else the first (B5)', () => {
    const shared = draft([
      ['lite6_robot', 'flange', 'lite_gripper'],
      ['lite6_robot', 'flange', 'camera'],
    ]);
    expect(fromDraft(shared).toolOn('lite6_robot', 'flange')).toBe('lite_gripper');
    ((shared.attachments as Array<Record<string, JsonValue>>)[1]).primary = true;
    expect(fromDraft(shared).toolOn('lite6_robot', 'flange')).toBe('camera');
  });

  it('reads an empty or non-intent Draft as no mounts', () => {
    expect(fromDraft(null).toolOn('lite6_robot', 'flange')).toBeNull();
    expect(fromDraft({ attachments: 'none' }).selfAndMounted('lite6_robot')).toEqual(['lite6_robot']);
  });
});
