export function flipRgbaRowsInPlace(
  pixels: Uint8Array,
  width: number,
  height: number,
): void {
  const rowBytes = width * 4;
  const scratch = new Uint8Array(rowBytes);
  for (let top = 0; top < Math.floor(height / 2); top += 1) {
    const bottom = height - top - 1;
    const topOffset = top * rowBytes;
    const bottomOffset = bottom * rowBytes;
    scratch.set(pixels.subarray(topOffset, topOffset + rowBytes));
    pixels.copyWithin(topOffset, bottomOffset, bottomOffset + rowBytes);
    pixels.set(scratch, bottomOffset);
  }
}

export function canvasToBlobWithTimeout(
  canvas: HTMLCanvasElement,
  mime: string,
  quality: number,
  timeoutMs: number,
): Promise<Blob> {
  return new Promise((resolve, reject) => {
    let settled = false;
    const timer = globalThis.setTimeout(() => {
      if (settled) return;
      settled = true;
      reject(new Error('Snapshot encoding timed out'));
    }, timeoutMs);
    canvas.toBlob(
      (blob) => {
        if (settled) return;
        settled = true;
        globalThis.clearTimeout(timer);
        if (blob === null) {
          reject(new Error('Snapshot encoding returned no image'));
          return;
        }
        resolve(blob);
      },
      mime,
      quality,
    );
  });
}
