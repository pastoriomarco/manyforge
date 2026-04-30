export function parseProfile(profile: string | null | undefined): [number, number] | null {
  const match = /^v(\d+)(?:\.(\d+))?$/.exec(profile ?? '');
  if (!match) {
    return null;
  }
  return [Number(match[1]), Number(match[2] ?? '0')];
}

export function profileSatisfies(
  minProfile: string | null | undefined,
  targetProfile: string | null | undefined,
): boolean {
  if (!minProfile || !targetProfile || minProfile === targetProfile) {
    return true;
  }
  const min = parseProfile(minProfile);
  const target = parseProfile(targetProfile);
  if (min == null || target == null) {
    return minProfile === targetProfile;
  }
  return target[0] > min[0] || (target[0] === min[0] && target[1] >= min[1]);
}
