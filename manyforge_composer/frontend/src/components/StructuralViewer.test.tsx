import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import { ViewerFailureBoundary } from './StructuralViewer';

describe('viewer failure isolation', () => {
  it('replaces only the failed viewer subtree with its fallback', () => {
    const boundary = new ViewerFailureBoundary({
      children: <div>viewport</div>,
      fallback: <div>isolated; authoring remains usable</div>,
    });
    boundary.state = ViewerFailureBoundary.getDerivedStateFromError();

    expect(renderToStaticMarkup(boundary.render())).toContain(
      'isolated; authoring remains usable',
    );
  });
});
