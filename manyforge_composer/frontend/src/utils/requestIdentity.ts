let fallbackIdentitySequence = 0;

/** A fresh request id: `<prefix>-<random>`. */
export function browserRequestIdentity(prefix: string): string {
  const browserCrypto = globalThis.crypto;
  if (typeof browserCrypto?.randomUUID === 'function') {
    return `${prefix}-${browserCrypto.randomUUID()}`;
  }
  if (typeof browserCrypto?.getRandomValues === 'function') {
    const words = new Uint32Array(4);
    browserCrypto.getRandomValues(words);
    const suffix = Array.from(words, (word) => word.toString(16).padStart(8, '0')).join('');
    return `${prefix}-${suffix}`;
  }
  fallbackIdentitySequence = (fallbackIdentitySequence + 1) % 0x1_0000_0000;
  return `${prefix}-${Date.now().toString(36)}-${fallbackIdentitySequence.toString(36)}`;
}
