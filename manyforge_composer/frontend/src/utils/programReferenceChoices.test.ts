import { describe, expect, it } from 'vitest';

import type { ProgramAuthoringDocument, ProgramDocument } from '../types/api';
import type { CandidateTarget, ReferenceSelectorFacts } from './referenceValidation';
import { programFrameChoices } from './programFrames';
import {
  programCollisionBodyReferenceChoices,
  programFrameReferenceChoices,
  programFrameScopeFromSelectorFacts,
  programSceneResourceBlackboardKeys,
  programSceneResourceIds,
} from './programReferenceChoices';

const arm: CandidateTarget = { component_id: 'arm', command_target_id: 'motion' };
const peer: CandidateTarget = { component_id: 'peer', command_target_id: 'motion' };

const facts: ReferenceSelectorFacts = {
  components: [], targets: [], signals: [],
  frames: [
    { reference: { component_id: 'arm', frame_id: 'base' }, ambiguous: false, moving_targets: [arm] },
    { reference: { component_id: 'tool', frame_id: 'tcp' }, ambiguous: false, moving_targets: [arm] },
    { reference: { component_id: 'peer', frame_id: 'base' }, ambiguous: false, moving_targets: [peer] },
    { reference: { component_id: 'tool', frame_id: 'ambiguous' }, ambiguous: true, moving_targets: [] },
  ],
  bodies: [
    { reference: { component_id: 'tool', collision_body_id: 'jaw' }, ambiguous: false, moving_targets: [arm] },
    { reference: { component_id: 'peer', collision_body_id: 'jaw' }, ambiguous: false, moving_targets: [peer] },
    { reference: { component_id: 'tool', collision_body_id: 'ambiguous' }, ambiguous: true, moving_targets: [] },
  ],
};

describe('program direct-reference selector data', () => {
  it('keeps frame identity direct and reports target qualification separately', () => {
    const choices = programFrameReferenceChoices(facts, arm);
    expect(choices.map((choice) => [choice.key, choice.reference, choice.selectable, choice.movingTargetMatches])).toEqual([
      [JSON.stringify(['arm', 'base']), { component_id: 'arm', frame_id: 'base' }, true, true],
      [JSON.stringify(['tool', 'tcp']), { component_id: 'tool', frame_id: 'tcp' }, true, true],
      [JSON.stringify(['peer', 'base']), { component_id: 'peer', frame_id: 'base' }, false, false],
      [JSON.stringify(['tool', 'ambiguous']), { component_id: 'tool', frame_id: 'ambiguous' }, false, false],
    ]);
    expect(programFrameReferenceChoices(facts).map((choice) => choice.selectable))
      .toEqual([true, true, true, false]);
  });

  it('builds a shared frame scope from the captured facts, including mounted-tool frames', () => {
    const program = { scene: [{ scene_resource_id: 'table' }] } as unknown as Pick<ProgramDocument, 'scene'>;
    const scope = programFrameScopeFromSelectorFacts(program, facts, arm);
    expect(scope.frames).toEqual([
      { component_id: 'arm', frame_id: 'base' },
      { component_id: 'tool', frame_id: 'tcp' },
      { component_id: 'peer', frame_id: 'base' },
    ]);
    expect(scope.movingFrames).toEqual([
      { component_id: 'arm', frame_id: 'base' },
      { component_id: 'tool', frame_id: 'tcp' },
    ]);
    expect(scope.unavailableFrames).toEqual([{
      reference: { component_id: 'tool', frame_id: 'ambiguous' },
      reason: 'Ambiguous frame identity',
    }]);
    expect(programFrameChoices(scope).map((choice) => [choice.group, choice.label])).toEqual([
      ['Moving robot · arm', 'arm · base'],
      ['Moving robot · arm', 'tool · tcp'],
      ['Other robots', 'peer · base'],
      ['Cell', 'Cell'],
      ['Scene objects', 'table'],
    ]);

    const targetOnly = programFrameScopeFromSelectorFacts(program, facts, arm, true);
    expect(targetOnly.frames).toEqual(scope.movingFrames);
    expect(targetOnly.unavailableFrames).toEqual([
      { reference: { component_id: 'peer', frame_id: 'base' }, reason: 'Frame is not qualified for the selected target' },
      { reference: { component_id: 'tool', frame_id: 'ambiguous' }, reason: 'Ambiguous frame identity' },
    ]);
  });

  it('keeps collision-body refs direct and marks ambiguity or requested-target mismatch', () => {
    const choices = programCollisionBodyReferenceChoices(facts, arm);
    expect(choices.map((choice) => [choice.reference, choice.selectable, choice.movingTargetMatches])).toEqual([
      [{ component_id: 'tool', collision_body_id: 'jaw' }, true, true],
      [{ component_id: 'peer', collision_body_id: 'jaw' }, false, false],
      [{ component_id: 'tool', collision_body_id: 'ambiguous' }, false, false],
    ]);
    expect(programCollisionBodyReferenceChoices(facts).map((choice) => choice.selectable))
      .toEqual([true, true, false]);
  });

  it('projects Scene resource IDs and only Scene-resource blackboard keys', () => {
    const program = {
      scene: [{ scene_resource_id: 'z_table' }, { scene_resource_id: 'a_part' }],
      blackboard: { keys: [
        { key: 'scene.z_table', type: 'scene_resource' },
        { key: 'planning.pose', type: 'pose' },
        { key: 'scene.a_part', type: 'scene_resource' },
      ] },
    } as unknown as ProgramAuthoringDocument;
    expect(programSceneResourceIds(program)).toEqual(['z_table', 'a_part']);
    expect(programSceneResourceBlackboardKeys(program)).toEqual(['scene.a_part', 'scene.z_table']);
  });
});
