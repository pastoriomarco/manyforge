/**
 * Copies through a transient text area: the Clipboard API exists only in a
 * secure context, and the Composer is also opened over plain http by address.
 */
export function copyToClipboard(text: string): boolean {
  const focused = document.activeElement as HTMLElement | null;
  const area = document.createElement('textarea');
  area.value = text;
  area.setAttribute('readonly', '');
  area.style.position = 'fixed';
  area.style.opacity = '0';
  document.body.append(area);
  area.select();
  const copied = document.execCommand('copy');
  area.remove();
  focused?.focus();
  return copied;
}
