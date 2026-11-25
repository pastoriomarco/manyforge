import { useState } from 'react';

import { copyToClipboard } from '../utils/clipboard';

/** A launch fault's last process output lines: collapsed, monospace, with Copy. */
export function ProcessOutputTail({ text }: { text: string }) {
  const [copied, setCopied] = useState<boolean | null>(null);
  return (
    <details className="process-output-tail">
      <summary>Process output (last lines)</summary>
      <pre>{text}</pre>
      <button type="button" className="quiet-button" title="Copy the process output lines."
        onClick={() => setCopied(copyToClipboard(text))}
      >{copied === null ? 'Copy' : copied ? 'Copied' : 'Copy failed'}</button>
    </details>
  );
}
