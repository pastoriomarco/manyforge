import { renderToStaticMarkup } from 'react-dom/server';
import { expect, it } from 'vitest';

import { PendingApplyNote, SettingsScope } from './SettingsScope';

it('names every scope with one word and explains it in the tooltip', () => {
  const tags = (['installation', 'program', 'session'] as const).map(
    (scope) => renderToStaticMarkup(<SettingsScope scope={scope} />),
  );
  expect(tags[0]).toMatch(/>Installation<\/span>$/);
  expect(tags[0]).toContain('kept across restarts');
  expect(tags[1]).toMatch(/>Program<\/span>$/);
  expect(tags[1]).toContain('Apply makes it part of the running Program');
  expect(tags[2]).toMatch(/>Session<\/span>$/);
  expect(tags[2]).toContain('until this page is reloaded');
  expect(renderToStaticMarkup(<PendingApplyNote />)).toContain('Changed in the Draft, not applied yet.');
});
