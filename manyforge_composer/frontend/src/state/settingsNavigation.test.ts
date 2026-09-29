import { afterEach, expect, it, vi } from 'vitest';

import { onOpenSettingsRequest, requestOpenSettings } from './settingsNavigation';

afterEach(() => {
  vi.unstubAllGlobals();
});

it('opens only the named surface settings', () => {
  const target = new EventTarget();
  vi.stubGlobal('addEventListener', target.addEventListener.bind(target));
  vi.stubGlobal('removeEventListener', target.removeEventListener.bind(target));
  vi.stubGlobal('dispatchEvent', target.dispatchEvent.bind(target));
  const opened: string[] = [];
  const stopViewer = onOpenSettingsRequest('viewer', () => opened.push('viewer'));
  const stopTree = onOpenSettingsRequest('program', () => opened.push('program'));
  requestOpenSettings('program');
  expect(opened).toEqual(['program']);
  stopViewer();
  stopTree();
  requestOpenSettings('program');
  expect(opened).toEqual(['program']);
});
