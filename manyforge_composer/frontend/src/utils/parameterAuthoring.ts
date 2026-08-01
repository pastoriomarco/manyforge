export function isOmittedOptionalParameter(
  required: boolean,
  value: unknown,
): boolean {
  return !required && (value === undefined || value === null);
}

export function numericUpperBoundError(
  value: number,
  limit: number | undefined,
  options: { label?: string; unit?: string } = {},
): string | null {
  if (limit === undefined || !Number.isFinite(limit) || value <= limit + 1e-9) {
    return null;
  }
  const suffix = options.unit ? ` ${options.unit}` : '';
  return `Exceeds ${options.label ?? 'cap'} ${limit}${suffix}`;
}
