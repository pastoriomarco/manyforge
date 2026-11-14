import { expect, it } from 'vitest';
import {
  displayApplyId,
  displayId,
  displayRequestKey,
  displaySessionCommand,
  fullId,
  isApplicationStartId,
  isRequestKey,
  newRequestKey,
  parseIdFilter,
  requestKeyNamesWebSocketCommand,
} from './applicationIdentity';
import { sameClientRequestKey } from './parsers/execution';

const START = '3f9a0123456789ab';

it('forms the display strings a person reads (BS 2.9)', () => {
  expect(displayId(START, 'op', 7)).toBe('3f9a/op7');
  expect(displayId(START, 'stop', 12)).toBe('3f9a/stop12');
  expect(displayId(START, 'run', 3)).toBe('3f9a/run3');
  expect(displayApplyId(START, 4)).toBe('3f9a/apply4');
  expect(displayApplyId(null, 4)).toBe('apply4');
  expect(displaySessionCommand(START, 4, 118)).toBe('3f9a/s4.118');
  expect(displayRequestKey(START, '1a2b3c4d5e6f708192a3b4c5d6e7f801')).toBe('3f9a/k1a2b3c4d');
});

it('forms the full copy form with all 16 hex characters', () => {
  expect(fullId(START, 'op', 7)).toBe('3f9a0123456789ab/op7');
});

it('reads the display form, the full form and the bare number back', () => {
  expect(parseIdFilter('3f9a/op7', 'op')).toEqual({
    applicationStartId: null, applicationStartPrefix: '3f9a', value: 7,
  });
  expect(parseIdFilter(fullId(START, 'run', 3), 'run')).toEqual({
    applicationStartId: START, applicationStartPrefix: null, value: 3,
  });
  expect(parseIdFilter(' 12 ', 'stop')).toEqual({
    applicationStartId: null, applicationStartPrefix: null, value: 12,
  });
  for (const text of ['3f9a/run7', 'op7', 'operation.1.7', '3f9a/op0', '0000000000000000/op7', '3f9/op7']) {
    expect(parseIdFilter(text, 'op')).toBeNull();
  }
});

it('mints HTTP request keys of 32 lowercase hex characters', () => {
  const first = newRequestKey();
  expect(isRequestKey(first)).toBe(true);
  expect(newRequestKey()).not.toBe(first);
  expect(isApplicationStartId(START)).toBe(true);
  expect(isApplicationStartId('0000000000000000')).toBe(false);
});

it('matches a Headless-formed key to the sent key on bytes 1-15 only', () => {
  const sent = '00112233445566778899aabbccddeeff';
  expect(sameClientRequestKey(`c0${sent.slice(2)}`, sent)).toBe(true);
  expect(sameClientRequestKey(`c0${'0'.repeat(30)}`, sent)).toBe(false);
});

it('reads a class 0 key as its session number and client sequence', () => {
  // Session 4 in bytes 1-7, client sequence 118 (0x76) in bytes 8-15.
  const key = '00000000000000040000000000000076';
  expect(requestKeyNamesWebSocketCommand(key, 4, '118')).toBe(true);
  // The part in byte 0 is not compared; the class must be 0.
  expect(requestKeyNamesWebSocketCommand(`01${key.slice(2)}`, 4, '118')).toBe(true);
  expect(requestKeyNamesWebSocketCommand(`40${key.slice(2)}`, 4, '118')).toBe(false);
  expect(requestKeyNamesWebSocketCommand(`c0${key.slice(2)}`, 4, '118')).toBe(false);
  expect(requestKeyNamesWebSocketCommand(key, 5, '118')).toBe(false);
  expect(requestKeyNamesWebSocketCommand(key, 4, '119')).toBe(false);
  expect(requestKeyNamesWebSocketCommand('request.1', 4, '118')).toBe(false);
});
