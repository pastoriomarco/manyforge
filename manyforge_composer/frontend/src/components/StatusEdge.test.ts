import { describe, expect, it } from 'vitest';
import { statusEdgePath } from './StatusEdge';

const geometry = { sourceX: 0, sourceY: 0, targetX: 100, targetY: 80 };

describe('status edge presentation', () => {
  it('draws orthogonal edges with rounded corners and step edges with sharp ones', () => {
    const orthogonal = statusEdgePath(geometry, 'smoothstep');
    expect(orthogonal).not.toContain('C');
    expect(orthogonal).toContain('Q 50,0 50,15');
    expect(orthogonal).toContain('L 50,65');

    const step = statusEdgePath(geometry, 'step');
    expect(step).not.toContain('C');
    expect(step).toContain('L 50,0');
    expect(step).toContain('L 50,80');
  });

  it('draws straight and curved edges', () => {
    expect(statusEdgePath(geometry, 'straight')).toBe('M 0,0L 100,80');
    expect(statusEdgePath(geometry, 'default')).toBe('M0,0 C50,0 50,80 100,80');
    expect(statusEdgePath(geometry, 'simplebezier')).toContain('C');
  });

  it('leaves and enters vertically in the horizontal node-row layout', () => {
    const orthogonal = statusEdgePath(geometry, 'smoothstep', 'horizontal');
    expect(orthogonal).toContain('Q 0,40 10,40');
    expect(orthogonal).toContain('L 90,40');
  });
});
