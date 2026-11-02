// Keep exact transport values. Convert to durations/display units only at the
// presentation boundary; the browser never owns an execution deadline.
export type Nanoseconds = number | string;

export function parseNanoseconds(value: unknown, path: string): Nanoseconds {
  if (typeof value === 'number' && Number.isSafeInteger(value) && value >= 0) return value;
  if (typeof value === 'string' && value.length <= 19
    && /^(0|[1-9]\d*)$/.test(value)
    && BigInt(value) <= 9_223_372_036_854_775_807n) return value;
  throw new Error(`${path}: expected exact nonnegative int64 nanoseconds`);
}
