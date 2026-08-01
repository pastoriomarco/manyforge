import { describe, expect, it } from 'vitest';
import { statusEdgePath } from './StatusEdge';

const geometry = { sourceX: 0, sourceY: 0, targetX: 100, targetY: 80 };

describe('status edge presentation', () => {
  it('retains selectable straight, stepped, and curved routes', () => {
    expect(statusEdgePath(geometry, 'straight')).toBe('M 0 0 L 100 80');
    expect(statusEdgePath(geometry, 'step')).toContain('L 50 0 L 50 80');
    expect(statusEdgePath(geometry, 'smoothstep')).toContain('C 50 0, 50 80');
    expect(statusEdgePath(geometry, 'default')).toContain('C 0 40, 100 40');
  });
});
