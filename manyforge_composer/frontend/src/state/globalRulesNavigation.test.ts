import { afterEach, expect, it, vi } from 'vitest';

import {
  onOpenGlobalRulesRequest,
  requestOpenGlobalRules,
  takeOpenGlobalRulesRequest,
} from './globalRulesNavigation';

afterEach(() => {
  vi.unstubAllGlobals();
  takeOpenGlobalRulesRequest();
});

it('notifies listeners and leaves one pending request for a section that mounts later', () => {
  const target = new EventTarget();
  vi.stubGlobal('addEventListener', target.addEventListener.bind(target));
  vi.stubGlobal('removeEventListener', target.removeEventListener.bind(target));
  vi.stubGlobal('dispatchEvent', target.dispatchEvent.bind(target));
  let opened = 0;
  const stop = onOpenGlobalRulesRequest(() => { opened += 1; });
  expect(takeOpenGlobalRulesRequest()).toBe(false);
  requestOpenGlobalRules();
  expect(opened).toBe(1);
  expect(takeOpenGlobalRulesRequest()).toBe(true);
  expect(takeOpenGlobalRulesRequest()).toBe(false);
  stop();
  requestOpenGlobalRules();
  expect(opened).toBe(1);
});
