export function parseProfile(
  profile: string | null | undefined,
): [number, number] | null {
  const match = /^v(\d+)(?:\.(\d+))?$/.exec(profile ?? '');
  if (!match) return null;
  return [Number(match[1]), Number(match[2] ?? '0')];
}

export function profileSatisfies(
  minimumProfile: string | null | undefined,
  targetProfile: string | null | undefined,
): boolean {
  if (!minimumProfile || !targetProfile || minimumProfile === targetProfile) {
    return true;
  }
  const minimum = parseProfile(minimumProfile);
  const target = parseProfile(targetProfile);
  if (minimum === null || target === null) {
    return minimumProfile === targetProfile;
  }
  return target[0] > minimum[0]
    || (target[0] === minimum[0] && target[1] >= minimum[1]);
}
