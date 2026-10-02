import { readFileSync } from 'node:fs';

import { describe, expect, it } from 'vitest';

import type { ProgramValueSource } from '../types/api';
import {
  checkValuePath,
  formatValuePath,
  hasSourcesWithin,
  listDeleteMove,
  listInsertMove,
  listReorderMove,
  moveListSources,
  overlappingSourcePaths,
  parseValuePath,
  setValueAtPath,
  sourcesAfterLiteral,
  valueAtPath,
  valuePathParameter,
  valuePathWithin,
  withValueSource,
} from './valuePaths';

const formula = (expression: string): ProgramValueSource => ({ kind: 'expression', expression });

const CONFORMANCE = new URL(
  '../../../../manyforge_deployment/resources/expression/value_path_conformance.v1.yaml',
  import.meta.url,
);

/**
 * Read the shared conformance cases. The file keeps one case per line (or a
 * path line and its segments line), so this reader stays line-based and
 * refuses any line it does not recognise instead of skipping it.
 */
function conformanceCases() {
  const valid: Array<{ path: string; segments: Array<string | number> }> = [];
  const invalid: Array<{ path: string; code: string }> = [];
  let section = '';
  for (const line of readFileSync(CONFORMANCE, 'utf8').split('\n')) {
    if (line.trim() === '' || line.startsWith('#') || /^(schema|limits):|^ {2}max_/.test(line)) continue;
    const header = /^(valid|invalid):$/.exec(line);
    const validPath = /^ {2}- path: (.+)$/.exec(line);
    const segments = /^ {4}segments: \[(.*)\]$/.exec(line);
    const invalidCase = /^ {2}- \{path: (".*"), code: ([a-z_]+)\}$/.exec(line);
    if (header) section = header[1]!;
    else if (section === 'valid' && validPath) valid.push({ path: validPath[1]!, segments: [] });
    else if (section === 'valid' && segments && valid.length > 0) {
      valid.at(-1)!.segments = [...segments[1]!.matchAll(/\{(name|index): "?([^"}]*)"?\}/g)]
        .map(([, kind, text]) => kind === 'index' ? Number(text) : text!);
    } else if (section === 'invalid' && invalidCase) {
      invalid.push({ path: JSON.parse(invalidCase[1]!) as string, code: invalidCase[2]! });
    } else {
      throw new Error(`Unrecognised conformance line: ${line.slice(0, 80)}`);
    }
  }
  return { valid, invalid };
}

describe('value paths', () => {
  it('parses and formats the canonical grammar', () => {
    for (const text of ['velocity_scale', 'goal.position_m[2]', 'target_reference.pose', 'waypoints[10].orientation.rpy_deg[0]']) {
      const segments = parseValuePath(text);
      expect(segments).not.toBeNull();
      expect(formatValuePath(segments!)).toBe(text);
    }
    expect(parseValuePath('goal.position_m[2]')).toEqual(['goal', 'position_m', 2]);
    expect(valuePathParameter('goal.position_m[2]')).toBe('goal');
  });

  it('refuses every non-canonical or malformed path', () => {
    for (const text of ['', '[0]', '.goal', 'goal.', 'goal..x', 'goal[01]', 'goal[-1]', 'goal[ 1]', 'goal[1',
      'goal. x', 'goal x', '2goal', 'goal.2x', 'goal[99999999999999999999]', 'goal["x"]', 'Goal', '_goal']) {
      expect(parseValuePath(text), text).toBeNull();
    }
  });

  it('passes every shared value-path conformance case', () => {
    const { valid, invalid } = conformanceCases();
    expect(valid.length).toBeGreaterThan(5);
    expect(invalid.length).toBeGreaterThan(5);
    for (const row of valid) {
      expect(checkValuePath(row.path), row.path).toEqual({ segments: row.segments });
      expect(formatValuePath(row.segments)).toBe(row.path);
    }
    for (const row of invalid) {
      expect(checkValuePath(row.path), row.path).toEqual({ refusal: row.code });
    }
  });

  it('reads and replaces values at a path without mutating the literal', () => {
    const parameters = { goal: { position_m: [0.3, 0.1, 0], orientation: { rpy_deg: [180, 0, 0] } } };
    expect(valueAtPath(parameters, 'goal.position_m[1]')).toBe(0.1);
    expect(valueAtPath(parameters, 'goal.position_m[3]')).toBeUndefined();
    expect(valueAtPath(parameters, 'goal.orientation.xyzw')).toBeUndefined();
    expect(valueAtPath(parameters, 'missing')).toBeUndefined();
    const next = setValueAtPath(parameters, 'goal.orientation.rpy_deg[2]', 90);
    expect(next).toEqual({ goal: { position_m: [0.3, 0.1, 0], orientation: { rpy_deg: [180, 0, 90] } } });
    expect(parameters.goal.orientation.rpy_deg[2]).toBe(0);
    expect(setValueAtPath(parameters, 'goal.orientation.xyzw[0]', 1)).toBeUndefined();
    expect(setValueAtPath(parameters, 'velocity_scale', 0.5)).toEqual({ ...parameters, velocity_scale: 0.5 });
  });

  it('knows which paths lie inside another', () => {
    expect(valuePathWithin('goal.position_m[2]', 'goal')).toBe(true);
    expect(valuePathWithin('goal', 'goal')).toBe(true);
    expect(valuePathWithin('goal_offset', 'goal')).toBe(false);
    expect(valuePathWithin('waypoints[10]', 'waypoints[1]')).toBe(false);
    expect(hasSourcesWithin({ 'goal.position_m[2]': formula('1') }, 'goal.position_m')).toBe(true);
    expect(hasSourcesWithin({ 'goal.position_m[2]': formula('1') }, 'goal.orientation')).toBe(false);
  });

  it('never keeps a whole value and an entry inside it', () => {
    const sources = { 'goal.position_m[2]': formula('1'), 'goal.position_m[0]': formula('2'), speed: formula('3') };
    const whole = withValueSource(sources, 'goal', { kind: 'blackboard_key', reference: 'place_pose' });
    expect(whole).toEqual({ speed: formula('3'), goal: { kind: 'blackboard_key', reference: 'place_pose' } });
    const inner = withValueSource(whole, 'goal.position_m[1]', formula('4'));
    expect(inner).toEqual({ speed: formula('3'), 'goal.position_m[1]': formula('4') });
    expect(overlappingSourcePaths(inner)).toEqual([]);
    expect(overlappingSourcePaths({ goal: formula('1'), 'goal.position_m[2]': formula('2') }))
      .toEqual([['goal', 'goal.position_m[2]']]);
  });

  it('moves entries with inserted, deleted and reordered list items', () => {
    const sources = {
      'path[0].position_m[2]': formula('a'),
      'path[1]': { kind: 'blackboard_key', reference: 'p1' } as ProgramValueSource,
      'path[2].orientation.rpy_deg[0]': formula('c'),
      speed: formula('s'),
    };
    expect(moveListSources(sources, listInsertMove('path', 3, 1))).toEqual({
      'path[0].position_m[2]': formula('a'),
      'path[2]': { kind: 'blackboard_key', reference: 'p1' },
      'path[3].orientation.rpy_deg[0]': formula('c'),
      speed: formula('s'),
    });
    expect(moveListSources(sources, listDeleteMove('path', 3, 1))).toEqual({
      'path[0].position_m[2]': formula('a'),
      'path[1].orientation.rpy_deg[0]': formula('c'),
      speed: formula('s'),
    });
    expect(moveListSources(sources, listReorderMove('path', 3, 2, 0))).toEqual({
      'path[1].position_m[2]': formula('a'),
      'path[2]': { kind: 'blackboard_key', reference: 'p1' },
      'path[0].orientation.rpy_deg[0]': formula('c'),
      speed: formula('s'),
    });
    expect(listReorderMove('path', 3, 0, 1).indexMap).toEqual([1, 0, 2]);
    // A nested list moves only its own entries.
    expect(moveListSources({ 'a[0].b[1]': formula('x'), 'a[1].b[1]': formula('y') },
      listDeleteMove('a[1].b', 2, 0))).toEqual({ 'a[0].b[1]': formula('x'), 'a[1].b[0]': formula('y') });
  });

  it('drops entries whose value no longer exists after typing a new value', () => {
    const sources = {
      'goal.orientation.rpy_deg[2]': formula('key.yaw'),
      'goal.position_m[2]': formula('param.z'),
      goal_whole: { kind: 'program_parameter', reference: 'p' } as ProgramValueSource,
    };
    // Switching to a quaternion removes the yaw formula; z still exists.
    expect(sourcesAfterLiteral(sources, 'goal', {
      position_m: [0, 0, 0],
      orientation: { xyzw: [0, 0, 0, 1] },
    })).toEqual({
      'goal.position_m[2]': formula('param.z'),
      goal_whole: { kind: 'program_parameter', reference: 'p' },
    });
    // Typing a whole value replaces its whole-value source.
    expect(sourcesAfterLiteral({ speed: formula('1') }, 'speed', 0.2)).toEqual({});
    // Deleting the first waypoint moves the second waypoint's entry to index 0.
    expect(sourcesAfterLiteral(
      { 'path[1].position_m[0]': formula('x'), 'path[0].position_m[0]': formula('y') },
      'path',
      [{ position_m: [0, 0, 0] }],
      [listDeleteMove('path', 2, 0)],
    )).toEqual({ 'path[0].position_m[0]': formula('x') });
  });
});
