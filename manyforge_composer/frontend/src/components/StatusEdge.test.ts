import { describe, expect, it } from 'vitest';
import { statusEdgePath } from './StatusEdge';

const geometry = { sourceX: 0, sourceY: 0, targetX: 100, targetY: 80 };

describe('status edge presentation', () => {
  it('uses horizontal tangents for the vertical node-band layout', () => {
    expect(statusEdgePath(geometry, 'straight')).toBe('M 0 0 L 100 80');
    expect(statusEdgePath(geometry, 'step')).toContain('L 50 0 L 50 80');
    expect(statusEdgePath(geometry, 'smoothstep')).toContain('C 50 0, 50 80');
    expect(statusEdgePath(geometry, 'default')).toContain('C 45 0, 55 80');
  });

  it('uses vertical tangents for the horizontal node-row layout', () => {
    expect(statusEdgePath(geometry, 'step', 'horizontal'))
      .toContain('L 0 40 L 100 40');
    expect(statusEdgePath(geometry, 'default', 'horizontal'))
      .toContain('C 0 40, 100 40');
  });
});
