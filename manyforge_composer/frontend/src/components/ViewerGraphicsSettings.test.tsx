import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import { ViewerGraphicsSettings } from './ViewerGraphicsSettings';

describe('Viewer graphics settings', () => {
  it('enables only the column of the renderer this browser uses', () => {
    const gpu = renderToStaticMarkup(<ViewerGraphicsSettings rendererKind="gpu" />);
    expect(gpu).toMatch(/<select aria-label="GPU resolution">/);
    expect(gpu).toMatch(/<select aria-label="Software resolution" title="Used by browsers that render with software rendering[^"]*" disabled="">/);
    expect(gpu).toContain('<th scope="col" class="current">GPU<small>this browser</small></th>');
    // Defaults reproduce the former fixed choices.
    expect(gpu).toMatch(/aria-label="GPU robot update rate"[^>]*>(?:(?!<\/select>).)*<option value="30" selected="">30 Hz/);
    expect(gpu).toMatch(/aria-label="Software robot update rate"[^>]*>(?:(?!<\/select>).)*<option value="10" selected="">10 Hz/);

    const software = renderToStaticMarkup(<ViewerGraphicsSettings rendererKind="software" />);
    expect(software).toMatch(/<input type="checkbox" aria-label="Software antialiasing"\/>/);
    expect(software).toMatch(/<input type="checkbox" aria-label="GPU antialiasing" title="[^"]*" disabled="" checked=""\/>/);
  });

  it('keeps both columns read-only until the renderer is detected', () => {
    const markup = renderToStaticMarkup(<ViewerGraphicsSettings rendererKind={null} />);
    expect(markup).toContain('How this browser renders is detected when the 3D view starts.');
    expect(markup).not.toMatch(/<select aria-label="[^"]*">/);
  });
});
