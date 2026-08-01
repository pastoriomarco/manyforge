import { describe, expect, it } from 'vitest';
import { btNodeConnectionAvailability } from './BtNode';

describe('behavior-tree node presentation', () => {
  it('retains child-cardinality and single-parent connection limits', () => {
    expect(
      btNodeConnectionAvailability({ childrenMax: 1, childCount: 0, hasParent: false }),
    ).toEqual({ sourceConnectable: true, targetConnectable: true });
    expect(
      btNodeConnectionAvailability({ childrenMax: 1, childCount: 1, hasParent: true }),
    ).toEqual({ sourceConnectable: false, targetConnectable: false });
    expect(
      btNodeConnectionAvailability({ childrenMax: 0, childCount: 0, hasParent: false }),
    ).toEqual({ sourceConnectable: false, targetConnectable: true });
  });
});
