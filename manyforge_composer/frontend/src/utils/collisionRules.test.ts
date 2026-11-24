import { describe, expect, it } from 'vitest';

import type { JsonValue, SourceModelProjection } from '../types/api';
import {
  SIGNED_RELEASE_DEFAULTS,
  collisionRuleSelectionKinds,
  collisionRuleSelectionProblem,
  collisionRulesProblem,
  defaultCollisionRuleSelection,
  draftCollisionBodies,
  everythingClearance,
  newCollisionRule,
  nextCollisionRuleId,
  releaseJson,
  releaseValueOf,
  sourceModelCollisionChoices,
  withEverythingClearance,
  withSpringSlider,
} from './collisionRules';

const everything = { rule_id: 'everything', clearance_m: 0.001 };
const tipOnPart = {
  rule_id: 'tip_on_part',
  a: [{ kind: 'active_tcp_contact_bodies' }],
  b: [{ kind: 'scene_object', blackboard_key: 'trial.probe_object' }],
  contact: { max_penetration_m: 0.002, end_from_fraction: 0.8 },
};
const fingersOnTable = {
  rule_id: 'fingers_table',
  a: [{ kind: 'collision_body', component_id: 'lite_gripper', collision_body_id: 'gripper_fingers' }],
  b: [{ kind: 'scene_objects' }],
  clearance_m: 0.004,
};

describe('collision rules', () => {
  it('accepts the three rule values and the row without selections', () => {
    expect(collisionRulesProblem([everything, fingersOnTable], 'deployment')).toBeNull();
    expect(collisionRulesProblem([tipOnPart], 'node')).toBeNull();
    expect(collisionRulesProblem([everything, tipOnPart, {
      rule_id: 'withdraw',
      a: [{ kind: 'attached_object', component_id: 'lite_gripper' }],
      b: [{ kind: 'scene_object', scene_resource_id: 'table' }],
      release: releaseJson(SIGNED_RELEASE_DEFAULTS),
    }], 'node')).toBeNull();
    expect(collisionRulesProblem([], 'node')).toBeNull();
  });

  it('requires the Deployment\'s first rule and keeps it first', () => {
    expect(collisionRulesProblem([], 'deployment')).toContain('first rule is required');
    expect(collisionRulesProblem([fingersOnTable, everything], 'deployment'))
      .toBe('The rule without selections is the first rule.');
    expect(collisionRulesProblem([everything, { rule_id: 'second', clearance_m: 0.002 }], 'deployment'))
      .toBe('Only one rule may be without selections.');
    expect(collisionRulesProblem([{ rule_id: 'everything', contact: {} }], 'deployment'))
      .toContain('holds a clearance only');
  });

  it('refuses move-only selections and legs on the Deployment', () => {
    expect(collisionRuleSelectionKinds('deployment')).not.toContain('scene_object');
    expect(collisionRuleSelectionKinds('deployment')).not.toContain('active_tcp_contact_bodies');
    expect(collisionRuleSelectionKinds('node')).toContain('scene_object');
    expect(collisionRuleSelectionKinds('deployment')).toContain('model_group');
    expect(collisionRuleSelectionKinds('node')).not.toContain('model_group');
    expect(collisionRulesProblem([everything, tipOnPart], 'deployment')).toContain('motion nodes only');
    expect(collisionRulesProblem([everything, { ...fingersOnTable, leg: 'descend' }], 'deployment'))
      .toContain('only pick and place sequences name a leg');
  });

  it('requires a leg on every row of a pick or place sequence', () => {
    expect(collisionRulesProblem([tipOnPart], 'sequence')).toContain('choose the leg');
    expect(collisionRulesProblem([{ ...tipOnPart, leg: 'lift' }], 'sequence')).toContain('choose the leg');
    expect(collisionRulesProblem([{ ...tipOnPart, leg: 'descend' }], 'sequence')).toBeNull();
  });

  it('checks ids, selection counts, exact selection keys and value ranges', () => {
    expect(collisionRulesProblem([tipOnPart, tipOnPart], 'node')).toBe('Rule id tip_on_part is used twice.');
    expect(collisionRulesProblem([{ ...tipOnPart, rule_id: '' }], 'node')).toContain('needs an id');
    expect(collisionRulesProblem([{ ...tipOnPart, rule_id: 'x'.repeat(129) }], 'node')).toContain('needs an id');
    expect(collisionRulesProblem([{ ...tipOnPart, b: [] }], 'node')).toContain('side B needs 1 to 16 selections');
    expect(collisionRulesProblem([{ ...tipOnPart, a: Array.from({ length: 17 }, () => ({ kind: 'scene_objects' })) }], 'node'))
      .toContain('side A needs 1 to 16 selections');
    expect(collisionRulesProblem([{ ...tipOnPart, clearance_m: 0.001 }], 'node')).toContain('exactly one value');
    expect(collisionRulesProblem([{ ...fingersOnTable, clearance_m: 0.051 }], 'node')).toContain('from 0 to 0.05');
    expect(collisionRulesProblem([{ ...tipOnPart, contact: { max_penetration_m: -1 } }], 'node'))
      .toContain('deepest penetration');
    expect(collisionRulesProblem([{ ...tipOnPart, contact: { end_from_fraction: 1.1 } }], 'node'))
      .toContain('fraction from 0 to 1');
    expect(collisionRulesProblem([{ ...tipOnPart, contact: { during: 'end' } }], 'node'))
      .toContain('only a depth limit, an end window and a spring step limit');
    expect(collisionRulesProblem([{ ...tipOnPart, during: 'end' }], 'node')).toContain('during is not a rule field');
    expect(collisionRuleSelectionProblem({ kind: 'scene_object', scene_resource_id: 'a', blackboard_key: 'b' }, 'node'))
      .toContain('Scene object');
    // A structured key names the scene_resource field it reads; a field needs its key.
    expect(collisionRuleSelectionProblem(
      { kind: 'scene_object', blackboard_key: 'planning.pick_plan', field: 'selected_object_id' }, 'node')).toBeNull();
    expect(collisionRuleSelectionProblem({ kind: 'scene_object', scene_resource_id: 'a', field: 'b' }, 'node'))
      .toContain('Scene object');
    expect(collisionRuleSelectionProblem({ kind: 'link', component_id: 'robot', link: '' }, 'node'))
      .toContain('one of its links');
    // A model group is a Deployment selection only.
    expect(collisionRuleSelectionProblem({ kind: 'model_group', component_id: 'robot', group: 'arm' }, 'node'))
      .toContain("Deployment's rules only");
    expect(collisionRuleSelectionProblem(
      { kind: 'model_group', component_id: 'robot', group: 'arm' }, 'deployment')).toBeNull();
    expect(collisionRuleSelectionProblem({ kind: 'model_group', component_id: 'robot' }, 'deployment'))
      .toContain('model groups');
    expect(collisionRuleSelectionProblem({ kind: 'scene_objects', component_id: 'robot' }, 'node'))
      .toContain('no further fields');
  });

  it('sets, replaces and clears the Everything else clearance', () => {
    expect(everythingClearance([tipOnPart])).toBeNull();
    const set = withEverythingClearance([tipOnPart], 0.002);
    expect(set).toEqual([{ rule_id: 'everything', clearance_m: 0.002 }, tipOnPart]);
    expect(everythingClearance(set)).toBe(0.002);
    expect(withEverythingClearance(set, 0)).toEqual([{ rule_id: 'everything', clearance_m: 0 }, tipOnPart]);
    expect(withEverythingClearance(set, null)).toEqual([tipOnPart]);
    // A rule already named `everything` keeps its id; the new row takes a free one.
    const taken: JsonValue[] = [{ ...tipOnPart, rule_id: 'everything' }];
    expect(withEverythingClearance(taken, 0.001)[0]).toEqual({ rule_id: 'rule_1', clearance_m: 0.001 });
  });

  it('reads a release with the defaults of omitted keys and writes every key', () => {
    expect(releaseValueOf({})).toEqual(SIGNED_RELEASE_DEFAULTS);
    expect(releaseValueOf({ release_evidence_mode: 'binary_contact' })).toEqual({
      evidence: 'binary_contact', enforceMaxStartPenetration: false, maxStartPenetrationM: 0,
      minReleaseProgressM: 0, releaseToleranceM: 0, requireStrictlyDecreasingPenetration: false,
      requireSeparationByEnd: true, allowRecontact: false,
    });
    expect(releaseValueOf({ release_evidence_mode: 'distance' })).toBeNull();
    expect(releaseValueOf({ allow_recontact: 1 })).toBeNull();
    expect(releaseJson({ ...SIGNED_RELEASE_DEFAULTS, maxStartPenetrationM: 0.03, releaseToleranceM: 0.001 })).toEqual({
      release_evidence_mode: 'signed_penetration', enforce_max_start_penetration: true,
      max_start_penetration_m: 0.03, min_release_progress_m: 0, release_tolerance_m: 0.001,
      require_strictly_decreasing_penetration: true, require_separation_by_end: true, allow_recontact: false,
    });
    // A lightweight release has no penetration metrics: its signed-only fields are neutral.
    expect(releaseJson({ ...SIGNED_RELEASE_DEFAULTS, evidence: 'binary_contact', maxStartPenetrationM: 0.03 })).toEqual({
      release_evidence_mode: 'binary_contact', enforce_max_start_penetration: false,
      max_start_penetration_m: 0, min_release_progress_m: 0, release_tolerance_m: 0,
      require_strictly_decreasing_penetration: false, require_separation_by_end: true, allow_recontact: false,
    });
  });

  it('starts a new rule from the first available choices', () => {
    const choices = {
      componentIds: ['lite6_robot', 'lite_gripper'],
      bodies: [{ component_id: 'lite_gripper', collision_body_id: 'gripper_collision' }],
      sceneObjectIds: [], sceneResourceKeys: ['trial.probe_object'],
    };
    expect(nextCollisionRuleId([{ rule_id: 'rule_1' }, tipOnPart])).toBe('rule_2');
    expect(newCollisionRule([everything], 'deployment', choices)).toEqual({
      rule_id: 'rule_1', a: [{ kind: 'component', component_id: 'lite6_robot' }],
      b: [{ kind: 'scene_objects' }], contact: {},
    });
    const sequenceRule = newCollisionRule([], 'sequence', choices);
    expect(sequenceRule).toMatchObject({ leg: 'descend', a: [{ kind: 'active_tcp_contact_bodies' }] });
    expect(collisionRulesProblem([sequenceRule], 'sequence')).toBeNull();
    expect(defaultCollisionRuleSelection('collision_body', choices))
      .toEqual({ kind: 'collision_body', component_id: 'lite_gripper', collision_body_id: 'gripper_collision' });
    // Without a Scene object, the first scene_resource key is offered.
    expect(defaultCollisionRuleSelection('scene_object', choices))
      .toEqual({ kind: 'scene_object', blackboard_key: 'trial.probe_object' });
    expect(defaultCollisionRuleSelection('scene_object', { ...choices, sceneObjectIds: ['table'] }))
      .toEqual({ kind: 'scene_object', scene_resource_id: 'table' });
  });
});

describe('collision bodies of a Deployment Draft', () => {
  it('lists each component\'s catalog bodies once, sorted, and skips components the catalog does not describe', () => {
    const profiles = {
      lite_gripper_v0: {
        tags: [], attachmentInterfaces: [], tcpTargets: [],
        collisionBodyTargets: [
          { commandTargetId: 'gripper', collisionBodyRoleIds: ['gripper_fingers', 'gripper_collision'],
            tcpContactBodyRoleIds: {}, gripBodyRoleId: 'gripper_collision' },
          { commandTargetId: 'aux', collisionBodyRoleIds: ['gripper_collision'],
            tcpContactBodyRoleIds: {}, gripBodyRoleId: null },
        ],
      },
      io_device_v0: { tags: [], attachmentInterfaces: [], tcpTargets: [] },
    };
    const document = {
      api_version: 'manyforge.deployment_intent.v3',
      components: [
        { component_id: 'tool_b', component_profile_id: 'lite_gripper_v0' },
        { component_id: 'station_io', component_profile_id: 'io_device_v0' },
        { component_id: 'tool_a', component_profile_id: 'lite_gripper_v0' },
        { component_id: 'unknown', component_profile_id: 'not_in_catalog' },
      ],
    };
    expect(draftCollisionBodies(document, profiles)).toEqual([
      { component_id: 'tool_a', collision_body_id: 'gripper_collision' },
      { component_id: 'tool_a', collision_body_id: 'gripper_fingers' },
      { component_id: 'tool_b', collision_body_id: 'gripper_collision' },
      { component_id: 'tool_b', collision_body_id: 'gripper_fingers' },
    ]);
    expect(draftCollisionBodies(null, profiles)).toEqual([]);
  });
});

describe('source model collision choices', () => {
  it('lists each component\'s cell-model links and target groups, sorted and unique', () => {
    const source = {
      occurrences: [
        { component_id: 'robot', links: [
          { source_link_name: 'link6', viewer_link_name: 'robot__link6' },
          { source_link_name: 'link5', viewer_link_name: 'robot__link5' },
        ] },
        { component_id: null, links: [{ source_link_name: 'table', viewer_link_name: 'table' }] },
      ],
      targets: [
        { component_id: 'robot', model_groups: [{ source_group_name: 'manipulator' }] },
        { component_id: 'robot', model_groups: [{ source_group_name: 'manipulator' }] },
      ],
    } as unknown as SourceModelProjection;
    expect(sourceModelCollisionChoices(source)).toEqual({
      links: [
        { component_id: 'robot', link: 'robot__link5' },
        { component_id: 'robot', link: 'robot__link6' },
      ],
      modelGroups: [{ component_id: 'robot', group: 'manipulator' }],
    });
    expect(sourceModelCollisionChoices(null)).toEqual({ links: null, modelGroups: null });
  });
});

describe('spring contact rules (OD-356)', () => {
  const spring = (contact: Record<string, number>) => ({ ...tipOnPart, contact });
  it('accepts a step limit with the stroke and refuses it alone or non-positive', () => {
    expect(collisionRulesProblem([spring({ max_penetration_m: 0.02, max_compression_step_m: 0.002 })], 'node')).toBeNull();
    expect(collisionRulesProblem([spring({ max_compression_step_m: 0.002 })], 'node'))
      .toContain('needs the deepest penetration');
    expect(collisionRulesProblem([spring({ max_penetration_m: 0.02, max_compression_step_m: 0 })], 'node'))
      .toContain('above 0 m');
  });
});

describe('spring slider rules (OD-371)', () => {
  const slider = {
    rule_id: 'tube_into_endplate',
    spring_slider: { component_id: 'tube_holder', element_id: 'holder_rod' },
    b: [{ kind: 'scene_object', scene_resource_id: 'endplate' }],
    contact: { end_from_fraction: 0 },
  };
  it('accepts the slider with b and an end window on moves only', () => {
    expect(collisionRulesProblem([slider], 'node')).toBeNull();
    expect(collisionRulesProblem([{ ...slider, contact: undefined } as unknown as JsonValue], 'node')).toBeNull();
    expect(collisionRulesProblem([{ ...slider, leg: 'descend' }], 'sequence')).toBeNull();
    expect(collisionRulesProblem([everything, slider], 'deployment')).toContain('motion nodes only');
  });
  it('refuses side A, other values and the stroke beside the slider', () => {
    expect(collisionRulesProblem([{ ...slider, a: tipOnPart.a }], 'node')).toContain('in place of side A');
    expect(collisionRulesProblem([{ ...slider, contact: { max_penetration_m: 0.01 } }], 'node'))
      .toContain('only the end window');
    expect(collisionRulesProblem([{ ...slider, spring_slider: { component_id: 'tube_holder' } }], 'node'))
      .toContain('its spring slider');
  });
  it('switches a rule to the slider and back, keeping the end window', () => {
    const choices = {
      componentIds: ['tube_holder'],
      bodies: [{ component_id: 'tube_holder', collision_body_id: 'holder_rod' }],
    };
    const switched = withSpringSlider({ ...tipOnPart, contact: { end_from_fraction: 0 } }, true, choices);
    expect(switched).toEqual({
      rule_id: 'tip_on_part',
      spring_slider: { component_id: 'tube_holder', element_id: 'holder_rod' },
      b: tipOnPart.b,
      contact: { end_from_fraction: 0 },
    });
    expect(withSpringSlider(switched, false, choices)).toEqual({
      rule_id: 'tip_on_part', a: [{ kind: 'active_tcp_contact_bodies' }], b: tipOnPart.b, contact: {},
    });
  });
  it('lets a carried-object selection name the body that carries it', () => {
    const carried = { kind: 'attached_object', component_id: 'tube_holder', collision_body_role_id: 'holder_rod' };
    expect(collisionRuleSelectionProblem(carried, 'node')).toBeNull();
    expect(collisionRuleSelectionProblem({ ...carried, collision_body_role_id: '' }, 'node')).not.toBeNull();
  });
});
