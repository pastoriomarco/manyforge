import { describe, expect, it } from 'vitest';
import { motionEndpointKeyOfNode, programTcpSelectorChoices, tcpSelectorKey, type CandidateTcpTarget } from './motionEndpointAuthoring';
import type { ProgramTreeNode } from '../types/api';

const arm = { component_id: 'arm', command_target_id: 'motion' };
const tool = { component_id: 'tool', command_target_id: 'grip' };
const frame = { component_id: 'tool', frame_id: 'TCP' };
const entry = { target: tool, frame };
const rows: CandidateTcpTarget[] = [
  { target: arm, default_tcp: null, tcp_entries: [{ entry, available: true, reason: null, end_effector: true,
    component_default: true, mounted_here: true, mounted_elsewhere: false }] },
  { target: tool, default_tcp: entry, tcp_entries: [] },
];

describe('direct TCP selectors', () => {
  it('uses exact moving-target TCP qualification, including cross-component frames', () => {
    const choices = programTcpSelectorChoices(arm, rows);
    expect(choices.find((choice) => choice.key === tcpSelectorKey({ kind: 'entry', ...entry }))?.disabled).toBeUndefined();
    expect(choices.find((choice) => choice.key === tcpSelectorKey({ kind: 'default_of_target', target: tool }))?.disabled).toBeUndefined();
  });
  it('refuses ambiguous eligibility and absent qualification', () => {
    const duplicate = [{ ...rows[0], tcp_entries: [...rows[0].tcp_entries, ...rows[0].tcp_entries] }, rows[1]];
    expect(programTcpSelectorChoices(arm, duplicate).filter((choice) => choice.key !== tcpSelectorKey({ kind: 'current' }))
      .every((choice) => choice.disabled)).toBe(true);
    expect(programTcpSelectorChoices(arm, null)).toHaveLength(1);
  });
  it('reads a direct frame selector without coercing identity', () => {
    const node = { parameters: { tcp_endpoint: { kind: 'entry', ...entry } } } as unknown as ProgramTreeNode;
    expect(motionEndpointKeyOfNode(node)).toBe(tcpSelectorKey({ kind: 'entry', ...entry }));
  });
});
