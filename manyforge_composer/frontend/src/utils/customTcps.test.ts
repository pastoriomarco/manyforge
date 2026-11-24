import { describe, expect, it } from 'vitest';

import type { JsonValue } from '../types/api';
import {
  customTcpParentFrames,
  customTcpProblem,
  customTcpsEdit,
  customTcpsOf,
  type CustomTcp,
} from './customTcps';

const pen = {
  tcp_id: 'pen',
  parent_frame: 'flange',
  pose: { position_m: [0, 0, 0.12], orientation: { rpy_deg: [0, 0, 90] } },
};

function document(customTcps?: JsonValue[]): Record<string, JsonValue> {
  return {
    api_version: 'manyforge.deployment_intent.v3',
    components: [
      { component_id: 'station_io', component_profile_id: 'io_device_v0' },
      {
        component_id: 'lite6_robot', component_profile_id: 'lite6_mock_commanded_v0',
        ...(customTcps === undefined ? {} : { custom_tcps: customTcps }),
      },
    ],
    attachments: [],
  };
}

const frames = ['base', 'flange', 'tcp'];

describe('custom TCPs', () => {
  it('reads the component row and its pose', () => {
    expect(customTcpsOf(document([pen]), 'lite6_robot')).toEqual([{
      tcpId: 'pen', parentFrame: 'flange', positionM: [0, 0, 0.12], rpyDeg: [0, 0, 90], contactBodies: null,
      stored: pen,
    }]);
    expect(customTcpsOf(document(), 'lite6_robot')).toEqual([]);
  });

  it('reads and writes the contact bodies; no key is the default, an empty list is none', () => {
    const withBodies = { ...pen, contact_bodies: ['gripper_fingers'] };
    const none = { ...pen, tcp_id: 'bare', contact_bodies: [] };
    expect(customTcpsOf(document([none, withBodies]), 'lite6_robot').map((tcp) => tcp.contactBodies))
      .toEqual([[], ['gripper_fingers']]);
    // A list that is not text does not read; the row is kept untouched.
    expect(customTcpsOf(document([{ ...pen, contact_bodies: [1] }]), 'lite6_robot')).toEqual([]);
    const probe: CustomTcp = { tcpId: 'probe', parentFrame: 'tcp', positionM: [0, 0, 0], rpyDeg: [0, 0, 0],
      contactBodies: ['pads', 'fingers'] };
    expect(customTcpsEdit(document(), 'lite6_robot', [probe])?.value).toEqual([{
      tcp_id: 'probe', parent_frame: 'tcp',
      pose: { position_m: [0, 0, 0], orientation: { rpy_deg: [0, 0, 0] } },
      contact_bodies: ['fingers', 'pads'],
    }]);
    expect(customTcpsEdit(document(), 'lite6_robot', [{ ...probe, contactBodies: [] }])?.value)
      .toEqual([expect.objectContaining({ contact_bodies: [] })]);
    expect(customTcpProblem({ ...probe, contactBodies: ['pads', 'pads'] }, [], frames, null))
      .toBe('Each contact body can be listed once.');
  });

  it('offers only the component\'s own frames as parents, never a custom TCP', () => {
    const custom = customTcpsOf(document([pen]), 'lite6_robot');
    // An Applied custom TCP is also a frame of the component: it is still excluded.
    expect(customTcpParentFrames([...frames, 'pen'], custom)).toEqual(['base', 'flange', 'tcp']);
    const onPen: CustomTcp = { tcpId: 'probe', parentFrame: 'pen', positionM: [0, 0, 0], rpyDeg: [0, 0, 0], contactBodies: null };
    expect(customTcpProblem(onPen, custom, frames, null)).toBe('Pick the parent from this component\'s frames.');
  });

  it('refuses ids that clash with a model frame or another custom TCP', () => {
    const custom = customTcpsOf(document([pen]), 'lite6_robot');
    const row = (tcpId: string): CustomTcp => ({ tcpId, parentFrame: 'flange', positionM: [0, 0, 0], rpyDeg: [0, 0, 0], contactBodies: null });
    expect(customTcpProblem(row('flange'), custom, frames, null)).toContain('already a frame');
    expect(customTcpProblem(row('pen'), custom, frames, null)).toContain('already a custom TCP');
    // Editing pen keeps its own id.
    expect(customTcpProblem(row('pen'), custom, frames, 'pen')).toBeNull();
    expect(customTcpProblem(row('Pen-1'), custom, frames, null)).toContain('lower-case');
    expect(customTcpProblem({ ...row('probe'), positionM: [Number.NaN, 0, 0] }, custom, frames, null))
      .toContain('numbers');
  });

  it('produces one Draft edit of the component row', () => {
    const probe: CustomTcp = { tcpId: 'probe', parentFrame: 'tcp', positionM: [0.01, 0, 0.05], rpyDeg: [0, 90, 0], contactBodies: null };
    expect(customTcpsEdit(document(), 'lite6_robot', [probe])).toEqual({
      path: ['components', 1, 'custom_tcps'],
      operation: 'set',
      value: [{
        tcp_id: 'probe', parent_frame: 'tcp',
        pose: { position_m: [0.01, 0, 0.05], orientation: { rpy_deg: [0, 90, 0] } },
      }],
    });
    // Deleting the last one removes the field; nothing to remove is no edit.
    expect(customTcpsEdit(document([pen]), 'lite6_robot', [])).toEqual({
      path: ['components', 1, 'custom_tcps'], operation: 'remove',
    });
    expect(customTcpsEdit(document(), 'lite6_robot', [])).toBeNull();
    expect(customTcpsEdit(document(), 'missing', [probe])).toBeNull();
    // Stored sorted by id, as both intent validators require.
    const added = customTcpsEdit(document([pen]), 'lite6_robot', [probe, ...customTcpsOf(document([pen]), 'lite6_robot')]);
    expect((added?.value as Array<{ tcp_id: string }>).map((row) => row.tcp_id)).toEqual(['pen', 'probe']);
  });

  it('reads an xyzw orientation at gimbal lock back to the same rotation (yaw 0)', () => {
    const xyzw = (rollDeg: number, pitchDeg: number, yawDeg: number) => {
      const [r, p, y] = [rollDeg, pitchDeg, yawDeg].map((value) => value * Math.PI / 360);
      const [cr, sr, cp, sp, cy, sy] = [Math.cos(r), Math.sin(r), Math.cos(p), Math.sin(p), Math.cos(y), Math.sin(y)];
      return [sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy];
    };
    const read = (rpy: [number, number, number]) => customTcpsOf(document([{
      tcp_id: 'probe', parent_frame: 'flange',
      pose: { position_m: [0, 0, 0], orientation: { xyzw: xyzw(...rpy) } },
    }]), 'lite6_robot')[0].rpyDeg;
    // Pitch +90: only roll - yaw is defined; pitch -90: only roll + yaw.
    for (const [rpy, expected] of [
      [[30, 90, 0], [30, 90, 0]],
      [[10, 90, 40], [-30, 90, 0]],
      [[10, -90, 40], [50, -90, 0]],
      [[12, 34, -56], [12, 34, -56]],
    ] as Array<[[number, number, number], [number, number, number]]>) {
      const value = read(rpy);
      value.forEach((angle, index) => expect(angle).toBeCloseTo(expected[index], 5));
    }
  });

  it('keeps rows that do not read, untouched, through any edit', () => {
    const unreadable = { tcp_id: 'odd', parent_frame: 'flange', pose: { position_m: [0, 0] } };
    const stored = customTcpsOf(document([pen, unreadable]), 'lite6_robot');
    expect(stored.map((tcp) => tcp.tcpId)).toEqual(['pen']);
    // Deleting the last readable row keeps the unreadable one instead of removing the field.
    expect(customTcpsEdit(document([pen, unreadable]), 'lite6_robot', [])).toEqual({
      path: ['components', 1, 'custom_tcps'], operation: 'set', value: [unreadable],
    });
    const probe: CustomTcp = { tcpId: 'probe', parentFrame: 'tcp', positionM: [0, 0, 0.05], rpyDeg: [0, 0, 0], contactBodies: null };
    expect(customTcpsEdit(document([pen, unreadable]), 'lite6_robot', [...stored, probe])?.value).toEqual([
      pen,
      { tcp_id: 'probe', parent_frame: 'tcp', pose: { position_m: [0, 0, 0.05], orientation: { rpy_deg: [0, 0, 0] } } },
      unreadable,
    ]);
  });

  it('writes the rows it did not edit back exactly as stored, a quaternion row included', () => {
    const quaternion = {
      tcp_id: 'probe', parent_frame: 'flange',
      pose: { position_m: [0, 0, 0.1], orientation: { xyzw: [0, 0, 0.7071068, 0.7071068] } },
    };
    const stored = customTcpsOf(document([pen, quaternion]), 'lite6_robot');
    const deleted = customTcpsEdit(document([pen, quaternion]), 'lite6_robot', stored.filter((row) => row.tcpId !== 'pen'));
    expect(deleted?.value).toEqual([quaternion]);
    const edited: CustomTcp = { tcpId: 'pen', parentFrame: 'tcp', positionM: [0, 0, 0.2], rpyDeg: [0, 0, 0], contactBodies: null };
    const replaced = customTcpsEdit(document([pen, quaternion]), 'lite6_robot',
      stored.map((row) => (row.tcpId === 'pen' ? edited : row)));
    expect(replaced?.value).toEqual([
      { tcp_id: 'pen', parent_frame: 'tcp', pose: { position_m: [0, 0, 0.2], orientation: { rpy_deg: [0, 0, 0] } } },
      quaternion,
    ]);
  });
});
